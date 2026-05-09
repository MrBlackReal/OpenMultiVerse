#version 330 core
/*
 * ring_sprite.frag - procedural ring disc for far-LOD
 *
 * Discards pixels outside the ring annulus; colours remaining pixels
 * according to the four ring zones (C, B, Cassini gap, A).  The optional
 * morph uniforms mirror ring.vert so distant rings keep the same collision
 * scale and shock shape as the particle LOD.
 */
in vec3 v_pos;

uniform vec3 u_center;   /* Saturn world pos (AU) */
uniform vec3 u_b1;
uniform vec3 u_b2;
uniform vec4 u_morph0;   /* scale, puff, shock_amp, shock_phase */
uniform vec4 u_morph1;   /* shock_width, shock_spin, inner_km, outer_km */
uniform vec4 u_morph2;   /* contact_norm, contact_width, contact_strength, unused */

out vec4 frag_color;

float wrap_pi(float a) {
    const float TWO_PI = 6.28318530718;
    if (a >  3.14159265359) a -= TWO_PI;
    if (a < -3.14159265359) a += TWO_PI;
    return a;
}

void main() {
    vec3  rel  = v_pos - u_center;
    float pu   = dot(rel, u_b1);
    float pv   = dot(rel, u_b2);
    float phi  = atan(pv, pu);
    float r_km = sqrt(pu*pu + pv*pv) * 149600000.0;
    float dphi = abs(wrap_pi(phi - u_morph0.w));
    float shock = 1.0 - smoothstep(u_morph1.x * 0.35, u_morph1.x, dphi);
    float ring_norm = clamp((r_km - u_morph1.z) / max(u_morph1.w - u_morph1.z, 1.0), 0.0, 1.0);
    float contact_dr = abs(ring_norm - u_morph2.x);
    float contact = shock
                  * (1.0 - smoothstep(u_morph2.y * 0.35, u_morph2.y, contact_dr))
                  * u_morph2.z;
    float split_dir = (ring_norm >= u_morph2.x) ? 1.0 : -1.0;
    float r_eff_km = r_km / max(u_morph0.x * (1.0 + u_morph0.z * shock * 0.55
                                      + contact * split_dir * 0.18), 1e-4);

    const float C_IN   =  74658.0;
    const float C_OUT  =  92000.0;
    const float B_OUT  = 117580.0;
    const float CASS   = 122170.0;
    const float A_OUT  = 136775.0;

    if (r_eff_km < C_IN || r_eff_km > A_OUT) discard;

    vec3  col;
    float alpha;

    if (r_eff_km < C_OUT) {
        float t = (r_eff_km - C_IN) / (C_OUT - C_IN);
        col   = vec3(0.60, 0.56, 0.50);
        alpha = 0.28 * t;
    } else if (r_eff_km < B_OUT) {
        float t = (r_eff_km - C_OUT) / (B_OUT - C_OUT);
        float env = smoothstep(0.0, 0.12, t) * smoothstep(1.0, 0.88, t);
        col   = vec3(0.88, 0.82, 0.68);
        alpha = 0.55 + 0.30 * env;
    } else if (r_eff_km < CASS) {
        col   = vec3(0.35, 0.33, 0.30);
        alpha = 0.04;
    } else {
        float t = 1.0 - (r_eff_km - CASS) / (A_OUT - CASS);
        col   = vec3(0.78, 0.73, 0.60);
        alpha = 0.48 * t;
    }

    alpha *= 1.0 + 0.08 * u_morph0.y * shock;

    const float FAR = 2000.0;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    frag_color = vec4(col, alpha);
}
