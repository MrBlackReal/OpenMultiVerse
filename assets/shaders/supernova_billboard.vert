#version 330 core
/*
 * supernova_billboard.vert - camera-facing billboard with configurable scale.
 */

layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;
uniform float u_radius;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform float u_bill_scale;
uniform float u_fullscreen;

out vec2 v_uv;

const float EDGE_OVERSCAN = 2.0;

void main() {
    vec2 off = a_uv * 2.0 - 1.0;
    if (u_fullscreen > 0.5) {
        v_uv = off * u_bill_scale * EDGE_OVERSCAN;
        gl_Position = vec4(off, 0.0, 1.0);
        return;
    }
    vec3 world = u_center
               + u_cam_right * (off.x * u_radius * u_bill_scale * EDGE_OVERSCAN)
               + u_cam_up    * (off.y * u_radius * u_bill_scale * EDGE_OVERSCAN);
    v_uv = off * u_bill_scale * EDGE_OVERSCAN;
    gl_Position = u_vp * vec4(world, 1.0);
}
