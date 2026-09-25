#version 330 core
/*
 * solid.frag — uniform colour with per-vertex alpha (from solid.vert)
 */

in  float v_alpha;

uniform vec4 u_color;   /* rgb from body colour, a = base opacity (0.6) */

out vec4 frag_color;

void main() {
    /* Logarithmic depth — consistent with phong.frag.
     * gl_FragCoord.w = 1/t for perspective, so t = 1/gl_FragCoord.w.   */
    gl_FragDepth = log_depth(1.0 / gl_FragCoord.w);
    frag_color = vec4(u_color.rgb, u_color.a * v_alpha);
}
