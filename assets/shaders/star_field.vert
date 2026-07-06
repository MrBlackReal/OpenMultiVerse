#version 330 core
/*
 * star_field.vert — static bulk star-field dots (the Gaia catalog field).
 *
 * These stars are frozen scenery: their world positions never change, so their
 * dot geometry is uploaded once into a persistent VBO holding ABSOLUTE positions
 * (AU).  This shader does per-frame work the CPU dot path used to do per star —
 * camera-relative transform, apparent magnitude → point size, HDR gain, and the
 * far-field horizon fade — entirely on the GPU, so the 100k+ field costs the CPU
 * nothing and never re-uploads.  Pairs with color.frag (round sprite + log depth).
 *
 * Stars closer than u_near_dist are culled here: the dynamic near path draws
 * them (as dots that can resolve into spheres), using the exact same threshold
 * so the handoff is seamless.  Stars past u_horizon are culled too.
 */

layout(location = 0) in vec3  a_pos;      /* absolute position, AU              */
layout(location = 1) in vec4  a_color;    /* display colour (a unused)          */
layout(location = 2) in float a_absmag;   /* absolute magnitude, baked at load  */

uniform mat4  u_vp;         /* proj * view_rot (rotation only; translation here) */
uniform vec3  u_cam;        /* camera position, AU                              */
uniform float u_near_dist;  /* AU: below this the dynamic path draws the star   */
uniform float u_horizon;    /* AU: far-field cull distance                      */
uniform float u_time;       /* seconds, twinkle animation                       */
uniform float u_twinkle;    /* twinkle amplitude 0..1 (0 = off)                 */

out vec4 v_color;

const float AU_PER_PC = 206264.806;

float log10f(float x) { return log(x) * 0.4342944819032518; }

void main() {
    vec3  rel = a_pos - u_cam;
    float d   = length(rel);

    /* Cull: near stars belong to the dynamic path; far stars are past horizon.
     * Emit a zero-size point pushed outside clip space so it is discarded. */
    if (d < u_near_dist || d >= u_horizon) {
        gl_Position  = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 0.0;
        v_color      = vec4(0.0);
        return;
    }

    /* Apparent magnitude from baked absolute magnitude + distance modulus,
     * then point size and HDR gain — the GLSL port of star_dot_apparent_mag /
     * star_dot_pixel_size / star_dot_hdr_gain in render.c. */
    float d_pc = max(d / AU_PER_PC, 1e-9);
    float m    = a_absmag + 5.0 * log10f(d_pc) - 5.0;

    float size = clamp(7.0 - 0.45 * (m + 1.0), 1.4, 7.0);
    float gain = (m < 2.5) ? min(6.0, pow(10.0, 0.28 * (2.5 - m))) : 1.0;

    /* Far-field horizon fade (matches farfield_horizon_fade): 1 below 0.85·h,
     * smoothstep to 0 at h.  This goes in the ALPHA channel, exactly like the
     * dynamic dot path (dot_data[...+6] = f): color.frag blends SRC_ALPHA, so a
     * fading star must lower its alpha to blend into the background.  Folding the
     * fade into RGB with alpha=1 instead makes a distant star fade to OPAQUE
     * BLACK — the "distant stars are black pixels" bug. */
    float hs   = u_horizon * 0.85;
    float fade = (d > hs) ? (1.0 - smoothstep(hs, u_horizon, d)) : 1.0;

    vec3 col = a_color.rgb * gain;   /* brightness in RGB; fade stays in alpha */

    /* Micro-twinkle: identical to star_dot.vert so field and near dots shimmer
     * consistently.  Phase hashed from the (stable) colour, not the position. */
    if (u_twinkle > 0.0) {
        float ph = fract(sin(dot(a_color.rgb, vec3(12.9898, 78.233, 37.719)))
                         * 43758.5453) * 6.2831853;
        float tw = sin(u_time * 2.7 + ph) * 0.6 + sin(u_time * 1.3 + ph * 1.7) * 0.4;
        col *= 1.0 + u_twinkle * 0.16 * tw;
    }

    v_color      = vec4(col, fade);
    gl_PointSize = size;
    gl_Position  = u_vp * vec4(rel, 1.0);
}
