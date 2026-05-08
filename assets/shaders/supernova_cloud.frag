#version 330 core
/*
 * supernova_cloud.frag - volumetric ejecta shell for the expanding remnant.
 *
 * This pass is responsible for the long-lived outer cloud. It raymarches a
 * deformed shell volume, layering broad lobes, filaments, ridges, clumps, and
 * hotter rim accents so the result reads more like asymmetric ejecta than a
 * perfect noisy sphere.
 */

in vec2 v_uv;

uniform vec3  u_oc;
uniform vec3  u_color;
uniform float u_radius;
uniform float u_shell_inner;
uniform float u_density;
uniform float u_hot_shell;
uniform float u_time;
uniform float u_seed;
uniform float u_bill_scale;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform vec3  u_cam_fwd;
uniform float u_fov_tan;
uniform float u_aspect;
uniform vec2  u_screen;

out vec4 frag_color;

const float FAR = 2000.0;
const float OUTER_BOUND = 1.24;

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float sn_value_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash31(i + vec3(0.0, 0.0, 0.0));
    float n100 = hash31(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash31(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash31(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash31(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash31(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash31(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash31(i + vec3(1.0, 1.0, 1.0));

    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.55;
    for (int i = 0; i < 4; i++) {
        v += sn_value_noise(p) * a;
        p = p * 2.03 + vec3(3.7, 1.9, 2.6);
        a *= 0.5;
    }
    return v;
}

float ridged_fbm(vec3 p) {
    float v = 0.0;
    float a = 0.60;
    for (int i = 0; i < 3; i++) {
        float n = sn_value_noise(p);
        n = 1.0 - abs(n * 2.0 - 1.0);
        v += n * a;
        p = p * 2.18 + vec3(4.1, 2.3, 3.4);
        a *= 0.55;
    }
    return v;
}

void main() {
    float outer = 1.0;
    float inner = clamp(u_shell_inner, 0.05, 0.96);
    float radius = max(u_radius, 1e-5);
    vec3 oc_local = u_oc / radius;
    /* Reconstruct the per-fragment world ray. The billboard is only a raster
     * carrier; all meaningful volume work happens from this ray onward. */
    vec2 ndc = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 ray_dir = normalize(u_cam_fwd
                           + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                           + u_cam_up    * (ndc.y * u_fov_tan));
    float b = dot(oc_local, ray_dir);
    float c = dot(oc_local, oc_local) - OUTER_BOUND * OUTER_BOUND;
    float disc = b * b - c;
    float t0, t1, tEnter, tExit, stepLen, eye_depth;
    float accumAlpha = 0.0;
    vec3 accumColor = vec3(0.0);

    if (u_density <= 0.00005 || disc < 0.0) discard;

    t0 = -b - sqrt(disc);
    t1 = -b + sqrt(disc);
    if (t1 <= 0.0) discard;
    tEnter = max(t0, 0.0);
    tExit = t1;
    if (tExit <= tEnter) discard;

    stepLen = (tExit - tEnter) / 16.0;

    for (int i = 0; i < 16; i++) {
        /* March a deformed shell rather than a full dense fog volume. The
         * inner/outer radii are warped independently so large-scale lobes can
         * protrude without collapsing the whole cloud into a uniform sphere. */
        float t = tEnter + (float(i) + 0.5) * stepLen;
        vec3 p = oc_local + ray_dir * t;
        float rr = length(p);
        vec3 dir = rr > 1e-4 ? p / rr : vec3(0.0, 0.0, 1.0);
        float swirl = fbm(p * 3.5 + vec3(u_seed * 10.0, u_time * 0.22, -u_time * 0.16));
        float filaments = fbm(p.yzx * 7.5 + vec3(8.0, u_seed * 15.0, u_time * 0.11));
        float macro = fbm(dir * 4.8 + vec3(u_seed * 23.0, u_time * 0.04, -u_time * 0.03));
        float lobe = fbm(dir.zxy * 8.0 + vec3(2.0, u_seed * 31.0, u_time * 0.06));
        float plumes = fbm(dir.xzy * 3.1 + vec3(u_seed * 13.0, -u_time * 0.03, u_time * 0.02));
        float anisotropy = fbm(dir * 2.2 + vec3(1.0, u_seed * 7.0, -u_time * 0.02));
        float ridges = ridged_fbm(p.zxy * 8.8 + dir * 3.5
                                + vec3(u_seed * 41.0, -u_time * 0.08, u_time * 0.12));
        float knots = fbm(p * 11.0 + dir.yzx * 4.5
                        + vec3(6.0, u_seed * 27.0, -u_time * 0.10));
        float plumeMask = smoothstep(0.52, 0.90, plumes) * mix(0.75, 1.35, ridges);
        float asymMask = mix(0.78, 1.32, anisotropy);
        float innerWarp = clamp(inner
                              + (macro - 0.5) * 0.18
                              + (lobe - 0.5) * 0.11
                              + (plumes - 0.5) * 0.06,
                                0.04, 0.90);
        float outerWarp = clamp(outer - 0.12
                              + (macro - 0.5) * 0.30
                              + (swirl - 0.5) * 0.13
                              + (lobe - 0.5) * 0.16
                              + plumeMask * 0.15 * asymMask,
                                innerWarp + 0.08, OUTER_BOUND - 0.02);
        float shell = smoothstep(innerWarp - 0.03, innerWarp + 0.08, rr)
                    * (1.0 - smoothstep(outerWarp - 0.16, outerWarp + 0.02, rr));
        float body = smoothstep(innerWarp * 0.78, innerWarp + 0.10, rr)
                   * (1.0 - smoothstep(outerWarp - 0.30, outerWarp - 0.05, rr));
        float rim = smoothstep(outerWarp - 0.18, outerWarp - 0.05, rr)
                  * (1.0 - smoothstep(outerWarp - 0.02, outerWarp + 0.05, rr));
        float shockBands = 0.5 + 0.5 * sin(rr * 24.0 - u_time * 0.45
                                          + (macro - 0.5) * 4.0 + filaments * 2.8);
        float streaks = pow(smoothstep(0.34, 0.92, filaments), 1.35)
                      * mix(0.78, 1.28, ridges);
        float pockets = smoothstep(0.34, 0.88, swirl) * smoothstep(0.30, 0.82, filaments);
        float clumps = smoothstep(0.44, 0.92, knots) * mix(0.82, 1.24, macro) * mix(0.82, 1.34, plumeMask);
        float voids = 1.0 - 0.50 * smoothstep(0.14, 0.52, ridges) * smoothstep(0.18, 0.58, knots);
        float ejectaFlares = smoothstep(0.48, 0.92, plumes)
                           * smoothstep(outerWarp - 0.26, outerWarp - 0.02, rr)
                           * mix(0.80, 1.52, anisotropy);
        float density = (shell * mix(0.24, 1.18, swirl)
                               * mix(0.72, 1.22, streaks)
                               * mix(0.84, 1.38, ridges)
                               * mix(0.86, 1.46, plumeMask))
                      + (rim * (0.18 + 0.44 * u_hot_shell)
                             * mix(0.84, 1.42, shockBands)
                             * mix(0.78, 1.20, ridges)
                             * mix(0.88, 1.52, ejectaFlares))
                      + (body * pockets * clumps * (0.14 + 0.24 * u_density));
        density *= voids * mix(0.74, 1.30, macro) * mix(0.80, 1.24, lobe) * mix(0.82, 1.22, asymMask);
        vec3 hot = mix(vec3(1.30, 0.80, 0.38), u_color * 1.04, smoothstep(innerWarp, outerWarp, rr));
        vec3 ember = vec3(1.08, 0.46, 0.18);
        vec3 cold = vec3(0.16, 0.30, 0.52);
        vec3 sampleCol = mix(cold, hot, smoothstep(innerWarp * 0.84, outerWarp - 0.05, rr));
        sampleCol = mix(sampleCol, ember, rim * (0.32 + 0.38 * shockBands) + ejectaFlares * 0.18);
        sampleCol *= mix(0.88, 1.14, clumps) * mix(0.90, 1.08, asymMask);
        float sampleAlpha = density * stepLen * 1.48 * (1.12 + u_hot_shell * 1.05) * u_density;

        sampleAlpha = clamp(sampleAlpha, 0.0, 0.32);
        accumColor += sampleCol * sampleAlpha * (1.0 - accumAlpha);
        accumAlpha += sampleAlpha * (1.0 - accumAlpha);
    }

    eye_depth = (tExit * radius) * dot(ray_dir, u_cam_fwd);
    eye_depth = max(eye_depth, 0.0);
    /* Use a soft far fade so very large late-stage clouds disappear gradually
     * instead of popping exactly at the volumetric depth horizon. */
    accumAlpha *= smoothstep(0.0, 0.018, accumAlpha);
    accumAlpha *= 1.0 - smoothstep(FAR * 0.82, FAR * 4.60, eye_depth);
    if (accumAlpha < 0.0008) discard;

    eye_depth = min(eye_depth, FAR * 0.9995);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);
    frag_color = vec4(accumColor, accumAlpha);
}
