/*
 * orbit_predict.h — orbit-prediction "ghost lines" (roadmap Layer 5.4).
 *
 * Draws where a body WILL go, complementary to trails (where it has been).
 * For the selected/inspected body (or a forced target via OMV_PREDICT_BODY),
 * a test particle is forward-integrated under the *active universe's* force
 * laws (g_laws — so force_exp != 2 shows precessing rosettes, lambda/PN too)
 * against its frozen parent chain, then drawn as a camera-relative line strip
 * reusing the trail shaders (solid.vert / solid.frag). The specific orbital
 * energy classifies the path bound (cyan) vs escaping (amber) vs plunging.
 */
#pragma once

/* Result of a prediction (physics only — no GL). */
typedef struct {
    int    valid;
    int    parent;        /* immediate (dominant) parent index, -1 if none */
    int    bound;         /* 1 = bound (specific energy < 0)               */
    int    plunge;        /* 1 = predicted path hit the parent's surface    */
    double period_days;   /* Keplerian period estimate (0 if unbound)       */
    double a_au;          /* semi-major axis (AU)                           */
    double peri_au;       /* periapsis / apoapsis (AU)                      */
    double apo_au;
    double ecc;           /* eccentricity                                   */
    int    count;         /* prediction sample points produced             */
    double ref[3];        /* reference world position (AU) pts are relative */
} OrbitPredictInfo;

void orbit_predict_init(void);
void orbit_predict_render(const float vp_camrel[16]);
void orbit_predict_shutdown(void);

/*
 * Compute a prediction for `body` (no GL, main-thread). Fills `info`. If
 * `pts` is non-NULL, writes up to `max_pts` interleaved [x,y,z,alpha] vertices
 * (ref-relative, AU render-units; alpha ramps 1 at the body → faint into the
 * future) and sets info->count. Returns 1 if a prediction was produced (the
 * body has a parent to orbit and is alive / not a star), 0 otherwise.
 */
int orbit_predict_compute(int body, OrbitPredictInfo *info,
                          float *pts, int max_pts);
