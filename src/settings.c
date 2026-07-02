/*
 * settings.c — global application settings (see settings.h)
 */
#include "settings.h"
#include "common.h"
#include "json.h"
#include "loading.h"
#include "starfield.h"

/* Zero-initialised; settings_reset()/settings_load() populate it before use. */
AppSettings g_settings;

/* Last on-disk state, captured at load/save. settings_dirty() compares against
 * it so we only rewrite the file when something actually changed this session. */
static AppSettings s_disk_snapshot;

void settings_reset(void)
{
    g_settings.warmup_radius_ly = 1.5;
    g_settings.warmup_years     = 2.0;
    g_settings.active_radius_ly = 2.0;

    g_settings.sys_trail_fade_start = 240.0f;
    g_settings.sys_trail_fade_end   = 1200.0f;
    g_settings.sys_dot_fade_start   = 240.0f;
    g_settings.sys_dot_fade_end     = 1200.0f;
    g_settings.farfield_horizon_au  = 1.0e6f;  /* ~15.8 ly; far dots/glare/BHs fade & cull past this */

    g_settings.lod_body_fade_start_px = 0.75f;
    g_settings.lod_body_fade_end_px   = 1.75f;
    g_settings.lod_glare_full_px      = 1.25f;
    g_settings.lod_glare_fade_px      = 5.00f;
    g_settings.lod_density_max        = 4.0f;
    g_settings.dot_hide_px            = 2.5f;
    g_settings.dot_excl_px            = 6.0f;
    g_settings.near_dot_dist_ly       = 3.0f;

    g_settings.label_max_dist_au = 55.0f;
    g_settings.label_pin_planets = 3;
    g_settings.label_pin_systems = 3;

    g_settings.num_stars     = 4000;
    g_settings.bg_star_count = 18000;

    g_settings.tonemap_mode     = 1;        /* ACES */
    g_settings.tonemap_exposure = 0.76f;
    g_settings.auto_exposure        = 0;
    g_settings.chromatic_aberration = 0.0f;
    g_settings.vignette             = 0.0f;
    g_settings.lens_spikes          = 0.0f;
    g_settings.lens_flare           = 0.25f;
    g_settings.relativistic         = 0.5f;

    g_settings.star_twinkle = 0.4f;
    g_settings.star_corona  = 0.4f;
    g_settings.starspots    = 0.35f;

    g_settings.fov               = 60.0f;
    g_settings.warp_speed_min_au = 200.0f;
    g_settings.warp_speed_max_au = 63241.0f;
    g_settings.adaptive_warp     = 1;
    g_settings.slider_step       = 0.05f;
    g_settings.mouse_sens_min    = 0.05f;
    g_settings.mouse_sens_max    = 1.0f;

    g_settings.status_font_px = 20;
    g_settings.pct_font_px    = 14;

    g_settings.fade_in_dur  = 0.28;
    g_settings.fade_out_dur = 0.30;
    g_settings.prog_ease    = 9.0;
    g_settings.sweep_speed  = 0.62;
    g_settings.present_dt    = 1.0 / 90.0;
    g_settings.accent_r = 0.0f;
    g_settings.accent_g = 0.39215687f;
    g_settings.accent_b = 0.87058824f;

    g_settings.trail_min_segment_len       = 2.0e4;
    g_settings.trail_max_segment_len       = 4.5e8;
    g_settings.trail_base_segment_len      = 1.0e8;
    g_settings.trail_satellite_segment_len = 2.0e6;
    g_settings.trail_close_approach_factor = 0.35;
    g_settings.trail_target_world_len      = 128.0 * AU;
    g_settings.trail_satellite_world_len   = 1.75 * AU;
    g_settings.trail_curve_error_ratio     = 0.22;
    g_settings.trail_curve_min_error       = 5.0e3;
    g_settings.trail_curve_max_error       = 2.0e7;
}

void settings_load(void)
{
    settings_reset();

    JsonNode *root = json_parse_file(SETTINGS_FILE);
    if (!root) {
        fprintf(stdout, "[Settings] no %s — using defaults\n", SETTINGS_FILE);
        /* No file yet: defaults are the baseline, so nothing is "dirty" until
         * the user changes something. */
        s_disk_snapshot = g_settings;
        return;
    }

    /* Missing keys keep their default (json_num returns the supplied default). */
    g_settings.warmup_radius_ly = json_num(json_get(root, "warmup_radius_ly"), g_settings.warmup_radius_ly);
    g_settings.warmup_years     = json_num(json_get(root, "warmup_years"),     g_settings.warmup_years);
    g_settings.active_radius_ly = json_num(json_get(root, "active_radius_ly"), g_settings.active_radius_ly);

    g_settings.sys_trail_fade_start = (float)json_num(json_get(root, "sys_trail_fade_start"), g_settings.sys_trail_fade_start);
    g_settings.sys_trail_fade_end   = (float)json_num(json_get(root, "sys_trail_fade_end"),   g_settings.sys_trail_fade_end);
    g_settings.sys_dot_fade_start   = (float)json_num(json_get(root, "sys_dot_fade_start"),   g_settings.sys_dot_fade_start);
    g_settings.sys_dot_fade_end     = (float)json_num(json_get(root, "sys_dot_fade_end"),     g_settings.sys_dot_fade_end);
    g_settings.farfield_horizon_au  = (float)json_num(json_get(root, "farfield_horizon_au"),  g_settings.farfield_horizon_au);
    /* The far-field horizon must sit inside the depth range or geometry past it
     * would clip; clamp defensively against a bad hand-edited settings file. */
    if (g_settings.farfield_horizon_au > RENDER_DEPTH_FAR)
        g_settings.farfield_horizon_au = RENDER_DEPTH_FAR;

    g_settings.lod_body_fade_start_px = (float)json_num(json_get(root, "lod_body_fade_start_px"), g_settings.lod_body_fade_start_px);
    g_settings.lod_body_fade_end_px   = (float)json_num(json_get(root, "lod_body_fade_end_px"),   g_settings.lod_body_fade_end_px);
    g_settings.lod_glare_full_px      = (float)json_num(json_get(root, "lod_glare_full_px"),      g_settings.lod_glare_full_px);
    g_settings.lod_glare_fade_px      = (float)json_num(json_get(root, "lod_glare_fade_px"),      g_settings.lod_glare_fade_px);
    g_settings.lod_density_max        = (float)json_num(json_get(root, "lod_density_max"),        g_settings.lod_density_max);
    g_settings.dot_hide_px            = (float)json_num(json_get(root, "dot_hide_px"),            g_settings.dot_hide_px);
    g_settings.dot_excl_px            = (float)json_num(json_get(root, "dot_excl_px"),            g_settings.dot_excl_px);
    g_settings.near_dot_dist_ly       = (float)json_num(json_get(root, "near_dot_dist_ly"),       g_settings.near_dot_dist_ly);
    /* Keep each crossfade window well-ordered (end > start) so the smoothstep
     * edges never coincide/cross — guards a hand-edited settings file. */
    if (g_settings.lod_body_fade_end_px < g_settings.lod_body_fade_start_px + 0.05f)
        g_settings.lod_body_fade_end_px = g_settings.lod_body_fade_start_px + 0.05f;
    if (g_settings.lod_glare_fade_px < g_settings.lod_glare_full_px + 0.05f)
        g_settings.lod_glare_fade_px = g_settings.lod_glare_full_px + 0.05f;
    if (g_settings.dot_excl_px < g_settings.dot_hide_px + 0.1f)
        g_settings.dot_excl_px = g_settings.dot_hide_px + 0.1f;
    if (g_settings.lod_density_max < 1.0f) g_settings.lod_density_max = 1.0f;
    if (g_settings.near_dot_dist_ly < 0.1f) g_settings.near_dot_dist_ly = 0.1f;

    g_settings.label_max_dist_au = (float)json_num(json_get(root, "label_max_dist_au"), g_settings.label_max_dist_au);
    g_settings.label_pin_planets = (int)json_num(json_get(root, "label_pin_planets"), g_settings.label_pin_planets);
    g_settings.label_pin_systems = (int)json_num(json_get(root, "label_pin_systems"), g_settings.label_pin_systems);
    if (g_settings.label_pin_planets < 0)  g_settings.label_pin_planets = 0;
    if (g_settings.label_pin_planets > 16) g_settings.label_pin_planets = 16;
    if (g_settings.label_pin_systems < 0)  g_settings.label_pin_systems = 0;
    if (g_settings.label_pin_systems > 16) g_settings.label_pin_systems = 16;

    g_settings.num_stars     = (int)json_num(json_get(root, "num_stars"),     g_settings.num_stars);
    g_settings.bg_star_count = (int)json_num(json_get(root, "bg_star_count"), g_settings.bg_star_count);
    if (g_settings.bg_star_count < 0)      g_settings.bg_star_count = 0;
    if (g_settings.bg_star_count > 200000) g_settings.bg_star_count = 200000;

    g_settings.tonemap_mode     = (int)json_num(json_get(root, "tonemap_mode"),       g_settings.tonemap_mode);
    g_settings.tonemap_exposure = (float)json_num(json_get(root, "tonemap_exposure"), g_settings.tonemap_exposure);
    g_settings.auto_exposure        = (int)json_num(json_get(root, "auto_exposure"),            g_settings.auto_exposure);
    g_settings.chromatic_aberration = (float)json_num(json_get(root, "chromatic_aberration"),   g_settings.chromatic_aberration);
    g_settings.vignette             = (float)json_num(json_get(root, "vignette"),               g_settings.vignette);
    g_settings.lens_spikes          = (float)json_num(json_get(root, "lens_spikes"),            g_settings.lens_spikes);
    g_settings.lens_flare           = (float)json_num(json_get(root, "lens_flare"),             g_settings.lens_flare);
    g_settings.relativistic         = (float)json_num(json_get(root, "relativistic"),           g_settings.relativistic);
    g_settings.star_twinkle = (float)json_num(json_get(root, "star_twinkle"), g_settings.star_twinkle);
    g_settings.star_corona  = (float)json_num(json_get(root, "star_corona"),  g_settings.star_corona);
    g_settings.starspots    = (float)json_num(json_get(root, "starspots"),    g_settings.starspots);

    g_settings.fov               = (float)json_num(json_get(root, "fov"),               g_settings.fov);
    g_settings.warp_speed_min_au = (float)json_num(json_get(root, "warp_speed_min_au"), g_settings.warp_speed_min_au);
    g_settings.warp_speed_max_au = (float)json_num(json_get(root, "warp_speed_max_au"), g_settings.warp_speed_max_au);
    g_settings.adaptive_warp     = (int)json_num(json_get(root, "adaptive_warp"),       g_settings.adaptive_warp);
    g_settings.slider_step       = (float)json_num(json_get(root, "slider_step"),       g_settings.slider_step);
    g_settings.mouse_sens_min    = (float)json_num(json_get(root, "mouse_sens_min"),    g_settings.mouse_sens_min);
    g_settings.mouse_sens_max    = (float)json_num(json_get(root, "mouse_sens_max"),    g_settings.mouse_sens_max);

    g_settings.status_font_px = (int)json_num(json_get(root, "status_font_px"), g_settings.status_font_px);
    g_settings.pct_font_px    = (int)json_num(json_get(root, "pct_font_px"),    g_settings.pct_font_px);

    g_settings.fade_in_dur  = json_num(json_get(root, "fade_in_dur"),  g_settings.fade_in_dur);
    g_settings.fade_out_dur = json_num(json_get(root, "fade_out_dur"), g_settings.fade_out_dur);
    g_settings.prog_ease    = json_num(json_get(root, "prog_ease"),    g_settings.prog_ease);
    g_settings.sweep_speed  = json_num(json_get(root, "sweep_speed"),  g_settings.sweep_speed);
    g_settings.present_dt   = json_num(json_get(root, "present_dt"),   g_settings.present_dt);
    g_settings.accent_r = (float)json_num(json_get(root, "accent_r"), g_settings.accent_r);
    g_settings.accent_g = (float)json_num(json_get(root, "accent_g"), g_settings.accent_g);
    g_settings.accent_b = (float)json_num(json_get(root, "accent_b"), g_settings.accent_b);

    g_settings.trail_min_segment_len       = json_num(json_get(root, "trail_min_segment_len"),       g_settings.trail_min_segment_len);
    g_settings.trail_max_segment_len       = json_num(json_get(root, "trail_max_segment_len"),       g_settings.trail_max_segment_len);
    g_settings.trail_base_segment_len      = json_num(json_get(root, "trail_base_segment_len"),      g_settings.trail_base_segment_len);
    g_settings.trail_satellite_segment_len = json_num(json_get(root, "trail_satellite_segment_len"), g_settings.trail_satellite_segment_len);
    g_settings.trail_close_approach_factor = json_num(json_get(root, "trail_close_approach_factor"), g_settings.trail_close_approach_factor);
    g_settings.trail_target_world_len      = json_num(json_get(root, "trail_target_world_len"),      g_settings.trail_target_world_len);
    g_settings.trail_satellite_world_len   = json_num(json_get(root, "trail_satellite_world_len"),   g_settings.trail_satellite_world_len);
    g_settings.trail_curve_error_ratio     = json_num(json_get(root, "trail_curve_error_ratio"),     g_settings.trail_curve_error_ratio);
    g_settings.trail_curve_min_error       = json_num(json_get(root, "trail_curve_min_error"),       g_settings.trail_curve_min_error);
    g_settings.trail_curve_max_error       = json_num(json_get(root, "trail_curve_max_error"),       g_settings.trail_curve_max_error);

    json_free(root);
    s_disk_snapshot = g_settings;     /* current state now matches disk */
    fprintf(stdout, "[Settings] loaded %s\n", SETTINGS_FILE);
}

int settings_dirty(void)
{
    return memcmp(&g_settings, &s_disk_snapshot, sizeof g_settings) != 0;
}

int settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) {
        fprintf(stderr, "[Settings] cannot write %s\n", SETTINGS_FILE);
        return -1;
    }
    fprintf(f, "{\n");
    fprintf(f, "  // OpenMultiVerse global settings — applies to every universe.\n");
    fprintf(f, "  // Per-universe physics (gravity, timestep) lives in each universe's \"laws\" block.\n\n");

    fprintf(f, "  \"warmup_radius_ly\": %.10g,\n", g_settings.warmup_radius_ly);
    fprintf(f, "  \"warmup_years\": %.10g,\n",     g_settings.warmup_years);
    fprintf(f, "  \"active_radius_ly\": %.10g,\n\n", g_settings.active_radius_ly);

    fprintf(f, "  \"sys_trail_fade_start\": %.6g, \"sys_trail_fade_end\": %.6g,\n",
            (double)g_settings.sys_trail_fade_start, (double)g_settings.sys_trail_fade_end);
    fprintf(f, "  \"sys_dot_fade_start\": %.6g, \"sys_dot_fade_end\": %.6g,\n",
            (double)g_settings.sys_dot_fade_start, (double)g_settings.sys_dot_fade_end);
    fprintf(f, "  \"farfield_horizon_au\": %.6g,\n\n", (double)g_settings.farfield_horizon_au);

    fprintf(f, "  \"lod_body_fade_start_px\": %.6g, \"lod_body_fade_end_px\": %.6g,\n",
            (double)g_settings.lod_body_fade_start_px, (double)g_settings.lod_body_fade_end_px);
    fprintf(f, "  \"lod_glare_full_px\": %.6g, \"lod_glare_fade_px\": %.6g,\n",
            (double)g_settings.lod_glare_full_px, (double)g_settings.lod_glare_fade_px);
    fprintf(f, "  \"lod_density_max\": %.6g,\n", (double)g_settings.lod_density_max);
    fprintf(f, "  \"dot_hide_px\": %.6g, \"dot_excl_px\": %.6g,\n",
            (double)g_settings.dot_hide_px, (double)g_settings.dot_excl_px);
    fprintf(f, "  \"near_dot_dist_ly\": %.6g,\n\n", (double)g_settings.near_dot_dist_ly);

    fprintf(f, "  \"label_max_dist_au\": %.6g,\n", (double)g_settings.label_max_dist_au);
    fprintf(f, "  \"label_pin_planets\": %d, \"label_pin_systems\": %d,\n\n",
            g_settings.label_pin_planets, g_settings.label_pin_systems);

    fprintf(f, "  \"num_stars\": %d, \"bg_star_count\": %d,\n\n",
            g_settings.num_stars, g_settings.bg_star_count);

    fprintf(f, "  \"tonemap_mode\": %d, \"tonemap_exposure\": %.6g,\n",
            g_settings.tonemap_mode, (double)g_settings.tonemap_exposure);
    fprintf(f, "  \"auto_exposure\": %d, \"chromatic_aberration\": %.6g,\n",
            g_settings.auto_exposure, (double)g_settings.chromatic_aberration);
    fprintf(f, "  \"vignette\": %.6g, \"lens_spikes\": %.6g, \"lens_flare\": %.6g, \"relativistic\": %.6g,\n",
            (double)g_settings.vignette, (double)g_settings.lens_spikes,
            (double)g_settings.lens_flare,
            (double)g_settings.relativistic);
    fprintf(f, "  \"star_twinkle\": %.6g, \"star_corona\": %.6g, \"starspots\": %.6g,\n\n",
            (double)g_settings.star_twinkle, (double)g_settings.star_corona,
            (double)g_settings.starspots);

    fprintf(f, "  \"fov\": %.6g,\n", (double)g_settings.fov);
    fprintf(f, "  \"warp_speed_min_au\": %.10g, \"warp_speed_max_au\": %.10g,\n",
            (double)g_settings.warp_speed_min_au, (double)g_settings.warp_speed_max_au);
    fprintf(f, "  \"adaptive_warp\": %d,\n", g_settings.adaptive_warp);
    fprintf(f, "  \"slider_step\": %.6g,\n", (double)g_settings.slider_step);
    fprintf(f, "  \"mouse_sens_min\": %.6g, \"mouse_sens_max\": %.6g,\n\n",
            (double)g_settings.mouse_sens_min, (double)g_settings.mouse_sens_max);

    fprintf(f, "  \"status_font_px\": %d, \"pct_font_px\": %d,\n\n",
            g_settings.status_font_px, g_settings.pct_font_px);

    fprintf(f, "  \"fade_in_dur\": %.6g, \"fade_out_dur\": %.6g,\n",
            g_settings.fade_in_dur, g_settings.fade_out_dur);
    fprintf(f, "  \"prog_ease\": %.6g, \"sweep_speed\": %.6g, \"present_dt\": %.10g,\n",
            g_settings.prog_ease, g_settings.sweep_speed, g_settings.present_dt);
    fprintf(f, "  \"accent_r\": %.5f, \"accent_g\": %.5f, \"accent_b\": %.5f,\n\n",
            (double)g_settings.accent_r, (double)g_settings.accent_g, (double)g_settings.accent_b);

    fprintf(f, "  \"trail_min_segment_len\": %.6g, \"trail_max_segment_len\": %.6g,\n",
            g_settings.trail_min_segment_len, g_settings.trail_max_segment_len);
    fprintf(f, "  \"trail_base_segment_len\": %.6g, \"trail_satellite_segment_len\": %.6g,\n",
            g_settings.trail_base_segment_len, g_settings.trail_satellite_segment_len);
    fprintf(f, "  \"trail_close_approach_factor\": %.6g,\n",
            g_settings.trail_close_approach_factor);
    fprintf(f, "  \"trail_target_world_len\": %.10g, \"trail_satellite_world_len\": %.10g,\n",
            g_settings.trail_target_world_len, g_settings.trail_satellite_world_len);
    fprintf(f, "  \"trail_curve_error_ratio\": %.6g,\n", g_settings.trail_curve_error_ratio);
    fprintf(f, "  \"trail_curve_min_error\": %.6g, \"trail_curve_max_error\": %.6g\n",
            g_settings.trail_curve_min_error, g_settings.trail_curve_max_error);

    fprintf(f, "}\n");
    fclose(f);
    s_disk_snapshot = g_settings;     /* disk now matches current state */
    fprintf(stdout, "[Settings] saved %s\n", SETTINGS_FILE);
    return 0;
}

void settings_apply_fonts(void)     { loading_reload_fonts(); }
void settings_apply_starfield(void) { starfield_init(); }
