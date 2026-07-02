/*
 * nebula.h — real-catalogue nebulae as world-space volumetric clouds.
 *
 * Each nebula is a real object at its catalogue position (J2000 RA/Dec +
 * distance) and real physical size, rendered as a screen-space volumetric
 * raymarch.  One representation at all ranges: far away it covers a few pixels
 * (a soft static blob), flown into it envelops the view — no LOD transition.
 * Beyond the render far-plane the centre/radius are clamped to a shell while
 * preserving angular size, exactly like the star-dot / black-hole passes.
 */
#pragma once
#include "common.h"

void nebula_init(void);

/* Draw all nebulae.  Camera-relative, so the caller passes the camera basis,
 * the camera world position (AU) and the projection params.  Expects to run
 * after opaque geometry with depth test on / depth writes off. */
void nebula_render(const float vp_camrel[16],
                   const float cam_right[3], const float cam_up[3],
                   const float cam_fwd[3], const double cam_pos[3],
                   float fov_tan, float aspect, int screen_w, int screen_h);

void nebula_shutdown(void);

/* Visibility toggle (exposed in the Visuals menu). */
void nebula_set_enabled(int enabled);
int  nebula_enabled(void);

/* Adjustable render params for the Visuals menu.
 *   density — opacity/brightness multiplier (0 = invisible)
 *   steps   — raymarch steps for near/large nebulae (quality vs. perf);
 *             distant nebulae are scaled down from this automatically. */
void nebula_get_params(int *enabled, float *density, int *steps);
void nebula_set_params(int enabled, float density, int steps);

/* Enumeration for the Navigate tab. */
int          nebula_count(void);
const char  *nebula_name(int i);
/* World position of nebula i in AU (zeroed if out of range). */
void         nebula_position(int i, double out[3]);
/* Bounding radius of nebula i in AU (0 if out of range). */
double       nebula_radius_au(int i);
/* Display colour of nebula i (white if out of range). */
void         nebula_color(int i, float out[3]);
