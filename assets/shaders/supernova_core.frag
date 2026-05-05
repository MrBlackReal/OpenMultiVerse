#version 330 core
/*
 * supernova_core.frag - volumetric blast core and shock front.
 */

in vec2 v_uv;

uniform vec3  u_oc;
uniform vec3  u_color;
uniform float u_radius;
uniform float u_flash_intensity;
uniform float u_core_intensity;
uniform float u_core_ratio;
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

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float noise3(vec3 p) {
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
        v += noise3(p) * a;
        p = p * 2.02 + vec3(3.1, 2.7, 1.9);
        a *= 0.5;
    }
    return v;
}

void main() {
    float radius = max(u_radius, 1e-5);
    vec3 oc_local = u_oc / radius;
    vec2 ndc = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 ray_dir = normalize(u_cam_fwd
                           + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                           + u_cam_up    * (ndc.y * u_fov_tan));
    float b = dot(oc_local, ray_dir);
    float c = dot(oc_local, oc_local) - 1.0;
    float disc = b * b - c;
    float core_r = clamp(u_core_ratio, 0.08, 0.92);
    float t0, t1, tEnter, tExit, stepLen, eye_depth;
    float accumAlpha = 0.0;
    vec3 accumColor = vec3(0.0);

    if ((u_flash_intensity <= 0.001 && u_core_intensity <= 0.001) || disc < 0.0) discard;

    t0 = -b - sqrt(disc);
    t1 = -b + sqrt(disc);
    if (t1 <= 0.0) discard;
    tEnter = max(t0, 0.0);
    tExit = t1;
    if (tExit <= tEnter) discard;

    stepLen = (tExit - tEnter) / 18.0;

    for (int i = 0; i < 18; i++) {
        float t = tEnter + (float(i) + 0.5) * stepLen;
        vec3 p = oc_local + ray_dir * t;
        float rr = length(p);
        vec3 dir = rr > 1e-4 ? p / rr : vec3(0.0, 0.0, 1.0);
        float core_shape = exp(-pow(rr / max(core_r, 0.12), 2.15) * 2.2);
        float flash_fill = exp(-rr * 2.4);
        float turbulence = fbm(p * 4.2 + vec3(u_seed * 11.0, u_time * 0.18, -u_time * 0.14));
        float wisps = fbm(p.zxy * 8.0 + vec3(7.0, u_seed * 17.0, u_time * 0.10));
        float macro = fbm(dir * 5.6 + vec3(u_seed * 19.0, u_time * 0.05, -u_time * 0.03));
        float shock_center = mix(core_r + 0.09, 0.84, clamp(u_flash_intensity * 0.9 + 0.1, 0.0, 1.0))
                           + (macro - 0.5) * 0.08;
        float shock_width = mix(0.20, 0.09, clamp(u_flash_intensity, 0.0, 1.0))
                          + (wisps - 0.5) * 0.03;
        float shock_shell = exp(-pow((rr - shock_center) / max(shock_width, 0.05), 2.0));
        float shock_fill = smoothstep(core_r + 0.02, shock_center, rr)
                         * (1.0 - smoothstep(shock_center + shock_width * 0.8,
                                             shock_center + shock_width * 1.8, rr));
        float density = (core_shape * (0.90 + 0.85 * u_core_intensity)
                       + flash_fill * (0.18 + 0.95 * u_flash_intensity)
                       + (shock_shell * 0.70 + shock_fill * 0.28) * (0.16 + 1.15 * u_flash_intensity));
        density *= mix(0.82, 1.20, turbulence) * mix(0.88, 1.12, wisps) * mix(0.90, 1.12, macro);

        vec3 white_hot = vec3(1.75, 1.68, 1.52);
        vec3 hot_edge = mix(vec3(1.48, 0.96, 0.48), u_color * 1.25, smoothstep(core_r, 1.0, rr));
        vec3 sampleCol = mix(white_hot, hot_edge, smoothstep(core_r * 0.45, 1.0, rr));
        float sampleAlpha = density * stepLen * 1.18;

        sampleAlpha = clamp(sampleAlpha, 0.0, 0.38);
        accumColor += sampleCol * sampleAlpha * (1.0 - accumAlpha);
        accumAlpha += sampleAlpha * (1.0 - accumAlpha);
    }

    if (accumAlpha < 0.01) discard;

    eye_depth = (tExit * radius) * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);
    frag_color = vec4(accumColor, accumAlpha);
}
