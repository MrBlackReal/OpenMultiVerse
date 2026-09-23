/*
 * freeze.c — frozen systems keep their shape and their clock. See freeze.h.
 */
#include "freeze.h"
#include "body.h"
#include "physics.h"
#include "frame.h"
#include "camera.h"
#include "laws.h"
#include "settings.h"
#include "trails.h"
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BodyHandle h;
    int    parent;          /* index in this record; -1 for the root         */
    double r[3], v[3];      /* state relative to that parent, m and m/s      */
    double mu;              /* G * (M_parent + m)                            */
} FrozenBody;

typedef struct {
    BodyHandle  root;
    double      t;          /* g_sim_time when frozen                        */
    int         n;
    FrozenBody *b;          /* b[0] is the root; parents precede children    */
} FrozenSys;

static FrozenSys  *s_rec = NULL;
static int         s_nrec = 0, s_rec_cap = 0;

/* Per body slot: record index of the system it roots (-1 = none), the update
 * tick it was last seen active, and scratch for building a record. */
static int        *s_rec_of = NULL;
static unsigned   *s_stamp  = NULL;
static int        *s_tmp    = NULL;
static int         s_body_cap = 0;

static BodyHandle *s_prev = NULL;          /* roots active at the last update */
static int         s_nprev = 0, s_prev_cap = 0;

static unsigned    s_tick = 0;
static unsigned    s_gen  = 0;             /* physics generation last swept   */
static int         s_listening = 0;

static void ensure_body_cap(void)
{
    if (g_nbodies <= s_body_cap) return;
    int cap = s_body_cap ? s_body_cap : 1024;
    while (cap < g_nbodies) cap *= 2;
    int      *ro = realloc(s_rec_of, (size_t)cap * sizeof *ro);
    unsigned *st = realloc(s_stamp,  (size_t)cap * sizeof *st);
    int      *tm = realloc(s_tmp,    (size_t)cap * sizeof *tm);
    if (!ro || !st || !tm) { fprintf(stderr, "[freeze] alloc failed\n"); exit(1); }
    for (int i = s_body_cap; i < cap; i++) { ro[i] = -1; st[i] = 0; tm[i] = -1; }
    s_rec_of = ro;  s_stamp = st;  s_tmp = tm;  s_body_cap = cap;
}

static void drop_record(int r)
{
    int root = s_rec[r].root.index;
    if (root >= 0 && root < s_body_cap && s_rec_of[root] == r) s_rec_of[root] = -1;
    free(s_rec[r].b);
    if (r != --s_nrec) {
        s_rec[r] = s_rec[s_nrec];
        int moved = s_rec[r].root.index;
        if (moved >= 0 && moved < s_body_cap) s_rec_of[moved] = r;
    }
}

/* Record the system rooted at `root` (members from its physics slot). */
static void freeze_system(int root)
{
    if (root < 0 || root >= s_body_cap || s_rec_of[root] >= 0) return;
    int slot = physics_root_slot(root);
    const int *mem = NULL;
    int n = physics_system_members(slot, &mem);
    if (n <= 1) return;

    /* Parents before children: counting sort on depth below the root. */
    enum { MAX_DEPTH = 64 };
    int count[MAX_DEPTH + 1] = { 0 };
    int *ord = malloc((size_t)n * sizeof *ord);
    int *dep = malloc((size_t)n * sizeof *dep);
    FrozenBody *fb = malloc((size_t)n * sizeof *fb);
    if (!ord || !dep || !fb) { free(ord); free(dep); free(fb); return; }
    for (int k = 0; k < n; k++) {
        int d = 0;
        for (int p = mem[k]; p != root && g_bodies[p].parent >= 0 && d < MAX_DEPTH;
             p = g_bodies[p].parent) d++;
        dep[k] = d;  count[d]++;
    }
    for (int d = 0, at = 0; d <= MAX_DEPTH; d++) { int c = count[d]; count[d] = at; at += c; }
    for (int k = 0; k < n; k++) ord[count[dep[k]]++] = mem[k];
    free(dep);
    if (ord[0] != root) { free(ord); free(fb); return; }   /* malformed system */

    for (int k = 0; k < n; k++) s_tmp[ord[k]] = k;
    for (int k = 0; k < n; k++) {
        const Body *b = &g_bodies[ord[k]];
        FrozenBody *e = &fb[k];
        e->h = body_handle(ord[k]);
        if (k == 0) {
            e->parent = -1;  e->mu = 0.0;
            memset(e->r, 0, sizeof e->r);  memset(e->v, 0, sizeof e->v);
            continue;
        }
        int p  = b->parent;
        int pk = (p >= 0 && p < g_nbodies && s_tmp[p] >= 0 && s_tmp[p] < k &&
                  ord[s_tmp[p]] == p) ? s_tmp[p] : 0;
        const Body *pb = &g_bodies[ord[pk]];
        e->parent = pk;
        e->mu = g_laws.G * (pb->mass + b->mass);
        for (int q = 0; q < 3; q++) {
            e->r[q] = b->pos[q] - pb->pos[q];
            e->v[q] = b->vel[q] - pb->vel[q];
        }
    }
    for (int k = 0; k < n; k++) s_tmp[ord[k]] = -1;
    free(ord);

    if (s_nrec == s_rec_cap) {
        int cap = s_rec_cap ? s_rec_cap * 2 : 64;
        FrozenSys *t = realloc(s_rec, (size_t)cap * sizeof *t);
        if (!t) { free(fb); return; }
        s_rec = t;  s_rec_cap = cap;
    }
    s_rec[s_nrec] = (FrozenSys){ body_handle(root), g_sim_time, n, fb };
    s_rec_of[root] = s_nrec++;
}

/* Place every member again from its parent, g_sim_time - t later. */
static void thaw_record(int r)
{
    FrozenSys *fs = &s_rec[r];
    int root = body_handle_resolve(fs->root);
    double dt = g_sim_time - fs->t;
    double (*ap)[3] = malloc((size_t)fs->n * sizeof *ap);
    double (*av)[3] = malloc((size_t)fs->n * sizeof *av);
    char   *ok = malloc((size_t)fs->n);
    if (root >= 0 && ap && av && ok) {
        for (int k = 0; k < fs->n; k++) {
            const FrozenBody *e = &fs->b[k];
            int idx = body_handle_resolve(e->h);
            ok[k] = 0;
            if (idx < 0) continue;
            Body *b = &g_bodies[idx];
            if (k == 0) {
                /* The root stays where the rest of the frozen sky left it. */
                for (int q = 0; q < 3; q++) { ap[0][q] = b->pos[q];  av[0][q] = b->vel[q]; }
            } else {
                if (!ok[e->parent] || body_root_star(idx) != root) continue;
                double rr[3], vv[3];
                if (dt != 0.0) kepler_propagate(e->r, e->v, e->mu, dt, rr, vv);
                else { memcpy(rr, e->r, sizeof rr);  memcpy(vv, e->v, sizeof vv); }
                for (int q = 0; q < 3; q++) {
                    ap[k][q] = ap[e->parent][q] + rr[q];
                    av[k][q] = av[e->parent][q] + vv[q];
                    b->pos[q] = ap[k][q];
                    b->vel[q] = av[k][q];
                }
                if (dt != 0.0) trails_reset_body(idx);
            }
            if (dt != 0.0)
                b->rotation_angle = fmod(b->rotation_angle + b->rotation_rate * dt, 2.0 * PI);
            ok[k] = 1;
        }
    }
    free(ap);  free(av);  free(ok);
    drop_record(r);
}

/* A jump is about to round away what the active systems it leaves behind
 * hold: freeze them first, while their positions are still exact. */
static void freeze_pre_rebase(const double d_m[3])
{
    (void)d_m;
    double cam[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
    double R = g_settings.active_radius_ly * LY;
    ensure_body_cap();
    for (int i = 0; i < s_nprev; i++) {
        int root = body_handle_resolve(s_prev[i]);
        if (root < 0) continue;
        double dx = g_bodies[root].pos[0] - cam[0];
        double dy = g_bodies[root].pos[1] - cam[1];
        double dz = g_bodies[root].pos[2] - cam[2];
        if (dx*dx + dy*dy + dz*dz > R * R) freeze_system(root);
    }
}

void freeze_update(const int *slots, int n)
{
    if (!s_listening) { frame_on_pre_rebase(freeze_pre_rebase); s_listening = 1; }
    ensure_body_cap();
    if (++s_tick == 0) {                      /* wrapped: stale stamps could match */
        memset(s_stamp, 0, (size_t)s_body_cap * sizeof *s_stamp);
        s_tick = 1;
    }

    if (n > s_prev_cap) {
        int cap = s_prev_cap ? s_prev_cap : 64;
        while (cap < n) cap *= 2;
        BodyHandle *t = realloc(s_prev, (size_t)cap * sizeof *t);
        if (!t) { fprintf(stderr, "[freeze] alloc failed\n"); exit(1); }
        s_prev = t;  s_prev_cap = cap;
    }

    /* Came back: thaw. */
    for (int a = 0; a < n; a++) {
        int root = physics_system_root(slots[a]);
        if (root < 0 || root >= s_body_cap) continue;
        s_stamp[root] = s_tick;
        int r = s_rec_of[root];
        if (r >= 0) {
            if (body_handle_resolve(s_rec[r].root) == root) thaw_record(r);
            else drop_record(r);              /* slot reused by another body */
        }
    }

    /* Left: freeze. */
    for (int i = 0; i < s_nprev; i++) {
        int root = body_handle_resolve(s_prev[i]);
        if (root >= 0 && s_stamp[root] != s_tick) freeze_system(root);
    }

    /* The body set changed: freeze every inactive system not yet recorded
     * (at the first update, every system that was loaded far away), and
     * drop records whose root is gone. */
    unsigned gen = physics_system_generation();
    if (gen != s_gen) {
        s_gen = gen;
        for (int r = s_nrec - 1; r >= 0; r--)
            if (body_handle_resolve(s_rec[r].root) < 0) drop_record(r);
        int ns = physics_system_count();
        for (int s = 0; s < ns; s++) {
            int root = physics_system_root(s);
            if (root >= 0 && root < s_body_cap && s_stamp[root] != s_tick)
                freeze_system(root);
        }
    }

    s_nprev = 0;
    for (int a = 0; a < n; a++) {
        int root = physics_system_root(slots[a]);
        if (root >= 0) s_prev[s_nprev++] = body_handle(root);
    }
}

void freeze_reset(void)
{
    for (int r = 0; r < s_nrec; r++) free(s_rec[r].b);
    s_nrec = 0;
    s_nprev = 0;
    s_gen = 0;
    for (int i = 0; i < s_body_cap; i++) s_rec_of[i] = -1;
}

int freeze_count(void) { return s_nrec; }

static void cam_world_m(double out[3])
{
    for (int k = 0; k < 3; k++) out[k] = g_cam.pos[k] * AU;
}

/* ── self-test ───────────────────────────────────────────────────────────────
 * Fly from Sol to M87 (16.4 Mpc), wander there through 1000 rebases, wait
 * 1.37 years, fly back. Earth and the Moon must be exactly where two-body
 * motion puts them: within a millimetre, where riding the rebases as absolute
 * positions would have rounded them by the local spacing of doubles. */
int freeze_selftest(void)
{
    int fail = 0;
#define CHECK(c, ...) do { if (c) fprintf(stdout, "[selftest] ok:   " __VA_ARGS__); \
                           else { fprintf(stdout, "[selftest] FAIL: " __VA_ARGS__); fail++; } \
                           fprintf(stdout, "\n"); } while (0)
    int earth = body_find_named("Earth"), moon = body_find_named("Moon");
    int sun = earth >= 0 ? body_root_star(earth) : -1;
    CHECK(earth >= 0 && moon >= 0 && sun >= 0, "Sol, Earth and the Moon loaded");
    if (earth < 0 || moon < 0 || sun < 0) return 0;

    physics_refresh_timestep_model();
    double se_r[3], se_v[3], em_r[3], em_v[3];
    for (int q = 0; q < 3; q++) {
        se_r[q] = g_bodies[earth].pos[q] - g_bodies[sun].pos[q];
        se_v[q] = g_bodies[earth].vel[q] - g_bodies[sun].vel[q];
        em_r[q] = g_bodies[moon].pos[q]  - g_bodies[earth].pos[q];
        em_v[q] = g_bodies[moon].vel[q]  - g_bodies[earth].vel[q];
    }
    double sun_home_m[3];
    frame_local_to_sun_m(g_bodies[sun].pos, sun_home_m);

    double cam_m[3];
    const int *slots = NULL;
    cam_world_m(cam_m);
    int na = physics_active_systems(cam_m, g_settings.active_radius_ly * LY, &slots);
    freeze_update(slots, na);
    CHECK(s_rec_of[sun] < 0, "Sol live at home (%d systems frozen elsewhere)", s_nrec);

    const double m87_au[3] = { -3.244e12, 8.345e11, -1.169e11 };
    frame_place_camera_sun(m87_au);
    CHECK(s_rec_of[sun] >= 0, "Sol frozen by the jump, before the rebase rounded it");
    unsigned rng = 777u;
    for (int i = 0; i < 1000; i++) {
        double d[3];
        for (int k = 0; k < 3; k++) {
            rng = rng * 1664525u + 1013904223u;
            d[k] = ((double)(rng >> 8) / 16777216.0 - 0.5) * 200.0;
        }
        frame_rebase(d);
        cam_world_m(cam_m);
        na = physics_active_systems(cam_m, g_settings.active_radius_ly * LY, &slots);
        freeze_update(slots, na);
    }
    double drift = 0.0;
    for (int q = 0; q < 3; q++) {
        double e = g_bodies[earth].pos[q] - g_bodies[sun].pos[q] - se_r[q];
        drift += e * e;
    }
    fprintf(stdout, "[selftest] info: at M87 the stored Earth-from-Sun vector is off by "
            "%.3g km: what riding the rebases does\n", sqrt(drift) / 1e3);

    const double dt = 1.37 * 3.15576e7;
    g_sim_time += dt;
    double home_au[3] = { sun_home_m[0] / AU, sun_home_m[1] / AU + 1.0, sun_home_m[2] / AU };
    frame_place_camera_sun(home_au);
    cam_world_m(cam_m);
    na = physics_active_systems(cam_m, g_settings.active_radius_ly * LY, &slots);
    freeze_update(slots, na);
    CHECK(s_rec_of[sun] < 0, "Sol thawed on return");

    double exp_se[3], exp_em[3], v[3];
    kepler_propagate(se_r, se_v, g_laws.G * (g_bodies[sun].mass + g_bodies[earth].mass), dt, exp_se, v);
    kepler_propagate(em_r, em_v, g_laws.G * (g_bodies[earth].mass + g_bodies[moon].mass), dt, exp_em, v);
    double e_err = 0.0, m_err = 0.0;
    for (int q = 0; q < 3; q++) {
        e_err = fmax(e_err, fabs(g_bodies[earth].pos[q] - g_bodies[sun].pos[q]  - exp_se[q]));
        m_err = fmax(m_err, fabs(g_bodies[moon].pos[q]  - g_bodies[earth].pos[q] - exp_em[q]));
    }
    CHECK(e_err < 1e-3, "Earth 1.37 yr on around the Sun: error %.3g m", e_err);
    CHECK(m_err < 1e-3, "Moon 1.37 yr on around Earth: error %.3g m", m_err);
    double dot = 0.0, n0 = 0.0, n1 = 0.0;
    for (int q = 0; q < 3; q++) {
        dot += se_r[q] * exp_se[q];  n0 += se_r[q] * se_r[q];  n1 += exp_se[q] * exp_se[q];
    }
    fprintf(stdout, "[selftest] info: Earth's position swung %.1f deg while away\n",
            acos(fmax(-1.0, fmin(1.0, dot / sqrt(n0 * n1)))) * 180.0 / PI);
    g_sim_time -= dt;
#undef CHECK
    fprintf(stdout, "[selftest] freeze: %s (%d failure%s)\n", fail ? "FAILED" : "passed",
            fail, fail == 1 ? "" : "s");
    return fail == 0;
}
