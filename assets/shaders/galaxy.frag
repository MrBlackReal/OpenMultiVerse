#version 330 core
/*
 * galaxy.frag — volumetric galaxy raymarch (roadmap Layer 4.2).
 *
 * Carried by nebula.vert (billboard or fullscreen quad); a screen-space ray is
 * intersected with the unit bounding sphere and marched front-to-back, like
 * nebula.frag — one representation from a few-pixel backdrop to a fly-through.
 *
 * The density model is galaxy-specific (u_type):
 *   0 SPIRAL      exponential stellar disc (thin, flaring) + warm bulge, two
 *                 logarithmic spiral arms with FBM star-forming knots, and
 *                 absorbing dust lanes — alpha with no emission — hugging the
 *                 arms' inner edges near the midplane, so an edge-on disc gets
 *                 the classic dark stripe (Sombrero) with no special case.
 *                 Differential rotation (flat curve, ω ∝ 1/r) shears the
 *                 pattern on u_time.
 *   1 ELLIPTICAL  smooth, steep-cored glow (de-Vaucouleurs-ish), old warm
 *                 population, barely any structure.
 *   2 IRREGULAR   torn clumpy cloud (LMC/SMC): ragged noise-warped outline,
 *                 an off-centre warm stellar bar, patchy blue starlight with
 *                 bright pink HII complexes, and dark dust patches.
 *
 * Output is premultiplied; blended "over" at log depth like the nebulae.
 */
in vec2 v_uv;

uniform vec3  u_oc;          /* camera-relative centre (world units)     */
uniform vec3  u_color;       /* stellar-population tint                  */
uniform float u_radius;      /* bounding radius (world units)            */
uniform float u_density;     /* overall opacity / brightness (artistic)  */
uniform int   u_steps;
uniform int   u_type;        /* morphology, see header                   */
uniform vec3  u_axis;        /* disc spin axis (unit, world frame)       */
uniform float u_seed;
uniform float u_time;        /* seconds; drives rotational shear         */
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform vec3  u_cam_fwd;
uniform float u_fov_tan;
uniform float u_aspect;
uniform vec2  u_screen;
uniform sampler2D u_scene_depth;   /* opaque scene log depth (half-res path) */
uniform float u_use_scene_depth;   /* >0.5: occlusion done here, not by the
                                    * depth test — the half-res target has no
                                    * depth buffer, so the march is clipped to
                                    * the scene's eye depth instead (planets
                                    * embed correctly in the volume)          */

out vec4 frag_color;

const float FAR   = DEPTH_FAR;
const float BOUND = 1.0;

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

float fbm3(vec3 p) {
    float v = vnoise(p) * 0.5;
    p = p * 2.03 + vec3(3.7, 1.9, 2.6);  v += vnoise(p) * 0.25;
    p = p * 2.03 + vec3(1.9, 4.2, 2.1);  v += vnoise(p) * 0.125;
    return v / 0.875;
}

float fbm2(vec3 p) {
    float v = vnoise(p) * 0.6;
    p = p * 2.11 + vec3(4.1, 2.3, 3.4);  v += vnoise(p) * 0.3;
    return v / 0.9;
}

/*
 * galaxy_sample — emission colour (premultiplied weight), emission density,
 * and pure absorption (dust) at unit-sphere position p.
 */
void galaxy_sample(vec3 p, float rr, vec3 seedv,
                   out vec3 col, out float dens, out float dust)
{
    col  = u_color;
    dens = 0.0;
    dust = 0.0;

    if (u_type == 1) {                               /* ELLIPTICAL */
        /* Steep bright core, long faint envelope; old, smooth, warm. */
        float e = exp(-pow(rr / 0.42, 0.62) * 3.2);
        float n = fbm2(p * 3.0 + seedv);
        dens = e * (0.85 + 0.15 * n) * 1.5;
        col  = u_color * mix(vec3(0.95, 0.88, 0.74), vec3(1.0, 0.98, 0.9),
                             smoothstep(0.25, 0.0, rr));
        return;
    }

    /* Disc frame: height above the midplane + in-plane radius/azimuth. */
    float h  = dot(p, u_axis);
    vec3  pr = p - u_axis * h;
    float r  = length(pr);
    vec3  t1 = normalize(cross(u_axis, vec3(0.31, 1.0, 0.71)));
    vec3  t2 = cross(u_axis, t1);
    float phi = atan(dot(pr, t2), dot(pr, t1));

    if (u_type == 2) {                               /* IRREGULAR */
        /* In-plane coords for the stellar bar. */
        float x1 = dot(pr, t1), x2 = dot(pr, t2);

        /* Ragged outline: large-scale noise warps the envelope radius so
         * the cloud reads as a torn lump from outside, not a smooth ball. */
        float lump = fbm2(p * 2.1 + seedv * 1.7);
        float env  = exp(-pow(r / (0.42 + 0.30 * lump), 2.2)
                         - pow(h / 0.30, 2.0));

        /* Off-centre elongated stellar bar (the LMC's defining feature) —
         * old warm population, slightly displaced from the cloud centre. */
        float bar = 1.5 * exp(-pow((x1 - 0.07) / 0.34, 2.0)
                              - pow( x2         / 0.115, 2.0)
                              - pow( h          / 0.13,  2.0));

        /* Patchy young population: low base fill, strong clumps, and rare
         * bright pink HII complexes (30 Doradus-class at the top end). */
        float n   = fbm3(p * 3.2 + seedv);
        float k   = smoothstep(0.48, 0.85, n);
        float hii = smoothstep(0.68, 0.86, fbm2(p * 4.6 - seedv));
        dens = env * (0.05 + 1.9 * k * k + 2.6 * hii) + bar;

        /* Torn dark dust patches for structure, absent from the bar core. */
        dust = smoothstep(0.58, 0.82, fbm2(p * 3.9 + seedv * 2.3))
             * env * (1.0 - clamp(bar, 0.0, 1.0)) * 0.9;

        float bw    = clamp(bar / max(dens, 1e-5), 0.0, 1.0);
        vec3  young = mix(vec3(0.72, 0.80, 1.00), vec3(1.0, 0.50, 0.55), hii);
        col = u_color * mix(young * (0.55 + 0.9 * k),
                            vec3(1.0, 0.90, 0.72), bw);
        return;
    }

    /* SPIRAL --------------------------------------------------------------
     * Differential rotation: flat rotation curve → ω ∝ 1/r. The whole
     * pattern (arms + clumps + dust) shears with time. */
    float rot = u_time * 0.010 / max(r, 0.10);
    float ph  = phi + rot;

    /* Two logarithmic arms: constant pitch in log-radius. */
    float wind = log(max(r, 0.035)) * 3.6;
    float armw = ph * 2.0 - wind;
    float arm  = pow(0.5 + 0.5 * cos(armw), 2.6);

    /* Thin exponential disc, slightly flaring outward; warm compact bulge. */
    float disc  = exp(-r / 0.30) * exp(-abs(h) / (0.035 + 0.09 * r * r))
                * smoothstep(1.0, 0.85, rr);
    float bulge = 2.4 * exp(-pow(rr / 0.14, 2.0));

    /* Star-forming knots along the arms (noise in the co-rotating frame so
     * clumps ride the shear instead of the arms sweeping through them). */
    float cr = cos(rot), sr = sin(rot);
    vec3  prot = pr * cr + cross(u_axis, pr) * sr + u_axis * h;
    float n     = fbm3(prot * 4.6 + seedv);
    float knots = smoothstep(0.55, 0.88, n) * arm;

    /* Star-cloud mottling: the band seen from inside is patchy star clouds,
     * not an airbrushed gradient. */
    float cloud = 0.60 + 0.80 * fbm2(prot * 3.1 + seedv * 1.3);

    /* The disc needs ~2.4x the naive weight to read against the compact
     * bulge from outside (verified face-on + edge-on on the Milky Way). */
    dens = disc * cloud * (0.38 + 2.8 * arm + 3.8 * knots) + bulge;

    /* Dust lanes: absorption on the arms' inner edges, pinned to the
     * midplane, absent from the bulge core. */
    float lane = pow(0.5 + 0.5 * cos(armw + 1.1), 3.0);
    float dn   = 0.55 + 0.45 * fbm2(prot * 6.0 - seedv);
    /* The lane rides slightly below the midplane, so edge-on it silhouettes
     * against the bright disc/bulge behind instead of coinciding with them. */
    dust = lane * exp(-abs(h + 0.028) / 0.030) * exp(-r / 0.40)
         * smoothstep(0.06, 0.18, r) * dn * 2.2;

    /* Great Rift: a ragged equatorial dust sheet independent of the arm
     * phase, so the band seen from inside is split lengthwise by a dark
     * lane (and an edge-on disc keeps its stripe between arm crossings). */
    dust += exp(-abs(h + 0.008) / 0.016) * exp(-r / 0.45)
          * smoothstep(0.04, 0.12, r) * dn * 1.1;

    /* Dust extinguishes the starlight embedded in it too, not just what is
     * behind — this is what carves the classic dark stripe across an
     * edge-on disc (Sombrero) instead of the midplane glowing through. */
    dens *= exp(-dust * 2.4);

    /* Population colours: warm bulge, cool blue-white arms, pink HII knots.
     * The weight uses the *extinguished* bulge — same dust as dens — or the
     * warm tint smears along every dust-dimmed midplane region. */
    float bw   = clamp(bulge * exp(-dust * 2.4) / max(dens, 1e-5), 0.0, 1.0);
    float hii  = smoothstep(0.80, 0.94, n) * arm;
    vec3  dcol = mix(vec3(0.72, 0.80, 1.00), vec3(1.0, 0.55, 0.60), hii);
    col = u_color * mix(dcol, vec3(1.0, 0.83, 0.56), bw);
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

    /* Discs are thin: clip the march to the slab that actually holds density
     * (|h| <= H about the midplane), so the fixed step budget samples the
     * disc instead of empty bounding-sphere volume. Without this a face-on
     * disc seen from outside catches ~1 of the samples and dissolves into a
     * dim smudge. Ellipticals are spheroidal — no slab. */
    if (u_type != 1) {
        float H  = (u_type == 2) ? 0.70 : 0.50;
        float h0 = dot(oc, u_axis);
        float dh = dot(rd, u_axis);
        if (abs(dh) > 1e-5) {
            float ta = (-H - h0) / dh;
            float tb = ( H - h0) / dh;
            tEnter = max(tEnter, min(ta, tb));
            tExit  = min(tExit,  max(ta, tb));
            if (tExit <= tEnter) discard;
        } else if (abs(h0) > H) {
            discard;                     /* parallel ray outside the slab */
        }
    }

    /* Half-res path: clip the march to the opaque scene's depth (sampled in
     * normalized UV, so the full-res depth texture maps onto the half-res
     * target). Glow in front of a planet still draws; glow behind does not. */
    if (u_use_scene_depth > 0.5) {
        float sd = texture(u_scene_depth, gl_FragCoord.xy / u_screen).r;
        if (sd < 1.0) {                              /* 1.0 = cleared (sky) */
            float scene_eye = exp2(sd * log2(FAR + 1.0)) - 1.0;
            float ray_cos   = max(dot(rd, u_cam_fwd), 1e-4);
            tExit = min(tExit, scene_eye / ray_cos / radius);
            if (tExit <= tEnter) discard;
        }
    }

    int   steps   = max(u_steps, 6);
    float stepLen = (tExit - tEnter) / float(steps);
    float jitter  = hash21(gl_FragCoord.xy);
    float accumA  = 0.0;
    vec3  accumC  = vec3(0.0);

    vec3 seedv = vec3(u_seed * 7.0, u_seed * 3.0, -u_seed * 5.0);

    for (int i = 0; i < steps; i++) {
        float t = tEnter + (float(i) + jitter) * stepLen;
        vec3  p = oc + rd * t;
        float rr = length(p);
        if (rr > 1.0) continue;

        vec3  col;
        float dens, dust;
        galaxy_sample(p, rr, seedv, col, dens, dust);

        /* Dust first: absorption only — darkens everything behind it.
         * Extinction scales with sqrt(density): the Milky Way's inside
         * veil dims the *emission* ~7x for taste, but the dark rift must
         * still carve the band, so absorption falls off much slower. */
        float ad = clamp(dust * stepLen * 5.5 * sqrt(max(u_density, 0.0)),
                         0.0, 0.45);
        accumA += ad * (1.0 - accumA);

        if (dens > 0.0015) {
            float a = clamp(dens * stepLen * 2.1 * u_density, 0.0, 0.30);
            accumC += col * a * (1.0 - accumA);
            accumA += a * (1.0 - accumA);
        }
        if (accumA > 0.985) break;
    }

    accumA *= smoothstep(0.0, 0.02, accumA);
    if (accumA < 0.0008) discard;

    float eye_depth = (tEnter * radius) * dot(rd, u_cam_fwd);
    eye_depth = clamp(eye_depth, 0.0, FAR * 0.9995);
    gl_FragDepth = log2(eye_depth + 1.0) / log2(FAR + 1.0);

    frag_color = vec4(accumC, accumA);
}
