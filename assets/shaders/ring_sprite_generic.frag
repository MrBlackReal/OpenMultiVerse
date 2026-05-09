#version 330 core
/*
 * ring_sprite_generic.frag - simple procedural ring disc for far-LOD
 *
 * Generic version for faint/narrow rings (Uranus, Neptune).  It shares the
 * same morph uniforms as the particle ring so collision scale and shock
 * motion remain continuous across the LOD switch.
 */

in vec3 v_pos;

uniform vec3  u_center;         /* planet world pos (AU)               */
uniform vec3  u_b1;             /* ring-plane basis 1                  */
uniform vec3  u_b2;             /* ring-plane basis 2                  */
uniform float u_r_inner_km;     /* inner edge (km from planet centre)  */
uniform float u_r_outer_km;     /* outer edge (km)                     */
uniform vec3  u_ring_color;     /* base ring colour                    */
uniform float u_alpha_max;      /* peak opacity                        */
uniform vec4  u_morph0;         /* scale, puff, shock_amp, shock_phase */
uniform vec4  u_morph1;         /* shock_width, shock_spin, inner_km, outer_km */
uniform vec4  u_morph2;         /* contact_norm, contact_width, contact_strength, unused */

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
    float r_eff_km = r_km / max(u_morph0.x * (1.0 + u_morph0.z * shock * 0.50
                                      + contact * split_dir * 0.16), 1e-4);

    if (r_eff_km < u_r_inner_km || r_eff_km > u_r_outer_km) discard;

    float width  = u_r_outer_km - u_r_inner_km;
    float fade_w = width * 0.08;
    float alpha  = smoothstep(u_r_inner_km, u_r_inner_km + fade_w, r_eff_km)
                 * smoothstep(u_r_outer_km, u_r_outer_km - fade_w, r_eff_km);

    alpha *= 1.0 + 0.06 * u_morph0.y * shock;

    const float FAR = 2000.0;
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(FAR + 1.0);

    frag_color = vec4(u_ring_color, u_alpha_max * alpha);
}
