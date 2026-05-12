#version 330 core
/*
 * color.frag — outputs interpolated vertex colour as-is.
 * When rendering GL_POINTS, pixels outside the inscribed circle are
 * discarded so points appear round instead of square.
 */

in  vec4 v_color;
out vec4 frag_color;

void main() {
    /* Logarithmic depth — consistent with phong.frag.
     * gl_FragCoord.w = 1/t for perspective, so t = 1/gl_FragCoord.w.   */
    const float FAR = 2000.0;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    /* gl_PointCoord is (0,0)..(1,1) across the point sprite.
     * Fade the outer edge so large point sprites, like the build preview
     * ghost planet, do not render with a jagged hard circle. */
    float d = length(gl_PointCoord - vec2(0.5));
    if (d > 0.5) discard;

    float edge = fwidth(d);
    float alpha = 1.0 - smoothstep(0.5 - edge, 0.5, d);
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
