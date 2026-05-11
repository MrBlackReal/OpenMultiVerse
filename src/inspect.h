/*
 * inspect.h - body inspection and orbit camera mode
 */
#pragma once
#include "common.h"
#include "labels.h"

extern int g_inspect_mode;
extern int g_inspect_orbit_mode;
extern int g_inspect_hovered;
extern int g_inspect_target;

void inspect_init(void);
void inspect_toggle(void);
void inspect_cancel(void);
void inspect_exit_orbit(void);

void inspect_pick_center(const float vp_camrel[16], const BodyRenderInfo *info);
int  inspect_begin_orbit(void);
void inspect_orbit_mouse(int dx, int dy, float sens_deg_per_px);
void inspect_orbit_zoom(int wheel_y);
void inspect_orbit_update(float dt);

/* Returns 1 and fills rel (camera-relative body position), dr (visual radius
   in AU), and alpha if a ring should be drawn this frame, 0 otherwise. */
int inspect_ring_params(const BodyRenderInfo *info,
                        float rel[3], float *dr, float *alpha);
