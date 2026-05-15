#version 330 core
/*
 * atm.vert — atmospheric glow billboard
 *
 * Sizes the quad to an oversized outer-atmosphere billboard so off-axis
 * planets do not have their atmosphere clipped by the quad bounds.
 */

layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;      /* camera-relative planet centre */
uniform float u_radius;      /* outer atmosphere radius (AU)  */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;

const float BILL_SCALE = 2.0;

void main() {
    vec2 off = a_uv * 2.0 - 1.0;          /* −1 .. +1 */

    if (dot(u_center, u_center) < u_radius * u_radius) {
        /* Camera is inside the atmosphere sphere — cover the full screen so
         * every pixel gets a fragment invocation for the ray test. */
        gl_Position = vec4(off, 0.0, 1.0);
    } else {
        vec3 pos = u_center
                 + u_cam_right * (off.x * u_radius * BILL_SCALE)
                 + u_cam_up    * (off.y * u_radius * BILL_SCALE);
        gl_Position = u_vp * vec4(pos, 1.0);
    }
}
