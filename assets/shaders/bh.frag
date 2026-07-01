#version 330 core
/*
 * bh.frag — raymarched black hole (Schwarzschild null geodesics).
 *
 * Each fragment seeds a view ray (normalize(v_world); camera at origin in
 * camera-relative space) and integrates it through the hole's curved spacetime
 * using the standard photon-orbit approximation
 *     d²p/dλ² = -1.5 · h² · p / |p|⁵      (units where the horizon Rs = 1),
 * with h² = |p × d|² the (conserved) specific angular momentum.  This bends
 * light around the hole, so:
 *   - rays that fall inside the horizon are swallowed (black shadow),
 *   - rays grazing the photon sphere (~1.5 Rs) light up as the photon ring,
 *   - the accretion disk — a real annulus in the hole's equatorial plane — is
 *     sampled wherever the bent ray crosses it, so the far side lenses up over
 *     the top into the Einstein arc and the near side sweeps under the front,
 *   - escaped rays pick up a faint lensed star field (procedural), fading to
 *     transparent where the ray is undeflected so the real background shows.
 *
 * The result is view-correct: orbit the hole and the disk tilts from face-on to
 * edge-on for real, instead of the old camera-locked billboard squash.
 *
 * Alpha-blended (GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA); HDR-bright so bloom
 * (post pass #2) turns the disk and ring into glow.
 */
in  vec2 v_uv;
in  vec3 v_world;
out vec4 frag_color;

uniform vec3  u_color;        /* accretion-disk base (warm) colour      */
uniform vec3  u_center;       /* black-hole centre, camera-relative AU  */
uniform float u_radius;       /* event-horizon radius, AU (= Rs)        */
uniform vec3  u_disk_normal;  /* disk spin axis, unit, world space      */
uniform float u_time;         /* seconds, for disk rotation             */
uniform float u_activity;     /* AGN activity: 0 quiet .. 1+ blazing quasar */
uniform float u_spin;         /* spin sense (+1 / -1) for frame dragging */
uniform float u_disk;         /* accretion-disk strength (0 = bare hole)   */
uniform float u_disk_in;      /* inner disk edge (ISCO) in Rs, from spin   */
uniform float u_disk_temp;    /* disk hotness 0..1 (from mass + accretion) */
uniform float u_disk_rate;    /* visual Keplerian swirl rate (from mass)   */
uniform mat4  u_vp;           /* view-projection (camera-relative) — for true depth */

/* Disk geometry, in horizon-radius (Rs) units.  The inner edge is the ISCO,
 * supplied per-hole (u_disk_in) since it depends on spin (3 Rs at a*=0 down to
 * ~0.5 Rs for a near-maximal Kerr hole). */
const float DISK_OUT = 6.5;
const int   STEPS    = 160;
const float BOUND    = 9.0;   /* march only within this radius (Rs) of the hole */

float hash13(vec3 p) {
    p  = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

/* Sparse procedural stars in a direction — used for the lensed background. */
float stars(vec3 dir) {
    vec3  g = dir * 70.0;
    vec3  c = floor(g);
    float h = hash13(c);
    float s = smoothstep(0.992, 1.0, h);
    vec3  f = fract(g) - 0.5;
    return s * exp(-dot(f, f) * 20.0) * 1.4;
}

/* Value noise + fbm over the disk plane, for turbulent filaments. */
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(vec3(i, 0.0));
    float b = hash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = hash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = hash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}

void main() {
    float Rs = u_radius;
    vec3  rd = normalize(v_world);            /* view ray (camera at origin)     */

    /* Work in the hole frame, scaled to Rs units (horizon at |p| = 1). */
    vec3 p = (vec3(0.0) - u_center) / Rs;
    vec3 d = rd;

    /* Skip the straight run-up: jump to where the ray enters the bounding
     * sphere around the hole (outside it, spacetime is ~flat here). */
    float tca = -dot(p, d);
    float b2  = dot(p, p) - tca * tca;        /* impact parameter²               */
    if (b2 > BOUND * BOUND) discard;                             /* misses: bg  */
    float thc = sqrt(BOUND * BOUND - b2);
    p += d * max(tca - thc, 0.0);

    /* Per-pixel start jitter: phase-shifts the march so the higher-order photon
     * images inside the shadow dither into fine noise instead of hard concentric
     * bands (stable per pixel — no temporal shimmer). */
    p += d * (hash13(vec3(gl_FragCoord.xy, 0.0)) - 0.5) * 0.08;

    float h2 = dot(cross(p, d), cross(p, d)); /* conserved angular momentum²     */

    /* Orthonormal basis in the disk plane for the azimuthal angle. */
    vec3 n   = normalize(u_disk_normal);
    vec3 ref = abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t1  = normalize(cross(n, ref));
    vec3 t2  = cross(n, t1);

    /* An active quasar disk reaches farther out and blazes brighter. */
    float act      = clamp(u_activity, 0.0, 2.0);
    float disk_out = mix(DISK_OUT, 8.0, clamp(act, 0.0, 1.0));

    vec3  disk_col   = vec3(0.0);
    float disk_a     = 0.0;
    bool  swallowed  = false;
    float min_r      = 1e9;                   /* closest approach, for the ring  */
    vec3  p_min      = p;                     /* position at closest approach    */
    bool  hit        = false;                 /* did the ray meet an opaque surf */
    vec3  hit_p      = vec3(0.0);             /* hole-frame hit pos (Rs units)   */

    for (int i = 0; i < STEPS; i++) {
        /* Adaptive step: fine near the hole (where the ray curves hard and the
         * higher-order photon images live), coarse far out. */
        float r  = length(p);
        float dl = clamp(0.12 * (r - 1.0), 0.010, 0.40);

        vec3 acc = -1.5 * h2 * p / pow(dot(p, p), 2.5);
        d += acc * dl;
        vec3 pn = p + d * dl;

        if (length(pn) < min_r) { min_r = length(pn); p_min = pn; }

        /* Disk-plane crossing between p and pn → sample the annulus.  Each
         * crossing is composited front-to-back (not stopped at the first), so a
         * faint near-disk crossing lets the bright *secondary lensed image* of
         * the far disk — the arc that wraps over/under the shadow — show through
         * instead of being occluded. A dense crossing still saturates and stops.
         * Rays that fall into the horizon break below, so nothing genuinely
         * behind the shadow is painted. */
        float s0 = dot(p, n);
        float s1 = dot(pn, n);
        if (u_disk > 0.0 && s0 * s1 < 0.0) {
            vec3  pc  = mix(p, pn, s0 / (s0 - s1));
            float rdk = length(pc - dot(pc, n) * n);   /* disk-plane radius (Rs)  */
            if (rdk > u_disk_in && rdk < disk_out) {
                float tt   = (rdk - u_disk_in) / (disk_out - u_disk_in);
                float prof = pow(1.0 - tt, 1.7);        /* hot inner               */

                /* Differential (Keplerian) rotation: rotate the sample point into
                 * the orbiting material's frame by ω(r)·t, ω ∝ r^-1.5, so the
                 * turbulence winds into trailing spirals and actually moves —
                 * inner fast, outer slow. */
                vec2  pl    = vec2(dot(pc, t1), dot(pc, t2));   /* disk-plane pos  */
                float omega = u_disk_rate * pow(rdk, -1.5);
                float a     = -omega * u_time;
                float ca = cos(a), sa = sin(a);
                vec2  q     = vec2(ca * pl.x - sa * pl.y, sa * pl.x + ca * pl.y);

                /* Turbulent density: broad clumps × finer filaments, plus fine
                 * radial striations advecting inward for a sense of speed. */
                float turb = fbm(q * 1.3);
                float fine = fbm(q * 4.2 + turb);
                float dens = mix(0.30, 1.30, turb) * (0.55 + 0.55 * fine);
                dens *= 0.80 + 0.20 * sin(rdk * 8.0 - omega * u_time * 0.6);
                /* Coherent trailing spiral arms sweeping at the local Keplerian
                 * rate: gives a clear sense of orbital rotation (inner arms lead
                 * / wind up faster) rather than just fine-filament shimmer. */
                float th  = atan(pl.y, pl.x);
                float arm = 0.5 + 0.5 * sin(2.0 * th + 6.0 * log(rdk) - omega * u_time);
                dens *= 0.68 + 0.42 * arm;
                float disk = prof * dens;
                disk *= 1.0 + 1.3 * smoothstep(0.16, 0.0, tt);  /* bright inner rim */

                /* Relativistic Doppler beaming: prograde orbital velocity (spin
                 * sense from u_spin); the side sweeping toward us beams brighter
                 * and bluer. */
                vec3  rhat = normalize(pc - dot(pc, n) * n);
                vec3  vel  = u_spin * normalize(cross(n, rhat));
                /* Beaming ∝ Doppler factor: sharpen so the approaching limb
                 * clearly blazes and the receding one darkens (the iconic
                 * Interstellar asymmetry). Orbital speed rises toward the ISCO. */
                float approach = 0.5 - 0.5 * dot(vel, normalize(d));
                float vfac     = 0.6 + 0.4 * (1.0 - tt);   /* faster inner disk   */
                float beam     = pow(clamp(approach, 0.0, 1.0), 0.7);
                beam = mix(0.25, 1.35, beam * vfac);

                /* Blackbody temperature ramp by radius, its overall hotness set
                 * per-hole from mass + accretion (u_disk_temp, Shakura-Sunyaev:
                 * T ∝ (Ṁ/M²)^¼ → low-mass holes run blue-white hot, supermassive
                 * ones cool to orange-red). The bright inner saturates to white
                 * as a real disk does; colour reads through the mid/outer disk. */
                float T = clamp(u_disk_temp, 0.0, 1.0);
                vec3 t_in  = mix(vec3(1.00, 0.95, 0.85), vec3(0.72, 0.83, 1.00), T);
                vec3 t_mid = mix(vec3(1.00, 0.60, 0.30), vec3(0.90, 0.95, 1.00), T);
                vec3 t_out = mix(vec3(0.85, 0.26, 0.10), vec3(0.70, 0.80, 1.00), T);
                vec3 tcol  = (tt < 0.4) ? mix(t_in, t_mid, tt / 0.4)
                                        : mix(t_mid, t_out, (tt - 0.4) / 0.6);
                /* Only the strongly approaching limb picks up a subtle blue. */
                tcol = mix(tcol, vec3(0.90, 0.95, 1.0),
                           clamp((beam - 0.82) * 1.6, 0.0, 0.30));
                /* Subtle per-hole tint from u_color (warms/cools the body). */
                tcol *= mix(vec3(1.0), u_color * 1.4 + 0.35, 0.15);

                float bright     = (1.9 + 1.4 * act) * beam;
                vec3  layer_col  = tcol * disk * bright;
                float layer_a    = clamp(disk * 1.6, 0.0, 1.0);

                /* Composite this crossing over what's already accumulated. */
                disk_col += (1.0 - disk_a) * layer_col;
                if (!hit && layer_a > 0.3) { hit = true; hit_p = pc; } /* depth surf */
                disk_a   += (1.0 - disk_a) * layer_a;
                if (disk_a > 0.98) break;               /* opaque enough: stop     */
            }
        }

        p = pn;
        if (dot(p, p) < 1.0)  { swallowed = true; if (!hit) { hit = true; hit_p = p; } break; }  /* fell in */
        if (length(p) > BOUND) break;                        /* escaped, ~straight */
    }

    /* Photon ring removed: the bright grazing-light ring around the shadow (and
     * the bloom halo it produced) was disabled by request. The shadow now reads
     * as a clean dark disk; the accretion disk (when present) still lenses over
     * the top. min_r/p_min tracking above is now unused but left in place. */

    vec3  col   = disk_col;
    float alpha = disk_a;

    if (swallowed) {
        alpha = 1.0;                          /* opaque black shadow (disk over) */
    } else {
        /* Lensed star background for escaped rays; fade in with deflection so
         * undeflected rays stay transparent and the real starfield shows. */
        float defl = acos(clamp(dot(normalize(d), rd), -1.0, 1.0));
        float w    = smoothstep(0.04, 0.40, defl);
        float st   = stars(normalize(d)) * w;
        col   += vec3(st) * (1.0 - alpha);
        alpha  = max(alpha, st);
    }

    if (alpha < 0.004) discard;

    /* True per-fragment depth from the 3D hit, but only where the surface is
     * genuinely opaque — the event horizon, or the dense body of the disk. This
     * lets the shadow/disk sort correctly against the torus, jets and orbiting
     * bodies in real 3D, without the faint lensed disk rim writing an opaque
     * depth that would punch black holes in the torus dust behind it. hit_p is
     * hole-frame Rs units; world_camrel = u_center + hit_p·Rs (matches bh.vert). */
    if (swallowed || (hit && disk_a > 0.5)) {
        vec4 clip = u_vp * vec4(u_center + hit_p * Rs, 1.0);
        gl_FragDepth = 0.5 + 0.5 * clip.z / clip.w;
    } else {
        gl_FragDepth = 1.0;                 /* transparent: never occlude behind */
    }
    frag_color = vec4(col, alpha);
}
