/*
 * walk.c — walking on planets (walk.h).
 *
 * The walker lives in the body-local frame (the one the surface and its
 * terrain turn with): a direction from the centre, a radius, and velocities
 * in metres. Each frame the body's current centre and spin carry that into
 * the world, so the walker rides the planet's rotation and orbit however fast
 * the simulation runs, and the floating origin never touches its state.
 *
 * The ground is the surface as drawn (terrain_ground): exact under the
 * camera, where the terrain keeps a read-back patch of the finest tile.
 */
#include "walk.h"
#include "body.h"
#include "camera.h"
#include "collision.h"
#include "terrain.h"

#define EYE_M        1.7    /* eye height above the ground                   */
#define WALK_MS      1.5    /* walking speed                                 */
#define RUN_MS       6.0    /* running speed                                 */
#define JUMP_MS      2.7    /* take-off speed of a standing jump (~0.4 m on  */
                            /* Earth, ~2.3 m on the Moon)                    */
#define STEP_M       0.3    /* ground falling away faster than this per      */
                            /* frame is a ledge: the walker falls off it     */
#define START_FRAMES 240    /* how long a G press waits for terrain below    */

static int          s_on, s_pending, s_body;
static unsigned int s_hash;
static double       s_dir[3];     /* feet direction, body-local, unit      */
static double       s_r;          /* feet radius, m                        */
static double       s_vr;         /* radial velocity, m/s                  */
static double       s_vt[3];      /* tangential velocity, body-local, m/s  */
static double       s_head[3];    /* yaw-0 heading, body-local tangent     */
static double       s_ground;     /* ground radius under the feet, m       */
static int          s_grounded;
static int          s_level;      /* detail level the ground came from     */

static double dot3(const double a[3], const double b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static void cross3(const double a[3], const double b[3], double o[3])
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}

static int norm3(double v[3])
{
    double l = sqrt(dot3(v, v));
    if (l <= 0.0) return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}

static void mul(const double m[9], const double v[3], double o[3])      /* m·v   */
{
    for (int i = 0; i < 3; i++) o[i] = m[i*3]*v[0] + m[i*3+1]*v[1] + m[i*3+2]*v[2];
}

static void mul_t(const double m[9], const double v[3], double o[3])    /* mᵀ·v  */
{
    for (int i = 0; i < 3; i++) o[i] = m[i]*v[0] + m[3+i]*v[1] + m[6+i]*v[2];
}

/* A usable body: alive, solid, and the one we mean (a slot can be reused). */
static const Body *body_ok(int i, unsigned int hash)
{
    if (i < 0 || i >= g_nbodies) return NULL;
    const Body *b = &g_bodies[i];
    if (!b->alive || b->is_star || b->is_black_hole) return NULL;
    if (hash && terrain_name_hash(b->name) != hash) return NULL;
    return b;
}

/* Camera position relative to a body's centre, body-local frame, metres. */
static void cam_local(const Body *b, const double l2w[9], double out[3])
{
    double rel[3];
    for (int k = 0; k < 3; k++) rel[k] = (g_cam.pos[k] - b->pos[k] * RS) * AU;
    mul_t(l2w, rel, out);
}

static double ground_radius(int i, const Body *b, const double dir[3], int *kind, int *level)
{
    double h = 0.0;
    *kind = terrain_ground(i, terrain_name_hash(b->name), dir, &h, level);
    return collision_visual_radius(i, b->radius) * (1.0 + h);
}

/* Heading made tangent to the current up again (transported as we move). */
static void tangent_frame(double e1[3], double e2[3])
{
    double d = dot3(s_head, s_dir);
    for (int k = 0; k < 3; k++) e1[k] = s_head[k] - s_dir[k] * d;
    if (!norm3(e1)) {
        double any[3] = { 1.0, 0.0, 0.0 };
        if (fabs(s_dir[0]) > 0.9) { any[0] = 0.0; any[1] = 1.0; }
        cross3(s_dir, any, e1);
        norm3(e1);
    }
    cross3(e1, s_dir, e2);                  /* rows b0 × b1 = b2, as x × y = z */
    for (int k = 0; k < 3; k++) s_head[k] = e1[k];
}

static void place_camera(const Body *b, const double l2w[9])
{
    double e1[3], e2[3], eye[3], w[3];
    tangent_frame(e1, e2);
    for (int k = 0; k < 3; k++) eye[k] = s_dir[k] * (s_r + EYE_M);
    mul(l2w, eye, w);
    for (int k = 0; k < 3; k++) g_cam.pos[k] = b->pos[k] * RS + w[k] / AU;
    mul(l2w, e1,    &g_cam.basis[0]);
    mul(l2w, s_dir, &g_cam.basis[3]);
    mul(l2w, e2,    &g_cam.basis[6]);
}

static int try_start(void)
{
    int i = g_cam_prox.body;
    const Body *b = body_ok(i, 0);
    if (!b) return 0;
    double l2w[9], loc[3];
    terrain_local_to_world(b->rotation_angle, b->obliquity, l2w);
    cam_local(b, l2w, loc);
    double dir[3] = { loc[0], loc[1], loc[2] };
    if (!norm3(dir)) return 0;
    int kind, level = 0;
    double ground = ground_radius(i, b, dir, &kind, &level);
    if (!kind) return 0;

    s_body = i;
    s_hash = terrain_name_hash(b->name);
    for (int k = 0; k < 3; k++) { s_dir[k] = dir[k]; s_vt[k] = 0.0; }
    s_ground = ground;
    s_level = level;
    s_r = ground;
    s_vr = 0.0;
    s_grounded = 1;

    /* Keep looking where the camera looked: its heading becomes yaw 0 of the
     * local horizon, its elevation the pitch. */
    float f[3];
    cam_get_dir(&f[0], &f[1], &f[2]);
    double fw[3] = { f[0], f[1], f[2] }, fl[3];
    mul_t(l2w, fw, fl);
    for (int k = 0; k < 3; k++) s_head[k] = fl[k];
    double el = asin(fmax(-1.0, fmin(1.0, dot3(fl, s_dir)))) * (180.0 / PI);
    g_cam.yaw   = 0.0f;
    g_cam.pitch = (float)fmax(-89.0, fmin(89.0, el));
    place_camera(b, l2w);
    s_on = 1;

    double R = collision_visual_radius(i, b->radius);
    fprintf(stdout, "[Walk] on %s (g = %.2f m/s^2) - WASD walk, Shift run, E jump, G fly\n",
            b->name, G_CONST * b->mass / (R * R));
    return 1;
}

int walk_active(void) { return s_on; }

void walk_exit(void)
{
    s_pending = 0;
    if (!s_on) return;
    s_on = 0;
    /* Free flight again: the same view direction, as world yaw/pitch. */
    float f[3];
    cam_get_dir(&f[0], &f[1], &f[2]);
    static const double identity[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
    memcpy(g_cam.basis, identity, sizeof identity);
    g_cam.yaw   = (float)(atan2(f[2], f[0]) * (180.0 / PI));
    g_cam.pitch = (float)fmax(-89.0, fmin(89.0, asin(fmax(-1.0, fmin(1.0, (double)f[1]))) * (180.0 / PI)));
    fprintf(stdout, "[Walk] off - free flight\n");
}

void walk_toggle(void)
{
    if (s_on || s_pending) { walk_exit(); return; }
    s_pending = START_FRAMES;
}

/* Free flight: never below an eye height over the ground as drawn (or, until
 * that is read back, over the top of the finest tile there). */
static void limit_camera(void)
{
    int i = g_cam_prox.body;
    const Body *b = body_ok(i, 0);
    if (!b) return;
    double l2w[9], loc[3];
    terrain_local_to_world(b->rotation_angle, b->obliquity, l2w);
    cam_local(b, l2w, loc);
    double r = sqrt(dot3(loc, loc)), dir[3] = { loc[0], loc[1], loc[2] };
    if (!norm3(dir)) return;
    int kind;
    double floor_r = ground_radius(i, b, dir, &kind, NULL) + EYE_M;
    if (!kind || r >= floor_r) return;
    double lift[3], w[3];
    for (int k = 0; k < 3; k++) lift[k] = dir[k] * (floor_r - r);
    mul(l2w, lift, w);
    for (int k = 0; k < 3; k++) g_cam.pos[k] += w[k] / AU;
}

void walk_update(float dt, const WalkInput *in)
{
    if (s_pending && !s_on) {
        if (try_start()) s_pending = 0;
        else if (--s_pending == 0)
            fprintf(stdout, "[Walk] no terrain below - get closer to a solid world\n");
    }
    if (!s_on) { limit_camera(); return; }

    const Body *b = body_ok(s_body, s_hash);
    if (!b) { walk_exit(); return; }
    double l2w[9];
    terrain_local_to_world(b->rotation_angle, b->obliquity, l2w);
    double R = collision_visual_radius(s_body, b->radius);
    double g = G_CONST * b->mass / (R * R);

    double e1[3], e2[3];
    tangent_frame(e1, e2);
    double yaw = g_cam.yaw * (PI / 180.0);
    double fwd[3], right[3];
    for (int k = 0; k < 3; k++) {
        fwd[k]   =  cos(yaw) * e1[k] + sin(yaw) * e2[k];
        right[k] = -sin(yaw) * e1[k] + cos(yaw) * e2[k];
    }

    /* On the ground the legs set the pace; in the air momentum carries. */
    if (s_grounded) {
        double mv[3];
        for (int k = 0; k < 3; k++)
            mv[k] = (in->forward - in->back) * fwd[k] + (in->right - in->left) * right[k];
        double speed = in->run ? RUN_MS : WALK_MS;
        if (!norm3(mv)) speed = 0.0;
        for (int k = 0; k < 3; k++) s_vt[k] = mv[k] * speed;
        if (in->jump) { s_vr = JUMP_MS; s_grounded = 0; }
    } else {
        s_vr -= g * dt;
    }

    double p[3];
    for (int k = 0; k < 3; k++) p[k] = s_dir[k] * (s_r + s_vr * dt) + s_vt[k] * dt;
    s_r = sqrt(dot3(p, p));
    for (int k = 0; k < 3; k++) s_dir[k] = p[k] / s_r;
    /* Keep the tangential velocity tangent on the curved ground. */
    double vd = dot3(s_vt, s_dir);
    for (int k = 0; k < 3; k++) s_vt[k] -= s_dir[k] * vd;

    int kind, level = s_level;
    double ground = ground_radius(s_body, b, s_dir, &kind, &level);
    if (kind == 1 || (kind == 2 && ground < s_ground)) s_ground = ground;

    if (s_grounded) {
        /* A drop is a ledge only on the same surface: when the tile below is
         * refined (streaming in after landing, or as detail follows us) the
         * drawn ground itself moves, and the feet go with it. */
        if (s_r - s_ground > STEP_M && level == s_level) { s_grounded = 0; s_vr = 0.0; }
        else s_r = s_ground;
    }
    s_level = level;
    if (!s_grounded && s_r <= s_ground) {
        s_r = s_ground;
        s_vr = 0.0;
        s_grounded = 1;
    }
    place_camera(b, l2w);
}
