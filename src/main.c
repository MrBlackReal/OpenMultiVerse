/*
 * main.c — application entry point
 *
 * Responsibilities:
 *   - SDL2 window + OpenGL 3.3 Core context
 *   - GLEW initialisation
 *   - Module init / shutdown sequence
 *   - Main loop: event handling, physics step, camera update, render
 *
 * Camera controls:
 *   W/S        - move forward / backward
 *   A/D        - strafe left / right
 *   Q/E        - move down / up
 *   Mouse drag - look (yaw/pitch)
 *   Scroll     - speed multiplier (×1.3 per notch)
 *   Space      - pause / resume simulation
 *   R          - reset camera
 *   +/-        - simulation speed up / down
 *   T          - toggle warp mode (interstellar camera speed)
 *   B          - toggle build mode
 *   F11/Alt+↵  - toggle fullscreen
 *
 * Physics integration overview (see physics.c for full detail):
 *   The simulation uses a two-rate RESPA integrator with per-system timesteps.
 *   Each planetary system runs independently with its own dt_outer / dt_inner.
 *   The main loop caps the per-frame sim time to MAX_OUTER_STEPS × dt_outer to
 *   prevent spiral-of-death when the frame rate drops.
 *
 * Warp mode:
 *   Camera speed range normally is [0.00001, 200] AU/s. Pressing T switches to
 *   the warp range [200, 63241] AU/s (≈ [0.003, 1] ly/s). Warp does not affect
 *   the simulation clock; only camera movement speed changes.
 */
#include "common.h"
#include "math3d.h"
#include "body.h"
#include "universe.h"
#include "physics.h"
#include "camera.h"
#include "starfield.h"
#include "nebula.h"
#include "galaxy.h"
#include "trails.h"
#include "labels.h"
#include "render.h"
#include "rings.h"
#include "asteroids.h"
#include "ui.h"
#include "build.h"
#include "inspect.h"
#include "collision.h"
#include "supernova.h"
#include "lifecycle.h"
#include "accretion.h"
#include "cosmic_field.h"
#include "radiance_field.h"
#include "field_graph.h"
#include "spectral.h"
#include "audio.h"
#include "presets.h"
#include "menu.h"
#include "post.h"
#include "loading.h"
#ifdef _OPENMP
#include <omp.h>
#endif

/* ── active universe ──────────────────────────────────────────────────────── */
/* Path of the universe JSON currently loaded. Changed via switch_universe();
 * init_runtime_world() reads this so a reset reloads the chosen multiverse. */
static char s_universe_path[512] = "assets/universe.json";

/* ── window / context ─────────────────────────────────────────────────────── */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
int g_win_w = DEFAULT_WIN_W;
int g_win_h = DEFAULT_WIN_H;
static int s_fullscreen = 0;

/* ── input state ──────────────────────────────────────────────────────────── */
static int   s_freelook   = 0;       /* 1 = free-look active, mouse captured */
static float s_mouse_sens = 0.25f;   /* degrees per pixel */
static int   s_vsync_enabled = 1;
static float s_music_vol  = 0.6f;

/* ── pause menu ───────────────────────────────────────────────────────────── */
/* Control tunables now live in g_settings (see settings.h); aliased here so the
 * existing call sites read the live value. */
#define SLIDER_STEP    (g_settings.slider_step)
#define MOUSE_SENS_MIN (g_settings.mouse_sens_min)
#define MOUSE_SENS_MAX (g_settings.mouse_sens_max)

static int s_pause_menu_open = 0;
static int s_pause_menu_selected = 0;
static int s_pause_menu_prev_paused = 0;
static int s_pause_page = 0;   /* 0 = main menu, 1 = controls */

enum {
    PAUSE_MENU_CONTINUE = 0,
    PAUSE_MENU_RESET_UNIVERSE,
    PAUSE_MENU_TOGGLE_VSYNC,
    PAUSE_MENU_CONTROLS,
    PAUSE_MENU_MUSIC_VOL,
    PAUSE_MENU_MOUSE_SENS,
    PAUSE_MENU_LEAVE,
    PAUSE_MENU_COUNT
};

/* Movement keys held */
static int s_key_w, s_key_s, s_key_a, s_key_d, s_key_q, s_key_e;

/* ── simulation speed table ───────────────────────────────────────────────── */
/* Clean display values in days/s. Index 4 (1.0 days/s) is the default.
 * The lowest non-zero entry (0.1 days/s) still lets the user watch fast orbiters.
 * The highest (365 days/s) runs one Earth year per real second. */
static const double SPEED_TABLE[] = {
    0.0,
    0.1, 0.25, 0.5,
    1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 100.0,
    365.0
};
#define SPEED_TABLE_LEN (int)(sizeof(SPEED_TABLE)/sizeof(SPEED_TABLE[0]))
static int s_speed_idx = 4;   /* start at 1.0 days/s */

/* ── warp mode ────────────────────────────────────────────────────────────── */
/* Warp mode (T key) — variable interstellar camera speed.
 * Normal range : [0.00001, 200] AU/s  (0.00001 AU/s ≈ walking pace near Earth)
 * Warp range   : [200, 63241] AU/s    (63241 AU/s = 1 ly/s, i.e. full warp)
 * Pressing T clamps the current speed to the warp range and shows "WARP" in HUD. */
#define WARP_SPEED_MIN_AU  (g_settings.warp_speed_min_au)
#define WARP_SPEED_MAX_AU  (g_settings.warp_speed_max_au)
int s_warp = 0;
int g_warp = 0;

/* ── logging helpers ──────────────────────────────────────────────────────── */
static void boot_log(const char *msg) {
    fprintf(stdout, "[Boot] %s\n", msg);
    fflush(stdout);
}

static void clear_movement_keys(void) {
    s_key_w = s_key_s = s_key_a = s_key_d = s_key_q = s_key_e = 0;
}

static void leave_inspect_keep_mouse(void) {
    inspect_cancel();
    s_freelook = 1;
    SDL_SetRelativeMouseMode(SDL_TRUE);
}

static int set_vsync(int enabled) {
    int interval = enabled ? 1 : 0;
    if (SDL_GL_SetSwapInterval(interval) != 0) {
        fprintf(stderr, "[Main] vsync toggle: %s\n", SDL_GetError());
        return 0;
    }
    s_vsync_enabled = enabled ? 1 : 0;
    fprintf(stdout, "[Main] V-Sync %s\n", s_vsync_enabled ? "ON" : "OFF");
    return 1;
}

static void set_music_vol(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_music_vol = vol;
    audio_set_music_volume(vol);
}

static void adjust_mouse_sensitivity(float delta) {
    s_mouse_sens += delta;
    if (s_mouse_sens < MOUSE_SENS_MIN) s_mouse_sens = MOUSE_SENS_MIN;
    if (s_mouse_sens > MOUSE_SENS_MAX) s_mouse_sens = MOUSE_SENS_MAX;
}

static void sync_pause_menu_ui(void) {
    ui_set_pause_menu(s_pause_menu_open, s_pause_menu_selected, s_vsync_enabled,
                      s_music_vol, s_mouse_sens, s_pause_page);
}

static void open_controls_page(void) {
    s_pause_page = 1;
    s_pause_menu_selected = -1;
    sync_pause_menu_ui();
}

static void close_controls_page(void) {
    s_pause_page = 0;
    s_pause_menu_selected = PAUSE_MENU_CONTROLS;
    sync_pause_menu_ui();
}

static void close_pause_menu(int resume_freelook) {
    s_pause_menu_open = 0;
    g_paused = s_pause_menu_prev_paused;
    if (resume_freelook) {
        s_freelook = 1;
        SDL_SetRelativeMouseMode(SDL_TRUE);
    } else {
        s_freelook = 0;
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }
    sync_pause_menu_ui();
}

static void open_pause_menu(void) {
    s_pause_menu_prev_paused = g_paused;
    s_pause_menu_open = 1;
    s_pause_menu_selected = PAUSE_MENU_CONTINUE;
    g_paused = 1;
    s_freelook = 0;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    clear_movement_keys();
    sync_pause_menu_ui();
}

static void move_pause_menu_selection(int delta) {
    s_pause_menu_selected += delta;
    while (s_pause_menu_selected < 0) s_pause_menu_selected += PAUSE_MENU_COUNT;
    while (s_pause_menu_selected >= PAUSE_MENU_COUNT) s_pause_menu_selected -= PAUSE_MENU_COUNT;
    sync_pause_menu_ui();
}

/* ── pre-simulation (warm-up) ─────────────────────────────────────────────── */
/*
 * warmup_universe — pre-simulate 2 years of physics before the first frame.
 *
 * Without a warm-up, freshly loaded elliptical orbits start at t=0 (periapsis)
 * and moons/rings appear bunched up at the same orbital phase. Running 2 years
 * of sim time spreads bodies to realistic positions.
 *
 * Per-system parallelism (OpenMP):
 *   Each planetary system runs independently; systems share no state during
 *   integration. OpenMP distributes systems across threads. Progress is reported
 *   from a critical section so console output isn't interleaved.
 *
 * Timestep model:
 *   Each system uses its own outer and inner timestep limits, queried from the
 *   physics module. n_inner = ceil(dt_outer / dt_inner) ensures the inner loop
 *   always covers the full outer step. outer_total = floor(WARMUP_DT / dt_outer).
 *   Trails are ticked alongside the integrator so the ring buffer is populated;
 *   otherwise the first frame would show empty trails.
 *
 * physics_advance_time() updates the global simulation clock after all systems
 * are done. It must be called once, not once per system, to keep g_sim_time
 * consistent with all bodies' state.
 */
/* Camera position in world metres (g_cam.pos is in AU). */
static void camera_world_m(double out[3]) {
    out[0] = g_cam.pos[0] * AU;
    out[1] = g_cam.pos[1] * AU;
    out[2] = g_cam.pos[2] * AU;
}

/* Squared distance (m²) from `root`'s star to the camera. */
static double system_cam_dist2(int root, const double cam_m[3]) {
    double dx = g_bodies[root].pos[0] - cam_m[0];
    double dy = g_bodies[root].pos[1] - cam_m[1];
    double dz = g_bodies[root].pos[2] - cam_m[2];
    return dx*dx + dy*dy + dz*dz;
}

/* Radius (light-years) of the live simulation region around the camera: only
 * systems this close are integrated each frame.  Everything farther is frozen
 * (a far-field dot) — this is what keeps a galaxy-scale universe real-time, and
 * also stops one distant tight exoplanet from capping everyone's timestep. */
#define ACTIVE_RADIUS_LY (g_settings.active_radius_ly)

static void warmup_universe(void) {
    /* A loaded snapshot already holds settled state at a specific instant;
     * pre-simulating would advance it away from what was saved. */
    if (g_universe_is_snapshot) {
        boot_log("Snapshot loaded — skipping warm-up");
        return;
    }
    const double WARMUP_DT = 365.0 * g_settings.warmup_years * DAY;
    int sys_n = physics_system_count();
    int completed = 0;

    /* Only pre-settle systems the camera starts near.  Distant systems are
     * far-field dots — warming them is wasted work (and a single tight close-in
     * exoplanet can need millions of substeps for 730 days).  They warm up
     * lazily if/when you fly to them.  The nearest system is always included so
     * single-system universes still settle. */
    const double WARMUP_RADIUS_LY = g_settings.warmup_radius_ly;
    double cam_m[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
    int nearest = -1;
    double nearest_d2 = 1e300, radius_m2 = (WARMUP_RADIUS_LY * LY) * (WARMUP_RADIUS_LY * LY);
    int n_warm = 0;
    for (int s = 0; s < sys_n; s++) {
        int root = physics_system_root(s);
        double dx = g_bodies[root].pos[0] - cam_m[0];
        double dy = g_bodies[root].pos[1] - cam_m[1];
        double dz = g_bodies[root].pos[2] - cam_m[2];
        double d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < nearest_d2) { nearest_d2 = d2; nearest = s; }
        if (d2 <= radius_m2) n_warm++;
    }
    if (n_warm == 0) n_warm = 1;   /* the nearest system is always warmed */

    fprintf(stdout, "[Boot] Warm-up: pre-simulating %.0f days across %d of %d "
                    "nearby system%s\n",
            WARMUP_DT / DAY, n_warm, sys_n, sys_n == 1 ? "" : "s");
    fflush(stdout);
    loading_status("Warming up %d system%s", n_warm, n_warm == 1 ? "" : "s");
    loading_progress(0.0);
    loading_tick();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
    for (int s = 0; s < sys_n; s++) {
        int root = physics_system_root(s);
        /* Skip far systems (but never the nearest). */
        {
            double dx = g_bodies[root].pos[0] - cam_m[0];
            double dy = g_bodies[root].pos[1] - cam_m[1];
            double dz = g_bodies[root].pos[2] - cam_m[2];
            if (s != nearest && dx*dx + dy*dy + dz*dz > radius_m2) continue;
        }
        double step_outer = physics_system_outer_dt_limit(s);
        double step_inner = physics_system_inner_dt_limit(s);
        int n_inner = (int)(step_outer / step_inner) + 1;
        int outer_total = (int)(WARMUP_DT / step_outer);
        for (int o = 0; o < outer_total; o++) {
            double dt_outer = WARMUP_DT / outer_total;
            double dt_inner = dt_outer / n_inner;
            physics_respa_begin_system(root, dt_outer);
            for (int i = 0; i < n_inner; i++) {
                physics_respa_inner_system(root, dt_inner);
            }
            physics_respa_end_system(root, dt_outer);
            trails_tick_system(root, dt_outer);
        }
#ifdef _OPENMP
#pragma omp critical
#endif
        {
            completed++;
            fprintf(stdout, "[Boot] Warm-up progress: %d/%d systems (%s)\n",
                    completed, sys_n, g_bodies[root].name);
            fflush(stdout);
            /* Only the master thread owns the GL context, so only it may draw
             * the loading overlay; workers keep integrating meanwhile. */
#ifdef _OPENMP
            if (omp_get_thread_num() == 0)
#endif
            {
                loading_status("Warming up systems  %d / %d", completed, n_warm);
                loading_progress((double)completed / (double)n_warm);
                loading_tick();
            }
        }
    }
    loading_progress(1.0);
    loading_tick();
    physics_advance_time(WARMUP_DT);
    boot_log("Warm-up complete");
}

/* ── world init / shutdown ────────────────────────────────────────────────── */
/* Set a loading-overlay phase: status text + indeterminate sweep + one frame.
 * Used for the quick GL-init steps, which each run as a single blocking call. */
static void loading_phase(const char *label) {
    loading_status("%s", label);
    loading_indeterminate();
    loading_tick();
}

static void init_runtime_world(void) {
    boot_log("Preparing runtime world");
    loading_begin();
    loading_phase("Loading universe");
    universe_load(s_universe_path);   /* drives its own determinate progress */
    boot_log("Resetting camera");
    cam_reset();
    loading_phase("Generating starfield");
    boot_log("Initializing starfield");
    starfield_init();
    loading_phase("Placing nebulae");
    boot_log("Initializing nebulae");
    nebula_init();
    boot_log("Initializing galaxies");
    galaxy_init();
    loading_phase("Allocating trails");
    boot_log("Initializing trails");
    trails_gl_init();
    loading_phase("Initializing renderer");
    boot_log("Initializing renderer");
    render_init();
    loading_phase("Initializing post-processing");
    boot_log("Initializing post-processing");
    post_init();
    post_set_tonemap(g_settings.tonemap_mode, g_settings.tonemap_exposure);
    post_set_optics(g_settings.auto_exposure, g_settings.chromatic_aberration,
                    g_settings.vignette);
    loading_phase("Building ring systems");
    boot_log("Initializing rings");
    rings_init(s_universe_path);
    loading_phase("Building asteroid belts");
    boot_log("Initializing asteroid belts");
    asteroids_init(s_universe_path);
    loading_phase("Preparing labels");
    boot_log("Initializing labels");
    labels_init();
    boot_log("Initializing build mode");
    build_init();
    boot_log("Initializing inspect mode");
    inspect_init();
    loading_phase("Building acceleration structures");
    boot_log("Refreshing physics timestep model");
    physics_refresh_timestep_model();
    boot_log("Building cosmic density field");
    cosmic_field_rebuild();
    boot_log("Building radiance field");
    radiance_field_rebuild();
    boot_log("Building field graph");
    field_graph_rebuild();
    warmup_universe();
    boot_log("Runtime world ready");
    loading_end();
}

static void shutdown_runtime_world(void) {
    asteroids_shutdown();
    rings_shutdown();
    render_shutdown();
    labels_shutdown();
    trails_gl_shutdown();
    galaxy_shutdown();
    nebula_shutdown();
    starfield_shutdown();
    universe_shutdown();
}

/* Tear down and rebuild everything (triggered by "Reset Universe" menu item). */
static void reset_universe_state(void) {
    shutdown_runtime_world();
    collision_reset();
    supernova_reset();
    field_graph_reset();   /* event history belongs to the old universe */
    clear_movement_keys();
    s_freelook = 0;
    s_warp = 0;
    g_warp = 0;
    s_speed_idx = 4;
    g_sim_time = 0.0;
    g_sim_speed = SPEED_TABLE[s_speed_idx] * DAY;
    g_paused = 0;
    s_pause_menu_open = 0;
    s_pause_menu_selected = PAUSE_MENU_CONTINUE;
    s_pause_menu_prev_paused = 0;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    sync_pause_menu_ui();
    init_runtime_world();
}

/* Load a different universe (from the picker). Falls back to a no-op if the
 * path is empty; otherwise records it and rebuilds the world from scratch. */
static void switch_universe(const char *path) {
    if (!path || !path[0]) return;
    /* Pre-flight the file before tearing down the live world: a missing or
     * malformed user-supplied path (typed into the menu's load box, or a failed
     * catalog import) must be a no-op, not abort the process via the exit(1)
     * inside universe_load(). */
    if (universe_validate(path) != 0) {
        fprintf(stderr, "[Sim] cannot load universe '%s' (missing, unparseable, "
                        "or no bodies) - keeping current universe\n", path);
        fflush(stderr);
        return;
    }
    snprintf(s_universe_path, sizeof(s_universe_path), "%s", path);
    fprintf(stdout, "[Sim] switching universe -> %s\n", s_universe_path);
    fflush(stdout);
    reset_universe_state();
}

/* ── init / quit ──────────────────────────────────────────────────────────── */
static void update_viewport_size(void) {
    int w = DEFAULT_WIN_W;
    int h = DEFAULT_WIN_H;
    if (s_win) SDL_GL_GetDrawableSize(s_win, &w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    g_win_w = w;
    g_win_h = h;
    glViewport(0, 0, g_win_w, g_win_h);
}

static void toggle_fullscreen(void) {
    Uint32 flags = s_fullscreen ? 0u : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(s_win, flags) != 0) {
        fprintf(stderr, "[Main] fullscreen toggle: %s\n", SDL_GetError());
        return;
    }
    s_fullscreen = !s_fullscreen;
    update_viewport_size();
}

static int app_init(void) {
    boot_log("Initializing SDL");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[Main] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    s_win = SDL_CreateWindow("OpenVerse Simulator",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             DEFAULT_WIN_W, DEFAULT_WIN_H,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_win) {
        fprintf(stderr, "[Main] SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }

    {
        SDL_Surface *icon = SDL_LoadBMP("assets/window_icon.bmp");
        if (!icon) {
            fprintf(stderr, "[Main] window icon: %s\n", SDL_GetError());
        } else {
            SDL_SetWindowIcon(s_win, icon);
            SDL_FreeSurface(icon);
        }
    }

    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "[Main] GL context: %s\n", SDL_GetError());
        return 0;
    }

    boot_log("Configuring swap interval");
    set_vsync(1);

    /* GLEW */
    boot_log("Initializing GLEW");
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    /* GLEW loads the GL function pointers first, then probes GLX. Under the
     * offscreen/EGL (surfaceless) driver there is no GLX display, so glewInit()
     * returns GLEW_ERROR_NO_GLX_DISPLAY *after* the core pointers are already
     * loaded — harmless for headless rendering, so tolerate just that one. */
    if (err != GLEW_OK
#ifdef GLEW_ERROR_NO_GLX_DISPLAY
        && err != GLEW_ERROR_NO_GLX_DISPLAY
#endif
        ) {
        fprintf(stderr, "[Main] GLEW: %s\n", glewGetErrorString(err));
        return 0;
    }
    /* glewInit() spuriously sets GL_INVALID_ENUM on some drivers; flush it */
    glGetError();

    fprintf(stdout, "[Main] OpenGL %s | GLSL %s\n",
            glGetString(GL_VERSION),
            glGetString(GL_SHADING_LANGUAGE_VERSION));

    glEnable(GL_MULTISAMPLE);
    glClearColor(0.0f, 0.0f, 0.02f, 1.0f);
    update_viewport_size();
    boot_log("OpenGL context ready");

    boot_log("Initializing audio");
    audio_init();

    boot_log("Initializing universe menu");
    menu_init(s_win, s_ctx);

    return 1;
}

static void app_quit(void) {
    if (settings_dirty())     /* persist only if something changed this session */
        settings_save();
    audio_shutdown();
    menu_shutdown();
    loading_shutdown();
    ui_shutdown();
    field_graph_shutdown();
    radiance_field_shutdown();
    cosmic_field_shutdown();
    shutdown_runtime_world();
    SDL_GL_DeleteContext(s_ctx);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
}

/* ── event handling ───────────────────────────────────────────────────────── */
static void activate_pause_menu_action(int *running) {
    switch (s_pause_menu_selected) {
    case PAUSE_MENU_CONTINUE:
        close_pause_menu(1);
        break;
    case PAUSE_MENU_RESET_UNIVERSE:
        reset_universe_state();
        break;
    case PAUSE_MENU_TOGGLE_VSYNC:
        set_vsync(!s_vsync_enabled);
        sync_pause_menu_ui();
        break;
    case PAUSE_MENU_CONTROLS:
        open_controls_page();
        break;
    case PAUSE_MENU_MUSIC_VOL:
    case PAUSE_MENU_MOUSE_SENS:
        /* adjusted via left/right arrows or scroll, not Enter */
        break;
    case PAUSE_MENU_LEAVE:
        *running = 0;
        break;
    }
}

static void handle_event(const SDL_Event *e, float dt, int *running) {
    if (s_pause_menu_open && s_pause_page == 1) {
        /* ── controls page ── */
        switch (e->type) {
        case SDL_KEYDOWN:
            switch (e->key.keysym.sym) {
            case SDLK_ESCAPE:
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                close_controls_page();
                break;
            default: break;
            }
            break;
        case SDL_MOUSEMOTION: {
            int hit = ui_controls_return_hit_test(e->motion.x, e->motion.y);
            int sel = hit ? 0 : -1;
            if (sel != s_pause_menu_selected) {
                s_pause_menu_selected = sel;
                sync_pause_menu_ui();
            }
        }   break;
        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT &&
                ui_controls_return_hit_test(e->button.x, e->button.y))
                close_controls_page();
            break;
        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e->window.event == SDL_WINDOWEVENT_RESIZED)
                update_viewport_size();
            break;
        default: break;
        }
        (void)dt; (void)running;
        return;
    }

    if (s_pause_menu_open) {
        switch (e->type) {
        case SDL_QUIT:
            break;

        case SDL_KEYDOWN:
            switch (e->key.keysym.sym) {
            case SDLK_ESCAPE:
                close_pause_menu(1);
                break;
            case SDLK_UP:
            case SDLK_w:
                if (!e->key.repeat) move_pause_menu_selection(-1);
                break;
            case SDLK_DOWN:
            case SDLK_s:
                if (!e->key.repeat) move_pause_menu_selection(1);
                break;
            case SDLK_LEFT:
                if (s_pause_menu_selected == PAUSE_MENU_MUSIC_VOL)
                    set_music_vol(s_music_vol - SLIDER_STEP);
                else if (s_pause_menu_selected == PAUSE_MENU_MOUSE_SENS)
                    adjust_mouse_sensitivity(-SLIDER_STEP);
                sync_pause_menu_ui();
                break;
            case SDLK_RIGHT:
                if (s_pause_menu_selected == PAUSE_MENU_MUSIC_VOL)
                    set_music_vol(s_music_vol + SLIDER_STEP);
                else if (s_pause_menu_selected == PAUSE_MENU_MOUSE_SENS)
                    adjust_mouse_sensitivity(SLIDER_STEP);
                sync_pause_menu_ui();
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                if (!e->key.repeat) activate_pause_menu_action(running);
                break;
            default:
                break;
            }
            break;

        case SDL_MOUSEWHEEL:
            if (s_pause_menu_selected == PAUSE_MENU_MUSIC_VOL)
                set_music_vol(s_music_vol + e->wheel.y * SLIDER_STEP);
            else if (s_pause_menu_selected == PAUSE_MENU_MOUSE_SENS)
                adjust_mouse_sensitivity(e->wheel.y * SLIDER_STEP);
            sync_pause_menu_ui();
            break;

        case SDL_MOUSEMOTION:
        {
            int hover = ui_pause_menu_hit_test(e->motion.x, e->motion.y);
            if (hover != s_pause_menu_selected) {
                s_pause_menu_selected = hover;
                sync_pause_menu_ui();
            }
        }   break;

        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT) {
                int hover = ui_pause_menu_hit_test(e->button.x, e->button.y);
                if (hover >= 0) {
                    s_pause_menu_selected = hover;
                    sync_pause_menu_ui();
                    if (hover == PAUSE_MENU_MUSIC_VOL || hover == PAUSE_MENU_MOUSE_SENS) {
                        int delta = ui_pause_menu_slider_click_delta(e->button.x, e->button.y);
                        if (delta == -1) {
                            if (hover == PAUSE_MENU_MUSIC_VOL)
                                set_music_vol(s_music_vol - SLIDER_STEP);
                            else
                                adjust_mouse_sensitivity(-SLIDER_STEP);
                            sync_pause_menu_ui();
                        } else if (delta == 1) {
                            if (hover == PAUSE_MENU_MUSIC_VOL)
                                set_music_vol(s_music_vol + SLIDER_STEP);
                            else
                                adjust_mouse_sensitivity(SLIDER_STEP);
                            sync_pause_menu_ui();
                        }
                    } else {
                        activate_pause_menu_action(running);
                    }
                }
            }
            break;

        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e->window.event == SDL_WINDOWEVENT_RESIZED) {
                update_viewport_size();
            }
            break;

        default:
            break;
        }
        (void)dt;
        return;
    }

    switch (e->type) {
    case SDL_QUIT:
        /* handled in main loop */
        break;

    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_w:
            s_key_w = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_s:
            s_key_s = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_a:
            s_key_a = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_d:
            s_key_d = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_q:
            s_key_q = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_e:
            s_key_e = 1;
            if (g_inspect_orbit_mode) leave_inspect_keep_mouse();
            break;
        case SDLK_r: cam_reset(); break;
        case SDLK_F11:
            if (!e->key.repeat) toggle_fullscreen();
            break;
        case SDLK_RETURN:
            if (!e->key.repeat && (e->key.keysym.mod & KMOD_ALT))
                toggle_fullscreen();
            break;
        case SDLK_b:
            if (!e->key.repeat) {
                if (g_inspect_mode)
                    leave_inspect_keep_mouse();
                build_toggle();
            }
            break;
        case SDLK_i:
            if (!e->key.repeat) {
                if (g_build_mode) build_toggle();
                inspect_toggle();
                s_freelook = 1;
                SDL_SetRelativeMouseMode(SDL_TRUE);
            }
            break;
        case SDLK_u:
            /* Toggle the multiverse picker. When it opens, release the mouse so
             * the cursor can interact with the ImGui window. No-op without
             * USE_IMGUI (menu_visible() stays 0). */
            if (!e->key.repeat) {
                menu_toggle();
                if (menu_visible()) {
                    s_freelook = 0;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
            }
            break;
        case SDLK_TAB:
            build_set_tab_held(1);
            break;
        case SDLK_t:
            /* Toggle warp mode, clamping speed into the appropriate range. */
            s_warp = !s_warp;
            g_warp = s_warp;
            if (s_warp) {
                if (g_cam.speed < WARP_SPEED_MIN_AU) g_cam.speed = WARP_SPEED_MIN_AU;
                if (g_cam.speed > WARP_SPEED_MAX_AU) g_cam.speed = WARP_SPEED_MAX_AU;
            } else {
                if (g_cam.speed > WARP_SPEED_MIN_AU) g_cam.speed = WARP_SPEED_MIN_AU;
            }
            fprintf(stdout, "[Cam] warp %s (%.0f AU/s = %.4f ly/s)\n",
                    s_warp ? "ON" : "OFF",
                    (double)g_cam.speed,
                    (double)(g_cam.speed / WARP_SPEED_MAX_AU));
            break;
        case SDLK_SPACE:
            g_paused = !g_paused;
            break;
        case SDLK_ESCAPE:
            if (g_build_mode) {
                build_toggle();
                break;
            }
            if (g_inspect_mode) {
                leave_inspect_keep_mouse();
                clear_movement_keys();
                break;
            }
            if (s_freelook) {
                open_pause_menu();
            }
            break;
        case SDLK_EQUALS:
        case SDLK_PLUS:
            if (s_speed_idx < SPEED_TABLE_LEN - 1) s_speed_idx++;
            g_sim_speed = SPEED_TABLE[s_speed_idx] * DAY;
            ui_notify_speed_change();
            fprintf(stdout, "[Sim] speed = %g days/s\n",
                    SPEED_TABLE[s_speed_idx]);
            break;
        case SDLK_MINUS:
            if (s_speed_idx > 0) s_speed_idx--;
            g_sim_speed = SPEED_TABLE[s_speed_idx] * DAY;
            ui_notify_speed_change();
            fprintf(stdout, "[Sim] speed = %g days/s\n",
                    SPEED_TABLE[s_speed_idx]);
            break;
        }
        break;

    case SDL_KEYUP:
        switch (e->key.keysym.sym) {
        case SDLK_w: s_key_w = 0; break;
        case SDLK_s: s_key_s = 0; break;
        case SDLK_a: s_key_a = 0; break;
        case SDLK_d: s_key_d = 0; break;
        case SDLK_q: s_key_q = 0; break;
        case SDLK_e: s_key_e = 0; break;
        case SDLK_TAB: build_set_tab_held(0); break;
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (g_build_mode && e->button.button == SDL_BUTTON_LEFT) {
            build_place_current();
            break;
        }
        if (g_inspect_mode && e->button.button == SDL_BUTTON_LEFT) {
            inspect_begin_orbit();
            break;
        }
        if (e->button.button == SDL_BUTTON_LEFT && !s_freelook) {
            s_freelook = 1;
            SDL_SetRelativeMouseMode(SDL_TRUE);
        }
        break;

    case SDL_MOUSEMOTION:
        if (g_inspect_orbit_mode) {
            inspect_orbit_mouse(e->motion.xrel, e->motion.yrel, s_mouse_sens);
        } else if (s_freelook) {
            g_cam.yaw   += e->motion.xrel * s_mouse_sens;
            g_cam.pitch -= e->motion.yrel * s_mouse_sens;
            if (g_cam.pitch >  89.0f) g_cam.pitch =  89.0f;
            if (g_cam.pitch < -89.0f) g_cam.pitch = -89.0f;
        }
        break;

    case SDL_MOUSEWHEEL:
        if (g_inspect_orbit_mode) {
            inspect_orbit_zoom(e->wheel.y);
            break;
        }
        if (g_build_mode && g_build_tab_held) {
            build_scroll(e->wheel.y);
            break;
        }
        /* Speed steps by ×1.3 per notch; clamped to the active range. */
        g_cam.speed *= (e->wheel.y > 0) ? 1.3f : (1.0f / 1.3f);
        if (s_warp) {
            if (g_cam.speed < WARP_SPEED_MIN_AU) g_cam.speed = WARP_SPEED_MIN_AU;
            if (g_cam.speed > WARP_SPEED_MAX_AU) g_cam.speed = WARP_SPEED_MAX_AU;
        } else {
            if (g_cam.speed < 0.00001f)          g_cam.speed = 0.00001f;
            if (g_cam.speed > WARP_SPEED_MIN_AU) g_cam.speed = WARP_SPEED_MIN_AU;
        }
        break;

    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED) {
            update_viewport_size();
        }
        break;

    default:
        break;
    }
    (void)dt;
}

/* ── camera movement ──────────────────────────────────────────────────────── */
/*
 * camera_move — apply WASDQE movement each frame.
 *
 * Position is stored as double-precision AU. Speed is in AU/s. Delta is
 * computed as double so that small increments (slow camera near planets) are
 * not lost to float32 ULP at large absolute coordinates. The right vector is
 * derived from (forward × Y_up) projected onto the XZ plane — this keeps
 * Q/E purely vertical regardless of pitch.
 */
static void camera_move(float dt) {
    float fdx, fdy, fdz;
    cam_get_dir(&fdx, &fdy, &fdz);

    /* Right vector: cross(forward, world_up) projected to XZ */
    float rx = -fdz, rz = fdx;
    float rlen = sqrtf(rx*rx + rz*rz);
    if (rlen > 1e-6f) { rx /= rlen; rz /= rlen; }

    double speed = (double)g_cam.speed;

    /* Adaptive warp (the §0.1 zoom-out): in warp, the effective speed also
     * scales with the distance to the nearest body — each decade of scale
     * takes a fixed ~18 s of flight, so interstellar space, the galaxy, and
     * the Local Group are all reachable by just holding W.  v = dist/8 means
     * the factor only ever *raises* the set speed once you are far from
     * everything, and approaching a target automatically decelerates. */
    if (s_warp && g_settings.adaptive_warp) {
        double cx = g_cam.pos[0] * AU, cy = g_cam.pos[1] * AU,
               cz = g_cam.pos[2] * AU;
        double best2 = -1.0;
        for (int i = 0; i < g_nbodies; i++) {
            if (!g_bodies[i].alive) continue;
            double dx = g_bodies[i].pos[0] - cx;
            double dy = g_bodies[i].pos[1] - cy;
            double dz = g_bodies[i].pos[2] - cz;
            double d2 = dx*dx + dy*dy + dz*dz;
            if (best2 < 0.0 || d2 < best2) best2 = d2;
        }
        double best_au = best2 > 0.0 ? sqrt(best2) * RS : -1.0;

        /* Galaxies are anchors too, or arriving at one would never slow
         * down (their stars aren't bodies): distance to the volume's edge,
         * floored inside so intra-galaxy travel stays brisk (~2% of the
         * radius ≈ 1 kly for the Milky Way → ~150 ly/s). */
        for (int i = 0; i < galaxy_count(); i++) {
            double gp[3], gr = galaxy_radius_au(i);
            galaxy_position(i, gp);
            double dx = gp[0] - g_cam.pos[0];
            double dy = gp[1] - g_cam.pos[1];
            double dz = gp[2] - g_cam.pos[2];
            double d  = sqrt(dx*dx + dy*dy + dz*dz) - 0.85 * gr;
            if (d < 0.02 * gr) d = 0.02 * gr;
            if (best_au < 0.0 || d < best_au) best_au = d;
        }

        if (best_au > 0.0) {
            double v_scale = best_au / 8.0;            /* AU/s */
            if (v_scale > speed) speed = v_scale;
        }
    }

    double dspd = speed * (double)dt;

    if (s_key_w) { g_cam.pos[0] += (double)fdx*dspd; g_cam.pos[1] += (double)fdy*dspd; g_cam.pos[2] += (double)fdz*dspd; }
    if (s_key_s) { g_cam.pos[0] -= (double)fdx*dspd; g_cam.pos[1] -= (double)fdy*dspd; g_cam.pos[2] -= (double)fdz*dspd; }
    if (s_key_d) { g_cam.pos[0] += (double)rx*dspd;                                      g_cam.pos[2] += (double)rz*dspd;  }
    if (s_key_a) { g_cam.pos[0] -= (double)rx*dspd;                                      g_cam.pos[2] -= (double)rz*dspd;  }
    if (s_key_e) { g_cam.pos[1] += dspd; }
    if (s_key_q) { g_cam.pos[1] -= dspd; }
}

/* ── main loop ────────────────────────────────────────────────────────────── */
/*
 * Physics integration loop (inside main):
 *
 * Per-frame sim time (sim_dt = g_sim_speed × dt_real) is capped to
 * MAX_OUTER_STEPS × dt_outer_max per system. This prevents the simulation
 * from freezing under low frame rates or high sim speeds.
 *
 * effective_sim_dt: the minimum of all systems' caps. All systems advance by
 * the same wall-clock step so g_sim_time stays consistent.
 *
 * Close-approach subdivision:
 *   collision_system_close_approach_subdivide() returns a factor > 1 when
 *   bodies in a system are near a collision threshold. outer_steps and dt_outer
 *   are refined by this factor, ensuring the collision detector sees smaller
 *   steps and can pinpoint the impact time accurately.
 *
 * Local encounter re-snapshot:
 *   collision_system_maybe_has_encounter() checks if any pair in the system
 *   is within encounter range for this outer step. If so, a new frame snapshot
 *   is taken (trails + positions) at the start of that step, so rollback on
 *   collision is accurate to within one dt_outer rather than one full frame.
 *
 * View matrices:
 *   'view'     — full lookAt including translation (float eye). Used only for
 *                ring rendering which is already in float world coordinates.
 *   'view_rot' — lookAt from the origin with direction only (no translation).
 *                Combined with proj to form vp_camrel = proj × view_rot, which
 *                is what render_frame() uses for all distant geometry. Avoids
 *                float32 cancellation at interstellar distances.
 */
/* ---- headless screenshot ---------------------------------------------------
 * Read the default framebuffer (GL_BACK, before the swap) and write a binary
 * PPM (P6), flipped to top-down.  Used by the --shot path so offscreen renders
 * can be captured and inspected without a display. */
static void save_screenshot_ppm(const char *path) {
    int w = WIN_W, h = WIN_H;
    if (w < 1 || h < 1) return;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        fprintf(stdout, "[Shot] wrote %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

int main(int argc, char **argv) {
    /* ---- headless / screenshot CLI ----------------------------------------
     * --headless           run with SDL's offscreen (EGL surfaceless) driver,
     *                      no window on the desktop.
     * --shot PATH          render --frames frames, dump PATH (PPM), then quit.
     * --frames N           frames to render before the shot (default 6).
     * --preset PATH        load this universe JSON instead of the default.
     * --cam x,y,z,yaw,pitch position the camera (AU, degrees) after load.    */
    const char *shot_path   = NULL;
    int         shot_frames = 6;
    int         headless    = 0;
    int         cam_set     = 0;
    double      cam_pos[3]  = { 0.0, 0.0, 0.0 };
    float       cam_yaw = 0.0f, cam_pitch = 0.0f;

    for (int a = 1; a < argc; a++) {
        if      (!strcmp(argv[a], "--headless")) headless = 1;
        else if (!strcmp(argv[a], "--shot")    && a + 1 < argc) shot_path   = argv[++a];
        else if (!strcmp(argv[a], "--frames")  && a + 1 < argc) shot_frames = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--preset")  && a + 1 < argc)
            snprintf(s_universe_path, sizeof s_universe_path, "%s", argv[++a]);
        else if (!strcmp(argv[a], "--cam")     && a + 1 < argc)
            cam_set = (sscanf(argv[++a], "%lf,%lf,%lf,%f,%f",
                              &cam_pos[0], &cam_pos[1], &cam_pos[2],
                              &cam_yaw, &cam_pitch) == 5);
        /* Stellar-clock rate (years of stellar evolution per real second) —
         * normally a menu slider; exposed as a flag so lifecycle-driven events
         * (aging, supernovae, accretion fading) are testable headless. */
        else if (!strcmp(argv[a], "--stellar-rate") && a + 1 < argc)
            g_stellar_years_per_sec = atof(argv[++a]);
    }
    if (shot_frames < 1) shot_frames = 1;
    if (headless) {
        setenv("SDL_VIDEODRIVER", "offscreen", 1);  /* EGL surfaceless, no window */
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    }

    /* Global settings first — every later macro (FOV, NUM_STARS, …) reads
     * g_settings, so it must be populated before anything else runs. */
    settings_load();

    if (!app_init()) return 1;

    boot_log("Resetting collision state");
    collision_reset();
    boot_log("Resetting supernova state");
    supernova_reset();
    boot_log("Initializing cosmic field");
    cosmic_field_init();
    boot_log("Initializing radiance field");
    radiance_field_init();
    boot_log("Initializing field graph");
    field_graph_init();
    boot_log("Initializing UI");
    ui_init();
    sync_pause_menu_ui();
    boot_log("Initializing loading overlay");
    loading_init(s_win);
    init_runtime_world();

    /* Headless camera override (after the world load, which resets the camera). */
    if (cam_set) {
        g_cam.pos[0] = cam_pos[0];
        g_cam.pos[1] = cam_pos[1];
        g_cam.pos[2] = cam_pos[2];
        g_cam.yaw    = cam_yaw;
        g_cam.pitch  = cam_pitch;
    }

    /* Headless: print a grep-able cosmic-field sample at the (possibly
     * overridden) camera, so the density field is verifiable without pixels. */
    if (headless) {
        CosmicSample cs;
        cosmic_field_rebuild();          /* reflect post-warmup positions       */
        cosmic_field_sample_camera(&cs);
        fprintf(stdout,
                "[CosmicField] n=%d rho=%.3e/ly3 mass=%.3e/ly3 clump=%.3f "
                "fill=%.3f nebulae=%d class=%s\n",
                cs.body_count, cs.number_density, cs.mass_density,
                cs.clumpiness, cs.continuous_fill, cs.nebulae_hit,
                cosmic_field_class_name(cs.dominant));

        /* Same for the radiance field: total/dominant incident flux at the
         * camera, so lighting is verifiable without pixels (Sun @ 1 AU ≈ 1361). */
        RadianceSample rs;
        radiance_field_rebuild();
        if (radiance_field_sample_camera(&rs)) {
            SpectralClass sc = {0};
            if (rs.dominant >= 0)
                spectral_classify(&g_bodies[rs.dominant], &sc);
            fprintf(stdout,
                    "[RadianceField] irr=%.4e W/m2 dom=%s cls=%s dom_irr=%.4e "
                    "L_dom=%.4e W n=%d\n",
                    rs.irradiance, rs.dom_label,
                    sc.class_str[0] ? sc.class_str : "-",
                    rs.dom_irr,
                    radiance_field_body_luminosity(rs.dominant), rs.n_sources);
        } else {
            fprintf(stdout, "[RadianceField] no emitters\n");
        }

        /* And the field graph: node/edge/event counts, so the harvested
         * relations are verifiable without pixels. Printed again at shot time
         * (gas flows and events only appear after stellar time has run). */
        FieldGraphStats fs;
        field_graph_rebuild();
        field_graph_stats(&fs);
        fprintf(stdout,
                "[FieldGraph] nodes=%d (stars=%d planets=%d holes=%d "
                "nebulae=%d galaxies=%d) edges=%d (grav=%d flow=%d) events=%d\n",
                fs.nodes, fs.stars, fs.planets, fs.black_holes, fs.nebulae,
                fs.galaxies, fs.edges, fs.grav_edges, fs.flow_edges,
                fs.events_logged);
    }

    /* Timing */
    Uint64 freq    = SDL_GetPerformanceFrequency();
    Uint64 prev    = SDL_GetPerformanceCounter();
    int    running = 1;
    int    frame_no = 0;

    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float  dt  = (float)((double)(now - prev) / (double)freq);
        if (dt > 0.1f) dt = 0.1f;  /* clamp: don't spiral if a frame takes > 100 ms */
        prev = now;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (menu_process_event(&e)) continue;  /* ImGui consumed this event */
            handle_event(&e, dt, &running);
        }

        /* Navigate-teleport fly animation. Any manual move key cancels it so
         * the user is never locked out of control mid-flight. */
        if (cam_fly_active()) {
            if (s_key_w || s_key_s || s_key_a || s_key_d || s_key_q || s_key_e)
                cam_fly_cancel();
            else
                cam_fly_update(dt);
        }

        if (!cam_fly_active() && !s_pause_menu_open && !g_inspect_orbit_mode)
            camera_move(dt);

        /* Physics — RESPA hierarchical integrator */
        if (!g_paused && g_sim_speed > 0.0) {
            const int MAX_OUTER_STEPS = 120;
            physics_refresh_timestep_model();
            {
                int sys_n = physics_system_count();
                /* g_laws.time_scale lets a universe run its clock faster/slower
                 * than real-world speed presets; the per-system caps below still
                 * bound each outer step, preserving integrator stability. */
                double sim_dt = g_sim_speed * dt * g_laws.time_scale;
                double effective_sim_dt = sim_dt;

                double cam_m[3]; camera_world_m(cam_m);
                const double active_r2 =
                    (ACTIVE_RADIUS_LY * LY) * (ACTIVE_RADIUS_LY * LY);

                /* Find the most constrained ACTIVE system and cap to it.  A
                 * frozen distant system must not drag down the timestep (and
                 * thus the frame rate) of the system you are actually in. */
                for (int s = 0; s < sys_n; s++) {
                    if (system_cam_dist2(physics_system_root(s), cam_m) > active_r2)
                        continue;
                    double dt_outer_max = physics_system_outer_dt_limit(s);
                    double sys_cap = dt_outer_max * MAX_OUTER_STEPS;
                    if (effective_sim_dt > sys_cap)
                        effective_sim_dt = sys_cap;
                }

                trails_begin_frame_snapshot();
                collision_snapshot_positions();
                for (int s = 0; s < sys_n; s++) {
                    int root = physics_system_root(s);
                    /* Freeze systems outside the active region — they are
                     * far-field dots, so integrating them is wasted work. */
                    if (system_cam_dist2(root, cam_m) > active_r2) continue;
                    double dt_outer_max = physics_system_outer_dt_limit(s);
                    double dt_inner_max = physics_system_inner_dt_limit(s);
                    double sys_dt = effective_sim_dt;

                    int outer_steps = (int)(sys_dt / dt_outer_max) + 1;
                    double dt_outer = sys_dt / outer_steps;

                    /* Subdivide further if a close approach is detected */
                    int ca_factor = collision_system_close_approach_subdivide(root, dt_outer);
                    outer_steps *= ca_factor;
                    dt_outer    /= ca_factor;

                    int n_inner = (int)(dt_outer / dt_inner_max) + 1;
                    double dt_inner = dt_outer / n_inner;

                    for (int o = 0; o < outer_steps; o++) {
                        /* Re-snapshot at encounter onset for sub-frame rollback */
                        int local_encounter = collision_system_maybe_has_encounter(root, dt_outer);
                        if (local_encounter) {
                            trails_begin_frame_snapshot();
                            collision_snapshot_positions();
                        }
                        physics_respa_begin_system(root, dt_outer);
                        for (int i = 0; i < n_inner; i++) {
                            physics_respa_inner_system(root, dt_inner);
                        }
                        physics_respa_end_system(root, dt_outer);
                        rings_step_system(root, dt_outer);
                        trails_tick_system(root, dt_outer);
                        if (local_encounter) {
                            collision_step_system(root, dt_outer);
                        }
                    }
                }
                physics_advance_time(effective_sim_dt);
                supernova_step(effective_sim_dt);
                collision_step(effective_sim_dt);
                asteroids_step(effective_sim_dt);
                rings_tick(effective_sim_dt);
                /* Stellar evolution runs on its own clock (years/real-second),
                 * decoupled from the capped orbital dt — so a star can age and
                 * die without ever speeding up the integrator. dt is the real
                 * frame time; no-op unless auto-aging is enabled. */
                lifecycle_step(dt);
                /* Black-hole accretion runs on the same stellar clock: quasars
                 * drain their gas reservoir → Ṁ → Eddington ratio (activity) and
                 * grow, so they visibly fade over cosmic time. No-op at rate 0. */
                accretion_step(dt);
                /* Safety net: if a step produced a non-finite body (NaN/inf),
                 * remove it before it corrupts the camera-relative render math
                 * and freezes the view. The log tells us a runaway happened. */
                {
                    int scrubbed = physics_sanitize_state();
                    if (scrubbed > 0)
                        fprintf(stderr, "[physics] removed %d non-finite body(ies) "
                                        "after step\n", scrubbed);
                }
            }
        }

        if (!s_pause_menu_open && g_inspect_orbit_mode)
            inspect_orbit_update(dt);

        /* Refresh the cosmic density field (throttled; rebuilds on body-set
         * change). Queried by the HUD and, later, continuous LOD. */
        cosmic_field_tick(dt);

        /* Refresh emitter luminosities (throttled; they drift on the stellar
         * clock). Queried by render.c body lighting and the HUD. */
        radiance_field_tick(dt);

        /* Refresh the field graph's harvested edges (throttled; rebuilds on
         * body-set change). Queried by the Inspect panel's Relations view. */
        field_graph_tick(dt);

        /* Build matrices.
         * view_rot: rotation-only lookAt (origin as eye). Used for all distant
         *           geometry via vp_camrel = proj × view_rot.
         * view:     full lookAt with float eye. Only used for ring rendering
         *           (rings.c operates in float world space).                  */
        Mat4 proj, view, view_rot;

        float aspect = (float)WIN_W / (float)WIN_H;
        /* Far plane = the shared logarithmic-depth range (common.h). Log depth in
         * the fragment shaders preserves near precision across this huge range, so
         * geometry from planet surface to interstellar distance sorts in one pass
         * with no mode switch. The matrix far now governs only clip-plane culling. */
        mat4_perspective(proj, FOV, aspect, 0.0001f, RENDER_DEPTH_FAR);

        float fdx, fdy, fdz;
        cam_get_dir(&fdx, &fdy, &fdz);

        float up[3] = { 0.0f, 1.0f, 0.0f };
        float dir[3]  = { fdx, fdy, fdz };
        float zero3[3] = { 0.0f, 0.0f, 0.0f };
        mat4_lookAt(view_rot, zero3, dir, up);

        {
            float eye[3] = { (float)g_cam.pos[0], (float)g_cam.pos[1], (float)g_cam.pos[2] };
            float ctr[3] = { eye[0]+fdx,          eye[1]+fdy,          eye[2]+fdz          };
            mat4_lookAt(view, eye, ctr, up);
        }

        /* Bloom: render the scene into an HDR target, then composite the glow
         * to the screen. When disabled/unavailable these are no-ops and we draw
         * straight to the default framebuffer. */
        if (post_enabled()) {
            post_begin();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, WIN_W, WIN_H);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        render_frame(view, proj, view_rot, dt);

        /* Relativistic optics: derive an effective beta from the camera's actual
         * speed (position delta / dt). Warp velocities are >> c, so this is a
         * stylistic ramp across the warp range, not a literal v/c. Effect shows
         * only when actually moving fast; 0 below ~200 AU/s. */
        {
            static double rel_prev[3];
            static int    rel_have = 0;
            static float  rel_beta = 0.0f;   /* time-eased, not instantaneous   */
            static float  rel_cx   = 0.5f;   /* heading point in UV (eased)      */
            static float  rel_cy   = 0.5f;
            float target = 0.0f;
            float head_cx = 0.5f, head_cy = 0.5f;   /* this frame's raw heading  */
            if (rel_have && dt > 1e-4f) {
                double dx = g_cam.pos[0] - rel_prev[0];
                double dy = g_cam.pos[1] - rel_prev[1];
                double dz = g_cam.pos[2] - rel_prev[2];
                double len = sqrt(dx*dx + dy*dy + dz*dz);
                double sp  = len / dt;                          /* AU/s */
                float s = (float)((sp - 200.0) / (60000.0 - 200.0));
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
                s = s * s * (3.0f - 2.0f * s);                  /* smoothstep */
                target = (float)g_settings.relativistic * s;

                /* Heading: project the unit velocity vector to screen space via
                 * vp_camrel = proj × view_rot.  When motion aligns with the look
                 * axis this lands at the centre; strafing/off-axis travel offsets
                 * it, so the aberration + Doppler focus tracks where you're
                 * actually going rather than where you're pointing. */
                if (len > 1e-9) {
                    float vd[3] = { (float)(dx / len), (float)(dy / len),
                                    (float)(dz / len) };
                    Mat4 vp; mat4_mul(vp, proj, view_rot);
                    float sx, sy;
                    if (mat4_project(vp, vd[0], vd[1], vd[2],
                                     WIN_W, WIN_H, &sx, &sy)) {
                        float ox = sx / (float)WIN_W - 0.5f;
                        float oy = sy / (float)WIN_H - 0.5f;
                        /* Clamp the offset so a near-perpendicular velocity can't
                         * fling the focus into a corner (keeps the edge taper and
                         * texture sampling sane). */
                        float om = sqrtf(ox * ox + oy * oy);
                        const float lim = 0.28f;
                        if (om > lim) { ox *= lim / om; oy *= lim / om; }
                        head_cx = 0.5f + ox;
                        head_cy = 0.5f + oy;
                    }
                }
            }
            /* Ease beta in (warp "FOV" ramps up smoothly, tau ~0.5 s) but drop
             * out instantly when the target falls — so slowing/stopping snaps
             * the effect off rather than lingering. */
            if (target > rel_beta)
                rel_beta += (target - rel_beta) * (1.0f - expf(-dt / 0.5f));
            else
                rel_beta = target;
            /* Ease the heading both ways (tau ~0.25 s) so the focus glides when
             * the velocity direction changes instead of snapping. */
            {
                float k = 1.0f - expf(-dt / 0.25f);
                rel_cx += (head_cx - rel_cx) * k;
                rel_cy += (head_cy - rel_cy) * k;
            }
            rel_prev[0] = g_cam.pos[0];
            rel_prev[1] = g_cam.pos[1];
            rel_prev[2] = g_cam.pos[2];
            rel_have = 1;
            post_set_relativistic(rel_beta, rel_cx, rel_cy);
        }

        post_end();
        ui_render();

        /* Multiverse overlay (drawn last, on top). Returns a preset to switch
         * to, or -1, and may set load_path (e.g. a freshly imported real-data
         * catalog); law-slider edits set laws_changed. No-op without USE_IMGUI. */
        int laws_changed = 0;
        const char *load_path = NULL;
        int menu_pick = menu_render(preset_index_of_path(s_universe_path),
                                    &laws_changed, &load_path);

        /* Headless screenshot: capture the back buffer (this frame) then quit. */
        if (shot_path && ++frame_no >= shot_frames) {
            save_screenshot_ppm(shot_path);
            if (headless) {
                /* End-of-run field-graph stats: by now stellar time (if any)
                 * has run, so gas-flow edges and logged events are visible. */
                FieldGraphStats fs;
                field_graph_rebuild();
                field_graph_stats(&fs);
                fprintf(stdout,
                        "[FieldGraph] nodes=%d (stars=%d planets=%d holes=%d "
                        "nebulae=%d galaxies=%d) edges=%d (grav=%d flow=%d) events=%d\n",
                        fs.nodes, fs.stars, fs.planets, fs.black_holes,
                        fs.nebulae, fs.galaxies, fs.edges, fs.grav_edges, fs.flow_edges,
                        fs.events_logged);
            }
            running = 0;
        }

        /* Headless: SDL's offscreen driver makes SwapWindow a no-op, so
         * nothing ever syncs the GPU — at thousands of fps the driver's
         * command queue grows without bound until frames come back corrupted
         * (fully black, NaN-like). One glFinish per frame bounds the queue;
         * windowed mode doesn't need it (vsync/swap paces the pipeline). */
        if (headless)
            glFinish();

        SDL_GL_SwapWindow(s_win);

        if (laws_changed)
            physics_refresh_timestep_model();
        if (load_path) {
            switch_universe(load_path);
        } else if (menu_pick >= 0) {
            const UniversePreset *p = preset_at(menu_pick);
            if (p && strcmp(p->path, s_universe_path) != 0)
                switch_universe(p->path);
        }
    }

    app_quit();
    return 0;
}
