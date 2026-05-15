#version 330 core
/*
 * phong.vert — sphere billboard (oversized for off-axis coverage)
 *
 * The billboard is scaled by BILL_SCALE (2×) so that the sphere silhouette
 * is never clipped when the planet is near the viewport edge. The fragment
 * shader (ray-sphere intersection) handles the actual discard boundary.
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
uniform int   u_inside;   /* 1 when camera is inside the sphere */

out vec2 v_uv;

const float BILL_SCALE = 2.0;

void main() {
    vec2 off = a_uv * 2.0 - 1.0;           /* -1..+1                     */
    v_uv = off * BILL_SCALE;               /* -BILL_SCALE..+BILL_SCALE   */

    if (u_inside == 1) {
        /* Camera is inside this body — billboard would be behind the camera.
         * Use an oversized fullscreen NDC quad instead; the fragment shader
         * reconstructs the per-pixel ray from gl_FragCoord independently of
         * billboard position, so inside-surface rendering is correct. */
        gl_Position = vec4(off.x * 2.0, off.y * 2.0, 0.0, 1.0);
        return;
    }

    /* Oversized billboard: BILL_SCALE * radius in each camera axis */
    vec3 world = u_center
               + u_cam_right * (off.x * u_radius * BILL_SCALE)
               + u_cam_up    * (off.y * u_radius * BILL_SCALE);

    gl_Position = u_vp * vec4(world, 1.0);
}
