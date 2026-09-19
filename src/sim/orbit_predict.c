/*
 * orbit_predict.c — see orbit_predict.h.
 *
 * The predicted path is a test particle forward-integrated (velocity-Verlet)
 * under the ACTIVE universe's force laws against the target's frozen parent
 * chain. Using the same laws_pair_factor kernel + softening + cosmological/PN
 * terms as physics.c means the ghost matches where the sim will actually take
 * the body — including non-Newtonian precession (force_exp != 2) and PN rosettes
 * — which a static analytic ellipse could not represent. Render reuses the trail
 * shaders and the trail's double-precision camera-relative offset scheme.
 */
#include "orbit_predict.h"
#include "body.h"
#include "laws.h"
#include "common.h"       /* RS, AU, DAY, PI, SOFTENING, G_CONST, MAX_BODIES */
#include "camera.h"       /* g_cam */
#include "settings.h"     /* g_settings.orbit_predict */
#include "inspect.h"      /* g_inspect_mode / _orbit_mode / _target / _hovered */
#include "gl_utils.h"
#include <GL/glew.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define ORBIT_PREDICT_MAX_PTS 512
#define ORBIT_PREDICT_STEPS   3000   /* integration substeps across the span */
#define ORBIT_MAX_CHAIN       8      /* parent-chain attractor cap           */

static GLuint s_shader     = 0;
static GLint  s_loc_vp     = -1;
static GLint  s_loc_color  = -1;
static GLint  s_loc_offset = -1;
static GLuint s_vao = 0, s_vbo = 0;

/* Forced target (headless / demo): OMV_PREDICT_BODY=<name>. Resolved lazily. */
static char s_forced_name[32] = {0};
static int  s_forced_idx      = -1;

/* Acceleration on a test particle at (pp, vv), from the frozen attractor chain,
 * matching physics.c: softened inverse-(force_exp) gravity + cosmological (about
 * the root) + 1PN precession (about the dominant parent). */
static void predict_accel(const double pp[3], const double vv[3],
                          const int *chain, int nc, int root, double out[3])
{
    out[0] = out[1] = out[2] = 0.0;

    for (int k = 0; k < nc; k++) {
        const Body *a = &g_bodies[chain[k]];
        double dx = a->pos[0] - pp[0];
        double dy = a->pos[1] - pp[1];
        double dz = a->pos[2] - pp[2];
        double r2 = dx*dx + dy*dy + dz*dz + SOFTENING * SOFTENING;
        double r  = sqrt(r2);
        double f  = laws_pair_factor(r2, r);   /* per-universe force exponent */
        out[0] += f * a->mass * dx;
        out[1] += f * a->mass * dy;
        out[2] += f * a->mass * dz;
    }

    if (g_laws.lambda != 0.0 && root >= 0) {
        out[0] += g_laws.lambda * (pp[0] - g_bodies[root].pos[0]);
        out[1] += g_laws.lambda * (pp[1] - g_bodies[root].pos[1]);
        out[2] += g_laws.lambda * (pp[2] - g_bodies[root].pos[2]);
    }

    if (g_laws.pn_factor != 0.0) {
        const Body *p = &g_bodies[chain[0]];
        double rx = pp[0] - p->pos[0], ry = pp[1] - p->pos[1], rz = pp[2] - p->pos[2];
        double vx = vv[0] - p->vel[0], vy = vv[1] - p->vel[1], vz = vv[2] - p->vel[2];
        double r2 = rx*rx + ry*ry + rz*rz + SOFTENING * SOFTENING;
        double Lx = ry*vz - rz*vy, Ly = rz*vx - rx*vz, Lz = rx*vy - ry*vx;
        double L2 = Lx*Lx + Ly*Ly + Lz*Lz;
        double gm = g_laws.G * p->mass;
        double c2 = g_laws.c_light * g_laws.c_light;
        double k  = -g_laws.pn_factor * 3.0 * gm * L2 / (c2 * r2 * r2 * sqrt(r2));
        out[0] += k * rx; out[1] += k * ry; out[2] += k * rz;
    }
}

int orbit_predict_compute(int body, OrbitPredictInfo *info,
                          float *pts, int max_pts)
{
    memset(info, 0, sizeof(*info));
    info->parent = -1;
    if (body < 0 || body >= g_nbodies) return 0;
    const Body *b = &g_bodies[body];
    if (!b->alive || b->is_star) return 0;

    /* Attractor chain: parent, grandparent, ... up to the root star. */
    int chain[ORBIT_MAX_CHAIN], nc = 0;
    for (int p = b->parent;
         p >= 0 && p < g_nbodies && g_bodies[p].alive && nc < ORBIT_MAX_CHAIN;
         p = g_bodies[p].parent)
        chain[nc++] = p;
    if (nc == 0) return 0;                 /* a root/star has no orbit to draw */
    int root = chain[nc - 1];
    const Body *par = &g_bodies[chain[0]];
    double mu = G_CONST * par->mass;
    if (mu <= 0.0) return 0;
    info->parent = chain[0];

    /* Osculating classification relative to the dominant parent (vis-viva). */
    double rx = b->pos[0] - par->pos[0], ry = b->pos[1] - par->pos[1], rz = b->pos[2] - par->pos[2];
    double vx = b->vel[0] - par->vel[0], vy = b->vel[1] - par->vel[1], vz = b->vel[2] - par->vel[2];
    double r0 = sqrt(rx*rx + ry*ry + rz*rz);
    double v2 = vx*vx + vy*vy + vz*vz;
    if (r0 <= 0.0) return 0;
    double eps = 0.5 * v2 - mu / r0;
    info->bound = (eps < 0.0);

    double Tsec;
    if (info->bound) {
        double a = -mu / (2.0 * eps);
        /* eccentricity vector e = ((v²-mu/r)·r - (r·v)·v)/mu */
        double rdotv = rx*vx + ry*vy + rz*vz;
        double c1 = (v2 - mu / r0) / mu, c2 = rdotv / mu;
        double ex = c1*rx - c2*vx, ey = c1*ry - c2*vy, ez = c1*rz - c2*vz;
        double e = sqrt(ex*ex + ey*ey + ez*ez);
        info->a_au = a * RS;
        info->ecc = e;
        info->peri_au = a * (1.0 - e) * RS;
        info->apo_au  = a * (1.0 + e) * RS;
        Tsec = 2.0 * PI * sqrt(a*a*a / mu);
        info->period_days = Tsec / DAY;
    } else {
        /* Unbound: span from the local dynamical time at r0. */
        Tsec = 2.0 * PI * sqrt(r0*r0*r0 / mu);
        info->period_days = 0.0;
    }

    /* Bound: predict ~3 orbits — a closed Newtonian ellipse just retraces the
     * same line (clean), while a precessing orbit (force_exp != 2, or PN) fans
     * into a visible rosette. Unbound: a longer fixed multiple so the escape
     * hyperbola reads. */
    double span = info->bound ? 3.0 * Tsec : 6.0 * Tsec;
    if (!(span > 0.0)) span = 1.0e7;

    int cap = max_pts < 2 ? 2 : max_pts;
    if (cap > ORBIT_PREDICT_MAX_PTS) cap = ORBIT_PREDICT_MAX_PTS;
    int rec_every = ORBIT_PREDICT_STEPS / (cap - 1);
    if (rec_every < 1) rec_every = 1;
    double dt = span / (double)ORBIT_PREDICT_STEPS;

    double pp[3] = { b->pos[0], b->pos[1], b->pos[2] };
    double vv[3] = { b->vel[0], b->vel[1], b->vel[2] };
    info->ref[0] = b->pos[0] * RS;
    info->ref[1] = b->pos[1] * RS;
    info->ref[2] = b->pos[2] * RS;

    double acc[3];
    predict_accel(pp, vv, chain, nc, root, acc);
    int count = 0;
    double parent_rad = par->radius;

    for (int s = 0; s <= ORBIT_PREDICT_STEPS; s++) {
        if ((s % rec_every) == 0 && count < cap) {
            if (pts) {
                float alpha = 0.15f + 0.85f * (1.0f - (float)s / (float)ORBIT_PREDICT_STEPS);
                pts[count*4+0] = (float)(pp[0] * RS - info->ref[0]);
                pts[count*4+1] = (float)(pp[1] * RS - info->ref[1]);
                pts[count*4+2] = (float)(pp[2] * RS - info->ref[2]);
                pts[count*4+3] = alpha;
            }
            count++;
        }
        /* velocity Verlet (KDK) */
        vv[0] += 0.5*dt*acc[0]; vv[1] += 0.5*dt*acc[1]; vv[2] += 0.5*dt*acc[2];
        pp[0] += dt*vv[0];      pp[1] += dt*vv[1];      pp[2] += dt*vv[2];
        predict_accel(pp, vv, chain, nc, root, acc);
        vv[0] += 0.5*dt*acc[0]; vv[1] += 0.5*dt*acc[1]; vv[2] += 0.5*dt*acc[2];

        double ddx = pp[0] - par->pos[0], ddy = pp[1] - par->pos[1], ddz = pp[2] - par->pos[2];
        double dr2 = ddx*ddx + ddy*ddy + ddz*ddz;
        if (dr2 < parent_rad * parent_rad) { info->plunge = 1; break; }
        if (!info->bound && dr2 > (60.0*r0)*(60.0*r0)) break;   /* escaped */
    }

    info->count = count;
    info->valid = 1;
    return 1;
}

/* Resolve the active target: forced env body, else the inspect selection. */
static int select_target(void)
{
    if (s_forced_name[0]) {
        if (s_forced_idx < 0 || s_forced_idx >= g_nbodies ||
            !g_bodies[s_forced_idx].alive ||
            strcmp(g_bodies[s_forced_idx].name, s_forced_name) != 0) {
            s_forced_idx = -1;
            for (int i = 0; i < g_nbodies; i++)
                if (g_bodies[i].alive && !strcmp(g_bodies[i].name, s_forced_name)) {
                    s_forced_idx = i; break;
                }
        }
        return s_forced_idx;
    }
    if (!g_inspect_mode) return -1;
    return g_inspect_orbit_mode ? g_inspect_target : g_inspect_hovered;
}

void orbit_predict_render(const float vp_camrel[16])
{
    if (!s_shader || !g_settings.orbit_predict) return;

    int body = select_target();
    if (body < 0 || body >= g_nbodies ||
        !g_bodies[body].alive || g_bodies[body].is_star) return;

    static float pts[ORBIT_PREDICT_MAX_PTS * 4];
    OrbitPredictInfo info;
    if (!orbit_predict_compute(body, &info, pts, ORBIT_PREDICT_MAX_PTS)) return;
    if (info.count < 2) return;

    glUseProgram(s_shader);
    glUniformMatrix4fv(s_loc_vp, 1, GL_FALSE, vp_camrel);

    /* Camera subtraction in double, cast the residual (trail convention). */
    float off[3] = {
        (float)(info.ref[0] - g_cam.pos[0]),
        (float)(info.ref[1] - g_cam.pos[1]),
        (float)(info.ref[2] - g_cam.pos[2])
    };
    glUniform3fv(s_loc_offset, 1, off);

    float r, g, b;
    if (info.plunge)       { r = 1.0f; g = 0.35f; b = 0.20f; }  /* plunging: red   */
    else if (!info.bound)  { r = 1.0f; g = 0.65f; b = 0.20f; }  /* escaping: amber */
    else                   { r = 0.35f; g = 0.80f; b = 1.00f; } /* bound: cyan     */
    glUniform4f(s_loc_color, r, g, b, 0.9f);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, info.count * 4 * sizeof(float), pts);
    glDrawArrays(GL_LINE_STRIP, 0, info.count);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

void orbit_predict_init(void)
{
    s_shader = gl_shader_load("assets/shaders/solid.vert",
                              "assets/shaders/solid.frag");
    if (!s_shader) return;
    s_loc_vp     = glGetUniformLocation(s_shader, "u_vp");
    s_loc_color  = glGetUniformLocation(s_shader, "u_color");
    s_loc_offset = glGetUniformLocation(s_shader, "u_body_offset");

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(ORBIT_PREDICT_MAX_PTS * 4 * sizeof(float),
                          NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    const char *e = getenv("OMV_PREDICT_BODY");
    if (e && e[0]) { strncpy(s_forced_name, e, 31); s_forced_name[31] = '\0'; }
    s_forced_idx = -1;
}

void orbit_predict_shutdown(void)
{
    if (s_vbo) { glDeleteBuffers(1, &s_vbo);       s_vbo = 0; }
    if (s_vao) { glDeleteVertexArrays(1, &s_vao);  s_vao = 0; }
    if (s_shader) { glDeleteProgram(s_shader);     s_shader = 0; }
}
