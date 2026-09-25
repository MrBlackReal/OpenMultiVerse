/*
 * terrain_tile.glsl — cube-sphere tile geometry, shared by the tile generator
 * (terrain_gen.frag) and the mesh (terrain.vert). See src/render/terrain.c.
 *
 * A tile is the square [sc ± w/2] × [tc ± w/2] of one cube face, whose cube
 * point is q = n + s·u + t·v and whose sphere direction is P = q/|q|. Its
 * centre qc is a dyadic rational, exact in float at any depth the quadtree
 * reaches. What float cannot hold is P itself to metre precision, so P is
 * never formed: tile_dp returns P − Pc, the offset from the tile centre's
 * direction, computed from small quantities only. The CPU owns Pc in double
 * and folds it into the tile's anchor, so positions keep their precision
 * however deep the tile.
 */
#define TILE_N 65   /* vertices per tile edge: TERRAIN_TILE_N in terrain.h */

uniform vec3 u_face_n;   /* cube face normal, body-local frame   */
uniform vec3 u_face_u;   /* face axes (u × v = n)                */
uniform vec3 u_face_v;
uniform vec3 u_tile;     /* sc, tc, w                            */

/* P − Pc at grid point g (0..TILE_N-1 inside the tile; -1 and TILE_N are the
 * border ring the normals and generator read). With l = |q|, lc = |qc|:
 *   q/l − qc/lc = (dq·lc − qc·(l − lc)) / (l·lc)
 * and l − lc = (2 qc·dq + dq·dq) / (l + lc), cancellation-free. */
vec3 tile_dp(vec2 g)
{
    vec3  qc = u_face_n + u_tile.x * u_face_u + u_tile.y * u_face_v;
    vec2  d  = (g / float(TILE_N - 1) - 0.5) * u_tile.z;
    vec3  dq = d.x * u_face_u + d.y * u_face_v;
    float lc = length(qc);
    float l  = length(qc + dq);
    float dl = (2.0 * dot(qc, dq) + dot(dq, dq)) / (l + lc);
    return (dq * lc - qc * dl) / (l * lc);
}

/* The direction itself, where float precision is enough (continent-scale
 * noise, lighting). */
vec3 tile_dir(vec2 g)
{
    vec3 qc = u_face_n + u_tile.x * u_face_u + u_tile.y * u_face_v;
    vec2 d  = (g / float(TILE_N - 1) - 0.5) * u_tile.z;
    return normalize(qc + d.x * u_face_u + d.y * u_face_v);
}
