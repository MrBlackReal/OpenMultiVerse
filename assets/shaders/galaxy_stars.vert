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
uniform float u_seed;
uniform float u_gain;         /* global fade, 0..1                         */

/* ── two-tier population ──────────────────────────────────────────────────
 * These stars are the COMPLEMENT of the real catalog, not an alternative to
 * it. Each candidate draws a physical absolute magnitude from the catalog's
 * own measured luminosity function (tools/derive_lf.py -> stellar_lf.h), then
 * is discarded if the catalog could have detected it — because in that case a
 * real, measured star already occupies that part of the sky. What survives is
 * exactly what the survey missed: too faint, or too far, or (once the dust
 * cube lands) too reddened.
 *
 * That replaces the old radius crossfade, which faded procedural stars in by
 * distance from the origin and so had no physical meaning where the two
 * populations overlapped. There is now no radius and no crossfade.
 *
 * u_lf_mag is the luminosity function's inverse CDF indexed by LOG10
 * quantile: u_lf_mag[i] is the absolute magnitude at
 * q = 10^(LF_LOGQ_MIN + i/(LF_TABLE-1) * -LF_LOGQ_MIN). Log spacing matters —
 * a coarse cascade cuts at q_max ~ 1e-7, which a linear table cannot
 * resolve.                                                                 */
#define LF_TABLE 32
#define LF_LOGQ_MIN (-9.0)
uniform float u_lf_mag[LF_TABLE];
uniform float u_mag_limit;    /* catalog detection limit, apparent mag      */
uniform vec3  u_cam_abs;      /* camera absolute position, AU (Sun at 0)    */
uniform float u_q_max;        /* per-cascade quantile cut: a coarse cell
                               * carries only its brightest few members, so
                               * sampling is truncated to the bright end at
                               * the correct SPACE DENSITY rather than
                               * inflating luminosity to fake it            */
uniform ivec4 u_suppress[32]; /* promoted stars (cell.xyz, candidate): these
                               * exist as real bodies right now, so their
                               * point sprites are skipped (finest cascade
                               * only — promotion radius is ~1 ly)          */
uniform int   u_n_suppress;

out vec4 v_color;

/* This shader's own decision about each candidate, for starsys.c (which
 * promotes the drawn stars near the camera to real systems). Read back by
 * transform feedback from a capture-only program built from this same file
 * (galaxy_star_candidates); the drawing program ignores them. w = the
 * candidate index when the star is DRAWN, -1 otherwise. */
flat out ivec4 v_tf_cell;     /* lattice cell xyz, candidate index            */
out vec4       v_tf_star;     /* position hash in the cell (xyz), abs. mag    */             /* rgb premultiplied-ish, a = coverage        */

#define GS_PER_CELL 5

/* GLSL 330 has no log10; same helper star_field.vert uses. */
float log10f(float x) { return log(x) * 0.4342944819032518; }

#include "galaxy_model.glsl"

void main() {
    v_color      = vec4(0.0);
    gl_PointSize = 0.0;
    gl_Position  = vec4(0.0, 0.0, 2.0, 0.0);        /* rejected: clipped */
    v_tf_cell    = ivec4(0, 0, 0, -1);
    v_tf_star    = vec4(0.0);

    int cid = gl_VertexID / GS_PER_CELL;
    int sub = gl_VertexID - cid * GS_PER_CELL;
    ivec3 lc;
    lc.x = cid % u_grid_dim;
    lc.y = (cid / u_grid_dim) % u_grid_dim;
    lc.z = cid / (u_grid_dim * u_grid_dim);

    ivec3 cell = u_cell_base + lc;

    /* Promoted to a real body right now? Then skip the sprite. */
    if (u_inner_half <= 0.0) {
        for (int i = 0; i < u_n_suppress; i++)
            if (all(equal(u_suppress[i].xyz, cell)) && u_suppress[i].w == sub)
                return;
    }

    /* Stable per-star hashes from the absolute cell + candidate index. */
    vec3 cellf = vec3(cell);
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
    float bulge_w, knots, dens;
    {
        /* The volume's own model (galaxy_model.glsl), undusted: dust dims a
         * star, it does not remove it. No early-out (0 / 0 thresholds): the
         * acceptance test below needs the exact density. */
        vec3 col_; float dens_d_, dust_;
        galaxy_sample(p, rr, seedv, 0.0, 0.0, col_, dens_d_, dust_,
                      dens, bulge_w, knots);
    }
    if (hsel > clamp(dens * 2.4, 0.0, 1.0)) return;

    /* Absolute magnitude from the catalog's own luminosity function. The
     * hash is remapped into [0, u_q_max] so a coarse cascade draws only from
     * the bright tail — its cell volume holds far more stars than the
     * GS_PER_CELL candidates it can emit, so it must represent the brightest
     * of them at true space density. */
    float q  = max(hlum * u_q_max, 1e-9);
    float t  = clamp((log10f(q) - LF_LOGQ_MIN) / (-LF_LOGQ_MIN), 0.0, 1.0);
    float fi = t * float(LF_TABLE - 1);
    int   i0 = int(fi);
    int   i1 = min(i0 + 1, LF_TABLE - 1);
    float absmag = mix(u_lf_mag[i0], u_lf_mag[i1], fi - float(i0));

    const float AU_PER_PC = 206264.806;

    /* SELECTION FUNCTION — measured from the Sun, not the camera, because
     * that is where the survey observed from. If the catalog could have seen
     * this star, a real one is already there and this candidate must not be
     * drawn, or the sky is double-populated. */
    vec3  from_sun = pos + u_cam_abs;
    float d_sun_pc = max(length(from_sun) / AU_PER_PC, 1e-6);
    float m_sun    = absmag + 5.0 * log10f(d_sun_pc) - 5.0;
    /* Dust can only make it fainter, so it is only worth integrating for a
     * candidate the dust-free test would reject: it may have been hidden from
     * the survey behind a cloud, and then it belongs here. The Sun is at
     * u_dust_origin in this camera-relative frame. */
    if (m_sun < u_mag_limit)
        m_sun += DUST_AG_PER_AV * dust_av(u_dust_origin, pos);
    if (m_sun < u_mag_limit) return;

    /* Apparent magnitude at the CAMERA drives what we draw. Size and HDR gain
     * are star_field.vert's curves verbatim, so a procedural star and a
     * catalog star of equal apparent magnitude render identically — the two
     * tiers have to be one population on screen. */
    float d_cam_pc = max(length(pos) / AU_PER_PC, 1e-6);
    float av_cam   = dust_av(vec3(0.0), pos);
    float m_cam    = absmag + 5.0 * log10f(d_cam_pc) - 5.0 + DUST_AG_PER_AV * av_cam;

    float size = clamp(7.0 - 0.45 * (m_cam + 1.0), 1.4, 7.0);
    float hdr  = (m_cam < 2.5) ? min(6.0, pow(10.0, 0.28 * (2.5 - m_cam))) : 1.0;
    float a    = clamp(pow(10.0, -0.4 * (m_cam - STAR_FADE_MAG0)), 0.0, 1.0) * rim * u_gain;
    float b    = hdr;
    if (a < STAR_FADE_FLOOR) return;

    /* Population colour: warm in the bulge, blue-white in the arms, with a
     * per-star temperature spread; HII-knot members skew hot blue. */
    vec3 cool = mix(vec3(1.00, 0.82, 0.62), vec3(0.72, 0.80, 1.00), hcol);
    vec3 col  = mix(cool, vec3(1.00, 0.90, 0.72), bulge_w * 0.8);
    col = mix(col, vec3(0.70, 0.78, 1.00), knots * 0.5);

    /* HDR lift for the rare bright members — the bloom pass blazes them in
     * their own colour. star_field.vert applies exactly this gain to catalog
     * stars (col = a_color.rgb * gain), so the two tiers overbright alike. */
    col *= b * dust_redden(av_cam);

    v_color      = vec4(col, a);
    v_tf_cell    = ivec4(cell, sub);
    v_tf_star    = vec4(h3, absmag);
    gl_PointSize = size;
    gl_Position  = u_vp * vec4(pos, 1.0);
}
