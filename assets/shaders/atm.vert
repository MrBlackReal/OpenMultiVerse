#version 330 core
/*
 * atm.vert — atmospheric glow quad
 *
 * Far from the planet: oversized billboard (BILL_SCALE × radius) so off-axis
 * atmospheres are not clipped by the quad bounds.
 *
 * Within 4 radii: fullscreen NDC quad — the billboard math degenerates at
 * oblique view angles where the planet centre projects near the camera plane.
 * The fragment shader reconstructs the per-pixel ray independently and does
 * the ray-sphere intersection, so a fullscreen quad produces the correct
 * silhouette from any position or look direction.
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

    if (dot(u_center, u_center) < u_radius * u_radius * 16.0) {
        gl_Position = vec4(off.x * 2.0, off.y * 2.0, 0.0, 1.0);
    } else {
        vec3 pos = u_center
                 + u_cam_right * (off.x * u_radius * BILL_SCALE)
                 + u_cam_up    * (off.y * u_radius * BILL_SCALE);
        gl_Position = u_vp * vec4(pos, 1.0);
    }
}
