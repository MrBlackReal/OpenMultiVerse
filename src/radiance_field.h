/*
 * radiance_field.h — unified radiance transport field over all light emitters.
 *
 * This is the roadmap Phase A #4 foundation (§0.3 RadianceField): one queryable
 * representation of emitted light, so lighting stops being a per-shader special
 * case ("light = walk to the root star") and becomes a field question: *what
 * light arrives at this point, and from where?*  Every emitter — thermal stars
 * (main sequence through white dwarf, via Stefan-Boltzmann) and accretion-
 * powered black holes / AGN (via the accretion.c Ṁ or the authored Eddington
 * ratio) — reduces to the same record: a body, a luminosity in watts, and a
 * chromaticity.  One query then answers total irradiance and the dominant
 * source at any position, in physical units (W/m²; the Sun at 1 AU ≈ 1361).
 *
 * First consumers: render.c body/atmosphere lighting (the dominant emitter
 * replaces the root-star parent walk — so a body deep in a binary or orbiting
 * an active black hole is lit by what actually outshines the sky there), the
 * HUD readout, and a headless [RadianceField] print.  Later consumers per the
 * roadmap: nebular illumination, galaxy light distribution, relativistic
 * shifts.
 *
 * Implementation: a compact emitter list harvested from g_bodies, O(N) rebuild
 * on universe load and throttled per frame (luminosities only drift on the
 * stellar clock).  Queries are O(emitters) in doubles; emitter positions are
 * always read live from g_bodies so orbiting sources light correctly between
 * rebuilds.
 *
 * Threading: build and query are MAIN-THREAD ONLY (same contract as
 * cosmic_field.h) — never call from inside the physics OpenMP warmup.
 */
#pragma once
#include "common.h"

/* Result of a radiance sample at a point.
 * `dominant` is a body index — or -1 when the dominant emitter is a transient
 * source with no body (an active supernova); dir/color are still valid then. */
typedef struct {
    double irradiance;   /* total incident flux from all emitters, W/m²      */
    double dom_irr;      /* flux from the dominant (brightest-here) emitter  */
    int    dominant;     /* dominant emitter body index (-1 = supernova)     */
    double dir[3];       /* unit vector from the point toward the dominant   */
    float  color[3];     /* dominant emitter chromaticity (max component 1)  */
    int    n_sources;    /* emitters contributing > 0.1% of the total        */
} RadianceSample;

/* One-time init (zeroes state); call once at boot before the first rebuild. */
void radiance_field_init(void);
void radiance_field_shutdown(void);

/* (Re)harvest emitters + luminosities from the current g_bodies snapshot.
 * O(N).  Call after a universe load/reset and whenever the body set changed. */
void radiance_field_rebuild(void);

/* Per-frame maintenance (real frame dt, seconds): rebuilds immediately when
 * the body high-water count changes, else on a time throttle — luminosities
 * evolve only on the stellar clock (lifecycle/accretion), so a coarse refresh
 * is exact enough.  Cheap when it does nothing. */
void radiance_field_tick(double dt);

/* Core query.  pos_m = world position (SI metres).  exclude_body suppresses
 * one body's own emission (pass the body being lit, or -1).  Zeroes *out,
 * then fills it.  Returns 1 if any emitter contributed, else 0. */
int  radiance_field_sample(const double pos_m[3], int exclude_body,
                           RadianceSample *out);

/* Convenience: body index of the dominant emitter at pos_m (-1 if none). */
int  radiance_field_dominant(const double pos_m[3], int exclude_body);

/* One contributing emitter at a sampled point.  Carries its own position and
 * chromaticity so consumers need no body lookup — and so transient emitters
 * (supernovae, body = -1) work identically. */
typedef struct {
    int    body;     /* g_bodies index, or -1 for a supernova         */
    double irr;      /* incident flux from it, W/m²                   */
    double pos[3];   /* emitter position, SI m                        */
    float  col[3];   /* chromaticity (max component 1)                */
} RadianceContrib;

/* The k emitters with the highest incident flux at pos_m, brightest first.
 * Returns the count written (≤ k).  This is what multi-light shading reads:
 * out[0] is the primary sun, out[1] the strongest secondary. */
int  radiance_field_top(const double pos_m[3], int exclude_body,
                        int k, RadianceContrib *out);

/* Emitter chromaticity (max component 1): the display colour for stars, hot
 * disk light for black holes.  White for non-emitters. */
void radiance_field_body_color(int body, float out_col[3]);

/* Cached luminosity of a body in watts (0 if it is not an emitter). */
double radiance_field_body_luminosity(int body);

/* Convenience: sample at the current camera position. */
int  radiance_field_sample_camera(RadianceSample *out);
