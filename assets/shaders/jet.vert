#version 330 core
/*
 * jet.vert — billboard for an AGN's twin relativistic jets.
 * A quad aligned to the spin axis in world space but rotated about that axis to
 * face the camera (a "cylindrical" billboard), spanning both lobes (-len..+len
 * along the axis).  The fragment shader shades a tapered, turbulent, knotted
 * plasma beam with Doppler brightening on the approaching lobe.
 *
 * v_uv = (across -1..1, along -1..1); along 0 is the black hole.
 */
layout(location = 0) in vec2 a_uv;

uniform mat4  u_vp;
uniform vec3  u_center;   /* black-hole centre, camera-relative AU */
uniform vec3  u_axis;     /* spin axis, unit, camera-relative world */
uniform float u_len;      /* jet half-length, AU                   */
uniform float u_width;    /* jet half-width at the billboard edge, AU */

out vec2 v_uv;

void main() {
    vec2 off = a_uv * 2.0 - 1.0;              /* -1 .. +1 */
    vec3 A   = normalize(u_axis);
    vec3 toC = normalize(u_center);           /* camera(origin) -> hole */
    vec3 R   = cross(A, toC);
    if (dot(R, R) < 1e-6) R = cross(A, vec3(1.0, 0.0, 0.0));  /* axis ~ view */
    R = normalize(R);
    vec3 world = u_center + A * (off.y * u_len) + R * (off.x * u_width);
    v_uv        = off;
    gl_Position = u_vp * vec4(world, 1.0);
}
