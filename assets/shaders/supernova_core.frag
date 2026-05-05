#version 330 core
/*
 * supernova_core.frag - bright flash core and transition wash.
 */

in vec2 v_uv;

uniform vec3  u_color;
uniform float u_flash_intensity;
uniform float u_core_intensity;
uniform float u_time;
uniform float u_seed;
uniform float u_bill_scale;

out vec4 frag_color;

const float FAR = 2000.0;

void main() {
    float r = length(v_uv);
    float edge = max(u_bill_scale - 1.6, 0.6);
    float theta = atan(v_uv.y, v_uv.x);
    float pulse = 0.92 + 0.08 * sin(u_time * 22.0 + r * 12.0 + u_seed * 9.0);
    float spokes = 0.82 + 0.18 * sin(theta * 6.0 + u_seed * 25.0 + u_time * 6.5);
    float core = exp(-r * r * 2.9);
    float shell = exp(-pow((r - 1.1) * 2.35, 2.0));
    float flash = exp(-r * 0.22) * (1.0 - smoothstep(edge, u_bill_scale, r));
    float glow = core * (1.6 + 1.4 * u_core_intensity)
               + shell * (0.4 + 0.7 * u_core_intensity)
               + flash * (1.1 + 2.1 * u_flash_intensity);
    vec3 hot = mix(vec3(1.62, 1.54, 1.34), u_color * 1.4, smoothstep(0.8, 4.2, r));
    float energy = glow * spokes * pulse;

    if (r >= u_bill_scale || energy < 0.003) discard;

    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);
    frag_color = vec4(hot * energy, 1.0);
}
