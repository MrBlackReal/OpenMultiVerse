/*
 * spectral.c — stellar spectral classification. See spectral.h.
 */
#include "spectral.h"
#include "lifecycle.h"   /* SOLAR_MASS_KG */
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Mass–temperature slope, piecewise around 1 M☉ (empirical main sequence):
 * below a solar mass the sequence flattens (TRAPPIST-1, 0.09 M☉ → ~2500 K,
 * mid-M), above it steepens (Sirius 2.06 M☉ → ~9000 K, A; 20 M☉ → ~37000 K,
 * O). Continuous at M☉ where T = T☉ by construction. */
#define MT_EXP_LOW   0.35
#define MT_EXP_HIGH  0.62
#define T_MIN        1800.0     /* latest M dwarfs                        */
#define T_MAX        55000.0    /* hottest O stars                        */

/* Display-colour fallback for stars without a usable mass: the blue−red
 * balance calibrated so Sol's authored colour (1, 0.92, 0.23) → T☉ exactly
 * (same constants the RadianceField used before this module existed). */
#define SUN_BLUE_RED  (-0.77)
#define HEAT_SLOPE    1.2
#define HEAT_MIN      0.45
#define HEAT_MAX      3.0

static double t_from_colour(const Body *b)
{
    double h = 1.0 + HEAT_SLOPE * ((double)(b->col[2] - b->col[0]) - SUN_BLUE_RED);
    if (h < HEAT_MIN) h = HEAT_MIN;
    if (h > HEAT_MAX) h = HEAT_MAX;
    return SPECTRAL_T_SUN * h;
}

/* Main-sequence T_eff from mass (colour fallback when mass is unusable). */
static double t_main_sequence(const Body *b)
{
    double m = b->mass / SOLAR_MASS_KG;
    if (m <= 0.0) return t_from_colour(b);
    double t = SPECTRAL_T_SUN * pow(m, m < 1.0 ? MT_EXP_LOW : MT_EXP_HIGH);
    if (t < T_MIN) t = T_MIN;
    if (t > T_MAX) t = T_MAX;
    return t;
}

double spectral_t_eff(const Body *b)
{
    if (!b->alive || !b->is_star || b->is_black_hole) return 0.0;
    switch (b->star_phase) {
        case STAR_SUBGIANT:         return t_main_sequence(b) * 0.85;
        case STAR_RED_GIANT:        return 3600.0;   /* giant-branch convergence */
        case STAR_PLANETARY_NEBULA: return 30000.0;  /* exposed hot core         */
        case STAR_WHITE_DWARF:      return 16000.0;  /* young DA                 */
        case STAR_NEUTRON_STAR:     return 6.0e5;    /* thermal surface          */
        case STAR_DEAD:             return 0.0;
        default:                    return t_main_sequence(b);
    }
}

/* Blackbody → sRGB (Tanner Helland's fit of the Planckian locus, 1000 K –
 * 40000 K), normalised so the max component is 1. Good to a few percent —
 * plenty for light tinting. */
void spectral_blackbody_rgb(double t, float out[3])
{
    double tc = t / 100.0;
    if (tc < 10.0)  tc = 10.0;
    if (tc > 400.0) tc = 400.0;

    double r, g, b2;
    r  = tc <= 66.0 ? 255.0
                    : 329.698727446 * pow(tc - 60.0, -0.1332047592);
    g  = tc <= 66.0 ? 99.4708025861 * log(tc) - 161.1195681661
                    : 288.1221695283 * pow(tc - 60.0, -0.0755148492);
    b2 = tc >= 66.0 ? 255.0
                    : (tc <= 19.0 ? 0.0
                                  : 138.5177312231 * log(tc - 10.0)
                                    - 305.0447927307);
    if (r  < 0.0)   r  = 0.0;
    if (r  > 255.0) r  = 255.0;
    if (g  < 0.0)   g  = 0.0;
    if (g  > 255.0) g  = 255.0;
    if (b2 < 0.0)   b2 = 0.0;
    if (b2 > 255.0) b2 = 255.0;

    double m = r > g ? r : g;
    if (b2 > m) m = b2;
    if (m < 1.0) { out[0] = out[1] = out[2] = 1.0f; return; }
    out[0] = (float)(r / m);
    out[1] = (float)(g / m);
    out[2] = (float)(b2 / m);
}

void spectral_light_tint(double t, float out[3])
{
    out[0] = out[1] = out[2] = 1.0f;
    if (t <= 0.0) return;
    float bb[3], sun[3];
    spectral_blackbody_rgb(t, bb);
    spectral_blackbody_rgb(SPECTRAL_T_SUN, sun);
    float m = 0.0f;
    for (int c = 0; c < 3; c++) {
        out[c] = sun[c] > 1e-6f ? bb[c] / sun[c] : bb[c];
        if (out[c] > m) m = out[c];
    }
    if (m < 1e-6f) { out[0] = out[1] = out[2] = 1.0f; return; }
    out[0] /= m; out[1] /= m; out[2] /= m;
}

/* MK class boundaries (K), hottest first. The G/F edge is tuned so Sol
 * (T☉ = 5772 K) lands exactly on G2. */
static const struct { char letter; double t_top, t_bottom; } MK_BINS[] = {
    { 'O', T_MAX + 1.0, 30000.0 },
    { 'B', 30000.0,      9700.0 },
    { 'A',  9700.0,      7200.0 },
    { 'F',  7200.0,      5930.0 },
    { 'G',  5930.0,      5300.0 },
    { 'K',  5300.0,      3900.0 },
    { 'M',  3900.0,      T_MIN - 1.0 },
};

int spectral_classify(const Body *b, SpectralClass *out)
{
    memset(out, 0, sizeof(*out));
    double t = spectral_t_eff(b);
    if (t <= 0.0) return 0;
    out->t_eff = t;

    /* Non-MK compact objects get their own tags. */
    switch (b->star_phase) {
        case STAR_WHITE_DWARF:
            out->letter = 'D';
            snprintf(out->class_str, sizeof(out->class_str), "DA");
            return 1;
        case STAR_PLANETARY_NEBULA:
            out->letter = 'P';
            snprintf(out->class_str, sizeof(out->class_str), "PN");
            return 1;
        case STAR_NEUTRON_STAR:
            out->letter = 'N';
            snprintf(out->class_str, sizeof(out->class_str), "NS");
            return 1;
        default:
            break;
    }

    /* MK letter + subtype from the temperature bins (0 hottest .. 9 coolest),
     * luminosity class from the lifecycle phase. */
    int nbins = (int)(sizeof(MK_BINS) / sizeof(MK_BINS[0]));
    for (int i = 0; i < nbins; i++) {
        if (t < MK_BINS[i].t_bottom && i < nbins - 1) continue;
        double frac = (MK_BINS[i].t_top - t)
                    / (MK_BINS[i].t_top - MK_BINS[i].t_bottom);
        int sub = (int)(frac * 10.0);
        if (sub < 0) sub = 0;
        if (sub > 9) sub = 9;
        out->letter  = MK_BINS[i].letter;
        out->subtype = sub;
        const char *lc = b->star_phase == STAR_RED_GIANT ? "III"
                       : b->star_phase == STAR_SUBGIANT  ? "IV" : "V";
        snprintf(out->class_str, sizeof(out->class_str), "%c%d%s",
                 out->letter, out->subtype, lc);
        return 1;
    }
    return 0;   /* unreachable: the M bin floor is below any clamped T */
}
