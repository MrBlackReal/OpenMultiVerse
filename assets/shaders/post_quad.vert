#version 330 core
/*
 * post_quad.vert — fullscreen-quad pass-through for post-processing.
 * Positions arrive in NDC (-1..1); UV is derived for sampling the scene/bloom
 * textures.
 */
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;
void main() {
    v_uv        = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
