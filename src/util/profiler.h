/*
 * profiler.h — per-stage frame profiler with spike attribution.
 *
 * Ported from the GLFW-Project engine profiler. Each frame is divided into
 * stages; every stage accumulates its own time, an exponential moving average,
 * and min/max, and a rolling history feeds the HUD graph. When a frame exceeds
 * the spike threshold the profiler records which stage deviated most from its
 * own average — the culprit — rather than merely which was largest, so a
 * consistently-expensive stage does not mask an occasional stall elsewhere.
 *
 * Enable with --profile; the report prints on exit and on demand.
 */
#ifndef PROFILER_H
#define PROFILER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROFILER_STAGE_FRAME_TOTAL = 0,
    PROFILER_STAGE_INPUT,        /* SDL events, camera, menu input          */
    PROFILER_STAGE_STARSYS,      /* procedural star -> real system promotion */
    PROFILER_STAGE_PHYSICS,      /* RESPA integration of active systems      */
    PROFILER_STAGE_COLLISION,    /* broadphase + resolution + supernova      */
    PROFILER_STAGE_LIFECYCLE,    /* stellar evolution, accretion             */
    PROFILER_STAGE_FIELDS,       /* cosmic / radiance field, field graph     */
    PROFILER_STAGE_CAMPREP,      /* camera proximity, view matrices          */
    PROFILER_STAGE_RENDER,       /* render_frame: every world pass           */
    PROFILER_STAGE_POST,         /* bloom / HDR composite                    */
    PROFILER_STAGE_UI,           /* HUD, labels, ImGui menu                  */
    PROFILER_STAGE_SWAP,         /* SDL_GL_SwapWindow: GPU / vsync wait      */
    PROFILER_STAGE_COUNT
} ProfilerStage;

#define PROFILER_HISTORY_LEN 180
#define PROFILER_MAX_SPIKES 32

typedef struct {
    const char *name;
    uint32_t    color_abgr;   // ImU32 ABGR format for ImGui rendering
    double      current_ms;   // Time in current frame (ms)
    double      avg_ms;       // Exponential moving average (ms)
    double      min_ms;       // Min recorded (ms)
    double      max_ms;       // Max recorded (ms)
    double      pct_cpu;      // Percentage of CPU frame time
} ProfilerStageStats;

/* Per-system timings, recorded from inside the OpenMP-parallel system loop —
 * hence the lock-free accumulators in the implementation. "sys" is one system's
 * RESPA integration; "col" is one system's collision pass. */
typedef struct {
    int    sys_count;         // Systems integrated in recent sample period
    double sys_avg_ms;
    double sys_min_ms;
    double sys_max_ms;

    int    col_count;         // Systems collision-stepped in recent period
    double col_avg_ms;
    double col_min_ms;
    double col_max_ms;
} ProfilerWorkerStats;

typedef struct {
    double        timestamp;       // Seconds since start
    double        frame_ms;        // Total frame duration (ms)
    ProfilerStage culprit_stage;   // Primary culprit stage
    double        culprit_ms;      // Duration of culprit stage (ms)
    int           bodies_active;
    int           systems_active;
    int           systems_dirty;
    char          description[128];
} ProfilerSpike;

typedef struct {
    float frame_times[PROFILER_HISTORY_LEN];
    float stage_times[PROFILER_STAGE_COUNT][PROFILER_HISTORY_LEN];
    int   head;
} ProfilerHistory;

typedef struct {
    ProfilerStageStats  stages[PROFILER_STAGE_COUNT];
    ProfilerWorkerStats worker;
    ProfilerSpike       spikes[PROFILER_MAX_SPIKES];
    int                 spike_count;
    int                 spike_head;

    double fps;
    double frame_ms;
    double cpu_ms;             // Total CPU time (frame_ms - swap_buffers_ms)
    double gpu_wait_ms;        // Swap buffers / GPU present wait time

    bool   paused;
    float  spike_threshold_ms; // Trigger threshold in ms (default: 20.0f)
    bool   has_spike;          // Did a spike occur on the most recent frame?
} ProfilerSnapshot;

// Contextual info for the profiler when recording spikes
typedef struct {
    int bodies_active;
    int systems_active;
    int systems_dirty;
} ProfilerFrameContext;

void profiler_init(void);
void profiler_shutdown(void);

void profiler_frame_begin(void);
void profiler_frame_end(const ProfilerFrameContext *ctx);

void profiler_stage_begin(ProfilerStage stage);
void profiler_stage_end(ProfilerStage stage);

// Per-system recording from parallel regions (lock-free, thread-safe)
void profiler_record_system_time(double elapsed_ms);
void profiler_record_collision_time(double elapsed_ms);

// Inspection & Controls
void profiler_get_snapshot(ProfilerSnapshot *out);
const ProfilerHistory *profiler_get_history(void);
void profiler_set_paused(bool paused);
bool profiler_is_paused(void);
void profiler_set_spike_threshold(float threshold_ms);
float profiler_get_spike_threshold(void);
void profiler_reset_stats(void);
void profiler_clear_spikes(void);
void profiler_dump_stdout(void);

/* ── zones ────────────────────────────────────────────────────────────────
 * Sub-stage timers keyed by name, for breaking one heavy stage into its parts
 * without minting an enum entry per part. Zones accumulate per frame and are
 * reported under their own heading. `name` must be a string literal (it is
 * stored by pointer, not copied). */
#define PROFILER_MAX_ZONES 40
void profiler_zone_add(const char *name, double ms);

/* Read back zones for display. Sorted heaviest-first by profiler_zone_sort();
 * `index` is a position in that order, valid until the next sort. */
int  profiler_zone_count(void);
void profiler_zone_sort(void);
int  profiler_zone_get(int index, const char **name, double *avg_ms, double *max_ms);

/* Monotonic milliseconds — exposed so callers can time a scope without a
 * dedicated stage (the per-system recorders take an elapsed value). */
double profiler_now_ms(void);

/* 1 while profiling is enabled (--profile). Stage markers are cheap but not
 * free, so hot per-system call sites check this first. */
int  profiler_enabled(void);
void profiler_set_enabled(int on);

// Stage display name, for callers that summarise the history themselves.
const char *profiler_stage_name(ProfilerStage stage);

#endif // PROFILER_H
