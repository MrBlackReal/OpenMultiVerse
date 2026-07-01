#version 330 core
/*
 * jet.frag — relativistic AGN jets: twin collimated plasma beams along the spin
 * axis.  Additively blended (glow that bloom picks up).
 *
 * Beam: a cone narrow at the base (flaring outward), turbulent filaments that
 * advect outward with time, and bright shock knots travelling up the jet.  The
 * lobe rotating/pointing toward the camera is Doppler-brightened and bluer (the
 * far lobe dims) — the effect that makes blazars blaze.
 */
in  vec2 v_uv;                /* x across (-1..1), y along (-1..1)             */
out vec4 frag_color;

uniform vec3  u_color;        /* jet base colour                              */
uniform float u_time;         /* seconds                                      */
uniform float u_activity;     /* AGN activity (jet strength)                  */
uniform vec3  u_axis;         /* spin axis, unit, camera-relative world       */
uniform vec3  u_center;       /* black-hole centre, camera-relative AU        */

float hash13(vec3 p) {
    p  = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(vec3(i, 0.0));
    float b = hash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = hash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = hash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}

void main() {
    float across = v_uv.x;
    float along  = v_uv.y;
    float a      = abs(along);                 /* 0 at hole .. 1 at tip        */

    /* Collimated cone: very tight at the base, flaring gently with distance. */
    float hw   = 0.035 + 0.5 * pow(a, 0.85);
    float r    = abs(across) / max(hw, 1e-3);
    if (r > 1.0) discard;
    float core = pow(smoothstep(1.0, 0.0, r), 1.6);   /* soft-edged across      */

    /* Length envelope: start above the disk (hide the base) and taper the tip. */
    float lp = smoothstep(0.03, 0.14, a) * (1.0 - smoothstep(0.55, 1.0, a));

    /* Turbulent filaments (two octaves) advecting outward + travelling shock
     * knots.  High contrast so the beam looks like ragged plasma, not a beam. */
    float turb = fbm(vec2(across * 3.0 + a * 2.0, a * 7.0  - u_time * 2.6));
    float fine = fbm(vec2(across * 8.0,           a * 17.0 - u_time * 5.0));
    float fil  = smoothstep(0.18, 0.85, mix(turb, fine, 0.4));
    /* Travelling shock knots — brighter and a touch wider so they read. */
    float knot = pow(0.5 + 0.5 * sin(a * 13.0 - u_time * 6.0), 4.0);

    /* Helical/braided structure: two counter-wound strands (magnetic collimation)
     * spiralling up the jet. */
    float h1    = sin(a * 9.0 + across * 3.2 - u_time * 3.5);
    float h2    = sin(a * 9.0 - across * 3.2 - u_time * 3.5);
    float braid = 0.55 + 0.45 * max(h1, h2);

    float emit = core * lp * braid * (0.22 + 1.05 * fil) * (1.0 + 3.2 * knot);

    /* Relativistic Doppler beaming per lobe: the lobe pointing toward the camera
     * blazes and blueshifts; the receding one dims. */
    vec3  lobe = sign(along) * normalize(u_axis);
    float appr = dot(lobe, -normalize(u_center));   /* +1 toward camera         */
    float beam = clamp(0.45 + 1.0 * appr, 0.12, 2.2);

    /* Colour gradient along the jet: hot white at the base → cool electric blue
     * toward the tip (synchrotron plasma cooling with distance). */
    vec3  c_hot  = vec3(0.90, 0.96, 1.0);
    vec3  c_cool = vec3(0.40, 0.62, 1.0);
    vec3  col    = mix(c_hot, c_cool, smoothstep(0.05, 0.55, a));
    col = mix(col, vec3(0.95, 0.98, 1.0), clamp(appr, 0.0, 0.5));  /* beamed → whiter */

    /* Fade the ribbon as the axis aligns with the view: the axis-aligned
     * billboard degenerates to a line looking down the jet, so cross-fade it out
     * (the beamed core + face-on disk carry the pole-on / blazar look). */
    float edge_on  = abs(dot(normalize(u_axis), normalize(u_center)));
    float sideness = 1.0 - smoothstep(0.80, 0.985, edge_on);

    float I = emit * beam * clamp(u_activity, 0.0, 2.0) * 1.25 * sideness;
    if (I < 0.002) discard;
    /* Logarithmic depth for the depth TEST (mask is off, so this doesn't write):
     * keeps the additive glow consistent with the log-depth scene, else standard
     * depth saturates at the far plane and the jets fail the test. */
    gl_FragDepth = log2(1.0 / gl_FragCoord.w + 1.0) / log2(DEPTH_FAR + 1.0);
    frag_color = vec4(col * I, I);
}
