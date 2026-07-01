/*
 * laws.h — per-universe physical laws
 *
 * The simulator historically baked its physical constants in as compile-time
 * #defines (G_CONST, SOFTENING, ...).  To support a *multiverse* — several
 * universes each with their own rules — those constants are promoted to a
 * single mutable global, g_laws, that is reset to Newtonian defaults and then
 * overridden per universe from the JSON "laws" block.
 *
 * Phase 1 threads G and softening through every force/orbit computation.
 * The remaining fields (force_exp, lambda, pn_factor, c_light, time_scale)
 * are scaffolded here with inert defaults and become active in later phases;
 * keeping them in the struct now means the JSON schema and save/load paths
 * are stable from the start.
 */
#pragma once

#include <math.h>

/* Newtonian defaults — these reproduce the original hard-coded behaviour. */
#define LAWS_DEFAULT_G          6.674e-11      /* m^3 kg^-1 s^-2            */
#define LAWS_DEFAULT_SOFTENING  1e5            /* collision softening (m)   */
#define LAWS_MIN_SOFTENING      1.0            /* floor: keep r^2 denom > 0 */
#define LAWS_DEFAULT_FORCE_EXP  2.0            /* inverse-square            */
#define LAWS_DEFAULT_C_LIGHT    2.99792458e8   /* m/s                       */
#define LAWS_DEFAULT_GRAV_ISOLATION 1.0        /* stars gravitationally isolated */

/* Timestep model (per-universe). dt_outer = T/outer_div clamped to
 * [outer_dt_min, outer_dt_default]; dt_inner = T/inner_div clamped to
 * [inner_dt_min, inner_dt_max]. SI seconds (1 day = 86400 s).
 * Don't set inner_dt_min much below 60 s — the integrator diverges. */
#define LAWS_DEFAULT_OUTER_PERIOD_DIVISOR  24.0
#define LAWS_DEFAULT_INNER_PERIOD_DIVISOR  96.0
#define LAWS_DEFAULT_OUTER_DT_MIN          4320.0    /* 0.05 day            */
#define LAWS_DEFAULT_INNER_DT_MIN          60.0      /* 1 min floor         */
#define LAWS_DEFAULT_INNER_DT_MAX          1728.0    /* 0.02 day (~29 min)  */
#define LAWS_DEFAULT_OUTER_DT_DEFAULT      86400.0   /* 1 day               */

typedef struct {
    /* ---- tunable constants (Phase 1) ---------------------------------- */
    double G;            /* gravitational constant                         */
    double softening;    /* Plummer softening length (m)                   */
    double time_scale;   /* multiplier applied to simulated dt (1 = real)  */

    /* ---- custom force law (Phase 2) ----------------------------------- */
    /* Radial acceleration magnitude is  G*m / r^force_exp.
     * force_exp == 2 is Newtonian; only 2 and 1 give closed orbits
     * (Bertrand's theorem) — other values produce precessing/alien orbits. */
    double force_exp;

    /* ---- exotic terms (Phase 2) --------------------------------------- */
    double lambda;       /* cosmological term: a += lambda * r_vec (1/s^2) */
    double pn_factor;    /* post-Newtonian precession strength (0 = off)   */
    double c_light;      /* speed of light, used by the PN term            */

    /* ---- scaling switch (galaxy-scale) -------------------------------- */
    /* When non-zero, star systems are treated as gravitationally isolated:
     * a body only feels the bodies in its own system, never cross-system
     * pairs.  This is physically exact at interstellar distances (the force
     * between stars light-years apart is negligible) and turns the force
     * kernel from O(active x N) into O(sum Ni^2) ~ O(N), which is what makes
     * thousand-system universes run in real time.  For a single-system
     * universe every body is already in one system, so this changes nothing.
     * Set to 0 in JSON for a deliberately-coupled scenario (star cluster,
     * galaxy collision) where interstellar gravity is the point. */
    double gravity_isolation;

    /* ---- per-system adaptive timestep model --------------------------- */
    /* See LAWS_DEFAULT_*_DIVISOR / *_DT_* above. These set integration
     * accuracy vs. cost: larger divisors / smaller dt caps = more accurate,
     * slower; the floors keep the integrator from diverging. */
    double outer_period_divisor;
    double inner_period_divisor;
    double outer_dt_min;
    double inner_dt_min;
    double inner_dt_max;
    double outer_dt_default;
} UniverseLaws;

/* The single active law set.  Read in the physics hot loop, so it is a plain
 * global rather than passed around. */
extern UniverseLaws g_laws;

/* Restore Newtonian defaults.  Call before loading a universe so unspecified
 * JSON fields fall back to physically-standard values. */
void laws_reset(void);

/*
 * laws_pair_factor — pairwise acceleration scale `f`, defined so that
 *
 *     acc += f * m_source * d_vec        (d_vec = source_pos - target_pos)
 *
 * yields an attractive radial acceleration of magnitude  G * m_source / r^p,
 * where p = g_laws.force_exp and r = |d_vec|.
 *
 * Because |d_vec| = r, the acceleration *magnitude* contributed by a pair is
 * exactly  f * m_source * r  for any exponent — handy for force cutoffs.
 *
 * Newtonian inverse-square (p == 2) keeps the original fast path
 * (f = G / r^3) and avoids the pow() call entirely.
 */
static inline double laws_pair_factor(double r2, double r)
{
    if (g_laws.force_exp == 2.0) return g_laws.G / (r2 * r);
    return g_laws.G * pow(r, -(g_laws.force_exp + 1.0));
}
