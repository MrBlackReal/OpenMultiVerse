#version 330 core
/*
 * terrain_minmax.frag — the height range of one freshly baked tile, written
 * to that tile's texel of the range strip (terrain.c reads it back a few
 * frames later, without stalling, to bound the tile for culling and LOD).
 * One fragment per tile, border ring included.
 */
uniform sampler2DArray u_heights;
uniform int u_layer;

out vec2 o_range;

void main()
{
    int   n  = textureSize(u_heights, 0).x;
    float lo = 1e30, hi = -1e30;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float h = texelFetch(u_heights, ivec3(i, j, u_layer), 0).r;
            lo = min(lo, h);
            hi = max(hi, h);
        }
    o_range = vec2(lo, hi);
}
