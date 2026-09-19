/*
 * camera.h — free-look camera (yaw/pitch, position in GL/AU units)
 */
#pragma once
#include "common.h"

typedef struct {
    double pos[3];  /* AU — double precision to avoid precision loss at large distances */
    float yaw;      /* degrees, horizontal */
    float pitch;    /* degrees, vertical   */
    float speed;    /* AU/second           */
} Camera;

extern Camera g_cam;
extern int    g_warp;   /* 1 = warp mode active (T key), 0 = normal */

/* Reset to default top-down solar-system view */
void cam_reset(void);

/* Compute forward direction vector from current yaw/pitch */
void cam_get_dir(float *dx, float *dy, float *dz);

/* ---- smooth fly-to (Navigate teleport) ----------------------------------
 * Animate the camera from its current pose to (target_pos, yaw, pitch):
 * rotate onto the travel heading first, then fly the straight line, settling
 * onto the final framing while decelerating.  Duration scales with the travel
 * distance.  cam_fly_update() must be called once per frame while
 * cam_fly_active(); cam_fly_cancel() aborts in place. */
void cam_fly_to(const double target_pos[3], float target_yaw, float target_pitch);
int  cam_fly_active(void);
void cam_fly_update(float dt);
void cam_fly_cancel(void);

/* Optional arrival payload: a body index the caller wants acted on when the
 * flight *completes* (a cancelled flight drops it).  The camera just carries
 * the int — main.c hands it to inspect on arrival.  Set after cam_fly_to();
 * cam_fly_take_arrival() returns it exactly once, then -1. */
void cam_fly_set_arrival_body(int idx);
int  cam_fly_take_arrival(void);
