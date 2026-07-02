#version 330 core
/*
 * galaxy_stars.vert — procedural resolved stars inside a galaxy volume.
 *
 * The §0.1 scale-continuity step between "galaxy as glow" and "star system":
 * when the camera is inside (or entering) a galaxy, this pass scatters point
 * stars whose placement follows the SAME density model as galaxy.frag, so the
 * sparkle appears exactly where the arms/bulge/knots glow — flying toward a
 * spiral arm resolves it into individual stars.
 *
 * Attribute-less: each gl_VertexID maps to one candidate star in a cubic
 * lattice cascade centred on the camera (cells of u_cell_size AU, u_grid_dim
 * per side, GS_PER_CELL candidates per cell). Cell coordinates are absolute
 * integers in the galaxy's lattice (u_cell_base + local), so stars are stable
 * world objects the camera flies past, not screen effects. Several cascades
 * with growing cell size are drawn per frame; each rejects stars inside the
 * next-finer cascade's box (u_inner_half) and fades at its own rim.
 *
 * A candidate becomes a star when a hash beats the local emission density —
 * brighter (rarer) luminosities come from a power-law hash, so distant
 * cascades still contribute a few visible supergiants while near cascades
 * fill in the faint field. Rejected candidates are emitted behind the w=0
 * clip plane and cost nothing.
 */

uniform mat4  u_vp;           /* camera-relative view-projection            */
uniform ivec3 u_cell_base;    /* grid corner, absolute lattice coords       */
uniform vec3  u_origin_rel;   /* camera-relative AU position of that corner */
uniform float u_cell_size;    /* lattice cell edge, AU                      */
uniform int   u_grid_dim;     /* cells per side                             */
uniform float u_inner_half;   /* Chebyshev radius handled by finer cascade  */
uniform float u_outer_half;   /* this cascade's Chebyshev coverage radius   */
uniform vec3  u_cam_in_gal;   /* camera position, unit-galaxy-radius coords */
uniform float u_radius_gal;   /* galaxy bounding radius, AU                 */
uniform vec3  u_axis;         /* disc spin axis (unit)                      */
uniform float u_seed;
uniform int   u_type;         /* 0 spiral, 1 elliptical, 2 irregular        */
uniform float u_time;         /* shear clock — must match galaxy.frag       */
uniform float u_gain;         /* global fade (skybox crossfade), 0..1       */
uniform float u_lum_scale;    /* per-cascade: a coarse cell's candidates
                               * represent the brightest stars of a much
                               * larger volume, so luminosity grows ~ with
                               * cell area — keeps each cascade's apparent
                               * brightness distribution scale-invariant    */

out vec4 v_color;             /* rgb premultiplied-ish, a = coverage        */

#define GS_PER_CELL 5

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm3(vec3 p) {
    float v = vnoise(p) * 0.5;
    p = p * 2.03 + vec3(3.7, 1.9, 2.6);  v += vnoise(p) * 0.25;
    p = p * 2.03 + vec3(1.9, 4.2, 2.1);  v += vnoise(p) * 0.125;
    return v / 0.875;
}

/* Emission density at unit-sphere position p — a reduced port of
 * galaxy.frag's galaxy_sample() (no dust, no colour): the two must stay in
 * step or stars detach from the glow they are supposed to resolve. Also
 * returns the bulge weight and knot strength for the population colour. */
float star_density(vec3 p, float rr, vec3 seedv, out float bulge_w, out float knots)
{
    bulge_w = 0.0;
    knots   = 0.0;

    if (u_type == 1) {                               /* ELLIPTICAL */
        bulge_w = 1.0;
        return exp(-pow(rr / 0.42, 0.62) * 3.2) * 1.5;
    }

    float h  = dot(p, u_axis);
    vec3  pr = p - u_axis * h;
    float r  = length(pr);
    vec3  t1 = normalize(cross(u_axis, vec3(0.31, 1.0, 0.71)));
    vec3  t2 = cross(u_axis, t1);
    float phi = atan(dot(pr, t2), dot(pr, t1));

    if (u_type == 2) {                               /* IRREGULAR */
        float env = exp(-pow(r / 0.62, 2.0) - pow(h / 0.34, 2.0));
        float n   = fbm3(p * 3.2 + seedv);
        float k   = smoothstep(0.42, 0.85, n);
        knots = k;
        return env * (0.20 + 1.6 * k * k) * 1.15;
    }

    /* SPIRAL — same shear/arm/disc/bulge terms as the volume shader. */
    float rot = u_time * 0.010 / max(r, 0.10);
    float ph  = phi + rot;
    float wind = log(max(r, 0.035)) * 3.6;
    float armw = ph * 2.0 - wind;
    float arm  = pow(0.5 + 0.5 * cos(armw), 2.6);

    float disc  = exp(-r / 0.30) * exp(-abs(h) / (0.035 + 0.09 * r * r))
                * smoothstep(1.0, 0.85, rr);
    float bulge = 1.9 * exp(-pow(rr / 0.13, 2.0));

    float cr = cos(rot), sr = sin(rot);
    vec3  prot = pr * cr + cross(u_axis, pr) * sr + u_axis * h;
    float n     = fbm3(prot * 4.6 + seedv);
    float kn    = smoothstep(0.55, 0.88, n) * arm;

    knots   = kn;
    bulge_w = clamp(bulge / max(disc * (0.38 + 2.8 * arm + 3.8 * kn) + bulge, 1e-5),
                    0.0, 1.0);
    return disc * (0.38 + 2.8 * arm + 3.8 * kn) + bulge;
}

void main() {
    v_color      = vec4(0.0);
    gl_PointSize = 0.0;
    gl_Position  = vec4(0.0, 0.0, 2.0, 0.0);        /* rejected: clipped */

    int cid = gl_VertexID / GS_PER_CELL;
    int sub = gl_VertexID - cid * GS_PER_CELL;
    ivec3 lc;
    lc.x = cid % u_grid_dim;
    lc.y = (cid / u_grid_dim) % u_grid_dim;
    lc.z = cid / (u_grid_dim * u_grid_dim);

    /* Stable per-star hashes from the absolute cell + candidate index. */
    vec3 cellf = vec3(u_cell_base + lc);
    vec3 h3    = hash33(cellf + float(sub) * vec3(13.17, 7.71, 3.39)
                              + u_seed * vec3(0.173, 0.317, 0.531));
    float hsel = hash13(cellf * 1.7 + float(sub) * 41.7 + u_seed);
    float hlum = hash13(cellf * 3.1 + float(sub) * 17.3 - u_seed);
    float hcol = hash13(cellf * 5.3 + float(sub) * 29.1 + u_seed * 2.0);

    /* Camera-relative star position (grid corner precomputed in double). */
    vec3 pos = u_origin_rel + (vec3(lc) + h3) * u_cell_size;

    /* Cascade band: leave the interior to the finer cascade, fade the rim. */
    float cheb = max(max(abs(pos.x), abs(pos.y)), abs(pos.z));
    if (cheb < u_inner_half || cheb > u_outer_half) return;
    float rim = 1.0 - smoothstep(u_outer_half * 0.75, u_outer_half, cheb);

    /* Accept against the local emission density. */
    vec3  p  = u_cam_in_gal + pos / u_radius_gal;
    float rr = length(p);
    if (rr > 1.0) return;
    vec3  seedv = vec3(u_seed * 7.0, u_seed * 3.0, -u_seed * 5.0);
    float bulge_w, knots;
    float dens = star_density(p, rr, seedv, bulge_w, knots);
    if (hsel > clamp(dens * 2.4, 0.0, 1.0)) return;

    /* Power-law luminosity: most stars faint, a rare tail of supergiants.
     * Apparent brightness in inverse-square, with luminosity scaled to the
     * cascade cell so far cascades show only their brightest members. */
    float lum  = (0.04 + 260.0 * pow(hlum, 7.0)) * u_lum_scale;
    float dist = max(length(pos), 1.0);
    float dly  = dist / 63241.077;                   /* AU → ly */
    float b    = lum / max(dly * dly, 1e-4);

    float size = clamp(sqrt(b) * 4.0, 0.0, 6.0);
    float a    = clamp(b * 8.0, 0.0, 1.0) * rim * u_gain;
    if (size < 0.35 || a < 0.01) return;
    if (size < 1.0) { a *= size; size = 1.0; }       /* sub-pixel → dimmer */

    /* Population colour: warm in the bulge, blue-white in the arms, with a
     * per-star temperature spread; HII-knot members skew hot blue. */
    vec3 cool = mix(vec3(1.00, 0.82, 0.62), vec3(0.72, 0.80, 1.00), hcol);
    vec3 col  = mix(cool, vec3(1.00, 0.90, 0.72), bulge_w * 0.8);
    col = mix(col, vec3(0.70, 0.78, 1.00), knots * 0.5);

    v_color      = vec4(col, a);
    gl_PointSize = size;
    gl_Position  = u_vp * vec4(pos, 1.0);
}
