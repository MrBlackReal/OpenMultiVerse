#version 330 core
/*
 * comet.vert — pass-through for comet coma/tail ribbons.
 * Positions arrive camera-relative (AU); v_pos feeds the fragment shader's
 * per-fragment log-depth (the ribbons span a large depth range, so the
 * usual per-primitive billboard depth would sort wrongly along the tail).
 */
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;

uniform mat4 u_vp;

out vec2 v_uv;
out vec3 v_pos;

void main() {
    v_uv  = a_uv;
    v_pos = a_pos;
    gl_Position = u_vp * vec4(a_pos, 1.0);
}
