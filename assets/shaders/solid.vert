#version 330 core
/*
 * solid.vert — uniform-colour geometry with explicit per-vertex alpha
 * Used for: orbital trails (GL_LINE_STRIP)
 *
 * Positions are stored world-relative in the VBO (camera subtraction is NOT
 * pre-baked per vertex). Instead, u_body_offset carries the per-body reference
 * point minus the current camera position (computed CPU-side in double precision
 * to preserve accuracy near planets). This lets the VBO remain valid across
 * camera movement — only a cheap uniform update is needed per body per frame.
 * Alpha represents normalized trail distance from oldest (0) to newest (1).
 */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in float a_alpha;

uniform mat4 u_vp;
uniform vec3 u_body_offset;

out float v_alpha;

void main() {
    v_alpha = a_alpha;
    gl_Position = u_vp * vec4(a_pos + u_body_offset, 1.0);
}
