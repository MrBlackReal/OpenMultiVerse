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
