#version 330 core
/*
 * atm.frag — single-scattering Rayleigh + Mie atmosphere (roadmap §3.1)
 *
 * Carrier and structure are unchanged from the limb-glow version: atm.vert
 * billboard/fullscreen quad, per-pixel ray + shell intersection, planet-face
 * discards, additive blend, back-face log depth.  What changed is the colour
 * model: the art-directed limb/twilight/forward terms are replaced by a real
 * scattering integral — 14 view samples through the shell, each with a
 * 4-sample secondary march toward the sun for transmittance (correct at the
 * terminator for any planet/shell ratio, which is where sunsets live).
 *
 * Units: distances are AU, but scattering path lengths are measured in shell
 * thicknesses (H = R_atm − R), so β coefficients are per-shell-unit and one
 * set of constants serves every planet.  For thin shells on huge planets
 * (gas giants) the tangential path in shell units grows ∝ √(R/H); β is scaled
 * by min(1, 3.3/(R/H)) so the limb optical depth stays in the calibrated
 * range instead of blowing out.
 *
 * Art direction survives as physics inputs: the JSON atmosphere colour tints
 * the Rayleigh β spectrum (Earth's blue → blue sky + red sunsets; a heated
 * collision shell's red → red-scattering glow), u_atm_intensity stays the
 * overall gain, and both RadianceField lights get the full treatment
 * (u_light2 = 0 is bit-identical to the single-sun path).
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
uniform vec3  u_sun_col;         /* primary blackbody tint vs Sol       */
uniform vec3  u_sun2_rel;        /* secondary light − centre (AU)       */
uniform float u_light2;          /* secondary strength vs primary, 0..1 */
uniform vec3  u_light2_col;      /* secondary chromaticity              */
uniform vec3  u_atm_color;
uniform float u_atm_intensity;

out vec4 frag_color;

const float PI_C     = 3.14159265;
const float H_RAY    = 0.12;     /* Rayleigh scale height, shell units  */
const float H_MIE    = 0.06;     /* Mie: half the Rayleigh scale height */
const float MIE_G    = 0.76;     /* Henyey-Greenstein anisotropy        */
const int   N_VIEW   = 14;
const int   N_SUN    = 4;
const float GAIN     = 30.0;     /* calibrated: Earth limb at intensity
                                    0.6 reads like the old art pass     */

float phase_rayleigh(float mu) {
    return (3.0 / (16.0 * PI_C)) * (1.0 + mu * mu);
}

float phase_mie(float mu) {
    float d = 1.0 + MIE_G * MIE_G - 2.0 * MIE_G * mu;
    return (1.0 - MIE_G * MIE_G) / (4.0 * PI_C * d * sqrt(d));
}

/* Optical depth (per-channel) from a shell point toward a light, by a short
 * secondary march to the shell exit.  Returns transmittance; zero when the
 * solid planet blocks the ray (this shadow IS the terminator). */
vec3 sun_transmittance(vec3 pos, vec3 light_dir, float R, float R_atm,
                       float H, vec3 beta_r, float beta_m)
{
    float b  = dot(pos, light_dir);
    float r2 = dot(pos, pos);
    float p2 = r2 - b * b;
    if (b < 0.0 && p2 < R * R) return vec3(0.0);          /* planet shadow */
    float t_exit = -b + sqrt(max(0.0, b * b - r2 + R_atm * R_atm));
    float dt = t_exit / float(N_SUN);
    float dl = dt / H;                                     /* shell units  */
    vec3 tau = vec3(0.0);
    for (int j = 0; j < N_SUN; j++) {
        vec3  sp = pos + (float(j) + 0.5) * dt * light_dir;
        float h  = clamp((length(sp) - R) / H, 0.0, 1.0);
        tau += (beta_r * exp(-h / H_RAY)
                + vec3(beta_m) * exp(-h / H_MIE)) * dl;
    }
    return exp(-tau);
}

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

    /* March segment: shell entry (clamped to the camera when inside) to the
     * shell exit.  The planet-face discard above guarantees the segment never
     * crosses the solid sphere. */
    float t_atm      = -b - sqrt(max(0.0, R_atm * R_atm - p2));
    float t_atm_back = -b + sqrt(max(0.0, R_atm * R_atm - p2));
    float t0  = max(t_atm, 0.0);
    float seg = t_atm_back - t0;
    if (seg <= 0.0) discard;

    float H  = max(R_atm - R, 1e-9);
    float Rn = R / H;                       /* planet radius, shell units */
    float bscale = min(1.0, 3.3 / Rn);      /* thin-shell (gas giant) cap */

    /* Rayleigh spectrum DERIVED from the authored atmosphere colour: β ∝ ac²
     * (max-normalised, so intensity lives in u_atm_intensity and hue in the
     * colour).  Earth's authored blue (0.45, 0.65, 1.0)² × 3.2 = (0.65, 1.35,
     * 3.2) ≈ the physical Rayleigh spectrum, so Earth stays physical — while
     * Mars' rust actually scatters red (with the real-Mars blue terminator),
     * Titan goes orange haze, Uranus cyan.  A fixed blue-heavy β merely
     * tinted by the colour made every planet's shell read the same. */
    vec3 ac = u_atm_color
            / max(max(u_atm_color.r, u_atm_color.g), max(u_atm_color.b, 1e-3));
    vec3  beta_r = 3.2 * ac * ac * bscale;
    float beta_m = 0.25 * bscale;

    vec3 sun_dir = normalize(u_sun_rel);
    float mu  = dot(ray_dir, sun_dir);
    float pr  = phase_rayleigh(mu);
    float pm  = phase_mie(mu);

    vec3 sun2_dir = vec3(0.0);
    float pr2 = 0.0, pm2 = 0.0;
    if (u_light2 > 0.0) {
        sun2_dir = normalize(u_sun2_rel);
        float mu2 = dot(ray_dir, sun2_dir);
        pr2 = phase_rayleigh(mu2);
        pm2 = phase_mie(mu2);
    }

    /* Jitter the sample comb per pixel (same screen hash as the volumetrics)
     * so 14 samples don't band into onion shells. */
    float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))
                      * 43758.5453);

    float dt = seg / float(N_VIEW);
    float dl = dt / H;
    vec3 tau_v = vec3(0.0);
    vec3 L  = vec3(0.0);
    vec3 L2 = vec3(0.0);
    for (int j = 0; j < N_VIEW; j++) {
        vec3  pos = u_oc + (t0 + (float(j) + jit) * dt) * ray_dir;
        float h   = clamp((length(pos) - R) / H, 0.0, 1.0);
        vec3  s_r = beta_r * exp(-h / H_RAY);
        float s_m = beta_m * exp(-h / H_MIE);
        tau_v += (s_r + vec3(s_m)) * dl;
        vec3 Tv = exp(-tau_v);
        vec3 Ts = sun_transmittance(pos, sun_dir, R, R_atm, H, beta_r, beta_m);
        L += Tv * Ts * (s_r * pr + vec3(s_m * pm)) * dl;
        if (u_light2 > 0.0) {
            vec3 Ts2 = sun_transmittance(pos, sun2_dir, R, R_atm, H,
                                         beta_r, beta_m);
            L2 += Tv * Ts2 * (s_r * pr2 + vec3(s_m * pm2)) * dl;
        }
    }

    vec3 col = L * u_sun_col;
    if (u_light2 > 0.0) col += L2 * u_light2_col * u_light2;
    col *= u_atm_intensity * GAIN;

    if (max(col.r, max(col.g, col.b)) < 0.0005) discard;

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
    float eye_depth  = t_atm_back * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    /* Radiance out, alpha 1: under GL_SRC_ALPHA/GL_ONE this adds L exactly
     * once (the old path multiplied colour by alpha and then blended by it —
     * alpha-squared radiance). */
    frag_color = vec4(col, 1.0);
}
