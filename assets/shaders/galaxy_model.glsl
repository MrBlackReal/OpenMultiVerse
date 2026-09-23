/*
 * galaxy_model.glsl — THE galaxy density model, #included by galaxy.frag (the
 * volumetric glow) and galaxy_stars.vert (the resolved stars placed in it).
 *
 * It used to exist three times: here, a "reduced port" in galaxy_stars.vert,
 * and a float-exact CPU port of that port in starsys.c, all kept in step by
 * hand ("MUST stay in sync"). The stars now read the same function the glow
 * does, and the CPU no longer evaluates it at all: starsys.c promotes the
 * candidates the star shader itself accepted, read back by transform
 * feedback (galaxy.c). One definition; nothing left to drift.
 *
 * The including shader supplies nothing but this file's uniforms.
 */
uniform int   u_type;        /* 0 spiral, 1 elliptical, 2 irregular       */
uniform vec3  u_axis;        /* disc spin axis (unit, world frame)        */
uniform float u_time;        /* seconds; drives rotational shear          */
uniform vec3  u_color;       /* stellar-population tint (volume colour)   */

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

float fbm2(vec3 p) {
    float v = vnoise(p) * 0.6;
    p = p * 2.11 + vec3(4.1, 2.3, 3.4);  v += vnoise(p) * 0.3;
    return v / 0.9;
}

/*
 * galaxy_sample — emission colour (premultiplied weight), emission density,
 * and pure absorption (dust) at unit-sphere position p.
 */
void galaxy_sample(vec3 p, float rr, vec3 seedv, float emis_min, float dust_min,
                   out vec3 col, out float dens, out float dust,
                   out float dens_raw, out float bulge_w_raw, out float knots_out)
{
    col  = u_color;
    dens = 0.0;
    dust = 0.0;
    dens_raw = 0.0;
    bulge_w_raw = 0.0;
    knots_out = 0.0;

    /* Lossless early-out: every noise term below is multiplied by a cheap
     * analytic envelope (exp falloffs). Compute the envelope first and, when it
     * cannot possibly clear the caller's contribution threshold, skip the FBM
     * entirely. EMIS_MIN matches main()'s `dens > 0.0015` gate exactly, so a
     * skipped emission sample is one the caller would have discarded anyway —
     * bit-for-bit identical output; DUST_MIN is a conservative absorption floor
     * (dust is a thin midplane sheet, negligible where this bound is tiny).
     * The volume passes 0.0015 / 0.0004; the star pass passes 0 / 0, which
     * disables the skip, because a star's acceptance needs the exact value. */
    float EMIS_MIN = emis_min;
    float DUST_MIN = dust_min;

    if (u_type == 1) {                               /* ELLIPTICAL */
        /* Steep bright core, long faint envelope; old, smooth, warm. */
        float e = exp(-pow(rr / 0.42, 0.62) * 3.2);
        if (e * 1.5 < EMIS_MIN) return;              /* n<=1 -> dens<=e*1.5 */
        float n = fbm2(p * 3.0 + seedv);
        dens = e * (0.85 + 0.15 * n) * 1.5;
        col  = u_color * mix(vec3(0.95, 0.88, 0.74), vec3(1.0, 0.98, 0.9),
                             smoothstep(0.25, 0.0, rr));
        dens_raw = dens;
        bulge_w_raw = 1.0;
        return;
    }

    /* Disc frame: height above the midplane + in-plane radius/azimuth. */
    float h  = dot(p, u_axis);
    vec3  pr = p - u_axis * h;
    float r  = length(pr);
    vec3  t1 = normalize(cross(u_axis, vec3(0.31, 1.0, 0.71)));
    vec3  t2 = cross(u_axis, t1);
    float phi = atan(dot(pr, t2), dot(pr, t1));

    if (u_type == 2) {                               /* IRREGULAR */
        /* In-plane coords for the stellar bar. */
        float x1 = dot(pr, t1), x2 = dot(pr, t2);

        /* Off-centre elongated stellar bar (the LMC's defining feature) —
         * old warm population, slightly displaced from the cloud centre. */
        float bar = 1.5 * exp(-pow((x1 - 0.07) / 0.34, 2.0)
                              - pow( x2         / 0.115, 2.0)
                              - pow( h          / 0.13,  2.0));

        /* Envelope upper bound (lump<=1 gives the widest, slowest falloff):
         * emission <= env*(0.05+1.9+2.6)+bar, dust <= env*0.9. */
        float env_ub = exp(-pow(r / 0.72, 2.2) - pow(h / 0.30, 2.0));
        if (env_ub * 4.55 + bar < EMIS_MIN && env_ub * 0.9 < DUST_MIN) return;

        /* Ragged outline: large-scale noise warps the envelope radius so
         * the cloud reads as a torn lump from outside, not a smooth ball. */
        float lump = fbm2(p * 2.1 + seedv * 1.7);
        float env  = exp(-pow(r / (0.42 + 0.30 * lump), 2.2)
                         - pow(h / 0.30, 2.0));

        /* Patchy young population: low base fill, strong clumps, and rare
         * bright pink HII complexes (30 Doradus-class at the top end). */
        float n   = fbm3(p * 3.2 + seedv);
        float k   = smoothstep(0.48, 0.85, n);
        float hii = smoothstep(0.68, 0.86, fbm2(p * 4.6 - seedv));
        dens = env * (0.05 + 1.9 * k * k + 2.6 * hii) + bar;

        /* Torn dark dust patches for structure, absent from the bar core. */
        dust = smoothstep(0.58, 0.82, fbm2(p * 3.9 + seedv * 2.3))
             * env * (1.0 - clamp(bar, 0.0, 1.0)) * 0.9;

        float bw    = clamp(bar / max(dens, 1e-5), 0.0, 1.0);
        dens_raw = dens;
        bulge_w_raw = bw;
        knots_out = k;
        vec3  young = mix(vec3(0.72, 0.80, 1.00), vec3(1.0, 0.50, 0.55), hii);
        col = u_color * mix(young * (0.55 + 0.9 * k),
                            vec3(1.0, 0.90, 0.72), bw);
        return;
    }

    /* SPIRAL --------------------------------------------------------------
     * Differential rotation: flat rotation curve → ω ∝ 1/r. The whole
     * pattern (arms + clumps + dust) shears with time. */
    float rot = u_time * 0.010 / max(r, 0.10);
    float ph  = phi + rot;

    /* Two logarithmic arms: constant pitch in log-radius. */
    float wind = log(max(r, 0.035)) * 3.6;
    float armw = ph * 2.0 - wind;
    float arm  = pow(0.5 + 0.5 * cos(armw), 2.6);

    /* Thin exponential disc, slightly flaring outward; warm compact bulge. */
    float disc  = exp(-r / 0.30) * exp(-abs(h) / (0.035 + 0.09 * r * r))
                * smoothstep(1.0, 0.85, rr);
    float bulge = 2.4 * exp(-pow(rr / 0.14, 2.0));

    /* Dust-lane vertical/radial envelopes (analytic; reused by the full path
     * below so this costs nothing extra when not skipped). Upper-bound the
     * dust with lane<=1, dn<=1 for the early-out test. */
    float dV1 = exp(-abs(h + 0.028) / 0.030) * exp(-r / 0.40)
              * smoothstep(0.06, 0.18, r);
    float dV2 = exp(-abs(h + 0.008) / 0.016) * exp(-r / 0.45)
              * smoothstep(0.04, 0.12, r);
    float dust_ub = dV1 * 2.2 + dV2 * 1.1;

    /* Lossless skip: disc*9.8 upper-bounds cloud(<=1.4)*(0.38+2.8*arm+3.8*knots)
     * with arm,knots<=1, so disc*9.8+bulge upper-bounds emission. When neither
     * emission nor dust can register, skip all four FBM evaluations. */
    if (disc * 9.8 + bulge < EMIS_MIN && dust_ub < DUST_MIN) return;

    /* Star-forming knots along the arms (noise in the co-rotating frame so
     * clumps ride the shear instead of the arms sweeping through them). */
    float cr = cos(rot), sr = sin(rot);
    vec3  prot = pr * cr + cross(u_axis, pr) * sr + u_axis * h;
    float n     = fbm3(prot * 4.6 + seedv);
    float knots = smoothstep(0.55, 0.88, n) * arm;

    /* Star-cloud mottling: the band seen from inside is patchy star clouds,
     * not an airbrushed gradient. */
    float cloud = 0.60 + 0.80 * fbm2(prot * 3.1 + seedv * 1.3);

    /* The disc needs ~2.4x the naive weight to read against the compact
     * bulge from outside (verified face-on + edge-on on the Milky Way). */
    dens = disc * cloud * (0.38 + 2.8 * arm + 3.8 * knots) + bulge;

    /* Before any dust: where the STARS are. Dust dims them, it does not
     * remove them, so star placement reads these. */
    dens_raw = dens;
    bulge_w_raw = clamp(bulge / max(dens, 1e-5), 0.0, 1.0);
    knots_out = knots;

    /* Dust lanes: absorption on the arms' inner edges, pinned to the
     * midplane, absent from the bulge core. dV1/dV2 (computed above) are the
     * lane-independent vertical/radial envelopes, reused here so a non-skipped
     * sample does identical work to before. Fold in the arm-phase lane and the
     * noise modulation dn.  Great Rift (dV2) is independent of arm phase. */
    float lane = pow(0.5 + 0.5 * cos(armw + 1.1), 3.0);
    float dn   = 0.55 + 0.45 * fbm2(prot * 6.0 - seedv);
    dust = lane * dV1 * dn * 2.2
         +        dV2 * dn * 1.1;

    /* Dust extinguishes the starlight embedded in it too, not just what is
     * behind — this is what carves the classic dark stripe across an
     * edge-on disc (Sombrero) instead of the midplane glowing through. */
    dens *= exp(-dust * 2.4);

    /* Population colours: warm bulge, cool blue-white arms, pink HII knots.
     * The weight uses the *extinguished* bulge — same dust as dens — or the
     * warm tint smears along every dust-dimmed midplane region. */
    float bw   = clamp(bulge * exp(-dust * 2.4) / max(dens, 1e-5), 0.0, 1.0);
    float hii  = smoothstep(0.80, 0.94, n) * arm;
    vec3  dcol = mix(vec3(0.72, 0.80, 1.00), vec3(1.0, 0.55, 0.60), hii);
    col = u_color * mix(dcol, vec3(1.0, 0.83, 0.56), bw);
}

