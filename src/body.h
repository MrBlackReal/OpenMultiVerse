/*
 * body.h — Body data structure and orbital mechanics
 */
#pragma once
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

typedef struct {
    char   name[32];
    double mass;           /* kg                              */
    double radius;         /* m (physical)                    */
    double pos[3];         /* m, simulation frame             */
    double vel[3];         /* m/s                             */
    double acc[3];         /* m/s^2 (recomputed each step)    */
    double fast_acc[3];    /* m/s^2 dominant parent force, RESPA inner step */
    float  col[3];         /* RGB display colour              */
    int    is_star;
    int    is_black_hole;  /* 1 = render as accretion disk + shadow (no glare/   */
                           /* sphere). Also is_star=1 so it acts as a system root */
    float  agn_activity;   /* 0 = quiet hole; >0 = active quasar/AGN: scales     */
                           /* disk brightness + drives the relativistic jets     */
    float  accretion_disk; /* accretion-disk strength (0 = none). A ring-like    */
                           /* element decoupled from is_black_hole so a hole can  */
                           /* be bare or dressed; default on for BH/quasar types  */
    float  dust_torus;     /* obscuring dust-torus strength (0 = none): the outer */
                           /* AGN doughnut that hides the core when edge-on       */

    /* Accretion state (black holes; evolved by accretion.c on the stellar
     * clock). agn_activity above is the Eddington ratio, now an OUTPUT of this
     * model rather than a fixed authored tag. See accretion.h. */
    double gas_reservoir;   /* kg of fuel left to accrete (0 = starved)           */
    double mdot;            /* current accretion rate, kg/s                       */
    double eddington_ratio; /* L / L_edd (mirrors agn_activity, unclamped)        */
    double spin_a;          /* dimensionless Kerr spin a* (signed: +prograde),    */
                            /* seeded from rotation_rate, spun up by accretion;   */
                            /* source of truth for the ISCO in render.c           */
    int    alive;           /* 0 = removed/absorbed; index kept stable */
    int    parent;         /* index of parent body (-1 = none)                  */
                           /* stars: -1; planets: star idx; moons: planet idx   */
    double dyn_period;     /* s, estimated local orbital/dynamical period       */
    double dyn_dt_outer;   /* s, recommended slow-force timestep ceiling         */
    double dyn_dt_inner;   /* s, recommended parent-force timestep ceiling       */
    int    dyn_bucket;     /* 0=slow .. 3=very fast                             */

    /* Stellar lifecycle (stars only; lazily initialised by lifecycle.c).
     * base_* capture the main-sequence appearance so phases scale/tint off it
     * and so a phase change is reversible. base_radius<=0 means "not captured
     * yet" — lifecycle_ensure_base() fills it from the current radius/colour. */
    int    star_phase;       /* StarPhase                                    */
    double age_yr;           /* accumulated stellar age in years             */
    double ms_lifetime_yr;   /* main-sequence lifetime from mass (0=uncomputed)*/
    double base_radius;      /* main-sequence radius in m (0=not captured)   */
    float  base_col[3];      /* main-sequence display colour                 */

    /* Rotation */
    double obliquity;       /* axial tilt in degrees (from ecliptic north)  */
    double rotation_rate;   /* rad/s (positive = prograde)                  */
    double rotation_angle;  /* current rotation phase, rad (0..2π)          */
    double cloud_rotation;  /* continuous cloud angle, rad (never wrapped)  */

    /* Tidal disruption (set by collision.c when a black hole shreds this body).
     * tidal_frac ramps 0→1 as the body is devoured; render.c stretches the body
     * toward g_bodies[tidal_hole] and shrinks it. Only read while tidal_frac>0. */
    float  tidal_frac;
    int    tidal_hole;

    /* Atmosphere (set by universe loader; zero = no atmosphere) */
    float  atm_color[3];    /* RGB atmosphere rim colour                    */
    float  atm_intensity;   /* peak glow strength (0 = no atmosphere)       */
    float  atm_scale;       /* outer atm radius as multiple of planet radius */

    /* Orbital trail (circular buffer, TRAIL_LEN samples, positions in AU) */
    double trail_accum;      /* meters accumulated toward next sample      */
    int    trail_head;       /* index of next write slot                   */
    int    trail_count;      /* number of valid samples (0..TRAIL_LEN)     */
    double (*trail)[3];      /* heap-allocated [TRAIL_LEN][3]              */
    double *trail_seg_len;   /* segment length ending at each sample index */
    double trail_total_len;  /* retained trail length in world meters      */
    double trail_fade;       /* 1.0 = full alpha; fades to 0 after death   */
    int    trail_emitting;   /* 1 while the body should keep adding points */
    double trail_prev_pos[3];/* previous trail tick position for interpolation */
    double trail_prev_vel[3];/* previous trail tick velocity for curve reconstruction */
    double trail_frame_accum;     /* meters accumulated at frame start        */
    int    trail_frame_head;      /* trail head snapshot at frame start       */
    int    trail_frame_count;     /* trail count snapshot at frame start      */
    double trail_frame_total_len; /* retained trail length at frame start     */
    double trail_frame_pos[3];    /* body position at frame start             */
    double trail_frame_vel[3];    /* body velocity at frame start             */
    double trail_frame_prev_pos[3];/* previous curve anchor at frame start    */
    double trail_frame_prev_vel[3];/* previous curve velocity at frame start  */
} Body;

/* g_bodies is a heap-allocated array that grows via realloc.
 * g_nbodies is the high-water slot count, not the number of alive bodies.
 * Absorbed slots stay addressable for stable indices and may be reused by
 * universe_add_body() once their Body.alive flag is clear.
 * g_bodies_cap is the current allocated capacity.
 * MAX_BODIES (common.h) is also the compile-time bound for per-frame arrays
 * in render/labels/physics/collision, so runtime-added body indices stay
 * below that value. */
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

/* Index of the star body nearest to the camera (camera.h must be included first). */
int nearest_star_idx(void);

/* Walk parent links to find the owning root star for a body. */
int body_root_star(int i);

/* Convert a world-space direction into the body's surface-local frame.
 * This matches the local-space convention used by the planet shader. */
void body_world_to_local_surface_dir(int body_idx, const double world_dir[3],
                                     float out[3]);

/* Planetocentric state from simple moon elements (a in km, angles in degrees). */
void moon_to_state(
        double a_km, double e, double i_deg,
        double Omega_deg, double omega_deg, double M0_deg,
        double gm,
        double pos_m[3], double vel_ms[3]);
