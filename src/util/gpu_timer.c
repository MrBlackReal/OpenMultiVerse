/*
 * gpu_timer.c — GPU-side pass timing with GL timestamp queries (gpu_timer.h).
 *
 * GL_TIMESTAMP pairs rather than GL_TIME_ELAPSED: the same numbers, but
 * timestamps need no begin/end pairing across the frame's control flow.
 */
#include "gpu_timer.h"
#include "common.h"

#define GPU_TIMER_LAG   2      /* frames kept; with the finishes they are  */
                               /* complete by the next frame, so 2 is ample */
#define GPU_TIMER_MARKS 48     /* zones per frame (render.c places ~20)     */
#define GPU_TIMER_ZONES 40

typedef struct {
    GLuint      qb[GPU_TIMER_MARKS], qe[GPU_TIMER_MARKS];  /* begin / end   */
    const char *name[GPU_TIMER_MARKS];
    int         n;                       /* zones opened this frame          */
} GpuFrame;

typedef struct { const char *name; double acc_ms, avg_ms, max_ms; } GpuZone;

static int      s_enabled = 0;
static GpuFrame s_frames[GPU_TIMER_LAG];
static int      s_cur = 0;
static int      s_ready = 0;             /* query objects generated          */
static int      s_open = 0;              /* a zone is open                   */

static GpuZone  s_zones[GPU_TIMER_ZONES];
static int      s_zone_n = 0;
static int      s_order[GPU_TIMER_ZONES];
static double   s_frame_avg = 0.0, s_frame_max = 0.0;
static long     s_frames_read = 0;

void gpu_timer_set_enabled(int on) { s_enabled = on ? 1 : 0; }
int  gpu_timer_enabled(void)       { return s_enabled; }

static void zone_add(const char *name, double ms)
{
    for (int i = 0; i < s_zone_n; i++)
        if (s_zones[i].name == name) { s_zones[i].acc_ms += ms; return; }
    if (s_zone_n >= GPU_TIMER_ZONES) return;
    s_zones[s_zone_n].name = name;
    s_zones[s_zone_n].acc_ms = ms;
    s_zones[s_zone_n].avg_ms = s_zones[s_zone_n].max_ms = 0.0;
    s_zone_n++;
}

/* Same smoothing as the CPU zones (profiler.c), so the columns compare. */
static void zones_commit(double frame_ms)
{
    for (int i = 0; i < s_zone_n; i++) {
        double cur = s_zones[i].acc_ms;
        if (s_zones[i].avg_ms <= 0.0001) s_zones[i].avg_ms = cur;
        else s_zones[i].avg_ms += (cur - s_zones[i].avg_ms) * 0.08;
        if (cur > s_zones[i].max_ms) s_zones[i].max_ms = cur;
        s_zones[i].acc_ms = 0.0;
    }
    if (s_frame_avg <= 0.0001) s_frame_avg = frame_ms;
    else s_frame_avg += (frame_ms - s_frame_avg) * 0.08;
    if (frame_ms > s_frame_max) s_frame_max = frame_ms;
    s_frames_read++;
}

/* Fold in a finished frame. The frame total is the sum of zone spans: pure
 * execution, without the gaps where the GPU waited on the CPU. */
static void collect(GpuFrame *f)
{
    if (f->n == 0) return;
    GLint avail = 0;
    glGetQueryObjectiv(f->qe[f->n - 1], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (avail) {
        double total = 0.0;
        for (int k = 0; k < f->n; k++) {
            GLuint64 tb = 0, te = 0;
            glGetQueryObjectui64v(f->qb[k], GL_QUERY_RESULT, &tb);
            glGetQueryObjectui64v(f->qe[k], GL_QUERY_RESULT, &te);
            double ms = te > tb ? (double)(te - tb) * 1e-6 : 0.0;
            zone_add(f->name[k], ms);
            total += ms;
        }
        zones_commit(total);
    }
    f->n = 0;
}

static void zone_close(void)
{
    if (!s_open) return;
    GpuFrame *f = &s_frames[s_cur];
    glQueryCounter(f->qe[f->n - 1], GL_TIMESTAMP);
    glFinish();                          /* the zone's batch ends here       */
    s_open = 0;
}

static void zone_open(const char *name)
{
    GpuFrame *f = &s_frames[s_cur];
    if (f->n >= GPU_TIMER_MARKS) return;
    glQueryCounter(f->qb[f->n], GL_TIMESTAMP);
    f->name[f->n] = name;
    f->n++;
    s_open = 1;
}

void gpu_timer_frame_begin(void)
{
    if (!s_enabled) return;
    if (!s_ready) {
        for (int i = 0; i < GPU_TIMER_LAG; i++) {
            glGenQueries(GPU_TIMER_MARKS, s_frames[i].qb);
            glGenQueries(GPU_TIMER_MARKS, s_frames[i].qe);
            s_frames[i].n = 0;
        }
        s_ready = 1;
    }
    /* This slot was written GPU_TIMER_LAG frames ago: read it, then reuse. */
    collect(&s_frames[s_cur]);
    glFinish();                          /* start from an idle GPU           */
    zone_open("(before render)");
}

void gpu_timer_mark(const char *name)
{
    if (!s_enabled || !s_ready) return;
    zone_close();
    zone_open(name);
}

void gpu_timer_frame_end(void)
{
    if (!s_enabled || !s_ready) return;
    zone_close();
    s_cur = (s_cur + 1) % GPU_TIMER_LAG;
}

int gpu_timer_count(void) { return s_zone_n; }

void gpu_timer_sort(void)
{
    for (int i = 0; i < s_zone_n; i++) s_order[i] = i;
    for (int i = 1; i < s_zone_n; i++) {
        int k = s_order[i], j = i - 1;
        while (j >= 0 && s_zones[s_order[j]].avg_ms < s_zones[k].avg_ms) {
            s_order[j + 1] = s_order[j]; j--;
        }
        s_order[j + 1] = k;
    }
}

int gpu_timer_get(int index, const char **name, double *avg_ms, double *max_ms)
{
    if (index < 0 || index >= s_zone_n) return 0;
    const GpuZone *z = &s_zones[s_order[index]];
    if (name)   *name   = z->name;
    if (avg_ms) *avg_ms = z->avg_ms;
    if (max_ms) *max_ms = z->max_ms;
    return 1;
}

void gpu_timer_frame_stats(double *avg_ms, double *max_ms)
{
    if (avg_ms) *avg_ms = s_frame_avg;
    if (max_ms) *max_ms = s_frame_max;
}

void gpu_timer_dump(void)
{
    if (s_frames_read == 0) return;
    printf("---- GPU (timer queries, %ld frames) ---------------------------------\n",
           s_frames_read);
    printf("  %-22s avg %7.3f ms   max %7.3f ms\n", "GPU frame", s_frame_avg, s_frame_max);
    gpu_timer_sort();
    for (int i = 0; i < s_zone_n; i++) {
        const GpuZone *z = &s_zones[s_order[i]];
        if (z->avg_ms < 0.005 && z->max_ms < 0.05) continue;
        printf("  %-22s avg %7.3f ms   max %7.3f ms\n", z->name, z->avg_ms, z->max_ms);
    }
    printf("----------------------------------------------------------------------\n");
}

void gpu_timer_shutdown(void)
{
    if (!s_ready) return;
    for (int i = 0; i < GPU_TIMER_LAG; i++) {
        glDeleteQueries(GPU_TIMER_MARKS, s_frames[i].qb);
        glDeleteQueries(GPU_TIMER_MARKS, s_frames[i].qe);
    }
    s_ready = 0;
}
