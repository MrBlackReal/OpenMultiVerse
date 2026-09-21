/*
 * cinema_blur.c — full vs camera-only motion blur (see cinema_blur.h).
 */
#include "cinema_blur.h"
#include "cinema_cam.h"
#include "camera.h"
#include "body.h"
#include "laws.h"
#include "settings.h"
#include "universe.h"
#include "asteroids.h"
#include "rings.h"

#include <stdio.h>

#define BLUR_PX_THRESHOLD 0.5   /* below this, a smear is invisible */

static long s_frames, s_object_frames;

/* Screen-space motion (px) of something at camera-relative r (AU) moving at
 * v (m/s, already relative to the tracked frame) for sim_dt seconds. */
static double screen_px(const double r[3], const double v[3], double sim_dt,
                        double px_per_rad)
{
    double d = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (d <= 0.0) return 0.0;
    double u[3] = { r[0] / d, r[1] / d, r[2] / d };
    double vr = v[0]*u[0] + v[1]*u[1] + v[2]*u[2];
    double p[3] = { v[0] - vr*u[0], v[1] - vr*u[1], v[2] - vr*u[2] };
    return sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) * sim_dt * RS / d * px_per_rad;
}

int cinema_blur_objects_needed(double sim_open_s)
{
    s_frames++;
    if (sim_open_s <= 0.0) return 0;

    float fx, fy, fz;
    cam_get_dir(&fx, &fy, &fz);
    const float fwd[3] = { fx, fy, fz };
    double aspect = (double)WIN_W / (double)(WIN_H > 0 ? WIN_H : 1);
    double tanh_ = tan(g_settings.fov * 0.5 * (PI / 180.0));
    double px_per_rad = (double)WIN_H / (2.0 * tanh_);
    /* In view, with a margin: a body just off-frame can smear into it. */
    double half_diag = atan(tanh_ * sqrt(1.0 + aspect * aspect));
    double cos_lim = cos(fmin(half_diag + 5.0 * (PI / 180.0), PI));

    /* What the camera rides moves with it, so only motion relative to it can
     * smear: the current key's anchor body, else a fixed frame. */
    double ref[3] = { 0.0, 0.0, 0.0 };
    int anchor = cinema_shot_playing() ? cinema_shot_anchor_body(cinema_shot_time()) : -1;
    if (anchor >= 0) {
        ref[0] = g_bodies[anchor].vel[0];
        ref[1] = g_bodies[anchor].vel[1];
        ref[2] = g_bodies[anchor].vel[2];
    }

    for (int i = 0; i < g_nbodies; i++) {
        if (i == g_field_star_begin && g_field_star_end > g_field_star_begin) {
            i = g_field_star_end - 1;           /* frozen scenery never moves */
            continue;
        }
        const Body *b = &g_bodies[i];
        if (!b->alive || i == anchor) continue;
        double r[3] = { b->pos[0] * RS - g_cam.pos[0],
                        b->pos[1] * RS - g_cam.pos[1],
                        b->pos[2] * RS - g_cam.pos[2] };
        double d = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
        if (d <= 0.0) continue;
        if ((r[0]*fwd[0] + r[1]*fwd[1] + r[2]*fwd[2]) / d < cos_lim) continue;
        double v[3] = { b->vel[0] - ref[0], b->vel[1] - ref[1], b->vel[2] - ref[2] };
        if (screen_px(r, v, sim_open_s, px_per_rad) > BLUR_PX_THRESHOLD) goto needed;

        /* A ringed body: its particles orbit it at sqrt(GM/r) at the inner
         * edge, on top of the body's own motion. */
        float rin, rout, pole[3];
        if (rings_query(i, &rin, &rout, pole) && rin > 0.0f && b->mass > 0.0) {
            /* Direction-free upper bound: orbit plus body speed, all of it
             * across the line of sight. */
            double vorb = sqrt(g_laws.G * b->mass / ((double)rin * AU));
            double vmag = vorb + sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (vmag * sim_open_s * RS / d * px_per_rad > BLUR_PX_THRESHOLD) goto needed;
        }
    }

    if (asteroids_max_screen_px(ref, sim_open_s, fwd, cos_lim, px_per_rad) > BLUR_PX_THRESHOLD)
        goto needed;
    return 0;

needed:
    s_object_frames++;
    return 1;
}

void cinema_blur_report(void)
{
    if (s_frames <= 0) return;
    fprintf(stdout, "[Cinematic] motion blur: %ld/%ld frames needed object blur "
            "(the rest were camera-only, sim advanced once)\n",
            s_object_frames, s_frames);
}
