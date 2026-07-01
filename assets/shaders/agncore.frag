#version 330 core
/*
 * agncore.frag — beamed-core glow (additive). A tight radial glow with a very
 * bright centre; intensity is supplied by the CPU (peaks when the jet points at
 * the camera). HDR-bright so bloom flares it into a blazar core.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform vec3  u_color;
uniform float u_intensity;

void main() {
    float r = length(v_uv);
    if (r > 1.0) discard;
    float halo = pow(1.0 - r, 3.0);
    float core = pow(1.0 - r, 14.0) * 3.0;
    float I = (halo + core) * u_intensity;
    if (I < 0.002) discard;
    /* Logarithmic depth for the depth TEST (mask off), consistent with the
     * log-depth scene — see jet.frag. */
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(DEPTH_FAR + 1.0);
    frag_color = vec4(u_color * I, I);
}
