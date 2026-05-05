#version 330 core
/*
 * supernova_flash.frag - screen-space exposure wash around the event center.
 */

in vec2 v_uv;

uniform vec2  u_center_uv;
uniform float u_intensity;
uniform float u_radius_uv;
uniform float u_aspect;
uniform vec3  u_tint;

out vec4 frag_color;

void main() {
    vec2 delta = v_uv - u_center_uv;
    float radius = max(u_radius_uv, 1e-4);
    delta.x *= u_aspect;
    float d = length(delta) / radius;
    float inner = exp(-d * d * 24.0);
    float nearGlow = exp(-d * 4.2);
    float wash = exp(-d * 1.20);
    float exposure = clamp(u_intensity * (0.08 + wash * 0.82 + nearGlow * 0.46 + inner * 0.32),
                           0.0, 0.96);
    vec3 col = mix(vec3(1.0), u_tint, smoothstep(0.06, 0.88, d));
    frag_color = vec4(col * exposure, exposure);
}
