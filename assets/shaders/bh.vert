#version 330 core
/*
 * bh.vert — billboard for a black hole's shadow + accretion disk.
 * Same expansion as star_glare.vert but sized to the accretion-disk extent.
 * v_uv is in event-horizon-radius units: length(v_uv) == 1 at the horizon,
 * up to BILL_SCALE at the billboard corners (clipped in the fragment shader).
 */
layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;     /* black-hole centre, camera-relative AU */
uniform float u_radius;     /* event-horizon radius, AU              */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;

out vec2 v_uv;

const float BILL_SCALE = 6.0;

void main() {
    vec2 off   = a_uv * 2.0 - 1.0;          /* -1 .. +1 */
    vec3 world = u_center
               + u_cam_right * (off.x * u_radius * BILL_SCALE)
               + u_cam_up    * (off.y * u_radius * BILL_SCALE);
    v_uv        = off * BILL_SCALE;          /* horizon-radius units */
    gl_Position = u_vp * vec4(world, 1.0);
}
