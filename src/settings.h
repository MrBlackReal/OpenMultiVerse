/*
 * settings.h — global application settings (g_settings)
 *
 * These are the app-level tunables that used to be compile-time #defines:
 * starfield density, camera FOV, far-field fade distances, warm-up / active
 * region radii, control sensitivities, loading-overlay look, and trail
 * sampling geometry.  They apply to every universe and persist to a single
 * global settings file (settings.json), independent of any universe.
 *
 * Per-universe *physics* tunables (gravity, timestep model, …) do NOT live
 * here — they belong to g_laws and are saved inside each universe's JSON
 * "laws" block.  See laws.h.
 *
 * Pattern mirrors g_laws: a single mutable global, read directly by call
 * sites through the macro aliases in common.h (FOV, NUM_STARS, …).  Changing
 * a field takes effect immediately, except num_stars and the font sizes,
 * which need a regenerate/reload step (settings_apply_starfield() /
 * settings_apply_fonts()) because they own GL / TTF resources.
 */
#pragma once

typedef struct {
    /* ---- load / scale ------------------------------------------------- */
    double warmup_radius_ly;   /* systems this close to the camera get warmed */
    double warmup_years;       /* years of physics pre-simulated at load      */
    double active_radius_ly;   /* live-simulated region radius each frame      */

    /* ---- far-field fade (AU) ------------------------------------------ */
    float  sys_trail_fade_start, sys_trail_fade_end;
    float  sys_dot_fade_start,   sys_dot_fade_end;

    /* ---- starfield ---------------------------------------------------- */
    int    num_stars;          /* procedural skybox count (apply = regen)     */

    /* ---- post-processing / tonemap ----------------------------------- */
    int    tonemap_mode;       /* 0 = off (linear), 1 = ACES, 2 = Reinhard    */
    float  tonemap_exposure;   /* linear exposure before the tonemap curve    */
    int    auto_exposure;      /* adapt exposure to scene luminance (0/1)     */
    float  chromatic_aberration; /* lateral CA strength (0 = off)             */
    float  vignette;           /* corner darkening 0..1 (0 = off)             */
    float  lens_spikes;        /* star-glare diffraction spike strength (0=off)*/
    float  relativistic;       /* relativistic aberration/Doppler at warp 0..1 */

    /* ---- stellar appearance ------------------------------------------ */
    float  star_twinkle;       /* dot brightness shimmer 0..1 (0 = off)       */
    float  star_corona;        /* glare corona streamers 0..1 (0 = off)       */
    float  starspots;          /* star-surface spot/granulation 0..1 (0=off)  */

    /* ---- camera / controls ------------------------------------------- */
    float  fov;                /* vertical field of view (degrees)            */
    float  warp_speed_min_au, warp_speed_max_au;
    float  slider_step;        /* keyboard/wheel adjust increment             */
    float  mouse_sens_min, mouse_sens_max;

    /* ---- loading overlay: fonts (apply = reload) --------------------- */
    int    status_font_px;
    int    pct_font_px;

    /* ---- loading overlay: animation / look --------------------------- */
    double fade_in_dur, fade_out_dur;
    double prog_ease;          /* progress approach rate (1/s)                */
    double sweep_speed;        /* indeterminate sweeps per second             */
    double present_dt;         /* min seconds between overlay presents         */
    float  accent_r, accent_g, accent_b;

    /* ---- trail sampling geometry ------------------------------------- */
    double trail_min_segment_len, trail_max_segment_len;
    double trail_base_segment_len, trail_satellite_segment_len;
    double trail_close_approach_factor;
    double trail_target_world_len, trail_satellite_world_len;
    double trail_curve_error_ratio;
    double trail_curve_min_error, trail_curve_max_error;
} AppSettings;

/* The single active settings set, read directly in hot paths. */
extern AppSettings g_settings;

/* Default global settings file (relative to the working directory). */
#define SETTINGS_FILE "settings.json"

/* Reset every field to its built-in default. */
void settings_reset(void);

/* Load from SETTINGS_FILE on top of defaults (missing keys keep defaults).
 * Always leaves g_settings fully populated, even if the file is absent. */
void settings_load(void);

/* Write the current g_settings to SETTINGS_FILE. Returns 0 on success. */
int  settings_save(void);

/* True if g_settings differs from what's on disk (changed since load/save). */
int  settings_dirty(void);

/* Apply settings that own external resources (call after changing them):
 *   _fonts     — reopen the loading-overlay fonts at the new sizes.
 *   _starfield — regenerate the procedural skybox at the new star count. */
void settings_apply_fonts(void);
void settings_apply_starfield(void);
