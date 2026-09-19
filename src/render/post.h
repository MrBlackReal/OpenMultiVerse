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
void post_shutdown(void);   /* delete all FBOs/textures/shaders/buffers post owns */
void post_begin(void);      /* bind HDR scene target (no-op if disabled) */
void post_end(void);        /* run bloom + composite to screen (no-op if disabled) */

int  post_available(void);  /* shaders compiled OK */
int  post_enabled(void);    /* available AND turned on */

/* GL name of the current frame's scene depth texture, or 0 when post is
 * disabled/unavailable (the scene then renders to the default framebuffer,
 * whose depth cannot be sampled). Consumers must not write depth while
 * sampling it — volumetrics draw with depth writes off, so this holds. */
unsigned int post_scene_depth_tex(void);

/* GL name of the HDR scene FBO the frame is rendering into, or 0 when post is
 * disabled (scene renders to the default framebuffer). Lets mid-frame passes
 * that redirect to their own target restore the scene binding without a
 * glGetIntegerv round-trip. */
unsigned int post_scene_fbo(void);

/* Snapshot the scene colour rendered so far into a spare texture and return
 * its GL name (0 when post is disabled/unavailable). The copy is what makes
 * screen-space refraction-style effects legal mid-frame: the caller keeps
 * rendering into the scene target while sampling the snapshot — sampling the
 * live render-target attachment itself would be undefined. Used by the
 * black-hole pass to gravitationally lens the real background. The snapshot
 * is only valid until the next post_grab_scene()/post_end(). */
unsigned int post_grab_scene(void);

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

/* Lens flare feed for this frame (render.c projects the dominant emitter).
 * (ndc_x, ndc_y) = light in NDC, log_depth = its log-encoded depth (same
 * formula the depth-writing shaders use), intensity 0 = no flare (the pass is
 * skipped entirely — byte-identical output), col = light chromaticity.
 * Must be called every frame; the value does not persist. */
void post_set_lens_flare(float ndc_x, float ndc_y, float log_depth,
                         float intensity, const float col[3]);
