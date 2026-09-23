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
uniform float u_radius;      /* bounding radius (world units)            */
uniform float u_density;     /* overall opacity / brightness (artistic)  */
uniform int   u_steps;
uniform float u_seed;
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

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

#include "galaxy_model.glsl"

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
        float dens_raw_, bulge_w_, knots_;
        galaxy_sample(p, rr, seedv, 0.0015, 0.0004, col, dens, dust,
                      dens_raw_, bulge_w_, knots_);

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

    /* Star veil (render.c): emission drowns in a nearby star's glare, as in
     * nebula.frag. Applied here rather than in the half-res composite so the
     * direct path (the black-hole lensing environment) is veiled identically. */
    accumC *= veil_vis(rd, max(max(accumC.r, accumC.g), accumC.b) * VEIL_DIFFUSE);
    frag_color = vec4(accumC, accumA);
}
