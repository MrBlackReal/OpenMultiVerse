#version 330 core
/*
 * supernova_cloud.frag - volumetric shell cloud for the expanding remnant.
 */

in vec2 v_uv;

uniform vec3  u_oc;
uniform vec3  u_color;
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
        p = p * 2.03 + vec3(3.7, 1.9, 2.6);
        a *= 0.5;
    }
    return v;
}

void main() {
    float outer = 1.0;
    float inner = clamp(u_shell_inner, 0.05, 0.96);
    vec2 ndc = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 ray_dir = normalize(u_cam_fwd
                           + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                           + u_cam_up    * (ndc.y * u_fov_tan));
    float b = dot(u_oc, ray_dir);
    float c = dot(u_oc, u_oc) - 1.0;
    float disc = b * b - c;
    float t0, t1, tEnter, tExit, stepLen, eye_depth;
    float accumAlpha = 0.0;
    vec3 accumColor = vec3(0.0);

    if (u_density <= 0.001 || disc < 0.0) discard;

    t0 = -b - sqrt(disc);
    t1 = -b + sqrt(disc);
    if (t1 <= 0.0) discard;
    tEnter = max(t0, 0.0);
    tExit = t1;
    if (tExit <= tEnter) discard;

    stepLen = (tExit - tEnter) / 14.0;

    for (int i = 0; i < 14; i++) {
        float t = tEnter + (float(i) + 0.5) * stepLen;
        vec3 p = u_oc + ray_dir * t;
        float rr = length(p);
        float shell = smoothstep(inner, inner + 0.08, rr)
                    * (1.0 - smoothstep(0.86, 1.0, rr));
        float swirl = fbm(p * 3.5 + vec3(u_seed * 10.0, u_time * 0.22, -u_time * 0.16));
        float filaments = fbm(p.yzx * 7.5 + vec3(8.0, u_seed * 15.0, u_time * 0.11));
        float density = shell * mix(0.30, 1.0, swirl) * mix(0.74, 1.12, filaments);
        vec3 hot = mix(vec3(1.28, 0.78, 0.36), u_color, smoothstep(inner, 1.0, rr));
        vec3 cold = vec3(0.18, 0.34, 0.56);
        vec3 sampleCol = mix(cold, hot, smoothstep(inner, 1.0, rr));
        float sampleAlpha = density * stepLen * 1.55 * (1.15 + u_hot_shell * 0.9) * u_density;

        sampleAlpha = clamp(sampleAlpha, 0.0, 0.32);
        accumColor += sampleCol * sampleAlpha * (1.0 - accumAlpha);
        accumAlpha += sampleAlpha * (1.0 - accumAlpha);
    }

    if (accumAlpha < 0.01) discard;

    eye_depth = tExit * dot(ray_dir, u_cam_fwd);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);
    frag_color = vec4(accumColor, accumAlpha);
}
