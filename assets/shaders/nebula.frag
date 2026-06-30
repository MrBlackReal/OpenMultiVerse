#version 330 core
/*
 * nebula.frag — volumetric emission nebula.
 *
 * A screen-space ray is reconstructed per fragment, intersected with the
 * nebula's bounding sphere (camera-relative u_oc, normalised by u_radius), and
 * marched front-to-back.  Density is an FBM cloud — dense, knotty core
 * feathering to wispy filaments at the rim — with cheap ridged detail.  Because
 * the marcher works purely from the ray it behaves identically whether the
 * camera is light-years away (a few-pixel static blob) or flown right inside it
 * (it envelops the view): one representation, nothing pops.
 *
 * Performance: the per-step noise budget is deliberately small (one 3-octave +
 * one 2-octave value-noise eval), the step count is adjustable (u_steps, scaled
 * down for distant nebulae on the CPU), the march jitters per pixel so few
 * steps don't band, and it terminates early once opacity saturates.
 *
 * Output is premultiplied; the caller blends GL_ONE / GL_ONE_MINUS_SRC_ALPHA
 * ("over"), and bright knots exceed 1.0 in the HDR buffer so they bloom (#2).
 */
in vec2 v_uv;

uniform vec3  u_oc;          /* camera-relative centre (world units)     */
uniform vec3  u_color;       /* emission-type tint                       */
uniform float u_radius;      /* bounding radius (world units)            */
uniform float u_density;     /* overall opacity / brightness (artistic)  */
uniform int   u_steps;       /* raymarch steps for this draw             */
uniform int   u_shape;       /* morphology archetype, see shape_env()    */
uniform float u_seed;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform vec3  u_cam_fwd;
uniform float u_fov_tan;
uniform float u_aspect;
uniform vec2  u_screen;

out vec4 frag_color;

const float FAR   = 2000.0;
const float BOUND = 1.0;     /* sphere radius in normalised units */

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash31(i + vec3(0,0,0)), hash31(i + vec3(1,0,0)), f.x),
                   mix(hash31(i + vec3(0,1,0)), hash31(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash31(i + vec3(0,0,1)), hash31(i + vec3(1,0,1)), f.x),
                   mix(hash31(i + vec3(0,1,1)), hash31(i + vec3(1,1,1)), f.x), f.y), f.z);
}

/* 3-octave FBM */
float fbm3(vec3 p) {
    float v = vnoise(p) * 0.5;
    p = p * 2.03 + vec3(3.7, 1.9, 2.6);  v += vnoise(p) * 0.25;
    p = p * 2.03 + vec3(1.9, 4.2, 2.1);  v += vnoise(p) * 0.125;
    return v / 0.875;
}

/* 2-octave FBM */
float fbm2(vec3 p) {
    float v = vnoise(p) * 0.6;
    p = p * 2.11 + vec3(4.1, 2.3, 3.4);  v += vnoise(p) * 0.3;
    return v / 0.9;
}

/* ----------------------------------------------------------------------------
 * shape_env — base density envelope for the nebula's morphology archetype.
 *
 * p is the sample position inside the unit bounding sphere (|p| <= 1); the FBM
 * cloud is multiplied on top, so this only defines the large-scale 3D *form*.
 * Every shape is a function of 3D position, so it is correct from any viewing
 * angle (not a flat impostor).  `axis` is the nebula's symmetry axis.
 *
 *   0 DIFFUSE  soft ball                 reflection nebulae, big HII glows
 *   1 SHELL    hollow spherical shell    supernova remnants (Veil)
 *   2 RING     torus around axis         planetary nebulae (Helix), ring HII (Rosette)
 *   3 BIPOLAR  two lobes along axis      bipolar planetaries (Dumbbell)
 *   4 PILLARS  columns rising from a base star-forming pillars (Eagle)
 *   5 CAVITY   cloud with a blown cavity + bright rim   blister HII (Orion, Lagoon)
 * ------------------------------------------------------------------------- */
float shape_env(vec3 p, float rr, int shape, vec3 axis) {
    if (shape == 1) {                       /* SHELL */
        return smoothstep(0.45, 0.68, rr) * (1.0 - smoothstep(0.84, 1.0, rr));
    }
    if (shape == 2) {                       /* RING / TORUS */
        float ax  = dot(p, axis);
        float rad = length(p - axis * ax);
        float ring  = 1.0 - smoothstep(0.12, 0.36, abs(rad - 0.60));
        float thick = 1.0 - smoothstep(0.14, 0.34, abs(ax));
        return ring * thick;
    }
    if (shape == 3) {                       /* BIPOLAR */
        float ax   = dot(p, axis);
        float r2   = length(p - axis * ax);
        float lobe = exp(-pow((abs(ax) - 0.42) / 0.30, 2.0));
        float waist= smoothstep(0.0, 0.45, abs(ax));     /* pinch at centre */
        float radial = 1.0 - smoothstep(0.16, 0.46, r2);
        return lobe * radial * (0.35 + 0.65 * waist);
    }
    if (shape == 4) {                       /* PILLARS */
        float h    = dot(p, axis);                       /* -1..1 along axis */
        vec3  perp = p - axis * h;
        vec3  t1   = normalize(cross(axis, vec3(0.31, 1.0, 0.71)));
        vec3  t2   = cross(axis, t1);
        vec2  pp   = vec2(dot(perp, t1), dot(perp, t2));
        float base = (1.0 - smoothstep(0.45, 1.0, rr))
                   * (1.0 - smoothstep(-0.15, 0.45, h)); /* cloud at the foot */
        float taper = clamp(1.0 - (h + 0.5) * 0.55, 0.28, 1.0);
        float rad   = 0.17 * taper;
        float col = 0.0;
        col = max(col, 1.0 - smoothstep(rad * 0.55, rad, length(pp - vec2( 0.00, 0.00))));
        col = max(col, 1.0 - smoothstep(rad * 0.55, rad, length(pp - vec2( 0.30, 0.12))));
        col = max(col, 1.0 - smoothstep(rad * 0.55, rad, length(pp - vec2(-0.24, 0.22))));
        float hmask = smoothstep(-0.72, -0.40, h)
                    * (1.0 - smoothstep(0.34, 0.62, h)); /* extent + eroded tips */
        return clamp(base * 0.7 + col * hmask, 0.0, 1.0);
    }
    if (shape == 5) {                       /* CAVITY (blister HII) */
        float ball = 1.0 - smoothstep(0.20, 1.0, rr);
        float cav  = 1.0 - smoothstep(0.0, 0.46, length(p - axis * 0.28));
        float rim  = (1.0 - smoothstep(0.0, 0.16, abs(length(p - axis * 0.28) - 0.46)));
        return clamp(ball - cav * 0.85, 0.0, 1.0) + rim * ball * 0.5;
    }
    return 1.0 - smoothstep(0.12, 1.0, rr);                /* DIFFUSE */
}

void main() {
    float radius = max(u_radius, 1e-5);
    vec3  oc = u_oc / radius;

    vec2 ndc = (gl_FragCoord.xy / (u_screen * 0.5)) - 1.0;
    vec3 rd  = normalize(u_cam_fwd
                       + u_cam_right * (ndc.x * u_aspect * u_fov_tan)
                       + u_cam_up    * (ndc.y * u_fov_tan));

    float b = dot(oc, rd);
    float c = dot(oc, oc) - BOUND * BOUND;
    float disc = b * b - c;
    if (u_density <= 0.00005 || disc < 0.0) discard;

    float sq = sqrt(disc);
    float tEnter = max(-b - sq, 0.0);
    float tExit  = -b + sq;
    if (tExit <= tEnter) discard;

    int   steps   = max(u_steps, 4);
    float stepLen = (tExit - tEnter) / float(steps);
    float jitter  = hash21(gl_FragCoord.xy);   /* break up low-step banding */
    float accumA  = 0.0;
    vec3  accumC  = vec3(0.0);

    vec3 seedv = vec3(u_seed * 7.0, u_seed * 3.0, -u_seed * 5.0);

    /* Stable per-nebula symmetry axis for the shape archetypes (real spatial
     * orientations are largely unknown, so a deterministic axis is fine). */
    vec3 axis = normalize(vec3(sin(u_seed * 1.7),
                               cos(u_seed * 1.1) * 0.6 + 0.55,
                               sin(u_seed * 2.3 + 1.0)));

    for (int i = 0; i < steps; i++) {
        float t = tEnter + (float(i) + jitter) * stepLen;
        vec3  p = oc + rd * t;
        float rr = length(p);
        if (rr > 1.0) continue;

        /* Large-scale 3D form for this nebula's morphology archetype. */
        float env = shape_env(p, rr, u_shape, axis);
        if (env <= 0.001) continue;

        vec3  q     = p * 2.0 + seedv;
        float n     = fbm3(q);
        float ridge = 1.0 - abs(2.0 * fbm2(q * 2.5 + n * 0.8) - 1.0);

        float density = env * (0.25 + 1.05 * n) * mix(0.55, 1.40, ridge);
        density = max(density - 0.13, 0.0);
        if (density <= 0.0) continue;

        /* Hotter, paler core; emission tint through the body and wisps. */
        float coreT = pow(env, 1.6) * smoothstep(0.40, 0.90, n);
        vec3  col   = mix(u_color * (0.60 + 0.50 * n),
                          mix(u_color, vec3(1.0), 0.55), coreT);

        float a = clamp(density * stepLen * 2.4 * u_density, 0.0, 0.32);
        accumC += col * a * (1.0 - accumA);
        accumA += a * (1.0 - accumA);
        if (accumA > 0.985) break;
    }

    accumA *= smoothstep(0.0, 0.02, accumA);
    if (accumA < 0.0008) discard;

    /* Depth at the near surface so the gas envelops embedded stars; clamped to
     * the far horizon and log-encoded to match the other passes. */
    float eye_depth = (tEnter * radius) * dot(rd, u_cam_fwd);
    eye_depth = clamp(eye_depth, 0.0, FAR * 0.9995);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    frag_color = vec4(accumC, accumA);
}
