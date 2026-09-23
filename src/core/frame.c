/*
 * frame.c — the floating origin (frame.h).
 */
#include "frame.h"
#include "common.h"
#include "body.h"
#include "camera.h"

double g_frame_origin_au[3] = { 0.0, 0.0, 0.0 };

/* Spacing of doubles at magnitude x. */
static double ulp_of(double x) { return nextafter(fabs(x), INFINITY) - fabs(x); }

/* Rebase once the camera is this far from the origin. 8 AU keeps everything
 * within reach of the camera at <= 1.2e-4 m resolution (the ulp of 1.2e12 m),
 * which is what a millimetre-resolved planet surface needs. A rebase costs one
 * pass over the bodies plus the resident trails: a couple of ms, a few times a
 * second at most even flying through a system. */
#define FRAME_REBASE_AU 8.0

#define FRAME_MAX_LISTENERS 32
static FrameShiftFn s_listeners[FRAME_MAX_LISTENERS];
static int          s_nlisteners = 0;
static FrameShiftFn s_pre[FRAME_MAX_LISTENERS];
static int          s_npre = 0;

static void add_listener(FrameShiftFn *list, int *n, FrameShiftFn fn)
{
    for (int i = 0; i < *n; i++) if (list[i] == fn) return;
    if (*n < FRAME_MAX_LISTENERS) list[(*n)++] = fn;
    else fprintf(stderr, "[frame] too many rebase listeners\n");
}

void frame_on_pre_rebase(FrameShiftFn fn) { add_listener(s_pre, &s_npre, fn); }

void frame_on_rebase(FrameShiftFn fn) { add_listener(s_listeners, &s_nlisteners, fn); }

void frame_cam_sun(double out[3])
{
    for (int k = 0; k < 3; k++) out[k] = g_frame_origin_au[k] + g_cam.pos[k];
}

void frame_sun_to_local_m(const double sun_m[3], double out[3])
{
    for (int k = 0; k < 3; k++) out[k] = sun_m[k] - g_frame_origin_au[k] * AU;
}

void frame_local_to_sun_m(const double local_m[3], double out[3])
{
    for (int k = 0; k < 3; k++) out[k] = local_m[k] + g_frame_origin_au[k] * AU;
}

void frame_rebase(const double delta_au[3])
{
    const double d_m[3] = { delta_au[0] * AU, delta_au[1] * AU, delta_au[2] * AU };
    for (int i = 0; i < s_npre; i++) s_pre[i](d_m);
    for (int k = 0; k < 3; k++) {
        g_frame_origin_au[k] += delta_au[k];
        g_cam.pos[k]         -= delta_au[k];
    }
    for (int i = 0; i < g_nbodies; i++)
        for (int k = 0; k < 3; k++) g_bodies[i].pos[k] -= d_m[k];
    for (int i = 0; i < s_nlisteners; i++) s_listeners[i](d_m);
}

int frame_rebase_if_needed(void)
{
    double r2 = g_cam.pos[0]*g_cam.pos[0] + g_cam.pos[1]*g_cam.pos[1] +
                g_cam.pos[2]*g_cam.pos[2];
    if (r2 < FRAME_REBASE_AU * FRAME_REBASE_AU) return 0;
    double d[3] = { g_cam.pos[0], g_cam.pos[1], g_cam.pos[2] };
    frame_rebase(d);
    return 1;
}

void frame_place_camera_sun(const double sun_au[3])
{
    double d[3];
    for (int k = 0; k < 3; k++) d[k] = (sun_au[k] - g_frame_origin_au[k]) - g_cam.pos[k];
    /* Move the camera there in the local frame, then rebase onto it, so the
     * camera ends at the local origin with nothing lost to rounding. */
    for (int k = 0; k < 3; k++) g_cam.pos[k] += d[k];
    double c[3] = { g_cam.pos[0], g_cam.pos[1], g_cam.pos[2] };
    frame_rebase(c);
}

/* ── self-test (--selftest-frame) ───────────────────────────────────────────
 * Precision where it used to be impossible: a star with a planet 1 AU out,
 * built at M87's distance (16.4 Mpc, 3.4e12 AU), where a Sun-origin double
 * resolves only 6.7e7 m. Then 1000 random rebases, as flying around would
 * cause. The planet-star separation must stay millimetre-exact, and the
 * camera and bodies must keep their Sun-frame positions. */
#include "universe.h"
int frame_selftest(void)
{
    int fail = 0;
#define CHECK(c, ...) do { if (c) fprintf(stdout, "[selftest] ok:   " __VA_ARGS__); \
                           else { fprintf(stdout, "[selftest] FAIL: " __VA_ARGS__); fail++; } \
                           fprintf(stdout, "\n"); } while (0)
    const double m87_au[3] = { -3.244e12, 8.345e11, -1.169e11 };
    frame_place_camera_sun(m87_au);
    CHECK(fabs(g_cam.pos[0]) + fabs(g_cam.pos[1]) + fabs(g_cam.pos[2]) == 0.0,
          "camera at the local origin after placement");

    BodyCreateSpec s;
    memset(&s, 0, sizeof s);
    s.name = "selftest star";  s.mass = 1.989e30;  s.radius = 6.957e8;  s.is_star = 1;
    s.parent = -1;
    s.pos[0] = 1.0e9;  s.pos[1] = -2.0e9;  s.pos[2] = 5.0e8;          /* near the camera */
    int star = universe_add_body(&s);
    memset(&s, 0, sizeof s);
    s.name = "selftest planet";  s.mass = 5.972e24;  s.radius = 6.371e6;  s.parent = star;
    s.pos[0] = g_bodies[star].pos[0] + AU;
    s.pos[1] = g_bodies[star].pos[1];
    s.pos[2] = g_bodies[star].pos[2];
    int planet = universe_add_body(&s);
    CHECK(star >= 0 && planet >= 0, "system created");
    if (star < 0 || planet < 0) return 0;

    double sep0 = g_bodies[planet].pos[0] - g_bodies[star].pos[0];
    double sun0[3];
    frame_local_to_sun_m(g_bodies[star].pos, sun0);
    CHECK(fabs(sep0 - AU) < 1e-3, "planet placed 1 AU out to the mm (error %.3g m; a "
          "Sun-origin double here resolves %.3g m)", fabs(sep0 - AU),
          ulp_of(sqrt(sun0[0]*sun0[0] + sun0[1]*sun0[1] + sun0[2]*sun0[2])));

    unsigned rng = 12345u;
    double cam_sun0[3];
    frame_cam_sun(cam_sun0);
    for (int i = 0; i < 1000; i++) {
        double d[3];
        for (int k = 0; k < 3; k++) {
            rng = rng * 1664525u + 1013904223u;
            d[k] = ((double)(rng >> 8) / 16777216.0 - 0.5) * 200.0;   /* +-100 AU */
        }
        frame_rebase(d);
    }
    double dx = g_bodies[planet].pos[0] - g_bodies[star].pos[0];
    double dy = g_bodies[planet].pos[1] - g_bodies[star].pos[1];
    double dz = g_bodies[planet].pos[2] - g_bodies[star].pos[2];
    double sep = sqrt(dx*dx + dy*dy + dz*dz);
    CHECK(fabs(sep - AU) < 1e-2, "separation after 1000 rebases: error %.3g m", fabs(sep - AU));

    double cs[3], sun1[3];
    frame_cam_sun(cs);
    frame_local_to_sun_m(g_bodies[star].pos, sun1);
    double cam_drift = 0.0, star_drift = 0.0;
    for (int k = 0; k < 3; k++) {
        cam_drift  = fmax(cam_drift,  fabs(cs[k] - cam_sun0[k]) * AU);
        star_drift = fmax(star_drift, fabs(sun1[k] - sun0[k]));
    }
    /* Sun-frame positions are only as good as the Sun-frame double itself. */
    double tol = 64.0 * ulp_of(sqrt(sun0[0]*sun0[0] + sun0[1]*sun0[1] + sun0[2]*sun0[2]));
    CHECK(cam_drift < tol && star_drift < tol, "Sun-frame positions kept (camera %.3g m, "
          "star %.3g m, bound %.3g m)", cam_drift, star_drift, tol);

    g_bodies[star].alive = g_bodies[planet].alive = 0;
#undef CHECK
    fprintf(stdout, "[selftest] frame: %s (%d failure%s)\n", fail ? "FAILED" : "passed",
            fail, fail == 1 ? "" : "s");
    return fail == 0;
}
