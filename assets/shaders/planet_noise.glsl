/*
 * planet_noise.glsl — the procedural noise and relief every planet surface
 * shares: the ray-traced sphere (phong.frag) paints and bump-maps
 * with it, and the terrain tile generator (terrain_gen.frag) displaces the
 * mesh with the same functions, so both views agree on where the mountains are.
 */

/* ======================================================================
 * 3-D value noise — no seams, rotation-aware
 * ====================================================================== */

float hash3(vec3 p) {
    p  = fract(p * vec3(127.1, 311.7, 74.7));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);          /* smooth-step */
    return mix(
        mix(mix(hash3(i),               hash3(i + vec3(1,0,0)), f.x),
            mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), f.x),
            mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), f.x), f.y),
        f.z);
}

/* 2-octave FBM — deliberately blurry (rough and washy, as requested) */
float fbm(vec3 p) {
    return vnoise(p)               * 0.65
         + vnoise(p * 2.1 + vec3(7.3, 2.1, 5.8)) * 0.35;
}

float moon_height(vec3 NL)
{
    float n = fbm(NL * 3.5);
    float n2 = fbm(NL * 6.0 + vec3(2.7, 5.4, 1.8));
    float n3 = fbm(NL * 12.0 + vec3(6.8, 1.7, 4.9));
    float maria = smoothstep(0.34, 0.72, 1.0 - n2)
                * smoothstep(0.22, 0.86, n3);
    float highland = smoothstep(0.50, 0.88, n2);
    float crater_noise = vnoise(NL * 22.0 + vec3(3.1, 7.4, 1.6));
    float crater_soft = smoothstep(0.76, 0.94, crater_noise)
                      * smoothstep(0.28, 0.90, n3);
    float crater_cell = vnoise(NL * 34.0 + vec3(8.6, 2.2, 5.4));
    float crater_rim = smoothstep(0.56, 0.72, crater_cell)
                     * (1.0 - smoothstep(0.72, 0.88, crater_cell))
                     * smoothstep(0.30, 0.86, n3);
    float crater_floor = smoothstep(0.78, 0.96, crater_cell)
                       * smoothstep(0.24, 0.80, 1.0 - n2);
    float fine = smoothstep(0.92, 0.99,
                            vnoise(NL * 28.0 + vec3(8.3, 2.1, 5.6)));

    return n * 0.05
         + highland * 0.10
         - maria * 0.08
         - crater_soft * 0.055
         - crater_floor * 0.090
         + crater_rim * 0.095
         + fine * 0.010;
}

/* Generic terrain relief for solid worlds: continent-scale undulation (keyed
 * on the same fbm(NL·3.5) the colour recipes use, so relief follows the
 * painted landforms), ridged mountain chains, and fine roughness. */
float terrain_height(vec3 NL)
{
    float base  = fbm(NL * 3.5);
    float ridge = 1.0 - abs(2.0 * fbm(NL * 8.0 + vec3(4.4, 8.8, 2.2)) - 1.0);
    float fine  = fbm(NL * 24.0 + vec3(9.1, 3.3, 6.6));
    return base * 0.45 + ridge * ridge * 0.40 + fine * 0.15;
}

/* Continent-scale relief shape of a solid world, in units of its relief
 * amplitude (terrain.c terrain_relief_amp scales it to radii). Built on the
 * same noise the colour recipes paint with, so the mesh's mountains stand
 * where the sphere draws highlands. `rough` (0..~1.3) scales the fine detail
 * the tile generator adds on top: flat seas stay flat, uplands get rugged.
 *   14 airless moon   moon_height: maria, highlands, crater rims
 *    1 Earth-like     land only; the sea is a level surface at 0
 *   other solids      terrain_height, centred on its mean */
float relief_shape(vec3 NL, int ptype, out float rough)
{
    if (ptype == 14) {
        rough = 1.0;
        return moon_height(NL) * 4.0;
    }
    float th = terrain_height(NL);
    if (ptype == 1) {
        float land = smoothstep(0.44, 0.49, fbm(NL * 3.5));
        rough = land * (0.3 + th);
        return land * (0.10 + th);
    }
    rough = 0.3 + th;
    return th - 0.45;
}
