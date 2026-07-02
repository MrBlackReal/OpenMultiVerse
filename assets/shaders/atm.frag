#version 330 core
/*
 * atm.frag — atmospheric limb glow
 *
 * Per-pixel ray–atmosphere shell intersection.
 *
 * Glow model:
 *   p    = closest-approach distance of the ray to the planet centre
 *   norm = (p − R) / (R_atm − R)   maps [R, R_atm] → [0, 1]
 *   glow = (1 − norm)^3            smooth power-law falloff
 *
 * Using a power-law (rather than the chord-length model) gives a
 * continuously smooth gradient that fades to zero at R_atm with no
 * visible hard edge.  The cubic exponent concentrates the bulk of the
 * glow near the planet limb (norm ≈ 0) and tapers gently outward.
 *
 * Lit-side boost: dot(outward_normal, sun_dir) dims the night-side
 * glow to ~15 % of its day-side value.
 *
 * Rendered additively (GL_SRC_ALPHA / GL_ONE); logarithmic
 * gl_FragDepth keeps it correctly depth-sorted against opaque geometry.
 */

uniform vec3  u_oc;              /* cam − centre  (camera-relative, AU) */
uniform float u_planet_radius;   /* inner sphere radius  (AU)           */
uniform float u_radius;          /* outer atmosphere radius (AU)        */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform vec3  u_cam_fwd;
uniform float u_fov_tan;
uniform float u_aspect;
uniform vec2  u_screen;

uniform vec3  u_sun_rel;         /* sun − centre (AU)                   */
uniform vec3  u_sun2_rel;        /* secondary light − centre (AU)       */
uniform float u_light2;          /* secondary strength vs primary, 0..1 */
uniform vec3  u_light2_col;      /* secondary chromaticity              */
uniform vec3  u_atm_color;
uniform float u_atm_intensity;

out vec4 frag_color;

void main() {
    /* Per-pixel ray direction — identical formula to phong.frag */
    vec2 ndc     = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 ray_dir = normalize(u_cam_fwd
                           + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                           + u_cam_up    * (ndc.y * u_fov_tan));

    float b   = dot(u_oc, ray_dir);
    float oc2 = dot(u_oc, u_oc);
    float p2  = max(0.0, oc2 - b * b);  /* squared closest-approach dist */

    float R     = u_planet_radius;
    float R_atm = u_radius;

    /* Discard outside outer atmosphere shell */
    if (p2 > R_atm * R_atm) discard;

    /* Discard where ray hits the planet's front face (sphere renders that) */
    float d_inner = R * R - p2;
    if (d_inner >= 0.0) {
        float t = -b - sqrt(d_inner);
        if (t > 0.0) discard;
    }

    /* Front face of the outer atmosphere sphere */
    float t_atm = -b - sqrt(max(0.0, R_atm * R_atm - p2));
    /* When t_atm <= 0 the camera is already inside the atmosphere — still
     * render glow.  Clamp t_hit to 0 so the lit-side normal is evaluated at
     * the camera position rather than a point behind it. */
    float t_hit = max(t_atm, 0.0);

    /* Smooth power-law falloff.
     * norm = 0 at the planet surface (limb), 1 at the outer atmosphere edge.
     * (1−norm)^3 concentrates the glow at the limb and tapers continuously
     * to zero — no visible hard cutoff at the boundary.
     *
     * When the camera is inside the atmosphere and the ray points outward
     * (b > 0), the geometric closest approach on the positive ray is at t=0
     * (the camera itself), so use oc2 rather than the infinite-line p2. */
    float p    = sqrt(b > 0.0 ? oc2 : p2);
    float norm = clamp((p - R) / (R_atm - R), 0.0, 1.0);
    /* Broad power-law glow plus a tighter term for a brighter limb line. */
    float glow = pow(1.0 - norm, 3.0) + 0.35 * pow(1.0 - norm, 9.0);

    /* Scattering model:
     *   sun_dot  — illumination of the limb point (lit side > 0)
     *   day      — smooth day/night with a soft terminator
     *   twilight — scattering peaks where the sun grazes the limb (sunset band)
     *   forward  — forward-scatter halo when looking toward the star through the
     *              shell, i.e. a bright ring when the star is behind the planet */
    vec3  sun_dir = normalize(u_sun_rel);
    vec3  hit_n   = normalize(u_oc + t_hit * ray_dir);
    float sun_dot = dot(hit_n, sun_dir);

    float day      = clamp(sun_dot * 1.2 + 0.1, 0.0, 1.0);
    float twilight = pow(clamp(1.0 - abs(sun_dot) * 2.2, 0.0, 1.0), 1.5);
    float forward  = pow(clamp(dot(ray_dir, sun_dir), 0.0, 1.0), 6.0);

    /* Cool day tint shifts toward a warm sunset hue through the twilight band. */
    vec3 sunset = mix(u_atm_color, vec3(1.0, 0.5, 0.25), 0.85);
    vec3 col    = mix(u_atm_color, sunset, twilight);

    float lit   = 0.10 + 0.90 * day + 0.70 * forward;

    /* Secondary light: same day/twilight/forward model, weighted by its flux
     * relative to the primary; colours blend by lit share.  u_light2 = 0 →
     * bit-identical single-sun path. */
    if (u_light2 > 0.0) {
        vec3  sun2 = normalize(u_sun2_rel);
        float sd2  = dot(hit_n, sun2);
        float day2 = clamp(sd2 * 1.2 + 0.1, 0.0, 1.0);
        float tw2  = pow(clamp(1.0 - abs(sd2) * 2.2, 0.0, 1.0), 1.5);
        float fw2  = pow(clamp(dot(ray_dir, sun2), 0.0, 1.0), 6.0);
        float lit2 = (0.90 * day2 + 0.70 * fw2) * u_light2;
        vec3  col2 = mix(u_atm_color, sunset, tw2) * u_light2_col;
        col  = (col * lit + col2 * lit2) / max(lit + lit2, 1e-4);
        lit += lit2;
    }

    float alpha = glow * u_atm_intensity * lit;
    if (alpha < 0.003) discard;

    /* Logarithmic depth — use the BACK face of the atmosphere sphere.
     *
     * Using the front face (t_atm) causes the glow of body A to render
     * additively over body B whenever A's atmosphere front face sits in
     * front of B's solid surface — which happens during merges because the
     * target's atmosphere shell extends past the impactor's sphere.  The
     * back face (t_atm_back) is always behind any solid object that sits
     * inside the atmosphere shell, so the depth test correctly rejects the
     * glow at pixels occupied by a closer solid body.                    */
    const float FAR = DEPTH_FAR;
    float t_atm_back = -b + sqrt(max(0.0, R_atm * R_atm - p2));
    float eye_depth  = t_atm_back * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    frag_color = vec4(col * alpha, alpha);
}
