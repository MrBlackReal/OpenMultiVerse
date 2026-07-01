#version 330 core
/*
 * star_dot.vert — body centre dots with a per-point size.
 *
 * Same as color.vert (pass-through colour, camera-relative positions × u_vp)
 * but each point carries its own pixel size in attribute 2, so the dot field
 * can convey stellar magnitude (bright/near stars draw larger).  The host must
 * enable GL_PROGRAM_POINT_SIZE.  Pairs with color.frag, which clips each point
 * sprite to a round disc.
 */

layout(location = 0) in vec3  a_pos;
layout(location = 1) in vec4  a_color;
layout(location = 2) in float a_size;   /* final point size in pixels */

uniform mat4  u_vp;
uniform float u_time;      /* seconds, for twinkle animation         */
uniform float u_twinkle;   /* twinkle amplitude 0..1 (0 = off)       */

out vec4 v_color;

void main() {
    v_color = a_color;

    /* Micro-twinkle: a subtle per-star brightness shimmer, no atmosphere
     * needed.  Phase is hashed from the (stable) star colour so it does not
     * drift as the camera moves; two sines of different rates avoid an obvious
     * common period across the field. */
    if (u_twinkle > 0.0) {
        float ph = fract(sin(dot(a_color.rgb, vec3(12.9898, 78.233, 37.719)))
                         * 43758.5453) * 6.2831853;
        float tw = sin(u_time * 2.7 + ph) * 0.6 + sin(u_time * 1.3 + ph * 1.7) * 0.4;
        v_color.rgb *= 1.0 + u_twinkle * 0.16 * tw;
    }

    gl_PointSize = a_size;
    gl_Position  = u_vp * vec4(a_pos, 1.0);
}
