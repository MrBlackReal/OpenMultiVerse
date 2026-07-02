/*
 * accretion.c — see accretion.h. Emergent AGN activity from a draining gas
 * reservoir, plus spin-up from accreted angular momentum and companion
 * Roche-lobe feeding — all on the stellar clock.
 */
#include "accretion.h"
#include "lifecycle.h"   /* g_stellar_years_per_sec, SOLAR_MASS_KG */
#include <math.h>

#define ACC_ETA          0.1          /* radiative efficiency, L = η·Ṁ·c²        */
#define ACC_T_VISC_YR    1.0e6        /* viscous reservoir-draining time, years   */
#define ACC_T_TRANSFER_YR 1.0e7       /* Roche mass-transfer timescale, years     */
#define L_EDD_PER_MSUN   1.26e31      /* Eddington luminosity, W per solar mass    */
#define G_GRAV           6.674e-11
#define C_LIGHT          2.99792458e8
#define SEC_PER_YEAR     3.15576e7
#define EDD_MAX          2.0          /* clamp activity (super-Eddington blazars)  */
#define A_MAX            0.998         /* Thorne spin limit                         */

static double eddington_luminosity(double mass_kg)
{
    return L_EDD_PER_MSUN * (mass_kg / SOLAR_MASS_KG);
}

/* Schwarzschild radius (m) from mass. */
static double schwarzschild_radius(double mass_kg)
{
    return 2.0 * G_GRAV * mass_kg / (C_LIGHT * C_LIGHT);
}

/* Prograde Kerr ISCO radius in units of M (= GM/c²), Bardeen 1972. */
static double kerr_risco_M(double a)
{
    a = fabs(a);
    if (a > A_MAX) a = A_MAX;
    double z1 = 1.0 + cbrt(1.0 - a * a) * (cbrt(1.0 + a) + cbrt(1.0 - a));
    double z2 = sqrt(3.0 * a * a + z1 * z1);
    return 3.0 + z2 - sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2));
}

/* Dimensionless specific angular momentum L̃ = L·c/(G·M) of matter at the
 * prograde ISCO (Bardeen-Press-Teukolsky 1972). At a*=0 this is √12. */
static double kerr_l_isco(double a)
{
    a = fabs(a);
    if (a > A_MAX) a = A_MAX;
    double r  = kerr_risco_M(a);
    double sr = sqrt(r);
    /* L̃ = (r² − 2a√r + a²) / (r^¾·√(r^{3/2} − 3√r + 2a)); = √12 at a=0. */
    return (r * r - 2.0 * a * sr + a * a) /
           (pow(r, 0.75) * sqrt(r * sr - 3.0 * sr + 2.0 * a));
}

void accretion_init_body(Body *b)
{
    b->mdot            = 0.0;
    b->eddington_ratio = 0.0;
    if (!b->is_black_hole) { b->gas_reservoir = 0.0; b->spin_a = 0.0; return; }

    /* Seed the dimensionless spin a* from the authored rotation_rate: a* = Ω·Rs/c
     * (matches render.c bh_scales), signed by rotation sense, clamped to the
     * Thorne limit. This is now the source of truth for the ISCO. */
    double Rs = schwarzschild_radius(b->mass > 0.0 ? b->mass : 1.0);
    double a  = b->rotation_rate * Rs / C_LIGHT;
    if (a >  A_MAX) a =  A_MAX;
    if (a < -A_MAX) a = -A_MAX;
    b->spin_a = a;

    /* Only active holes start with fuel; a bare/quiet hole stays starved unless
     * a companion feeds it later. */
    if (b->agn_activity <= 0.0f || b->mass <= 0.0) { b->gas_reservoir = 0.0; return; }
    double L_edd  = eddington_luminosity(b->mass);
    double edd0   = b->agn_activity;                  /* authored Eddington ratio */
    double mdot0  = edd0 * L_edd / (ACC_ETA * C_LIGHT * C_LIGHT);
    double t_visc = ACC_T_VISC_YR * SEC_PER_YEAR;
    b->gas_reservoir   = mdot0 * t_visc;              /* fuel to sustain ~t_visc  */
    b->mdot            = mdot0;
    b->eddington_ratio = edd0;
}

/* Roche streams seen during the most recent step, for field_graph.c. A pair
 * per feeding binary, so a small fixed table suffices (drop on overflow). */
#define ACC_MAX_FLOWS 64
static AccretionFlow s_flows[ACC_MAX_FLOWS];
static int           s_flow_count = 0;

int accretion_flows(const AccretionFlow **out)
{
    *out = s_flows;
    return s_flow_count;
}

/* Eggleton (1983) Roche-lobe radius of the donor, as a fraction of the orbital
 * separation, for mass ratio q = M_donor / M_accretor. */
static double roche_lobe_frac(double q)
{
    double q13 = cbrt(q), q23 = q13 * q13;
    return 0.49 * q23 / (0.6 * q23 + log(1.0 + q13));
}

/* Feed a black hole from any bound companion (direct child) overflowing its
 * Roche lobe: move mass from the donor into the hole's gas reservoir. */
static void roche_feed(int hole, double dt_sec)
{
    Body *h = &g_bodies[hole];
    double t_transfer = ACC_T_TRANSFER_YR * SEC_PER_YEAR;
    for (int j = 0; j < g_nbodies; j++) {
        Body *d = &g_bodies[j];
        if (j == hole || !d->alive || d->parent != hole) continue;
        if (d->mass <= 0.0 || d->radius <= 0.0) continue;

        double dx = d->pos[0] - h->pos[0];
        double dy = d->pos[1] - h->pos[1];
        double dz = d->pos[2] - h->pos[2];
        double sep = sqrt(dx * dx + dy * dy + dz * dz);
        if (sep <= 0.0) continue;

        double q  = d->mass / h->mass;
        double RL = sep * roche_lobe_frac(q);
        if (d->radius <= RL) continue;                /* fits inside its lobe     */

        double overflow = (d->radius - RL) / d->radius;   /* 0..1 fractional      */
        double dM = d->mass * (dt_sec / t_transfer) * overflow;
        if (dM > 0.02 * d->mass) dM = 0.02 * d->mass; /* cap per step (stability) */
        d->mass          -= dM;
        h->gas_reservoir += dM;                       /* fuels the hole           */

        /* Record the stream for the field graph (donor → hole, kg/s). */
        if (dM > 0.0 && dt_sec > 0.0 && s_flow_count < ACC_MAX_FLOWS) {
            s_flows[s_flow_count].donor     = j;
            s_flows[s_flow_count].hole      = hole;
            s_flows[s_flow_count].rate_kg_s = dM / dt_sec;
            s_flow_count++;
        }
    }
}

void accretion_step(double dt_real_sec)
{
    s_flow_count = 0;   /* streams are re-observed every step */
    if (g_stellar_years_per_sec <= 0.0 || dt_real_sec <= 0.0) return;

    double dt_sec = dt_real_sec * g_stellar_years_per_sec * SEC_PER_YEAR;
    double t_visc = ACC_T_VISC_YR * SEC_PER_YEAR;

    for (int i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        if (!b->alive || !b->is_black_hole) continue;

        /* Companion Roche-lobe overflow tops up the reservoir first. */
        roche_feed(i, dt_sec);

        if (b->gas_reservoir <= 0.0) {
            b->mdot = 0.0;
            b->eddington_ratio = 0.0;
            b->agn_activity = 0.0f;          /* starved: fades to a quiet hole   */
            continue;
        }

        double mdot = b->gas_reservoir / t_visc;
        double dM   = mdot * dt_sec;
        if (dM > b->gas_reservoir) dM = b->gas_reservoir;

        /* Spin-up: matter falling in from the prograde ISCO carries specific
         * angular momentum ℓ = L̃·GM/c, so J += ℓ·dM and a* = Jc/(GM²) climbs
         * toward the Thorne limit (Bardeen 1970). Use the pre-accretion mass. */
        double Mold = b->mass;
        double mag  = fabs(b->spin_a);
        double sgn  = b->spin_a < 0.0 ? -1.0 : 1.0;    /* 0 spins up prograde     */
        double J    = mag * G_GRAV * Mold * Mold / C_LIGHT;
        double dJ   = kerr_l_isco(mag) * (G_GRAV * Mold / C_LIGHT) * dM;
        double Mnew = Mold + dM;
        double magnew = (J + dJ) * C_LIGHT / (G_GRAV * Mnew * Mnew);
        if (magnew > A_MAX) magnew = A_MAX;
        b->spin_a = sgn * magnew;

        b->gas_reservoir -= dM;              /* deplete */
        b->mass           = Mnew;            /* the hole grows as it eats */

        double L   = ACC_ETA * mdot * C_LIGHT * C_LIGHT;
        double edd = L / eddington_luminosity(b->mass);
        b->mdot            = mdot;
        b->eddington_ratio = edd;
        b->agn_activity    = (float)(edd > EDD_MAX ? EDD_MAX : edd);
    }
}
