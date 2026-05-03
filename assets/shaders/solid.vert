#version 330 core
/*
 * solid.vert — uniform-colour geometry with explicit per-vertex alpha
 * Used for: orbital trails (GL_LINE_STRIP)
 *
 * Positions are camera-relative (pre-subtracted on CPU in double precision
 * to avoid float cancellation jitter when the camera is near the trail).
 * Alpha is provided by the CPU and typically represents normalized trail
 * distance from oldest (0) to newest (1).
 */

layout(location = 0) in vec3 a_pos;
layout(location = 1) in float a_alpha;

uniform mat4 u_vp;

out float v_alpha;

void main() {
    v_alpha = a_alpha;
    gl_Position = u_vp * vec4(a_pos, 1.0);
}
