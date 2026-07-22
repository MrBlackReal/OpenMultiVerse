#version 330 core
/*
 * cluster.frag — aggregate star-cluster impostor.
 *
 * One soft additive gaussian glow standing in for a dense clump of field stars
 * that are individually sub-pixel / culled at this distance (the "cluster/
 * hybrid aggregation" LOD handoff).  Pairs with star_dot.vert (per-point size,
 * camera-relative position).  a_color.rgb carries the clump tint pre-scaled by
 * its aggregate brightness; a_color.a carries the impostor crossfade factor
 * (1 when merged/unresolved, →0 as the clump resolves into individual stars).
 *
 * Drawn with additive blending (GL_ONE, GL_ONE) and depth test on / write off,
 * so foreground bodies still occlude it but it never occludes anything.
 */

in  vec4 v_color;
out vec4 frag_color;

void main() {
    /* Logarithmic depth — consistent with color.frag / phong.frag. */
    const float FAR = DEPTH_FAR;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    vec2  pc = gl_PointCoord - vec2(0.5);
    float d2 = dot(pc, pc);
    if (d2 > 0.25) discard;                 /* round sprite */

    /* Bright gaussian core with a broad soft halo — reads as an unresolved
     * glowing swarm rather than a hard dot. */
    float core = exp(-d2 * 22.0);
    float halo = exp(-d2 * 4.0) * 0.35;
    float g = core + halo;

    frag_color = vec4(v_color.rgb * v_color.a * g, 1.0);
}
