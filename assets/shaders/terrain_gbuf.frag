#version 330 core
/*
 * terrain_gbuf.frag — rasterise the terrain mesh into its depth + normal
 * buffer. phong.frag then shades that buffer at one fragment per pixel, as it
 * shades a sphere: shading the mesh directly ran the planet shader over every
 * small triangle's partly covered 2×2 pixel blocks and cost 2–3× as much.
 * Depth is the exact per-pixel log depth, so phong.frag recovers the surface
 * point from it to the depth buffer's precision (3e-6 of the distance).
 */
in vec3 v_rel;
in vec3 v_nrm;

uniform vec3 u_cam_fwd;

out vec4 o_normal;

void main()
{
    gl_FragDepth = log_depth(dot(v_rel, u_cam_fwd));
    o_normal = vec4(normalize(v_nrm), 1.0);
}
