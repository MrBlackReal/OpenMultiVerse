#version 330 core
/*
 * bh.vert — billboard for a raymarched black hole.
 * The quad faces the camera and is sized to the lensing/disk extent; the
 * fragment shader casts a real ray per pixel and bends it through the hole's
 * curved spacetime, so the disk and shadow are view-correct from any angle.
 *
 * v_uv  : event-horizon-radius units (length 1 == horizon) for cheap culling.
 * v_world: this billboard point in camera-relative AU.  The camera sits at the
 *          origin in camera-relative space, so normalize(v_world) is exactly the
 *          view ray through this fragment — the raymarch seed direction.
 */
layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;     /* black-hole centre, camera-relative AU */
uniform float u_radius;     /* event-horizon radius, AU              */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;

out vec2 v_uv;
out vec3 v_world;

const float BILL_SCALE = 11.0;

void main() {
    vec2 off   = a_uv * 2.0 - 1.0;          /* -1 .. +1 */
    vec3 world = u_center
               + u_cam_right * (off.x * u_radius * BILL_SCALE)
               + u_cam_up    * (off.y * u_radius * BILL_SCALE);
    v_uv        = off * BILL_SCALE;          /* horizon-radius units */
    v_world     = world;
    gl_Position = u_vp * vec4(world, 1.0);
}
