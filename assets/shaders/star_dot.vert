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

/* Interstellar dust (prelude dust_av; positions here are camera-relative, so
 * the camera is the origin). u_dust_extend > 0 replaces each point's distance
 * with that many AU along its direction -- for sprites parked on a shell
 * closer than their true distance (galaxy impostors), whose light still
 * crosses all the dust on the way. u_dust_sun_corr = 1 when the dot's
 * magnitude came from a catalog, i.e. already contains the Sun's column. */
uniform float u_dust_extend;
uniform float u_dust_sun_corr;

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

    /* Star veil (clusters only: the dynamic-dot pass applies it on the CPU
     * so it can exempt the exposed star's own system, and leaves
     * u_veil_e/f at 0 here). */
    v_color.a *= veil_vis(a_pos, max(max(v_color.r, v_color.g), v_color.b));

    /* Extinction. The magnitude was turned into size (0.45 px per mag, 1.4 px
     * floor) and colour on the CPU; dust moves the magnitude by dm, so shrink
     * by the same slope and put whatever the floor cannot show into alpha. */
    float size = a_size;
    if (u_dust_on > 0.0) {
        vec3  far = (u_dust_extend > 0.0) ? normalize(a_pos) * u_dust_extend : a_pos;
        float av  = dust_av(vec3(0.0), far);
        float dm  = DUST_AG_PER_AV * av;
        if (u_dust_sun_corr > 0.0)
            dm -= DUST_AG_PER_AV * dust_av(u_dust_origin, far);
        if (dm != 0.0) {
            size = clamp(a_size - 0.45 * dm, min(a_size, 1.4), 7.0);
            float hidden = dm - (a_size - size) / 0.45;
            v_color.a  *= min(1.0, pow(10.0, -0.4 * hidden));
        }
        v_color.rgb *= dust_redden(av);
    }

    gl_PointSize = size;
    gl_Position  = u_vp * vec4(a_pos, 1.0);
}
