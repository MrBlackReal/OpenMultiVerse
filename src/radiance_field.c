/*
 * radiance_field.c — unified radiance transport field (roadmap Phase A #4).
 *
 * See radiance_field.h for the design contract.  Everything here is a plain
 * emitter list + closed-form luminosity models; no GL, no SDL.
 *
 * ── Luminosity models ─────────────────────────────────────────────────────
 *
 * Thermal stars:  L = L☉ · (R/R☉)² · h⁴, the Stefan-Boltzmann law with the
 * effective-temperature ratio h = T/T☉ estimated from the star's display
 * colour.  The codebase's existing temperature proxy is the blue−red channel
 * balance (star_glare corona weighting, catalog colour conversion), so h is
 * calibrated against Sol's authored colour: a star coloured like the Sun gets
 * exactly h = 1, bluer runs hotter, redder cooler.  This is a v1 estimate —
 * when spectral classification lands (roadmap §1.1) it becomes the source of
 * truth for T and this refines for free.
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
#include "camera.h"
#include "supernova.h"

#include <stdlib.h>
#include <string.h>

/* Physical constants (SI). */
#define L_SUN      3.828e26        /* solar luminosity, W                    */
#define R_SUN      6.957e8         /* solar radius, m                        */
#define M_SUN      1.989e30        /* solar mass, kg                         */
#define C_LIGHT    2.99792458e8    /* speed of light, m/s                    */
#define EDD_PER_MSUN 1.26e31       /* Eddington luminosity per solar mass, W */
#define ACC_EFF    0.1             /* radiative efficiency η (accretion.c)   */

/* Colour→temperature-ratio calibration: Sol's authored colour (1, 0.92, 0.23)
 * has blue−red = −0.77 and must map to h = T/T☉ = 1.  Slope chosen so a
 * blue-white star (b−r ≈ +0.3) runs h ≈ 2.3 (~13000 K, early A/B) and a deep
 * red dwarf (b−r ≈ −1.0) h ≈ 0.7 (~4000 K).  Clamped to keep L finite for
 * arbitrary hand-authored colours. */
#define SUN_BLUE_RED  (-0.77)
#define HEAT_SLOPE    1.2
#define HEAT_MIN      0.45
#define HEAT_MAX      3.0

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

/* ── emitter list ─────────────────────────────────────────────────────────
 * Membership + luminosity are cached at rebuild.  Body emitters read their
 * position live from g_bodies at query time (orbits move between rebuilds);
 * transient (supernova) emitters use the snapshot pos — the blast is anchored. */
typedef struct {
    int    body;      /* g_bodies index, or -1 = transient (supernova)      */
    double lum;       /* luminosity, W                                      */
    double pos[3];    /* SI m; used when body < 0 (bodies are read live)    */
    double rmin2;     /* min distance² clamp (photosphere / core), m²       */
    float  col[3];    /* chromaticity (max component 1)                     */
} Emitter;

static Emitter *s_em = NULL;
static int      s_count = 0;
static int      s_cap   = 0;

/* Per-body luminosity lookup (0 for non-emitters), sized to g_nbodies. */
static double *s_body_lum = NULL;
static int     s_body_lum_cap = 0;

/* Rebuild throttle. */
static double s_since_rebuild = 0.0;
static int    s_last_nbodies  = -1;
static int    s_had_sn        = 0;   /* last rebuild harvested a supernova */
#define REBUILD_PERIOD_SEC 0.5

/* Thermal (photosphere) luminosity of a star-flagged body, W. */
static double star_luminosity(const Body *b)
{
    double rr = b->radius / R_SUN;
    double h  = 1.0 + HEAT_SLOPE * ((double)(b->col[2] - b->col[0]) - SUN_BLUE_RED);
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
    s_count = s_cap = s_body_lum_cap = 0;
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

    for (int i = 0; i < g_nbodies; i++) {
        const Body *b = &g_bodies[i];
        if (!b->alive || !b->is_star) continue;   /* is_black_hole ⇒ is_star */
        double lum = b->is_black_hole ? bh_luminosity(b) : star_luminosity(b);
        if (lum <= 0.0) continue;
        Emitter *e = emitters_push();
        e->body  = i;
        e->lum   = lum;
        e->pos[0] = e->pos[1] = e->pos[2] = 0.0;   /* read live */
        e->rmin2 = b->radius * b->radius;
        radiance_field_body_color(i, e->col);
        s_body_lum[i] = lum;
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
        }
    }

    s_since_rebuild = 0.0;
    s_last_nbodies  = g_nbodies;
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

    double total = 0.0;
    double best  = 0.0;
    int    best_e = -1;

    for (int e = 0; e < s_count; e++) {
        double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
        if (irr <= 0.0) continue;
        total += irr;
        if (irr > best) { best = irr; best_e = e; }
    }
    if (best_e < 0) return 0;

    const Emitter *de = &s_em[best_e];
    out->irradiance = total;
    out->dom_irr    = best;
    out->dominant   = de->body;   /* -1 for a transient (supernova) */

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

    /* Count meaningful contributors (> 0.1% of the total). */
    double floor_irr = total * 1e-3;
    for (int e = 0; e < s_count; e++)
        if (emitter_irr(&s_em[e], pos_m, exclude_body) >= floor_irr)
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

int radiance_field_top(const double pos_m[3], int exclude_body,
                       int k, RadianceContrib *out)
{
    if (k <= 0) return 0;
    int n = 0;
    for (int e = 0; e < s_count; e++) {
        double irr = emitter_irr(&s_em[e], pos_m, exclude_body);
        if (irr <= 0.0) continue;
        int i;
        if (n < k) i = n++;
        else if (irr <= out[k-1].irr) continue;
        else i = k - 1;
        while (i > 0 && out[i-1].irr < irr) {
            out[i] = out[i-1];
            i--;
        }
        out[i].body = s_em[e].body;
        out[i].irr  = irr;
        emitter_pos(&s_em[e], out[i].pos);
        out[i].col[0] = s_em[e].col[0];
        out[i].col[1] = s_em[e].col[1];
        out[i].col[2] = s_em[e].col[2];
    }
    return n;
}

/* Chromaticity: display colour normalised so the max component is 1 (colour
 * only — irradiance carries the magnitude).  Black holes emit hot disk light,
 * not their (black) body colour. */
void radiance_field_body_color(int body, float out_col[3])
{
    out_col[0] = out_col[1] = out_col[2] = 1.0f;
    if (body < 0 || body >= g_nbodies) return;
    const Body *b = &g_bodies[body];
    float c[3];
    if (b->is_black_hole) { c[0] = 1.0f; c[1] = 0.93f; c[2] = 0.82f; }
    else { c[0] = b->col[0]; c[1] = b->col[1]; c[2] = b->col[2]; }
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
