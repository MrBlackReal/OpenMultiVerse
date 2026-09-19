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

/* The flight is phased like a pilot would fly it: turn in place until the
 * camera faces where it is going, accelerate straight down that line, then
 * settle onto the final framing orientation while decelerating into the
 * target.  No translation happens until the heading is right. */

static int    s_fly_active = 0;
static double s_fly_t = 0.0;               /* seconds into the flight        */
static double s_fly_turn_dur = 0.0;        /* phase 1: rotate onto heading   */
static double s_fly_dur = 1.0;             /* phase 2: straight-line travel  */
static double s_fly_p0[3], s_fly_p1[3];
static float  s_fly_yaw0,  s_fly_yaw1;
static float  s_fly_pitch0, s_fly_pitch1;
static float  s_fly_yaw_h, s_fly_pitch_h;  /* travel heading (face-of-flight) */
static int    s_fly_arrival = -1;          /* body to focus on arrival        */

static double fly_ease(double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

/* Wrap `to` into the half-turn nearest `from` so every yaw blend takes the
 * shortest arc and never spins the long way round. */
static float yaw_near(float from, float to) {
    while (to - from >  180.0f) to -= 360.0f;
    while (to - from < -180.0f) to += 360.0f;
    return to;
}

void cam_fly_to(const double target_pos[3], float target_yaw, float target_pitch) {
    double dx, dy, dz, dist, horiz, turn;
    for (int i = 0; i < 3; i++) {
        s_fly_p0[i] = g_cam.pos[i];
        s_fly_p1[i] = target_pos[i];
    }
    s_fly_yaw0   = g_cam.yaw;
    s_fly_pitch0 = g_cam.pitch;
    s_fly_yaw1   = target_yaw;
    s_fly_pitch1 = target_pitch;

    dx = s_fly_p1[0] - s_fly_p0[0];
    dy = s_fly_p1[1] - s_fly_p0[1];
    dz = s_fly_p1[2] - s_fly_p0[2];
    dist  = sqrt(dx*dx + dy*dy + dz*dz);   /* AU */
    horiz = sqrt(dx*dx + dz*dz);

    /* Travel heading = the displacement direction (cam_get_dir convention:
     * yaw = atan2(dz,dx), pitch = asin(dy/d)).  Degenerate hops — no travel,
     * or a near-vertical climb where yaw is meaningless — head straight for
     * the final framing instead. */
    if (dist > 1e-9 && horiz > dist * 1e-4) {
        s_fly_yaw_h   = (float)(atan2(dz, dx) * 180.0 / PI);
        s_fly_pitch_h = (float)(asin (dy / dist) * 180.0 / PI);
    } else {
        s_fly_yaw_h   = s_fly_yaw1;
        s_fly_pitch_h = s_fly_pitch1;
    }
    s_fly_yaw_h = yaw_near(s_fly_yaw0, s_fly_yaw_h);
    s_fly_yaw1  = yaw_near(s_fly_yaw_h, s_fly_yaw1);

    /* Turn-in-place duration from the larger angular leg (~150°/s, eased);
     * an already-aligned camera skips the phase entirely. */
    turn = fabs(s_fly_yaw_h - s_fly_yaw0);
    if (fabs(s_fly_pitch_h - s_fly_pitch0) > turn)
        turn = fabs(s_fly_pitch_h - s_fly_pitch0);
    s_fly_turn_dur = turn < 2.0 ? 0.0 : turn / 150.0;
    if (s_fly_turn_dur > 1.2) s_fly_turn_dur = 1.2;

    /* Scale travel duration with distance (log, so a light-year hop is only a
     * few× longer than an in-system one), clamped to a comfortable range. */
    s_fly_dur = 0.9 + 0.30 * log10(1.0 + dist);
    if (s_fly_dur < 0.9) s_fly_dur = 0.9;
    if (s_fly_dur > 3.0) s_fly_dur = 3.0;
    s_fly_t = 0.0;
    s_fly_arrival = -1;   /* payload belongs to this flight; caller re-sets */
    s_fly_active = 1;
}

int  cam_fly_active(void) { return s_fly_active; }
void cam_fly_cancel(void) { s_fly_active = 0; s_fly_arrival = -1; }

void cam_fly_set_arrival_body(int idx) { s_fly_arrival = idx; }
int  cam_fly_take_arrival(void) {
    int idx = s_fly_arrival;
    s_fly_arrival = -1;
    return idx;
}

void cam_fly_update(float dt) {
    double t, u, w;
    if (!s_fly_active) return;
    s_fly_t += (double)dt;

    /* Phase 1 — rotate onto the travel heading; position holds. */
    if (s_fly_t < s_fly_turn_dur) {
        u = fly_ease(s_fly_t / s_fly_turn_dur);
        g_cam.yaw   = (float)(s_fly_yaw0   + (s_fly_yaw_h   - s_fly_yaw0)   * u);
        g_cam.pitch = (float)(s_fly_pitch0 + (s_fly_pitch_h - s_fly_pitch0) * u);
        return;
    }

    /* Phase 2 — accelerate straight at the target, decelerate into it. */
    t = (s_fly_t - s_fly_turn_dur) / (s_fly_dur > 1e-3 ? s_fly_dur : 1e-3);
    if (t > 1.0) t = 1.0;
    u = fly_ease(t);
    for (int i = 0; i < 3; i++)
        g_cam.pos[i] = s_fly_p0[i] + (s_fly_p1[i] - s_fly_p0[i]) * u;

    /* Hold the travel heading while at speed; ease onto the final framing
     * over the deceleration tail so arrival and the look-at land together. */
    const double SETTLE = 0.65;
    w = t <= SETTLE ? 0.0 : fly_ease((t - SETTLE) / (1.0 - SETTLE));
    g_cam.yaw   = (float)(s_fly_yaw_h   + (s_fly_yaw1   - s_fly_yaw_h)   * w);
    g_cam.pitch = (float)(s_fly_pitch_h + (s_fly_pitch1 - s_fly_pitch_h) * w);

    if (t >= 1.0) s_fly_active = 0;
}
