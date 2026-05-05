#version 330 core
/*
 * supernova_flash.frag - screen-space exposure wash around the event center.
 */

in vec2 v_uv;

uniform vec2  u_center_uv;
uniform float u_intensity;
uniform vec3  u_tint;

out vec4 frag_color;

void main() {
    vec2 delta = v_uv - u_center_uv;
    float d = length(delta);
    float angle = atan(delta.y, delta.x);
    float starburst = 0.82 + 0.18 * sin(angle * 8.0);
    float nearGlow = exp(-d * 5.0) * starburst;
    float wash = exp(-d * 1.45);
    float exposure = clamp(u_intensity * (0.22 + wash * 0.95 + nearGlow * 0.65), 0.0, 0.98);
    vec3 col = mix(vec3(1.0), u_tint, smoothstep(0.10, 0.85, d));
    frag_color = vec4(col * exposure, exposure);
}
