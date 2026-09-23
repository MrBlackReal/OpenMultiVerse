/*
 * dust_field.h — the measured 3D interstellar dust around the Sun.
 *
 * Edenhofer et al. (2023), a Gaussian-process reconstruction of dust extinction
 * from 54 million Gaia stars, baked by tools/fetch_dustmap.py into a DustBin
 * cube (assets/catalogs/dust_local.bin): 256^3 voxels over +/-1250 pc, centred
 * on the Sun, already in the simulator's GL axes.
 *
 * Values are extinction per parsec in the Zhang, Green & Rix (2023) system;
 * integrated along a ray and multiplied by DUST_AV_PER_ZGR they give A_V, the
 * V-band extinction in magnitudes (Edenhofer+ 2023; dustmaps docs).
 *
 * Consumers:
 *   - star shaders, through the gl_utils prelude's dust_av() and the 3D
 *     texture render.c builds from dust_field_density(): stars dim and redden
 *     by the column between the camera and each star;
 *   - catalog magnitudes, which were derived from Gaia apparent magnitudes and
 *     so already contain the column between the SUN and the star:
 *     dust_field_ag() removes it once at load, making them intrinsic.
 * From the Sun the two cancel exactly, so the home sky is unchanged; the dust
 * only shows as you travel.
 *
 * Main-thread only. Everything reads as zero dust if the file is missing.
 */
#pragma once

#define DUST_AV_PER_ZGR  2.8     /* A_V per unit ZGR23 extinction            */
#define DUST_AG_PER_AV   0.789   /* Gaia G band (Wang & Chen 2019)           */

/* Display-channel extinction relative to A_V: Cardelli, Clayton & Mathis
 * (1989), R_V = 3.1, at 610 / 540 / 460 nm. Blue is lost fastest: reddening. */
#define DUST_KR 0.890
#define DUST_KG 1.020
#define DUST_KB 1.252

/* Load a DustBin; returns 1 on success. Safe to call again (reloads). */
int  dust_field_load(const char *path);
int  dust_field_loaded(void);

/* Half-size of the cube in AU (it spans [-h, h] on each axis about the Sun),
 * voxel edge in AU, and the density scale (ZGR23 per pc at 1.0). */
double dust_field_half_au(void);
double dust_field_voxel_au(void);
float  dust_field_vmax(void);

/* A_V in magnitudes along the straight segment a -> b, positions in AU in the
 * Sun-centred world frame. Zero outside the cube or when nothing is loaded. */
double dust_field_av(const double a_au[3], const double b_au[3]);

/* A_G from the Sun (world origin) to p: what a catalog magnitude contains. */
double dust_field_ag_from_sun(const double p_au[3]);

/* A new malloc'd copy of the cube as linear density divided by vmax (so it
 * fits a half-float texture), x fastest; the caller frees it. *dim and *vmax
 * receive the size and the scale. NULL when nothing is loaded. */
float *dust_field_density_alloc(int *dim, float *vmax);
