#version 330 core
/*
 * ring_sprite_generic.frag - simple procedural ring disc for far-LOD
 *
 * Generic version for faint/narrow rings (Uranus, Neptune).  It shares the
 * same morph uniforms as the particle ring so collision shock motion remains
 * continuous across the LOD switch.
 */

in vec3 v_pos;

uniform vec3  u_center;         /* planet world pos (AU)               */
uniform vec3  u_b1;             /* ring-plane basis 1                  */
uniform vec3  u_b2;             /* ring-plane basis 2                  */
uniform float u_r_inner_km;     /* inner edge (km from planet centre)  */
uniform float u_r_outer_km;     /* outer edge (km)                     */
uniform vec3  u_ring_color;     /* base ring colour                    */
uniform float u_alpha_max;      /* peak opacity                        */
uniform vec4  u_morph0;         /* unused, puff, shock_amp, shock_phase */
uniform vec4  u_morph1;         /* shock_width, shock_spin, inner_km, outer_km */
uniform vec4  u_morph2;         /* contact_norm, contact_width, contact_strength, unused */
uniform vec4  u_tide0;          /* phase, radius_norm, width, strength */

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
    float tide_dphi = abs(wrap_pi(phi - u_tide0.x));
    float tide_ang_width = mix(0.34, 1.35, clamp(u_tide0.z, 0.0, 1.0));
    float tide_ang = 1.0 - smoothstep(tide_ang_width * 0.40, tide_ang_width, tide_dphi);
    float tide_radial = 1.0 - smoothstep(u_tide0.z * 0.38, u_tide0.z, abs(ring_norm - u_tide0.y));
    float tide = tide_ang * tide_radial * u_tide0.w;
    float split_band = max(u_morph2.y * 0.70, 0.12);
    float split_t = smoothstep(-split_band, split_band, ring_norm - u_morph2.x);
    float split_dir = split_t * 2.0 - 1.0;
    float split_fade = 1.0 - smoothstep(0.42, 0.78, u_morph2.y);
    float ring_width_km = max(u_morph1.w - u_morph1.z, 1.0);
    float edge_falloff = smoothstep(0.00, 0.12, ring_norm)
                       * smoothstep(1.00, 0.88, ring_norm);
    float r_offset_km = ring_width_km * edge_falloff
                      * (u_morph0.z * shock * (ring_norm - 0.45) * 0.032
                       + contact * split_dir * split_fade * 0.024
                       + tide * 0.030);
    float r_eff_km = r_km - r_offset_km;

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
