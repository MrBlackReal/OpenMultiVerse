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
#include "benchmark.h"
#include "comet.h"
#include "trails.h"
#include "orbit_predict.h"
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
#include "profiler.h"
#include "gpu_timer.h"
#include "dust_field.h"
#include "frame.h"
#include "freeze.h"

/* Active-system count, captured for profiler spike context only. */
static int s_prof_active_systems = 0;
#include "starsys.h"
#include "spectral.h"
#include "audio.h"
#include "presets.h"
#include "menu.h"
#include "post.h"
#include "cinematic.h"
#include "cinema_cam.h"
#include "cinema_tour.h"
#include "cinema_titles.h"
#include "cinema_blur.h"
#include "loading.h"
#ifdef _OPENMP
#include <omp.h>
#endif

/* ── active universe ──────────────────────────────────────────────────────── */
/* Path of the universe JSON currently loaded. Changed via switch_universe();
 * init_runtime_world() reads this so a reset reloads the chosen multiverse. */
static char s_universe_path[512] = "assets/universes/known_universe.json";

/* When set (via --export-body-catalog), init_runtime_world() writes the loaded
 * universe's bulk bodies to this BodyBin path and exits before warm-up/GL — the
 * offline half of the manifest+binary build. */
static char s_export_bodybin_path[512] = "";

/* ── window / context ─────────────────────────────────────────────────────── */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
int g_win_w = DEFAULT_WIN_W;
int g_win_h = DEFAULT_WIN_H;

/* Window size to create. Normally the defaults; cinematic film-out overrides it
 * so the render resolution is independent of any desktop window (docs/CINEMATIC.md
 * §6): WIN_W/WIN_H are what every pass — post.c's bloom chain, render.c's
 * volumetric slots — derives its own size from, so setting them once makes the
 * whole pipeline follow with no per-call-site viewport audit. */
/* --focus <body name>, applied after the universe loads (bodies must exist). */
static const char *s_cine_focus_body = NULL;
/* --shot-script PATH, likewise: its anchor/look_at names need a loaded world. */
static const char *s_cine_shot_script = NULL;
static const char *s_cine_shot_save   = NULL;
static int         s_cine_tour_only = 0, s_cine_director_only = 0;
static int         s_cine_dur_set     = 0;

static int s_init_w = DEFAULT_WIN_W;
static int s_init_h = DEFAULT_WIN_H;

/* Animation clock (see common.h). Wall-clock-driven in normal operation;
 * advanced by exactly 1/fps per frame during film-out. */
double g_render_time = 0.0;
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
int g_hud_hidden = 0;   /* toggled by H / --no-hud; hides HUD overlay + labels */

/* ── logging helpers ──────────────────────────────────────────────────────── */
static void boot_log(const char *msg) {
    fprintf(stdout, "[Boot] %s\n", msg);
    fflush(stdout);
}

static void clear_movement_keys(void) {
    s_key_w = s_key_s = s_key_a = s_key_d = s_key_q = s_key_e = 0;
}

/* Open/close the ImGui multiverse panel.  While it is open it owns every input
 * event (see the poll loop), so opening must release any state the game is
 * holding: the pending SDL_KEYUPs are swallowed by the menu and would other-
 * wise leave a key stuck down for as long as the panel stays up. */
static void set_menu_open(int open) {
    menu_set_visible(open);
    if (open) {
        s_freelook = 0;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        clear_movement_keys();
        build_set_tab_held(0);
    }
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
/* Previous-frame camera for the relativistic-shift speed estimate: a local
 * position kept across frames, so it moves with the frame (frame.h) -- or a
 * rebase would read as an 8 AU jump in one frame. */
static double rel_prev[3];
static void rel_prev_frame_shift(const double d_m[3])
{
    for (int k = 0; k < 3; k++) rel_prev[k] -= d_m[k] / AU;
}

/* Camera position in world metres (g_cam.pos is in AU). */
static void camera_world_m(double out[3]) {
    out[0] = g_cam.pos[0] * AU;
    out[1] = g_cam.pos[1] * AU;
    out[2] = g_cam.pos[2] * AU;
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
    /* Measured dust around the Sun: needed before the load, which turns
     * catalog magnitudes intrinsic against it (dust_field.h). */
    if (!dust_field_loaded()) dust_field_load("assets/catalogs/dust_local.bin");
    universe_load(s_universe_path);   /* drives its own determinate progress */

    /* Offline export: dump the freshly-loaded (pre-warm-up) bulk bodies to a
     * BodyBin and quit. Runs here — after Keplerian→state/CoM derivation but
     * before warm-up and the GL passes — so the binary reloads identically to a
     * fresh JSON load (warm-up then re-runs on reload, same as before). */
    if (s_export_bodybin_path[0]) {
        int n = universe_export_body_catalog(s_export_bodybin_path);
        exit(n >= 0 ? 0 : 1);
    }

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
    galaxy_spawn_agn();   /* embed AGN nuclei in host galaxies (after positions) */
    boot_log("Initializing comets");
    comet_init();
    loading_phase("Allocating trails");
    boot_log("Initializing trails");
    trails_gl_init();
    orbit_predict_init();
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
    gpu_timer_shutdown();
    render_shutdown();
    labels_shutdown();
    orbit_predict_shutdown();
    trails_gl_shutdown();
    comet_shutdown();
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
    starsys_reset();       /* promoted bodies were wiped with the world */
    freeze_reset();        /* records point at the old world's bodies */
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
                             s_init_w, s_init_h,
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
    /* True black: space is not navy. Any blue floor here gets lifted by
     * auto-exposure and reads as a washed-out void. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    update_viewport_size();
    boot_log("OpenGL context ready");

    boot_log("Initializing audio");
    audio_init();

    boot_log("Initializing universe menu");
    menu_init(s_win, s_ctx);

    return 1;
}

static void app_quit(void) {
    /* Before GL goes away: flush the readback ring and close the encoder pipe,
     * so an interrupted film still produces a playable file. */
    cinema_tour_shutdown();
    cinematic_shutdown();
    /* Persist only if something changed this session — and never from a
     * film-out run: its CLI look overrides and the film-only quality forcings
     * are properties of that one render, not preferences to remember. */
    /* A shot playing or previewing has the snapshot of your settings; put
     * it back first, so the shot's fov/aperture/... are not saved as yours. */
    cinema_shot_end();
    if (settings_dirty() && !cinematic_filming())
        settings_save();
    audio_shutdown();
    if (profiler_enabled()) { profiler_dump_stdout(); gpu_timer_dump(); }
    benchmark_shutdown();
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
        case SDLK_k:
            /* Drop a cinematic keyframe at the current camera pose, or remove
             * the last one with Shift. Authoring happens in the NORMAL app —
             * you fly to a framing you like and pin it (docs/CINEMATIC.md §9.3).
             * Keyframe drop rather than recording the raw flight path: a path
             * is jittery, enormous and uneditable; a spline through a dozen
             * deliberate poses is clean, small and hand-editable after. */
            if (!e->key.repeat) {
                if (e->key.keysym.mod & KMOD_SHIFT) cinema_shot_remove_last();
                else                                cinema_shot_add_key_here(5.0);
            }
            break;
        case SDLK_h:
            /* Toggle the 2D HUD overlay + body labels (declutter / clean shots). */
            if (!e->key.repeat) g_hud_hidden = !g_hud_hidden;
            break;
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
            if (!e->key.repeat)
                set_menu_open(!menu_visible());
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
        /* Nearest body from the shared per-frame proximity cache (at most one
         * frame stale — fine for a speed governor). */
        double best_au = g_cam_prox.body >= 0 ? g_cam_prox.body_dist_au : -1.0;

        /* Galaxies are anchors too, or arriving at one would never slow
         * down (their stars aren't bodies): distance to the volume's edge,
         * floored inside so intra-galaxy travel stays brisk (~2% of the
         * radius ≈ 1 kly for the Milky Way → ~150 ly/s). */
        double cam_sun[3];
        frame_cam_sun(cam_sun);                    /* galaxies are Sun frame */
        for (int i = 0; i < galaxy_count(); i++) {
            double gp[3], gr = galaxy_radius_au(i);
            galaxy_position(i, gp);
            double dx = gp[0] - cam_sun[0];
            double dy = gp[1] - cam_sun[1];
            double dz = gp[2] - cam_sun[2];
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

/* ── per-frame active-system integration ──────────────────────────────────────
 *
 * The RESPA integration of one star system touches only that system's own
 * members (under gravity_isolation), exactly like the OpenMP-parallel warm-up
 * loop — so independent systems can integrate concurrently.  The obstacle is
 * the collision machinery interleaved in the outer-step loop:
 * collision_snapshot_positions() is a GLOBAL write and collision_step_system()
 * can add/remove bodies, neither of which is safe under threads.
 *
 * We therefore split the active set: a system is "hot" if the read-only
 * encounter predicate says a collision / close approach is possible this frame
 * (these are inherently the low-index <MAX_BODIES systems the collision code can
 * even touch).  Cold systems integrate in parallel; hot systems run serially
 * with the exact original collision semantics.  When gravity_isolation is off
 * (deliberately-coupled clusters) cross-system force reads would race concurrent
 * position writes, so everything falls back to serial.
 */

/* Compute one system's RESPA step schedule for a frame of `sys_dt` seconds. */
static void system_step_schedule(int s, double sys_dt, int *outer_steps,
                                 double *dt_outer, int *n_inner, double *dt_inner)
{
    int    root         = physics_system_root(s);
    double dt_outer_max = physics_system_outer_dt_limit(s);
    double dt_inner_max = physics_system_inner_dt_limit(s);

    int    osteps = (int)(sys_dt / dt_outer_max) + 1;
    double douter = sys_dt / osteps;
    /* Subdivide further through a detected close approach. */
    int    ca = collision_system_close_approach_subdivide(root, douter);
    osteps *= ca;
    douter /= ca;
    int    ninner = (int)(douter / dt_inner_max) + 1;

    *outer_steps = osteps;
    *dt_outer    = douter;
    *n_inner     = ninner;
    *dt_inner    = douter / ninner;
}

/* Cold path: independent integration only — safe to run from a worker thread.
 * No encounter branch fires (the system was classified as collision-free). */
static void integrate_system_cold(int s, double sys_dt)
{
    int root = physics_system_root(s);
    int outer_steps, n_inner;
    double dt_outer, dt_inner;

    trails_begin_frame_snapshot_system(root);
    system_step_schedule(s, sys_dt, &outer_steps, &dt_outer, &n_inner, &dt_inner);

    for (int o = 0; o < outer_steps; o++) {
        /* After the first outer step the previous end() left acc[] valid at
         * these exact positions (nothing between moves the bodies on the cold
         * path), so skip the redundant slow-force recompute. */
        physics_respa_begin_system_ex(root, dt_outer, o == 0);
        for (int i = 0; i < n_inner; i++)
            physics_respa_inner_system(root, dt_inner);
        physics_respa_end_system(root, dt_outer);
        rings_step_system(root, dt_outer);
        trails_tick_system(root, dt_outer);
    }
}

/* Hot path: the exact original per-system loop, including the per-outer-step
 * encounter re-snapshot and collision resolution.  Runs serially. */
static void integrate_system_hot(int s, double sys_dt)
{
    int root = physics_system_root(s);
    int outer_steps, n_inner;
    double dt_outer, dt_inner;

    trails_begin_frame_snapshot_system(root);
    system_step_schedule(s, sys_dt, &outer_steps, &dt_outer, &n_inner, &dt_inner);

    for (int o = 0; o < outer_steps; o++) {
        /* Re-snapshot at encounter onset for sub-frame rollback.  Scoped to
         * this system's trails; collision_snapshot_positions stays global
         * (bounded to MAX_BODIES) — safe because hot systems run serially. */
        int local_encounter = collision_system_maybe_has_encounter(root, dt_outer);
        if (local_encounter) {
            trails_begin_frame_snapshot_system(root);
            collision_snapshot_positions();
        }
        physics_respa_begin_system(root, dt_outer);
        for (int i = 0; i < n_inner; i++)
            physics_respa_inner_system(root, dt_inner);
        physics_respa_end_system(root, dt_outer);
        rings_step_system(root, dt_outer);
        trails_tick_system(root, dt_outer);
        if (local_encounter) {
            double t0 = profiler_enabled() ? profiler_now_ms() : 0.0;
            collision_step_system(root, dt_outer);
            if (profiler_enabled())
                profiler_record_collision_time(profiler_now_ms() - t0);
        }
    }
}

/* Advance the simulation by `dt` seconds (RESPA hierarchical integrator).
 *
 * Lifted out of the main loop because cinematic motion blur calls it once per
 * accumulation sub-frame instead of once per frame, sampling the sim at several
 * instants across the open shutter. Everything it touches is global state, so
 * `dt` is the only thing that crosses the boundary.
 *
 * Note this makes motion blur N times the SIMULATION cost, not just N times the
 * render cost: at high sample counts and large timescales the sim, not the
 * renderer, becomes the bottleneck (docs/CINEMATIC.md §15). --shutter 0 opts out.
 */
static void advance_simulation(float dt)
{
    /* Physics — RESPA hierarchical integrator */
    if (!g_paused && g_sim_speed > 0.0) {
        /* Per-frame outer-step budget. The cap exists so a slow frame or a
         * huge sim speed cannot make one frame take unbounded time — a REALTIME
         * concern, and 120 is right when a frame must land in ~16ms.
         *
         * Film-out has no such concern: frame time is irrelevant offline, and
         * the cap silently truncates the per-frame sim advance that
         * docs/CINEMATIC.md §5 promises is exact. Left at 120, a film asking for a
         * stellar-evolution timescale runs thousands of times slower than
         * requested, and every timescale past the cap renders identically.
         * Integrator accuracy is bounded by dt_outer, not by the number of
         * steps, so raising the ceiling costs wall time and nothing else. */
        const int MAX_OUTER_STEPS = cinematic_filming() ? 200000 : 120;
        physics_refresh_timestep_model_if_needed(dt);
        {
            int sys_n = physics_system_count();
            /* g_laws.time_scale lets a universe run its clock faster/slower
             * than real-world speed presets; the per-system caps below still
             * bound each outer step, preserving integrator stability. */
            double sim_dt = g_sim_speed * dt * g_laws.time_scale;
            double effective_sim_dt = sim_dt;

            double cam_m[3]; camera_world_m(cam_m);

            /* Collect the ACTIVE (in-radius) systems once — a root star
             * cannot cross the multi-ly active boundary within one frame,
             * so the timestep-cap loop and the integration loop below both
             * reuse this list instead of re-testing every system twice.
             * Backed by the movement-gated near-system cache, so the
             * O(system_count) scan runs only when the camera moves enough or
             * the body set changes — not every frame (galaxy-scale critical). */
            static int *s_active_sys = NULL;
            static int  s_active_sys_cap = 0;
            if (sys_n > s_active_sys_cap) {
                int cap = s_active_sys_cap ? s_active_sys_cap : 64;
                while (cap < sys_n) cap *= 2;
                s_active_sys = realloc(s_active_sys, (size_t)cap * sizeof(int));
                if (!s_active_sys) { fprintf(stderr, "[main] active-sys alloc failed\n"); exit(1); }
                s_active_sys_cap = cap;
            }
            const int *act_slots = NULL;
            int n_active = physics_active_systems(cam_m, ACTIVE_RADIUS_LY * LY,
                                                  &act_slots);
            s_prof_active_systems = n_active;
            for (int a = 0; a < n_active; a++)
                s_active_sys[a] = act_slots[a];

            /* Staleness-driven timestep refresh, active systems only.
             * Frozen systems' orbits cannot drift, so the full O(N)
             * rebuild above runs only on body-set changes. */
            physics_refresh_active_timesteps(s_active_sys, n_active, dt);

            /* Find the most constrained ACTIVE system and cap to it.  A
             * frozen distant system must not drag down the timestep (and
             * thus the frame rate) of the system you are actually in. */
            for (int a = 0; a < n_active; a++) {
                int s = s_active_sys[a];
                double dt_outer_max = physics_system_outer_dt_limit(s);
                double sys_cap = dt_outer_max * MAX_OUTER_STEPS;
                if (effective_sim_dt > sys_cap)
                    effective_sim_dt = sys_cap;
            }

            /* When the budget still binds, the film is not running at the
             * timescale that was asked for. Say so once, rather than silently
             * producing a slower film than the shot specifies. */
            if (cinematic_filming() && effective_sim_dt < sim_dt * 0.999) {
                static int warned = 0;
                if (!warned) {
                    warned = 1;
                    fprintf(stderr,
                        "[Cinematic] timescale capped: asked %.4g sim-seconds per "
                        "frame, integrator allows %.4g (%.3gx). The film runs "
                        "slower than the requested timescale.\n",
                        sim_dt, effective_sim_dt, effective_sim_dt / sim_dt);
                }
            }

            /* Trail snapshots are per-system (only integrated bodies can
             * move within a frame), so frozen far systems skip the ~200-
             * byte-per-body rewrite — at 16k bodies that full sweep was a
             * dominant per-frame cost. collision_snapshot_positions stays
             * global: it is bounded to MAX_BODIES and collision_step's
             * broad pass reads it after this loop. */

            collision_snapshot_positions();

            /* Parallelism is sound only when systems are gravitationally
             * isolated (the default); with coupling on, one system's slow
             * force reads another's positions while they are being written. */
            int parallel_ok = (g_laws.gravity_isolation != 0.0);

            /* Classify each active system hot (collision/close-approach
             * possible → serial) or cold (independent → parallel).  Both
             * predicates are read-only, so this pass is thread-safe and
             * cheap; it uses the full-frame dt (>= any sub-step) so a cold
             * verdict is conservative — no encounter can appear mid-frame. */
            static unsigned char *s_hot = NULL;
            static int s_hot_cap = 0;
            if (n_active > s_hot_cap) {
                int cap = s_hot_cap ? s_hot_cap : 64;
                while (cap < n_active) cap *= 2;
                s_hot = realloc(s_hot, (size_t)cap);
                if (!s_hot) { fprintf(stderr, "[main] hot-flag alloc failed\n"); exit(1); }
                s_hot_cap = cap;
            }
            /* This classifier calls the collision broadphase predicates
             * once per active system, so it is collision cost and belongs
             * in that stage — it sits outside collision_step(), which is
             * exactly how it stayed invisible. */
            profiler_stage_begin(PROFILER_STAGE_COLLISION);
            int n_cold = 0;
            for (int a = 0; a < n_active; a++) {
                int s = s_active_sys[a];
                int hot = !parallel_ok;
                if (parallel_ok) {
                    int    root = physics_system_root(s);
                    double dt_outer_max = physics_system_outer_dt_limit(s);
                    int    osteps = (int)(effective_sim_dt / dt_outer_max) + 1;
                    double first_outer = effective_sim_dt / osteps;
                    if (collision_system_maybe_has_encounter(root, effective_sim_dt) ||
                        collision_system_close_approach_subdivide(root, first_outer) > 1)
                        hot = 1;
                }
                s_hot[a] = (unsigned char)hot;
                n_cold += !hot;
            }
            profiler_stage_end(PROFILER_STAGE_COLLISION);

            /* Cold systems integrate concurrently (mirrors the warm-up loop).
             * Below the threshold the per-frame fork/join cost outweighs the
             * gain, so OpenMP's if-clause runs the loop serially in-thread. */
            profiler_stage_begin(PROFILER_STAGE_PHYSICS);
            #define PARALLEL_MIN_COLD_SYSTEMS 8
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) if(n_cold >= PARALLEL_MIN_COLD_SYSTEMS)
#endif
            for (int a = 0; a < n_active; a++) {
                if (s_hot[a]) continue;
                integrate_system_cold(s_active_sys[a], effective_sim_dt);
            }

            /* Hot systems (and the whole coupled-cluster case) run serially
             * with the original collision-rollback semantics preserved. */
            for (int a = 0; a < n_active; a++) {
                if (!s_hot[a]) continue;
                integrate_system_hot(s_active_sys[a], effective_sim_dt);
            }
            physics_advance_time(effective_sim_dt);
            profiler_stage_end(PROFILER_STAGE_PHYSICS);

            profiler_stage_begin(PROFILER_STAGE_COLLISION);
            supernova_step(effective_sim_dt);
            collision_step(effective_sim_dt);
            profiler_stage_end(PROFILER_STAGE_COLLISION);
            profiler_stage_begin(PROFILER_STAGE_PHYSICS);
            asteroids_step(effective_sim_dt);
            rings_tick(effective_sim_dt);
            profiler_stage_end(PROFILER_STAGE_PHYSICS);
            /* Stellar evolution runs on its own clock (years/real-second),
             * decoupled from the capped orbital dt — so a star can age and
             * die without ever speeding up the integrator. dt is the real
             * frame time; no-op unless auto-aging is enabled. */
            profiler_stage_begin(PROFILER_STAGE_LIFECYCLE);
            lifecycle_step(dt);
            /* Black-hole accretion runs on the same stellar clock: quasars
             * drain their gas reservoir → Ṁ → Eddington ratio (activity) and
             * grow, so they visibly fade over cosmic time. No-op at rate 0. */
            accretion_step(dt);
            profiler_stage_end(PROFILER_STAGE_LIFECYCLE);
            /* Safety net: if a step produced a non-finite body (NaN/inf),
             * remove it before it corrupts the camera-relative render math
             * and freezes the view. The log tells us a runaway happened.
             * Per-frame the sweep covers only the active systems (frozen
             * bodies are never integrated, so they cannot go non-finite);
             * a slow full sweep backstops any unaudited mutation path. */
            {
                static double s_sanitize_full_t = 0.0;
                int scrubbed = 0;
                for (int a = 0; a < n_active; a++)
                    scrubbed += physics_sanitize_system(
                                    physics_system_root(s_active_sys[a]));
                s_sanitize_full_t += dt;
                if (s_sanitize_full_t >= 1.0) {
                    s_sanitize_full_t = 0.0;
                    scrubbed += physics_sanitize_state();
                }
                if (scrubbed > 0)
                    fprintf(stderr, "[physics] removed %d non-finite body(ies) "
                                    "after step\n", scrubbed);
            }
        }
    }
}

/* Print the full CLI reference (for --help / unknown options). */
static void print_usage(const char *prog)
{
    printf(
"OpenMultiVerse — real-time scale-continuous N-body universe simulator.\n"
"\n"
"Usage: %s [options]\n"
"  With no options, opens a window and loads assets/universe.json.\n"
"\n"
"Universe:\n"
"  --preset PATH           Load this universe JSON instead of the default.\n"
"\n"
"Headless / screenshots:\n"
"  --headless              Run offscreen (EGL surfaceless) with no window.\n"
"  --shot PATH             Render --frames frames, write PATH (a PPM), then quit.\n"
"  --frames N              Frames to simulate before the --shot (default 1).\n"
"  --no-hud                Hide the 2D HUD + body labels (clean shots; = key H).\n"
"  --cam X,Y,Z,YAW,PITCH   Place the camera: position in AU, yaw/pitch in degrees.\n"
"  --fov DEG               Field of view in degrees (telephoto compression).\n"
"  --exposure V            Fix exposure V (disables auto-exposure).\n"
"  --timescale V           Override the universe time_scale (0 freezes the sim).\n"
"  --stellar-rate YRS      Years of stellar evolution per real second (aging,\n"
"                          supernovae, accretion) — testable headless.\n"
"\n"
"Cinematic (see docs/CINEMATIC.md):\n"
"  --cinematic             Use the cinematic renderer (accumulation-sampled).\n"
"                          On its own this only swaps the renderer: the window,\n"
"                          free-look camera and menu behave exactly as before.\n"
"  --output PATH           Film out to PATH instead (implies --headless): fixed\n"
"                          timestep, deterministic, piped to ffmpeg. Container\n"
"                          comes from the extension (.mp4/.mov/.mkv/.webm).\n"
"  --res WxH               Render resolution, independent of the window\n"
"                          (default 1920x1080).\n"
"  --fps N                 Output frame rate (default 60).\n"
"  --duration S            Seconds of film (default 30).\n"
"  --samples N             Accumulation sub-frames per output frame\n"
"                          (default 32 filming, 4 live). 1 disables it.\n"
"  --crf N                 Encoder quality, lower is better (default 16).\n"
"  --film                  Preset: 3840x2160 @ 24fps, 64 samples.\n"
"  --draft                 Preset: 1280x720 @ 30fps, 4 samples (fast preview).\n"
"  --shutter DEG           Shutter angle, 0-360 (default 180). Drives motion\n"
"                          blur length; 0 disables it.\n"
"  --aperture F            f-number (default 0 = depth of field off). Lower is\n"
"                          shallower; 2.8 is a strong, obvious look.\n"
"  --shot-script PATH      Play a keyframed camera shot (assets/shots/*.json).\n"
"                          Its length sets --duration unless you pass one.\n"
"  --shot-save PATH        Load --shot-script, rewrite it canonicalised to PATH,\n"
"                          and exit (validates a hand-written shot). Without\n"
"                          --shot-script, saves the generated --duration tour.\n"
"  --tour                  Procedural tour only, no event cutaways.\n"
"  --director              Event cutaways only, no procedural spine.\n"
"                          With neither, filming without --shot-script builds a\n"
"                          tour and lets the director interrupt it.\n"
"  --focus NAME|AU|auto    Focus on a named body (racks focus as it moves), a\n"
"                          literal distance in AU, or the nearest body (auto).\n"
"  --grain V               Film grain amount (0 = off).\n"
"  --relativistic V        Warp aberration/Doppler strength, 0-1 (0 = off).\n"
"                          Saturates on fast pull-backs; 0 for film work.\n"
"  --letterbox [AR]        Crop to aspect AR with matte bars (bare = 2.39).\n"
"  --audio PATH            Soundtrack to mux (default assets/soundtrack.ogg).\n"
"  --no-audio              Film silent.\n"
"  --cinematic-info        Title cards (name, distance, date, scale bar) and a\n"
"                          camera-speed readout. Default output is clean.\n"
"  --no-orbits             No orbit trails or orbit prediction: not drawn and\n"
"                          not computed (skips the per-step trail work).\n"
"\n"
"Benchmark / tools:\n"
"  --profile               Per-stage frame profiler; prints a report on exit.\n"
"  --selftest-frame        Floating-origin precision test at M87 distance; exit.\n"
"  --selftest-starsys      Run the procedural-system delta round-trip test; exit.\n"
"  --frame-offset X,Y,Z    Displace the floating origin by X,Y,Z AU (test: the\n"
"                          picture must not change beyond rounding).\n"
"  --profile-gpu           --profile plus per-pass GPU times (timer queries).\n"
"                          Serialises CPU and GPU, so fps under it is not real.\n"
"  --benchmark             Scripted galaxy flythrough; prints an FPS report.\n"
"  --benchmark-ab          Also fly a galaxies-OFF pass to price the galaxy\n"
"                          layer (doubles the run time).\n"
"  --benchmark-shots DIR   Also write one PPM per marked stage into DIR\n"
"                          (solar system, supernova, galaxies, quasars, ...).\n"
"  --export-body-catalog PATH\n"
"                          Load --preset, write its bulk (non-curated) bodies to a\n"
"                          BodyBin at PATH, and exit (offline manifest+binary build).\n"
"\n"
"  -h, --help              Show this help and exit.\n"
"\n"
"Data tooling lives in tools/ (fetch_catalogs.py, build_known_universe.py) and\n"
"catalogtool; see docs/ARCHITECTURE.md and CLAUDE.md for the full reference.\n",
        prog);
}

int main(int argc, char **argv) {
    /* ---- headless / screenshot CLI ----------------------------------------
     * --headless           run with SDL's offscreen (EGL surfaceless) driver,
     *                      no window on the desktop.
     * --shot PATH          render --frames frames, dump PATH (PPM), then quit.
     * --frames N           frames to render before the shot (default 6).
     * --preset PATH        load this universe JSON instead of the default.
     * --cam x,y,z,yaw,pitch position the camera (AU, degrees) after load.    */
    const char *shot_path      = NULL;
    const char *bench_shot_dir = NULL;
    int         bench_ab       = 0;
    int         shot_frames    = 6;
    int         headless    = 0;
    int         run_bench   = 0;
    int         cli_res_set = 0;
    int         cam_set     = 0;
    double      cam_pos[3]  = { 0.0, 0.0, 0.0 };
    float       cam_yaw = 0.0f, cam_pitch = 0.0f;
    float       cli_fov = -1.0f, cli_exposure = -1.0f;   /* <0 = leave default */
    int         cli_ts_set = 0;    double cli_ts = 0.0;   /* --timescale override */
    int         cine_film = 0, cine_draft = 0;  /* presets, applied before flags */
    float       cli_shutter = -1.0f, cli_aperture = -1.0f;   /* <0 = untouched */
    float       cli_grain = -1.0f, cli_letterbox = -1.0f, cli_rel = -1.0f;
    const char *cli_focus = NULL;
    const char *cli_shot_script = NULL;
    const char *cli_shot_save   = NULL;
    int         cli_selftest_starsys = 0;
    int         cli_selftest_frame = 0;
    int         cli_frame_offset_set = 0;
    double      cli_frame_offset[3] = { 0.0, 0.0, 0.0 };
    int         cli_tour_only = 0, cli_director_only = 0;
    int         cine_dur_set = 0;   /* --duration given explicitly? */

    /* Cinematic config is populated before the parse so individual flags can
     * override the presets regardless of the order they appear in. */
    cinematic_defaults();

    for (int a = 1; a < argc; a++) {
        if      (!strcmp(argv[a], "--help") || !strcmp(argv[a], "-h")) {
            print_usage(argv[0]);
            return 0;
        }
        else if (!strcmp(argv[a], "--headless")) headless = 1;
        else if (!strcmp(argv[a], "--shot")    && a + 1 < argc) shot_path   = argv[++a];
        else if (!strcmp(argv[a], "--frames")  && a + 1 < argc) shot_frames = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--preset")  && a + 1 < argc)
            snprintf(s_universe_path, sizeof s_universe_path, "%s", argv[++a]);
        /* --export-body-catalog PATH  load --preset, write its bulk bodies to a
         * BodyBin at PATH, and exit (offline manifest+binary build step). */
        else if (!strcmp(argv[a], "--export-body-catalog") && a + 1 < argc) {
            snprintf(s_export_bodybin_path, sizeof s_export_bodybin_path, "%s", argv[++a]);
            headless = 1;   /* offscreen: export exits before any GL pass */
        }
        else if (!strcmp(argv[a], "--cam")     && a + 1 < argc)
            cam_set = (sscanf(argv[++a], "%lf,%lf,%lf,%f,%f",
                              &cam_pos[0], &cam_pos[1], &cam_pos[2],
                              &cam_yaw, &cam_pitch) == 5);
        /* Stellar-clock rate (years of stellar evolution per real second) —
         * normally a menu slider; exposed as a flag so lifecycle-driven events
         * (aging, supernovae, accretion fading) are testable headless. */
        else if (!strcmp(argv[a], "--stellar-rate") && a + 1 < argc)
            g_stellar_years_per_sec = atof(argv[++a]);
        /* Scripted cinematic flythrough of the Milky Way and its neighbour
         * galaxies, timing each leg and printing an FPS report on completion. */
        else if (!strcmp(argv[a], "--benchmark")) run_bench = 1;
        /* Per-stage frame profiler; report on exit. */
        else if (!strcmp(argv[a], "--profile")) profiler_set_enabled(1);
        else if (!strcmp(argv[a], "--profile-gpu")) {
            profiler_set_enabled(1);
            gpu_timer_set_enabled(1);
        }
        /* Add the galaxies-OFF pass that prices the galaxy layer (doubles the
         * run time; the per-stage fps numbers do not need it). */
        else if (!strcmp(argv[a], "--benchmark-ab")) { run_bench = 1; bench_ab = 1; }
        /* Per-stage screenshots from the benchmark tour, into an existing
         * directory: one PPM per marked stage, galaxies-ON pass only. */
        else if (!strcmp(argv[a], "--benchmark-shots") && a + 1 < argc)
            bench_shot_dir = argv[++a];
        /* Hide the 2D HUD overlay + body labels (clean cinematic screenshots);
         * same effect as pressing H in-app. */
        else if (!strcmp(argv[a], "--no-hud")) g_hud_hidden = 1;
        /* Cinematic framing knobs for shots: narrow the field of view for
         * telephoto compression, and fix the exposure (disabling auto-exposure)
         * so bright star fields don't wash out the subject. */
        else if (!strcmp(argv[a], "--fov")      && a + 1 < argc) cli_fov = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--exposure") && a + 1 < argc) cli_exposure = (float)atof(argv[++a]);
        /* Override the universe's time_scale — pass 0 to freeze the sim so a body
         * stays exactly where it was queried, keeping close-range shot framing
         * reproducible (warmup still runs, so orbits are settled first). */
        else if (!strcmp(argv[a], "--timescale") && a + 1 < argc) { cli_ts = atof(argv[++a]); cli_ts_set = 1; }
        /* ---- cinematic renderer / film-out (docs/CINEMATIC.md §3) -------------- */
        else if (!strcmp(argv[a], "--cinematic")) g_cine.enabled = 1;
        else if (!strcmp(argv[a], "--output") && a + 1 < argc) {
            snprintf(g_cine.output, sizeof g_cine.output, "%s", argv[++a]);
            g_cine.filming = 1;
        }
        else if (!strcmp(argv[a], "--film"))  cine_film  = 1;
        else if (!strcmp(argv[a], "--draft")) cine_draft = 1;
        else if (!strcmp(argv[a], "--res") && a + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++a], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_cine.width = w; g_cine.height = h;
                cli_res_set = 1;
            } else {
                fprintf(stderr, "%s: --res expects WxH (e.g. 1920x1080)\n", argv[0]);
                return 2;
            }
        }
        else if (!strcmp(argv[a], "--fps")      && a + 1 < argc) g_cine.fps      = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--duration") && a + 1 < argc) { g_cine.duration = atof(argv[++a]); cine_dur_set = 1; }
        else if (!strcmp(argv[a], "--shot-script") && a + 1 < argc) cli_shot_script = argv[++a];
        /* Tour / director isolation. With neither, film-out and no shot script
         * means both: the tour is the spine and the director interrupts it. */
        else if (!strcmp(argv[a], "--tour"))     cli_tour_only = 1;
        else if (!strcmp(argv[a], "--director")) cli_director_only = 1;
        /* Load --shot-script, write it back out canonicalised, and exit: a
         * validator for hand-written shots, and what makes the format's
         * round-trip testable without the GUI. */
        else if (!strcmp(argv[a], "--frame-offset") && a + 1 < argc) {
            cli_frame_offset_set = sscanf(argv[++a], "%lf,%lf,%lf", &cli_frame_offset[0],
                                          &cli_frame_offset[1], &cli_frame_offset[2]) == 3;
        }
        else if (!strcmp(argv[a], "--selftest-frame")) {
            cli_selftest_frame = 1;
            headless = 1;
        }
        else if (!strcmp(argv[a], "--selftest-starsys")) {
            cli_selftest_starsys = 1;
            headless = 1;
        }
        else if (!strcmp(argv[a], "--shot-save") && a + 1 < argc) {
            cli_shot_save = argv[++a];
            headless = 1;
        }
        else if (!strcmp(argv[a], "--samples")  && a + 1 < argc) g_cine.samples  = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--crf")      && a + 1 < argc) g_cine.crf      = atoi(argv[++a]);
        /* Look parameters land in g_settings (they persist and the ImGui Look
         * panel edits the same fields), exactly as --fov/--exposure do. */
        else if (!strcmp(argv[a], "--shutter")  && a + 1 < argc) cli_shutter  = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--aperture") && a + 1 < argc) cli_aperture = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--grain")    && a + 1 < argc) cli_grain    = (float)atof(argv[++a]);
        /* Warp aberration/Doppler is a flourish for flying the app by hand; on
         * a fast cinematic pull-back it saturates and whites the frame out, so
         * it needs to be dialled down or off for film work. */
        else if (!strcmp(argv[a], "--relativistic") && a + 1 < argc) cli_rel = (float)atof(argv[++a]);
        /* --focus takes a body name ("Earth") or a literal distance in AU. */
        else if (!strcmp(argv[a], "--focus") && a + 1 < argc) {
            cli_focus = argv[++a];
        }
        /* --letterbox takes an optional aspect; bare, it means scope (2.39). */
        else if (!strcmp(argv[a], "--letterbox")) {
            cli_letterbox = 2.39f;
            if (a + 1 < argc && argv[a + 1][0] != '-') {
                double ar = atof(argv[a + 1]);
                if (ar > 0.0) { cli_letterbox = (float)ar; a++; }
            }
        }
        else if (!strcmp(argv[a], "--no-audio")) g_cine.audio = 0;
        else if (!strcmp(argv[a], "--cinematic-info")) cinema_titles_set_enabled(1);
        else if (!strcmp(argv[a], "--no-orbits")) g_orbits_off = 1;
        else if (!strcmp(argv[a], "--audio") && a + 1 < argc)
            snprintf(g_cine.audio_path, sizeof g_cine.audio_path, "%s", argv[++a]);
        else {
            fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], argv[a]);
            print_usage(argv[0]);
            return 2;
        }
    }
    if (shot_frames < 1) shot_frames = 1;

    /* ---- cinematic setup (before app_init: it fixes the window size) ------ */
    if (cine_film && cine_draft) {
        fprintf(stderr, "%s: --film and --draft are mutually exclusive\n", argv[0]);
        return 2;
    }
    if (g_cine.filming && !g_cine.enabled) {
        fprintf(stderr, "%s: --output requires --cinematic "
                        "(single stills are what --shot is for)\n", argv[0]);
        return 2;
    }
    if (cine_film || cine_draft) {
        /* Presets are re-applied here, after the parse, but must not clobber an
         * explicit flag — so re-run the whole argv for the fields they set. */
        int had_res = 0, had_fps = 0, had_samples = 0;
        for (int a = 1; a < argc; a++) {
            if (!strcmp(argv[a], "--res"))     had_res     = 1;
            if (!strcmp(argv[a], "--fps"))     had_fps     = 1;
            if (!strcmp(argv[a], "--samples")) had_samples = 1;
        }
        int w = g_cine.width, h = g_cine.height, f = g_cine.fps, n = g_cine.samples;
        if (cine_film) cinematic_preset_film(); else cinematic_preset_draft();
        if (had_res)     { g_cine.width = w; g_cine.height = h; }
        if (had_fps)     { g_cine.fps = f; }
        if (had_samples) { g_cine.samples = n; }
    }
    if (g_cine.filming) {
        /* H.264 in yuv420p subsamples chroma 2x2, so both dimensions must be
         * even. An odd --res makes ffmpeg fail to open the encoder and write a
         * zero-byte file while the render itself reports success — round here
         * instead, and say so. */
        if ((g_cine.width & 1) || (g_cine.height & 1)) {
            int w = g_cine.width  & ~1, h = g_cine.height & ~1;
            fprintf(stderr, "[Cinematic] %dx%d has an odd dimension; "
                            "rounding to %dx%d (H.264 needs even sizes)\n",
                    g_cine.width, g_cine.height, w, h);
            g_cine.width = w; g_cine.height = h;
        }
        if (g_cine.width < 2 || g_cine.height < 2) {
            fprintf(stderr, "%s: --res is too small\n", argv[0]); return 2;
        }
        if (g_cine.fps < 1)       { fprintf(stderr, "%s: --fps must be >= 1\n", argv[0]); return 2; }
        if (g_cine.duration <= 0) { fprintf(stderr, "%s: --duration must be > 0\n", argv[0]); return 2; }
        if (g_cine.samples < 0)   g_cine.samples = 0;
        /* Film-out is an offscreen render: no window, no vsync pacing, and the
         * render resolution is whatever was asked for rather than a desktop
         * window's size. */
        headless   = 1;
        s_init_w   = g_cine.width;
        s_init_h   = g_cine.height;
        g_hud_hidden = 1;        /* clean frames; --cinematic-info adds titles */
    } else if (headless && cli_res_set) {
        /* Headless stills (--shot, --benchmark-shots) capture the back buffer,
         * so --res sizes the offscreen window they are read from. */
        s_init_w = g_cine.width;
        s_init_h = g_cine.height;
    }

    if (headless) {
        /* SDL_setenv, not POSIX setenv: the latter needs a feature-test macro
         * (_DEFAULT_SOURCE/_GNU_SOURCE) that -std=c99 does not set, and which
         * only some distributions' sdl2-config happens to add to CFLAGS. */
        SDL_setenv("SDL_VIDEODRIVER", "offscreen", 1);  /* EGL surfaceless, no window */
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }

    /* Global settings first — every later macro (FOV, NUM_STARS, …) reads
     * g_settings, so it must be populated before anything else runs. */
    settings_load();

    /* CLI overrides applied after settings_load so they win over settings.json. */
    if (cli_fov > 0.0f)      g_settings.fov = cli_fov;
    if (cli_exposure > 0.0f) { g_settings.tonemap_exposure = cli_exposure;
                               g_settings.auto_exposure = 0; }
    if (cli_shutter   >= 0.0f) g_settings.cine_shutter   = cli_shutter;
    if (cli_aperture  >= 0.0f) g_settings.cine_aperture  = cli_aperture;
    if (cli_grain     >= 0.0f) g_settings.cine_grain     = cli_grain;
    if (cli_rel       >= 0.0f) g_settings.relativistic   = cli_rel;
    if (cli_letterbox >  0.0f) g_settings.cine_letterbox = cli_letterbox;
    s_cine_shot_script = cli_shot_script;
    s_cine_shot_save   = cli_shot_save;
    if (cli_tour_only && cli_director_only) {
        /* Each flag switches the OTHER half off, so together they would leave
         * nothing driving the camera. Asking for both is asking for the
         * default, but say so rather than guess. */
        fprintf(stderr, "--tour and --director are mutually exclusive "
                        "(omit both for tour + director)\n");
        return 1;
    }
    s_cine_tour_only   = cli_tour_only;
    s_cine_director_only = cli_director_only;
    s_cine_dur_set     = cine_dur_set;
    if (cli_focus) {
        /* A bare number is a literal focus distance; anything else is a body
         * name to lock onto. */
        char *endp = NULL;
        double fd = strtod(cli_focus, &endp);
        if (endp && *endp == '\0' && fd > 0.0) {
            g_settings.cine_focus_au   = (float)fd;
            g_settings.cine_focus_auto = 0;
        } else if (!strcmp(cli_focus, "auto")) {
            g_settings.cine_focus_auto = 1;
        } else {
            s_cine_focus_body = cli_focus;   /* applied once the world exists */
        }
    }

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

    /* Freeze/scale the sim after warm-up (so orbits are settled but then held
     * still for reproducible close-range shot framing). */
    if (cli_ts_set) g_laws.time_scale = cli_ts;

    /* Headless camera override (after the world load, which resets the camera). */
    if (cam_set) {
        /* --cam is a Sun-frame position: place it through the floating
         * origin, which rebases onto it (frame.h). */
        frame_place_camera_sun(cam_pos);
        g_cam.yaw    = cam_yaw;
        g_cam.pitch  = cam_pitch;
    }
    /* --frame-offset: displace the local origin by that many AU (Sun frame)
     * before the first frame. Nothing on screen may change beyond rounding:
     * a subsystem that mixes local and Sun-frame positions shows up as
     * something out of place. */
    frame_on_rebase(rel_prev_frame_shift);
    if (cli_frame_offset_set) frame_rebase(cli_frame_offset);

    /* Galaxy rebuilds off the main thread when interactive; headless shots and
     * film-out keep them inline so every frame is reproducible. */
    galaxy_proc_set_async(!headless);

    /* --selftest-frame: floating-origin precision far from home; exits. */
    if (cli_selftest_frame) {
        int ok = freeze_selftest();
        ok = frame_selftest() && ok;
        app_quit();
        return ok ? 0 : 1;
    }

    /* --selftest-starsys: the procedural-system delta round trip; exits. */
    if (cli_selftest_starsys) {
        int ok = starsys_selftest();
        app_quit();
        return ok ? 0 : 1;
    }

    /* --shot-save: load, canonicalise, write, exit. Needs the world (anchor and
     * look_at names resolve against it) but no GL work beyond what is already up. */
    if (s_cine_shot_save) {
        int ok;
        if (s_cine_shot_script) {
            ok = cinema_shot_load(s_cine_shot_script)
              && cinema_shot_save(s_cine_shot_save);
            if (!ok) fprintf(stderr, "[CineCam] --shot-save needs a valid --shot-script\n");
        } else {
            /* No script: write out the tour this universe would film, as a
             * normal shot — the starting point for hand-tuning one (§10), and
             * the way to inspect a tour's keys without rendering it. */
            cinema_tour_set_mode(!s_cine_director_only, !s_cine_tour_only);
            ok = cinema_tour_build(g_cine.duration)
              && cinema_shot_save(s_cine_shot_save);
            if (!ok) fprintf(stderr, "[CineCam] no tour could be built to save\n");
        }
        app_quit();
        return ok ? 0 : 1;
    }

    /* Cinematic renderer: allocate the accumulation targets (needs GL and
     * post.c, both up by now) and open the encoder if we are filming. */
    if (g_cine.enabled) {
        if (g_cine.filming && (WIN_W != g_cine.width || WIN_H != g_cine.height))
            fprintf(stderr, "[Cinematic] drawable is %dx%d, asked for %dx%d — "
                            "filming at the drawable size\n",
                    WIN_W, WIN_H, g_cine.width, g_cine.height);
        cinematic_init();
        if (s_cine_focus_body) cinematic_set_focus_target(s_cine_focus_body);
        /* A shot script takes the camera over from here. Loaded after the
         * world exists, since anchor/look_at/focus names resolve against it. */
        /* No shot script + filming: generate one. Point the tool at any
         * universe and get a watchable film with zero authoring (§10). */
        if (!s_cine_shot_script && g_cine.filming) {
            cinema_tour_set_mode(!s_cine_director_only, !s_cine_tour_only);
            if (!cinema_tour_build(g_cine.duration))
                fprintf(stderr, "[Cinematic] no tour could be built; "
                                "filming from the static camera instead\n");
        }
        if (s_cine_shot_script && cinema_shot_load(s_cine_shot_script)) {
            cinema_shot_play(0.0);
            if (!s_cine_dur_set && g_cine.filming) {
                /* The shot's own length is the film's length unless the user
                 * asked for something else — filming half a move is never what
                 * you meant. */
                g_cine.duration = cinema_shot_duration();
                fprintf(stdout, "[Cinematic] duration from shot: %.2fs (%d frames)\n",
                        g_cine.duration, cinematic_total_frames());
            }
        }
        if (g_cine.filming) {
            set_vsync(0);          /* pace on the GPU, not on a display refresh */
            if (!cinematic_encoder_open()) { app_quit(); return 1; }
        }
    }

    /* Benchmark: build the flythrough tour and begin.  Uncap the frame rate so
     * the run actually measures throughput rather than the vsync interval. */
    if (run_bench) {
        set_vsync(0);
        benchmark_set_ab(bench_ab);
        if (bench_shot_dir) benchmark_set_shot_dir(bench_shot_dir);
        benchmark_start();
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

        /* Cluster aggregation (Phase A #2): how many dense field-star clumps
         * were extracted for impostor rendering, and the richest one. */
        {
            const CosmicCluster *cl;
            int ncl = cosmic_field_clusters(&cl);
            int biggest = 0;
            for (int ci = 1; ci < ncl; ci++)
                if (cl[ci].count > cl[biggest].count) biggest = ci;
            fprintf(stdout, "[Clusters] extracted=%d", ncl);
            if (ncl > 0)
                fprintf(stdout, " richest=%d stars (extent %.1f ly)",
                        cl[biggest].count, cl[biggest].radius_m / LY);
            fprintf(stdout, "\n");
        }

        /* AGN-in-galaxy-host: the nuclei spawned into host galaxies, with world
         * positions (AU) so a fly-to camera can be framed on one. */
        for (int gi = 0; gi < galaxy_count(); gi++) {
            double mkg; float act;
            if (!galaxy_agn(gi, &mkg, &act, NULL)) continue;
            double gp[3];
            galaxy_position(gi, gp);
            float ax[3]; galaxy_axis(gi, ax);
            fprintf(stdout, "[GalaxyAGN] %-18s M=%.2e Msun act=%.2f "
                    "pos_au=%.3e,%.3e,%.3e jet_axis=%.3f,%.3f,%.3f\n",
                    galaxy_name(gi), mkg / 1.989e30, act, gp[0], gp[1], gp[2],
                    ax[0], ax[1], ax[2]);
        }

        /* Orbit prediction (Layer 5.4): validate the forward integrator + energy
         * classifier on a representative body — the forced OMV_PREDICT_BODY if
         * set, else the first curated non-star with a parent. */
        {
            int pb = -1;
            const char *fe = getenv("OMV_PREDICT_BODY");
            if (fe && fe[0])
                for (int i = 0; i < g_nbodies; i++)
                    if (g_bodies[i].alive && !strcmp(g_bodies[i].name, fe)) { pb = i; break; }
            if (pb < 0)
                for (int i = 0; i < g_nbodies && i < MAX_BODIES; i++)
                    if (g_bodies[i].alive && !g_bodies[i].is_star &&
                        g_bodies[i].parent >= 0) { pb = i; break; }
            OrbitPredictInfo pi;
            if (pb >= 0 && !g_orbits_off && orbit_predict_compute(pb, &pi, NULL, 64))
                fprintf(stdout, "[OrbitPredict] %s parent=%s %s period=%.1fd "
                        "a=%.4fau peri=%.4f apo=%.4f e=%.4f pts=%d\n",
                        g_bodies[pb].name,
                        pi.parent >= 0 ? g_bodies[pi.parent].name : "-",
                        pi.plunge ? "plunge" : (pi.bound ? "bound" : "escaping"),
                        pi.period_days, pi.a_au, pi.peri_au, pi.apo_au, pi.ecc, pi.count);
        }

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

        /* Post-warmup body positions (GL frame, AU) — warmup moves everything
         * off its authored phase, so this is what --cam framing must aim at.
         * The 16 bodies nearest the camera, nearest first (O(N·16) selection). */
        {
            int sel[16]; int nsel = 0;
            for (int pick = 0; pick < 16; pick++) {
                int best = -1; double best_d2 = 0.0;
                for (int i = 0; i < g_nbodies; i++) {
                    if (!g_bodies[i].alive) continue;
                    int used = 0;
                    for (int k = 0; k < nsel; k++) if (sel[k] == i) { used = 1; break; }
                    if (used) continue;
                    double dx = g_bodies[i].pos[0] * RS - g_cam.pos[0];
                    double dy = g_bodies[i].pos[1] * RS - g_cam.pos[1];
                    double dz = g_bodies[i].pos[2] * RS - g_cam.pos[2];
                    double d2 = dx*dx + dy*dy + dz*dz;
                    if (best < 0 || d2 < best_d2) { best = i; best_d2 = d2; }
                }
                if (best < 0) break;
                sel[nsel++] = best;
                fprintf(stdout, "[Body] %-16s pos_au=(%.6f, %.6f, %.6f) dcam=%.4g\n",
                        g_bodies[best].name,
                        g_bodies[best].pos[0] * RS, g_bodies[best].pos[1] * RS,
                        g_bodies[best].pos[2] * RS, sqrt(best_d2));
            }
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
    int    cine_frame_no = 0;   /* output frames written during film-out */

    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float  dt  = (float)((double)(now - prev) / (double)freq);
        float  dt_raw = dt;        /* true frame time, before the sim clamp   */
        if (dt > 0.1f) dt = 0.1f;  /* clamp: don't spiral if a frame takes > 100 ms */
        prev = now;

        /* Film-out replaces the wall clock with a fixed frame interval: the
         * sim advances by exactly timescale/fps per frame however long the
         * frame took to compute, which is what makes the output reproducible.
         * The 0.1s clamp above must not apply — it exists to stop a realtime
         * spiral and would silently truncate a long-timescale film. */
        if (cinematic_filming()) dt = dt_raw = (float)cinematic_frame_dt();

        /* Animation phases (corona, galaxy rotation, jets, twinkle) read this
         * rather than SDL_GetTicks, so they advance deterministically too. */
        g_render_time += (double)dt;

        SDL_Event e;
        profiler_frame_begin();
        gpu_timer_frame_begin();
        profiler_stage_begin(PROFILER_STAGE_INPUT);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (menu_process_event(&e)) {
                /* The open menu is modal — it swallows all input. The one
                 * exception is the close keys, so the panel can never trap the
                 * user; while a text field has focus they belong to ImGui
                 * ('u' is a letter there, Esc cancels the edit). */
                if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                    !menu_wants_text_input() &&
                    (e.key.keysym.sym == SDLK_u || e.key.keysym.sym == SDLK_ESCAPE))
                    set_menu_open(0);
                continue;
            }
            handle_event(&e, dt, &running);
        }

        /* Navigate-teleport fly animation. Any manual move key cancels it so
         * the user is never locked out of control mid-flight. */
        if (cam_fly_active()) {
            if (s_key_w || s_key_s || s_key_a || s_key_d || s_key_q || s_key_e)
                cam_fly_cancel();
            else
                cam_fly_update(dt);
            /* Arrived (not cancelled): auto-target the navigated body in
             * inspect mode so the orbit camera + panel pick it up. */
            if (!cam_fly_active()) {
                int fb = cam_fly_take_arrival();
                if (fb >= 0 && fb < g_nbodies && g_bodies[fb].alive)
                    inspect_focus_body(fb);
            }
        }

        /* Benchmark flythrough owns the camera while it runs; it scripts the
         * pose directly, so free-look, fly-to and the pause menu step aside. */
        if (benchmark_active()) {
            /* Feed the *unclamped* frame time so stages running below 10 fps
             * are measured honestly (the sim's 100 ms clamp would otherwise
             * floor every heavy frame at exactly 10 fps). */
            benchmark_update(dt_raw);
            if (!benchmark_active()) running = 0;   /* tour + summary done */
        } else if (!cam_fly_active() && !s_pause_menu_open && !g_inspect_orbit_mode) {
            camera_move(dt);
        }

        /* Star→system promotion (§0.1 final step): make the nearest
         * procedural galaxy stars real bodies before physics runs, so a
         * freshly promoted system integrates this same frame. */
        /* Keep the local frame centred on the camera (frame.h): after the
         * camera moved, before anything simulates or draws. */
        frame_rebase_if_needed();
        /* Systems entering or leaving the active region thaw or freeze
         * (freeze.h), paused or not: the camera moves either way. */
        {
            double fz_cam[3];
            const int *fz_slots = NULL;
            camera_world_m(fz_cam);
            physics_refresh_timestep_model_if_needed(0.0);
            int fz_n = physics_active_systems(fz_cam, ACTIVE_RADIUS_LY * LY, &fz_slots);
            freeze_update(fz_slots, fz_n);
        }
        profiler_stage_end(PROFILER_STAGE_INPUT);

        profiler_stage_begin(PROFILER_STAGE_STARSYS);
        {
            double cam_sun[3];
            frame_cam_sun(cam_sun);
            starsys_tick(cam_sun, (float)g_render_time);
        }
        profiler_stage_end(PROFILER_STAGE_STARSYS);

        /* A playing shot owns the timescale, so evaluate it before the sim
         * steps. Otherwise each frame advances at the previous frame's rate,
         * and the first frame at the universe's default rate: 1 day/s spun
         * Earth 12 degrees before a 30 fps film's first frame. The camera is
         * posed again below, once the anchors have moved. */
        if (cinema_shot_playing()) cinema_shot_eval(cinema_shot_time());

        /* Motion blur (docs/CINEMATIC.md §4.1): the shutter is open for
         * shutter_angle/360 of the frame interval, and the sim is sampled at
         * `nsub` instants across it. The frame still advances by exactly `dt`
         * in total — open_dt during the accumulation loop, the remainder once
         * the shutter closes — so the deterministic time model is unchanged
         * and a blurred film runs at the same rate as an unblurred one. */
        int    cine_mblur  = cinematic_motion_blur_active();
        double cine_open   = cine_mblur ? (double)dt * cinematic_shutter_fraction() : 0.0;
        double cine_closed = (double)dt - cine_open;
        /* Camera-only blur when no object would visibly smear (cinema_blur.h):
         * the camera still re-poses per sample, but the sim advances once
         * instead of N times. From here on cine_mblur means "slice the sim". */
        if (cine_mblur && !g_paused &&
            !cinema_blur_objects_needed(cine_open * g_sim_speed * g_laws.time_scale)) {
            cine_mblur  = 0;
            cine_open   = 0.0;
            cine_closed = (double)dt;
        }

        /* Physics — RESPA hierarchical integrator. Under motion blur this
         * moves into the accumulation loop below, one slice per sub-frame. */
        if (!cine_mblur) advance_simulation(dt);

        if (!s_pause_menu_open && g_inspect_orbit_mode)
            inspect_orbit_update(dt);

        /* Refresh the cosmic density field (throttled; rebuilds on body-set
         * change). Queried by the HUD and, later, continuous LOD. */
        profiler_stage_begin(PROFILER_STAGE_FIELDS);
        {
            /* Per-tick zones: this stage averages a few ms but spikes to ~90,
             * and all three rebuild on a body-set change, so the aggregate
             * cannot say which one hitches. */
            double z0 = profiler_now_ms();
            cosmic_field_tick(dt);
            double z1 = profiler_now_ms();

            /* Refresh emitter luminosities (throttled; they drift on the
             * stellar clock). Queried by render.c body lighting and the HUD. */
            radiance_field_tick(dt);
            double z2 = profiler_now_ms();

            /* Refresh the field graph's harvested edges (throttled; rebuilds
             * on body-set change). Queried by the Inspect Relations view. */
            field_graph_tick(dt);
            double z3 = profiler_now_ms();

            profiler_zone_add("  cosmic_field_tick", z1 - z0);
            profiler_zone_add("  radiance_field_tick", z2 - z1);
            profiler_zone_add("  field_graph_tick", z3 - z2);
        }
        profiler_stage_end(PROFILER_STAGE_FIELDS);

        /* A shot owns the camera while it plays: pose it for this frame before
         * the matrices, the proximity pass and the focus query read g_cam.
         * (The accumulation loop re-poses per sub-frame for motion blur.) */
        if (cinema_shot_playing()) cinema_shot_eval(cinema_shot_time());

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
        profiler_stage_begin(PROFILER_STAGE_CAMPREP);
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

        /* One shared nearest-star/-body pass for this frame — trail fade, the
         * HUD readout and the adaptive-warp governor all read g_cam_prox. */
        body_update_cam_proximity();
        profiler_stage_end(PROFILER_STAGE_CAMPREP);

        /* Relativistic optics: derive an effective beta from the camera's actual
         * speed (position delta / dt). Warp velocities are >> c, so this is a
         * stylistic ramp across the warp range, not a literal v/c. Effect shows
         * only when actually moving fast; 0 below ~200 AU/s. */
        {
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
                /* Not while a shot or tour drives the camera. The effect is a
                 * stylistic ramp for flying at warp; a directed camera covering
                 * 10^9 c between keys is choreography, not a warp flight, and it
                 * pinned beta at 1 — radial "warp tunnel" streaks and the frame
                 * shoved off-centre on every nebula and galaxy leg. */
                if (cinema_shot_playing()) target = 0.0f;

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

        /* Accumulation sampling (docs/CINEMATIC.md §4). Outside cinematic mode
         * nsub is 1 and this is the original single pass, byte-identical:
         * the jitter, accumulate and resolve steps are all skipped and post
         * composites straight to the back buffer. In cinematic mode the frame
         * becomes the mean of nsub sub-frames, each with the projection
         * jittered by a sub-pixel offset — supersampling rather than a
         * post-process AA because the scene is a field of sub-pixel
         * high-contrast points, which TAA ghosts on and FXAA erases. */
        int nsub = cinematic_samples();
        double cine_focus = cinematic_focus_distance();

        /* Unjittered camera basis. Recomputed per sub-frame when a shot is
         * driving the camera, because then the pose genuinely differs between
         * sub-frames — that is what turns the accumulation into CAMERA motion
         * blur rather than only object motion blur. Without a shot the camera
         * is static within the frame, so it is captured once.
         *
         * Doubles throughout: the lens offset is added to g_cam.pos, which
         * render.c subtracts from every body in double precision. */
        double base_fwd[3]   = { fdx, fdy, fdz };
        double base_right[3], base_up[3];
        {
            Vec3 r0, u0;
            mat4_get_right(view_rot, r0);
            mat4_get_up   (view_rot, u0);
            for (int k = 0; k < 3; k++) { base_right[k] = r0[k]; base_up[k] = u0[k]; }
        }

        cinematic_frame_begin();
        for (int sub = 0; sub < nsub; sub++) {
            double cam_save[3] = { g_cam.pos[0], g_cam.pos[1], g_cam.pos[2] };
            int    lens_moved  = 0;

            /* Re-pose the camera at this sub-frame's instant on the shutter.
             * Sub-frame s sees the shot at t + (s/nsub) * shutter_fraction/fps,
             * so a moving camera smears across the open shutter exactly the way
             * a moving body does. */
            if (cinema_shot_playing() && nsub > 1) {
                double sub_t = cinema_shot_time() +
                    ((double)sub / nsub) * cinematic_shutter_fraction() * (double)dt;
                cinema_shot_eval(sub_t);
                cam_save[0] = g_cam.pos[0];
                cam_save[1] = g_cam.pos[1];
                cam_save[2] = g_cam.pos[2];

                /* The shot may have moved, turned, or changed the FOV, so this
                 * sub-frame's clean basis and projection are rebuilt from it. */
                float sfx, sfy, sfz;
                cam_get_dir(&sfx, &sfy, &sfz);
                base_fwd[0] = sfx; base_fwd[1] = sfy; base_fwd[2] = sfz;
                float zf[3] = { 0.0f, 0.0f, 0.0f }, uw[3] = { 0.0f, 1.0f, 0.0f };
                float df[3] = { sfx, sfy, sfz };
                mat4_lookAt(view_rot, zf, df, uw);
                Vec3 r1, u1;
                mat4_get_right(view_rot, r1);
                mat4_get_up   (view_rot, u1);
                for (int k = 0; k < 3; k++) { base_right[k] = r1[k]; base_up[k] = u1[k]; }
                cine_focus = cinematic_focus_distance();
            }

            if (nsub > 1) {
                /* Where this sub-frame's camera looks. Starts from the clean
                 * forward direction; depth of field and the AA jitter both
                 * bend it.
                 *
                 * Both are applied by MOVING AND ROTATING THE CAMERA, never by
                 * skewing the projection matrix. The planet, atmosphere and
                 * volumetric passes reconstruct their camera ray per fragment
                 * from u_fov_tan/aspect and ignore the projection entirely, so
                 * a frustum skew moves the starfield and leaves the planets
                 * exactly where they were. Rotating the camera moves both. */
                double aim[3] = { base_fwd[0], base_fwd[1], base_fwd[2] };

                /* Depth of field: sample a point on the aperture disc, then
                 * aim the offset eye back at the focal point so the plane in
                 * focus stays pinned while everything else parallaxes. */
                double du, dv;
                cinematic_lens_sample(sub, &du, &dv);
                if (cine_focus > 0.0 && (du != 0.0 || dv != 0.0)) {
                    cinematic_lens_aim(du, dv, cine_focus,
                                       base_fwd, base_right, base_up, aim);
                    g_cam.pos[0] += du * base_right[0] + dv * base_up[0];
                    g_cam.pos[1] += du * base_right[1] + dv * base_up[1];
                    g_cam.pos[2] += du * base_right[2] + dv * base_up[2];
                    lens_moved = 1;
                }

                /* Antialiasing: a sub-pixel rotation on top. */
                float ax, ay;
                cinematic_jitter(sub, &ax, &ay);
                if (ax != 0.0f || ay != 0.0f) {
                    for (int k = 0; k < 3; k++)
                        aim[k] += (double)ax * base_right[k] + (double)ay * base_up[k];
                    double len = sqrt(aim[0]*aim[0] + aim[1]*aim[1] + aim[2]*aim[2]);
                    if (len > 0.0) { aim[0] /= len; aim[1] /= len; aim[2] /= len; }
                }

                /* Rebuild both view matrices on the sub-frame pose. view_rot
                 * is rotation-only (far geometry, and the basis the fragment
                 * shaders orient their reconstructed rays with); view carries
                 * the float eye and is used for rings. */
                float dirf[3] = { (float)aim[0], (float)aim[1], (float)aim[2] };
                float upw[3]  = { 0.0f, 1.0f, 0.0f };
                float zerof[3] = { 0.0f, 0.0f, 0.0f };
                mat4_lookAt(view_rot, zerof, dirf, upw);
                float eye[3] = { (float)g_cam.pos[0], (float)g_cam.pos[1],
                                 (float)g_cam.pos[2] };
                float ctr[3] = { eye[0]+dirf[0], eye[1]+dirf[1], eye[2]+dirf[2] };
                mat4_lookAt(view, eye, ctr, upw);
            }
            /* One slice of the open shutter before each sample, so the N
             * sub-frames see N different instants and moving bodies smear
             * instead of stroboscoping. */
            if (cine_mblur) advance_simulation((float)(cine_open / nsub));

            cinematic_sub_begin(sub);

            /* Bloom: render the scene into an HDR target, then composite the
             * glow. When disabled/unavailable these are no-ops and we draw
             * straight to the default framebuffer. */
            if (post_enabled()) {
                post_begin();
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, WIN_W, WIN_H);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }
            profiler_stage_begin(PROFILER_STAGE_RENDER);
            /* dt only on the first sub-frame: render_frame draws, but the few
             * things it does animate on dt (label fades, the aurora low-pass)
             * must advance once per *output* frame, not once per sample. */
            render_frame(view, proj, view_rot, sub == 0 ? dt : 0.0f);
            profiler_stage_end(PROFILER_STAGE_RENDER);

            profiler_stage_begin(PROFILER_STAGE_POST);
            post_end();
            profiler_stage_end(PROFILER_STAGE_POST);

            cinematic_sub_end();

            if (lens_moved) {
                g_cam.pos[0] = cam_save[0];
                g_cam.pos[1] = cam_save[1];
                g_cam.pos[2] = cam_save[2];
            }
        }
        /* Shutter closed: run the rest of the frame's sim time with nothing
         * rendered, so the next frame starts where it would have anyway. */
        if (cine_mblur && cine_closed > 0.0) advance_simulation((float)cine_closed);

        profiler_stage_begin(PROFILER_STAGE_POST);
        cinematic_frame_resolve();   /* average -> back buffer (no-op if nsub==1) */
        /* --cinematic-info titles: once per OUTPUT frame, after the average and
         * before capture, so text is never smeared through the samples. */
        if (cinematic_active() && cinema_titles_enabled())
            cinema_titles_frame(cinematic_filming() ? cinematic_frame_dt() : (double)dt);
        profiler_stage_end(PROFILER_STAGE_POST);
        /* Film-out writes clean frames: no HUD, no menu. */
        if (!cinematic_filming()) {
            profiler_stage_begin(PROFILER_STAGE_UI);
            ui_render();
            profiler_stage_end(PROFILER_STAGE_UI);
        }

        /* Multiverse overlay (drawn last, on top). Returns a preset to switch
         * to, or -1, and may set load_path (e.g. a freshly imported real-data
         * catalog); law-slider edits set laws_changed. No-op without USE_IMGUI. */
        int laws_changed = 0;
        const char *load_path = NULL;
        int menu_pick = -1;
        if (!cinematic_filming()) {
            profiler_stage_begin(PROFILER_STAGE_UI);
            menu_pick = menu_render(preset_index_of_path(s_universe_path),
                                    &laws_changed, &load_path);
            profiler_stage_end(PROFILER_STAGE_UI);
        }

        /* Benchmark stage screenshot: same back-buffer capture as --shot, but
         * mid-run and without quitting. Requested by benchmark_update() earlier
         * this frame; taken here because the frame is now fully drawn. */
        {
            const char *bshot = benchmark_take_shot_path();
            if (bshot) save_screenshot_ppm(bshot);
        }

        /* Shot clock. Film-out drives it straight off the frame counter so the
         * render stays deterministic and independent of how long a frame took;
         * live playback runs on the wall clock. */
        if (cinema_shot_playing()) {
            if (cinema_tour_active()) {
                /* The tour owns the clock: it parks the spine while a director
                 * cutaway plays, so the time it advances is not simply the
                 * frame number. Fixed dt keeps it deterministic all the same. */
                cinema_tour_tick(cinematic_filming()
                                 ? cinematic_frame_dt() : (double)dt);
            } else if (cinematic_filming()) {
                cinema_shot_set_time((double)(cine_frame_no + 1) / (double)g_cine.fps);
            } else {
                cinema_shot_advance((double)dt);
            }
        }

        /* Film-out: read back this frame and push it to the encoder. Must run
         * after everything that draws and before the swap, since the capture
         * reads GL_BACK. */
        if (cinematic_filming()) {
            profiler_stage_begin(PROFILER_STAGE_SWAP);
            cinematic_capture_frame();
            profiler_stage_end(PROFILER_STAGE_SWAP);
            cinematic_report_progress();
            if (++cine_frame_no >= cinematic_total_frames()) running = 0;
        }

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
         * windowed mode doesn't need it (vsync/swap paces the pipeline).
         *
         * Timed as SWAP, not UI: this is the frame's GPU drain, the headless
         * equivalent of SwapWindow. Charging it to whatever stage happens to
         * enclose it makes that stage look like it owns every GPU cost. */
        if (headless) {
            profiler_stage_begin(PROFILER_STAGE_SWAP);
            glFinish();
            profiler_stage_end(PROFILER_STAGE_SWAP);
        }

        gpu_timer_frame_end();
        profiler_stage_begin(PROFILER_STAGE_SWAP);
        SDL_GL_SwapWindow(s_win);
        profiler_stage_end(PROFILER_STAGE_SWAP);
        {
            ProfilerFrameContext pctx;
            pctx.bodies_active  = g_nbodies;
            pctx.systems_active = s_prof_active_systems;
            pctx.systems_dirty  = collision_dirty_system_count();
            profiler_frame_end(&pctx);
        }

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
