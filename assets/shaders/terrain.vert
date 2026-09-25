#version 330 core
/*
 * terrain.vert — one cube-sphere tile of a planet's mesh terrain.
 *
 * The shared grid carries only integer grid coordinates; the tile's shape
 * comes from terrain_tile.glsl and its heights from the tile cache. The
 * position is built as anchor + (offset from the tile centre), where the
 * anchor (the tile centre on the sphere, camera-relative) was formed on the
 * CPU in double: a vertex a metre from the camera is exact to well under a
 * millimetre however far the planet sits from the floating origin.
 */
#include "terrain_tile.glsl"

layout(location = 0) in vec3 a_grid;   /* i, j, skirt (0 or 1) */

uniform mat4  u_vp;
uniform vec3  u_anchor;    /* tile centre on the sphere − camera (AU), world */
uniform mat3  u_l2w;       /* body-local → world rotation                     */
uniform vec3  u_pc;        /* tile centre direction, body-local (float)       */
uniform float u_body_r;    /* radius (AU)                                     */
uniform float u_skirt;     /* skirt drop, body radii                          */
uniform int   u_layer;
uniform sampler2DArray u_heights;

out vec3 v_rel;   /* surface point − camera (AU) */
out vec3 v_nrm;   /* mesh normal, world frame    */

/* Offset of grid point g from the tile centre on the sphere, in body radii,
 * body-local frame; P returns its direction. */
vec3 local_offset(ivec2 g, out vec3 P)
{
    vec3  dp = tile_dp(vec2(g));
    float h  = texelFetch(u_heights, ivec3(g + 1, u_layer), 0).r;
    P = u_pc + dp;
    return dp + P * h;
}

void main()
{
    ivec2 g = ivec2(a_grid.xy);
    vec3 P, Pn;
    vec3 x  = local_offset(g, P);
    vec3 xr = local_offset(g + ivec2(1, 0), Pn);
    vec3 xl = local_offset(g - ivec2(1, 0), Pn);
    vec3 xu = local_offset(g + ivec2(0, 1), Pn);
    vec3 xd = local_offset(g - ivec2(0, 1), Pn);
    vec3 n  = normalize(cross(xr - xl, xu - xd));
    if (dot(n, P) < 0.0) n = -n;

    /* Skirts hang the tile's rim straight down, so the gap where a coarser
     * neighbour's edge runs lower is filled with rock, not sky. */
    x -= P * (u_skirt * a_grid.z);

    v_rel = u_anchor + u_l2w * (x * u_body_r);
    v_nrm = u_l2w * n;
    gl_Position = u_vp * vec4(v_rel, 1.0);
}
