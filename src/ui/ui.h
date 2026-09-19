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
