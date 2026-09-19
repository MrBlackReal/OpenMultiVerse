/*
 * spectral.h — stellar spectral classification (roadmap §1.1).
 *
 * One place that answers "how hot is this star, and what kind is it": the
 * effective temperature **source of truth** promised by the RadianceField
 * design note (§0.3) — physical, from the star's mass and lifecycle phase,
 * instead of guessed from its art-directed display colour.
 *
 *   Main sequence:  T = T☉ · (M/M☉)^k, the empirical mass–temperature slope
 *                   (piecewise k so both M dwarfs and OB stars land on their
 *                   real classes: TRAPPIST-1 → M, Sirius → A, 20 M☉ → O).
 *   Subgiant:       cooler than its main-sequence T (envelope expands).
 *   Red giant:      ~3600 K regardless of mass (they converge on the giant
 *                   branch), luminosity class III.
 *   Remnants:       white dwarf "DA" (hot, tiny), planetary-nebula nucleus
 *                   "PN" (very hot), neutron star "NS" (not an MK class).
 *
 * Stars with no usable mass (hand-authored or sparse catalog rows) fall back
 * to the display-colour estimate the RadianceField used before — the same
 * blue−red balance calibrated so Sol's authored colour → T☉ exactly.
 *
 * Consumers: radiance_field.c (T drives Stefan-Boltzmann luminosity), the
 * Inspect panel (class label + T_eff), and the headless [RadianceField]
 * print. Pure functions of the Body — no state, no GL, safe anywhere.
 */
#pragma once
#include "body.h"

#define SPECTRAL_T_SUN 5772.0   /* solar effective temperature, K */

typedef struct {
    char   class_str[8];   /* MK-style label: "G2V", "M5III", "DA", "NS" */
    char   letter;         /* O B A F G K M — or 'D' (WD), 'P' (PN), 'N' (NS) */
    int    subtype;        /* 0 (hottest in class) .. 9 (coolest); MK only */
    double t_eff;          /* effective temperature, K */
} SpectralClass;

/* Classify star body b. Returns 1 and fills *out for any live star
 * (black holes excluded — no photosphere), else returns 0. */
int spectral_classify(const Body *b, SpectralClass *out);

/* Effective temperature alone (K); 0 for non-stars/black holes. This is what
 * radiance_field.c reads for the Stefan-Boltzmann luminosity. */
double spectral_t_eff(const Body *b);

/* Approximate sRGB chromaticity of a blackbody at temperature t (K),
 * normalised so the max component is 1. */
void spectral_blackbody_rgb(double t, float out[3]);

/* Chromaticity of a blackbody at t RELATIVE to the Sun's, normalised so the
 * max component is 1 — i.e. exactly white for t = T☉. This is the light tint
 * shaders multiply onto their art-directed sunlight ramp: Sol-lit scenes are
 * bit-identical, an M dwarf's planets are lit warm orange, an A star's
 * faintly blue. */
void spectral_light_tint(double t, float out[3]);
