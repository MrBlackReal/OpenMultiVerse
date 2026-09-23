/*
 * body.h — Body data structure and orbital mechanics
 */
#pragma once
#include <stdint.h>
#include "common.h"

/* Stellar lifecycle phase. Drives radius/colour and death events.
 * MAIN_SEQUENCE is 0 so zero-initialised stars start on the main sequence. */
typedef enum {
    STAR_MAIN_SEQUENCE = 0,
    STAR_SUBGIANT,
    STAR_RED_GIANT,
    STAR_PLANETARY_NEBULA, /* transient low-mass death puff */
    STAR_WHITE_DWARF,
    STAR_NEUTRON_STAR,
    STAR_BLACK_HOLE_REMNANT,
    STAR_DEAD             /* source star retired into a supernova remnant body */
} StarPhase;

/* Black-hole, AGN and accretion state. A few dozen bodies in a universe of
 * hundreds of thousands carry any of it, so it lives off the Body and is
 * allocated on first write. Any body may own one — the ring-like elements are
 * deliberately decoupled from is_black_hole — and a body without one reads as
 * all zeros, exactly what the inline fields held before. */
typedef struct BlackHole {
    float  agn_activity;   /* 0 = quiet hole; >0 = active quasar/AGN: scales     */
                           /* disk brightness + drives the relativistic jets     */
    float  accretion_disk; /* accretion-disk strength (0 = none). A ring-like    */
                           /* element decoupled from is_black_hole so a hole can  */
                           /* be bare or dressed; default on for BH/quasar types  */
    float  dust_torus;     /* obscuring dust-torus strength (0 = none): the outer */
                           /* AGN doughnut that hides the core when edge-on       */
    float  agn_visual_scale;/* artistic jet-size multiplier (0/1 = physical Rs).  */
                           /* >1 stretches the relativistic jet to galaxy scale   */
                           /* (a kpc beam) while the disk/torus stay Rs-sized —    */
                           /* used by galaxy-hosted nuclei. Disk visuals unchanged */
    float  agn_axis[3];    /* explicit 3-D jet/spin axis (unit; 0,0,0 = derive    */
                           /* from obliquity). Aligns a hosted nucleus' jet with   */
                           /* its galaxy's disc axis.                             */

    /* Accretion state (evolved by accretion.c on the stellar clock).
     * agn_activity above is the Eddington ratio, now an OUTPUT of this model
     * rather than a fixed authored tag. See accretion.h. */
    double gas_reservoir;   /* kg of fuel left to accrete (0 = starved)           */
    double mdot;            /* current accretion rate, kg/s                       */
    double eddington_ratio; /* L / L_edd (mirrors agn_activity, unclamped)        */
    double spin_a;          /* dimensionless Kerr spin a* (signed: +prograde),    */
                            /* seeded from rotation_rate, spun up by accretion;   */
                            /* source of truth for the ISCO in render.c           */
} BlackHole;

/* Per-body orbital trail, allocated only while the body is trail-resident.
 *
 * Two parallel states live side-by-side:
 *   live ("head/count/..."): the circular sample buffer in AU plus per-segment
 *     arc lengths, drawn by trails_render().
 *   frame ("frame_*"): the live state as it was at the start of the current
 *     frame, so trails_cut_body_at_time() can roll a trail back to the exact
 *     impact point when a collision lands mid-frame. */
typedef struct Trail {
    double accum;            /* meters accumulated toward next sample      */
    int    head;             /* index of next write slot                   */
    int    count;            /* number of valid samples (0..TRAIL_LEN)     */
    double total_len;        /* retained trail length in world meters      */
    double fade;             /* 1.0 = full alpha; fades to 0 after death   */
    int    emitting;         /* 1 while the body should keep adding points */
    double prev_pos[3];      /* previous trail tick position for interpolation */
    double prev_vel[3];      /* previous trail tick velocity for curve reconstruction */
    double frame_accum;      /* meters accumulated at frame start        */
    int    frame_head;       /* trail head snapshot at frame start       */
    int    frame_count;      /* trail count snapshot at frame start      */
    double frame_total_len;  /* retained trail length at frame start     */
    double frame_pos[3];     /* body position at frame start             */
    double frame_vel[3];     /* body velocity at frame start             */
    double frame_prev_pos[3];/* previous curve anchor at frame start    */
    double frame_prev_vel[3];/* previous curve velocity at frame start  */
    double pts[TRAIL_LEN][3];    /* samples, positions in AU              */
    double seg_len[TRAIL_LEN];   /* segment length ending at each sample  */
} Trail;

/* Allocate an empty trail anchored at (pos, vel): emitting, full fade, and
 * prev/frame curve anchors seeded so the first segment has a valid tangent.
 * Returns NULL on out-of-memory. Release with free(). */
Trail *trail_new(const double pos[3], const double vel[3]);

/* A body in the active simulation. Field order is deliberate: the gravity
 * inner loop (physics.c compute_acc_system) touches only the first 64-byte
 * cache line, and the integrator only the first two, so they stream through
 * a large active set without dragging names, colours and lifecycle state
 * through the cache. Keep hot fields at the top; body.c asserts the split.
 * Rarely-populated state (trail, black hole) lives behind pointers. */
typedef struct {
    /* ── cache line 0: pairwise gravity ─────────────────────────────── */
    double pos[3];         /* m, simulation frame             */
    double mass;           /* kg                              */
    double acc[3];         /* m/s^2 (recomputed each step)    */
    int    alive;          /* 0 = removed/absorbed; index kept stable */
    int    parent;         /* index of parent body (-1 = none)                  */
                           /* stars: -1; planets: star idx; moons: planet idx   */

    /* ── cache line 1: integration, collisions, classification ──────── */
    double vel[3];         /* m/s                             */
    double fast_acc[3];    /* m/s^2 dominant parent force, RESPA inner step */
    double radius;         /* m (physical)                    */
    uint8_t is_star;
    uint8_t is_comet;      /* 1 = comet nucleus: comet.c draws coma + ion/dust   */
                           /* tails, activity driven by RadianceField flux       */
    uint8_t is_black_hole; /* 1 = render as accretion disk + shadow (no glare/   */
                           /* sphere). Also is_star=1 so it acts as a system root */
    /* Catalogue absolute magnitude (StarBin v2 field stars): drives the
     * star's drawn brightness when set. Zero-initialised bodies have none,
     * hence the flag rather than a sentinel value. */
    uint8_t has_abs_mag;
    /* Slot generation, bumped every time universe_add_body() reuses this slot
     * for a different body. An array index alone cannot tell "the body I meant"
     * from "whatever moved into its slot after it was absorbed"; index plus
     * generation can, in O(1). See BodyHandle below. */
    uint32_t generation;

    /* ── warm: timestep model, rotation, tidal state ────────────────── */
    double dyn_period;     /* s, estimated local orbital/dynamical period       */
    double dyn_dt_outer;   /* s, recommended slow-force timestep ceiling         */
    double dyn_dt_inner;   /* s, recommended parent-force timestep ceiling       */
    int    dyn_bucket;     /* 0=slow .. 3=very fast                             */

    /* Tidal disruption (set by collision.c when a black hole shreds this body).
     * tidal_frac ramps 0→1 as the body is devoured; render.c stretches the body
     * toward g_bodies[tidal_hole] and shrinks it. Only read while tidal_frac>0. */
    float  tidal_frac;
    int    tidal_hole;

    /* Rotation */
    double obliquity;       /* axial tilt in degrees (from ecliptic north)  */
    double rotation_rate;   /* rad/s (positive = prograde)                  */
    double rotation_angle;  /* current rotation phase, rad (0..2π)          */
    double cloud_rotation;  /* continuous cloud angle, rad (never wrapped)  */

    /* Stellar lifecycle (stars only; lazily initialised by lifecycle.c).
     * base_* capture the main-sequence appearance so phases scale/tint off it
     * and so a phase change is reversible. base_radius<=0 means "not captured
     * yet" — lifecycle_ensure_base() fills it from the current radius/colour. */
    int    star_phase;       /* StarPhase                                    */
    float  base_col[3];      /* main-sequence display colour                 */
    double age_yr;           /* accumulated stellar age in years             */
    double ms_lifetime_yr;   /* main-sequence lifetime from mass (0=uncomputed)*/
    double base_radius;      /* main-sequence radius in m (0=not captured)   */

    /* ── cold: presentation ─────────────────────────────────────────── */
    char   name[32];
    float  col[3];         /* RGB display colour              */
    float  abs_mag;        /* valid only when has_abs_mag     */

    /* Atmosphere (set by universe loader; zero = no atmosphere) */
    float  atm_color[3];    /* RGB atmosphere rim colour                    */
    float  atm_intensity;   /* peak glow strength (0 = no atmosphere)       */
    float  atm_scale;       /* outer atm radius as multiple of planet radius */

    /* ── out of line: allocated only for the bodies that need them ──── */
    /* Black-hole / AGN / accretion state. NULL for the vast majority of
     * bodies; read through body_bh() (all-zero when absent), write through
     * body_bh_mut() (allocates on first write). See BlackHole above. */
    struct BlackHole *bh;
    /* Orbital trail: NULL unless the body is inside the simulated region
     * (trails.c trail_residency). All per-trail state lives behind this
     * pointer so the ~250 bytes of it are paid only by the few bodies that
     * are actually drawing a path, not by every body in the universe. */
    struct Trail *trail;
} Body;

/* A reference that survives slot reuse.
 *
 * The codebase currently guards stale indices in four ad-hoc ways -- comparing
 * names before demoting a promoted system (starsys.c), dropping orphaned rings
 * on reuse (rings.h), snapshotting participant names into field-graph events
 * (field_graph.h), and repairing parent/tidal_hole links on absorption
 * (collision.c). Each solves the same problem locally and none composes. A
 * handle makes the check uniform and cheap, so new call sites get it for free
 * rather than inventing a fifth guard. */
typedef struct {
    int      index;       /* g_bodies slot, or -1 for "none"   */
    uint32_t generation;  /* Body.generation when taken        */
} BodyHandle;

/* Take a handle to body `i` (pass -1 for a null handle). */
BodyHandle body_handle(int i);

/* Resolve a handle to a live slot index, or -1 if the body it referred to is
 * gone or its slot has since been reused by a different body. */
int body_handle_resolve(BodyHandle h);

/* Black-hole state of `b` for reading: its own, or a shared all-zero one. */
extern const BlackHole g_bh_none;
static inline const BlackHole *body_bh(const Body *b)
{
    return b->bh ? b->bh : &g_bh_none;
}

/* Black-hole state of `b` for writing, allocated zeroed on first use.
 * Exits on out-of-memory, like the body array itself. */
BlackHole *body_bh_mut(Body *b);

/* Free a body's out-of-line state (trail, black hole). For a slot about to be
 * reused or discarded; call before zeroing it. */
void body_release(Body *b);

/* Set the authored AGN dressing (activity, disk, torus). Allocates the
 * BlackHole only when one of them is non-zero, so bulk loaders can call it
 * for every body without paying for the planets and plain stars. */
void body_set_agn(Body *b, float activity, float disk, float torus);

/* g_bodies is a heap-allocated array that grows via realloc.
 * g_nbodies is the high-water slot count, not the number of alive bodies.
 * Absorbed slots stay addressable for stable indices and may be reused by
 * universe_add_body() once their Body.alive flag is clear.
 * g_bodies_cap is the current allocated capacity.
 * MAX_BODIES (common.h) sizes some fixed per-frame caches (labels.c slots),
 * but it does NOT bound body indices: universe_add_body() grows g_bodies past
 * it, and starsys promotion routinely lands bodies in the hundreds of
 * thousands. Code indexing a MAX_BODIES array by body index must range-check
 * rather than assume. */
extern Body *g_bodies;
extern int   g_nbodies;
extern int   g_bodies_cap;

/* State from Keplerian elements around a star of given GM (angles in degrees,
 * a in AU, gm_au_day2 in AU³/day² — use GM_SUN for Sol planets). */
void keplerian_to_state(
        double a, double e, double i_deg,
        double Omega_deg, double omega_tilde_deg, double L_deg,
        double gm_au_day2,
        double pos_m[3], double vel_ms[3]);

/* Camera-proximity cache: nearest star / nearest body to the camera, in AU.
 * Refreshed by ONE shared O(N) pass per frame (body_update_cam_proximity,
 * called from the main loop) — consumers (trail fade, HUD readout, adaptive
 * warp) read this instead of each running their own full-body scan.
 * Indices are -1 when the universe holds no matching living body. */
typedef struct {
    int    star;          /* nearest living star */
    double star_dist_au;
    int    body;          /* nearest living body of any kind */
    double body_dist_au;
} CamProximity;
extern CamProximity g_cam_prox;
void body_update_cam_proximity(void);

/* Index of the star body nearest to the camera, from g_cam_prox. */
int nearest_star_idx(void);

/* Find a living, non-field body by name (case-insensitive); -1 if there is no
 * match. The bulk field-star range is skipped in O(1): those are frozen
 * scenery and are never a camera subject, and scanning ~260k of them per frame
 * would be a top per-frame cost.
 *
 * Callers should resolve names per frame rather than caching the index — a
 * body can be absorbed mid-shot, and g_nbodies' dead slots get reused, so a
 * stale index can silently come back pointing at a different body. */
int body_find_named(const char *name);

/* Walk parent links to find the owning root star for a body. */
int body_root_star(int i);

/* Convert a world-space direction into the body's surface-local frame.
 * This matches the local-space convention used by the planet shader. */
void body_world_to_local_surface_dir(int body_idx, const double world_dir[3],
                                     float out[3]);

/* Two-body propagation: advance the relative state (r0, v0) of a body about
 * its primary by dt seconds, mu = G * (M_primary + m_body). Universal-variable
 * Kepler (Vallado, alg. 8), so ellipses, parabolas and hyperbolas all work;
 * elliptic spans are first reduced modulo the period, so a billion years
 * costs what one orbit costs. Returns 0 if the solver failed to converge
 * (then r, v are a straight-line drift). */
int kepler_propagate(const double r0[3], const double v0[3], double mu, double dt,
                     double r[3], double v[3]);

/* Planetocentric state from simple moon elements (a in km, angles in degrees). */
void moon_to_state(
        double a_km, double e, double i_deg,
        double Omega_deg, double omega_deg, double M0_deg,
        double gm,
        double pos_m[3], double vel_ms[3]);
