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

/* ---------------------------------------------------------------- fly-to */

static int    s_fly_active = 0;
static double s_fly_t = 0.0, s_fly_dur = 1.0;
static double s_fly_p0[3], s_fly_p1[3];
static float  s_fly_yaw0,  s_fly_yaw1;
static float  s_fly_pitch0, s_fly_pitch1;

/* Quintic smootherstep, front-loaded like inspect's fly-in so the motion feels
 * snappy at the start and settles gently at the target. */
static double fly_ease(double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    t = pow(t, 1.35);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

void cam_fly_to(const double target_pos[3], float target_yaw, float target_pitch) {
    double dx, dy, dz, dist;
    for (int i = 0; i < 3; i++) {
        s_fly_p0[i] = g_cam.pos[i];
        s_fly_p1[i] = target_pos[i];
    }
    s_fly_yaw0   = g_cam.yaw;
    s_fly_yaw1   = target_yaw;
    /* Rotate along the shortest arc so a small turn never spins the long way. */
    while (s_fly_yaw1 - s_fly_yaw0 >  180.0f) s_fly_yaw1 -= 360.0f;
    while (s_fly_yaw1 - s_fly_yaw0 < -180.0f) s_fly_yaw1 += 360.0f;
    s_fly_pitch0 = g_cam.pitch;
    s_fly_pitch1 = target_pitch;

    dx = s_fly_p1[0] - s_fly_p0[0];
    dy = s_fly_p1[1] - s_fly_p0[1];
    dz = s_fly_p1[2] - s_fly_p0[2];
    dist = sqrt(dx*dx + dy*dy + dz*dz);   /* AU */

    /* Scale duration with travel distance (log, so a light-year hop is only a
     * few× longer than an in-system one), clamped to a comfortable range. */
    s_fly_dur = 0.9 + 0.30 * log10(1.0 + dist);
    if (s_fly_dur < 0.9) s_fly_dur = 0.9;
    if (s_fly_dur > 3.0) s_fly_dur = 3.0;
    s_fly_t = 0.0;
    s_fly_active = 1;
}

int  cam_fly_active(void) { return s_fly_active; }
void cam_fly_cancel(void) { s_fly_active = 0; }

void cam_fly_update(float dt) {
    double t, u;
    if (!s_fly_active) return;
    s_fly_t += (double)dt / (s_fly_dur > 1e-3 ? s_fly_dur : 1e-3);
    t = s_fly_t > 1.0 ? 1.0 : s_fly_t;
    u = fly_ease(t);

    for (int i = 0; i < 3; i++)
        g_cam.pos[i] = s_fly_p0[i] + (s_fly_p1[i] - s_fly_p0[i]) * u;
    g_cam.yaw   = (float)(s_fly_yaw0   + (s_fly_yaw1   - s_fly_yaw0)   * u);
    g_cam.pitch = (float)(s_fly_pitch0 + (s_fly_pitch1 - s_fly_pitch0) * u);

    if (s_fly_t >= 1.0) s_fly_active = 0;
}
