#version 330 core
/*
 * starfield.frag — color.frag plus a global fade (u_fade).
 *
 * The skybox starfield is a painted backdrop for the stellar neighbourhood:
 * direction-only, no parallax. When the camera travels to galactic scales
 * (the §0.1 zoom-out) a glued-to-the-sky star field would break the illusion
 * of leaving, so the CPU fades it out over ~50 ly → ~5 kly of distance from
 * the origin; the Milky Way volume takes over as the unresolved-star glow.
 */

in  vec4 v_color;
out vec4 frag_color;

uniform float u_fade;

void main() {
    if (u_fade <= 0.001) discard;

    /* Logarithmic depth — consistent with phong.frag. */
    const float FAR = DEPTH_FAR;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    float d = length(gl_PointCoord - vec2(0.5));
    if (d > 0.5) discard;

    frag_color = v_color * u_fade;
}
