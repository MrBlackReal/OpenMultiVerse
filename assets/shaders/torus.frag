#version 330 core
/*
 * torus.frag — AGN obscuring dust torus (the doughnut around the accretion
 * disk, from the quasar unified model).
 *
 * A volumetric torus in the hole's equatorial plane, raymarched front-to-back
 * with Beer-Lambert absorption: dusty and dark on the outside, glowing warm on
 * the inner rim (lit by the accretion disk), thick enough that edge-on it
 * obscures the central engine (the "radio galaxy" view) while pole-on you look
 * straight through the hole at the bright core (the "quasar" view).
 *
 * Alpha-over blended, drawn after the disk/jets.  The doughnut hole aligns with
 * the core, so the torus only covers the core when the line of sight passes
 * through the tube (edge-on) — no explicit depth sorting needed.
 */
in  vec3 v_world;
out vec4 frag_color;

uniform vec3  u_center;    /* torus centre, camera-relative AU   */
uniform float u_rs;        /* horizon radius in AU (Rs unit)     */
uniform vec3  u_normal;    /* torus plane normal (spin axis)     */
uniform float u_rmaj;      /* major radius, Rs                   */
uniform float u_rmin;      /* tube radius, Rs                    */
uniform vec3  u_color;     /* dust base colour                   */
uniform float u_time;
uniform float u_rate;      /* signed azimuthal angular speed, rad/s (spin) */
uniform mat4  u_vp;        /* view-projection (camera-relative) — for true depth */

float hash13(vec3 p) {
    p  = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float vnoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0,0,0)), n100 = hash13(i + vec3(1,0,0));
    float n010 = hash13(i + vec3(0,1,0)), n110 = hash13(i + vec3(1,1,0));
    float n001 = hash13(i + vec3(0,0,1)), n101 = hash13(i + vec3(1,0,1));
    float n011 = hash13(i + vec3(0,1,1)), n111 = hash13(i + vec3(1,1,1));
    return mix(mix(mix(n000,n100,f.x), mix(n010,n110,f.x), f.y),
               mix(mix(n001,n101,f.x), mix(n011,n111,f.x), f.y), f.z);
}
float fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 2; i++) { s += a * vnoise(p); p *= 2.05; a *= 0.5; }
    return s;
}

void main() {
    vec3  rd = normalize(v_world);
    vec3  p  = (vec3(0.0) - u_center) / u_rs;      /* Rs units, hole frame */
    vec3  d  = rd;

    float bound = u_rmaj + u_rmin + 1.0;
    float tca = -dot(p, d);
    float b2  = dot(p, p) - tca * tca;
    if (b2 > bound * bound) discard;
    float thc = sqrt(bound * bound - b2);
    float t0  = max(tca - thc, 0.0);
    float t1  = tca + thc;

    vec3 n   = normalize(u_normal);
    vec3 ref = abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 e1  = normalize(cross(n, ref));
    vec3 e2  = cross(n, e1);

    const int STEPS = 16;
    float dt    = (t1 - t0) / float(STEPS);
    float trans = 1.0;
    vec3  col   = vec3(0.0);
    bool  thit  = false;              /* first half-opaque dust hit, for depth */
    vec3  thit_p = vec3(0.0);

    for (int i = 0; i < STEPS; i++) {
        vec3  q       = p + d * (t0 + (float(i) + 0.5) * dt);
        float axial   = dot(q, n);
        vec3  qp      = q - axial * n;
        float inplane = length(qp);
        float rr      = inplane - u_rmaj;               /* in-plane tube offset */
        float dtube   = length(vec2(rr, axial));        /* dist to tube centre  */
        if (dtube >= u_rmin) continue;

        /* Azimuth, rotated by the torus's angular speed so the dust clumps
         * orbit the axis over time (the whole doughnut turns; it sits at large
         * radius so this is slow, matching the disk's spin sense). */
        float az   = atan(dot(qp, e2), dot(qp, e1)) - u_rate * u_time;
        /* Feathered tube edge so the silhouette isn't a hard doughnut. */
        float base = smoothstep(u_rmin, u_rmin * 0.28, dtube);
        /* Ragged, cloudy dust: two turbulence scales carve gaps and filaments
         * so it reads as clumpy gas, not a solid CG doughnut. */
        float turb = fbm(vec3(az * 3.0, axial * 1.4 + u_time * 0.04, inplane * 0.9));
        float fine = fbm(vec3(az * 8.5, axial * 3.4, inplane * 2.1) + turb * 1.5);
        float dens = base * smoothstep(0.17, 0.92, mix(turb, fine, 0.5));

        /* Inner rim glows (lit by the accretion disk), warm→hot toward the hole;
         * dense outer dust reddens (extinction). */
        float glow = smoothstep(u_rmaj + u_rmin * 0.8, u_rmaj - u_rmin * 0.7, inplane);
        glow = glow * glow;
        vec3  c_dust = u_color;
        vec3  c_rim  = vec3(1.0, 0.70, 0.40);
        vec3  c_deep = vec3(0.26, 0.11, 0.07);              /* reddened shadow    */
        vec3  emit = mix(mix(c_deep, c_dust, 0.6), c_rim, glow) * (0.16 + 2.4 * glow);

        float a = clamp(dens * 1.1, 0.0, 1.0);
        col   += emit * a * trans;
        trans *= 1.0 - a;
        if (!thit && (1.0 - trans) > 0.5) { thit = true; thit_p = q; }
        if (trans < 0.05) break;
    }

    float alpha = 1.0 - trans;
    if (alpha < 0.004) discard;

    /* True per-fragment depth at the first half-opaque dust sample, so the torus
     * sorts against the disk/shadow/jets in real 3D (no coplanar-billboard
     * z-fighting). thit_p is hole-frame Rs units; world = u_center + thit_p·u_rs. */
    if (thit) {
        vec4 clip = u_vp * vec4(u_center + thit_p * u_rs, 1.0);
        gl_FragDepth = 0.5 + 0.5 * clip.z / clip.w;
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }
    frag_color = vec4(col, alpha);
}
