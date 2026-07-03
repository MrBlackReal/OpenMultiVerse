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
 *
 * Auroras (u_aurora.w > 0) are emission accumulated inside the same march —
 * a noise-curtained oval of magnetic latitude around the spin pole, green
 * body / red top / violet fringe, mostly night-side.  Rays that hit the
 * planet face — normally discarded, the sphere pass owns them — take an
 * emission-only march from shell entry to the surface so the oval shows
 * against the night-side disc from orbit.  Strength comes from the CPU's
 * physical proxy (rotation dynamo × RadianceField flux × stellar-wind storm
 * gusting on the sim clock — quiet oval most of the time, occasional storms
 * that also push the oval equatorward), 0 = off and bit-identical to the
 * pre-aurora shader.
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
uniform vec4  u_aurora;          /* xyz = spin axis (world), w = strength
                                    (0 = off, bit-identical to before)    */
uniform vec4  u_aur_shape;       /* x = quiet oval centre (sin mag-lat),
                                    y = quiet gaussian half-width,
                                    z = equatorward shift per strength,
                                    w = widening per strength             */
uniform vec3  u_aur_look;        /* x = emission gain, y = red band gain,
                                    z = violet fringe gain                */
uniform float u_time;            /* seconds — curtain animation           */

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

/* 1-D value noise for the aurora curtains. */
float a_hash(float p) { return fract(sin(p * 127.1) * 43758.5453); }
float a_noise(float p) {
    float i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(a_hash(i), a_hash(i + 1.0), f);
}

/* Auroral emission at a shell sample.  nrm = unit position from the planet
 * centre, h = shell height 0..1, sun_dir = unit toward the sun.  The oval
 * is a ring of magnetic latitude around each spin pole; curtains are
 * longitude noise drifting on u_time; colour runs green (low oxygen line)
 * → red (high), with a violet fringe at the bottom edge; the night side
 * carries most of the glow.  Returns radiance to accumulate. */
vec3 aurora_emission(vec3 nrm, float h, vec3 sun_dir) {
    /* Storms don't just brighten the oval — they push it equatorward and
     * thicken it (Kp expansion), so a big gust visibly changes the shape. */
    float w      = clamp(u_aurora.w, 0.0, 2.5);
    float centre = u_aur_shape.x - u_aur_shape.z * w;
    float width  = u_aur_shape.y + u_aur_shape.w * w;
    float sinlat = dot(nrm, u_aurora.xyz);
    float oval_d = (abs(sinlat) - centre) / width;
    float oval   = exp(-oval_d * oval_d);
    if (oval < 0.003) return vec3(0.0);

    /* Longitude around the spin axis for curtain structure. */
    vec3 e1 = normalize(abs(u_aurora.y) < 0.98
                        ? cross(u_aurora.xyz, vec3(0.0, 1.0, 0.0))
                        : cross(u_aurora.xyz, vec3(1.0, 0.0, 0.0)));
    vec3 e2 = cross(u_aurora.xyz, e1);
    float phi  = atan(dot(nrm, e2), dot(nrm, e1));
    float hemi = sinlat > 0.0 ? 0.0 : 3.7;      /* decouple the two ovals */
    float curt = a_noise(phi * 14.0 + u_time * 0.21 + hemi)
               * (0.35 + 0.65 * a_noise(phi * 47.0 - u_time * 0.53 + hemi))
               * (0.55 + 0.45 * a_noise(phi * 5.0 + u_time * 0.07 + hemi));
    curt = smoothstep(0.12, 0.55, curt);

    /* Altitude bands: violet fringe → green body → red top.  Green must
     * dominate: oblique rays integrate the tall red column far longer than
     * the thin green layer, so the red gain is kept well below what a
     * side-on curtain photo suggests or the whole oval reads pink. */
    float g_band = smoothstep(0.03, 0.10, h) * (1.0 - smoothstep(0.16, 0.45, h));
    float r_band = smoothstep(0.25, 0.50, h) * (1.0 - smoothstep(0.55, 0.85, h));
    float v_band = smoothstep(0.01, 0.04, h) * (1.0 - smoothstep(0.05, 0.10, h));
    vec3 col = vec3(0.15, 1.00, 0.35) * g_band
             + vec3(0.90, 0.18, 0.35) * r_band * u_aur_look.y
             + vec3(0.55, 0.25, 0.95) * v_band * u_aur_look.z;

    /* Aurora exists on the day side too, but sunlight buries it. */
    float night = 0.18 + 0.82 * smoothstep(0.25, -0.30, dot(nrm, sun_dir));

    return col * (oval * curt * night);
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

    /* Rays that hit the planet's front face: the sphere pass owns the surface,
     * so no scattering is added there — but auroral curtains hang ABOVE the
     * surface and must still draw against the disc (the classic orbital view
     * of the oval over the night side).  Those pixels take an emission-only
     * march from shell entry to the surface. */
    bool  aur_only = false;
    float t_end;
    float d_inner = R * R - p2;
    if (d_inner >= 0.0) {
        float t = -b - sqrt(d_inner);
        if (t > 0.0) {
            if (u_aurora.w <= 0.0) discard;
            aur_only = true;
            t_end    = t;
        }
    }

    /* March segment: shell entry (clamped to the camera when inside) to the
     * shell exit — or to the planet surface on the aurora-only path. */
    float t_atm      = -b - sqrt(max(0.0, R_atm * R_atm - p2));
    float t_atm_back = -b + sqrt(max(0.0, R_atm * R_atm - p2));
    if (!aur_only) t_end = t_atm_back;
    float t0  = max(t_atm, 0.0);
    float seg = t_end - t0;
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
    vec3 La = vec3(0.0);
    for (int j = 0; j < N_VIEW; j++) {
        vec3  pos = u_oc + (t0 + (float(j) + jit) * dt) * ray_dir;
        float rr  = length(pos);
        float h   = clamp((rr - R) / H, 0.0, 1.0);
        vec3  s_r = beta_r * exp(-h / H_RAY);
        float s_m = beta_m * exp(-h / H_MIE);
        tau_v += (s_r + vec3(s_m)) * dl;
        vec3 Tv = exp(-tau_v);
        if (!aur_only) {
            vec3 Ts = sun_transmittance(pos, sun_dir, R, R_atm, H,
                                        beta_r, beta_m);
            L += Tv * Ts * (s_r * pr + vec3(s_m * pm)) * dl;
            if (u_light2 > 0.0) {
                vec3 Ts2 = sun_transmittance(pos, sun2_dir, R, R_atm, H,
                                             beta_r, beta_m);
                L2 += Tv * Ts2 * (s_r * pr2 + vec3(s_m * pm2)) * dl;
            }
        }
        /* Auroral curtains: pure emission attenuated by the air in front of
         * it (Tv) — no phase function, it IS the light source. */
        if (u_aurora.w > 0.0)
            La += Tv * aurora_emission(pos / rr, h, sun_dir) * dl;
    }

    vec3 col = L * u_sun_col;
    if (u_light2 > 0.0) col += L2 * u_light2_col * u_light2;
    col *= u_atm_intensity * GAIN;
    col += La * (u_aurora.w * u_aur_look.x);

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
    /* Aurora-only fragments sit against the planet's own surface, which is
     * BEHIND the shell back face in depth terms — use the shell entry instead
     * so the depth test against the sphere passes. */
    float eye_depth  = (aur_only ? t0 : t_atm_back) * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    /* Radiance out, alpha 1: under GL_SRC_ALPHA/GL_ONE this adds L exactly
     * once (the old path multiplied colour by alpha and then blended by it —
     * alpha-squared radiance). */
    frag_color = vec4(col, 1.0);
}
