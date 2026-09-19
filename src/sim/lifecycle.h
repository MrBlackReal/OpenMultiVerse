/*
 * lifecycle.h — stellar evolution state machine.
 *
 * Stellar evolution lives on its own clock, decoupled from the N-body
 * integrator: a star's radius/colour is a function of its age, and the only
 * moment it perturbs the simulation is its death (a discrete supernova /
 * planetary-nebula event). The orbital integrator is never sped up, so this
 * module is mostly cosmetic + discrete events.
 *
 * Phases: MAIN_SEQUENCE -> SUBGIANT -> RED_GIANT -> death
 *   high mass (>= ~8 Msun): supernova -> NEUTRON_STAR or BLACK_HOLE remnant
 *   low  mass:              planetary-nebula puff -> WHITE_DWARF
 */
#pragma once
#include "common.h"

/* Continuous auto-aging rate: stellar years elapsed per real second.
 * 0 (default) disables auto-aging — phases only change via the Inspect
 * controls. Exposed so the menu can offer a slider. */
extern double g_stellar_years_per_sec;

#define SOLAR_MASS_KG 1.98847e30
#define SUPERNOVA_MASS_MSUN 8.0   /* progenitor mass threshold for core collapse */

/* Main-sequence lifetime estimate: tau ~ 10 Gyr * (M/Msun)^-2.5. */
double lifecycle_ms_lifetime_yr(double mass_kg);

/* 1 if body i is a living, evolvable star (alive, is_star, not a remnant). */
int lifecycle_is_evolvable_star(int i);

/* 1 if a star of this progenitor mass ends in a core-collapse supernova. */
int lifecycle_will_supernova(double mass_kg);

/* Lazily capture the main-sequence appearance + lifetime for star i. */
void lifecycle_ensure_base(int star);

/* Step a star one phase forward along its track. On a red giant this triggers
 * the terminal death event. Returns the new remnant body index (>0) if this
 * step caused the star's death, 0 otherwise. */
int lifecycle_advance_phase(int star);

/* Force terminal death now (supernova+remnant, or planetary nebula+white
 * dwarf) regardless of current phase. Returns the new remnant body index
 * (>0) if a death occurred, 0 otherwise. */
int lifecycle_trigger_death(int star);

/* Continuous auto-aging: advance every star's age by dt_real_sec scaled by
 * g_stellar_years_per_sec and apply any phase transitions. No-op when the
 * rate is 0. */
void lifecycle_step(double dt_real_sec);

/* Human-readable phase name for UI. */
const char *lifecycle_phase_name(int phase);
