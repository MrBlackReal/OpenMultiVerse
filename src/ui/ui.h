/*
 * ui.h — 2D HUD overlay
 */
#pragma once
#include "common.h"

void ui_init(void);
void ui_set_pause_menu(int visible, int selected, int vsync_enabled, float music_vol, float mouse_sens, int page);
int  ui_pause_menu_hit_test(int mouse_x, int mouse_y);
int  ui_pause_menu_slider_click_delta(int mouse_x, int mouse_y);
int  ui_controls_return_hit_test(int mouse_x, int mouse_y);
void ui_notify_speed_change(void);
void ui_render(void);
void ui_shutdown(void);

/* Cinematic lower-third (docs/CINEMATIC.md §11.1): up to three lines and an
 * optional scale bar (bar_px long, labelled) in the lower left of the picture
 * band [band_top, band_bottom] (output pixels, y down — inside any letterbox).
 * Fades with alpha; draws nothing at 0. */
void ui_cine_title(const char *title, const char *line2, const char *line3,
                   float alpha, float bar_px, const char *bar_label,
                   float band_top, float band_bottom);

/* Camera-speed readout, right-aligned in the lower right of the picture band
 * (same fonts and margins as ui_cine_title). */
void ui_cine_speed(const char *label, float alpha, float band_top, float band_bottom);
