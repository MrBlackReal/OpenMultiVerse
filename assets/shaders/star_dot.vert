#version 330 core
/*
 * star_dot.vert — body centre dots with a per-point size.
 *
 * Same as color.vert (pass-through colour, camera-relative positions × u_vp)
 * but each point carries its own pixel size in attribute 2, so the dot field
 * can convey stellar magnitude (bright/near stars draw larger).  The host must
 * enable GL_PROGRAM_POINT_SIZE.  Pairs with color.frag, which clips each point
 * sprite to a round disc.
 */

layout(location = 0) in vec3  a_pos;
layout(location = 1) in vec4  a_color;
layout(location = 2) in float a_size;   /* final point size in pixels */

uniform mat4 u_vp;

out vec4 v_color;

void main() {
    v_color      = a_color;
    gl_PointSize = a_size;
    gl_Position  = u_vp * vec4(a_pos, 1.0);
}
