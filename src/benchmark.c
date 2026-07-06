/*
 * benchmark.c — scripted cinematic flythrough + FPS benchmark (see benchmark.h).
 *
 * The tour is built at start() from the loaded galaxy catalogue: Sun → out of
 * the solar system → above the galactic disc → the Milky Way from outside →
 * a sequence of other galaxies.  Each leg (waypoint→waypoint) is a timed stage.
 * The camera pose is scripted directly (g_cam.pos/yaw/pitch); position is
 * eased-lerped between waypoints and the orientation always looks at the
 * interpolated focus point, so the target stays framed the whole way.
 */
#include "benchmark.h"
#include "camera.h"
#include "galaxy.h"
#include "common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* light-years expressed in AU (camera units) */
#define LY_AU ((double)(LY / AU))

/* ------------------------------------------------------------------ tour */

typedef struct {
    double pos[3];     /* camera position, AU                              */
    double focus[3];   /* point the camera looks at during this leg, AU    */
    char   label[48];
    float  travel_s;   /* seconds to fly from the PREVIOUS waypoint to here */
} Waypoint;

typedef struct {
    int    frames;
    double sum_dt;
    float  min_dt;     /* fastest frame  → max fps */
    float  max_dt;     /* slowest frame  → min fps */
} SegStat;

typedef struct { int seg; int pass; float dt; } Sample;

/* The tour is flown twice (an A/B pass): once with the volumetric galaxies on,
 * once with them off, so the report quantifies the galaxy render cost. */
#define BENCH_PASSES 2
static const char *pass_name(int p) { return p == 0 ? "galaxies ON" : "galaxies OFF"; }

static Waypoint *s_wp   = NULL;
static int       s_nwp = 0, s_wp_cap = 0;

/* per-pass, per-destination-waypoint timing: s_stat[pass][waypoint] */
static SegStat  *s_stat[BENCH_PASSES] = { NULL, NULL };

static Sample   *s_samp = NULL;
static int       s_nsamp = 0, s_samp_cap = 0;

static int    s_active = 0;      /* running OR summary lingering  */
static int    s_running = 0;     /* camera still flying           */
static int    s_pass = 0;        /* 0 = galaxies on, 1 = galaxies off */
static int    s_gal_was = 1;     /* galaxy-enabled state to restore   */
static int    s_seg = 0;         /* destination waypoint index    */
static double s_seg_t = 0.0;     /* seconds into current leg      */
static double s_total_dur = 0.0;
static double s_elapsed = 0.0;
static double s_done_timer = 0.0;
static int    s_warmup = 0;       /* frames of timing to discard at start */
static char   s_summary[128] = "";

/* ------------------------------------------------------------- helpers */

static double v_len(const double v[3]) {
    return sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}
static void v_norm(double v[3]) {
    double l = v_len(v);
    if (l > 1e-300) { v[0]/=l; v[1]/=l; v[2]/=l; }
}
static void v_cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
static double smoother(double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t*t*t*(t*(t*6.0 - 15.0) + 10.0);
}

static void wp_push(const double pos[3], const double focus[3],
                    const char *label, float travel) {
    if (s_nwp >= s_wp_cap) {
        int cap = s_wp_cap ? s_wp_cap * 2 : 16;
        s_wp = realloc(s_wp, (size_t)cap * sizeof(Waypoint));
        if (!s_wp) { fprintf(stderr, "[Benchmark] alloc failed\n"); exit(1); }
        s_wp_cap = cap;
    }
    Waypoint *w = &s_wp[s_nwp];
    for (int i = 0; i < 3; i++) { w->pos[i] = pos[i]; w->focus[i] = focus[i]; }
    snprintf(w->label, sizeof(w->label), "%s", label);
    w->travel_s = travel;
    s_nwp++;
}

/* (Re)allocate and zero the per-pass timing tables once the tour size is known. */
static void stats_alloc_reset(void) {
    for (int p = 0; p < BENCH_PASSES; p++) {
        s_stat[p] = realloc(s_stat[p], (size_t)s_nwp * sizeof(SegStat));
        if (!s_stat[p]) { fprintf(stderr, "[Benchmark] stat alloc failed\n"); exit(1); }
        for (int i = 0; i < s_nwp; i++) {
            s_stat[p][i].frames = 0; s_stat[p][i].sum_dt = 0.0;
            s_stat[p][i].min_dt = 1e30f; s_stat[p][i].max_dt = 0.0f;
        }
    }
}

/* Find a catalogue galaxy by (partial) name; -1 if absent. */
static int galaxy_find(const char *needle) {
    for (int i = 0; i < galaxy_count(); i++)
        if (strstr(galaxy_name(i), needle)) return i;
    return -1;
}

/* Push a leg that frames galaxy `gi` from a 3/4 angle, `mult` radii out. */
static void wp_push_galaxy(int gi, double mult, const char *label, float travel) {
    if (gi < 0) return;
    double c[3];  galaxy_position(gi, c);
    double r = galaxy_radius_au(gi);
    if (r <= 0.0) r = 1.0;

    double axis[3]; { float a[3]; galaxy_axis(gi, a);
        axis[0]=a[0]; axis[1]=a[1]; axis[2]=a[2]; }
    v_norm(axis);
    /* a vector perpendicular to the disc axis for the 3/4 tilt */
    double up[3] = {0.0, 1.0, 0.0};
    if (fabs(axis[1]) > 0.9) { up[0]=1.0; up[1]=0.0; up[2]=0.0; }
    double perp[3]; v_cross(axis, up, perp); v_norm(perp);

    double dir[3];
    for (int i = 0; i < 3; i++) dir[i] = axis[i]*0.55 + perp[i]*0.85;
    v_norm(dir);

    double pos[3];
    for (int i = 0; i < 3; i++) pos[i] = c[i] + dir[i] * r * mult;
    wp_push(pos, c, label, travel);
}

/* ------------------------------------------------------------- lifecycle */

static void build_tour(void) {
    s_nwp = 0; s_total_dur = 0.0;

    /* 0 — start: Sol, close overview (travel_s of wp0 is unused). */
    { double p[3] = {0.0, 3.0, 9.0}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Sol System", 0.0f); }

    /* 1 — drift through the inner system so planets read as discs. */
    { double p[3] = {7.0, 4.0, 15.0}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Inner System", 4.0f); }

    /* 2 — pull out; the Sun collapses to a point of light. */
    { double p[3] = {0.0, 2.5e4, 6.0e4}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Leaving Sol", 4.5f); }

    /* 3 — rise above the disc into the stellar neighbourhood. */
    { double p[3] = {0.0, 35.0*LY_AU, 55.0*LY_AU}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Solar Neighborhood", 5.0f); }

    /* 4/5 — the Milky Way itself (galaxy 0), face-on then a diving 3/4. */
    int mw = galaxy_find("Milky Way");
    if (mw >= 0) {
        double c[3]; galaxy_position(mw, c);
        double r = galaxy_radius_au(mw); if (r <= 0.0) r = 1.0;
        double axis[3]; { float a[3]; galaxy_axis(mw, a);
            axis[0]=a[0]; axis[1]=a[1]; axis[2]=a[2]; }
        v_norm(axis);
        double p[3];
        for (int i = 0; i < 3; i++) p[i] = c[i] + axis[i] * r * 1.8;
        wp_push(p, c, "Milky Way - face on", 6.0f);
        wp_push_galaxy(mw, 1.15, "Milky Way - spiral arms", 6.0f);
    }

    /* 6+ — a grand tour of the neighbours. */
    wp_push_galaxy(galaxy_find("LMC"),        2.4, "Large Magellanic Cloud", 5.5f);
    wp_push_galaxy(galaxy_find("Andromeda"),  2.2, "Andromeda (M31)",        6.0f);
    wp_push_galaxy(galaxy_find("Triangulum"), 2.3, "Triangulum (M33)",       5.5f);
    wp_push_galaxy(galaxy_find("M81"),        2.3, "Bode's Galaxy (M81)",    5.5f);
    wp_push_galaxy(galaxy_find("Sombrero"),   2.4, "Sombrero (M104)",        5.5f);
    wp_push_galaxy(galaxy_find("Centaurus"),  2.3, "Centaurus A",            5.5f);

    /* close the loop back on the Milky Way for a clean ending frame. */
    if (mw >= 0) wp_push_galaxy(mw, 1.8, "Home - Milky Way", 6.5f);

    for (int i = 1; i < s_nwp; i++) s_total_dur += s_wp[i].travel_s;
}

void benchmark_start(void) {
    build_tour();
    if (s_nwp < 2) { fprintf(stderr, "[Benchmark] no tour to run\n"); return; }
    stats_alloc_reset();

    s_nsamp = 0;                 /* keep any prior sample allocation */
    s_pass = 0;                  /* pass A: galaxies on */
    s_gal_was = galaxy_enabled();
    galaxy_set_enabled(1);
    s_seg = 1; s_seg_t = 0.0; s_elapsed = 0.0;
    s_active = 1; s_running = 1; s_done_timer = 0.0; s_warmup = 2;
    s_summary[0] = '\0';

    /* start pose = waypoint 0 */
    for (int i = 0; i < 3; i++) g_cam.pos[i] = s_wp[0].pos[i];

    fprintf(stdout,
            "[Benchmark] starting A/B flythrough: %d stages x %d passes "
            "(galaxies on/off), ~%.0f s\n",
            s_nwp - 1, BENCH_PASSES, s_total_dur * BENCH_PASSES);
    fprintf(stdout, "[Benchmark] pass 1/%d — %s\n", BENCH_PASSES, pass_name(0));
}

int benchmark_active(void)  { return s_active; }
int benchmark_running(void) { return s_running; }

/* --------------------------------------------------------------- report */

static double pct_low_fps(int seg, int pass) {
    /* Collect this stage's frame times, sort descending, take the frame time
     * exceeded by 1% of frames → its reciprocal is the "1% low" fps. */
    int n = 0;
    for (int i = 0; i < s_nsamp; i++)
        if (s_samp[i].seg == seg && s_samp[i].pass == pass) n++;
    if (n == 0) return 0.0;
    float *dt = malloc((size_t)n * sizeof(float));
    if (!dt) return 0.0;
    int k = 0;
    for (int i = 0; i < s_nsamp; i++)
        if (s_samp[i].seg == seg && s_samp[i].pass == pass) dt[k++] = s_samp[i].dt;
    /* simple insertion sort descending (n is a few hundred/thousand) */
    for (int i = 1; i < n; i++) {
        float v = dt[i]; int j = i - 1;
        while (j >= 0 && dt[j] < v) { dt[j+1] = dt[j]; j--; }
        dt[j+1] = v;
    }
    int idx = (int)(n * 0.01);
    if (idx >= n) idx = n - 1;
    float worst = dt[idx];
    free(dt);
    return worst > 0.0f ? 1.0 / worst : 0.0;
}

static double seg_avg(int pass, int s) {
    SegStat *st = &s_stat[pass][s];
    return st->frames > 0 ? st->frames / st->sum_dt : 0.0;
}

static void print_report(void) {
    /* Columns: the same stage timed with galaxies on vs off, and the average
     * per-frame milliseconds the galaxy layer costs (1/on − 1/off). */
    fprintf(stdout, "[Benchmark] ============= A/B RESULTS: galaxy render cost =============\n");
    fprintf(stdout, "[Benchmark] %-24s %7s %7s %7s %7s %8s\n",
            "Stage", "on avg", "off avg", "on 1%", "off 1%", "gal ms");

    double tot_dt_on = 0.0, tot_dt_off = 0.0;
    int    tot_fr_on = 0,   tot_fr_off = 0;

    for (int s = 1; s < s_nwp; s++) {
        double on  = seg_avg(0, s);
        double off = seg_avg(1, s);
        double gal_ms = (on > 0.0 && off > 0.0)
                      ? (1.0/on - 1.0/off) * 1000.0 : 0.0;
        fprintf(stdout, "[Benchmark] %-24s %7.1f %7.1f %7.1f %7.1f %8.1f\n",
                s_wp[s].label, on, off,
                pct_low_fps(s, 0), pct_low_fps(s, 1), gal_ms);
        tot_fr_on  += s_stat[0][s].frames; tot_dt_on  += s_stat[0][s].sum_dt;
        tot_fr_off += s_stat[1][s].frames; tot_dt_off += s_stat[1][s].sum_dt;
    }

    double ov_on  = tot_fr_on  > 0 ? tot_fr_on  / tot_dt_on  : 0.0;
    double ov_off = tot_fr_off > 0 ? tot_fr_off / tot_dt_off : 0.0;
    double ov_gal_ms = (ov_on > 0.0 && ov_off > 0.0)
                     ? (1.0/ov_on - 1.0/ov_off) * 1000.0 : 0.0;
    fprintf(stdout, "[Benchmark] %-24s %7s %7s %7s %7s %8s\n",
            "------------------------", "-------", "-------",
            "-------", "-------", "--------");
    fprintf(stdout, "[Benchmark] %-24s %7.1f %7.1f %7s %7s %8.1f\n",
            "OVERALL", ov_on, ov_off, "", "", ov_gal_ms);
    fprintf(stdout, "[Benchmark] galaxies cost %.1f ms/frame on average "
            "(%.0f -> %.0f fps with them off)\n", ov_gal_ms, ov_on, ov_off);
    fprintf(stdout, "[Benchmark] ==========================================================\n");

    snprintf(s_summary, sizeof(s_summary),
             "Benchmark done   galaxies on %.0f fps  off %.0f fps  (cost %.1f ms/frame)",
             ov_on, ov_off, ov_gal_ms);
}

/* --------------------------------------------------------------- update */

void benchmark_update(float dt_real) {
    if (!s_active) return;

    if (!s_running) {                 /* summary lingering on screen */
        s_done_timer -= (double)dt_real;
        if (s_done_timer <= 0.0) s_active = 0;
        return;
    }

    /* Discard the first couple of frames of the run: their dt carries the
     * one-time cost of world init / first render (and would otherwise clamp to
     * the 100 ms frame cap and dominate "min fps").  Also reject nonsensical
     * sub-50 µs frames (>20k fps) — a timing glitch, not a rendered frame. */
    if (s_warmup > 0) { s_warmup--; dt_real = 0.0f; }

    /* record this frame's timing under the current stage + pass */
    if (dt_real > 5.0e-5f) {
        SegStat *st = &s_stat[s_pass][s_seg];
        st->frames++;
        st->sum_dt += (double)dt_real;
        if (dt_real < st->min_dt) st->min_dt = dt_real;
        if (dt_real > st->max_dt) st->max_dt = dt_real;

        if (s_nsamp >= s_samp_cap) {
            int cap = s_samp_cap ? s_samp_cap * 2 : 4096;
            s_samp = realloc(s_samp, (size_t)cap * sizeof(Sample));
            if (!s_samp) { fprintf(stderr, "[Benchmark] sample alloc failed\n"); exit(1); }
            s_samp_cap = cap;
        }
        s_samp[s_nsamp].seg  = s_seg;
        s_samp[s_nsamp].pass = s_pass;
        s_samp[s_nsamp].dt   = dt_real;
        s_nsamp++;
    }

    s_seg_t   += (double)dt_real;
    s_elapsed += (double)dt_real;

    /* advance to the next leg when this one is done */
    while (s_seg < s_nwp && s_seg_t >= s_wp[s_seg].travel_s) {
        s_seg_t = 0.0;
        s_seg++;
    }
    if (s_seg >= s_nwp) {             /* current pass finished */
        for (int i = 0; i < 3; i++) g_cam.pos[i] = s_wp[s_nwp-1].pos[i];
        if (s_pass + 1 < BENCH_PASSES) {
            /* start the next pass: replay the same tour with galaxies off. */
            s_pass++;
            galaxy_set_enabled(s_pass == 1 ? 0 : 1);
            s_seg = 1; s_seg_t = 0.0; s_elapsed = 0.0; s_warmup = 2;
            for (int i = 0; i < 3; i++) g_cam.pos[i] = s_wp[0].pos[i];
            fprintf(stdout, "[Benchmark] pass %d/%d — %s\n",
                    s_pass + 1, BENCH_PASSES, pass_name(s_pass));
            return;
        }
        /* all passes done */
        galaxy_set_enabled(s_gal_was);   /* restore the user's setting */
        s_running = 0;
        s_done_timer = 8.0;
        print_report();
        return;
    }

    /* interpolate pose along the current leg */
    const Waypoint *a = &s_wp[s_seg - 1];
    const Waypoint *b = &s_wp[s_seg];
    double u = smoother(s_seg_t / (b->travel_s > 1e-3f ? b->travel_s : 1e-3f));

    double focus[3];
    for (int i = 0; i < 3; i++) {
        g_cam.pos[i] = a->pos[i]   + (b->pos[i]   - a->pos[i])   * u;
        focus[i]     = a->focus[i] + (b->focus[i] - a->focus[i]) * u;
    }

    /* orient toward the focus (cam_get_dir convention) */
    double d[3] = { focus[0]-g_cam.pos[0], focus[1]-g_cam.pos[1], focus[2]-g_cam.pos[2] };
    double len = v_len(d);
    if (len > 1e-9) {
        g_cam.yaw   = (float)(atan2(d[2], d[0]) * 180.0 / PI);
        g_cam.pitch = (float)(asin(d[1] / len)  * 180.0 / PI);
    }
}

/* ------------------------------------------------------------------ hud */

int benchmark_hud(char *stage, int stage_n, char *line, int line_n,
                  float *progress) {
    if (!s_active) return 0;

    if (!s_running) {                 /* summary card */
        snprintf(stage, (size_t)stage_n, "%s", s_summary);
        if (line_n > 0) line[0] = '\0';
        if (progress) *progress = 1.0f;
        return 1;
    }

    int seg = s_seg;
    if (seg < 1) seg = 1;
    if (seg >= s_nwp) seg = s_nwp - 1;
    snprintf(stage, (size_t)stage_n, "%s", s_wp[seg].label);

    SegStat *st = &s_stat[s_pass][seg];
    double avg = st->frames > 0 ? st->frames / st->sum_dt : 0.0;
    double mn  = st->max_dt > 0.0f ? 1.0 / st->max_dt : 0.0;
    snprintf(line, (size_t)line_n,
             "pass %d/%d [%s]   stage %d/%d   avg %.0f  min %.0f fps",
             s_pass + 1, BENCH_PASSES, pass_name(s_pass),
             seg, s_nwp - 1, avg, mn);

    if (progress) {
        double denom = s_total_dur * BENCH_PASSES;
        double done  = s_pass * s_total_dur + s_elapsed;
        *progress = denom > 0.0 ? (float)(done / denom) : 0.0f;
    }
    return 1;
}

void benchmark_shutdown(void) {
    free(s_wp);   s_wp = NULL;   s_nwp = s_wp_cap = 0;
    for (int p = 0; p < BENCH_PASSES; p++) { free(s_stat[p]); s_stat[p] = NULL; }
    free(s_samp); s_samp = NULL; s_nsamp = s_samp_cap = 0;
    s_active = s_running = 0;
}
