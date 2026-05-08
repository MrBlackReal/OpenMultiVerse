#version 330 core
/*
 * supernova_fallback.frag - lightweight visible supernova glow.
 *
 * This pass is intentionally simple. It gives low-end or picky OpenGL drivers
 * a guaranteed visible explosion even if the full volumetric raymarch is too
 * heavy or behaves poorly with depth.
 */

in vec2 v_uv;

uniform vec3  u_color;
uniform float u_flash_intensity;
uniform float u_core_intensity;
uniform float u_cloud_intensity;
uniform float u_bill_scale;

out vec4 frag_color;

void main() {
    float r = length(v_uv) / max(u_bill_scale, 0.001);
    if (r >= 2.0) discard;

    float flash = clamp(u_flash_intensity, 0.0, 1.0);
    float core = clamp(u_core_intensity, 0.0, 1.0);
    float cloud = clamp(u_cloud_intensity, 0.0, 1.0);

    float hot_core = exp(-r * r * 14.0) * (0.42 * flash + 0.34 * core);
    float shock = exp(-pow((r - 0.68) / 0.16, 2.0)) * (0.34 * max(flash, core));
    float haze = exp(-r * 1.9) * (0.18 * cloud + 0.08 * core);
    float rim = smoothstep(1.45, 0.72, r) * smoothstep(0.05, 0.42, r) * 0.22 * cloud;

    float alpha = clamp(hot_core + shock + haze + rim, 0.0, 0.82);
    if (alpha < 0.002) discard;

    vec3 white_hot = vec3(1.55, 1.42, 1.10);
    vec3 ember = vec3(1.10, 0.42, 0.18);
    vec3 col = mix(white_hot, u_color, smoothstep(0.18, 1.35, r));
    col = mix(col, ember, smoothstep(0.72, 1.70, r) * 0.38);

    frag_color = vec4(col * alpha, alpha);
}
