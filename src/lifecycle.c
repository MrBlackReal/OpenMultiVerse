/*
 * lifecycle.c — stellar evolution state machine. See lifecycle.h.
 */
#include "lifecycle.h"
#include "body.h"
#include "field_graph.h"
#include "supernova.h"
#include <math.h>

double g_stellar_years_per_sec = 0.0;   /* auto-aging off by default */

double lifecycle_ms_lifetime_yr(double mass_kg)
{
    double m = mass_kg / SOLAR_MASS_KG;
    if (m < 0.08) m = 0.08;             /* below this it isn't a fusing star */
    /* tau ~ 10 Gyr * (M/Msun)^-2.5 (luminosity ~ M^3.5, fuel ~ M). */
    return 1.0e10 * pow(m, -2.5);
}

int lifecycle_will_supernova(double mass_kg)
{
    return mass_kg >= SUPERNOVA_MASS_MSUN * SOLAR_MASS_KG;
}

int lifecycle_is_evolvable_star(int i)
{
    if (i < 0 || i >= g_nbodies) return 0;
    const Body *b = &g_bodies[i];
    if (!b->alive || !b->is_star || b->is_black_hole) return 0;
    /* Remnants are terminal — they don't evolve further. */
    switch (b->star_phase) {
        case STAR_WHITE_DWARF:
        case STAR_NEUTRON_STAR:
        case STAR_BLACK_HOLE_REMNANT:
        case STAR_DEAD:
            return 0;
        default:
            return 1;
    }
}

void lifecycle_ensure_base(int star)
{
    if (star < 0 || star >= g_nbodies) return;
    Body *b = &g_bodies[star];
    if (b->base_radius <= 0.0) {
        b->base_radius = b->radius;
        b->base_col[0] = b->col[0];
        b->base_col[1] = b->col[1];
        b->base_col[2] = b->col[2];
    }
    if (b->ms_lifetime_yr <= 0.0)
        b->ms_lifetime_yr = lifecycle_ms_lifetime_yr(b->mass);
}

/* Set radius/colour for the evolving (pre-death) phases off the captured
 * main-sequence appearance. Remnant looks are set by supernova.c at spawn. */
static void lifecycle_apply_visual(int star)
{
    Body *b = &g_bodies[star];
    double r0 = b->base_radius > 0.0 ? b->base_radius : b->radius;
    switch (b->star_phase) {
        case STAR_SUBGIANT:
            b->radius = r0 * 4.0;
            b->col[0] = b->base_col[0] * 0.6f + 1.00f * 0.4f;
            b->col[1] = b->base_col[1] * 0.6f + 0.62f * 0.4f;
            b->col[2] = b->base_col[2] * 0.6f + 0.28f * 0.4f;
            break;
        case STAR_RED_GIANT:
            b->radius = r0 * 40.0;
            b->col[0] = 1.00f;
            b->col[1] = 0.38f;
            b->col[2] = 0.16f;
            break;
        case STAR_MAIN_SEQUENCE:
        default:
            b->radius = r0;
            b->col[0] = b->base_col[0];
            b->col[1] = b->base_col[1];
            b->col[2] = b->base_col[2];
            break;
    }
}

int lifecycle_advance_phase(int star)
{
    if (!lifecycle_is_evolvable_star(star)) return 0;
    lifecycle_ensure_base(star);
    Body *b = &g_bodies[star];
    switch (b->star_phase) {
        case STAR_MAIN_SEQUENCE:
            b->star_phase = STAR_SUBGIANT;
            lifecycle_apply_visual(star);
            field_graph_notify_phase(star, b->star_phase);
            return 0;
        case STAR_SUBGIANT:
            b->star_phase = STAR_RED_GIANT;
            lifecycle_apply_visual(star);
            field_graph_notify_phase(star, b->star_phase);
            return 0;
        case STAR_RED_GIANT:
        default:
            return lifecycle_trigger_death(star);
    }
}

int lifecycle_trigger_death(int star)
{
    if (!lifecycle_is_evolvable_star(star)) return 0;
    lifecycle_ensure_base(star);
    /* Both the core-collapse and the gentle planetary-nebula path are handled
     * by supernova_detonate(): it retires this star, spawns the appropriate
     * remnant (black hole / neutron star / white dwarf), and scales the blast
     * by progenitor mass so a low-mass death reads as a soft nebula. */
    return supernova_detonate(star);
}

void lifecycle_step(double dt_real_sec)
{
    if (g_stellar_years_per_sec <= 0.0 || dt_real_sec <= 0.0) return;
    double d_age = dt_real_sec * g_stellar_years_per_sec;

    for (int i = 0; i < g_nbodies; i++) {
        if (!lifecycle_is_evolvable_star(i)) continue;
        lifecycle_ensure_base(i);
        Body *b = &g_bodies[i];
        b->age_yr += d_age;
        double tau = b->ms_lifetime_yr;
        if (tau <= 0.0) continue;
        double frac = b->age_yr / tau;
        int target = STAR_MAIN_SEQUENCE;
        if (frac >= 1.15)      target = -1;                 /* death */
        else if (frac >= 1.10) target = STAR_RED_GIANT;
        else if (frac >= 1.00) target = STAR_SUBGIANT;

        if (target < 0) {
            lifecycle_trigger_death(i);   /* death logs via the supernova hook */
        } else if (target != b->star_phase) {
            b->star_phase = target;
            lifecycle_apply_visual(i);
            field_graph_notify_phase(i, b->star_phase);
        }
    }
}

const char *lifecycle_phase_name(int phase)
{
    switch (phase) {
        case STAR_MAIN_SEQUENCE:      return "Main sequence";
        case STAR_SUBGIANT:           return "Subgiant";
        case STAR_RED_GIANT:          return "Red giant";
        case STAR_PLANETARY_NEBULA:   return "Planetary nebula";
        case STAR_WHITE_DWARF:        return "White dwarf";
        case STAR_NEUTRON_STAR:       return "Neutron star";
        case STAR_BLACK_HOLE_REMNANT: return "Black hole";
        case STAR_DEAD:               return "Remnant";
        default:                      return "Unknown";
    }
}
