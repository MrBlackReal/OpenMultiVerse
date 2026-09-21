/*
 * cinematic.c — cinematic renderer + deterministic film-out (see cinematic.h
 * for the API contract and CINEMATIC.md for the full design).
 *
 * Phase 1 scope: the film-out pipeline end to end — fixed-timestep loop,
 * offscreen render at arbitrary resolution, accumulation buffer with sub-pixel
 * jitter (supersampled AA), PBO readback, ffmpeg pipe with a PPM fallback, and
 * audio mux.  Lens/shutter jitter (DOF + motion blur), the grade and the shot
 * system land on top of this in phases 2-3.
 */
#include "cinematic.h"
#include "cinema_blur.h"
#include "gl_utils.h"
#include "post.h"
#include "body.h"      /* g_cam_prox, g_bodies — focus targets */
#include "camera.h"    /* g_cam */
#include "universe.h"  /* g_field_star_begin/_end */

#include <errno.h>
#include <unistd.h>    /* isatty */

CinematicConfig g_cine;

/* ---- GL state -------------------------------------------------------------
 * s_sub_*  one jittered sub-frame, RGBA16F: post.c's composite is redirected
 *          here instead of the back buffer.  No depth attachment — post_end()
 *          composites with depth test and depth writes off.
 * s_acc_*  the RGBA32F accumulator the sub-frames are summed into.  32-bit
 *          because 64 accumulated sub-frames overflow half-float precision
 *          long before they overflow its range. */
static int    s_ready;
static int    s_w, s_h;
static GLuint s_sub_fbo, s_sub_tex;
static GLuint s_acc_fbo, s_acc_tex;
static GLuint s_sh_blit;
static GLint  s_u_src, s_u_scale, s_u_resolve;
static GLint  s_u_contrast, s_u_saturation, s_u_lift, s_u_warmth;
static GLint  s_u_grain, s_u_grain_seed, s_u_letterbox;
static long   s_frame_counter;        /* drives the grain seed */
static GLuint s_vao, s_vbo;

/* Effective sub-frame count: 1 when accumulation is unavailable, which makes
 * every accumulate/resolve entry point a no-op and sends post.c straight to
 * the back buffer as it always did. */
static int    s_nsub = 1;

/* ---- readback / encoder --------------------------------------------------- */
#define PBO_RING 2
static GLuint s_pbo[PBO_RING];
static int    s_pbo_ok;
static long   s_frames_issued;    /* readbacks started                        */
static long   s_frames_written;   /* frames handed to the encoder             */
static FILE  *s_enc;
static int    s_enc_is_pipe;      /* pclose vs fclose                          */
static unsigned char *s_flip_row; /* scratch for the PPM fallback's flip       */
static Uint64 s_t_start;
static Uint64 s_t_last_report;

/* ------------------------------------------------------------------ config */

void cinematic_defaults(void)
{
    memset(&g_cine, 0, sizeof g_cine);
    g_cine.width       = 1920;
    g_cine.height      = 1080;
    g_cine.fps         = 60;
    g_cine.duration    = 30.0;
    g_cine.samples     = 0;       /* 0 = "unset": resolved in cinematic_init */
    g_cine.crf         = 16;
    g_cine.audio       = 1;
    snprintf(g_cine.audio_path, sizeof g_cine.audio_path, "assets/soundtrack.ogg");
}

void cinematic_preset_film(void)
{
    g_cine.width   = 3840;
    g_cine.height  = 2160;
    g_cine.fps     = 24;
    g_cine.samples = 64;
}

void cinematic_preset_draft(void)
{
    g_cine.width   = 1280;
    g_cine.height  = 720;
    g_cine.fps     = 30;
    g_cine.samples = 4;
}

/* ------------------------------------------------------------------ targets */

static void destroy_targets(void)
{
    if (s_sub_fbo) { glDeleteFramebuffers(1, &s_sub_fbo); s_sub_fbo = 0; }
    if (s_acc_fbo) { glDeleteFramebuffers(1, &s_acc_fbo); s_acc_fbo = 0; }
    if (s_sub_tex) { glDeleteTextures(1, &s_sub_tex);     s_sub_tex = 0; }
    if (s_acc_tex) { glDeleteTextures(1, &s_acc_tex);     s_acc_tex = 0; }
}

static GLuint make_target(GLuint *fbo, GLenum internal, int w, int h)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return tex;
}

static int create_targets(int w, int h)
{
    destroy_targets();
    s_sub_tex = make_target(&s_sub_fbo, GL_RGBA16F, w, h);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[cinematic] sub-frame FBO incomplete; accumulation off\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_targets();
        return 0;
    }
    s_acc_tex = make_target(&s_acc_fbo, GL_RGBA32F, w, h);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[cinematic] accumulator FBO incomplete; accumulation off\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_targets();
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_w = w; s_h = h;
    return 1;
}

/* ------------------------------------------------------------------ init */

void cinematic_init(void)
{
    if (!g_cine.enabled) return;

    /* Resolve the sample count now that the mode is known: film-out wants
     * quality, live wants to stay interactive. An explicit --samples wins. */
    if (g_cine.samples <= 0)
        g_cine.samples = g_cine.filming ? 32
                       : (g_settings.cine_live_samples > 0
                          ? g_settings.cine_live_samples : 4);

    /* Accumulation redirects post.c's composite, so it needs post available.
     * Without it the scene renders straight to the back buffer and the only
     * honest thing to do is fall back to one sample per frame — the film still
     * gets made, just aliased. */
    if (!post_enabled()) {
        fprintf(stderr, "[cinematic] post-processing unavailable; "
                        "accumulation disabled (samples forced to 1)\n");
        s_nsub  = 1;
        s_ready = 1;
        return;
    }

    if (!create_targets(WIN_W, WIN_H)) { s_nsub = 1; s_ready = 1; return; }

    s_sh_blit = gl_shader_load("assets/shaders/post_quad.vert",
                               "assets/shaders/cinematic_blit.frag");
    if (!s_sh_blit) {
        fprintf(stderr, "[cinematic] blit shader failed; accumulation disabled\n");
        destroy_targets();
        s_nsub = 1; s_ready = 1;
        return;
    }
    s_u_src        = glGetUniformLocation(s_sh_blit, "u_src");
    s_u_scale      = glGetUniformLocation(s_sh_blit, "u_scale");
    s_u_resolve    = glGetUniformLocation(s_sh_blit, "u_resolve");
    s_u_contrast   = glGetUniformLocation(s_sh_blit, "u_contrast");
    s_u_saturation = glGetUniformLocation(s_sh_blit, "u_saturation");
    s_u_lift       = glGetUniformLocation(s_sh_blit, "u_lift");
    s_u_warmth     = glGetUniformLocation(s_sh_blit, "u_warmth");
    s_u_grain      = glGetUniformLocation(s_sh_blit, "u_grain");
    s_u_grain_seed = glGetUniformLocation(s_sh_blit, "u_grain_seed");
    s_u_letterbox  = glGetUniformLocation(s_sh_blit, "u_letterbox");

    /* Fullscreen triangle pair in NDC; post_quad.vert derives UV from it. */
    static const float quad[12] = {
        -1.0f, -1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        -1.0f, -1.0f,   1.0f,  1.0f,  -1.0f,  1.0f,
    };
    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    s_nsub  = g_cine.samples < 1 ? 1 : g_cine.samples;
    s_ready = 1;

    /* The dot-overlap dedup exists to stop far-field stars shimmering as they
     * cross each other; supersampling now resolves that properly, and the
     * dedup only costs stars (CINEMATIC.md §8.4). Film-out only — a live
     * session keeps the interactive behaviour. main.c skips settings_save()
     * while filming, so this override never reaches settings.json. */
    if (g_cine.filming && s_nsub > 1) g_settings.dot_hide_px = 0.0f;

    fprintf(stdout, "[Cinematic] renderer active — %dx%d, %d sample%s/frame\n",
            WIN_W, WIN_H, s_nsub, s_nsub == 1 ? "" : "s");
}

void cinematic_shutdown(void)
{
    cinematic_encoder_close();
    if (s_pbo_ok) { glDeleteBuffers(PBO_RING, s_pbo); s_pbo_ok = 0; }
    if (s_vbo)     { glDeleteBuffers(1, &s_vbo);      s_vbo = 0; }
    if (s_vao)     { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_sh_blit) { glDeleteProgram(s_sh_blit);      s_sh_blit = 0; }
    destroy_targets();
    free(s_flip_row); s_flip_row = NULL;
    s_ready = 0;
}

/* ------------------------------------------------------------------ queries */

int cinematic_active(void)  { return g_cine.enabled && s_ready; }
int cinematic_filming(void) { return g_cine.enabled && g_cine.filming; }

void cinematic_set_samples(int n)
{
    if (n < 1)   n = 1;
    if (n > 256) n = 256;
    g_cine.samples = n;
    /* Only take effect if accumulation is actually available; when it is not,
     * s_nsub is pinned at 1 and must stay there. */
    if (s_acc_fbo) s_nsub = n;
}

int cinematic_samples(void)
{
    if (!cinematic_active() || !s_acc_fbo) return 1;
    return s_nsub;
}

double cinematic_frame_dt(void)
{
    int fps = g_cine.fps > 0 ? g_cine.fps : 60;
    return 1.0 / (double)fps;
}

int cinematic_total_frames(void)
{
    int fps = g_cine.fps > 0 ? g_cine.fps : 60;
    double n = g_cine.duration * (double)fps;
    int    f = (int)(n + 0.5);
    return f < 1 ? 1 : f;
}

/* ------------------------------------------------------------------ jitter */

/* Radical-inverse base-b Halton. Low-discrepancy rather than white noise so a
 * 4-sample live frame is already evenly spread instead of clumping. */
static float halton(int index, int base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

void cinematic_jitter(int s, float *ax, float *ay)
{
    *ax = 0.0f;
    *ay = 0.0f;
    int n = cinematic_samples();
    if (n <= 1 || WIN_W < 1 || WIN_H < 1) return;

    /* Halton is 0-based-degenerate at index 0 (returns 0,0), which would waste
     * a sample on the unjittered centre for one of the two axes. Offset by 1. */
    float jx = halton(s + 1, 2) - 0.5f;    /* pixels, [-0.5, 0.5) */
    float jy = halton(s + 1, 3) - 0.5f;

    /* Pixels -> radians. One pixel is 2/W in NDC, and NDC maps to the tangent
     * of the view angle through tan(fov/2) (times aspect horizontally), so a
     * sub-pixel offset is a sub-pixel *angle*. The small-angle approximation is
     * beyond generous at half a pixel. */
    float tan_half = tanf((float)FOV * 0.5f * (float)(M_PI / 180.0));
    float aspect   = (float)WIN_W / (float)WIN_H;
    *ax = (2.0f * jx / (float)WIN_W) * tan_half * aspect;
    *ay = (2.0f * jy / (float)WIN_H) * tan_half;
}

/* ------------------------------------------------------- depth of field ---
 *
 * A literal camera aperture produces exactly zero depth of field here. The
 * circle of confusion on a real sensor scales with the sensor's own size —
 * ~24mm, which is 1.6e-13 AU — so against a scene measured in astronomical
 * units the blur is many orders of magnitude below one pixel. Physical units
 * are simply the wrong model for this scene.
 *
 * So the f-number is kept as a *control surface* a cinematographer recognises,
 * mapped onto a lens whose radius scales with the focus distance:
 *
 *     R = APERTURE_K * focus / N
 *
 * Scale-invariance is the whole point: f/2.8 then gives the same look framing
 * a moon at 0.01 AU as it does framing a galaxy at 1e11 AU, which is exactly
 * what a camera in a scale-continuous renderer should do. APERTURE_K is
 * calibrated so f/2.8 is roughly a 15-pixel background blur at 1080p / 45 FOV.
 *
 * This is a deliberate departure from "physically based" as CINEMATIC.md §8.1
 * originally framed it; the section records the reason.
 */
#define APERTURE_K 0.032

static char s_focus_name[32];
static int  s_focus_warned;

void cinematic_set_focus_target(const char *name)
{
    if (!name || !name[0]) { s_focus_name[0] = 0; s_focus_warned = 0; return; }
    snprintf(s_focus_name, sizeof s_focus_name, "%s", name);
    s_focus_warned = 0;
}

const char *cinematic_focus_target(void) { return s_focus_name; }

/* Camera distance in AU to the focus-locked body, or 0 if there isn't one.
 * Resolved every frame rather than cached as an index: a tracked body can be
 * absorbed mid-shot, and g_nbodies is a high-water mark whose dead slots are
 * reused, so a stale index could silently point at a different body. */
static double focus_target_distance(void)
{
    if (!s_focus_name[0]) return 0.0;
    int i = body_find_named(s_focus_name);
    if (i >= 0) {
        double dx = g_bodies[i].pos[0] * RS - g_cam.pos[0];
        double dy = g_bodies[i].pos[1] * RS - g_cam.pos[1];
        double dz = g_bodies[i].pos[2] * RS - g_cam.pos[2];
        double d  = sqrt(dx*dx + dy*dy + dz*dz);
        return d > 0.0 ? d : 0.0;
    }
    if (!s_focus_warned) {
        fprintf(stderr, "[Cinematic] focus target '%s' not found; "
                        "falling back to auto-focus\n", s_focus_name);
        s_focus_warned = 1;
    }
    return 0.0;
}

double cinematic_focus_distance(void)
{
    if (!cinematic_active()) return 0.0;
    if (g_settings.cine_aperture <= 0.0f) return 0.0;   /* DOF off */

    double locked = focus_target_distance();
    if (locked > 0.0) return locked;

    if (g_settings.cine_focus_auto) {
        /* g_cam_prox is the shared per-frame proximity pass, which already
         * handles the frozen field-star range correctly, so auto-focus cannot
         * latch onto bulk scenery. */
        if (g_cam_prox.body >= 0 && g_cam_prox.body_dist_au > 0.0 &&
            g_cam_prox.body_dist_au < 1e298)
            return g_cam_prox.body_dist_au;
    }
    return g_settings.cine_focus_au > 0.0f
         ? (double)g_settings.cine_focus_au : 0.0;
}

void cinematic_lens_sample(int s, double *du, double *dv)
{
    *du = *dv = 0.0;
    double focus = cinematic_focus_distance();
    int    n     = cinematic_samples();
    if (focus <= 0.0 || n <= 1) return;     /* one sample cannot blur anything */

    double fnum = (double)g_settings.cine_aperture;
    if (fnum <= 0.0) return;
    double R = APERTURE_K * focus / fnum;

    /* Uniform over the disc: sqrt on the radius, or samples pile up in the
     * centre and the bokeh develops a hot core. Bases 5/7 keep this sequence
     * independent of the (2,3) pixel jitter — correlated jitter would tie the
     * bokeh shape to screen position. */
    double r  = R * sqrt((double)halton(s + 1, 5));
    double th = 2.0 * 3.14159265358979323846 * (double)halton(s + 1, 7);
    *du = r * cos(th);
    *dv = r * sin(th);
}

void cinematic_lens_aim(double du, double dv, double focus,
                        const double fwd[3], const double right[3],
                        const double up[3], double out_dir[3])
{
    /* The focal point sits `focus` AU straight ahead of the unoffset camera.
     * The offset eye is at du*right + dv*up, so it must look along
     * (focus*fwd - offset) to keep that point pinned to the same pixel. A
     * point at infinity is unaffected by the eye offset but still swings by
     * the aim angle du/focus — which is the far blur, and is why this is one
     * mechanism rather than two. */
    for (int i = 0; i < 3; i++)
        out_dir[i] = focus * fwd[i] - (du * right[i] + dv * up[i]);

    double len = sqrt(out_dir[0]*out_dir[0] + out_dir[1]*out_dir[1] +
                      out_dir[2]*out_dir[2]);
    if (len > 0.0) {
        out_dir[0] /= len; out_dir[1] /= len; out_dir[2] /= len;
    } else {
        out_dir[0] = fwd[0]; out_dir[1] = fwd[1]; out_dir[2] = fwd[2];
    }
}

/* ------------------------------------------------------------ motion blur */

double cinematic_shutter_fraction(void)
{
    if (!cinematic_active()) return 0.0;
    double f = (double)g_settings.cine_shutter / 360.0;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;       /* past 360 is the long-exposure look, later */
    return f;
}

int cinematic_motion_blur_active(void)
{
    return cinematic_active() && cinematic_samples() > 1 &&
           cinematic_shutter_fraction() > 0.0;
}

float cinematic_quality_scale(void)
{
    if (!cinematic_filming()) return 1.0f;
    float q = g_settings.cine_quality;
    if (q < 1.0f) q = 1.0f;
    if (q > 8.0f) q = 8.0f;
    return q;
}

/* ------------------------------------------------------------------ passes */

/* Letterbox band half-height in UV for the configured target aspect, or 0 when
 * the target is no taller than the frame already is (no bars needed). */
static float letterbox_half(void)
{
    float target = g_settings.cine_letterbox;
    if (target <= 0.0f || WIN_H < 1) return 0.0f;
    float screen = (float)WIN_W / (float)WIN_H;
    if (target <= screen) return 0.0f;      /* frame is already this wide */
    return 0.5f * screen / target;
}

void cinematic_picture_band(float *top, float *bottom)
{
    float h = (float)WIN_H;
    float half = cinematic_active() ? letterbox_half() : 0.0f;
    if (half <= 0.0f) { *top = 0.0f; *bottom = h; return; }
    *top    = h * (0.5f - half);
    *bottom = h * (0.5f + half);
}

static void draw_quad(GLuint src_tex, float scale, int resolve)
{
    glUseProgram(s_sh_blit);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(s_u_src, 0);
    glUniform1f(s_u_scale, scale);
    glUniform1i(s_u_resolve, resolve);
    if (resolve) {
        glUniform1f(s_u_contrast,   g_settings.cine_contrast);
        glUniform1f(s_u_saturation, g_settings.cine_saturation);
        glUniform1f(s_u_lift,       g_settings.cine_lift);
        glUniform1f(s_u_warmth,     g_settings.cine_warmth);
        glUniform1f(s_u_grain,      g_settings.cine_grain);
        /* Seeded off the frame counter, not the wall clock: grain must move
         * between frames but reproduce exactly on a re-render. */
        glUniform1f(s_u_grain_seed, (float)(s_frame_counter % 1024) * 17.13f);
        glUniform1f(s_u_letterbox,  letterbox_half());
    }
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void cinematic_frame_begin(void)
{
    if (!cinematic_active() || !s_acc_fbo) return;

    /* The window can be resized under live cinematic mode; post.c reallocates
     * its own targets the same way on the same trigger. */
    if (s_w != WIN_W || s_h != WIN_H) {
        if (!create_targets(WIN_W, WIN_H)) { s_nsub = 1; return; }
    }
    s_frame_counter++;
    glBindFramebuffer(GL_FRAMEBUFFER, s_acc_fbo);
    glViewport(0, 0, s_w, s_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void cinematic_sub_begin(int sub)
{
    if (!cinematic_active() || !s_acc_fbo) return;
    post_set_target(s_sub_fbo);       /* post_end() composites here, not to 0 */
    /* Adapt exposure on the first sample only; the rest of this output frame
     * reuses it, so brightness cannot drift across the samples being averaged. */
    post_set_autoexposure_hold(sub > 0);
}

void cinematic_sub_end(void)
{
    if (!cinematic_active() || !s_acc_fbo) return;
    post_set_target(0);
    post_set_autoexposure_hold(0);

    glBindFramebuffer(GL_FRAMEBUFFER, s_acc_fbo);
    glViewport(0, 0, s_w, s_h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);      /* sum; the mean is taken at resolve */
    draw_quad(s_sub_tex, 1.0f, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void cinematic_frame_resolve(void)
{
    if (!cinematic_active() || !s_acc_fbo) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WIN_W, WIN_H);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    draw_quad(s_acc_tex, 1.0f / (float)s_nsub, 1);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

/* ------------------------------------------------------------------ encoder */

static int have_ffmpeg(void)
{
    /* One shell spawn at film start; cheaper than reimplementing PATH search. */
    return system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

/* Pick a video codec from the output extension. Unknown extensions fall back to
 * H.264, which every container in practical use here accepts. */
static const char *codec_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot) {
        if (!strcasecmp(dot, ".webm")) return "libvpx-vp9";
        if (!strcasecmp(dot, ".gif"))  return "gif";
    }
    return "libx264";
}

int cinematic_encoder_open(void)
{
    if (!cinematic_filming()) return 1;

    int w = WIN_W, h = WIN_H;
    s_t_start = SDL_GetTicks64();
    s_t_last_report = s_t_start;

    /* Readback ring: frame n's glReadPixels lands in a PBO while frame n+1
     * renders, so the (large, at 4K) pipeline stall is overlapped rather than
     * paid serially per frame. */
    glGenBuffers(PBO_RING, s_pbo);
    for (int i = 0; i < PBO_RING; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 3, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_pbo_ok = 1;

    if (!have_ffmpeg()) {
        fprintf(stderr,
            "[Cinematic] ffmpeg not found on PATH — writing a PPM sequence instead.\n"
            "            Finish the encode with:\n"
            "              ffmpeg -framerate %d -i %s.%%05d.ppm -c:v libx264 "
            "-preset slow -crf %d -pix_fmt yuv420p \"%s\"\n",
            g_cine.fps, g_cine.output, g_cine.crf, g_cine.output);
        s_enc = NULL;
        s_enc_is_pipe = 0;
        return 1;                    /* the fallback path needs no handle */
    }

    /* vflip: GL reads bottom-up. Done in the filter graph rather than on the
     * CPU so the readback stays a straight memcpy out of the PBO. */
    char cmd[2048];
    char audio_in[640]  = "";
    char audio_out[128] = "-an";
    if (g_cine.audio && g_cine.audio_path[0]) {
        FILE *t = fopen(g_cine.audio_path, "rb");
        if (t) {
            fclose(t);
            snprintf(audio_in, sizeof audio_in, "-i \"%s\" ", g_cine.audio_path);
            snprintf(audio_out, sizeof audio_out, "-c:a aac -b:a 192k -shortest");
        } else {
            fprintf(stderr, "[Cinematic] audio '%s' not found; filming silent\n",
                    g_cine.audio_path);
        }
    }

    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y "
             "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - %s"
             "-c:v %s -preset slow -crf %d -pix_fmt yuv420p "
             "-movflags +faststart -vf vflip %s \"%s\"",
             w, h, g_cine.fps, audio_in, codec_for(g_cine.output),
             g_cine.crf, audio_out, g_cine.output);

    s_enc = popen(cmd, "w");
    if (!s_enc) {
        fprintf(stderr, "[Cinematic] could not start ffmpeg: %s\n", strerror(errno));
        return 0;
    }
    s_enc_is_pipe = 1;

    fprintf(stdout, "[Cinematic] filming %d frames -> %s (%dx%d @ %dfps, "
                    "%d samples/frame)\n",
            cinematic_total_frames(), g_cine.output, w, h, g_cine.fps, s_nsub);
    return 1;
}

/* Write one fully-read frame out: down the pipe as raw RGB, or to disk as a
 * PPM (flipped, since there is no filter graph to do it). */
static void emit_frame(const unsigned char *px, int w, int h)
{
    if (s_enc) {
        fwrite(px, 1, (size_t)w * h * 3, s_enc);
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s.%05ld.ppm", g_cine.output, s_frames_written);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    fclose(f);
}

void cinematic_capture_frame(void)
{
    if (!cinematic_filming() || !s_pbo_ok) return;

    int w = WIN_W, h = WIN_H;
    int slot = (int)(s_frames_issued % PBO_RING);

    /* Issue this frame's readback into its slot (asynchronous: glReadPixels
     * into a bound PBO returns without waiting for the transfer). */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[slot]);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, 0);
    s_frames_issued++;

    /* Drain the oldest in-flight readback, which by now has had a full frame
     * to land. */
    if (s_frames_issued > PBO_RING - 1) {
        int old = (int)(s_frames_written % PBO_RING);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[old]);
        const unsigned char *px =
            (const unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (px) {
            emit_frame(px, w, h);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            s_frames_written++;
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void cinematic_encoder_close(void)
{
    if (!s_pbo_ok && !s_enc) return;

    /* Flush whatever is still in flight in the ring. */
    if (s_pbo_ok) {
        int w = WIN_W, h = WIN_H;
        while (s_frames_written < s_frames_issued) {
            int old = (int)(s_frames_written % PBO_RING);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[old]);
            const unsigned char *px =
                (const unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (!px) break;
            emit_frame(px, w, h);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            s_frames_written++;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    int enc_status = 0;
    if (s_enc) {
        enc_status = s_enc_is_pipe ? pclose(s_enc) : fclose(s_enc);
        s_enc = NULL;
    }

    if (s_frames_written > 0) {
        double secs = (double)(SDL_GetTicks64() - s_t_start) / 1000.0;
        /* Frames handed to the encoder are not frames encoded: ffmpeg can
         * refuse the stream outright (an odd frame size, an unwritable path)
         * and leave a zero-byte file behind while this side happily reports
         * success. Check what the pipe exited with, and check that something
         * actually landed on disk. */
        if (enc_status != 0) {
            fprintf(stderr, "[Cinematic] ENCODER FAILED (exit %d) — '%s' is not "
                            "a usable video. ffmpeg's error is above.\n",
                    enc_status, g_cine.output);
        } else {
            FILE *chk = fopen(g_cine.output, "rb");
            long sz = 0;
            if (chk) { fseek(chk, 0, SEEK_END); sz = ftell(chk); fclose(chk); }
            if (sz <= 0)
                fprintf(stderr, "[Cinematic] ENCODER WROTE NOTHING to '%s'.\n",
                        g_cine.output);
        }
        fprintf(stdout, "[Cinematic] wrote %ld frames in %.1fs (%.2f fps) -> %s\n",
                s_frames_written, secs,
                secs > 0.0 ? (double)s_frames_written / secs : 0.0,
                g_cine.output);
        cinema_blur_report();
        s_frames_written = 0;
        s_frames_issued  = 0;
    }
}

void cinematic_report_progress(void)
{
    if (!cinematic_filming()) return;
    Uint64 now = SDL_GetTicks64();
    long   done = s_frames_issued;
    int    total = cinematic_total_frames();
    Uint64 period = isatty(1) ? 1000 : 10000;
    if (now - s_t_last_report < period && done < total) return;
    s_t_last_report = now;

    double secs = (double)(now - s_t_start) / 1000.0;
    double rate = secs > 0.0 ? (double)done / secs : 0.0;
    double eta  = rate > 0.0 ? (double)(total - done) / rate : 0.0;

    /* Rewrite one line on a terminal, but print discrete lines when redirected
     * — a carriage return into a log file just produces one unreadable
     * mega-line, and film-out runs are exactly the thing people tee to a log. */
    int tty = isatty(1);
    fprintf(stdout, "%s[Cinematic] %ld/%d (%.1f%%)  %.2f fps  ETA %02d:%02d%s",
            tty ? "\r" : "", done, total,
            100.0 * (double)done / (double)total, rate,
            (int)(eta / 60.0), ((int)eta) % 60, tty ? "   " : "\n");
    fflush(stdout);
}
