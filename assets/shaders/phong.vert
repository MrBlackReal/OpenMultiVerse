#version 330 core
/*
 * phong.vert — sphere quad
 *
 * Far from the planet: oversized billboard (BILL_SCALE × radius) so the
 * sphere silhouette is never clipped near the viewport edge.
 *
 * Close to the planet (u_use_fullscreen == 1): fullscreen NDC quad.  The
 * billboard math degenerates whenever the sphere centre is at or behind the
 * camera plane (perspective division by ~0), which happens at any oblique
 * view angle when the camera is near the body.  The fragment shader
 * reconstructs the per-pixel ray from gl_FragCoord and does the ray-sphere
 * intersection independently of vertex position, so a fullscreen quad
 * produces the correct silhouette from any view direction.
 *
 * v_uv is passed in [-BILL_SCALE, +BILL_SCALE] so the fragment shader can
 * reconstruct the exact same world-space position as this vertex.
 */

layout(location = 0) in vec2 a_uv;   /* (0,0)..(1,1) unit quad */

uniform mat4  u_vp;
uniform vec3  u_center;
uniform float u_radius;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform int   u_use_fullscreen;   /* 1 when the billboard would degenerate */
uniform float u_stretch_along;    /* tidal elongation factor (>=1); enlarges quad */

out vec2 v_uv;

const float BILL_SCALE = 2.0;

void main() {
    vec2 off = a_uv * 2.0 - 1.0;           /* -1..+1                     */
    v_uv = off * BILL_SCALE;               /* -BILL_SCALE..+BILL_SCALE   */

    if (u_use_fullscreen == 1) {
        gl_Position = vec4(off.x * 2.0, off.y * 2.0, 0.0, 1.0);
        return;
    }

    /* A tidally stretched body extends past the sphere billboard; grow the quad
     * by the elongation factor so the strand is never clipped. */
    float ext = u_radius * BILL_SCALE * max(u_stretch_along, 1.0);
    vec3 world = u_center
               + u_cam_right * (off.x * ext)
               + u_cam_up    * (off.y * ext);

    gl_Position = u_vp * vec4(world, 1.0);
}
