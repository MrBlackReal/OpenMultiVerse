#include "profiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

static const char *STAGE_NAMES[PROFILER_STAGE_COUNT] = {
    "Frame Total",
    "Input",
    "StarSys promote",
    "Physics (RESPA)",
    "Collision",
    "Lifecycle",
    "Fields",
    "Cam/Prep",
    "Render",
    "Post/Bloom",
    "UI",
    "Swap"
};

// Colors in ImU32 ABGR (0xAABBGGRR)
static const uint32_t STAGE_COLORS[PROFILER_STAGE_COUNT] = {
    0xFFFFFFFF, // Frame Total (white)
    0xFFE0A040, // Input (blue)
    0xFFD0D030, // StarSys (cyan)
    0xFF40D060, // Physics (green)
    0xFF3050E0, // Collision (red-orange)
    0xFFC040C0, // Lifecycle (magenta)
    0xFF20C0D0, // Fields (dark yellow)
    0xFF80E0A0, // Cam/Prep (pale green)
    0xFF2080FF, // Render (orange)
    0xFFD05090, // Post (purple)
    0xFF9060E0, // UI (pink)
    0xFF30B0F0  // Swap (amber)
};

static bool   g_initialized = false;
static int    g_enabled = 0;
static bool   g_paused = false;
static float  g_spike_threshold_ms = 20.0f; // Alert on frames taking > 20ms (< 50fps)
static double g_init_time_ms = 0.0;

static double g_frame_start_ms = 0.0;
static double g_avg_frame_ms = 0.0;   /* smoothed, for the share column */
static double g_avg_cpu_ms   = 0.0;
static double g_stage_start_ms[PROFILER_STAGE_COUNT];
static double g_stage_acc_ms[PROFILER_STAGE_COUNT];

typedef struct { const char *name; double acc_ms, avg_ms, max_ms; int calls; } ProfilerZone;
static ProfilerZone g_zones[PROFILER_MAX_ZONES];
static int          g_zone_count = 0;

static ProfilerSnapshot g_snapshot;
static ProfilerHistory  g_history;

// Worker metrics (thread-safe, lock-free)
static atomic_ullong g_worker_sys_ns = 0;
static atomic_uint   g_worker_sys_count = 0;
static atomic_uint   g_worker_sys_min_us = UINT32_MAX;
static atomic_uint   g_worker_sys_max_us = 0;

static atomic_ullong g_worker_col_ns = 0;
static atomic_uint   g_worker_col_count = 0;
static atomic_uint   g_worker_col_min_us = UINT32_MAX;
static atomic_uint   g_worker_col_max_us = 0;

static inline double get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

void profiler_init(void) {
    if (g_initialized) return;

    memset(&g_snapshot, 0, sizeof(g_snapshot));
    memset(&g_history, 0, sizeof(g_history));
    memset(g_stage_start_ms, 0, sizeof(g_stage_start_ms));
    memset(g_stage_acc_ms, 0, sizeof(g_stage_acc_ms));

    for (int i = 0; i < PROFILER_STAGE_COUNT; i++) {
        g_snapshot.stages[i].name = STAGE_NAMES[i];
        g_snapshot.stages[i].color_abgr = STAGE_COLORS[i];
        g_snapshot.stages[i].min_ms = 1e9;
        g_snapshot.stages[i].max_ms = 0.0;
    }

    g_snapshot.spike_threshold_ms = g_spike_threshold_ms;
    g_init_time_ms = get_monotonic_ms();
    g_initialized = true;
}

void profiler_shutdown(void) {
    g_initialized = false;
}

void profiler_stage_begin(ProfilerStage stage) {
    if (!g_enabled) return;
    if ((unsigned)stage >= PROFILER_STAGE_COUNT) return;
    g_stage_start_ms[stage] = get_monotonic_ms();
}

void profiler_stage_end(ProfilerStage stage) {
    if (!g_enabled) return;
    if ((unsigned)stage >= PROFILER_STAGE_COUNT) return;
    double end = get_monotonic_ms();
    g_stage_acc_ms[stage] += (end - g_stage_start_ms[stage]);
}

void profiler_record_system_time(double elapsed_ms) {
    if (!g_enabled) return;
    unsigned long long ns = (unsigned long long)(elapsed_ms * 1000000.0);
    unsigned int us = (unsigned int)(elapsed_ms * 1000.0);
    atomic_fetch_add_explicit(&g_worker_sys_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_worker_sys_count, 1, memory_order_relaxed);

    unsigned int cur_min = atomic_load_explicit(&g_worker_sys_min_us, memory_order_relaxed);
    while (us < cur_min && !atomic_compare_exchange_weak_explicit(&g_worker_sys_min_us, &cur_min, us, memory_order_relaxed, memory_order_relaxed));

    unsigned int cur_max = atomic_load_explicit(&g_worker_sys_max_us, memory_order_relaxed);
    while (us > cur_max && !atomic_compare_exchange_weak_explicit(&g_worker_sys_max_us, &cur_max, us, memory_order_relaxed, memory_order_relaxed));
}

void profiler_record_collision_time(double elapsed_ms) {
    if (!g_enabled) return;
    unsigned long long ns = (unsigned long long)(elapsed_ms * 1000000.0);
    unsigned int us = (unsigned int)(elapsed_ms * 1000.0);
    atomic_fetch_add_explicit(&g_worker_col_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_worker_col_count, 1, memory_order_relaxed);

    unsigned int cur_min = atomic_load_explicit(&g_worker_col_min_us, memory_order_relaxed);
    while (us < cur_min && !atomic_compare_exchange_weak_explicit(&g_worker_col_min_us, &cur_min, us, memory_order_relaxed, memory_order_relaxed));

    unsigned int cur_max = atomic_load_explicit(&g_worker_col_max_us, memory_order_relaxed);
    while (us > cur_max && !atomic_compare_exchange_weak_explicit(&g_worker_col_max_us, &cur_max, us, memory_order_relaxed, memory_order_relaxed));
}

void profiler_zone_add(const char *name, double ms) {
    if (!g_enabled || !name) return;
    for (int i = 0; i < g_zone_count; i++) {
        if (g_zones[i].name == name) {          /* literals: pointer compare */
            g_zones[i].acc_ms += ms; g_zones[i].calls++;
            return;
        }
    }
    if (g_zone_count >= PROFILER_MAX_ZONES) return;
    g_zones[g_zone_count].name = name;
    g_zones[g_zone_count].acc_ms = ms;
    g_zones[g_zone_count].calls = 1;
    g_zone_count++;
}

void profiler_frame_begin(void) {
    if (!g_enabled) return;
    for (int i = 0; i < g_zone_count; i++) {
        double cur = g_zones[i].acc_ms;
        if (g_zones[i].avg_ms <= 0.0001) g_zones[i].avg_ms = cur;
        else g_zones[i].avg_ms += (cur - g_zones[i].avg_ms) * 0.08;
        if (cur > g_zones[i].max_ms) g_zones[i].max_ms = cur;
        g_zones[i].acc_ms = 0.0;
    }
    if (!g_initialized) profiler_init();
    g_frame_start_ms = get_monotonic_ms();
    memset(g_stage_acc_ms, 0, sizeof(g_stage_acc_ms));
}

void profiler_frame_end(const ProfilerFrameContext *ctx) {
    if (!g_enabled || !g_initialized) return;

    double now = get_monotonic_ms();
    double total_ms = now - g_frame_start_ms;
    g_stage_acc_ms[PROFILER_STAGE_FRAME_TOTAL] = total_ms;

    // Drain worker metrics
    unsigned long long sys_ns = atomic_exchange_explicit(&g_worker_sys_ns, 0, memory_order_relaxed);
    unsigned int sys_cnt = atomic_exchange_explicit(&g_worker_sys_count, 0, memory_order_relaxed);
    if (sys_cnt > 0) {
        double avg = (double)sys_ns / (1000000.0 * (double)sys_cnt);
        if (g_snapshot.worker.sys_avg_ms <= 0.001) g_snapshot.worker.sys_avg_ms = avg;
        else g_snapshot.worker.sys_avg_ms += (avg - g_snapshot.worker.sys_avg_ms) * 0.15;
        g_snapshot.worker.sys_count += (int)sys_cnt;

        unsigned int min_us = atomic_load_explicit(&g_worker_sys_min_us, memory_order_relaxed);
        unsigned int max_us = atomic_load_explicit(&g_worker_sys_max_us, memory_order_relaxed);
        if (min_us != UINT32_MAX) g_snapshot.worker.sys_min_ms = (double)min_us / 1000.0;
        g_snapshot.worker.sys_max_ms = (double)max_us / 1000.0;
    }

    unsigned long long col_ns = atomic_exchange_explicit(&g_worker_col_ns, 0, memory_order_relaxed);
    unsigned int col_cnt = atomic_exchange_explicit(&g_worker_col_count, 0, memory_order_relaxed);
    if (col_cnt > 0) {
        double avg = (double)col_ns / (1000000.0 * (double)col_cnt);
        if (g_snapshot.worker.col_avg_ms <= 0.001) g_snapshot.worker.col_avg_ms = avg;
        else g_snapshot.worker.col_avg_ms += (avg - g_snapshot.worker.col_avg_ms) * 0.15;
        g_snapshot.worker.col_count += (int)col_cnt;

        unsigned int min_us = atomic_load_explicit(&g_worker_col_min_us, memory_order_relaxed);
        unsigned int max_us = atomic_load_explicit(&g_worker_col_max_us, memory_order_relaxed);
        if (min_us != UINT32_MAX) g_snapshot.worker.col_min_ms = (double)min_us / 1000.0;
        g_snapshot.worker.col_max_ms = (double)max_us / 1000.0;
    }

    if (g_paused) return; // Keep snapshot frozen for inspection

    g_snapshot.frame_ms = total_ms;
    g_snapshot.fps = total_ms > 0.0 ? 1000.0 / total_ms : 0.0;
    double swap_ms = g_stage_acc_ms[PROFILER_STAGE_SWAP];
    g_snapshot.gpu_wait_ms = swap_ms;
    double cpu_ms = total_ms - swap_ms;
    if (cpu_ms < 0.0) cpu_ms = 0.0;
    g_snapshot.cpu_ms = cpu_ms;

    if (g_avg_frame_ms <= 0.001) { g_avg_frame_ms = total_ms; g_avg_cpu_ms = cpu_ms; }
    else {
        g_avg_frame_ms += (total_ms - g_avg_frame_ms) * 0.08;
        g_avg_cpu_ms   += (cpu_ms   - g_avg_cpu_ms)   * 0.08;
    }

    for (int i = 0; i < PROFILER_STAGE_COUNT; i++) {
        double cur = g_stage_acc_ms[i];
        g_snapshot.stages[i].current_ms = cur;

        if (g_snapshot.stages[i].avg_ms <= 0.001) {
            g_snapshot.stages[i].avg_ms = cur;
        } else {
            g_snapshot.stages[i].avg_ms += (cur - g_snapshot.stages[i].avg_ms) * 0.08;
        }

        if (cur < g_snapshot.stages[i].min_ms && cur > 0.0001)
            g_snapshot.stages[i].min_ms = cur;
        if (cur > g_snapshot.stages[i].max_ms)
            g_snapshot.stages[i].max_ms = cur;

        /* Share is taken from the smoothed average, not this frame. A stage
         * that is normally idle but expensive on one frame (a screenshot
         * write, a field rebuild) would otherwise define the whole column
         * from whichever frame the report happens to be printed on. */
        double share_of = g_snapshot.stages[i].avg_ms;
        if (i == PROFILER_STAGE_FRAME_TOTAL) {
            g_snapshot.stages[i].pct_cpu = 100.0;
        } else if (i == PROFILER_STAGE_SWAP) {
            g_snapshot.stages[i].pct_cpu = (g_avg_frame_ms > 0.0)
                                         ? (share_of / g_avg_frame_ms * 100.0) : 0.0;
        } else {
            g_snapshot.stages[i].pct_cpu = (g_avg_cpu_ms > 0.0)
                                         ? (share_of / g_avg_cpu_ms * 100.0) : 0.0;
        }
    }

    // Update history buffers
    int head = g_history.head;
    g_history.frame_times[head] = (float)total_ms;
    for (int i = 0; i < PROFILER_STAGE_COUNT; i++) {
        g_history.stage_times[i][head] = (float)g_stage_acc_ms[i];
    }
    g_history.head = (head + 1) % PROFILER_HISTORY_LEN;

    // Check for frame spike
    g_snapshot.has_spike = (total_ms >= g_snapshot.spike_threshold_ms);
    if (g_snapshot.has_spike) {
        // Find culprit stage (largest positive deviation above average)
        ProfilerStage culprit = PROFILER_STAGE_FRAME_TOTAL;
        double max_dev = -1e9;
        double max_time = -1e9;

        for (int i = 1; i < PROFILER_STAGE_COUNT; i++) {
            double cur = g_stage_acc_ms[i];
            double dev = cur - g_snapshot.stages[i].avg_ms;
            if (dev > max_dev) {
                max_dev = dev;
                culprit = (ProfilerStage)i;
            }
            if (cur > max_time) {
                max_time = cur;
            }
        }

        // If no strong deviation, fallback to largest absolute duration
        if (max_dev < 1.0) {
            for (int i = 1; i < PROFILER_STAGE_COUNT; i++) {
                if (g_stage_acc_ms[i] == max_time) {
                    culprit = (ProfilerStage)i;
                    break;
                }
            }
        }

        int sp_idx = g_snapshot.spike_head;
        ProfilerSpike *sp = &g_snapshot.spikes[sp_idx];
        sp->timestamp = (now - g_init_time_ms) / 1000.0;
        sp->frame_ms = total_ms;
        sp->culprit_stage = culprit;
        sp->culprit_ms = g_stage_acc_ms[culprit];
        sp->bodies_active = ctx ? ctx->bodies_active : 0;
        sp->systems_active = ctx ? ctx->systems_active : 0;
        sp->systems_dirty = ctx ? ctx->systems_dirty : 0;

        if (culprit == PROFILER_STAGE_SWAP) {
            snprintf(sp->description, sizeof(sp->description),
                     "Swap/VSync wait: %.1fms (GPU stall or display refresh wait)", sp->culprit_ms);
        } else if (culprit == PROFILER_STAGE_COLLISION) {
            snprintf(sp->description, sizeof(sp->description),
                     "Collision spike: %.1fms (%d dirty systems)",
                     sp->culprit_ms, sp->systems_dirty);
        } else if (culprit == PROFILER_STAGE_PHYSICS) {
            snprintf(sp->description, sizeof(sp->description),
                     "Physics spike: %.1fms (%d active systems)",
                     sp->culprit_ms, sp->systems_active);
        } else if (culprit == PROFILER_STAGE_STARSYS) {
            snprintf(sp->description, sizeof(sp->description),
                     "Star->system promotion: %.1fms (new systems materialised)",
                     sp->culprit_ms);
        } else if (culprit == PROFILER_STAGE_RENDER) {
            snprintf(sp->description, sizeof(sp->description),
                     "Render spike: %.1fms (%d active bodies)",
                     sp->culprit_ms, sp->bodies_active);
        } else if (culprit == PROFILER_STAGE_FIELDS) {
            snprintf(sp->description, sizeof(sp->description),
                     "Field rebuild: %.1fms (cosmic/radiance/graph)", sp->culprit_ms);
        } else {
            snprintf(sp->description, sizeof(sp->description),
                     "%s spike: %.1fms", STAGE_NAMES[culprit], sp->culprit_ms);
        }

        g_snapshot.spike_head = (sp_idx + 1) % PROFILER_MAX_SPIKES;
        if (g_snapshot.spike_count < PROFILER_MAX_SPIKES) {
            g_snapshot.spike_count++;
        }
    }
}

void profiler_get_snapshot(ProfilerSnapshot *out) {
    if (!out) return;
    if (!g_initialized) profiler_init();
    *out = g_snapshot;
}

const ProfilerHistory *profiler_get_history(void) {
    return &g_history;
}

void profiler_set_paused(bool paused) {
    g_paused = paused;
    g_snapshot.paused = paused;
}

bool profiler_is_paused(void) {
    return g_paused;
}

void profiler_set_spike_threshold(float threshold_ms) {
    if (threshold_ms < 5.0f) threshold_ms = 5.0f;
    g_spike_threshold_ms = threshold_ms;
    g_snapshot.spike_threshold_ms = threshold_ms;
}

float profiler_get_spike_threshold(void) {
    return g_spike_threshold_ms;
}

void profiler_reset_stats(void) {
    for (int i = 0; i < PROFILER_STAGE_COUNT; i++) {
        g_snapshot.stages[i].min_ms = 1e9;
        g_snapshot.stages[i].max_ms = 0.0;
        g_snapshot.stages[i].avg_ms = 0.0;
    }
    g_snapshot.worker.sys_min_ms = 0.0;
    g_snapshot.worker.sys_max_ms = 0.0;
    g_snapshot.worker.sys_avg_ms = 0.0;
    g_snapshot.worker.sys_count = 0;
    g_snapshot.worker.col_min_ms = 0.0;
    g_snapshot.worker.col_max_ms = 0.0;
    g_snapshot.worker.col_avg_ms = 0.0;
    g_snapshot.worker.col_count = 0;
    atomic_store_explicit(&g_worker_sys_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_worker_sys_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_worker_col_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_worker_col_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_worker_sys_min_us, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_worker_sys_max_us, 0, memory_order_relaxed);
    atomic_store_explicit(&g_worker_col_min_us, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_worker_col_max_us, 0, memory_order_relaxed);
}

void profiler_clear_spikes(void) {
    g_snapshot.spike_count = 0;
    g_snapshot.spike_head = 0;
}

static int g_zone_order[PROFILER_MAX_ZONES];

int profiler_zone_count(void) { return g_zone_count; }

void profiler_zone_sort(void)
{
    for (int i = 0; i < g_zone_count; i++) g_zone_order[i] = i;
    for (int i = 1; i < g_zone_count; i++) {
        int k = g_zone_order[i], j = i - 1;
        while (j >= 0 && g_zones[g_zone_order[j]].avg_ms < g_zones[k].avg_ms) {
            g_zone_order[j+1] = g_zone_order[j]; j--;
        }
        g_zone_order[j+1] = k;
    }
}

int profiler_zone_get(int index, const char **name, double *avg_ms, double *max_ms)
{
    if (index < 0 || index >= g_zone_count) return 0;
    const ProfilerZone *z = &g_zones[g_zone_order[index]];
    if (name)   *name   = z->name;
    if (avg_ms) *avg_ms = z->avg_ms;
    if (max_ms) *max_ms = z->max_ms;
    return 1;
}

double profiler_now_ms(void) { return get_monotonic_ms(); }
int  profiler_enabled(void) { return g_enabled; }
void profiler_set_enabled(int on) { g_enabled = on ? 1 : 0; if (on) profiler_init(); }

const char *profiler_stage_name(ProfilerStage stage) {
    if ((unsigned)stage >= PROFILER_STAGE_COUNT) return "?";
    return STAGE_NAMES[stage];
}

void profiler_dump_stdout(void) {
    printf("\n=== OPENMULTIVERSE PROFILER REPORT ===\n");
    printf("FPS: %.1f (Frame: %.2f ms | CPU: %.2f ms | Swap/GPU: %.2f ms)\n",
           g_snapshot.fps, g_snapshot.frame_ms, g_snapshot.cpu_ms, g_snapshot.gpu_wait_ms);
    printf("%-18s %10s %10s %10s %10s %8s\n",
           "Stage", "Current(ms)", "Avg(ms)", "Min(ms)", "Max(ms)", "% avg");
    printf("----------------------------------------------------------------------\n");

    for (int i = 1; i < PROFILER_STAGE_COUNT; i++) {
        const ProfilerStageStats *st = &g_snapshot.stages[i];
        printf("%-18s %10.2f %10.2f %10.2f %10.2f %7.1f%%\n",
               st->name, st->current_ms, st->avg_ms,
               (st->min_ms > 1e8 ? 0.0 : st->min_ms), st->max_ms, st->pct_cpu);
    }
    /* Everything inside the frame that no stage claimed. A large figure here
     * means the frame has uninstrumented work, not that the frame is idle —
     * treat it as "find the missing stage", never as slack. */
    double claimed = 0.0;
    for (int i = 1; i < PROFILER_STAGE_COUNT; i++) claimed += g_snapshot.stages[i].avg_ms;
    double unacc = g_avg_frame_ms - claimed;
    if (unacc < 0.0) unacc = 0.0;
    printf("%-18s %10s %10.2f %10s %10s %7.1f%%\n", "(unaccounted)", "",
           unacc, "", "", g_avg_cpu_ms > 0.0 ? unacc / g_avg_cpu_ms * 100.0 : 0.0);
    printf("----------------------------------------------------------------------\n");
    printf("Per-system physics  : avg %.3f ms (min %.3f, max %.3f) [%d stepped]\n",
           g_snapshot.worker.sys_avg_ms, g_snapshot.worker.sys_min_ms,
           g_snapshot.worker.sys_max_ms, g_snapshot.worker.sys_count);
    printf("Per-system collision: avg %.3f ms (min %.3f, max %.3f) [%d stepped]\n",
           g_snapshot.worker.col_avg_ms, g_snapshot.worker.col_min_ms,
           g_snapshot.worker.col_max_ms, g_snapshot.worker.col_count);
    if (g_zone_count > 0) {
        printf("---- zones (per frame) -----------------------------------------------\n");
        /* Heaviest first: the point of a zone table is to name the hotspot. */
        profiler_zone_sort();
        for (int i = 0; i < g_zone_count; i++) {
            const ProfilerZone *z = &g_zones[g_zone_order[i]];
            if (z->avg_ms < 0.005 && z->max_ms < 0.05) continue;
            printf("  %-22s avg %7.3f ms   max %7.3f ms\n", z->name, z->avg_ms, z->max_ms);
        }
        printf("----------------------------------------------------------------------\n");
    }
    printf("Recent Spikes     : %d recorded (> %.1f ms)\n",
           g_snapshot.spike_count, g_snapshot.spike_threshold_ms);
    for (int i = 0; i < g_snapshot.spike_count && i < 5; i++) {
        int idx = (g_snapshot.spike_head - 1 - i + PROFILER_MAX_SPIKES) % PROFILER_MAX_SPIKES;
        const ProfilerSpike *sp = &g_snapshot.spikes[idx];
        printf("  - [%6.1fs] %5.1f ms: %s\n", sp->timestamp, sp->frame_ms, sp->description);
    }
    printf("==============================\n\n");
}
