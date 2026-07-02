/*
 * post.c — HDR bloom post-processing.
 *
 * The scene is rendered into a full-resolution RGBA16F framebuffer so bright,
 * additively-blended sources (star glare, dots, emissive surfaces) can exceed
 * 1.0.  A bright-pass extracts those into a half-resolution target, a separable
 * Gaussian blurs it through a ping-pong pair, and a final pass composites the
 * blurred glow additively over the scene to the default framebuffer.
 *
 * Targets are (re)created lazily to match the current window size, so resizing
 * the window just rebuilds them on the next frame.
 */
#include "post.h"
#include "common.h"
#include "gl_utils.h"
#include <stdio.h>
#include <math.h>

static int    s_ok        = 0;     /* shaders compiled */
static int    s_enabled   = 1;     /* user toggle */
static float  s_threshold = 0.80f;
static float  s_intensity = 1.10f;

/* Tonemap: 0 = off (legacy linear), 1 = ACES, 2 = Reinhard. Seeded here, then
 * overridden from g_settings (tonemap_mode/exposure) at startup. */
static int    s_tonemap   = 1;
static float  s_exposure  = 0.76f;

/* Lens optics (all opt-in; 0 = no effect). Seeded from g_settings at startup. */
static int    s_auto_exp  = 0;       /* auto-exposure adaptation toggle        */
static float  s_chromatic = 0.0f;    /* lateral chromatic aberration strength  */
static float  s_vignette  = 0.0f;    /* corner darkening 0..1                  */
static float  s_adapted   = 1.0f;    /* smoothed auto-exposure factor (runtime)*/
static float  s_rel_beta  = 0.0f;    /* relativistic optics 0..1 (set per frame)*/
static float  s_rel_cx    = 0.5f;    /* heading point in UV (velocity vector)   */
static float  s_rel_cy    = 0.5f;

static int    s_w = 0, s_h = 0;    /* size the targets were built for */
static int    s_bw = 0, s_bh = 0;  /* half-res blur size */

static GLuint s_scene_fbo = 0, s_scene_tex = 0, s_scene_depth = 0;
static GLuint s_blur_fbo[2] = {0, 0}, s_blur_tex[2] = {0, 0};

static GLuint s_sh_bright = 0, s_sh_blur = 0, s_sh_comp = 0;
static GLint  s_u_bright_scene, s_u_bright_thresh;
static GLint  s_u_blur_tex, s_u_blur_dir;
static GLint  s_u_comp_scene, s_u_comp_bloom, s_u_comp_intensity;
static GLint  s_u_comp_exposure, s_u_comp_tonemap;
static GLint  s_u_comp_chromatic, s_u_comp_vignette, s_u_comp_rel_beta;
static GLint  s_u_comp_rel_center;

static GLuint s_quad_vao = 0, s_quad_vbo = 0;

static GLuint make_color_tex(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void destroy_targets(void)
{
    if (s_scene_tex)   { glDeleteTextures(1, &s_scene_tex);   s_scene_tex = 0; }
    if (s_scene_depth) { glDeleteTextures(1, &s_scene_depth); s_scene_depth = 0; }
    if (s_scene_fbo)   { glDeleteFramebuffers(1, &s_scene_fbo);    s_scene_fbo = 0; }
    for (int i = 0; i < 2; i++) {
        if (s_blur_tex[i]) { glDeleteTextures(1, &s_blur_tex[i]);     s_blur_tex[i] = 0; }
        if (s_blur_fbo[i]) { glDeleteFramebuffers(1, &s_blur_fbo[i]); s_blur_fbo[i] = 0; }
    }
}

static int create_targets(int w, int h)
{
    destroy_targets();
    s_w = w; s_h = h;
    s_bw = w / 2 > 1 ? w / 2 : 1;
    s_bh = h / 2 > 1 ? h / 2 : 1;

    /* Full-res HDR scene target with depth (the scene pass needs depth). */
    glGenFramebuffers(1, &s_scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_scene_fbo);
    s_scene_tex = make_color_tex(w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_scene_tex, 0);
    /* Depth as a texture (not a renderbuffer) so volumetric passes can
     * sample the opaque scene's depth — the half-res galaxy raymarch
     * terminates its march at scene depth to embed planets correctly. */
    glGenTextures(1, &s_scene_depth);
    glBindTexture(GL_TEXTURE_2D, s_scene_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, s_scene_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[post] scene FBO incomplete; bloom disabled\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    /* Half-res ping-pong blur targets (colour only). */
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &s_blur_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_blur_fbo[i]);
        s_blur_tex[i] = make_color_tex(s_bw, s_bh);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_blur_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[post] blur FBO incomplete; bloom disabled\n");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

void post_init(void)
{
    s_sh_bright = gl_shader_load("assets/shaders/post_quad.vert",
                                 "assets/shaders/bloom_bright.frag");
    s_sh_blur   = gl_shader_load("assets/shaders/post_quad.vert",
                                 "assets/shaders/bloom_blur.frag");
    s_sh_comp   = gl_shader_load("assets/shaders/post_quad.vert",
                                 "assets/shaders/bloom_composite.frag");
    if (!s_sh_bright || !s_sh_blur || !s_sh_comp) {
        fprintf(stderr, "[post] bloom shaders failed to load; bloom disabled\n");
        s_ok = 0;
        return;
    }
    s_u_bright_scene   = glGetUniformLocation(s_sh_bright, "u_scene");
    s_u_bright_thresh  = glGetUniformLocation(s_sh_bright, "u_threshold");
    s_u_blur_tex       = glGetUniformLocation(s_sh_blur,   "u_tex");
    s_u_blur_dir       = glGetUniformLocation(s_sh_blur,   "u_dir");
    s_u_comp_scene     = glGetUniformLocation(s_sh_comp,   "u_scene");
    s_u_comp_bloom     = glGetUniformLocation(s_sh_comp,   "u_bloom");
    s_u_comp_intensity = glGetUniformLocation(s_sh_comp,   "u_intensity");
    s_u_comp_exposure  = glGetUniformLocation(s_sh_comp,   "u_exposure");
    s_u_comp_tonemap   = glGetUniformLocation(s_sh_comp,   "u_tonemap");
    s_u_comp_chromatic = glGetUniformLocation(s_sh_comp,   "u_chromatic");
    s_u_comp_vignette  = glGetUniformLocation(s_sh_comp,   "u_vignette");
    s_u_comp_rel_beta  = glGetUniformLocation(s_sh_comp,   "u_rel_beta");
    s_u_comp_rel_center= glGetUniformLocation(s_sh_comp,   "u_rel_center");

    /* Fullscreen quad (two triangles) in NDC. */
    static const float quad[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
    };
    s_quad_vao = gl_vao_create();
    s_quad_vbo = gl_vbo_create(sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    s_ok = 1;
}

int post_available(void) { return s_ok; }
int post_enabled(void)   { return s_ok && s_enabled; }

unsigned int post_scene_depth_tex(void)
{
    return post_enabled() && s_scene_fbo ? s_scene_depth : 0;
}

void post_get_bloom(int *enabled, float *threshold, float *intensity)
{
    if (enabled)   *enabled   = s_enabled;
    if (threshold) *threshold = s_threshold;
    if (intensity) *intensity = s_intensity;
}

void post_set_bloom(int enabled, float threshold, float intensity)
{
    s_enabled   = enabled ? 1 : 0;
    s_threshold = threshold;
    s_intensity = intensity;
}

void post_get_tonemap(int *mode, float *exposure)
{
    if (mode)     *mode     = s_tonemap;
    if (exposure) *exposure = s_exposure;
}

void post_set_tonemap(int mode, float exposure)
{
    s_tonemap  = (mode < 0) ? 0 : (mode > 2 ? 2 : mode);
    s_exposure = exposure;
}

void post_get_optics(int *auto_exposure, float *chromatic, float *vignette)
{
    if (auto_exposure) *auto_exposure = s_auto_exp;
    if (chromatic)     *chromatic     = s_chromatic;
    if (vignette)      *vignette      = s_vignette;
}

void post_set_optics(int auto_exposure, float chromatic, float vignette)
{
    s_auto_exp  = auto_exposure ? 1 : 0;
    s_chromatic = chromatic < 0.0f ? 0.0f : chromatic;
    s_vignette  = vignette  < 0.0f ? 0.0f : (vignette > 1.0f ? 1.0f : vignette);
}

void post_set_relativistic(float beta, float cx, float cy)
{
    s_rel_beta = beta < 0.0f ? 0.0f : (beta > 1.0f ? 1.0f : beta);
    s_rel_cx   = cx;
    s_rel_cy   = cy;
}

/* Auto-exposure: average the scene luminance from the 1x1 top mip, then ease a
 * normalised factor toward a target so going from a dark void to a bright field
 * self-balances.  Result multiplies the manual exposure, so the user's exposure
 * value stays a meaningful anchor (comp). Returns the factor to multiply in. */
static float auto_exposure_factor(void)
{
    int max_dim = s_w > s_h ? s_w : s_h;
    if (max_dim < 1) return 1.0f;
    int last = (int)floorf(log2f((float)max_dim));   /* 1x1 mip level */

    glBindTexture(GL_TEXTURE_2D, s_scene_tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    float px[4] = { 0, 0, 0, 0 };
    glGetTexImage(GL_TEXTURE_2D, last, GL_RGBA, GL_FLOAT, px);

    float lum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
    /* K is a reference luminance: target factor ~1 for a typical bright scene.
     * Clamp so a near-black void can only brighten so far. */
    const float K = 0.40f;
    float target = K / (lum > 1e-4f ? lum : 1e-4f);
    if (target < 0.30f) target = 0.30f;
    if (target > 3.00f) target = 3.00f;

    /* Frame-rate-independent enough for an eye-adaptation feel. */
    s_adapted += (target - s_adapted) * 0.04f;
    return s_adapted;
}

void post_begin(void)
{
    if (!post_enabled()) return;
    if (s_w != WIN_W || s_h != WIN_H || !s_scene_fbo) {
        if (!create_targets(WIN_W, WIN_H)) { s_ok = 0; return; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_scene_fbo);
    glViewport(0, 0, s_w, s_h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void draw_quad(void)
{
    glBindVertexArray(s_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void post_end(void)
{
    if (!post_enabled() || !s_scene_fbo) return;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    /* Final exposure. Computed up front (before any texture units are bound for
     * the passes below) because auto_exposure_factor() generates mips on and
     * reads back the scene texture. */
    float exposure = s_exposure;
    if (s_tonemap != 0 && s_auto_exp) {
        glActiveTexture(GL_TEXTURE0);
        exposure *= auto_exposure_factor();
    }

    /* 1. Bright-pass: full-res scene -> half-res blur[0]. */
    glViewport(0, 0, s_bw, s_bh);
    glBindFramebuffer(GL_FRAMEBUFFER, s_blur_fbo[0]);
    glUseProgram(s_sh_bright);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scene_tex);
    glUniform1i(s_u_bright_scene, 0);
    glUniform1f(s_u_bright_thresh, s_threshold);
    draw_quad();

    /* 2. Separable Gaussian, ping-ponging between the two blur targets. */
    glUseProgram(s_sh_blur);
    glUniform1i(s_u_blur_tex, 0);
    GLuint src_tex = s_blur_tex[0];
    int    dst     = 1;
    const int PASSES = 10;   /* 5 horizontal + 5 vertical, interleaved */
    for (int i = 0; i < PASSES; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_blur_fbo[dst]);
        float dx = (i % 2 == 0) ? 1.0f / (float)s_bw : 0.0f;
        float dy = (i % 2 == 0) ? 0.0f : 1.0f / (float)s_bh;
        glUniform2f(s_u_blur_dir, dx, dy);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        draw_quad();
        src_tex = s_blur_tex[dst];
        dst     = 1 - dst;
    }

    /* 3. Composite scene + blurred glow to the default framebuffer. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WIN_W, WIN_H);
    glUseProgram(s_sh_comp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scene_tex);
    glUniform1i(s_u_comp_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(s_u_comp_bloom, 1);
    glUniform1f(s_u_comp_intensity, s_intensity);
    glUniform1f(s_u_comp_exposure, exposure);
    glUniform1i(s_u_comp_tonemap, s_tonemap);
    glUniform1f(s_u_comp_chromatic, s_chromatic);
    glUniform1f(s_u_comp_vignette, s_vignette);
    glUniform1f(s_u_comp_rel_beta, s_rel_beta);
    glUniform2f(s_u_comp_rel_center, s_rel_cx, s_rel_cy);
    draw_quad();

    /* Restore state expected by the UI / next frame. */
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}
