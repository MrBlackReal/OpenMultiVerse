/*
 * post.h — post-processing (HDR bloom).
 *
 * Wraps the scene render in an offscreen HDR framebuffer, then extracts bright
 * areas, blurs them, and composites the glow back over the scene.  Usage per
 * frame:
 *     post_begin();                  // bind HDR target + clear (or no-op)
 *     render_frame(...);             // scene draws into the HDR target
 *     post_end();                    // bloom + composite to the screen
 * When bloom is unavailable or disabled, post_begin()/post_end() are no-ops and
 * the caller renders straight to the default framebuffer as before.
 */
#pragma once

void post_init(void);       /* load shaders + quad; safe to call once after GL init */
void post_begin(void);      /* bind HDR scene target (no-op if disabled) */
void post_end(void);        /* run bloom + composite to screen (no-op if disabled) */

int  post_available(void);  /* shaders compiled OK */
int  post_enabled(void);    /* available AND turned on */

void post_get_bloom(int *enabled, float *threshold, float *intensity);
void post_set_bloom(int enabled, float threshold, float intensity);

/* Tonemap mode: 0 = off (legacy linear), 1 = ACES filmic, 2 = Reinhard.
 * exposure is a linear multiplier applied before the curve (tonemap on). */
void post_get_tonemap(int *mode, float *exposure);
void post_set_tonemap(int mode, float exposure);

/* Lens optics, all opt-in (0 = no effect):
 *   auto_exposure — adapt exposure to scene luminance (multiplies exposure).
 *   chromatic     — lateral chromatic aberration strength.
 *   vignette      — corner darkening, 0..1. */
void post_get_optics(int *auto_exposure, float *chromatic, float *vignette);
void post_set_optics(int auto_exposure, float chromatic, float vignette);

/* Relativistic optics for this frame (beta 0 = off). Set per frame from the
 * camera's actual velocity; applied as screen-space aberration + Doppler shift.
 * (cx, cy) is the heading point in UV (0.5, 0.5 = look axis) — the camera's
 * velocity vector projected to screen space, toward which the field bunches. */
void post_set_relativistic(float beta, float cx, float cy);
