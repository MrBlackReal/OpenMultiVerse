#version 330 core
/*
 * star_glare.frag — soft additive shine for star bodies
 *
 * Rendered with GL_ONE / GL_ONE (additive) over the disc pass.
 * v_uv is in solar-radius units; length(v_uv) == 1.0 at the disc edge.
 *
 * Single smooth exponential bloom — no spikes, no hard edges.
 *
 * Two fade multipliers guarantee the glow is exactly zero at the billboard
 * boundary so no sprite edge ring is ever visible:
 *   inner_fade  — ramps in over [0.5, 1.3] so the disc pass is not doubled
 *   outer_fade  — ramps out over [BILL_SCALE-4, BILL_SCALE] → smooth cutoff
 *
 * Colour: white-hot just outside the disc, transitions to star colour
 *         (typically solar yellow) as distance increases.
 */

in vec2 v_uv;

uniform vec3  u_color;
uniform float u_spike;    /* diffraction-spike strength (0 = none, default) */
uniform float u_corona;   /* corona streamer strength  (0 = none, default)  */
uniform float u_time;     /* seconds, for corona animation                  */
uniform float u_seed;     /* per-star phase offset so coronae differ        */

out vec4 frag_color;

const float BILL_SCALE = 15.0;

void main() {
    const float FAR = DEPTH_FAR;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    float r = length(v_uv);
    if (r >= BILL_SCALE) discard;

    float r_safe = max(r, 0.05);

    /* Smooth exponential bloom — covers disc + surround.
     * No inner_fade: the glow also brightens the disc area additively,
     * which eliminates the dark ring that appeared at the disc silhouette. */
    float shine = 2.1 * exp(-r_safe * 0.48);

    /* Very wide fade zone (9 solar radii) so the glow melts into black
     * gradually — no hard outer ring visible.                              */
    float outer_fade = 1.0 - smoothstep(BILL_SCALE - 9.0, BILL_SCALE, r);

    float total = shine * outer_fade;

    /* Optional 4-point diffraction spikes (screen-axis aligned, since v_uv runs
     * along cam_right/cam_up). Thin angular lobes with a long radial reach, added
     * on top of the smooth bloom. */
    if (u_spike > 0.0) {
        float ang  = atan(v_uv.y, v_uv.x);
        float lobe = pow(abs(cos(2.0 * ang)), 18.0);   /* peaks every 90° */
        total += u_spike * lobe * exp(-r_safe * 0.22) * outer_fade;
    }

    /* Optional corona: faint animated streamers, stronger on hot (blue) stars.
     * Soft one-sided angular lobes that rotate slowly; per-star u_seed keeps
     * neighbouring stars from pulsing in lockstep. */
    if (u_corona > 0.0) {
        float ang = atan(v_uv.y, v_uv.x);
        float streamers = sin(ang * 9.0  + u_seed * 6.2831 + u_time * 0.50)
                  + 0.6 * sin(ang * 17.0 - u_seed * 3.0    - u_time * 0.31);
        streamers = max(streamers, 0.0);
        float hot = clamp((u_color.b - u_color.r) * 1.4 + 0.35, 0.0, 1.0);
        total += u_corona * streamers * exp(-r_safe * 0.30) * outer_fade
                 * (0.25 + 0.75 * hot) * 0.35;
    }

    if (total < 0.001) discard;

    /* Colour: warm white near disc edge, pure star colour at distance */
    vec3 hot = vec3(1.3, 1.2, 0.85);          /* white-yellow hot core      */
    vec3 col = mix(hot, u_color, smoothstep(1.0, 4.0, r));

    frag_color = vec4(col * total, 1.0);
}
