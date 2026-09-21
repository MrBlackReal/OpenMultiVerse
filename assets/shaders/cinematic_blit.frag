#version 330 core
/*
 * cinematic_blit.frag — accumulate / resolve pass for the cinematic renderer.
 *
 * One shader serves both halves of the accumulation loop (cinematic.c):
 *   accumulate — u_resolve = 0, u_scale = 1, drawn with additive blending
 *                (GL_ONE/GL_ONE) into the RGBA32F accumulator, once per
 *                jittered sub-frame. Nothing but a copy.
 *   resolve    — u_resolve = 1, u_scale = 1/samples, blending off, into the
 *                default framebuffer: the accumulated sum becomes the mean and
 *                the film grade is applied on top.
 *
 * Sub-frames are accumulated AFTER post.c's tonemap, i.e. in display space.
 * That is deliberate: averaging linear HDR and tonemapping afterwards lets one
 * blown-out star dominate every sample it touches and produces fireflies along
 * high-contrast edges — which a starfield is made of. Averaging the tonemapped
 * result is what actually antialiases it.
 *
 * The grade runs at resolve and only at resolve. Grain in particular must not
 * go through the accumulator: N jittered samples of a noise field average back
 * out to flat grey, so accumulated grain is no grain at all.
 *
 * Alpha is forced to 1: the accumulator is additive, so a summed alpha would
 * read as `samples` and means nothing.
 */
in vec2 v_uv;

uniform sampler2D u_src;
uniform float     u_scale;      /* 1 when accumulating, 1/samples at resolve  */
uniform int       u_resolve;    /* 0 = plain copy, 1 = apply the grade        */

/* Grade (all neutral at their defaults, so the pass is a no-op untouched). */
uniform float u_contrast;       /* about the 0.18 pivot; 1 = unchanged        */
uniform float u_saturation;     /* 0 = mono, 1 = unchanged                    */
uniform float u_lift;           /* raise the black level (film blacks)        */
uniform float u_warmth;         /* -1 cool .. +1 warm                         */
uniform float u_grain;          /* amount; 0 = off                            */
uniform float u_grain_seed;     /* per-frame, so grain moves                  */
uniform float u_letterbox;      /* visible band half-height in UV; 0 = off    */

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

/* Cheap hash noise. Good enough for grain, which wants to look stochastic
 * rather than be statistically rigorous. */
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

out vec4 frag;

void main() {
    vec3 c = texture(u_src, v_uv).rgb * u_scale;

    if (u_resolve == 1) {
        /* Letterbox first: the bars are matte, not graded film. */
        if (u_letterbox > 0.0 && abs(v_uv.y - 0.5) > u_letterbox) {
            frag = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        /* Lift: film blacks are never truly black. Compresses toward white so
         * highlights are untouched. */
        c = c + u_lift * (1.0 - c);

        /* Contrast about middle grey rather than 0.5 — 0.18 is the scene-
         * referred mid-point the ACES curve in post.c is built around, so
         * turning contrast up darkens shadows instead of crushing midtones. */
        c = (c - 0.18) * u_contrast + 0.18;

        /* Saturation about luma. */
        float l = dot(c, LUMA);
        c = mix(vec3(l), c, u_saturation);

        /* Colour temperature: push red against blue, leaving green as the
         * hinge so the overall luminance barely moves. */
        c *= vec3(1.0 + 0.12 * u_warmth, 1.0, 1.0 - 0.12 * u_warmth);

        /* Grain, strongest in the midtones and nearly absent in blacks and
         * blown highlights — which is how film actually grains, and it keeps
         * noise out of the empty space that is most of this frame. */
        if (u_grain > 0.0) {
            float n  = hash(v_uv * vec2(1024.0, 1024.0) + u_grain_seed) - 0.5;
            float lm = dot(c, LUMA);
            float w  = 4.0 * lm * (1.0 - lm);       /* peaks at mid grey */
            c += n * u_grain * w;
        }

        c = max(c, vec3(0.0));
    }

    frag = vec4(c, 1.0);
}
