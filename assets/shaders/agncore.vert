#version 330 core
/*
 * agncore.vert — camera-facing billboard for the AGN beamed core.
 * A small bright glow at the black hole centre that lights up when you look
 * down the jet (blazar): the relativistically-beamed approaching jet base
 * outshines everything, and gives a jets-only / pole-on nucleus a punchy core.
 */
layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;
uniform float u_size;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;

out vec2 v_uv;

void main() {
    vec2 off   = a_uv * 2.0 - 1.0;
    vec3 world = u_center + u_cam_right * (off.x * u_size) + u_cam_up * (off.y * u_size);
    v_uv        = off;
    gl_Position = u_vp * vec4(world, 1.0);
}
