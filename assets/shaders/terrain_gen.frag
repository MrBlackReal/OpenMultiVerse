#version 330 core
/*
 * terrain_gen.frag — bake one terrain tile's heights (in body radii) into its
 * layer of the tile cache. One texel per mesh vertex plus a one-texel border
 * ring, so the mesh can take normals by central differences at its edges.
 *
 * Height = amp · (relief_shape + rough · detail):
 *   relief_shape  continent-scale, from planet_noise.glsl (what the sphere
 *                 paints and bump-maps with);
 *   detail        fractal octaves from 64 cycles per radius down to the
 *                 tile's own texel spacing (terrain.c picks the set, and
 *                 fades the last one in so a new level does not pop).
 * The detail octaves reach 10^7 cycles per radius, far past what a float
 * lattice coordinate can hold, so each is evaluated around the tile centre:
 * the CPU splits Rk·Pc·f into an integer cell (u_oct_i) and a fraction
 * (u_oct_r), and the shader adds the small offset Rk·(P − Pc)·f. The lattice
 * is hashed on integers, so it stays exact to any depth. Rk turns each
 * octave's lattice a different way and the noise is gradient noise, so no
 * grid direction survives the sum.
 */
#include "planet_noise.glsl"
#include "terrain_tile.glsl"

#define MAX_OCT 24   /* TERRAIN_MAX_OCT in terrain.c */

uniform int   u_ptype;
uniform float u_amp;              /* relief amplitude, body radii */
uniform int   u_noct;
uniform float u_oct_f[MAX_OCT];   /* cycles per radius            */
uniform ivec3 u_oct_i[MAX_OCT];   /* integer cell of Pc·f (+seed) */
uniform vec3  u_oct_r[MAX_OCT];   /* Pc·f − that cell             */
uniform float u_oct_a[MAX_OCT];   /* amplitude, fade included     */
uniform mat3  u_oct_rot[MAX_OCT]; /* the octave's lattice rotation */

out float o_height;

uvec3 pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

/* Gradient at a lattice point, components in [-1, 1]. */
vec3 lattice_grad(ivec3 c)
{
    return vec3(pcg3d(uvec3(c)) >> 8u) * (2.0 / 16777215.0) - 1.0;
}

float corner(ivec3 c, vec3 f, ivec3 o) { return dot(lattice_grad(c + o), f - vec3(o)); }

/* Gradient noise, about [-1, 1], on the integer lattice base + floor(p). */
float inoise(ivec3 base, vec3 p)
{
    vec3  fl = floor(p);
    ivec3 c  = base + ivec3(fl);
    vec3  f  = p - fl;
    vec3  u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(mix(corner(c, f, ivec3(0,0,0)), corner(c, f, ivec3(1,0,0)), u.x),
                   mix(corner(c, f, ivec3(0,1,0)), corner(c, f, ivec3(1,1,0)), u.x), u.y),
               mix(mix(corner(c, f, ivec3(0,0,1)), corner(c, f, ivec3(1,0,1)), u.x),
                   mix(corner(c, f, ivec3(0,1,1)), corner(c, f, ivec3(1,1,1)), u.x), u.y),
               u.z) * 1.4;
}

/* Quadratic through a 3×3 stencil f[j][i] (spacing 1) at offset x. */
float quad9(float f[9], vec2 x)
{
    float f0  = f[4];
    float fx  = 0.5 * (f[5] - f[3]),        fy  = 0.5 * (f[7] - f[1]);
    float fxx = f[5] - 2.0 * f0 + f[3],     fyy = f[7] - 2.0 * f0 + f[1];
    float fxy = 0.25 * (f[8] - f[6] - f[2] + f[0]);
    return f0 + fx * x.x + fy * x.y + 0.5 * (fxx * x.x * x.x + fyy * x.y * x.y) + fxy * x.x * x.y;
}

/* Tiles narrower than this take the continent-scale shape from a quadratic
 * fit instead of per texel: a float direction resolves ~1e-7, so across a
 * deep tile (2e-6 wide at the finest level) direct evaluation sees only a
 * dozen distinct points and the relief turns into centimetre stair-steps
 * along the tile axes. At that scale the shape is smooth, and the fit's nine
 * samples sit on exactly representable points. */
#define FIT_BELOW_W 1e-4

void main()
{
    vec2 g  = gl_FragCoord.xy - 1.5;     /* texel 0 is grid -1 */
    vec3 dp = tile_dp(g);
    vec3 P  = tile_dir(g);

    float rough, shape;
    if (u_tile.z < FIT_BELOW_W) {
        float e  = 0.5 * u_tile.z;                     /* dyadic: qc ± e exact */
        vec3  qc = u_face_n + u_tile.x * u_face_u + u_tile.y * u_face_v;
        float fs[9], fr[9];
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                vec3 Ps = normalize(qc + (float(i - 1) * e) * u_face_u
                                       + (float(j - 1) * e) * u_face_v);
                fs[j * 3 + i] = relief_shape(Ps, u_ptype, fr[j * 3 + i]);
            }
        vec2 x = (g / float(TILE_N - 1) - 0.5) * u_tile.z / e;
        shape = quad9(fs, x);
        rough = quad9(fr, x);
    } else {
        shape = relief_shape(P, u_ptype, rough);
    }

    float detail = 0.0;
    for (int k = 0; k < MAX_OCT; k++) {
        if (k >= u_noct) break;
        detail += u_oct_a[k] * inoise(u_oct_i[k], u_oct_r[k] + (u_oct_rot[k] * dp) * u_oct_f[k]);
    }
    o_height = u_amp * (shape + rough * detail);
}
