#version 330 core
/*
 * starfield.vert — color.vert plus the sky direction, so starfield.frag can
 * drown each star in a nearby star's glare (veil_vis, gl_utils prelude).
 * Skybox positions are directions on the unit sphere, in world axes.
 */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;

uniform mat4 u_vp;

out vec4 v_color;
out vec3 v_dir;

void main() {
    v_color     = a_color;
    v_dir       = a_pos;
    gl_Position = u_vp * vec4(a_pos, 1.0);
}
