#version 330 core
/*
 * bloom_composite.frag — final pass to the default framebuffer.
 *
 * Pipeline: optional chromatic aberration on the HDR scene -> add bloom ->
 * exposure + filmic tonemap (sRGB encode) -> optional vignette.
 *
 * With u_tonemap == 0, u_chromatic == 0 and u_vignette == 0 this reproduces the
 * original look bit-for-bit (linear HDR written straight out), so every optic is
 * a pure opt-in.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom0;     /* 1/2-res blur — tight core glow              */
uniform sampler2D u_bloom1;     /* 1/4-res blur — mid halo                     */
uniform sampler2D u_bloom2;     /* 1/8-res blur — wide photographic skirt      */
uniform vec3      u_bloom_w;    /* per-level weights, sum-normalised           */
uniform float     u_intensity;
uniform float     u_exposure;   /* linear exposure multiplier (tonemap on)     */
uniform int       u_tonemap;    /* 0 = off (linear), 1 = ACES, 2 = Reinhard    */
uniform float     u_chromatic;  /* lateral CA strength (0 = off)               */
uniform float     u_vignette;   /* corner darkening 0..1 (0 = off)             */
uniform float     u_rel_beta;   /* relativistic effect 0..1 (0 = off)          */
uniform vec2      u_rel_center; /* heading point in UV (0.5,0.5 = look axis)   */

/* Narkowicz 2015 ACES filmic approximation; linear in, display-linear out. */
vec3 tonemap_aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

/* Extended Reinhard on luminance — softer, less contrasty than ACES. */
vec3 tonemap_reinhard(vec3 x) {
    float l  = dot(x, vec3(0.2126, 0.7152, 0.0722));
    float lt = l / (1.0 + l);
    return x * (lt / max(l, 1e-4));
}

void main() {
    vec2 d = v_uv - 0.5;

    /* Relativistic aberration: under motion the sky bunches toward the heading.
     * The heading is the camera's velocity vector projected to screen space
     * (u_rel_center) — not necessarily the look axis, so strafing/looking off
     * the direction of travel offsets the focus.  Content is pulled toward that
     * point; the shift tapers to zero at the frame edge (measured from the true
     * screen centre) so we never sample past the texture border. */
    float rel_r  = length(d) * 1.41421356;           /* 0 centre .. 1 corner   */
    vec2  dc     = v_uv - u_rel_center;               /* vector from heading    */
    float head_r = length(dc) * 1.41421356;          /* 0 at heading .. ~1 far */
    vec2 suv = v_uv;
    if (u_rel_beta > 0.0)
        suv = v_uv + dc * (u_rel_beta * 0.55 * (1.0 - rel_r));

    /* Chromatic aberration: split the channels radially, growing toward the
     * edges (r^2), so the centre stays sharp. */
    vec3 scene;
    if (u_chromatic > 0.0) {
        vec2 off = d * u_chromatic * dot(d, d);
        scene.r = texture(u_scene, suv + off).r;
        scene.g = texture(u_scene, suv).g;
        scene.b = texture(u_scene, suv - off).b;
    } else {
        scene = texture(u_scene, suv).rgb;
    }

    /* Multi-scale bloom: three blur octaves recombined.  The widest level is
     * 1/8 res, so upsample it with a 4-tap tent to hide bilinear diamonds. */
    vec2 texel2 = 1.0 / vec2(textureSize(u_bloom2, 0));
    vec3 wide = ( texture(u_bloom2, v_uv + vec2( texel2.x,  texel2.y) * 0.5).rgb
                + texture(u_bloom2, v_uv + vec2(-texel2.x,  texel2.y) * 0.5).rgb
                + texture(u_bloom2, v_uv + vec2( texel2.x, -texel2.y) * 0.5).rgb
                + texture(u_bloom2, v_uv + vec2(-texel2.x, -texel2.y) * 0.5).rgb ) * 0.25;
    vec3 bloom = texture(u_bloom0, v_uv).rgb * u_bloom_w.x
               + texture(u_bloom1, v_uv).rgb * u_bloom_w.y
               + wide                        * u_bloom_w.z;
    vec3 hdr   = scene + bloom * u_intensity;

    vec3 col;
    if (u_tonemap == 0) {
        col = hdr;                       /* legacy: linear, unmanaged */
    } else {
        hdr *= u_exposure;
        vec3 mapped = (u_tonemap == 2) ? tonemap_reinhard(hdr) : tonemap_aces(hdr);
        /* Black point: space is black. Auto-exposure can lift a mostly-void
         * scene ×3, turning bloom spill + veil glow into a grey floor; sink
         * the darkest display-linear values back to true black. */
        mapped = max(mapped - 0.002, 0.0);
        col = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2));  /* sRGB encode */
    }

    /* Relativistic Doppler + beaming: blueshift ahead (toward the heading) /
     * redshift behind it as a hue shift, and beaming that only *brightens*
     * ahead — it never dims the edges, so it can't act as a second vignette (the
     * optical vignette stays the only corner darkening on the camera). Measured
     * from the heading point so it tracks the velocity vector, not the centre. */
    if (u_rel_beta > 0.0) {
        float shift = u_rel_beta * (0.5 - clamp(head_r, 0.0, 1.0));  /* +.5β .. -.5β */
        col.r *= 1.0 - 0.35 * shift;
        col.b *= 1.0 + 0.35 * shift;
        col   *= 1.0 + 0.22 * max(shift, 0.0);       /* beaming: brighten ahead only */
        col    = max(col, vec3(0.0));
    }

    if (u_vignette > 0.0) {
        float r = length(d) * 1.41421356;            /* 0 centre .. 1 corner */
        col *= 1.0 - u_vignette * smoothstep(0.5, 1.0, r);
    }

    frag_color = vec4(col, 1.0);
}
