/*
 * ui.c — 2D HUD overlay
 *
 * Layout (top bar, full width, BAR_H pixels tall):
 *   - Background: semi-transparent dark strip
 *   - Blue fill:  log-normalised camera movement speed (left → right)
 *   - Centre text: physical movement speed  ("0.50 AU/s")
 *   - Right text:  simulation speed         ("365 days/s")
 */
#include "ui.h"
#include "camera.h"
#include "physics.h"
#include "build.h"
#include "body.h"
#include "gl_utils.h"
#include "ui_theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ layout */
#define BAR_H         4       /* bar height, pixels                 */
#define BAR_W_FRAC    0.5f    /* fraction of screen width           */
#define BAR_TOP       12.0f   /* distance from top of screen        */
#define TEXT_GAP      6.0f    /* gap between bar bottom and text    */
#define FONT_SIZE     16
#define BUILD_ITEM_FONT_SIZE 18
#define MENU_TITLE_SIZE 24
#define MENU_TEXT_SIZE  18
#define MENU_HINT_SIZE  16
#define PAUSE_MENU_PANEL_W 360.0f
#define PAUSE_MENU_PANEL_H 312.0f
#define PAUSE_MENU_ITEM_W (PAUSE_MENU_PANEL_W - 36.0f)
#define PAUSE_MENU_ITEM_H 42.0f
#define PAUSE_MENU_ITEM_ACTIVE_H 42.0f
#define PAUSE_MENU_ITEM_GAP 10.0f

/* Camera speed range (AU / real-second) */
#define CAM_MIN       0.00001f
#define CAM_MAX       200.0f       /* normal max = warp min */
#define WARP_MAX  63241.0f         /* 1 ly/s */

/* ------------------------------------------------------------------ GL */
static GLuint s_shader     = 0;
static GLuint s_vao        = 0;
static GLuint s_vbo        = 0;    /* 6 vertices × 4 floats (x,y,u,v)  */
static GLint  s_loc_screen  = -1;
static GLint  s_loc_color   = -1;
static GLint  s_loc_use_tex = -1;
static GLint  s_loc_tex     = -1;

static TTF_Font *s_font = NULL;
static TTF_Font *s_build_item_font = NULL;
static TTF_Font *s_menu_font = NULL;
static TTF_Font *s_menu_title_font = NULL;

/* ------------------------------------------------------------------ text cache */
typedef struct {
    GLuint tex;
    int w, h;
    char str[64];
    SDL_Color col;
} TextCache;
static TextCache s_tc_move = {0};
static TextCache s_tc_sim  = {0};
static TextCache s_tc_fps  = {0};
static TextCache s_tc_nearest = {0};
static TextCache s_tc_build_title = {0};
static TextCache s_tc_build_hint  = {0};
static TextCache s_tc_build_items[8];
static TextCache s_tc_pause_title = {0};
static TextCache s_tc_pause_hint = {0};
static TextCache s_tc_pause_items[4];

static int s_pause_menu_visible = 0;
static int s_pause_menu_selected = 0;
static int s_pause_menu_vsync = 0;

typedef struct {
    int n;
    float item_w;
    float item_h;
    float item_active_h;
    float gap;
    float first_item_y;
    float item_x;
    float panel_x;
    float panel_y;
    float panel_w;
    float panel_h;
} PauseMenuLayout;

typedef struct {
    int n;
    float item_x;         /* x of item body (fixed) */
    float item_w;
    float item_h;
    float strip_w;        /* inactive strip width (strip right-edge = item_x) */
    float strip_active_w; /* active strip width (expands left) */
    float gap;
    float items_y;        /* y of first item */
    float title_y;        /* y of "BUILD" title text */
    float hint_y;         /* y of hint text below panel */
    float panel_x;
    float panel_y;
    float panel_w;
    float panel_h;
} BuildBarLayout;

/* ------------------------------------------------------------------ build slide-in animation */
static float  s_build_anim_t    = 0.0f;
static Uint64 s_build_anim_ts   = 0;
static int    s_build_prev_mode = 0;

/* ------------------------------------------------------------------ FPS smoothing */
/* Exponential moving average over ~30 frames, updated every UI frame.
 * We manage time internally so the ui_render() signature stays unchanged. */
static Uint64 s_fps_prev  = 0;
static float  s_fps_smooth = 0.0f;

/* ------------------------------------------------------------------ helpers */

static PauseMenuLayout pause_menu_layout(float W, float H)
{
    PauseMenuLayout layout;
    layout.n = 4;
    layout.item_w = PAUSE_MENU_ITEM_W;
    layout.item_h = PAUSE_MENU_ITEM_H;
    layout.item_active_h = PAUSE_MENU_ITEM_ACTIVE_H;
    layout.gap = PAUSE_MENU_ITEM_GAP;
    layout.panel_x = (W - PAUSE_MENU_PANEL_W) * 0.5f;
    layout.panel_y = (H - PAUSE_MENU_PANEL_H) * 0.5f;
    layout.panel_w = PAUSE_MENU_PANEL_W;
    layout.panel_h = PAUSE_MENU_PANEL_H;
    layout.item_x = layout.panel_x + 18.0f;
    layout.first_item_y = layout.panel_y + 72.0f;
    return layout;
}

static BuildBarLayout build_bar_layout(float W)
{
    (void)W;
    BuildBarLayout layout;
    layout.n = build_preset_count();
    if (layout.n > 8) layout.n = 8;

    const float PANEL_PAD    = 12.0f;
    const float LEFT_MARGIN  = PANEL_PAD;
    const float TITLE_AREA_H = 72.0f;
    const float BOTTOM_AREA_H = 42.0f;
    layout.strip_w        = 8.0f;
    layout.strip_active_w = 14.0f;
    layout.item_h         = PAUSE_MENU_ITEM_H;
    layout.item_w         = 112.0f;
    layout.gap            = PAUSE_MENU_ITEM_GAP;
    layout.panel_x        = LEFT_MARGIN;
    layout.item_x         = layout.panel_x + PANEL_PAD + layout.strip_w;
    layout.panel_w        = PANEL_PAD + layout.strip_w + layout.item_w + PANEL_PAD;

    float total_h    = layout.n * layout.item_h + (layout.n - 1) * layout.gap;
    layout.panel_h   = TITLE_AREA_H + total_h + BOTTOM_AREA_H;
    layout.panel_y   = ((float)WIN_H - layout.panel_h) * 0.5f;
    layout.items_y   = layout.panel_y + TITLE_AREA_H;
    layout.title_y   = layout.panel_y + 24.0f;
    layout.hint_y    = layout.panel_y + layout.panel_h - 28.0f;

    return layout;
}

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

/* Re-render text texture only when the string changes */
static void update_text_with_font(TextCache *tc, TTF_Font *font,
                                  const char *str, SDL_Color col) {
    if (!font) return;
    if (strcmp(tc->str, str) == 0 &&
        tc->col.r == col.r && tc->col.g == col.g &&
        tc->col.b == col.b && tc->col.a == col.a) return;
    strncpy(tc->str, str, 63);
    tc->str[63] = '\0';
    tc->col = col;
    if (tc->tex) { glDeleteTextures(1, &tc->tex); tc->tex = 0; }
    SDL_Surface *surf = TTF_RenderText_Blended(font, str, col);
    if (surf) tc->tex = surf_to_tex(surf, &tc->w, &tc->h);
}

static void update_text(TextCache *tc, const char *str, SDL_Color col) {
    update_text_with_font(tc, s_font, str, col);
}

static int pause_menu_item_at(float mx, float my)
{
    PauseMenuLayout layout = pause_menu_layout((float)WIN_W, (float)WIN_H);

    if (mx < layout.item_x || mx > layout.item_x + layout.item_w) return -1;

    for (int i = 0; i < layout.n; i++) {
        int active = (i == s_pause_menu_selected);
        float item_h = active ? layout.item_active_h : layout.item_h;
        float item_y = layout.first_item_y
                     + (float)i * (layout.item_h + layout.gap)
                     + (layout.item_h - item_h);
        if (my >= item_y && my <= item_y + item_h)
            return i;
    }
    return -1;
}

static void format_distance(double au, char *buf, size_t n)
{
    if (au < 0.001)
        snprintf(buf, n, "%.0f km", au * AU / 1000.0);
    else if (au < 1.0)
        snprintf(buf, n, "%.4f AU", au);
    else if (au < 1000.0)
        snprintf(buf, n, "%.2f AU", au);
    else
        snprintf(buf, n, "%.3f ly", au / 63241.0);
}

static void nearest_body_distance_string(char *buf, size_t n)
{
    int best = -1;
    double best_d = 1e300;

    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        double dx = g_bodies[i].pos[0] * RS - g_cam.pos[0];
        double dy = g_bodies[i].pos[1] * RS - g_cam.pos[1];
        double dz = g_bodies[i].pos[2] * RS - g_cam.pos[2];
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    if (best >= 0 && best_d > 1000.0) {
        best = -1;
        best_d = 1e300;
        for (int i = 0; i < g_nbodies; i++) {
            if (!g_bodies[i].alive) continue;
            if (!g_bodies[i].is_star) continue;
            double dx = g_bodies[i].pos[0] * RS - g_cam.pos[0];
            double dy = g_bodies[i].pos[1] * RS - g_cam.pos[1];
            double dz = g_bodies[i].pos[2] * RS - g_cam.pos[2];
            double d = sqrt(dx*dx + dy*dy + dz*dz);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
    }

    if (best < 0) {
        snprintf(buf, n, "nearest --");
    } else {
        char dist[32];
        format_distance(best_d, dist, sizeof(dist));
        snprintf(buf, n, "%.31s  %.28s", g_bodies[best].name, dist);
    }
}

/* Upload 2 triangles and draw */
static void draw_quad(float x, float y, float w, float h) {
    float v[24] = {
        x,   y,   0,0,
        x+w, y,   1,0,
        x+w, y+h, 1,1,
        x,   y,   0,0,
        x+w, y+h, 1,1,
        x,   y+h, 0,1,
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

static void draw_tex(TextCache *tc, float x, float y, float h) {
    if (!tc || !tc->tex) return;
    float w = h * (float)tc->w / (float)tc->h;
    glUniform1i(s_loc_use_tex, 1);
    glUniform4f(s_loc_color, 1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, tc->tex);
    draw_quad(x, y, w, h);
}

static void draw_build_bar(float W)
{
    if (!g_build_mode) {
        s_build_anim_t    = 0.0f;
        s_build_prev_mode = 0;
        return;
    }

    /* Slide-in animation ------------------------------------------------ */
    if (!s_build_prev_mode) {
        s_build_anim_t  = 0.0f;
        s_build_anim_ts = SDL_GetPerformanceCounter();
    }
    s_build_prev_mode = 1;

    if (s_build_anim_t < 1.0f) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - s_build_anim_ts) / (float)SDL_GetPerformanceFrequency();
        s_build_anim_ts = now;
        s_build_anim_t += dt / 0.22f;
        if (s_build_anim_t > 1.0f) s_build_anim_t = 1.0f;
    }

    /* Ease-out cubic: starts fast, settles smoothly */
    float inv  = 1.0f - s_build_anim_t;
    float ease = 1.0f - inv * inv * inv;

    BuildBarLayout layout = build_bar_layout(W);

    /* Shift everything left by (1-ease) × full panel width+margin */
    float ox = (ease - 1.0f) * (layout.panel_x + layout.panel_w);

    SDL_Color title_col = {255, 255, 255, 235};
    SDL_Color hint_col  = {255, 255, 255, 170};
    SDL_Color item_col  = {0, 0, 0, 255};

    update_text_with_font(&s_tc_build_title, s_menu_title_font, "BUILD", title_col);
    update_text_with_font(&s_tc_build_hint, s_font,
                          g_build_tab_held ? "scroll to select" : "hold TAB and scroll",
                          hint_col);

    draw_rect(layout.panel_x + ox, layout.panel_y, layout.panel_w, layout.panel_h,
              UI_ACCENT_R, UI_ACCENT_G, UI_ACCENT_B, 1.0f);
    draw_rect(layout.panel_x + ox, layout.panel_y, layout.panel_w, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f);

    if (s_tc_build_title.tex) {
        float tw = (float)MENU_TITLE_SIZE * (float)s_tc_build_title.w / (float)s_tc_build_title.h;
        draw_tex(&s_tc_build_title,
                 layout.panel_x + ox + (layout.panel_w - tw) * 0.5f,
                 layout.title_y,
                 (float)MENU_TITLE_SIZE);
    }

    int selected = build_selected_index();

    for (int i = 0; i < layout.n; i++) {
        const BuildPreset *p = build_preset_at(i);
        if (!p) continue;
        int active = (i == selected);
        float item_y  = layout.items_y + (layout.item_h + layout.gap) * (float)i;
        float sw      = active ? layout.strip_active_w : layout.strip_w;
        float strip_x = layout.item_x - layout.strip_w + ox;

        draw_rect(layout.item_x + ox, item_y, layout.item_w, layout.item_h,
                  1.0f, 1.0f, 1.0f, active ? 1.0f : 0.72f);
        draw_rect(strip_x, item_y, sw, layout.item_h,
                  p->col[0], p->col[1], p->col[2], 1.0f);

        update_text_with_font(&s_tc_build_items[i], s_build_item_font ? s_build_item_font : s_font,
                              p->name, item_col);
        if (s_tc_build_items[i].tex) {
            float tw     = (float)BUILD_ITEM_FONT_SIZE * (float)s_tc_build_items[i].w / (float)s_tc_build_items[i].h;
            float text_y = item_y + (layout.item_h - (float)BUILD_ITEM_FONT_SIZE) * 0.5f;
            draw_tex(&s_tc_build_items[i],
                     layout.item_x + ox + (layout.item_w - tw) * 0.5f,
                     text_y,
                     (float)BUILD_ITEM_FONT_SIZE);
        }
    }

    if (s_tc_build_hint.tex) {
        float tw = (float)FONT_SIZE * (float)s_tc_build_hint.w / (float)s_tc_build_hint.h;
        draw_tex(&s_tc_build_hint,
                 layout.panel_x + ox + (layout.panel_w - tw) * 0.5f,
                 layout.hint_y,
                 (float)FONT_SIZE);
    }
}

static void draw_pause_menu(float W, float H)
{
    if (!s_pause_menu_visible) return;

    PauseMenuLayout layout = pause_menu_layout(W, H);
    const char *labels[4] = {
        "Continue",
        "Reset Universe",
        s_pause_menu_vsync ? "Deactivate V-Sync" : "Activate V-Sync",
        "Leave"
    };

    SDL_Color title_col = {255, 255, 255, 235};
    SDL_Color item_col = {0, 0, 0, 255};
    SDL_Color hint_col = {255, 255, 255, 170};

    update_text_with_font(&s_tc_pause_title, s_menu_title_font, "MENU", title_col);
    update_text_with_font(&s_tc_pause_hint, s_font, "ENTER select  |  ESC continue", hint_col);
    for (int i = 0; i < 4; i++)
        update_text_with_font(&s_tc_pause_items[i], s_menu_font, labels[i], item_col);

    draw_rect(0.0f, 0.0f, W, H, 0.0f, 0.0f, 0.0f, 0.38f);
    draw_rect(layout.panel_x, layout.panel_y, layout.panel_w, layout.panel_h,
              UI_ACCENT_R, UI_ACCENT_G, UI_ACCENT_B, 1.0f);
    draw_rect(layout.panel_x, layout.panel_y, layout.panel_w, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f);

    if (s_tc_pause_title.tex) {
        float tw = (float)MENU_TITLE_SIZE * (float)s_tc_pause_title.w / (float)s_tc_pause_title.h;
        draw_tex(&s_tc_pause_title,
                 layout.panel_x + (layout.panel_w - tw) * 0.5f,
                 layout.panel_y + 24.0f,
                 (float)MENU_TITLE_SIZE);
    }

    for (int i = 0; i < layout.n; i++) {
        int active = (i == s_pause_menu_selected);
        float item_h = active ? layout.item_active_h : layout.item_h;
        float item_y = layout.first_item_y + i * (layout.item_h + layout.gap) + (layout.item_h - item_h);

        draw_rect(layout.item_x, item_y, layout.item_w, item_h, 1.0f, 1.0f, 1.0f,
                  active ? 1.0f : 0.72f);

        if (s_tc_pause_items[i].tex) {
            float tw = (float)MENU_TEXT_SIZE * (float)s_tc_pause_items[i].w / (float)s_tc_pause_items[i].h;
            float text_y = item_y + (item_h - (float)MENU_TEXT_SIZE) * 0.5f;
            draw_tex(&s_tc_pause_items[i],
                     layout.item_x + (layout.item_w - tw) * 0.5f,
                     text_y,
                     (float)MENU_TEXT_SIZE);
        }
    }

    if (s_tc_pause_hint.tex) {
        float tw = (float)MENU_HINT_SIZE * (float)s_tc_pause_hint.w / (float)s_tc_pause_hint.h;
        draw_tex(&s_tc_pause_hint,
                 layout.panel_x + (layout.panel_w - tw) * 0.5f,
                 layout.panel_y + layout.panel_h - 28.0f,
                 (float)MENU_HINT_SIZE);
    }
}

/* ------------------------------------------------------------------ public */
void ui_init(void) {
    s_shader = gl_shader_load("assets/shaders/ui.vert",
                              "assets/shaders/ui.frag");
    if (!s_shader) { fprintf(stderr, "[UI] shader failed\n"); return; }

    s_loc_screen  = glGetUniformLocation(s_shader, "u_screen");
    s_loc_color   = glGetUniformLocation(s_shader, "u_color");
    s_loc_use_tex = glGetUniformLocation(s_shader, "u_use_tex");
    s_loc_tex     = glGetUniformLocation(s_shader, "u_tex");

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(24 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    /* u_screen and u_tex are frame-constant */
    glUseProgram(s_shader);
    glUniform2f(s_loc_screen, (float)WIN_W, (float)WIN_H);
    glUniform1i(s_loc_tex, 0);
    glUseProgram(0);

    TTF_Init();
    s_font = ui_theme_open_font(FONT_SIZE);
    s_build_item_font = ui_theme_open_font(BUILD_ITEM_FONT_SIZE);
    s_menu_font = ui_theme_open_font(MENU_TEXT_SIZE);
    s_menu_title_font = ui_theme_open_font(MENU_TITLE_SIZE);
    if (!s_font) fprintf(stderr, "[UI] no font found\n");
    if (!s_build_item_font) s_build_item_font = s_font;
    if (!s_menu_font) s_menu_font = s_font;
    if (!s_menu_title_font) s_menu_title_font = s_menu_font;
    if (s_menu_title_font && s_menu_title_font != s_menu_font && s_menu_title_font != s_font)
        TTF_SetFontStyle(s_menu_title_font, TTF_STYLE_BOLD);
}

void ui_set_pause_menu(int visible, int selected, int vsync_enabled)
{
    s_pause_menu_visible = visible ? 1 : 0;
    s_pause_menu_selected = selected;
    s_pause_menu_vsync = vsync_enabled ? 1 : 0;
}

int ui_pause_menu_hit_test(int mouse_x, int mouse_y)
{
    return pause_menu_item_at((float)mouse_x, (float)mouse_y);
}

void ui_render(void) {
    if (!s_shader || !s_font) return;

    const float W  = (float)WIN_W;
    const float TH = (float)FONT_SIZE;

    /* Log-normalised camera speed → fill fraction.
     * In warp mode the bar uses the warp range [CAM_MAX, WARP_MAX]. */
    float spd = g_cam.speed;
    float t;
    if (g_warp) {
        float ws = spd;
        if (ws < CAM_MAX)  ws = CAM_MAX;
        if (ws > WARP_MAX) ws = WARP_MAX;
        t = logf(ws / CAM_MAX) / logf(WARP_MAX / CAM_MAX);
    } else {
        if (spd < CAM_MIN) spd = CAM_MIN;
        if (spd > CAM_MAX) spd = CAM_MAX;
        t = logf(spd / CAM_MIN) / logf(CAM_MAX / CAM_MIN);
    }

    /* Format movement speed string — show WARP indicator when T is held */
    char mv_str[64];
    if (g_warp) {
        double ly_s = (double)g_cam.speed / (double)WARP_MAX;
        snprintf(mv_str, sizeof(mv_str), "WARP  %.4f ly/s", ly_s);
    } else if (spd < 0.001f) {
        snprintf(mv_str, sizeof(mv_str), "%.5f AU/s", (double)spd);
    } else if (spd < 1.0f) {
        snprintf(mv_str, sizeof(mv_str), "%.3f AU/s", (double)spd);
    } else {
        snprintf(mv_str, sizeof(mv_str), "%.2f AU/s", (double)spd);
    }

    /* Format sim speed string */
    char ss_str[64];
    if (g_paused)
        snprintf(ss_str, sizeof(ss_str), "Paused");
    else {
        double days = g_sim_speed / DAY;
        if (days < 1.0)
            snprintf(ss_str, sizeof(ss_str), "%.2g days/s", days);
        else if (days < 10.0)
            snprintf(ss_str, sizeof(ss_str), "%.1f days/s", days);
        else
            snprintf(ss_str, sizeof(ss_str), "%.0f days/s", days);
    }

    /* FPS — exponential moving average, alpha=0.1 (≈ 10-frame window) */
    Uint64 now  = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    if (s_fps_prev != 0 && freq > 0) {
        float inst = (float)freq / (float)(now - s_fps_prev);
        s_fps_smooth = (s_fps_smooth == 0.0f) ? inst
                     : s_fps_smooth + 0.1f * (inst - s_fps_smooth);
    }
    s_fps_prev = now;

    char fps_str[32];
    snprintf(fps_str, sizeof(fps_str), "%.0f fps", (double)s_fps_smooth);

    char nearest_str[64];
    nearest_body_distance_string(nearest_str, sizeof(nearest_str));

    SDL_Color white = {255, 255, 255, 220};
    update_text(&s_tc_move, mv_str, white);
    update_text(&s_tc_sim,  ss_str, white);
    update_text(&s_tc_fps,  fps_str, white);
    update_text(&s_tc_nearest, nearest_str, white);

    /* ---- layout ---- */
    const float BH   = (float)BAR_H;
    const float BW   = W * BAR_W_FRAC;
    const float BX   = (W - BW) * 0.5f;     /* centered */
    const float BY   = BAR_TOP;
    const float TY   = BY + BH + TEXT_GAP;  /* text baseline below bar */

    /* ---- GL state ---- */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_shader);
    glUniform2f(s_loc_screen, (float)WIN_W, (float)WIN_H);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    /* Bar background (dark, full width of bar) */
    draw_rect(BX, BY, BW, BH, 1.0f, 1.0f, 1.0f, 0.15f);

    /* Bar fill */
    draw_rect(BX, BY, BW * t, BH, 1.0f, 1.0f, 1.0f, 0.85f);

    /* Movement speed — centred below bar */
    if (s_tc_move.tex) {
        float tw = TH * (float)s_tc_move.w / (float)s_tc_move.h;
        draw_tex(&s_tc_move, (W - tw) * 0.5f, TY, TH);
    }

    /* Sim speed — right-aligned below bar */
    if (s_tc_sim.tex) {
        float tw = TH * (float)s_tc_sim.w / (float)s_tc_sim.h;
        draw_tex(&s_tc_sim, BX + BW - tw, TY, TH);
    }

    /* FPS — top-right corner, right-aligned to screen edge */
    if (s_tc_fps.tex) {
        const float MARGIN = 12.0f;
        float tw = TH * (float)s_tc_fps.w / (float)s_tc_fps.h;
        draw_tex(&s_tc_fps, W - tw - MARGIN, 8.0f, TH);
    }

    if (s_tc_nearest.tex) {
        draw_tex(&s_tc_nearest, BX, TY, TH);
    }

    draw_build_bar(W);
    draw_pause_menu(W, (float)WIN_H);

    /* ---- restore ---- */
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void ui_shutdown(void) {
    if (s_tc_move.tex) glDeleteTextures(1, &s_tc_move.tex);
    if (s_tc_sim.tex)  glDeleteTextures(1, &s_tc_sim.tex);
    if (s_tc_fps.tex)  glDeleteTextures(1, &s_tc_fps.tex);
    if (s_tc_nearest.tex) glDeleteTextures(1, &s_tc_nearest.tex);
    if (s_tc_build_title.tex) glDeleteTextures(1, &s_tc_build_title.tex);
    for (int i = 0; i < 8; i++)
        if (s_tc_build_items[i].tex) glDeleteTextures(1, &s_tc_build_items[i].tex);
    if (s_tc_pause_title.tex) glDeleteTextures(1, &s_tc_pause_title.tex);
    if (s_tc_pause_hint.tex) glDeleteTextures(1, &s_tc_pause_hint.tex);
    for (int i = 0; i < 4; i++)
        if (s_tc_pause_items[i].tex) glDeleteTextures(1, &s_tc_pause_items[i].tex);
    if (s_vbo)  glDeleteBuffers(1, &s_vbo);
    if (s_vao)  glDeleteVertexArrays(1, &s_vao);
    if (s_shader) glDeleteProgram(s_shader);
    if (s_menu_title_font && s_menu_title_font != s_menu_font && s_menu_title_font != s_font)
        TTF_CloseFont(s_menu_title_font);
    if (s_build_item_font && s_build_item_font != s_font)
        TTF_CloseFont(s_build_item_font);
    if (s_menu_font && s_menu_font != s_font)
        TTF_CloseFont(s_menu_font);
    if (s_font)   TTF_CloseFont(s_font);
    TTF_Quit();
    s_shader = s_vao = s_vbo = 0;
    s_font = NULL;
    s_build_item_font = NULL;
    s_menu_font = NULL;
    s_menu_title_font = NULL;
}
