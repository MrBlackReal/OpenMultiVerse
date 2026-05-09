#version 330 core
/*
 * ring.vert — Keplerian ring particle animation on the GPU
 *
 * Each particle stores its mean anomaly (a_M), semi-major axis (a_a),
 * eccentricity (a_e), argument of periapsis (a_omega), and height (a_height).
 * The CPU advances a_M each physics step by  ΔM = n·dt  (pure addition).
 * The GPU computes the first-order Keplerian solution each frame:
 *
 *   ν ≈ M + 2e·sin(M)          true anomaly  (O(e²) error)
 *   r ≈ a·(1 − e·cos(M))       orbital radius (O(e²) error)
 *
 * This makes inner particles orbit faster than outer ones (Kepler's 3rd law)
 * and gives each particle a slightly elliptical path around Saturn.
 */
layout(location = 0) in float a_M;      /* mean anomaly (rad)              */
layout(location = 1) in float a_a;      /* semi-major axis (AU)            */
layout(location = 2) in float a_e;      /* eccentricity                    */
layout(location = 3) in float a_omega;  /* argument of periapsis (rad)     */
layout(location = 4) in float a_height; /* vertical offset (AU)            */
layout(location = 5) in vec3  a_color;

uniform mat4 u_vp;
uniform vec3 u_center;   /* Saturn world pos (AU) */
uniform vec3 u_b1;       /* ring-plane basis 1 */
uniform vec3 u_b2;       /* ring-plane basis 2 */
uniform vec3 u_pole;     /* ring-plane normal (Saturn pole) */
uniform vec4 u_morph0;   /* scale, puff, shock_amp, shock_phase */
uniform vec4 u_morph1;   /* shock_width, shock_spin, inner_au, outer_au */
uniform vec4 u_morph2;   /* contact_norm, contact_width, contact_strength, unused */
uniform vec4 u_tide0;    /* phase, radius_norm, width, strength */
uniform vec4 u_tide1;    /* local direction to perturber: b1,b2,pole */

out vec4 v_color;

float wrap_pi(float a) {
    const float TWO_PI = 6.28318530718;
    if (a >  3.14159265359) a -= TWO_PI;
    if (a < -3.14159265359) a += TWO_PI;
    return a;
}

void main() {
    /* First-order Keplerian approximation — valid for e < 0.05 */
    float sinM = sin(a_M);
    float cosM = cos(a_M);
    float nu   = a_M + 2.0 * a_e * sinM;    /* true anomaly  */
    float r    = a_a * (1.0 - a_e * cosM);  /* orbital radius */

    float c = cos(nu + a_omega);
    float s = sin(nu + a_omega);
    float phi = nu + a_omega;
    float r_norm = clamp((r - u_morph1.z) / max(u_morph1.w - u_morph1.z, 1e-8), 0.0, 1.0);
    float dphi = abs(wrap_pi(phi - u_morph0.w));
    float shock = 1.0 - smoothstep(u_morph1.x * 0.35, u_morph1.x, dphi);
    float contact_dr = abs(r_norm - u_morph2.x);
    float contact_radial = 1.0 - smoothstep(u_morph2.y * 0.35, u_morph2.y, contact_dr);
    float contact = shock * contact_radial * u_morph2.z;
    float split_dir = (r_norm >= u_morph2.x) ? 1.0 : -1.0;
    float radial_wave = u_morph0.z * shock * (0.55 + 0.70 * r_norm);
    float contact_push = contact * (0.10 + 0.18 * u_morph2.z);
    float height_wave = u_morph0.y * (0.20 + 0.80 * shock) + contact * 1.10;
    float tide_dphi = abs(wrap_pi(phi - u_tide0.x));
    float tide_ang_width = mix(0.34, 1.35, clamp(u_tide0.z, 0.0, 1.0));
    float tide_ang = 1.0 - smoothstep(tide_ang_width * 0.40, tide_ang_width, tide_dphi);
    float tide_dr = abs(r_norm - u_tide0.y);
    float tide_radial = 1.0 - smoothstep(u_tide0.z * 0.38, u_tide0.z, tide_dr);
    float tide = tide_ang * tide_radial * u_tide0.w;
    vec3 tide_dir = normalize(u_tide1.x * u_b1 + u_tide1.y * u_b2 + u_tide1.z * u_pole);
    float ring_width = max(u_morph1.w - u_morph1.z, 1e-8);
    float tide_pull = ring_width * tide * (0.055 + 0.115 * u_tide0.w);

    vec3 pos = u_center
             + (r * u_morph0.x * (1.0 + radial_wave * (r_norm - 0.12) + contact_push * split_dir)) * (c * u_b1 + s * u_b2)
             + (a_height * (1.0 + height_wave) + height_wave * 0.00002 * shock) * u_pole
             + tide_pull * tide_dir;

    v_color     = vec4(a_color, 1.0);
    gl_Position = u_vp * vec4(pos, 1.0);
}
