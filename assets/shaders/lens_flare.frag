#version 330 core
/*
 * lens_flare.frag — cinematic sun lens flare, drawn additively over the final
 * LDR composite (a lens artifact happens in the camera, after the "film", so
 * post-tonemap is physically the right place — and it means the scene depth
 * texture is no longer an attachment and can legally be sampled here).
 *
 * Elements: four ghost sprites mirrored through the screen centre (each with
 * its own chromatic tint, as successive lens-element reflections disperse),
 * one faint halo ring, one blue anamorphic streak, and a small warm glow at
 * the light itself.  Everything scales with u_intensity × depth-buffer
 * visibility, so the whole flare fades smoothly as the sun slides behind a
 * planet's limb.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_depth;       /* scene depth (log-encoded, see below)     */
uniform vec2      u_light_uv;    /* light position in screen UV              */
uniform float     u_light_depth; /* light's log depth: log2(d+1)/log2(FAR+1) */
uniform float     u_intensity;   /* setting × flux ramp (0 = skipped on CPU) */
uniform vec3      u_color;       /* light chromaticity                       */
uniform float     u_aspect;      /* WIN_W / WIN_H                            */
uniform vec4      u_tune;        /* x = ghost gain, y = halo gain,
                                    z = halo radius, w = streak gain
                                    (1/1/0.38/1 = the calibrated look)       */
uniform vec2      u_tune2;       /* x = streak length ×, y = core glow gain  */

/* Soft round sprite: 1 in the core, smooth falloff to the radius. */
float ghost(vec2 p, vec2 c, float r)
{
    float g = smoothstep(r, r * 0.30, length(p - c));
    return g * g;
}

void main()
{
    /* Occlusion: 3×3 depth taps around the light.  Depth is log-encoded and
     * smaller = closer; anything meaningfully in front of the light occludes
     * that tap.  The average gives a soft 0..1 limb fade.  The epsilon is
     * absolute in log-depth units, which is a constant distance-*ratio*
     * tolerance (~5%) at every scale — wide enough that the light's own disc
     * surface (radius ≪ distance) never self-occludes, while any real
     * occluder sits at a much smaller distance and still wins. */
    vec2 texel = 1.0 / vec2(textureSize(u_depth, 0));
    float vis = 0.0;
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            float d = texture(u_depth, u_light_uv + vec2(i, j) * texel).r;
            if (d >= u_light_depth - 0.002) vis += 1.0 / 9.0;
        }

    /* Fade the whole flare out as the light leaves the frame (its depth is
     * unknowable off-screen, and clamped border taps would strobe). */
    vec2  outside = max(-u_light_uv, u_light_uv - 1.0);
    float off     = max(outside.x, outside.y);
    vis *= 1.0 - smoothstep(0.0, 0.06, off);

    float k = u_intensity * vis;
    if (k <= 0.0) { frag_color = vec4(0.0); return; }

    /* Aspect-corrected centred coordinates so all shapes stay circular. */
    vec2 p = (v_uv       - 0.5) * vec2(u_aspect, 1.0);
    vec2 l = (u_light_uv - 0.5) * vec2(u_aspect, 1.0);

    vec3 col = vec3(0.0);

    /* Ghost chain mirrored through the screen centre.  Amplitudes are set for
     * the default intensity ≈ 0.2 (setting 0.25 × flux ramp): softly present,
     * not a light show — the slider at 1.0 is the "music video" end. */
    vec2 c;
    c = l * -0.60; col += vec3(1.00, 0.55, 0.35) * 0.30 * ghost(p, c, 0.070);
    c = l * -0.30; col += vec3(0.45, 1.00, 0.55) * 0.24 * ghost(p, c, 0.038);
    c = l *  0.35; col += vec3(0.40, 0.65, 1.00) * 0.27 * ghost(p, c, 0.055);
    c = l *  0.85; col += vec3(0.85, 0.45, 1.00) * 0.19 * ghost(p, c, 0.100);
    col *= u_tune.x;

    /* Halo ring around the screen centre, weighted toward the side opposite
     * the light (where an internal-reflection halo actually lands). */
    float ringd = length(p) - u_tune.z;
    float ring  = exp(-ringd * ringd / (2.0 * 0.030 * 0.030));
    float lside = (length(p) > 1e-4 && length(l) > 1e-4)
                ? 0.5 - 0.5 * dot(normalize(p), normalize(l)) : 0.5;
    col += u_color * (0.14 * u_tune.y * ring * (0.25 + 0.75 * lside));

    /* Blue anamorphic streak through the light. */
    vec2  s = v_uv - u_light_uv;
    float streak = exp(-abs(s.y) * 240.0)
                 * exp(-abs(s.x) * 4.5 / max(u_tune2.x, 0.05));
    col += vec3(0.35, 0.55, 1.00) * (1.30 * u_tune.w * streak);

    /* Small warm bloom at the light itself (the bloom pass already blazes the
     * core; this just anchors the artifact chain to it). */
    col += u_color * (0.45 * u_tune2.y * exp(-length(p - l) * 9.0));

    frag_color = vec4(col * k, 1.0);
}
