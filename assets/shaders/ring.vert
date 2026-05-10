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
 * Point size stays at the legacy 1 px until close range, then grows smoothly.
 */
layout(location = 0) in float a_M;      /* mean anomaly (rad)              */
layout(location = 1) in float a_a;      /* semi-major axis (AU)            */
layout(location = 2) in float a_e;      /* eccentricity                    */
layout(location = 3) in float a_omega;  /* argument of periapsis (rad)     */
layout(location = 4) in float a_height; /* vertical offset (AU)            */
layout(location = 5) in vec3  a_color;

uniform mat4 u_vp;
uniform vec3 u_center;   /* camera-relative ring centre (AU) */
uniform vec3 u_b1;       /* ring-plane basis 1 */
uniform vec3 u_b2;       /* ring-plane basis 2 */
uniform vec3 u_pole;     /* ring-plane normal (Saturn pole) */
uniform vec4 u_morph0;   /* alpha, puff, shock_amp, shock_phase */
uniform vec4 u_morph1;   /* shock_width, shock_spin, inner_au, outer_au */
uniform vec4 u_morph2;   /* contact_norm, contact_width, contact_strength, unused */
uniform vec4 u_tide0;    /* phase, radius_norm, width, strength */
uniform vec4 u_tide1;    /* local direction to perturber: b1,b2,pole */
uniform vec4 u_body0;    /* perturber local center xyz (AU), visual radius (AU) */
uniform vec4 u_body1;    /* perturber visibility strength, parent visual radius (AU), unused */
uniform vec4 u_light0;   /* sun direction from parent xyz, ambient light */
uniform float u_shadow_strength; /* 0 = skip planet shadow, 1 = full shadow */

out vec4 v_color;

float wrap_pi(float a) {
    const float TWO_PI = 6.28318530718;
    if (a >  3.14159265359) a -= TWO_PI;
    if (a < -3.14159265359) a += TWO_PI;
    return a;
}

void main() {
    if (a_a <= 0.0) {
        v_color = vec4(0.0);
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    /* Expand compact CPU particle state into orbit position on the GPU. */
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
    float split_band = max(u_morph2.y * 0.70, 0.12);
    float split_t = smoothstep(-split_band, split_band, r_norm - u_morph2.x);
    float split_dir = split_t * 2.0 - 1.0;
    float split_fade = 1.0 - smoothstep(0.42, 0.78, u_morph2.y);
    float radial_wave = u_morph0.z * shock;
    float contact_push = contact * split_fade * split_dir;
    float height_wave = u_morph0.y * (0.20 + 0.80 * shock) + contact * 1.10;
    float tide_dphi = abs(wrap_pi(phi - u_tide0.x));
    float tide_ang_width = mix(0.34, 1.35, clamp(u_tide0.z, 0.0, 1.0));
    float tide_ang = 1.0 - smoothstep(tide_ang_width * 0.40, tide_ang_width, tide_dphi);
    float tide_dr = abs(r_norm - u_tide0.y);
    float tide_radial = 1.0 - smoothstep(u_tide0.z * 0.38, u_tide0.z, tide_dr);
    float tide = tide_ang * tide_radial * u_tide0.w;
    vec3 tide_dir_local = vec3(u_tide1.x, u_tide1.y, u_tide1.z);
    float tide_dir_len = max(length(tide_dir_local), 1e-5);
    vec3 tide_dir = (tide_dir_local.x * u_b1 + tide_dir_local.y * u_b2 + tide_dir_local.z * u_pole) / tide_dir_len;
    float ring_width = max(u_morph1.w - u_morph1.z, 1e-8);
    float tide_pull = ring_width * tide * (0.028 + 0.060 * u_tide0.w);
    /*
     * Collision warp is an additive ring-width offset, not a radius scale.
     * Fade it near edges so displaced particles never pile onto a hard clamp.
     */
    float edge_falloff = smoothstep(0.00, 0.12, r_norm)
                       * smoothstep(1.00, 0.88, r_norm);
    float radial_offset = ring_width * edge_falloff
                        * (radial_wave * (r_norm - 0.45) * 0.045
                         + contact_push * 0.032);
    float ring_radius = r + radial_offset;
    vec3 ring_dir = c * u_b1 + s * u_b2;
    vec3 height_offset = (a_height * (1.0 + height_wave) + height_wave * 0.00002 * shock) * u_pole;
    vec3 local_orbit = ring_radius * ring_dir + height_offset;
    vec3 body_center = u_body0.x * u_b1 + u_body0.y * u_b2 + u_body0.z * u_pole;

    /*
     * Differential visual gravity.
     *
     * Pull the particle and the parent separately, then subtract.  This reads
     * like a tidal field instead of translating the whole ring toward the
     * perturber.
     */
    vec3 to_body = body_center - local_orbit;
    float soft = max(u_body0.w * 0.70, ring_width * 0.055);
    float inv_particle = inversesqrt(dot(to_body, to_body) + soft * soft);
    float inv_parent = inversesqrt(dot(body_center, body_center) + soft * soft);
    vec3 particle_pull = to_body * inv_particle * inv_particle * inv_particle;
    vec3 parent_pull = body_center * inv_parent * inv_parent * inv_parent;
    vec3 tidal_field = (particle_pull - parent_pull) * u_body0.w * u_body0.w;
    float field_len = length(tidal_field);
    vec3 field_dir = field_len > 1e-8 ? tidal_field / field_len : tide_dir;
    float field_mag = field_len / (0.62 + field_len);
    float body_power = clamp(u_body1.x, 0.0, 1.0);
    float grav_pull = ring_width * field_mag * clamp(u_body1.x, 0.0, 1.0)
                    * (0.070 + 0.210 * body_power);
    vec3 shear_dir = normalize(-s * u_b1 + c * u_b2 + u_pole * u_tide1.z * 0.35);
    float shear_phase = sin(tide_dphi) * sign(wrap_pi(phi - u_tide0.x));
    float shear_pull = ring_width * tide * body_power * shear_phase * 0.055;

    vec3 pos = u_center
             + local_orbit
             + tide_pull * tide_dir
             + grav_pull * field_dir
             + shear_pull * shear_dir;

    vec3 local_pos = pos - u_center;
    vec3 parent_delta = local_pos;
    float parent_dist = length(parent_delta);
    float parent_hide = min(u_body1.y * 1.03, u_morph1.w * 1.12);
    float parent_fade = 1.0;
    if (parent_hide > 0.0) {
        /*
         * Do not push swallowed particles onto a hard safety radius.  Fading
         * them avoids artificial dense rings along the merged planet surface.
         */
        float fade_width = max(ring_width * 0.10, u_body1.y * 0.035);
        parent_fade = smoothstep(parent_hide, parent_hide + fade_width, parent_dist);
        if (parent_fade <= 0.001) {
            v_color = vec4(0.0);
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }

    vec3 body_delta = local_pos - body_center;
    float body_dist = length(body_delta);
    float body_hide = u_body0.w * 0.995;
    if (u_body1.x > 0.001 && body_hide > 0.0 && body_dist < body_hide) {
        v_color = vec4(0.0);
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    float dist = length(pos);
    float point_t = 1.0 - smoothstep(0.0006, 0.0040, dist);
    float incidence = abs(dot(u_pole, u_light0.xyz));
    float diffuse = mix(u_light0.w, 1.0, incidence);
    float shadow = 1.0;
    if (u_shadow_strength > 0.001) {
        float sun_side = dot(local_pos, u_light0.xyz);
        vec3 shadow_axis = local_pos - u_light0.xyz * sun_side;
        float shadow_d2 = dot(shadow_axis, shadow_axis);
        float parent_r = u_body1.y;
        float shadow_inner = parent_r * 0.92;
        float shadow_outer = parent_r * 1.24;
        float planet_shadow = sun_side < 0.0
                            ? smoothstep(shadow_inner * shadow_inner,
                                         shadow_outer * shadow_outer,
                                         shadow_d2)
                            : 1.0;
        shadow = mix(1.0, planet_shadow, u_shadow_strength);
    }
    float light = mix(u_light0.w * 0.55, diffuse, shadow);
    float alpha = clamp(u_morph0.x, 0.0, 1.0) * parent_fade;
    v_color     = vec4(a_color * light, alpha);
    gl_PointSize = mix(1.0, 2.1, point_t);
    gl_Position = u_vp * vec4(pos, 1.0);
}
