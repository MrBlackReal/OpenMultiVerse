/*
 * earth_tex.h — real satellite imagery for Earth.
 *
 * Day: NASA Blue Marble Next Generation (true colour, no topographic shading).
 * Night: NASA Black Marble city lights. Both public domain; see
 * assets/textures/earth/README.md. Mapped in phong.frag onto the body named
 * "Earth" only — Earth-like exoplanets keep the procedural recipe.
 *
 * Files and resolution come from settings (earth_day_texture,
 * earth_night_texture, earth_texture_max_px), so higher-resolution NASA
 * releases can be dropped in, or a big one capped for a lighter GPU. A missing
 * or unreadable file falls back to procedural Earth, never a failure.
 */
#pragma once

void earth_tex_init(void);      /* load per settings; safe to call again   */
void earth_tex_shutdown(void);
int  earth_tex_ready(void);     /* day texture present                      */

/* Bind day/night to their texture units and set the sampler uniforms on
 * `prog`; `on` = 0 switches the shader's imagery path off for this draw. */
void earth_tex_bind(unsigned int prog, int on);
