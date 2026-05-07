/*
 * camera.c — camera state and free-look movement helpers
 *
 * The camera stores position in AU (double precision) and orientation as
 * yaw/pitch angles in degrees.  Speed is in AU per real-second and is shared
 * by both normal and warp modes (main.c clamps it to the appropriate range
 * before calling camera_move).
 *
 * Coordinate convention (matches the ecliptic → GL frame in body.c):
 *   +X = toward vernal equinox (ecliptic east)
 *   +Y = ecliptic north (up in the default view)
 *   +Z = ecliptic south-east (depth into the scene)
 */
#include "camera.h"
#include <math.h>

Camera g_cam;

/* ---------------------------------------------------------------- public */

/* Reset to a top-down overview position above the ecliptic plane.
 * Yaw -90° points the camera toward -X (roughly toward the inner planets).
 * Pitch -26.565° is arctan(-0.5) — a natural "looking down" angle that shows
 * the full solar system without excessive foreshortening. */
void cam_reset(void) {
    g_cam.pos[0] =   0.0;
    g_cam.pos[1] =   3.0;   /* 3 AU above ecliptic */
    g_cam.pos[2] =   6.0;   /* 6 AU out along +Z   */
    g_cam.yaw    = -90.0f;
    g_cam.pitch  = -26.565f;
    g_cam.speed  =   0.5f;
}

/* Compute the unit forward vector from yaw and pitch (spherical coordinates).
 *
 * Yaw rotates around the Y axis (left/right), pitch tilts up/down.
 * The derivation is standard spherical-to-Cartesian:
 *   dx = cos(pitch) * cos(yaw)
 *   dy = sin(pitch)
 *   dz = cos(pitch) * sin(yaw)
 *
 * Pitch is clamped to ±89° in the event handler so cp never reaches zero,
 * avoiding a degenerate forward vector parallel to world-up. */
void cam_get_dir(float *dx, float *dy, float *dz) {
    float cy = cosf(g_cam.yaw   * (float)(PI / 180.0));
    float sy = sinf(g_cam.yaw   * (float)(PI / 180.0));
    float cp = cosf(g_cam.pitch * (float)(PI / 180.0));
    float sp = sinf(g_cam.pitch * (float)(PI / 180.0));
    *dx = cy * cp;
    *dy = sp;
    *dz = sy * cp;
}
