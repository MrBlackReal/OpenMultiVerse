/*
 * accretion.h — lightweight black-hole accretion model.
 *
 * Grounds AGN activity in physics instead of authored tags. Each black hole
 * holds a gas reservoir that drains at a viscous rate into an accretion rate Ṁ,
 * which sets the luminosity, the Eddington ratio (L/L_edd = agn_activity, the
 * dimensionless "how active" number that drives the disk/jets/torus in
 * render.c), and grows the hole's mass. Runs on the same stellar clock as
 * lifecycle.c (g_stellar_years_per_sec), decoupled from the orbital integrator,
 * so a quasar visibly fades and the hole grows over cosmic time. A no-op at
 * rate 0, so the authored initial look is preserved until the user advances
 * stellar time.
 *
 * Model (all standard closed forms, no fluid sim):
 *   Ṁ   = reservoir / t_visc                 (viscous draining)
 *   L    = η · Ṁ · c²                         (η = radiative efficiency)
 *   L_edd = 1.26e31 W · (M / M☉)              (Eddington luminosity)
 *   activity = L / L_edd                      (Eddington ratio)
 *   M   += Ṁ · dt ;  reservoir -= Ṁ · dt      (feedback: grow + deplete)
 */
#pragma once
#include "body.h"

/* Seed a body's accretion state from its authored mass + agn_activity: sets the
 * gas reservoir so the initial Ṁ reproduces the authored Eddington ratio. A
 * non-black-hole or inactive hole gets an empty reservoir. Call once at load
 * (and when a new black hole is created at runtime). */
void accretion_init_body(Body *b);

/* Advance every black hole's accretion by dt_real_sec scaled by
 * g_stellar_years_per_sec: drain reservoir → Ṁ → L/L_edd → agn_activity, and
 * accrete the mass. No-op when the stellar rate is 0. */
void accretion_step(double dt_real_sec);

/* One Roche-lobe mass-transfer stream observed by the model. */
typedef struct {
    int    donor;       /* g_bodies index of the overflowing companion       */
    int    hole;        /* g_bodies index of the accreting black hole        */
    double rate_kg_s;   /* transfer rate over the last step (stellar-clock s) */
} AccretionFlow;

/* Roche-lobe transfers seen during the most recent accretion_step() (cleared
 * each step; empty while the stellar clock is 0). Sets *out to an internal
 * array valid until the next step; returns the count. Read by field_graph.c
 * as GAS_FLOW edges. */
int accretion_flows(const AccretionFlow **out);
