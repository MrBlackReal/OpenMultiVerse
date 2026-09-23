/*
 * render.h — main scene renderer
 *
 * Owns sphere mesh and dot geometry; delegates to starfield, trails,
 * and labels sub-renderers.
 */
#pragma once
#include "common.h"
#include "labels.h"

/* Initialise sphere mesh VAO and dot VBO.  Call after GL context is ready. */
void render_init(void);

/*
 * Draw one full frame.
 *   view     — view matrix (camera transform)
 *   proj     — projection matrix
 *   view_rot — view matrix with translation stripped (for skybox)
 */
void render_frame(const float view[16], const float proj[16],
                  const float view_rot[16], float dt);

void render_shutdown(void);

/* Upload this frame's star-veil glare (star_veil.h) to a program whose shader
 * calls veil_vis(); call after glUseProgram. */
void render_star_veil_uniforms(unsigned int prog);

/* Bind the interstellar dust texture and its uniforms (gl_utils prelude
 * dust_av) to a program; call after glUseProgram. (ox, oy, oz) is the Sun --
 * the dust cube's centre -- in the program's position frame, AU. */
void render_dust_uniforms(unsigned int prog, float ox, float oy, float oz);
