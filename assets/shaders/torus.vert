#version 330 core
/*
 * torus.vert — camera-facing billboard for the AGN dust torus.
 * Sized to the torus's outer extent; the fragment shader raymarches the volume.
 * v_world is the camera-relative world position (camera at origin), so
 * normalize(v_world) is the view ray seed.
 */
layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;     /* torus centre (= black hole), camera-relative AU */
uniform float u_ext;        /* billboard half-extent, AU                       */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;

out vec3 v_world;

void main() {
    vec2 off   = a_uv * 2.0 - 1.0;
    vec3 world = u_center
               + u_cam_right * (off.x * u_ext)
               + u_cam_up    * (off.y * u_ext);
    v_world     = world;
    gl_Position = u_vp * vec4(world, 1.0);
}
