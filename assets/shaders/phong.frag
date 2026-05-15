#version 330 core
/*
 * phong.frag — per-pixel ray-sphere intersection + procedural surface texturing
 *
 * Texturing uses 3D value-noise FBM keyed on the surface normal rotated into
 * the body's local frame.  3D noise is seam-free by construction — no UV
 * atlas, no texture files required.
 *
 * u_planet_type selects the colour recipe:
 *   0  generic rocky   (modulates u_color with noise)
 *   1  Earth           (ocean / land / desert / polar ice)
 *   2  Mars            (red-orange with polar frost)
 *   3  Venus           (swirly yellow-white clouds)
 *   4  Jupiter         (orange-brown horizontal bands)
 *   5  Saturn          (tan-cream horizontal bands)
 *   6  ice giant       (smooth pale tint with faint bands)
 *   7  Io              (yellow-orange with dark volcanic spots)
 *   8  Titan           (orange haze)
 *   9  Europa          (icy white with rust cracks)
 *   10 build rocky     (grey-brown terrestrial)
 *   11 build gas giant (soft cloudy beige-brown giant)
 *   12 build ice       (icy blue giant)
 */

in vec2 v_uv;   /* unused for planets, kept for linker compatibility */

uniform mat4  u_vp;
uniform vec3  u_color;
uniform float u_emission;
uniform float u_ambient;
uniform vec3  u_sun_pos_world;
uniform vec3  u_center;
uniform float u_radius;
uniform vec3  u_oc;
uniform vec3  u_sun_rel;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform vec3  u_cam_fwd;
uniform float u_fov_tan;
uniform float u_aspect;
uniform vec2  u_screen;

uniform float u_rotation;        /* body rotation angle, radians           */
uniform float u_cloud_rotation;  /* continuous cloud angle, never wrapped  */
uniform float u_obliquity;    /* axial tilt, radians (0 = upright)      */
uniform int   u_planet_type;  /* 0-9, selects colour recipe             */
uniform float u_star_heat;    /* 0..1 surface heating from star approach */
uniform int   u_impact_count;
uniform vec3  u_impact_dir[32];
uniform vec3  u_impact_tangent1[32];
uniform float u_impact_radius[32];
uniform float u_impact_heat[32];
uniform float u_impact_progress[32];
uniform float u_impact_seed[32];
uniform int   u_impact_kind[32];

out vec4 frag_color;

/* ======================================================================
 * 3-D value noise — no seams, rotation-aware
 * ====================================================================== */

float hash3(vec3 p) {
    p  = fract(p * vec3(127.1, 311.7, 74.7));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);          /* smooth-step */
    return mix(
        mix(mix(hash3(i),               hash3(i + vec3(1,0,0)), f.x),
            mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), f.x),
            mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), f.x), f.y),
        f.z);
}

/* 2-octave FBM — deliberately blurry (rough and washy, as requested) */
float fbm(vec3 p) {
    return vnoise(p)               * 0.65
         + vnoise(p * 2.1 + vec3(7.3, 2.1, 5.8)) * 0.35;
}

vec3 lava_color(float heat)
{
    heat = clamp(heat, 0.0, 1.0);

    if (heat < 0.33) {
        float t = heat / 0.33;
        return mix(vec3(0.10, 0.025, 0.02), vec3(0.42, 0.06, 0.02), t);
    }
    if (heat < 0.72) {
        float t = (heat - 0.33) / 0.39;
        return mix(vec3(0.42, 0.06, 0.02), vec3(0.88, 0.30, 0.06), t);
    }
    if (heat < 0.92) {
        float t = (heat - 0.72) / 0.20;
        return mix(vec3(0.88, 0.30, 0.06), vec3(0.98, 0.58, 0.14), t);
    }

    return mix(vec3(0.98, 0.58, 0.14), vec3(1.00, 0.80, 0.34), (heat - 0.92) / 0.08);
}

vec3 local_surface_dir_to_world(vec3 local_dir)
{
    float cr = cos(u_rotation);
    float sr = sin(u_rotation);
    float tx = local_dir.x * cr - local_dir.z * sr;
    float ty = local_dir.y;
    float tz = local_dir.x * sr + local_dir.z * cr;

    float co = cos(u_obliquity);
    float so = sin(u_obliquity);
    return normalize(vec3(co * tx + so * ty,
                          -so * tx + co * ty,
                          tz));
}

/* ======================================================================
 * Per-planet surface colour  (NL = normal in body-local frame)
 * ====================================================================== */

vec3 surface_color(vec3 NL) {
    float n = fbm(NL * 3.5);

    /* ---- Earth -------------------------------------------------------- */
    if (u_planet_type == 1) {
        float n2  = fbm(NL * 3.5 + vec3(5.3, 2.7, 8.1));
        float lat = abs(NL.y);
        vec3 col;
        if (n < 0.44)
            col = mix(vec3(0.04, 0.13, 0.52), vec3(0.08, 0.28, 0.68),
                      n / 0.44);
        else if (n < 0.68)
            col = mix(vec3(0.22, 0.46, 0.14), vec3(0.58, 0.46, 0.22),
                      (n - 0.44) / 0.24);
        else
            col = mix(vec3(0.58, 0.46, 0.22), vec3(0.70, 0.65, 0.50),
                      (n - 0.68) / 0.32);
        /* polar ice caps */
        col = mix(col, vec3(0.88, 0.91, 0.96),
                  smoothstep(0.82, 0.97, lat));
        return col;
    }

    /* ---- Mars --------------------------------------------------------- */
    if (u_planet_type == 2) {
        vec3 col = mix(vec3(0.38, 0.16, 0.06), vec3(0.86, 0.34, 0.13), n);
        col = mix(col, vec3(0.84, 0.81, 0.76),
                  smoothstep(0.88, 0.97, abs(NL.y)));
        return col;
    }

    /* ---- Venus -------------------------------------------------------- */
    if (u_planet_type == 3) {
        float n2 = fbm(NL * 4.5 + vec3(3.7, 1.2, 8.4));
        return mix(vec3(0.74, 0.66, 0.36), vec3(0.96, 0.90, 0.60),
                   n * 0.5 + n2 * 0.5);
    }

    /* ---- Jupiter ------------------------------------------------------ */
    if (u_planet_type == 4) {
        float band = sin(NL.y * 18.0 + (n - 0.5) * 3.2) * 0.5 + 0.5;
        return mix(vec3(0.48, 0.32, 0.14), vec3(0.94, 0.80, 0.54),
                   band * 0.65 + n * 0.35);
    }

    /* ---- Saturn ------------------------------------------------------- */
    if (u_planet_type == 5) {
        float band = sin(NL.y * 11.0 + (n - 0.5) * 2.2) * 0.5 + 0.5;
        return mix(vec3(0.58, 0.48, 0.24), vec3(0.98, 0.92, 0.64),
                   band * 0.65 + n * 0.35);
    }

    /* ---- Ice giant (Uranus / Neptune) --------------------------------- */
    if (u_planet_type == 6) {
        float band = sin(NL.y * 5.0 + (n - 0.5)) * 0.07 + 0.93;
        return u_color * (0.72 + 0.56 * n) * band;
    }

    /* ---- Build Rocky Planet ------------------------------------------- */
    if (u_planet_type == 10) {
        float n2 = fbm(NL * 6.2 + vec3(3.2, 5.4, 1.7));
        vec3 dark = u_color * vec3(0.62, 0.60, 0.58);
        vec3 mid  = u_color * vec3(0.95, 0.92, 0.88);
        vec3 bright = u_color * vec3(1.18, 1.12, 1.04);
        vec3 col = mix(dark, mid, n * 0.75 + n2 * 0.25);
        col = mix(col, bright, smoothstep(0.70, 0.90, n2) * 0.35);
        return col;
    }

    /* ---- Build Gas Giant ---------------------------------------------- */
    if (u_planet_type == 11) {
        float n2 = fbm(NL * 3.2 + vec3(4.4, 1.6, 7.2));
        float n3 = fbm(NL * 6.8 + vec3(1.8, 6.2, 2.7));
        float cloud = sin(NL.y * 11.5 + (n - 0.5) * 1.8 + (n2 - 0.5) * 1.0) * 0.5 + 0.5;
        float haze  = sin(NL.y * 5.2 + (n3 - 0.5) * 1.4) * 0.5 + 0.5;
        vec3 deep = u_color * vec3(0.78, 0.64, 0.48);
        vec3 light = u_color * vec3(1.12, 1.04, 0.92);
        vec3 warm = u_color * vec3(0.96, 0.80, 0.60);
        vec3 col = mix(deep, light, cloud * 0.46 + haze * 0.22 + n * 0.18);
        col = mix(col, warm, smoothstep(0.70, 0.90, n3) * 0.18);
        return col;
    }

    /* ---- Build Ice Planet --------------------------------------------- */
    if (u_planet_type == 12) {
        float n2 = fbm(NL * 5.5 + vec3(2.6, 7.4, 1.3));
        float bands = sin(NL.y * 8.0 + (n2 - 0.5) * 2.8) * 0.5 + 0.5;
        vec3 deep = u_color * vec3(0.72, 0.88, 1.05);
        vec3 light = u_color * vec3(1.04, 1.08, 1.10);
        vec3 frost = vec3(0.92, 0.97, 1.0);
        vec3 col = mix(deep, light, n * 0.45 + bands * 0.35);
        col = mix(col, frost, smoothstep(0.78, 0.92, n2) * 0.26);
        return col;
    }

    /* ---- Io ----------------------------------------------------------- */
    if (u_planet_type == 7) {
        float n2 = fbm(NL * 8.5 + vec3(5.1, 3.2, 7.4));
        vec3 col;
        if (n < 0.5)
            col = mix(vec3(0.88, 0.80, 0.22), vec3(0.84, 0.38, 0.06), n * 2.0);
        else
            col = mix(vec3(0.88, 0.80, 0.22), vec3(0.93, 0.90, 0.84),
                      (n - 0.5) * 2.0);
        /* dark volcanic patches */
        col = mix(col, vec3(0.10, 0.07, 0.03),
                  smoothstep(0.72, 0.84, n2) * 0.75);
        return col;
    }

    /* ---- Titan -------------------------------------------------------- */
    if (u_planet_type == 8) {
        float n2 = fbm(NL * 2.2 + vec3(4.2, 1.8, 3.3));
        return mix(vec3(0.50, 0.28, 0.06), vec3(0.84, 0.62, 0.26),
                   n2 * 0.6 + 0.2);
    }

    /* ---- Europa ------------------------------------------------------- */
    if (u_planet_type == 9) {
        float n2 = fbm(NL * 9.0 + vec3(2.2, 6.6, 1.1));
        vec3 col = mix(vec3(0.84, 0.82, 0.79), vec3(0.65, 0.56, 0.40),
                       n * 0.25);
        /* rust-brown cracks */
        col = mix(col, vec3(0.44, 0.28, 0.16),
                  smoothstep(0.60, 0.72, n2) * 0.55);
        return col;
    }

    /* ---- type 0: generic rocky --------------------------------------- */
    return mix(u_color * 0.55, u_color * 1.28, n);
}

/* ====================================================================== */

void main() {
    /* ---- per-pixel ray ------------------------------------------------ */
    vec2 ndc    = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 ray_dir = normalize(u_cam_fwd
                           + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                           + u_cam_up    * (ndc.y * u_fov_tan));

    /* ---- ray-sphere intersection --------------------------------------- */
    float b_   = dot(u_oc, ray_dir);
    float c_   = dot(u_oc, u_oc) - u_radius * u_radius;
    float disc = b_ * b_ - c_;
    if (disc < 0.0) discard;

    float t = -b_ - sqrt(disc);
    if (t < 0.0) t = -b_ + sqrt(disc);
    if (t < 0.0) discard;

    vec3 hit_rel = u_oc + t * ray_dir;
    vec3 N       = normalize(hit_rel);

    /* ---- logarithmic depth — consistent with solid.frag / color.frag
     * All shaders must use the same depth metric: view-direction depth
     * (eye_depth = distance along the view axis, NOT Euclidean distance).
     * Using raw t (Euclidean) causes t > eye_depth off-centre, making the
     * planet appear deeper in the log buffer than rings/trails, which use
     * 1/gl_FragCoord.w = eye_depth.  The mismatch only shows when the body
     * is not centred on screen (rings render in front of the sphere limb).
     *
     * eye_depth = t * dot(ray_dir, u_cam_fwd)  [= t * cos(θ) off-axis]
     * This equals 1/gl_FragCoord.w for all non-raycast geometry.         */
    const float FAR  = 2000.0;
    float eye_depth  = t * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    /* ---- emissive (stars) --------------------------------------------- */
    if (u_emission > 0.5) {
        float mu     = max(dot(N, -ray_dir), 0.0);
        float one_mu = 1.0 - mu;
        float limb   = 1.0 - 0.38 * one_mu - 0.31 * one_mu * one_mu;
        vec3 white   = vec3(1.0, 0.98, 0.95);
        vec3 disc    = mix(white, u_color, 0.15 * one_mu);
        frag_color   = vec4(disc * limb, 1.0);
        return;
    }

    /* ---- procedural surface texture ----------------------------------- */
    /* Rotate N into body-local frame.
     * The spin axis is tilted from ecliptic-north (Y) toward +X by the
     * body's obliquity: axis = (sin(obl), cos(obl), 0).
     * Apply the inverse body rotation (Rodrigues, angle = -u_rotation).
     * When u_obliquity == 0 this reduces to a plain Y-axis rotation.    */
    vec3 NL;
    {
        /* Step 1 — undo axial tilt: rotate N around Z by +obliquity.
         * This maps the tilted spin axis back onto the Y axis so the
         * subsequent Y-rotation is a simple spin around the new Y.     */
        float co = cos(u_obliquity);
        float so = sin(u_obliquity);
        float tx =  co * N.x - so * N.y;
        float ty =  so * N.x + co * N.y;
        float tz =  N.z;

        /* Step 2 — undo body spin: plain Y-axis rotation (same convention
         * as the original no-tilt code).                                */
        float cr = cos(-u_rotation);
        float sr = sin(-u_rotation);
        NL = vec3(tx * cr - tz * sr,
                  ty,
                  tx * sr + tz * cr);
    }

    vec3 base_surface = surface_color(NL);
    vec3 surface = base_surface;
    vec3 shade_N = N;
    vec3 lava_emit = vec3(0.0);
    float cloud_mask = 0.0;
    float impact_light_block = 0.0;

    /* ---- Cloud layer (Earth only) --------------------------------------- */
    if (u_planet_type == 1) {
        /* Clouds rotate slightly faster than the surface. */
        vec3 NL_cloud;
        {
            float co = cos(u_obliquity);
            float so = sin(u_obliquity);
            float tx =  co * N.x - so * N.y;
            float ty =  so * N.x + co * N.y;
            float tz =  N.z;
            float cloud_rot = u_cloud_rotation;
            float cr = cos(-cloud_rot);
            float sr = sin(-cloud_rot);
            NL_cloud = vec3(tx * cr - tz * sr, ty, tx * sr + tz * cr);
        }

        /* Domain warping: warp the sample position with a flow field
         * before sampling cloud density — creates swirling spiral shapes. */
        vec3 warp_q = NL_cloud * 3.2 + vec3(1.7, 9.2, 3.4);
        vec3 warp = vec3(
            fbm(warp_q),
            fbm(warp_q + vec3(5.2, 1.3, 8.1)),
            fbm(warp_q + vec3(2.8, 6.7, 0.5))
        );
        vec3 warped = NL_cloud * 4.2 + warp * 0.55 + vec3(4.1, 2.3, 6.8);
        float cn1 = fbm(warped);
        float cn2 = fbm(warped * 2.1 + vec3(3.3, 7.6, 1.9));
        float cloud_val = cn1 * 0.70 + cn2 * 0.30;
        cloud_mask = smoothstep(0.48, 0.66, cloud_val);
    }

    /* Sun direction — computed once, reused for emission clamping inside the
     * impact loop (kind 4 dampens edge/fringe emission on the lit hemisphere
     * to avoid a transparent-looking orange halo when viewed from the sun). */
    vec3  L    = normalize(u_sun_rel - hit_rel);
    float sun_dot = dot(N, L);
    float diff = max(dot(N, L), 0.0);

    if (u_star_heat > 0.001) {
        float star_heat = clamp(u_star_heat, 0.0, 1.0);
        float day_side = smoothstep(-0.12, 0.42, sun_dot);
        float hot_side = pow(max(sun_dot, 0.0), 0.72);
        float view_mu = max(dot(N, -ray_dir), 0.0);
        float rim = pow(1.0 - view_mu, 2.4);
        float heat_noise = fbm(NL * 7.4 + vec3(u_rotation * 0.35, star_heat * 4.0, -u_rotation * 0.22));
        float heat_noise2 = fbm(NL * 13.0 + vec3(2.7, -u_rotation * 0.18, 4.1 + star_heat * 3.3));
        float crack_mix = heat_noise * 0.64 + heat_noise2 * 0.36;
        float warm_band = day_side * (0.32 + 0.68 * star_heat);
        float crust = smoothstep(0.28 - star_heat * 0.10,
                                 0.96 - star_heat * 0.18,
                                 crack_mix + hot_side * (0.16 + star_heat * 0.26));
        float molten_gate = smoothstep(0.46, 0.98, star_heat);
        float molten = crust * hot_side * molten_gate;
        float lava_pool = smoothstep(0.62, 1.0, star_heat)
                        * pow(max(sun_dot, 0.0), 1.35)
                        * smoothstep(0.50, 0.92, crack_mix + star_heat * 0.22);
        float haze = smoothstep(0.36, 1.0, star_heat) * day_side * rim;
        vec3 haze_col = mix(vec3(0.55, 0.08, 0.03),
                            vec3(0.92, 0.18, 0.06),
                            0.35 + star_heat * 0.45);
        vec3 scorch = mix(base_surface * vec3(0.42, 0.24, 0.14),
                          base_surface * vec3(0.86, 0.38, 0.16),
                          0.35 + star_heat * 0.45);
        vec3 crust_col = mix(scorch, lava_color(0.22 + star_heat * 0.38), 0.24 + crust * 0.34);
        vec3 molten_col = lava_color(clamp(0.34 + star_heat * 0.58 + crack_mix * 0.16, 0.0, 1.0));
        vec3 white_hot = mix(molten_col, vec3(1.0, 0.84, 0.42), smoothstep(0.82, 1.0, star_heat) * hot_side);

        surface = mix(surface, scorch, warm_band * 0.42);
        surface = mix(surface, crust_col, warm_band * (0.22 + star_heat * 0.44));
        surface = mix(surface, molten_col, molten * (0.26 + star_heat * 0.48));
        surface = mix(surface, white_hot, lava_pool * 0.55);
        lava_emit += haze_col * haze * (0.18 + star_heat * 0.34);
        lava_emit += molten_col * molten * (0.22 + star_heat * 1.05);
        lava_emit += white_hot * lava_pool * (0.40 + star_heat * 1.45);
    }

    /* Pre-pass: accumulate kind-4 (intersection) coverage so craters (kind 5)
     * don't overwrite the live collision bloom regardless of slot order. */
    float s_intersection = 0.0;
    for (int i = 0; i < 32; i++) {
        if (i >= u_impact_count) break;
        if (u_impact_kind[i] != 4) continue;
        vec3 jdir = normalize(u_impact_dir[i]);
        float jrad = max(u_impact_radius[i], 0.001);
        float jang = acos(clamp(dot(NL, jdir), -1.0, 1.0));
        s_intersection = max(s_intersection,
                             1.0 - smoothstep(jrad * 0.80, jrad * 1.55, jang));
    }

    for (int i = 0; i < 32; i++) {
        if (i >= u_impact_count) break;
        vec3 idir = normalize(u_impact_dir[i]);
        vec3 it1 = normalize(u_impact_tangent1[i]);
        float ang = acos(clamp(dot(NL, idir), -1.0, 1.0));
        float rad = max(u_impact_radius[i], 0.001);
        float heat = clamp(u_impact_heat[i], 0.0, 1.0);
        float progress = clamp(u_impact_progress[i], 0.0, 1.0);
        float seed = u_impact_seed[i];
        int kind = u_impact_kind[i];
        float core = 1.0 - smoothstep(0.0, rad * 0.30, ang);
        float melt = 1.0 - smoothstep(rad * 0.10, rad * 0.98, ang);
        float ring = smoothstep(rad * 0.48, rad * 0.72, ang)
                   * (1.0 - smoothstep(rad * 0.72, rad * 1.02, ang));
        float outer = smoothstep(rad * 0.72, rad * 1.30, ang)
                    * (1.0 - smoothstep(rad * 1.30, rad * 1.85, ang));
        impact_light_block = max(impact_light_block,
                                 1.0 - smoothstep(rad * 0.80, rad * 1.18, ang));
        float lava_noise = fbm(NL * 10.5 + idir * 4.0 + vec3(progress * 5.0, progress * 2.2, progress * 3.7));
        float lava_var = (lava_noise - 0.5) * 0.22;
        vec3 lava_hot = lava_color(clamp(min(1.0, heat * 1.02 + 0.06) + lava_var * 0.55, 0.0, 1.0));
        vec3 lava_molten = lava_color(clamp(min(1.0, heat * 0.82 + 0.08) + lava_var * 0.45, 0.0, 1.0));
        vec3 lava_warm = lava_color(clamp(heat * 0.58 + lava_var * 0.30, 0.0, 1.0));
        vec3 lava_deep = lava_color(clamp(heat * 0.34 + lava_var * 0.18, 0.0, 1.0));
        vec3 lava = mix(lava_warm, lava_hot, core * 0.75 + (1.0 - progress) * 0.25);

        if (kind == 1) {
            float crater_floor = 1.0 - smoothstep(0.0, rad * 0.72, ang);
            float hot_rim = ring * (1.0 - progress * 0.65);
            float glow_cover = heat * (0.42 + 0.28 * (1.0 - progress));
            surface = mix(surface, surface * 0.42, crater_floor * heat * 0.22);
            surface = mix(surface, mix(lava_deep, lava_molten, core), core * glow_cover);
            surface = mix(surface, vec3(0.08, 0.05, 0.04), ring * heat * 0.14);
            lava_emit += lava_molten * (core * 1.5 + hot_rim * 0.9) * heat;
        } else if (kind == 2) {
            float ash = outer * 0.35;
            float glow_cover = heat * (0.50 + 0.28 * (1.0 - progress));
            surface = mix(surface, surface * 0.34, ring * heat * 0.24 + ash * 0.45);
            surface = mix(surface, mix(lava_deep, lava_hot, core), melt * glow_cover);
            lava_emit += lava * (core * 1.8 + melt * 0.9 + outer * 0.35) * heat;
        } else if (kind == 3) {
            float flood = 1.0 - smoothstep(0.0, rad * 1.08, ang);
            float seam = ring * (0.55 + 0.35 * (1.0 - progress));
            surface = mix(surface, surface * 0.20, seam * 0.55);
            surface = mix(surface, mix(lava_deep, lava_molten, flood), flood * (0.45 + heat * 0.55));
            lava_emit += mix(lava_warm, lava_molten, flood) *
                         (flood * 1.75 + seam * 0.65 + outer * 0.25) * heat;
        } else if (kind == 4) {
            /* Live sphere-sphere intersection boundary.
             * rad = angular radius of the buried cap on this sphere.
             * Render the cap as molten lava with a bright ring at the edge. */
            vec3 scar_t1 = it1;
            vec3 scar_t2 = normalize(cross(idir, scar_t1));
            scar_t1 = normalize(cross(scar_t2, idir));
            vec3 scar_p = vec3(dot(NL, scar_t1),
                               dot(NL, scar_t2),
                               dot(NL, idir) - cos(rad));
            float scar_scale = 8.0 / max(rad, 0.08);
            vec3 scar_q = vec3(scar_p.xy * (scar_scale * 1.18),
                               scar_p.z * 4.8);
            float seam_noise = fbm(scar_q);
            float seam_var = (seam_noise - 0.5) * 0.22;
            float seam_coarse = vnoise(scar_q * 0.58 + vec3(3.1, 1.7, 4.9));
            float seam_coarse_var = (seam_coarse - 0.5) * 0.26;
            float cool_phase = smoothstep(0.34, 0.88, progress);
            float inside_cap  = 1.0 - smoothstep(rad * 0.80, rad * 1.10, ang);
            float edge_ring   = smoothstep(rad * 0.55, rad * 0.85, ang)
                              * (1.0 - smoothstep(rad * 0.85, rad * 1.55, ang));
            float outer_fringe = smoothstep(rad * 0.98, rad * 1.30, ang)
                               * (1.0 - smoothstep(rad * 1.30, rad * 1.95, ang));
            vec3 seam_hot = lava_color(clamp(min(1.0, heat * 1.02 + 0.06) + seam_var * 0.55, 0.0, 1.0));
            vec3 seam_molten = lava_color(clamp(min(1.0, heat * 0.82 + 0.08)
                                                + seam_var * 0.34
                                                + seam_coarse_var * (0.10 + 0.18 * cool_phase), 0.0, 1.0));
            vec3 seam_warm = lava_color(clamp(heat * 0.58 + seam_var * 0.18 + seam_coarse_var * 0.28, 0.0, 1.0));
            vec3 seam_deep = lava_color(clamp(heat * 0.34 + seam_var * 0.08 + seam_coarse_var * 0.24, 0.0, 1.0));
            vec3 seam_core = mix(seam_molten, seam_hot, inside_cap * 0.75 + (1.0 - progress) * 0.25);
            seam_core = mix(seam_core, mix(seam_deep, seam_molten, 0.62 + seam_coarse * 0.18),
                            cool_phase * (1.0 - inside_cap * 0.55));

            surface = mix(surface, mix(seam_deep, seam_core, inside_cap * 0.7),
                          inside_cap * heat * 0.88);
            lava_emit += mix(seam_warm, seam_molten, inside_cap) * inside_cap * heat * 1.65;
            /* Dampen edge/fringe glow on the lit side: viewing from the sun
             * direction the ring at ang≈90° (the equatorial limb) would
             * otherwise appear as a bright orange halo that looks transparent.
             * Keep full intensity on the night side (diff≈0).              */
            float night_blend = 1.0 - diff * 0.80;
            lava_emit += seam_molten * edge_ring * heat * 1.8 * night_blend;
            lava_emit += seam_warm * outer_fringe * heat * 0.9 * night_blend;
        } else if (kind == 5) {
            vec3 scar_t1 = it1;
            vec3 scar_t2 = normalize(cross(idir, scar_t1));
            scar_t1 = normalize(cross(scar_t2, idir));
            vec3 scar_p = vec3(dot(NL, scar_t1),
                               dot(NL, scar_t2),
                               dot(NL, idir) - cos(rad));
            float cap_gate = 1.0 - smoothstep(rad * 1.02, rad * 1.22, ang);
            vec2 crater_uv = scar_p.xy / max(rad, 0.001);
            float crater_r = length(crater_uv);
            float crater_ang = atan(crater_uv.y, crater_uv.x);
            float crater_scale = 5.4 / max(rad, 0.08);
            vec3 crater_q = vec3(scar_p.xy * crater_scale + vec2(seed * 0.07, seed * 0.11),
                                 scar_p.z * 5.6 + seed * 0.05);
            float crater_coarse = vnoise(crater_q * 0.56 + vec3(1.7, 3.2, 4.4));
            float crater_fine = fbm(crater_q * 1.45 + vec3(6.1, 2.4, 1.8));
            float crater_rubble = vnoise(crater_q * 2.6 + vec3(2.8, 5.4, 1.3));
            float crater_fracture = fbm(vec3(crater_ang * 2.1 + seed * 0.03,
                                             crater_r * 3.6,
                                             seed * 0.09));
            float crater_chunks = fbm(crater_q * 3.4 + vec3(8.2, 1.6, 4.7));
            float radial_rays = sin(crater_ang * 9.0 + seed * 0.21) * 0.5 + 0.5;
            float radial_rays2 = sin(crater_ang * 5.0 - seed * 0.17) * 0.5 + 0.5;
            float radial_rays3 = sin(crater_ang * 13.0 + seed * 0.29) * 0.5 + 0.5;
            float crater_var = (crater_coarse - 0.5) * 0.38 + (crater_fine - 0.5) * 0.24;
            float rubble_var = (crater_rubble - 0.5) * 0.28;
            float chunk_var = (crater_chunks - 0.5) * 0.26;
            float fracture_var = (crater_fracture - 0.5) * 0.32;
            float ray_break = (radial_rays - 0.5) * 0.05 + (radial_rays2 - 0.5) * 0.03;
            float rim_shift = crater_var * 0.05 + fracture_var * 0.08 + ray_break;
            float bowl = 1.0 - smoothstep(0.0, 0.54 + crater_var * 0.05, crater_r);
            float inner_slope = smoothstep(0.14 + crater_var * 0.03, 0.70 + crater_var * 0.05, crater_r);
            float wall_band = smoothstep(0.30 + crater_var * 0.04, 0.78 + rim_shift, crater_r)
                            * (1.0 - smoothstep(0.78 + rim_shift, 0.96 + rim_shift, crater_r));
            float rim = smoothstep(0.74 + rim_shift, 0.88 + rim_shift, crater_r)
                      * (1.0 - smoothstep(0.88 + rim_shift, 1.02 + rim_shift, crater_r));
            float fractured_band = smoothstep(0.24 + crater_var * 0.03, 0.96 + rim_shift, crater_r)
                                 * (1.0 - smoothstep(0.96 + rim_shift, 1.14 + rim_shift, crater_r));
            float crater_mask = 1.0 - smoothstep(0.78 + crater_var * 0.04, 1.14 + rim_shift, crater_r);
            float outer_fade = 1.0 - smoothstep(0.92 + crater_var * 0.03, 1.14 + rim_shift, crater_r);
            float central_peak = 0.0; /* disabled */
            /* terrace — concentric step at ~50 % radius */
            float terrace = smoothstep(0.38 + crater_var * 0.03, 0.52 + crater_var * 0.04, crater_r)
                          * (1.0 - smoothstep(0.52 + crater_var * 0.04, 0.66 + crater_var * 0.03, crater_r));
            float radial_mix = radial_rays * 0.50 + radial_rays2 * 0.28 + radial_rays3 * 0.22;
            float radial_fractures = smoothstep(0.58, 0.84, radial_mix)
                                   * smoothstep(0.12, 0.56, crater_r)
                                   * (1.0 - smoothstep(0.56, 0.80, crater_r));
            /* ejecta streaks stay within the crater rim */
            float ejecta_streaks = smoothstep(0.62, 0.90, radial_mix)
                                 * smoothstep(0.68, 0.82, crater_r)
                                 * (1.0 - smoothstep(0.82, 0.96, crater_r));
            float depth = heat;
            float earth_crater = u_planet_type == 1 ? 1.0 : 0.0;
            float size_fade = smoothstep(0.006, 0.18, rad);
            float crater_alpha = mix(0.58, 1.0, earth_crater) * mix(0.08, 1.0, size_fade)
                               * (1.0 - s_intersection * 0.35) * cap_gate;
            vec3 crater_radial_dir = (scar_t1 * crater_uv.x + scar_t2 * crater_uv.y)
                                   / max(crater_r, 0.001);
            float bowl_slope = smoothstep(0.12, 0.62, crater_r)
                             * (1.0 - smoothstep(0.62, 0.92, crater_r));
            float rim_inner = smoothstep(0.60 + rim_shift, 0.84 + rim_shift, crater_r)
                            * (1.0 - smoothstep(0.84 + rim_shift, 0.98 + rim_shift, crater_r));
            float rim_outer = smoothstep(0.84 + rim_shift, 0.98 + rim_shift, crater_r)
                            * (1.0 - smoothstep(0.98 + rim_shift, 1.16 + rim_shift, crater_r));
            float normal_depth = crater_alpha * crater_mask * outer_fade;
            vec3 rubble_normal = (scar_t1 * rubble_var + scar_t2 * chunk_var)
                               * fractured_band * 0.18;
            vec3 crater_local_n = normalize(NL
                                      - crater_radial_dir * bowl_slope * (0.34 + depth * 0.18)
                                      - crater_radial_dir * rim_inner * 0.22
                                      + crater_radial_dir * rim_outer * 0.18
                                      + rubble_normal);
            shade_N = normalize(mix(shade_N,
                                    local_surface_dir_to_world(crater_local_n),
                                    normal_depth * 0.82));
            vec3 warm_brown = mix(vec3(0.18, 0.14, 0.11), vec3(0.34, 0.26, 0.18), crater_coarse);
            vec3 dusty_brown = mix(vec3(0.24, 0.19, 0.14), vec3(0.42, 0.33, 0.24), crater_fine);
            vec3 ridge_brown = mix(vec3(0.32, 0.25, 0.18), vec3(0.50, 0.40, 0.30), crater_coarse * 0.65 + crater_fine * 0.35);
            vec3 crater_dark = mix(base_surface * 0.28, warm_brown, earth_crater * 0.25 + 0.75);
            vec3 crater_mid = mix(base_surface * 0.52, dusty_brown, earth_crater * 0.15 + 0.55);
            vec3 crater_ridge = mix(base_surface * 0.74, ridge_brown, earth_crater * 0.10 + 0.45);
            vec3 crater_floor_col = mix(crater_dark * 0.72, crater_mid * 0.84,
                                        0.16 + crater_var * 0.18 + rubble_var * 0.20 + chunk_var * 0.18);
            vec3 crater_wall_col = mix(crater_dark * 0.92, crater_mid,
                                       0.48 + crater_var * 0.18 + rubble_var * 0.16 + chunk_var * 0.14);
            vec3 crater_surface = mix(crater_floor_col, crater_wall_col, inner_slope);
            vec3 crater_rim_col = mix(crater_surface, crater_ridge, 0.78 + crater_var * 0.12 + rubble_var * 0.12 + chunk_var * 0.10);
            vec3 fractured_col = mix(crater_dark * 0.85, crater_ridge,
                                     0.44 + crater_fine * 0.18 + rubble_var * 0.26 + chunk_var * 0.16);
            vec3 chunk_col = mix(crater_dark * 0.90, crater_mid * 1.05, 0.42 + chunk_var * 0.28);
            crater_surface = mix(crater_surface, chunk_col,
                                 smoothstep(0.54, 0.82, crater_chunks)
                                 * smoothstep(0.10, 0.88, crater_r)
                                 * (1.0 - smoothstep(0.88, 1.02, crater_r))
                                 * 0.34);
            crater_surface = mix(crater_surface, fractured_col,
                                 fractured_band * (0.30 + crater_rubble * 0.22 + max(fracture_var, 0.0) * 0.85));
            crater_surface = mix(crater_surface, crater_dark * 0.62,
                                 radial_fractures * (0.62 + max(fracture_var, 0.0) * 0.55));
            crater_surface = mix(crater_surface, crater_wall_col * 0.68, bowl * (0.34 + depth * 0.22));
            crater_surface = mix(crater_surface, crater_wall_col, wall_band * (0.58 + depth * 0.18));
            crater_surface = mix(crater_surface, crater_rim_col, rim * (0.56 + max(fracture_var, 0.0) * 0.24));
            /* terrace step — slightly lighter than wall, with rubble variation */
            vec3 terrace_col = mix(crater_wall_col * 0.88, crater_ridge * 0.82,
                                   0.45 + crater_var * 0.20 + rubble_var * 0.18);
            crater_surface = mix(crater_surface, terrace_col,
                                 terrace * (0.58 + max(fracture_var, 0.0) * 0.30));
            /* central peak — bright rocky protrusion */
            vec3 peak_col = mix(crater_mid * 1.06, crater_ridge * 0.92,
                                crater_coarse * 0.55 + crater_fine * 0.45);
            crater_surface = mix(crater_surface, peak_col, central_peak * 0.74);

            surface = mix(surface, crater_surface, crater_alpha * crater_mask * outer_fade);
            surface = mix(surface, crater_floor_col * 0.72, crater_alpha * bowl * (0.48 + depth * 0.26));
            surface = mix(surface, crater_rim_col, crater_alpha * rim * (0.30 + depth * 0.14));
            surface = mix(surface, crater_mid * 1.04, crater_alpha * ejecta_streaks * 0.34);
        }
    }

    /* ---- City lights (Earth, night side only) --------------------------- */
    if (u_planet_type == 1) {
        float night_mask  = smoothstep(0.08, -0.12, sun_dot);
        float n_land      = fbm(NL * 3.5);
        float land_mask   = smoothstep(0.40, 0.50, n_land);
        float lat_mask    = 1.0 - smoothstep(0.72, 0.92, abs(NL.y));

        /* Two-level clustering: broad region density + local subclusters.
         * High thresholds → only isolated hotspot peaks survive.       */
        float region_pop  = fbm(NL *  4.5 + vec3(3.3, 1.1, 7.7));
        float local_pop   = vnoise(NL * 16.0 + vec3(1.7, 5.4, 2.9));
        float pop_mask    = pow(smoothstep(0.46, 0.74, region_pop), 1.2)
                          * smoothstep(0.38, 0.68, local_pop)
                          * land_mask * lat_mask * night_mask;

        /* Sharp fine dots — three interleaved layers for dense coverage. */
        float city_sharp  = vnoise(NL * 85.0 + vec3(5.5, 2.8, 1.3));
        float core_lights = smoothstep(0.70, 0.88, city_sharp);

        float city_sharp2 = vnoise(NL * 85.0 + vec3(9.1, 4.6, 0.7));
        float core_lights2 = smoothstep(0.70, 0.88, city_sharp2) * 0.85;

        float city_sharp3 = vnoise(NL * 85.0 + vec3(2.3, 7.8, 5.1));
        float core_lights3 = smoothstep(0.72, 0.90, city_sharp3) * 0.70;

        /* Subtle glow: just enough to soften edges, not smear into blobs. */
        float city_glow_n = vnoise(NL * 40.0 + vec3(5.5, 2.8, 1.3));
        float glow_lights = smoothstep(0.76, 0.92, city_glow_n) * 0.12;

        vec3 city_color   = mix(vec3(1.0, 0.92, 0.65), vec3(1.0, 0.75, 0.40),
                                vnoise(NL * 22.0 + vec3(2.1, 6.3, 4.4)));
        lava_emit += city_color * (core_lights + core_lights2 + core_lights3 + glow_lights)
                   * pop_mask * (1.0 - impact_light_block) * 0.90;
    }

    if (cloud_mask > 0.0) {
        surface = mix(surface, vec3(0.93, 0.95, 0.97), cloud_mask);
        lava_emit *= 1.0 - cloud_mask;
        shade_N = normalize(mix(shade_N, N, cloud_mask));
    }

    /* ---- Phong diffuse ------------------------------------------------ */
    diff = max(dot(shade_N, L), 0.0);
    float light = u_ambient + (1.0 - u_ambient) * diff;

    frag_color = vec4(surface * light + lava_emit, 1.0);
}
