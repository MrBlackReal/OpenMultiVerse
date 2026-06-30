#version 330 core
/*
 * bloom_blur.frag — separable Gaussian blur (9-tap).
 * Run twice per iteration with u_dir = (1/width, 0) then (0, 1/height) to get a
 * 2D blur from two cheap 1D passes.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_tex;
uniform vec2      u_dir;   /* texel step along the blur axis */

void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 result = texture(u_tex, v_uv).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        result += texture(u_tex, v_uv + u_dir * float(i)).rgb * w[i];
        result += texture(u_tex, v_uv - u_dir * float(i)).rgb * w[i];
    }
    frag_color = vec4(result, 1.0);
}
