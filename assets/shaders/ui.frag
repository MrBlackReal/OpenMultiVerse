#version 330 core
/*
 * ui.frag — flat colour or textured quad
 */
in vec2 v_uv;

uniform vec4      u_color;
uniform sampler2D u_tex;
uniform int       u_use_tex;
uniform vec2      u_texel_size;

out vec4 frag_color;

void main() {
    if (u_use_tex == 1)
        frag_color = texture(u_tex, v_uv) * u_color;
    else if (u_use_tex == 2) {
        vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
        vec2 step = u_texel_size;
        vec4 blur = vec4(0.0);
        float total = 0.0;
        for (int y = -10; y <= 10; y++) {
            for (int x = -10; x <= 10; x++) {
                vec2 p = vec2(float(x), float(y));
                vec2 sample_uv = clamp(uv + p * step, vec2(0.0), vec2(1.0));
                float weight = exp(-dot(p, p) / 50.0);
                blur += texture(u_tex, sample_uv) * weight;
                total += weight;
            }
        }
        frag_color = (blur / total) * u_color;
    } else
        frag_color = u_color;
}
