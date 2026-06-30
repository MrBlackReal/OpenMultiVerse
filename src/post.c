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

static int    s_ok        = 0;     /* shaders compiled */
static int    s_enabled   = 1;     /* user toggle */
static float  s_threshold = 0.80f;
static float  s_intensity = 1.10f;

static int    s_w = 0, s_h = 0;    /* size the targets were built for */
static int    s_bw = 0, s_bh = 0;  /* half-res blur size */

static GLuint s_scene_fbo = 0, s_scene_tex = 0, s_scene_depth = 0;
static GLuint s_blur_fbo[2] = {0, 0}, s_blur_tex[2] = {0, 0};

static GLuint s_sh_bright = 0, s_sh_blur = 0, s_sh_comp = 0;
static GLint  s_u_bright_scene, s_u_bright_thresh;
static GLint  s_u_blur_tex, s_u_blur_dir;
static GLint  s_u_comp_scene, s_u_comp_bloom, s_u_comp_intensity;

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
    if (s_scene_tex)   { glDeleteTextures(1, &s_scene_tex);        s_scene_tex = 0; }
    if (s_scene_depth) { glDeleteRenderbuffers(1, &s_scene_depth); s_scene_depth = 0; }
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
    glGenRenderbuffers(1, &s_scene_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_scene_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, s_scene_depth);
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
    draw_quad();

    /* Restore state expected by the UI / next frame. */
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}
