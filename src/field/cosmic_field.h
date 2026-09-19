/*
 * cosmic_field.h — unified density/variance field over discrete bodies and
 * continuous nebulae.
 *
 * This is the roadmap Phase A #3 foundation: one queryable spatial field that
 * classifies universe content as discrete / continuous / hybrid and reports
 * local density, mass density, nebular medium fill, and a clumpiness (spatial
 * variance) metric at any point.  It exists to feed the continuous-LOD selection
 * (Phase A #2, not built here) with the "local density and field variance" the
 * roadmap calls for; the LOD consumer is a separate follow-up.
 *
 * Implementation: a uniform spatial hash over g_bodies (SI metres, double),
 * rebuilt O(N) on universe load and throttled per frame.  Nebulae contribute a
 * continuous medium term so the abstraction genuinely unifies discrete points
 * with volumetric fields.
 *
 * Threading: build and query are MAIN-THREAD ONLY.  Never call from inside the
 * physics OpenMP warmup region — the field is queried at render/HUD time only.
 */
#pragma once
#include "common.h"

/* Dominant content class at a sampled point.  Derived at query time from the
 * sample (body count vs nebular fill) — there is no per-body tagging. */
typedef enum {
    COSMIC_DISCRETE = 0,  /* resolvable bodies dominate (stars/planets)     */
    COSMIC_CONTINUOUS,    /* volumetric medium dominates (nebula / vacuum)  */
    COSMIC_HYBRID         /* discrete + continuous both significant         */
} CosmicClass;

/* Result of a field sample.  All densities are per cubic light-year so the
 * numbers stay human-readable across galaxy scales. */
typedef struct {
    double number_density;  /* alive bodies per cubic light-year            */
    double mass_density;    /* kg per cubic light-year                      */
    double continuous_fill; /* 0..1 peak nebular medium coverage at point   */
    double clumpiness;      /* 0 = uniform scatter .. 1 = tight cluster     */
    int    body_count;      /* alive bodies inside the sample sphere        */
    int    nebulae_hit;     /* nebulae whose volume contains the point      */
    CosmicClass dominant;
} CosmicSample;

/* One-time init (zeroes state); call once at boot before the first rebuild. */
void cosmic_field_init(void);
void cosmic_field_shutdown(void);

/* (Re)build the spatial hash from the current g_bodies snapshot.  O(N).
 * Call after a universe load/reset and whenever the body set changed. */
void cosmic_field_rebuild(void);

/* Per-frame maintenance (call once per frame with the real frame dt in seconds).
 * Rebuilds immediately if the body high-water count changed, else on a time
 * throttle; cheap otherwise. */
void cosmic_field_tick(double dt);

/* Core query.  pos_m = world position (SI metres); radius_m = sample radius (m).
 * Zeroes *out first, then fills it.  Returns 1 if the field had anything to
 * sample, 0 if the field is empty/unbuilt or radius <= 0. */
int  cosmic_field_sample(const double pos_m[3], double radius_m, CosmicSample *out);

/* Convenience: sample at the current camera with the default HUD radius. */
int  cosmic_field_sample_camera(CosmicSample *out);

/* Human-readable class name ("DISCRETE" / "CONTINUOUS" / "HYBRID"). */
const char *cosmic_field_class_name(CosmicClass c);

/* ── cluster / hybrid aggregation (Phase A #2 consumer) ─────────────────────
 * A dense clump of stars extracted from the frozen field-star partition so it
 * can be drawn as ONE aggregate impostor glow when far (its members would be
 * sub-pixel and merge into a single blob anyway) instead of N far dots.  The
 * renderer crossfades the impostor OUT as the clump resolves into individual
 * stars on approach — the last "cluster/hybrid aggregation" LOD handoff.
 *
 * Extracted once per universe load (field stars never move), cached; zero
 * recurring per-frame cost — the consumer only projects the cached list. */
typedef struct {
    double pos_m[3];   /* member centroid, SI metres                          */
    double radius_m;   /* RMS spatial extent of members, metres               */
    int    count;      /* member star count                                   */
    float  color[3];   /* mean member display colour                          */
} CosmicCluster;

/* Access the cached cluster list.  Returns the count; *out points at an
 * internal array valid until the next universe (re)load.  Main-thread only. */
int cosmic_field_clusters(const CosmicCluster **out);
