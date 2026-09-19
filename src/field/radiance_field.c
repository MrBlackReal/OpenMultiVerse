/*
 * radiance_field.c — unified radiance transport field (roadmap Phase A #4).
 *
 * See radiance_field.h for the design contract.  Everything here is a plain
 * emitter list + closed-form luminosity models; no GL, no SDL.
 *
 * ── Luminosity models ─────────────────────────────────────────────────────
 *
 * Thermal stars:  L = L☉ · (R/R☉)² · h⁴, the Stefan-Boltzmann law with the
 * effective-temperature ratio h = T/T☉ from **spectral classification**
 * (spectral.c, roadmap §1.1): T is physical — mass + lifecycle phase — with
 * the old display-colour estimate as the fallback for massless catalog rows
 * (calibrated so Sol → T☉ either way, so Earth still gets ~1361 W/m²).
 *
 * Black holes / AGN:  L = η·Ṁ·c² (η = 0.1, matching accretion.c) when the
 * accretion model is running; otherwise the authored Eddington ratio seeds
 * L = agn_activity · L_edd = activity · 1.26e31 W · (M/M☉), so presets emit
 * sensibly at t=0 before any stellar time has advanced.  A bare, starved hole
 * (no disk, no activity, no Ṁ) emits nothing; a quiet hole that still wears a
 * disk floors at 1% Eddington (the rendered disk visibly glows, so it must
 * light its surroundings too).
 *
 * Supernovae:  transient emitters harvested from supernova.c's render events —
 * flash/core/cloud intensities scale a peak luminosity (~1e36 W, supernova
 * order of magnitude), anchored at the detonation point (the visible blast
 * stays there even as the remnant body drifts).  They carry body = -1: query
 * results identify them by position, not body index.  While any event is
 * active the field rebuilds every tick so the flash decay is smooth.
 */
#include "radiance_field.h"
#include "body.h"
#include "universe.h"   /* g_field_star_begin/end */
#include "physics.h"    /* physics_active_bodies (near field stars) */
#include "camera.h"
#include "nebula.h"
#include "galaxy.h"
#include "spectral.h"
#include "supernova.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Physical constants (SI). */
#define L_SUN      3.828e26        /* solar luminosity, W                    */
#define R_SUN      6.957e8         /* solar radius, m                        */
#define M_SUN      1.989e30        /* solar mass, kg                         */
#define C_LIGHT    2.99792458e8    /* speed of light, m/s                    */
#define EDD_PER_MSUN 1.26e31       /* Eddington luminosity per solar mass, W */
#define ACC_EFF    0.1             /* radiative efficiency η (accretion.c)   */

/* Temperature-ratio clamp for the Stefan-Boltzmann term: wide enough for the
 * physical range spectral.c produces (late M dwarfs ~1800 K up to hot
 * planetary-nebula nuclei; a neutron star's 6e5 K clamps here, but its tiny
 * radius keeps L sane), tight enough that h⁴ stays finite. */
#define HEAT_MIN      0.3
#define HEAT_MAX      20.0

/* A quiet hole that still wears an accretion disk (e.g. the Gargantua preset:
 * disk on, no authored activity) renders as a glowing disk, so it must light
 * its surroundings too: floor its luminosity at 1% Eddington.  A bare hole
 * (disk off, starved) emits nothing. */
#define DISK_FLOOR_EDD 0.01

/* Supernova peak luminosities per intensity unit (W).  The flash dominates,
 * the hot core carries the weeks after, the expanding cloud a faint tail. */
#define SN_L_FLASH 1.0e36
#define SN_L_CORE  2.0e35
#define SN_L_CLOUD 2.0e34

/* Nebular emission (roadmap §0.3 "nebular glow" as an *emitter*): each cloud
 * reradiates like an emission nebula powered by its embedded association —
 * L ∝ projected area, referenced to an Orion-class nebula (~1e5 L☉ at 10 ly).
 * Because L ∝ R², the irradiance just inside any cloud is a constant
 * L_ref/(4π·R_ref²) ≈ 3e-4 W/m² — deliberately below the 1e-3 W/m² nebula
 * *receiver* threshold, so a cloud never boosts itself, but a body drifting
 * through it picks up a faint coloured glow when no star outshines it. */
#define NEB_L_REF   3.8e31             /* W (≈1e5 L☉)                       */
#define NEB_R_REF_M (10.0 * 9.461e15)  /* 10 ly in metres                   */

/* Galaxy emission (roadmap §0.3 "galaxy light distribution", first step):
 * one integrated emitter per catalogue galaxy, L ∝ area referenced to a
 * Milky-Way-class disc (~2.5e10 L☉ at 50 kly). Interstellar-scale scenes are
 * numerically untouched (a Local Group galaxy delivers ~1e-9 W/m² here);
 * per-region light within a galaxy is future work for Layer 4.2. */
#define GAL_L_REF   1.0e37                 /* W (≈2.6e10 L☉)                */
#define GAL_R_REF_M (5.0e4 * 9.461e15)     /* 50 kly in metres              */

/* ── emitter list ─────────────────────────────────────────────────────────
 * Membership + luminosity are cached at rebuild.  Body emitters read their
 * position live from g_bodies at query time (orbits move between rebuilds);
 * transient (supernova) emitters use the snapshot pos — the blast is anchored. */
typedef struct {
    int    body;      /* g_bodies index, or -1 = body-less (SN / nebula)    */
    double lum;       /* luminosity, W                                      */
    double pos[3];    /* SI m; used when body < 0 (bodies are read live)    */
    double rmin2;     /* min distance² clamp (photosphere / core), m²       */
    float  col[3];    /* chromaticity (max component 1)                     */
    const char *label;/* display name for body-less emitters (static str)   */
    int    nebula;    /* nebula index for nebula emitters, else -1          */
} Emitter;

static Emitter *s_em = NULL;
static int      s_count = 0;
static int      s_cap   = 0;

/* Per-body luminosity lookup (0 for non-emitters), sized to g_nbodies. */
static double *s_body_lum = NULL;
static int     s_body_lum_cap = 0;

/* Scratch for radiance_field_sample: per-emitter irradiance from its first
 * pass, reused by the contributor count so emitter_irr is not evaluated twice. */
static double *s_sample_irr = NULL;
static int     s_sample_irr_cap = 0;

/* Rebuild throttle. */
static double s_since_rebuild = 0.0;
static int    s_last_nbodies  = -1;
static int    s_had_sn        = 0;   /* last rebuild harvested a supernova */
#define REBUILD_PERIOD_SEC 0.5

/* Thermal (photosphere) luminosity of a star-flagged body, W. */
static double star_luminosity(const Body *b)
{
    double t = spectral_t_eff(b);
    if (t <= 0.0) return 0.0;
    double rr = b->radius / R_SUN;
    double h  = t / SPECTRAL_T_SUN;
    if (h < HEAT_MIN) h = HEAT_MIN;
    if (h > HEAT_MAX) h = HEAT_MAX;
    return L_SUN * rr * rr * (h * h) * (h * h);
}

/* Accretion luminosity of a black hole, W (0 when bare + starved). */
static double bh_luminosity(const Body *b)
{
    double edd = EDD_PER_MSUN * (b->mass / M_SUN);
    double l = 0.0;
    if (b->mdot > 0.0)
        l = ACC_EFF * b->mdot * C_LIGHT * C_LIGHT;
    else if (b->agn_activity > 0.0f)
        l = (double)b->agn_activity * edd;
    if (b->accretion_disk > 0.0f && l < DISK_FLOOR_EDD * edd)
        l = DISK_FLOOR_EDD * edd;
    return l;
}

static Emitter *emitters_push(void)
{
    if (s_count >= s_cap) {
        int ncap = s_cap > 0 ? s_cap * 2 : 64;
        s_em  = (Emitter *)realloc(s_em, (size_t)ncap * sizeof(Emitter));
        s_cap = ncap;
    }
    return &s_em[s_count++];
}

/* Emitter position (SI m): live body position, or the transient snapshot. */
static void emitter_pos(const Emitter *e, double out[3])
{
    if (e->body >= 0) {
        out[0] = g_bodies[e->body].pos[0];
        out[1] = g_bodies[e->body].pos[1];
        out[2] = g_bodies[e->body].pos[2];
    } else {
        out[0] = e->pos[0];
        out[1] = e->pos[1];
        out[2] = e->pos[2];
    }
}

/* Incident flux of emitter e at pos_m (W/m²); 0 when dead/excluded. */
static double emitter_irr(const Emitter *e, const double pos_m[3],
                          int exclude_body)
{
    if (e->body >= 0 &&
        (e->body == exclude_body || !g_bodies[e->body].alive)) return 0.0;
    double p[3];
    emitter_pos(e, p);
    double dx = p[0] - pos_m[0];
    double dy = p[1] - pos_m[1];
    double dz = p[2] - pos_m[2];
    double d2 = dx*dx + dy*dy + dz*dz;
    if (d2 < e->rmin2) d2 = e->rmin2;   /* inside the source: clamp to surface */
    if (d2 <= 0.0) return 0.0;
    return e->lum / (4.0 * PI * d2);
}

/* ── spatial hash over star emitters (accelerates radiance_field_top) ───────
 * radiance_field_top used to scan every emitter (~10k for a galaxy catalog)
 * on each call, dozens of times per frame.  Body-backed emitters (stars/BHs)
 * are bucketed into a uniform 4-ly grid at rebuild, so a query scans only the
 * emitters in the cells around the query point, plus the handful of body-less
 * area emitters (nebulae/galaxies/supernovae) which are always considered
 * (they are the legitimately bright-but-distant sources).
 *
 * Correct for the visible result: a resolved body's parent star sits in the
 * same 4-ly cell and outshines anything a cell away by many orders of
 * magnitude (parent ≈ light-minutes vs neighbours ≈ light-years), so the ±1
 * neighbourhood never drops the true dominant emitter.  Only a body with NO
 * star within a few ly — an isolated rogue, rarely resolved and dim anyway —
 * could differ, and then only in its faintest contributor.  Disabled below
 * GRID_MIN_EMITTERS so every existing small preset stays bit-identical. */
#define GRID_CELL_M       (4.0 * 9.461e15)   /* 4 light-years, in metres      */
#define GRID_RADIUS       1                  /* ±1 cell → 3×3×3 search         */
#define GRID_MIN_EMITTERS 512

static int      *s_grid_head   = NULL;   /* [nbuckets] head emitter, -1 empty */
static int      *s_grid_next   = NULL;   /* [s_count] next in same bucket      */
static int       s_grid_nbuckets = 0;
static int      *s_area_em     = NULL;   /* indices of body-less emitters      */
static int       s_area_count  = 0, s_area_cap = 0;
static unsigned *s_em_stamp    = NULL;   /* [s_count] per-query visited stamp   */
static unsigned  s_query_stamp = 0;
static int       s_grid_active = 0;      /* 0 = fall back to the exact full scan */

static inline int grid_cell(double x) { return (int)floor(x / GRID_CELL_M); }

static inline unsigned grid_hash(int cx, int cy, int cz)
{
    unsigned h = (unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u
               ^ (unsigned)cz * 83492791u;
    return h & (unsigned)(s_grid_nbuckets - 1);
}

/* (Re)build the emitter grid + area list from the current s_em[].  Called at
 * the end of every rebuild. */
static void radiance_field_grid_build(void)
{
    s_grid_active = 0;
    s_area_count  = 0;
    for (int e = 0; e < s_count; e++) {
        if (s_em[e].body >= 0) continue;
        if (s_area_count >= s_area_cap) {
            s_area_cap = s_area_cap ? s_area_cap * 2 : 64;
            s_area_em  = realloc(s_area_em, (size_t)s_area_cap * sizeof(int));
        }
        s_area_em[s_area_count++] = e;
    }
    if (s_count < GRID_MIN_EMITTERS) return;   /* keep the exact full scan */

    int nb = 1;
    while (nb < 2 * s_count) nb <<= 1;
    s_grid_nbuckets = nb;
    s_grid_head = realloc(s_grid_head, (size_t)nb * sizeof(int));
    s_grid_next = realloc(s_grid_next, (size_t)s_count * sizeof(int));
    s_em_stamp  = realloc(s_em_stamp,  (size_t)s_count * sizeof(unsigned));
    if (!s_grid_head || !s_grid_next || !s_em_stamp) return;  /* stay full-scan */
    for (int b = 0; b < nb; b++) s_grid_head[b] = -1;
    memset(s_em_stamp, 0, (size_t)s_count * sizeof(unsigned));
    for (int e = 0; e < s_count; e++) {
        if (s_em[e].body < 0) continue;
        double p[3]; emitter_pos(&s_em[e], p);
        unsigned b = grid_hash(grid_cell(p[0]), grid_cell(p[1]), grid_cell(p[2]));
        s_grid_next[e] = s_grid_head[b];
        s_grid_head[b] = e;
    }
    s_grid_active = 1;
}

/* ── public API ───────────────────────────────────────────────────────────── */

void radiance_field_init(void)
{
    s_em = NULL; s_count = 0; s_cap = 0;
    s_body_lum = NULL; s_body_lum_cap = 0;
    s_since_rebuild = 0.0;
    s_last_nbodies  = -1;
    s_had_sn        = 0;
}

void radiance_field_shutdown(void)
{
    free(s_em);       s_em = NULL;
    free(s_body_lum); s_body_lum = NULL;
    free(s_grid_head); s_grid_head = NULL;
    free(s_grid_next); s_grid_next = NULL;
    free(s_area_em);   s_area_em   = NULL;
    free(s_em_stamp);  s_em_stamp  = NULL;
    free(s_sample_irr); s_sample_irr = NULL; s_sample_irr_cap = 0;
    s_grid_nbuckets = s_area_count = s_area_cap = 0;
    s_grid_active = 0; s_query_stamp = 0;
    s_count = s_cap = s_body_lum_cap = 0;
}

/* Field stars within this radius of the camera are added as emitters, so an
 * approached field star still lights the scene / shows as the dominant source in
 * deep field.  Generous (irradiance is 1/r², but out in the field the nearest
 * stars are the only light); stellar density is low, so this stays a small set. */
#define RADIANCE_FIELD_NEAR_LY 20.0
#define RADIANCE_NEAR_MAX      4096

/* Push one star (body index i) as a radiance emitter, if it is a live star with
 * positive luminosity.  Shared by the non-field and near-field passes. */
static void add_star_emitter(int i)
{
    const Body *b = &g_bodies[i];
    if (!b->alive || !b->is_star) return;   /* is_black_hole ⇒ is_star */
    double lum = b->is_black_hole ? bh_luminosity(b) : star_luminosity(b);
    if (lum <= 0.0) return;
    Emitter *e = emitters_push();
    e->body  = i;
    e->lum   = lum;
    e->pos[0] = e->pos[1] = e->pos[2] = 0.0;   /* read live */
    e->rmin2 = b->radius * b->radius;
    radiance_field_body_color(i, e->col);
    e->label  = NULL;
    e->nebula = -1;
    s_body_lum[i] = lum;
}

void radiance_field_rebuild(void)
{
    s_count = 0;
    if (s_body_lum_cap < g_nbodies) {
        s_body_lum = (double *)realloc(s_body_lum,
                                       (size_t)(g_nbodies > 0 ? g_nbodies : 1)
                                       * sizeof(double));
        s_body_lum_cap = g_nbodies > 0 ? g_nbodies : 1;
    }
    if (s_body_lum)
        memset(s_body_lum, 0, (size_t)s_body_lum_cap * sizeof(double));

    /* Emitter list = every non-field star, plus the field stars currently near
     * the camera.  The bulk Gaia field (frozen, distant) contributes negligible
     * irradiance, but including all of it would blow the emitter count to
     * hundreds of thousands and make every radiance_field_sample / _top (called
     * per body and per comet, each frame) O(that) — the dominant per-frame cost
     * at galaxy scale.  A field star you approach still becomes an emitter (and
     * can be the dominant light in deep field), refreshed on each rebuild. */
    for (int i = 0; i < g_nbodies; i++) {
        if (i >= g_field_star_begin && i < g_field_star_end) {
            i = g_field_star_end - 1;   /* O(1) skip of the whole field range */
            continue;
        }
        add_star_emitter(i);
    }
    if (g_field_star_end > g_field_star_begin) {
        double cam_m[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
        static int s_nf[RADIANCE_NEAR_MAX];
        int nn = physics_active_bodies(cam_m, RADIANCE_FIELD_NEAR_LY * LY,
                                       s_nf, RADIANCE_NEAR_MAX);
        for (int j = 0; j < nn; j++)
            if (s_nf[j] >= g_field_star_begin && s_nf[j] < g_field_star_end)
                add_star_emitter(s_nf[j]);
    }

    /* Nebulae as faint area emitters (skipped when nebulae are disabled in
     * the menu — no glow drawn, no light cast). */
    if (nebula_enabled()) {
        int nn = nebula_count();
        for (int i = 0; i < nn; i++) {
            double r_m = nebula_radius_au(i) * AU;
            if (r_m <= 0.0) continue;
            double pos_au[3];
            nebula_position(i, pos_au);
            Emitter *e = emitters_push();
            e->body   = -1;
            e->lum    = NEB_L_REF * (r_m / NEB_R_REF_M) * (r_m / NEB_R_REF_M);
            e->pos[0] = pos_au[0] * AU;
            e->pos[1] = pos_au[1] * AU;
            e->pos[2] = pos_au[2] * AU;
            e->rmin2  = r_m * r_m;
            nebula_color(i, e->col);
            float m = e->col[0] > e->col[1] ? e->col[0] : e->col[1];
            if (e->col[2] > m) m = e->col[2];
            if (m > 1e-6f) {
                e->col[0] /= m; e->col[1] /= m; e->col[2] /= m;
            } else {
                e->col[0] = e->col[1] = e->col[2] = 1.0f;
            }
            e->label  = nebula_name(i);
            e->nebula = i;
        }
    }

    /* Galaxies as integrated area emitters (Layer 4.2 hosts; skipped when
     * disabled in the menu alongside their rendering). */
    if (galaxy_enabled()) {
        int ng = galaxy_count();
        for (int i = 0; i < ng; i++) {
            double r_m = galaxy_radius_au(i) * AU;
            if (r_m <= 0.0) continue;
            double pos_au[3];
            galaxy_position(i, pos_au);
            Emitter *e = emitters_push();
            e->body   = -1;
            e->lum    = GAL_L_REF * (r_m / GAL_R_REF_M) * (r_m / GAL_R_REF_M);
            e->pos[0] = pos_au[0] * AU;
            e->pos[1] = pos_au[1] * AU;
            e->pos[2] = pos_au[2] * AU;
            e->rmin2  = r_m * r_m;
            galaxy_color(i, e->col);
            float m = e->col[0] > e->col[1] ? e->col[0] : e->col[1];
            if (e->col[2] > m) m = e->col[2];
            if (m > 1e-6f) {
                e->col[0] /= m; e->col[1] /= m; e->col[2] /= m;
            } else {
                e->col[0] = e->col[1] = e->col[2] = 1.0f;
            }
            e->label  = galaxy_name(i);
            e->nebula = -1;
        }
    }

    /* Transient supernova emitters, anchored at the detonation point.  Asking
     * with a zero camera returns world positions in AU. */
    {
        SupernovaRenderEvent ev[SUPERNOVA_MAX_EVENTS];
        double origin[3] = { 0.0, 0.0, 0.0 };
        int n = supernova_render_events(ev, SUPERNOVA_MAX_EVENTS, origin);
        s_had_sn = (n > 0);
        for (int i = 0; i < n; i++) {
            double lum = SN_L_FLASH * ev[i].flash_intensity
                       + SN_L_CORE  * ev[i].core_intensity
                       + SN_L_CLOUD * ev[i].cloud_intensity;
            if (lum <= 0.0) continue;
            Emitter *e = emitters_push();
            e->body   = -1;
            e->lum    = lum;
            e->pos[0] = (double)ev[i].pos[0] * AU;
            e->pos[1] = (double)ev[i].pos[1] * AU;
            e->pos[2] = (double)ev[i].pos[2] * AU;
            double rmin = (double)ev[i].core_radius * AU;
            if (rmin < AU) rmin = AU;
            e->rmin2  = rmin * rmin;
            /* Flash is white-hot; the tint shows as the flash decays. */
            float w = ev[i].flash_intensity;
            if (w > 1.0f) w = 1.0f;
            for (int c = 0; c < 3; c++)
                e->col[c] = ev[i].color[c] + (1.0f - ev[i].color[c]) * w;
            float m = e->col[0] > e->col[1] ? e->col[0] : e->col[1];
            if (e->col[2] > m) m = e->col[2];
            if (m > 1e-6f) {
                e->col[0] /= m; e->col[1] /= m; e->col[2] /= m;
            } else {
                e->col[0] = e->col[1] = e->col[2] = 1.0f;
            }
            e->label  = "supernova";
            e->nebula = -1;
        }
    }

    s_since_rebuild = 0.0;
    s_last_nbodies  = g_nbodies;

    radiance_field_grid_build();
}

void radiance_field_tick(double dt)
{
    s_since_rebuild += dt;
    /* While a supernova is running (or just ended), refresh every tick so the
     * flash decay lights smoothly; otherwise the coarse throttle suffices. */
    int sn_now = 0;
    {
        SupernovaRenderEvent ev[SUPERNOVA_MAX_EVENTS];
        double origin[3] = { 0.0, 0.0, 0.0 };
        sn_now = supernova_render_events(ev, SUPERNOVA_MAX_EVENTS, origin);
    }
    if (g_nbodies != s_last_nbodies || sn_now > 0 || s_had_sn ||
        s_since_rebuild >= REBUILD_PERIOD_SEC)
        radiance_field_rebuild();
}

int radiance_field_sample(const double pos_m[3], int exclude_body,
                          RadianceSample *out)
{
    memset(out, 0, sizeof(*out));
    out->dominant = -1;

    if (s_sample_irr_cap < s_count) {
        s_sample_irr = realloc(s_sample_irr, (size_t)s_count * sizeof(double));
        if (!s_sample_irr) { s_sample_irr_cap = 0; return 0; }
        s_sample_irr_cap = s_count;
    }

    double total = 0.0;
    double best  = 0.0;
    int    best_e = -1;

    for (int e = 0; e < s_count; e++) {
        double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
        s_sample_irr[e] = irr;
        if (irr <= 0.0) continue;
        total += irr;
        if (irr > best) { best = irr; best_e = e; }
    }
    if (best_e < 0) return 0;

    const Emitter *de = &s_em[best_e];
    out->irradiance = total;
    out->dom_irr    = best;
    out->dominant   = de->body;   /* -1 for a body-less emitter */
    out->dom_label  = de->body >= 0 ? g_bodies[de->body].name
                    : de->label     ? de->label : "?";

    double p[3];
    emitter_pos(de, p);
    double dx = p[0] - pos_m[0];
    double dy = p[1] - pos_m[1];
    double dz = p[2] - pos_m[2];
    double d  = sqrt(dx*dx + dy*dy + dz*dz);
    if (d > 0.0) { out->dir[0] = dx/d; out->dir[1] = dy/d; out->dir[2] = dz/d; }
    out->color[0] = de->col[0];
    out->color[1] = de->col[1];
    out->color[2] = de->col[2];

    /* Count meaningful contributors (> 0.1% of the total) from the cached
     * irradiances — no second emitter_irr pass. */
    double floor_irr = total * 1e-3;
    for (int e = 0; e < s_count; e++)
        if (s_sample_irr[e] >= floor_irr)
            out->n_sources++;
    return 1;
}

int radiance_field_dominant(const double pos_m[3], int exclude_body)
{
    double best = 0.0;
    int    best_e = -1;
    for (int e = 0; e < s_count; e++) {
        double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
        if (irr > best) { best = irr; best_e = e; }
    }
    return best_e >= 0 ? s_em[best_e].body : -1;
}

/* Insert emitter e (with precomputed irradiance irr > 0) into the descending
 * top-k list out[0..k-1], updating the running count *n.  Shared by the grid
 * and full-scan paths of radiance_field_top. */
static inline void rf_topk_insert(int e, double irr, int k,
                                  RadianceContrib *out, int *n)
{
    int i;
    if (*n < k) i = (*n)++;
    else if (irr <= out[k-1].irr) return;
    else i = k - 1;
    while (i > 0 && out[i-1].irr < irr) {
        out[i] = out[i-1];
        i--;
    }
    out[i].body   = s_em[e].body;
    out[i].nebula = s_em[e].nebula;
    out[i].irr    = irr;
    emitter_pos(&s_em[e], out[i].pos);
    out[i].col[0] = s_em[e].col[0];
    out[i].col[1] = s_em[e].col[1];
    out[i].col[2] = s_em[e].col[2];
}

int radiance_field_top(const double pos_m[3], int exclude_body,
                       int k, RadianceContrib *out)
{
    if (k <= 0) return 0;
    int n = 0;

    /* Exact full scan for small universes (grid disabled) — bit-identical. */
    if (!s_grid_active) {
        for (int e = 0; e < s_count; e++) {
            double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
            if (irr > 0.0) rf_topk_insert(e, irr, k, out, &n);
        }
        return n;
    }

    /* Body-less area emitters (nebulae/galaxies/supernovae) are few and can be
     * bright from far away — always considered. */
    for (int a = 0; a < s_area_count; a++) {
        int e = s_area_em[a];
        double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
        if (irr > 0.0) rf_topk_insert(e, irr, k, out, &n);
    }

    /* Star emitters: only the cells around the query point.  A fresh per-query
     * stamp dedups emitters reached through more than one neighbour cell that
     * hashed to the same bucket. */
    if (++s_query_stamp == 0) {   /* wraparound guard (astronomically rare) */
        memset(s_em_stamp, 0, (size_t)s_count * sizeof(unsigned));
        s_query_stamp = 1;
    }
    int cx = grid_cell(pos_m[0]), cy = grid_cell(pos_m[1]), cz = grid_cell(pos_m[2]);
    for (int dz = -GRID_RADIUS; dz <= GRID_RADIUS; dz++)
    for (int dy = -GRID_RADIUS; dy <= GRID_RADIUS; dy++)
    for (int dx = -GRID_RADIUS; dx <= GRID_RADIUS; dx++) {
        unsigned b = grid_hash(cx + dx, cy + dy, cz + dz);
        for (int e = s_grid_head[b]; e >= 0; e = s_grid_next[e]) {
            if (s_em_stamp[e] == s_query_stamp) continue;
            s_em_stamp[e] = s_query_stamp;
            double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
            if (irr > 0.0) rf_topk_insert(e, irr, k, out, &n);
        }
    }
    return n;
}

/* Chromaticity (max component 1; irradiance carries the magnitude).  Stars
 * use the physical blackbody tint of their spectral T_eff *relative to the
 * Sun's* — exactly white for a Sol twin, so Sol-lit scenes keep their
 * established art-directed look, while an M dwarf lights its planets warm
 * orange and an A star faintly blue.  Black holes emit hot disk light, not
 * their (black) body colour. */
void radiance_field_body_color(int body, float out_col[3])
{
    out_col[0] = out_col[1] = out_col[2] = 1.0f;
    if (body < 0 || body >= g_nbodies) return;
    const Body *b = &g_bodies[body];
    if (b->is_black_hole) {
        out_col[0] = 1.0f; out_col[1] = 0.93f; out_col[2] = 0.82f;
        return;
    }
    double t = spectral_t_eff(b);
    if (t > 0.0) {
        spectral_light_tint(t, out_col);
        return;
    }
    /* Non-emitter fallback: display colour, normalised. */
    float c[3] = { b->col[0], b->col[1], b->col[2] };
    float m = c[0] > c[1] ? c[0] : c[1];
    if (c[2] > m) m = c[2];
    if (m < 1e-6f) return;
    out_col[0] = c[0] / m;
    out_col[1] = c[1] / m;
    out_col[2] = c[2] / m;
}

double radiance_field_body_luminosity(int body)
{
    if (body < 0 || body >= s_body_lum_cap || !s_body_lum) return 0.0;
    return s_body_lum[body];
}

int radiance_field_sample_camera(RadianceSample *out)
{
    double pos_m[3] = { (double)g_cam.pos[0] * AU,
                        (double)g_cam.pos[1] * AU,
                        (double)g_cam.pos[2] * AU };
    return radiance_field_sample(pos_m, -1, out);
}
