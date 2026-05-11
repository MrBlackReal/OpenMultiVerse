/*
 * inspect.c - target picking and orbit camera for inspect mode
 *
 * Sections (search "§"):
 *   § STATE  — statics, constants
 *   § MATH   — geometry helpers (double-precision, orbit-specific)
 *   § CAMERA — look-at animation, orbit distance lerp
 *   § API    — public interface
 */
#include "inspect.h"
#include "body.h"
#include "camera.h"
#include "physics.h"
#include "math3d.h"
#include "collision.h"
#include <float.h>

/* ── § STATE ─────────────────────────────────────────────────────────────── */

int g_inspect_mode = 0;
int g_inspect_orbit_mode = 0;
int g_inspect_hovered = -1;
int g_inspect_target = -1;

static int    s_prev_paused = 0;
static double s_orbit_distance         = 0.01;
static double s_orbit_current_distance = 0.01;
static double s_orbit_start_distance   = 0.01;
static double s_orbit_min_distance     = 0.01;
static double s_orbit_max_distance     = 0.1;
static double s_orbit_transition_t        = 1.0; /* ≥1 = idle (no fly-in) */
static double s_orbit_transition_duration = 1.45;
static int    s_orbit_zoom_smoothing = 0;
static double s_orbit_dir[3] = {0.0, 0.0, 1.0}; /* camera offset direction, unit */
static double s_look_transition_t        = 1.0;  /* ≥1 = locked on target */
static double s_look_transition_duration = 0.75;

#define INSPECT_MAX_PICK_AU 100.0

/* ── § MATH ──────────────────────────────────────────────────────────────── */

/* Orbit pole constraint axis — world Y-up; not the body's orbital plane. */
static void world_orbit_axis(double axis[3])
{
    axis[0] = 0.0;
    axis[1] = 1.0;
    axis[2] = 0.0;
}

static void normalize3(double v[3])
{
    double len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len <= 1e-12) return;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

/* Rodrigues rotation; renormalizes to prevent float drift. */
static void rotate_vec_axis(double v[3], const double axis[3], double angle)
{
    double c = cos(angle);
    double s = sin(angle);
    double dot = v[0]*axis[0] + v[1]*axis[1] + v[2]*axis[2];
    double cross[3] = {
        axis[1]*v[2] - axis[2]*v[1],
        axis[2]*v[0] - axis[0]*v[2],
        axis[0]*v[1] - axis[1]*v[0]
    };
    double out[3] = {
        v[0]*c + cross[0]*s + axis[0]*dot*(1.0 - c),
        v[1]*c + cross[1]*s + axis[1]*dot*(1.0 - c),
        v[2]*c + cross[2]*s + axis[2]*dot*(1.0 - c)
    };
    v[0] = out[0];
    v[1] = out[1];
    v[2] = out[2];
    normalize3(v);
}

/* Clamp s_orbit_dir away from the poles of axis: keeps |dot| ≤ 0.985 (~10°). */
static void clamp_orbit_pole(const double axis[3])
{
    const double limit = 0.985;
    double dot = s_orbit_dir[0]*axis[0] + s_orbit_dir[1]*axis[1] + s_orbit_dir[2]*axis[2];
    double side[3], side_len, side_scale, sign;
    if (dot <= limit && dot >= -limit) return;
    sign = dot < 0.0 ? -1.0 : 1.0;
    side[0] = s_orbit_dir[0] - axis[0] * dot;
    side[1] = s_orbit_dir[1] - axis[1] * dot;
    side[2] = s_orbit_dir[2] - axis[2] * dot;
    side_len = sqrt(side[0]*side[0] + side[1]*side[1] + side[2]*side[2]);
    if (side_len < 1e-9) {
        side[0] = 1.0;
        side[1] = 0.0;
        side[2] = 0.0;
        side_len = 1.0;
    }
    side[0] /= side_len; side[1] /= side_len; side[2] /= side_len;
    side_scale = sqrt(1.0 - limit * limit);
    s_orbit_dir[0] = axis[0] * limit * sign + side[0] * side_scale;
    s_orbit_dir[1] = axis[1] * limit * sign + side[1] * side_scale;
    s_orbit_dir[2] = axis[2] * limit * sign + side[2] * side_scale;
    normalize3(s_orbit_dir);
}

/* ── § CAMERA ────────────────────────────────────────────────────────────── */

/* pow(t,1.45) bias front-loads the motion so the initial snap feels snappy. */
static double smootherstep(double t)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    t = pow(t, 1.45);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

/* Log-space lerp: equal perceptual speed across orders-of-magnitude distances. */
static double visual_distance_lerp(double start, double target, double t)
{
    double log_start, log_target, log_dist;
    if (start <= 1e-12 || target <= 1e-12)
        return start + (target - start) * t;

    log_start = log(start);
    log_target = log(target);
    log_dist = log_start + (log_target - log_start) * t;
    return exp(log_dist);
}

static void set_orbit_target_from_camera(int idx, double duration)
{
    double center[3], rel[3], dist, radius_au, target_dist, axis[3];
    double visual_ratio, distance_blend;

    center[0] = g_bodies[idx].pos[0] * RS;
    center[1] = g_bodies[idx].pos[1] * RS;
    center[2] = g_bodies[idx].pos[2] * RS;
    rel[0] = g_cam.pos[0] - center[0];
    rel[1] = g_cam.pos[1] - center[1];
    rel[2] = g_cam.pos[2] - center[2];
    dist = sqrt(rel[0]*rel[0] + rel[1]*rel[1] + rel[2]*rel[2]);
    if (dist < 1e-12) dist = 1e-12;

    radius_au   = collision_visual_radius(idx, g_bodies[idx].radius) * RS;
    target_dist = radius_au * 7.0;
    if (target_dist < 0.000015) target_dist = 0.000015;
    g_cam.speed = (float)(2.5 * pow(radius_au, 0.835));
    if (g_cam.speed < 0.00003f) g_cam.speed = 0.00003f;
    if (g_cam.speed > 0.08f)    g_cam.speed = 0.08f;

    s_orbit_min_distance = radius_au * 2.4;
    s_orbit_max_distance = radius_au * 55.0;
    if (s_orbit_min_distance < 0.000006) s_orbit_min_distance = 0.000006;
    if (s_orbit_max_distance < s_orbit_min_distance * 4.0)
        s_orbit_max_distance = s_orbit_min_distance * 4.0;
    if (target_dist < s_orbit_min_distance) target_dist = s_orbit_min_distance;
    if (target_dist > s_orbit_max_distance) target_dist = s_orbit_max_distance;

    /* rel/dist is already a unit vector — no further normalize needed. */
    s_orbit_dir[0] = rel[0] / dist;
    s_orbit_dir[1] = rel[1] / dist;
    s_orbit_dir[2] = rel[2] / dist;
    g_inspect_target = idx;
    world_orbit_axis(axis);
    clamp_orbit_pole(axis);

    s_orbit_current_distance = dist;
    s_orbit_start_distance   = dist;
    s_orbit_distance         = target_dist;
    s_orbit_transition_t     = 0.0;

    /* Scale animation duration by log10 of the distance ratio so a 10000×
     * distance change gets ~4× longer than a same-scale transition. */
    visual_ratio     = fmax(dist, target_dist) / fmax(fmin(dist, target_dist), 1e-9);
    distance_blend   = log(visual_ratio) / log(10000.0);
    if (distance_blend > 1.0) distance_blend = 1.0;
    if (distance_blend < 0.0) distance_blend = 0.0;
    s_orbit_transition_duration = duration * (1.0 + distance_blend * 3.25);
    s_orbit_zoom_smoothing      = 0;
    s_look_transition_t         = 0.0;
    s_look_transition_duration  = duration < 1.0 ? 0.55 : 0.75;
}

static void desired_look_angles(int idx, double *yaw, double *pitch)
{
    double tx, ty, tz, dx, dy, dz, len;
    if (idx < 0 || idx >= g_nbodies) return;

    tx = g_bodies[idx].pos[0] * RS;
    ty = g_bodies[idx].pos[1] * RS;
    tz = g_bodies[idx].pos[2] * RS;
    dx = tx - g_cam.pos[0];
    dy = ty - g_cam.pos[1];
    dz = tz - g_cam.pos[2];
    len = sqrt(dx*dx + dy*dy + dz*dz);
    if (len <= 1e-12) return;

    dx /= len;
    dy /= len;
    dz /= len;
    *yaw   = atan2(dz, dx) * 180.0 / PI;
    *pitch = asin(dy)      * 180.0 / PI;
}

static void smooth_look_at_target(int idx, float dt)
{
    double yaw = g_cam.yaw;
    double pitch = g_cam.pitch;
    double dyaw, dpitch, follow;

    desired_look_angles(idx, &yaw, &pitch);
    dyaw = yaw - (double)g_cam.yaw;
    while (dyaw >  180.0) dyaw -= 360.0;
    while (dyaw < -180.0) dyaw += 360.0;
    dpitch = pitch - (double)g_cam.pitch;

    if (s_look_transition_t >= 1.0) {
        /* Transition done: snap-track so the target stays centered. */
        g_cam.yaw += (float)dyaw;
        g_cam.pitch = (float)pitch;
        if (g_cam.pitch >  89.0f) g_cam.pitch =  89.0f;
        if (g_cam.pitch < -89.0f) g_cam.pitch = -89.0f;
        return;
    }

    s_look_transition_t += (double)dt / fmax(s_look_transition_duration, 0.001);
    if (s_look_transition_t > 1.0) s_look_transition_t = 1.0;
    follow = 1.0 - exp(-(double)dt * (3.0 + 10.0 * smootherstep(s_look_transition_t)));
    if (follow < 0.0) follow = 0.0;
    if (follow > 1.0) follow = 1.0;

    g_cam.yaw   += (float)(dyaw   * follow);
    g_cam.pitch += (float)(dpitch * follow);
    if (g_cam.pitch >  89.0f) g_cam.pitch =  89.0f;
    if (g_cam.pitch < -89.0f) g_cam.pitch = -89.0f;
}

/* ── § API ───────────────────────────────────────────────────────────────── */

void inspect_init(void)
{
    g_inspect_mode       = 0;
    g_inspect_orbit_mode = 0;
    g_inspect_hovered    = -1;
    g_inspect_target     = -1;
    s_prev_paused              = 0;
    s_orbit_distance           = 0.01;
    s_orbit_current_distance   = 0.01;
    s_orbit_start_distance     = 0.01;
    s_orbit_min_distance       = 0.01;
    s_orbit_max_distance       = 0.1;
    s_orbit_transition_t       = 1.0;
    s_orbit_transition_duration= 1.45;
    s_orbit_zoom_smoothing     = 0;
    s_look_transition_t        = 1.0;
    s_look_transition_duration = 0.75;
    s_orbit_dir[0] = 0.0;
    s_orbit_dir[1] = 0.0;
    s_orbit_dir[2] = 1.0;
}

void inspect_cancel(void)
{
    if (g_inspect_mode)
        g_paused = s_prev_paused;
    g_inspect_mode       = 0;
    g_inspect_orbit_mode = 0;
    g_inspect_hovered    = -1;
    g_inspect_target     = -1;
}

void inspect_exit_orbit(void)
{
    g_inspect_orbit_mode = 0;
    g_inspect_target     = -1;
}

void inspect_toggle(void)
{
    if (g_inspect_mode) {
        inspect_cancel();
        return;
    }
    s_prev_paused        = g_paused;
    g_paused             = 1;
    g_inspect_mode       = 1;
    g_inspect_orbit_mode = 0;
    g_inspect_hovered    = -1;
    g_inspect_target     = -1;
}

void inspect_pick_center(const float vp_camrel[16], const BodyRenderInfo *info)
{
    float center_x, center_y;
    float best_score = FLT_MAX;
    int best_idx = -1;

    if (!g_inspect_mode || g_inspect_orbit_mode || !info) {
        if (!g_inspect_orbit_mode) g_inspect_hovered = -1;
        return;
    }

    center_x = (float)WIN_W * 0.5f;
    center_y = (float)WIN_H * 0.5f;

    for (int i = 0; i < g_nbodies; i++) {
        float rx, ry, rz, sx, sy, sy_top, dx, dy, screen_d, body_px, tol, score;
        if (!g_bodies[i].alive) continue;
        if (info[i].dcam > INSPECT_MAX_PICK_AU) continue;

        rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
        ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
        rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
        if (!mat4_project(vp_camrel, rx, ry, rz, WIN_W, WIN_H, &sx, &sy))
            continue;

        sy_top   = (float)WIN_H - sy;
        dx       = sx - center_x;
        dy       = sy_top - center_y;
        screen_d = sqrtf(dx*dx + dy*dy);
        body_px  = (WIN_H * 0.5f) * info[i].dr
                 / (info[i].dcam * tanf(FOV * 0.5f * (float)(PI / 180.0)) + 1e-9f);
        tol = body_px * 1.35f + 56.0f;
        if (tol < 72.0f)  tol = 72.0f;
        if (tol > 190.0f) tol = 190.0f;
        if (screen_d > tol) continue;

        /* Prefer centred bodies; dcam term breaks ties between overlapping bodies. */
        score = screen_d / tol + info[i].dcam * 0.000001f;
        if (score < best_score) {
            best_score = score;
            best_idx   = i;
        }
    }

    g_inspect_hovered = best_idx;
}

int inspect_begin_orbit(void)
{
    int idx = g_inspect_hovered;

    if (!g_inspect_mode || idx < 0 || idx >= g_nbodies || !g_bodies[idx].alive)
        return 0;

    set_orbit_target_from_camera(idx, 1.45);
    g_inspect_orbit_mode = 1;
    return 1;
}

static int inspect_centering_active(void)
{
    return s_orbit_transition_t < 1.0 || s_look_transition_t < 1.0;
}

void inspect_orbit_mouse(int dx, int dy, float sens_deg_per_px)
{
    double s = (double)sens_deg_per_px * PI / 180.0;
    double axis[3], view_fwd[3], world_up[3] = {0.0, 1.0, 0.0};
    double cam_right[3], len;
    if (!g_inspect_orbit_mode) return;
    if (inspect_centering_active()) return;

    world_orbit_axis(axis);
    rotate_vec_axis(s_orbit_dir, axis, -(double)dx * s);

    view_fwd[0] = -s_orbit_dir[0];
    view_fwd[1] = -s_orbit_dir[1];
    view_fwd[2] = -s_orbit_dir[2];
    cam_right[0] = view_fwd[1]*world_up[2] - view_fwd[2]*world_up[1];
    cam_right[1] = view_fwd[2]*world_up[0] - view_fwd[0]*world_up[2];
    cam_right[2] = view_fwd[0]*world_up[1] - view_fwd[1]*world_up[0];
    len = sqrt(cam_right[0]*cam_right[0] + cam_right[1]*cam_right[1] + cam_right[2]*cam_right[2]);
    if (len < 1e-9) {
        cam_right[0] = 1.0;
        cam_right[1] = 0.0;
        cam_right[2] = 0.0;
    } else {
        cam_right[0] /= len;
        cam_right[1] /= len;
        cam_right[2] /= len;
    }
    rotate_vec_axis(s_orbit_dir, cam_right, -(double)dy * s);
    clamp_orbit_pole(axis);
}

void inspect_orbit_zoom(int wheel_y)
{
    if (!g_inspect_orbit_mode || wheel_y == 0) return;
    if (inspect_centering_active()) return;
    s_orbit_distance *= (wheel_y > 0) ? (1.0 / 1.18) : 1.18;
    if (s_orbit_distance < s_orbit_min_distance)
        s_orbit_distance = s_orbit_min_distance;
    if (s_orbit_distance > s_orbit_max_distance)
        s_orbit_distance = s_orbit_max_distance;
    /* Switch to zoom-smoothing path; mark fly-in transition as done. */
    s_orbit_zoom_smoothing = 1;
    s_orbit_transition_t   = 1.0;
}

void inspect_orbit_update(float dt)
{
    int idx = g_inspect_target;
    double center[3], u;
    if (!g_inspect_orbit_mode || idx < 0 || idx >= g_nbodies || !g_bodies[idx].alive) {
        /* Target was absorbed — follow the survivor if one exists. */
        int survivor = collision_body_absorbed_by(idx);
        if (survivor >= 0 && survivor < g_nbodies && g_bodies[survivor].alive) {
            set_orbit_target_from_camera(survivor, 0.95);
            idx = survivor;
        } else {
            g_inspect_orbit_mode = 0;
            g_inspect_target     = -1;
            return;
        }
    }

    center[0] = g_bodies[idx].pos[0] * RS;
    center[1] = g_bodies[idx].pos[1] * RS;
    center[2] = g_bodies[idx].pos[2] * RS;
    if (s_orbit_zoom_smoothing) {
        double follow = 1.0 - exp(-(double)dt * 14.0);
        if (follow < 0.0) follow = 0.0;
        if (follow > 1.0) follow = 1.0;
        s_orbit_current_distance += (s_orbit_distance - s_orbit_current_distance) * follow;
        if (fabs(s_orbit_current_distance - s_orbit_distance) < s_orbit_distance * 0.0005)
            s_orbit_zoom_smoothing = 0;
    } else if (s_orbit_transition_t < 1.0) {
        s_orbit_transition_t += (double)dt / fmax(s_orbit_transition_duration, 0.001);
        if (s_orbit_transition_t > 1.0) s_orbit_transition_t = 1.0;
        u = smootherstep(s_orbit_transition_t);
        s_orbit_current_distance = visual_distance_lerp(s_orbit_start_distance,
                                                        s_orbit_distance, u);
    } else {
        s_orbit_current_distance = s_orbit_distance;
    }

    g_cam.pos[0] = center[0] + s_orbit_current_distance * s_orbit_dir[0];
    g_cam.pos[1] = center[1] + s_orbit_current_distance * s_orbit_dir[1];
    g_cam.pos[2] = center[2] + s_orbit_current_distance * s_orbit_dir[2];
    smooth_look_at_target(idx, dt);
}

int inspect_ring_params(const BodyRenderInfo *info,
                        float rel[3], float *dr, float *alpha)
{
    int idx;
    if (!g_inspect_mode || !info) return 0;
    idx = g_inspect_orbit_mode ? g_inspect_target : g_inspect_hovered;
    if (idx < 0 || idx >= g_nbodies || !g_bodies[idx].alive) return 0;

    rel[0] = (float)(g_bodies[idx].pos[0] * RS - g_cam.pos[0]);
    rel[1] = (float)(g_bodies[idx].pos[1] * RS - g_cam.pos[1]);
    rel[2] = (float)(g_bodies[idx].pos[2] * RS - g_cam.pos[2]);
    *dr    = info[idx].dr;
    *alpha = g_inspect_orbit_mode ? 0.92f : 0.78f;
    return 1;
}
