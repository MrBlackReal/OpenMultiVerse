#version 330 core
/*
 * galaxy_stars.frag — round soft point sprite for procedural galaxy stars.
 * Log depth like color.frag so planets/opaque geometry occlude correctly;
 * additive-friendly output (colour premultiplied by coverage, alpha 0 so the
 * GL_ONE/GL_ONE blend just adds light over the volume glow).
 */

in  vec4 v_color;
out vec4 frag_color;

void main() {
    const float FAR = DEPTH_FAR;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    /* Soft round falloff instead of a hard disc: tiny stars stay round and
     * bright ones get a slight halo for free. */
    float d = length(gl_PointCoord - vec2(0.5)) * 2.0;
    if (d > 1.0) discard;
    float w = (1.0 - d * d);
    w *= w;

    frag_color = vec4(v_color.rgb * (v_color.a * w), 0.0);
}
