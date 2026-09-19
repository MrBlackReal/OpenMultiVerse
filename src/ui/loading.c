/*
 * loading.c — full-screen loading / universe-switch overlay
 *
 * Reuses the UI shader (assets/shaders/ui.vert + ui.frag) but owns its own VAO,
 * VBO, font and text caches so it has no ordering dependency on ui.c's per-frame
 * state. The overlay is drawn to the default framebuffer and presented by the
 * loader itself (see loading.h for the why).
 *
 * Animation model (all time-based so it stays smooth regardless of how often the
 * loader happens to tick):
 *   - s_fade       eases 0→1 on begin, 1→0 on end (FADE_*_DUR seconds).
 *   - s_disp       eases toward s_target (the requested progress) so a value
 *                  that jumps after a long blocking phase glides into place.
 *   - s_sweep      advances continuously and drives the indeterminate highlight.
 *
 * Presentation throttle:
 *   loading_tick() only redraws when PRESENT_DT has elapsed since the last
 *   present, so per-body calls in the universe loader are nearly free. V-Sync is
 *   forced off across a session (saved/restored around begin/end) so each
 *   present is cheap rather than a 16 ms block.
 */
#include "loading.h"
#include "gl_utils.h"
#include "ui_theme.h"
#include "settings.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

/* ── tunables ─────────────────────────────────────────────────────────────────
 * These now live in g_settings (global, persisted) — aliased here so the rest
 * of the file reads unchanged. Defaults match the historical compile-time
 * values; see settings.c. */
#define FADE_IN_DUR   (g_settings.fade_in_dur)
#define FADE_OUT_DUR  (g_settings.fade_out_dur)
#define PROG_EASE     (g_settings.prog_ease)
#define SWEEP_SPEED   (g_settings.sweep_speed)
#define PRESENT_DT    (g_settings.present_dt)
#define STATUS_FONT   (g_settings.status_font_px)
#define PCT_FONT      (g_settings.pct_font_px)

/* Accent (default #0064DE, matches the rest of the UI). */
#define ACCENT_R (g_settings.accent_r)
#define ACCENT_G (g_settings.accent_g)
#define ACCENT_B (g_settings.accent_b)

/* ── GL / font state ──────────────────────────────────────────────────────── */
static SDL_Window *s_win        = NULL;
static GLuint      s_shader     = 0;
static GLuint      s_vao        = 0;
static GLuint      s_vbo        = 0;
static GLint       s_loc_screen = -1;
static GLint       s_loc_color  = -1;
static GLint       s_loc_use_tex= -1;
static GLint       s_loc_tex    = -1;
static TTF_Font   *s_font       = NULL;   /* status text */
static TTF_Font   *s_pct_font   = NULL;   /* percentage  */

/* ── session state ────────────────────────────────────────────────────────── */
static int    s_active      = 0;
static int    s_indet       = 0;      /* 1 = indeterminate */
static double s_target      = 0.0;    /* requested progress */
static double s_disp        = 0.0;    /* eased, displayed progress */
static double s_fade        = 0.0;
static double s_fade_target = 0.0;
static double s_sweep       = 0.0;
static Uint64 s_last_anim   = 0;      /* perf counter at last animation step */
static Uint64 s_last_present= 0;
static int    s_saved_vsync = 1;

/* ── text cache ───────────────────────────────────────────────────────────── */
typedef struct { GLuint tex; int w, h; char str[96]; } TextCache;
static TextCache s_tc_status = {0};
static TextCache s_tc_pct    = {0};

/* ── helpers ──────────────────────────────────────────────────────────────── */
static GLuint surf_to_tex(SDL_Surface *surf, int *w, int *h) {
    SDL_Surface *c = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!c) return 0;
    *w = c->w; *h = c->h;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(c);
    return tex;
}

static void cache_text(TextCache *tc, TTF_Font *font, const char *str) {
    if (!font) return;
    if (strcmp(tc->str, str) == 0) return;     /* unchanged → keep texture */
    strncpy(tc->str, str, sizeof(tc->str) - 1);
    tc->str[sizeof(tc->str) - 1] = '\0';
    if (tc->tex) { glDeleteTextures(1, &tc->tex); tc->tex = 0; }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderText_Blended(font, str[0] ? str : " ", white);
    if (surf) tc->tex = surf_to_tex(surf, &tc->w, &tc->h);
}

static void draw_quad(float x, float y, float w, float h) {
    float v[24] = {
        x,   y,   0,0,  x+w, y,   1,0,  x+w, y+h, 1,1,
        x,   y,   0,0,  x+w, y+h, 1,1,  x,   y+h, 0,1,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void draw_rect(float x, float y, float w, float h,
                      float r, float g, float b, float a) {
    glUniform1i(s_loc_use_tex, 0);
    glUniform4f(s_loc_color, r, g, b, a);
    draw_quad(x, y, w, h);
}

/* Draw a cached text texture centred horizontally at cx, top at y, height h. */
static void draw_text_centered(TextCache *tc, float cx, float y, float h, float alpha) {
    if (!tc || !tc->tex) return;
    float w = h * (float)tc->w / (float)tc->h;
    glUniform1i(s_loc_use_tex, 1);
    glUniform4f(s_loc_color, 1, 1, 1, alpha);
    glBindTexture(GL_TEXTURE_2D, tc->tex);
    draw_quad(cx - w * 0.5f, y, w, h);
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Advance time-based animation since the last call. */
static void step_anim(void) {
    Uint64 now  = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    double dt = s_last_anim ? (double)(now - s_last_anim) / (double)freq : 0.0;
    s_last_anim = now;
    if (dt > 0.1) dt = 0.1;          /* don't snap after a long blocking phase */

    /* Fade. */
    if (s_fade < s_fade_target)
        s_fade = fmin(s_fade_target, s_fade + dt / FADE_IN_DUR);
    else if (s_fade > s_fade_target)
        s_fade = fmax(s_fade_target, s_fade - dt / FADE_OUT_DUR);

    /* Progress easing (exponential approach). */
    double k = 1.0 - exp(-dt * PROG_EASE);
    s_disp += (s_target - s_disp) * k;
    if (s_disp < 0.0) s_disp = 0.0;
    if (s_disp > 1.0) s_disp = 1.0;

    /* Indeterminate sweep. */
    s_sweep += dt * SWEEP_SPEED;
    if (s_sweep > 1.0) s_sweep -= floor(s_sweep);
}

static void draw_overlay(void) {
    float W = (float)WIN_W, H = (float)WIN_H;
    float a = (float)s_fade;

    /* Backdrop — matches the app clear colour, faded so a universe switch
     * dissolves cleanly over the previous scene. */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_shader);
    glUniform2f(s_loc_screen, W, H);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    draw_rect(0, 0, W, H, 0.0f, 0.0f, 0.02f, a);

    /* Bar geometry. */
    float bw = clampf(W * 0.42f, 320.0f, 760.0f);
    float bh = 6.0f;
    float bx = (W - bw) * 0.5f;
    float by = floorf(H * 0.56f);
    float cx = W * 0.5f;

    /* Track. */
    draw_rect(bx, by, bw, bh, 1.0f, 1.0f, 1.0f, 0.10f * a);

    if (s_indet) {
        /* Sweeping highlight with eased velocity at the ends. */
        float p   = (float)s_sweep;
        p = p * p * (3.0f - 2.0f * p);          /* smoothstep */
        float seg = bw * 0.30f;
        float x   = bx + (bw + seg) * p - seg;
        float l   = fmaxf(bx, x);
        float r   = fminf(bx + bw, x + seg);
        if (r > l)
            draw_rect(l, by, r - l, bh, ACCENT_R, ACCENT_G, ACCENT_B, 0.95f * a);
    } else {
        float fw = bw * (float)s_disp;
        if (fw > 0.0f)
            draw_rect(bx, by, fw, bh, ACCENT_R, ACCENT_G, ACCENT_B, 0.95f * a);
    }

    /* Status above the bar. */
    if (s_tc_status.tex)
        draw_text_centered(&s_tc_status, cx, by - (float)STATUS_FONT - 16.0f,
                           (float)STATUS_FONT, 0.92f * a);

    /* Percentage below the bar (determinate only). */
    if (!s_indet) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)(s_disp * 100.0 + 0.5));
        cache_text(&s_tc_pct, s_pct_font, pct);
        if (s_tc_pct.tex)
            draw_text_centered(&s_tc_pct, cx, by + bh + 12.0f,
                               (float)PCT_FONT, 0.55f * a);
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

static void present(void) {
    /* Keep the window alive and correctly sized: no SDL events are pumped while
     * the (blocking) loader runs, so do the minimum here. Pumping marks us
     * responsive to the OS; syncing the drawable size keeps the overlay centred
     * and leaves g_win_w/h correct for when the main loop resumes. Queued events
     * are left for the main loop to handle once loading finishes. */
    int dw = WIN_W, dh = WIN_H;
    SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    if (dw > 0 && dh > 0) { g_win_w = dw; g_win_h = dh; }
    SDL_PumpEvents();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WIN_W, WIN_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_overlay();
    SDL_GL_SwapWindow(s_win);
    s_last_present = SDL_GetPerformanceCounter();
}

/* ── public API ───────────────────────────────────────────────────────────── */
void loading_init(SDL_Window *win) {
    s_win = win;
    s_shader = gl_shader_load("assets/shaders/ui.vert", "assets/shaders/ui.frag");
    if (!s_shader) { fprintf(stderr, "[Loading] shader failed\n"); return; }
    s_loc_screen  = glGetUniformLocation(s_shader, "u_screen");
    s_loc_color   = glGetUniformLocation(s_shader, "u_color");
    s_loc_use_tex = glGetUniformLocation(s_shader, "u_use_tex");
    s_loc_tex     = glGetUniformLocation(s_shader, "u_tex");

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(24 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    glUseProgram(s_shader);
    glUniform1i(s_loc_tex, 0);
    glUseProgram(0);

    /* TTF is already initialised by ui_init(); just open our fonts. */
    loading_reload_fonts();
}

void loading_reload_fonts(void) {
    if (s_pct_font && s_pct_font != s_font) TTF_CloseFont(s_pct_font);
    if (s_font) TTF_CloseFont(s_font);
    
    s_font = s_pct_font = NULL;

    s_font     = ui_theme_open_font(STATUS_FONT);
    s_pct_font = ui_theme_open_font(PCT_FONT);

    if (!s_pct_font) s_pct_font = s_font;
    if (!s_font) fprintf(stderr, "[Loading] no font\n");

    /* Force cached glyph textures to re-render at the new size. */
    s_tc_status.str[0] = '\0';
    s_tc_pct.str[0]    = '\0';
}

void loading_begin(void) {
    if (!s_shader) return;
    s_active       = 1;
    s_indet        = 1;
    s_target       = 0.0;
    s_disp         = 0.0;
    s_fade         = 0.0;
    s_fade_target  = 1.0;
    s_sweep        = 0.0;
    s_last_anim    = 0;
    s_last_present = 0;
    s_tc_status.str[0] = '\0';
    s_saved_vsync = SDL_GL_GetSwapInterval();
    SDL_GL_SetSwapInterval(0);     /* cheap presents while loading */
    step_anim();
    present();
}

void loading_status(const char *fmt, ...) {
    if (!s_active) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cache_text(&s_tc_status, s_font, buf);
}

void loading_progress(double frac) {
    if (!s_active) return;
    int was_indet = s_indet;
    s_indet = 0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    s_target = frac;
    /* Leaving sweep mode: snap so the bar doesn't ease backward from a stale
     * (often full) determinate value into a new phase that restarts near zero. */
    if (was_indet) s_disp = frac;
}

void loading_indeterminate(void) {
    if (!s_active) return;
    s_indet = 1;
}

void loading_tick(void) {
    if (!s_active || !s_shader) return;
    Uint64 now  = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    if (s_last_present &&
        (double)(now - s_last_present) / (double)freq < PRESENT_DT)
        return;                      /* throttle: cheap to call in a tight loop */
    step_anim();
    present();
}

void loading_end(void) {
    if (!s_active || !s_shader) { s_active = 0; return; }
    /* Settle the bar to full, then fade the overlay out. */
    s_target = s_indet ? s_target : 1.0;
    s_fade_target = 0.0;
    while (s_fade > 0.01) {
        step_anim();
        present();
        SDL_Delay(8);
    }
    s_active = 0;
    SDL_GL_SetSwapInterval(s_saved_vsync);
}

int loading_active(void) { return s_active; }

void loading_shutdown(void) {
    if (s_tc_status.tex) glDeleteTextures(1, &s_tc_status.tex);
    if (s_tc_pct.tex)    glDeleteTextures(1, &s_tc_pct.tex);
    if (s_vbo)    glDeleteBuffers(1, &s_vbo);
    if (s_vao)    glDeleteVertexArrays(1, &s_vao);
    if (s_shader) glDeleteProgram(s_shader);
    if (s_pct_font && s_pct_font != s_font) TTF_CloseFont(s_pct_font);
    if (s_font)   TTF_CloseFont(s_font);
    s_shader = s_vao = s_vbo = 0;
    s_font = s_pct_font = NULL;
    memset(&s_tc_status, 0, sizeof(s_tc_status));
    memset(&s_tc_pct, 0, sizeof(s_tc_pct));
}
