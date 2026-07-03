#version 330 core
/*
 * comet.frag — coma / ion tail / dust tail intensity profiles.
 *
 * u_kind: 0 = coma (uv is the billboard −1..+1 square)
 *         1 = ion tail  (u along tail 0→1, v across −1..+1): narrow, blue,
 *             filamentary streaks flowing tailward
 *         2 = dust tail: broad, warm, smooth, with a quadratic bend of the
 *             ridge line (u_curve) — dust lags the nucleus along the orbit
 *
 * Additive (GL_ONE/GL_ONE), depth test on / write off; log depth computed
 * per fragment from the interpolated camera-relative position so the ribbon
 * sorts correctly against planets along its whole length.
 */
in vec2 v_uv;
in vec3 v_pos;

uniform vec3  u_cam_fwd;
uniform int   u_kind;
uniform float u_act;      /* sublimation activity 0..1.5                   */
uniform vec3  u_col;
uniform float u_time;
uniform float u_seed;
uniform float u_curve;    /* dust ridge bend (signed), 0 for ion/coma      */
uniform float u_gain;     /* per-draw intensity (crossed planes draw at    */
                          /* reduced gain so the pair sums to one plane)   */

out vec4 frag_color;

float hash1(float p) { return fract(sin(p * 127.1 + u_seed * 311.7) * 43758.5453); }

float noise1(float p) {
    float i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash1(i), hash1(i + 1.0), f);
}

void main() {
    /* Log depth from the true fragment position (DEPTH_FAR prelude). */
    const float FAR = DEPTH_FAR;
    float eye_depth = max(dot(v_pos, u_cam_fwd), 1e-6);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    vec3 col;
    if (u_kind == 0) {
        /* Coma: bright core + soft envelope. */
        float r = length(v_uv);
        if (r > 1.0) discard;
        float g = exp(-r * 5.0) * 1.6 + exp(-r * 14.0) * 2.4;
        col = u_col * (g * (0.25 + 0.75 * min(u_act, 1.0)));
    } else {
        float u = v_uv.x;
        float v = v_uv.y;
        /* Kill the quad borders unconditionally — a gaussian alone leaves a
         * few-percent hard edge that reads as a solid translucent wedge. */
        float edge = (1.0 - smoothstep(0.55, 0.95, abs(v_uv.y)))
                   * (1.0 - smoothstep(0.80, 1.00, u));
        /* Dust ridge bends quadratically with distance down the tail. */
        if (u_kind == 2) v -= u_curve * u * u;
        float taper = pow(1.0 - u, u_kind == 1 ? 1.6 : 1.3);

        if (u_kind == 1) {
            /* Ion: tight core, streaks advecting tailward (solar wind). */
            float w = exp(-v * v * (12.0 - 6.0 * u));
            float streak = 0.55
                         + 0.45 * noise1(u * 26.0 - u_time * 0.9 + v * 3.0)
                         * (0.6 + 0.4 * noise1(v * 7.0 + u * 9.0 + 13.7));
            streak = mix(streak, 0.75, u);   /* calm the blocky far end */
            col = u_col * (taper * w * streak * 1.1);
        } else {
            /* Dust: broad fan with radial striae (synchrones — Hale-Bopp
             * style).  Constant v is ~constant fan angle on the trapezoid
             * ribbon, so noise in v alone makes rays that emanate from the
             * nucleus; they drift slowly as fresh dust is released. */
            float w = exp(-v * v * 6.0);
            float striae = 0.70 + 0.30 * noise1(v * 9.0 + u * 1.5 - u_time * 0.05)
                         * (0.5 + 0.5 * noise1(v * 23.0 + 7.3));
            col = u_col * (taper * w * striae * 0.5);
        }
        col *= edge * min(u_act, 1.2) * u_gain;
    }

    if (max(col.r, max(col.g, col.b)) < 0.002) discard;
    frag_color = vec4(col, 1.0);
}
