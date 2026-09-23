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
#include "frame.h"
#include "camera.h"
#include "galaxy.h"
#include "body.h"
#include "lifecycle.h"
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
    float  hold_s;     /* seconds parked on the object once arrived          */
    char   shot[40];   /* screenshot basename; "" = this stage captures none  */
    float  shot_at;    /* fraction of the hold at which to capture (0..1)     */
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
#define BENCH_PASSES 2   /* array sizing only — the run length is s_passes */
static const char *pass_name(int p) { return p == 0 ? "galaxies ON" : "galaxies OFF"; }

/* One pass by default: the second exists only to price the galaxy layer, and it
 * doubles a ~7-minute run. --benchmark-ab opts back in. */
static int s_passes = 1;

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
static int    s_ns_idx = -1;      /* remnant minted for the tour, -1 = none */
static char   s_ns_label[48] = "Stellar remnant";

/* Each stage is flown in two phases: TRANSIT (moving, looking where it is
 * going) then HOLD (parked on the object). Only HOLD frames are recorded — a
 * transit frame is a smear across changing scene content and pollutes the
 * per-object numbers this report exists to give. */
#define PH_TRANSIT 0
#define PH_HOLD    1
static int    s_phase = PH_TRANSIT;
static double s_transit_sum = 0.0;   /* aggregate transit timing, reported once */
static int    s_transit_frames = 0;

/* Live supernova stage: one progenitor per pass, so both passes watch a fresh
 * explosion rather than pass B inheriting pass A's remnant. */
static int    s_sn_stage = -1;            /* waypoint index of the SN stage */
static int    s_sn_prog[BENCH_PASSES] = { -1, -1 };
static int    s_sn_fired = 0;             /* death triggered this pass? */

#define ORBIT_DEG_PER_S 6.0   /* gentle parallax during HOLD */

/* Per-stage screenshots (--benchmark-shots DIR). Requested here, taken by
 * main.c after the frame is rendered — benchmark.c holds no GL. */
static char s_shot_dir[512]  = "";
static char s_pending_shot[640] = "";
static int  s_has_pending    = 0;
static int  s_shot_done      = 0;   /* captured this stage already? */
static int  s_shot_skip      = 0;   /* frames to exclude after a capture */

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
                    const char *label, float travel, float hold) {
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
    w->hold_s   = hold;
    w->shot[0]  = '\0';
    w->shot_at  = 0.6f;
    s_nwp++;
}

/* Tag the most recently pushed stage for a screenshot. Kept separate from the
 * wp_push* signatures so the three push helpers stay readable. */
static void wp_mark_shot(const char *name, float at) {
    if (s_nwp < 1) return;
    Waypoint *w = &s_wp[s_nwp - 1];
    snprintf(w->shot, sizeof(w->shot), "%s", name);
    w->shot_at = at;
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

/* Find a live body by (partial) name; -1 if absent. */
static int body_find(const char *needle) {
    for (int i = 0; i < g_nbodies; i++)
        if (g_bodies[i].alive && strstr(g_bodies[i].name, needle)) return i;
    return -1;
}

/* Push a leg that frames body `bi` from a 3/4 angle, `mult` body radii out.
 * Compact objects (black holes, AGN, remnants) are the most expensive things
 * the renderer draws — the BH pass alone snapshots the scene via
 * post_grab_scene() to lens the background — so the tour has to actually visit
 * one for the report to mean anything. Body pos/radius are metres; waypoints
 * are AU, hence the RS scaling. */
static void wp_push_body(int bi, double mult, const char *label,
                         float travel, float hold) {
    if (bi < 0) return;
    const Body *b = &g_bodies[bi];
    double sun_m[3];
    frame_local_to_sun_m(b->pos, sun_m);      /* waypoints are Sun frame */
    double c[3] = { sun_m[0] * RS, sun_m[1] * RS, sun_m[2] * RS };
    double r = b->radius * RS;
    if (r <= 0.0) r = 1.0;

    /* Off-axis so a disk reads as an ellipse and a jet is not end-on. For an
     * AGN the jet follows agn_axis when authored; tilt away from it. */
    double axis[3] = { 0.0, 1.0, 0.0 };
    if (body_bh(b)->agn_axis[0] != 0.0f || body_bh(b)->agn_axis[1] != 0.0f || body_bh(b)->agn_axis[2] != 0.0f) {
        axis[0] = body_bh(b)->agn_axis[0]; axis[1] = body_bh(b)->agn_axis[1]; axis[2] = body_bh(b)->agn_axis[2];
    }
    v_norm(axis);
    double up[3] = {0.0, 0.0, 1.0};
    if (fabs(axis[2]) > 0.9) { up[0]=1.0; up[1]=0.0; up[2]=0.0; }
    double perp[3]; v_cross(axis, up, perp); v_norm(perp);

    double dir[3];
    for (int i = 0; i < 3; i++) dir[i] = axis[i]*0.45 + perp[i]*0.89;
    v_norm(dir);

    double pos[3];
    for (int i = 0; i < 3; i++) pos[i] = c[i] + dir[i] * r * mult;
    /* Angular diameter at the framing distance: a stage that draws the object
     * at a fraction of a degree is measuring an empty sky, not a compact
     * object, so make it checkable from the log. */
    fprintf(stdout, "[Benchmark]   stage '%s': %s, r=%.3g AU, %.1fx out, "
                    "subtends %.1f deg\n",
            label, b->name, r, mult, 2.0 * atan2(1.0, mult) * 180.0 / PI);
    wp_push(pos, c, label, travel, hold);
}

/* The `n` most massive evolvable stars, heaviest first. Returns how many were
 * found. Distinct progenitors are needed because a star can only die once: the
 * static remnant stage consumes one, and the live supernova stage consumes one
 * per A/B pass. */
static int find_supernova_candidates(int *out, int n) {
    int found = 0;
    for (int k = 0; k < n; k++) {
        int best = -1; double best_mass = 0.0;
        for (int i = 0; i < g_nbodies; i++) {
            if (!lifecycle_is_evolvable_star(i)) continue;
            int taken = 0;
            for (int j = 0; j < found; j++) if (out[j] == i) taken = 1;
            if (taken) continue;
            if (g_bodies[i].mass > best_mass) { best_mass = g_bodies[i].mass; best = i; }
        }
        if (best < 0) break;
        out[found++] = best;
    }
    return found;
}

/* Push a leg that frames galaxy `gi` from a 3/4 angle, `mult` radii out. */
static void wp_push_galaxy(int gi, double mult, const char *label,
                           float travel, float hold) {
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
    wp_push(pos, c, label, travel, hold);
}

/* ------------------------------------------------------------- lifecycle */

/* travel_s / hold_s per stage. The hold is where the measurement happens, so it
 * is the larger of the two for anything whose render cost we actually care
 * about. */
#define T_NEAR 4.0f
#define T_FAR  5.0f
#define H_STD  5.0f
#define H_BIG  6.5f   /* the heavy passes: AGN, supernova, dense inner system */

static void build_tour(int pass) {
    s_nwp = 0; s_total_dur = 0.0; s_sn_stage = -1;

    /* 0 — start: Sol, close overview (travel/hold of wp0 are unused). */
    { double p[3] = {0.0, 3.0, 9.0}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Sol System", 0.0f, 0.0f); }

    /* 1 — the inner system: the densest per-body workload in the tour. */
    { double p[3] = {7.0, 4.0, 15.0}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Inner System", T_NEAR, H_BIG); }
    wp_mark_shot("solar_system", 0.60f);

    /* 2 — pull out; the Sun collapses to a point of light. */
    { double p[3] = {0.0, 2.5e4, 6.0e4}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Leaving Sol", T_NEAR, H_STD); }

    /* 3 — rise above the disc into the stellar neighbourhood. */
    { double p[3] = {0.0, 35.0*LY_AU, 55.0*LY_AU}, f[3] = {0.0, 0.0, 0.0};
      wp_push(p, f, "Solar Neighborhood", T_FAR, H_STD); }
    wp_mark_shot("solar_neighborhood", 0.60f);

    /* 4 — a live core collapse. The death fires on arrival (see on_arrive), so
     * the hold covers the flash and the expanding volumetric ejecta — the one
     * heavy render path a static tour can never reach. */
    if (s_sn_prog[pass] >= 0) {
        s_sn_stage = s_nwp;
        wp_push_body(s_sn_prog[pass], 2200.0, "Supernova (live)", T_FAR, H_BIG);
    wp_mark_shot("supernova", 0.35f);
    }

    /* 5 — the remnant minted before pass A: identical in both passes. */
    wp_push_body(s_ns_idx, 30.0, s_ns_label, T_NEAR, H_STD);
    wp_mark_shot("neutron_star", 0.60f);

    /* 6 — a stellar-mass black hole up close: accretion disk, event-horizon
     * shadow, and the lensed background (the scene-snapshot path). */
    wp_push_body(body_find("Cygnus X-1"), 25.0, "Cygnus X-1 (stellar BH)", T_FAR, H_STD);
    wp_mark_shot("black_hole_stellar", 0.60f);

    /* 7/8 — the Milky Way itself (galaxy 0), face-on then a diving 3/4. */
    int mw = galaxy_find("Milky Way");
    if (mw >= 0) {
        double c[3]; galaxy_position(mw, c);
        double r = galaxy_radius_au(mw); if (r <= 0.0) r = 1.0;
        double axis[3]; { float a[3]; galaxy_axis(mw, a);
            axis[0]=a[0]; axis[1]=a[1]; axis[2]=a[2]; }
        v_norm(axis);
        double p[3];
        for (int i = 0; i < 3; i++) p[i] = c[i] + axis[i] * r * 1.8;
        wp_push(p, c, "Milky Way - face on", T_FAR, H_STD);
    wp_mark_shot("galaxy_milkyway", 0.60f);
        wp_push_galaxy(mw, 1.15, "Milky Way - spiral arms", T_FAR, H_STD);
    }

    /* 9 — the galaxy's own supermassive hole, a nucleus inside its host. */
    wp_push_body(body_find("Sagittarius A"), 25.0, "Sagittarius A* (SMBH)", T_FAR, H_STD);
    wp_mark_shot("smbh_sagittarius_a", 0.60f);

    /* 10+ — a grand tour of the neighbours, with their nuclei. */
    wp_push_galaxy(galaxy_find("LMC"),        2.4, "Large Magellanic Cloud", T_FAR, H_STD);
    wp_push_galaxy(galaxy_find("Andromeda"),  2.2, "Andromeda (M31)",        T_FAR, H_STD);
    wp_mark_shot("galaxy_andromeda", 0.60f);
    wp_push_body(body_find("M31*"), 25.0, "M31* (hosted nucleus)", T_NEAR, H_BIG);
    wp_mark_shot("agn_hosted_nucleus", 0.60f);
    wp_push_galaxy(galaxy_find("Triangulum"), 2.3, "Triangulum (M33)",       T_FAR, H_STD);
    wp_push_galaxy(galaxy_find("M81"),        2.3, "Bode's Galaxy (M81)",    T_FAR, H_STD);

    /* The AGN passes: dust torus + relativistic jets + hot core on top of the
     * disk/shadow/lensing stack. The heaviest per-pixel work in the renderer. */
    wp_push_body(body_find("M87*"),   22.0, "M87* (SMBH shadow)",     T_FAR, H_BIG);
    wp_mark_shot("intergalactic_m87", 0.60f);
    wp_push_body(body_find("3C 273"), 30.0, "3C 273 (quasar + jets)", T_FAR, H_BIG);
    wp_mark_shot("quasar_3c273", 0.60f);
    wp_push_body(body_find("TON 618"), 30.0, "TON 618 (max-mass AGN)", T_FAR, H_BIG);
    wp_mark_shot("agn_ton618", 0.60f);

    wp_push_galaxy(galaxy_find("Sombrero"),   2.4, "Sombrero (M104)",        T_FAR, H_STD);
    wp_push_galaxy(galaxy_find("Centaurus"),  2.3, "Centaurus A",            T_FAR, H_STD);

    /* close the loop back on the Milky Way for a clean ending frame. */
    if (mw >= 0) wp_push_galaxy(mw, 1.8, "Home - Milky Way", T_FAR, H_STD);

    for (int i = 1; i < s_nwp; i++) s_total_dur += s_wp[i].travel_s + s_wp[i].hold_s;
}

/* Called once, the moment the camera settles onto stage `seg`. */
static void on_arrive(int seg) {
    if (seg != s_sn_stage || s_sn_fired) return;
    int prog = s_sn_prog[s_pass];
    if (prog < 0) return;
    double msun = g_bodies[prog].mass / SOLAR_MASS_KG;
    char nm[32]; snprintf(nm, sizeof(nm), "%s", g_bodies[prog].name);
    if (lifecycle_trigger_death(prog) > 0)
        fprintf(stdout, "[Benchmark]   detonated %s (%.1f Msun) on arrival\n", nm, msun);
    s_sn_fired = 1;
}

/* Mint a remnant for the tour, and reserve the live-supernova progenitors.
 * Remnant phases are reachable only through stellar evolution, never authored
 * in a universe JSON, so the tour has to create one. This runs once, before
 * pass A, so the static remnant stage is identical in both passes; the live
 * supernova stage uses its own per-pass progenitor (see on_arrive). */
static void mint_neutron_star(void) {
    s_ns_idx = -1;
    /* Reserve BENCH_PASSES + 1 distinct progenitors: one per pass for the live
     * supernova stage, plus one for the static remnant. */
    int cand[BENCH_PASSES + 1];
    int ncand = find_supernova_candidates(cand, BENCH_PASSES + 1);

    /* The heaviest star goes to the *static* remnant stage: that stage exists
     * to draw a neutron star, and only a core-collapse progenitor yields one.
     * The live-supernova stages care about the ejecta raymarch, which looks the
     * same whichever remnant it leaves behind, so they take the lighter ones. */
    for (int i = 0; i < BENCH_PASSES; i++)
        s_sn_prog[i] = (ncand > i + 1) ? cand[i + 1] : -1;
    if (s_sn_prog[BENCH_PASSES - 1] < 0) {
        for (int i = 0; i < BENCH_PASSES; i++) s_sn_prog[i] = -1;
        fprintf(stdout, "[Benchmark] fewer than %d evolvable stars — no live "
                        "supernova stage\n", BENCH_PASSES + 1);
    }

    int prog = (ncand > 0) ? cand[0] : -1;
    if (prog < 0) {
        fprintf(stdout, "[Benchmark] no evolvable star — skipping the remnant "
                        "stage\n");
        return;
    }
    char   prog_name[32];
    double prog_msun = g_bodies[prog].mass / SOLAR_MASS_KG;
    snprintf(prog_name, sizeof(prog_name), "%s", g_bodies[prog].name);
    if (!lifecycle_will_supernova(g_bodies[prog].mass))
        fprintf(stdout, "[Benchmark] remnant progenitor %s is %.1f Msun "
                        "(< %.0f) — that stage will draw a white dwarf, not a "
                        "neutron star\n",
                prog_name, prog_msun, (double)SUPERNOVA_MASS_MSUN);

    int remnant = lifecycle_trigger_death(prog);
    if (remnant <= 0) {
        fprintf(stdout, "[Benchmark] death of %s produced no remnant\n", prog_name);
        return;
    }
    fprintf(stdout, "[Benchmark] %s (%.1f Msun) -> %s\n", prog_name, prog_msun,
            lifecycle_phase_name(g_bodies[remnant].star_phase));
    snprintf(s_ns_label, sizeof(s_ns_label), "%s (remnant)",
             lifecycle_phase_name(g_bodies[remnant].star_phase));
    s_ns_idx = remnant;
}

/* Waypoints are Sun-frame positions (galaxies, the Sun); the camera lives in
 * the local frame (frame.h). */
static void set_cam_sun(const double p[3])
{
    for (int i = 0; i < 3; i++) g_cam.pos[i] = p[i] - g_frame_origin_au[i];
}

void benchmark_start(void) {
    mint_neutron_star();
    build_tour(0);
    if (s_nwp < 2) { fprintf(stderr, "[Benchmark] no tour to run\n"); return; }
    stats_alloc_reset();

    s_nsamp = 0;                 /* keep any prior sample allocation */
    s_pass = 0;                  /* pass A: galaxies on */
    s_gal_was = galaxy_enabled();
    galaxy_set_enabled(1);
    s_seg = 1; s_seg_t = 0.0; s_elapsed = 0.0;
    s_phase = PH_TRANSIT; s_sn_fired = 0;
    s_transit_sum = 0.0; s_transit_frames = 0;
    s_active = 1; s_running = 1; s_done_timer = 0.0; s_warmup = 2;
    s_summary[0] = '\0';

    /* start pose = waypoint 0 */
    set_cam_sun(s_wp[0].pos);

    fprintf(stdout,
            "[Benchmark] starting flythrough: %d stages x %d pass(es) "
            "~%.0f s — timing HOLD frames only\n",
            s_nwp - 1, s_passes, s_total_dur * s_passes);
    if (s_passes > 1)
        fprintf(stdout, "[Benchmark] pass 1/%d — %s\n", s_passes, pass_name(0));
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
    if (s_passes < 2) {
        /* Single pass: avg and 1% low per stage, nothing to difference. */
        double tot_dt = 0.0; int tot_fr = 0;
        fprintf(stdout, "[Benchmark] ================ RESULTS (galaxies ON) ================\n");
        fprintf(stdout, "[Benchmark] %-24s %9s %9s\n", "Stage", "avg fps", "1% low");
        for (int s = 1; s < s_nwp; s++) {
            fprintf(stdout, "[Benchmark] %-24s %9.1f %9.1f\n",
                    s_wp[s].label, seg_avg(0, s), pct_low_fps(s, 0));
            tot_fr += s_stat[0][s].frames; tot_dt += s_stat[0][s].sum_dt;
        }
        double ov = tot_fr > 0 ? tot_fr / tot_dt : 0.0;
        fprintf(stdout, "[Benchmark] %-24s %9s %9s\n",
                "------------------------", "---------", "---------");
        fprintf(stdout, "[Benchmark] %-24s %9.1f\n", "OVERALL", ov);
        fprintf(stdout, "[Benchmark] =======================================================\n");
        snprintf(s_summary, sizeof(s_summary), "Benchmark done   %.0f fps average", ov);
        return;
    }

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
     * sub-50 us frames (>20k fps) — a timing glitch, not a rendered frame. */
    if (s_warmup > 0) { s_warmup--; dt_real = 0.0f; }

    /* Record HOLD frames only. During transit the scene content is changing
     * under the camera, so those frames describe the journey, not the object;
     * they are accumulated separately and reported as one aggregate line. */
    if (s_shot_skip > 0 && dt_real > 5.0e-5f) { s_shot_skip--; dt_real = 0.0f; }

    if (dt_real > 5.0e-5f) {
        if (s_phase == PH_HOLD) {
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
        } else {
            s_transit_sum += (double)dt_real;
            s_transit_frames++;
        }
    }

    s_seg_t   += (double)dt_real;
    s_elapsed += (double)dt_real;

    /* ---- phase / stage advance ---------------------------------------- */
    for (;;) {
        const Waypoint *w = &s_wp[s_seg];
        if (s_phase == PH_TRANSIT) {
            double dur = w->travel_s > 1e-3f ? w->travel_s : 1e-3f;
            if (s_seg_t < dur) break;
            s_seg_t -= dur;
            s_phase  = PH_HOLD;
            s_shot_done = 0;
            on_arrive(s_seg);          /* fire the supernova, if this is it */
        } else {
            if (s_seg_t < w->hold_s) break;
            s_seg_t -= w->hold_s;
            s_phase  = PH_TRANSIT;
            s_seg++;
            if (s_seg >= s_nwp) break;
        }
    }

    if (s_seg >= s_nwp) {             /* current pass finished */
        set_cam_sun(s_wp[s_nwp-1].pos);
        if (s_pass + 1 < s_passes) {
            /* Next pass: same tour, galaxies off, and its own SN progenitor —
             * so the tour is rebuilt (identical stage count, one moved
             * waypoint) rather than replayed. */
            s_pass++;
            galaxy_set_enabled(s_pass == 1 ? 0 : 1);
            int prev_nwp = s_nwp;
            build_tour(s_pass);
            if (s_nwp != prev_nwp) {
                fprintf(stderr, "[Benchmark] stage count changed between passes "
                                "(%d -> %d); results would misalign\n",
                        prev_nwp, s_nwp);
                s_running = 0; s_active = 0; return;
            }
            s_seg = 1; s_seg_t = 0.0; s_elapsed = 0.0; s_warmup = 2;
            s_phase = PH_TRANSIT; s_sn_fired = 0;
            set_cam_sun(s_wp[0].pos);
            fprintf(stdout, "[Benchmark] pass %d/%d — %s\n",
                    s_pass + 1, s_passes, pass_name(s_pass));
            return;
        }
        /* all passes done */
        galaxy_set_enabled(s_gal_was);   /* restore the user's setting */
        s_running = 0;
        s_done_timer = 8.0;
        print_report();
        return;
    }

    /* ---- pose ---------------------------------------------------------- */
    const Waypoint *a = &s_wp[s_seg - 1];
    const Waypoint *b = &s_wp[s_seg];
    double look[3], cam[3];                   /* Sun frame, like the waypoints */

    if (s_phase == PH_TRANSIT) {
        double dur = b->travel_s > 1e-3f ? b->travel_s : 1e-3f;
        double u   = smoother(s_seg_t / dur);
        for (int i = 0; i < 3; i++)
            cam[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * u;

        /* Look where we are going: aim at the destination *camera* point, so
         * the gaze lies along the flight path instead of dragging sideways
         * across the target. Over the last third, ease onto the object itself
         * so arrival is already framed. */
        double w = smoother((u - 0.65) / 0.35);
        for (int i = 0; i < 3; i++)
            look[i] = b->pos[i] + (b->focus[i] - b->pos[i]) * w;
    } else {
        /* Capture once, partway through the hold: late enough that the view has
         * settled, and for the supernova early enough that the ejecta is still
         * expanding. Pass A only — the galaxies-OFF pass would write images of
         * a scene missing its galaxies. */
        if (!s_shot_done && s_shot_dir[0] && s_pass == 0 && b->shot[0] &&
            s_seg_t >= (double)b->shot_at * b->hold_s) {
            snprintf(s_pending_shot, sizeof(s_pending_shot), "%s/%02d_%s.ppm",
                     s_shot_dir, s_seg, b->shot);
            s_has_pending = 1;
            s_shot_done   = 1;
            s_shot_skip   = 2;   /* this frame and the next carry the readback */
        }

        /* Parked: hold the framing distance and drift slowly around the object
         * so the sample is a view of it rather than one frozen frame. */
        double ang = ORBIT_DEG_PER_S * s_seg_t * PI / 180.0;
        double ca = cos(ang), sa = sin(ang);
        double off[3];
        for (int i = 0; i < 3; i++) off[i] = b->pos[i] - b->focus[i];
        double rx =  off[0]*ca + off[2]*sa;
        double rz = -off[0]*sa + off[2]*ca;
        cam[0] = b->focus[0] + rx;
        cam[1] = b->focus[1] + off[1];
        cam[2] = b->focus[2] + rz;
        for (int i = 0; i < 3; i++) look[i] = b->focus[i];
    }

    /* orient toward the look point (cam_get_dir convention) */
    set_cam_sun(cam);
    double d[3] = { look[0]-cam[0], look[1]-cam[1], look[2]-cam[2] };
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

void benchmark_set_ab(int on) { s_passes = on ? BENCH_PASSES : 1; }

void benchmark_set_shot_dir(const char *dir) {
    snprintf(s_shot_dir, sizeof(s_shot_dir), "%s", dir ? dir : "");
}

const char *benchmark_take_shot_path(void) {
    if (!s_has_pending) return NULL;
    s_has_pending = 0;
    return s_pending_shot;
}

void benchmark_shutdown(void) {
    free(s_wp);   s_wp = NULL;   s_nwp = s_wp_cap = 0;
    for (int p = 0; p < BENCH_PASSES; p++) { free(s_stat[p]); s_stat[p] = NULL; }
    free(s_samp); s_samp = NULL; s_nsamp = s_samp_cap = 0;
    s_active = s_running = 0;
}
