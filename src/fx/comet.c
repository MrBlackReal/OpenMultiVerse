/*
 * comet.c — comet coma + ion/dust tails (roadmap §2.3)
 *
 * Model
 * -----
 * A comet is a normal body (`is_comet`, parsed from `"type": "comet"`): the
 * nucleus takes the ordinary dot/sphere path, physics, labels and Inspect.
 * This module adds the volatile display on top, three additive quads per
 * comet, all camera-relative floats (the standard double-subtract recipe):
 *
 *   coma      — camera-facing radial glow at the nucleus
 *   ion tail  — straight ribbon pointing exactly anti-sunward (solar wind
 *               couples the plasma to the field, not the orbit), blue,
 *               narrow, filamentary
 *   dust tail — a PHYSICAL syndyne fan: every sample is a real dust grain,
 *               back-propagated to its release point on the comet's orbit
 *               and forward-propagated under radiation-pressure-reduced
 *               gravity μ(1−β).  A grid of β (grain size) curves drawn as a
 *               ruled surface spans the fan: big grains lag toward the
 *               orbit, small grains hug the anti-sun line, and the whole
 *               sheet sweeps continuously as the comet rounds perihelion.
 *               NOT a contrail — dust is not exhaust; grains keep orbiting
 *               at nearly the comet's speed and only lag/drift with age.
 *
 * Sublimation intensity is PHYSICAL: activity ramps with the incident flux
 * at the nucleus from the RadianceField (log-scaled between ~30 W/m², i.e.
 * ~6.7 AU from a Sol-class star, and Earth-flux 1361 W/m²), so tails grow on
 * approach and vanish in the outer system with zero authored keyframes.
 * Tail length scales with activity up to ~1 AU.
 *
 * Ribbons face the camera (side vector = tail × view), draw additively with
 * depth test but no depth write, and write nothing themselves — the frag
 * shader depth-tests against the scene via the standard log-depth metric
 * computed per fragment from the interpolated camera-relative position.
 *
 * Tidal fragmentation (§2.3) is NOT implemented here — deferred.
 */
#include "comet.h"
#include "common.h"
#include "body.h"
#include "radiance_field.h"
#include "gl_utils.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Activity ramp: log10(flux) mapped 30 W/m² → 0, 1361 W/m² → 1, capped 1.5
 * inside 1 AU so a sungrazer blazes without blowing out. */
#define COMET_FLUX_LO   30.0
#define COMET_FLUX_HI   1361.0
#define COMET_ACT_CAP   1.5f

/* Tail geometry (AU) at activity 1. */
#define ION_LEN_MAX     0.90f
#define DUST_LEN_MAX    0.55f
#define ION_TIP_W       0.055f
#define DUST_TIP_W      0.16f
#define COMA_R_MAX      0.012f

/* Skip everything past this camera distance (the whole display is sub-pixel
 * long before this; the nucleus dot continues on the normal body path). */
#define COMET_MAX_DIST_AU  60.0

/* Dust-tail syndyne samples (age axis) and grain-size curves (β axis).
 * The fan is drawn as a ruled surface between adjacent β curves. */
#define DUST_SAMPLES_MAX   48
#define DUST_BETAS         7

static GLuint s_shader = 0;
static GLuint s_vao = 0, s_vbo = 0;
static GLint  s_u_vp, s_u_cam_fwd, s_u_kind, s_u_act, s_u_col;
static GLint  s_u_time, s_u_seed, s_u_curve, s_u_gain;
static int    s_ok = 0;

/* Cached comet slot indices: is_comet is only ever set by the universe
 * loader, so scanning all g_nbodies (16k at catalog scale) every frame to
 * find the handful (often zero) of comets was pure waste.  Rebuilt when the
 * loader invalidates it; slot reuse can only *remove* a comet, which the
 * per-entry alive/is_comet check in comet_render handles. */
static int *s_comets   = NULL;
static int  s_n_comets = -1;     /* -1 = stale: rebuild on next render */

void comet_notify_bodies_changed(void) { s_n_comets = -1; }

static void comet_list_ensure(void)
{
    if (s_n_comets >= 0) return;
    s_n_comets = 0;
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !g_bodies[i].is_comet) continue;
        int *grown = realloc(s_comets, (size_t)(s_n_comets + 1) * sizeof(int));
        if (!grown) return;                   /* keep the shorter list */
        s_comets = grown;
        s_comets[s_n_comets++] = i;
    }
}

void comet_init(void)
{
    s_shader = gl_shader_load("assets/shaders/comet.vert",
                              "assets/shaders/comet.frag");
    if (!s_shader) {
        fprintf(stderr, "[comet] shader failed; comets disabled\n");
        return;
    }
    s_u_vp      = glGetUniformLocation(s_shader, "u_vp");
    s_u_cam_fwd = glGetUniformLocation(s_shader, "u_cam_fwd");
    s_u_kind    = glGetUniformLocation(s_shader, "u_kind");
    s_u_act     = glGetUniformLocation(s_shader, "u_act");
    s_u_col     = glGetUniformLocation(s_shader, "u_col");
    s_u_time    = glGetUniformLocation(s_shader, "u_time");
    s_u_seed    = glGetUniformLocation(s_shader, "u_seed");
    s_u_curve   = glGetUniformLocation(s_shader, "u_curve");
    s_u_gain    = glGetUniformLocation(s_shader, "u_gain");

    /* Streaming strip buffer: sized for the dust tail's curved centerline
     * (2 verts per sample) — the simple quads use the first 4 verts. */
    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(2 * DUST_SAMPLES_MAX * 5 * sizeof(float), NULL,
                          GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    s_ok = 1;
}

static void draw_quad(const float v[20])
{
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 20 * sizeof(float), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Two-body Kepler propagation (state → state, dt of either sign, relative
 * to the parent).  Elliptic path is exact; unbound/degenerate states fall
 * back to a drag-free ballistic step (linear + central-gravity quadratic) —
 * good enough for the escaping high-β dust grains that take it.  O(1),
 * independent of trail-buffer state, smooth at any sample spacing. */
static void kepler_prop(const double r0[3], const double v0[3], double mu,
                        double dt, double out_r[3], double out_v[3])
{
    double r  = sqrt(r0[0]*r0[0] + r0[1]*r0[1] + r0[2]*r0[2]);
    double v2 = v0[0]*v0[0] + v0[1]*v0[1] + v0[2]*v0[2];
    double rv = r0[0]*v0[0] + r0[1]*v0[1] + r0[2]*v0[2];
    double a  = (r > 0.0) ? 1.0 / (2.0 / r - v2 / mu) : -1.0;

    double ev[3] = { 0, 0, 0 }, h[3] = { 0, 0, 0 };
    double e = 1.0, hl = 0.0;
    if (a > 0.0 && r > 0.0) {
        ev[0] = ((v2 - mu / r) * r0[0] - rv * v0[0]) / mu;
        ev[1] = ((v2 - mu / r) * r0[1] - rv * v0[1]) / mu;
        ev[2] = ((v2 - mu / r) * r0[2] - rv * v0[2]) / mu;
        e = sqrt(ev[0]*ev[0] + ev[1]*ev[1] + ev[2]*ev[2]);
        h[0] = r0[1]*v0[2] - r0[2]*v0[1];
        h[1] = r0[2]*v0[0] - r0[0]*v0[2];
        h[2] = r0[0]*v0[1] - r0[1]*v0[0];
        hl = sqrt(h[0]*h[0] + h[1]*h[1] + h[2]*h[2]);
    }

    if (!(a > 0.0) || e < 1e-8 || e >= 1.0 || hl < 1e-8 || r <= 0.0) {
        /* Ballistic fallback: straight line bent by the central pull. */
        double g = (r > 1e3) ? -mu / (r * r * r) : 0.0;
        for (int k = 0; k < 3; k++) {
            out_r[k] = r0[k] + v0[k] * dt + 0.5 * g * r0[k] * dt * dt;
            out_v[k] = v0[k] + g * r0[k] * dt;
        }
        return;
    }

    double ph[3] = { ev[0]/e, ev[1]/e, ev[2]/e };
    double wh[3] = { h[0]/hl, h[1]/hl, h[2]/hl };
    double qh[3] = { wh[1]*ph[2] - wh[2]*ph[1],
                     wh[2]*ph[0] - wh[0]*ph[2],
                     wh[0]*ph[1] - wh[1]*ph[0] };

    /* Current eccentric anomaly → mean anomaly, step, re-solve.  The solver
     * must survive e ≈ 0.97 near perihelion, where naive Newton from E = M
     * diverges (this exact failure once scattered the tail into a
     * starburst).  f(E) = E − e·sinE − M is strictly increasing for e < 1
     * and |E − M| ≤ e, so bracketed Newton with bisection fallback always
     * converges. */
    double n_mot = sqrt(mu / (a * a * a));
    double E0 = atan2(rv / (n_mot * a * a), 1.0 - r / a);
    double M  = E0 - e * sin(E0) + n_mot * dt;
    double lo = M - e, hi = M + e;
    double E  = 0.5 * (lo + hi);
    for (int it = 0; it < 24; it++) {
        double f  = E - e * sin(E) - M;
        if (f > 0.0) hi = E; else lo = E;
        double fp = 1.0 - e * cos(E);
        double En = E - f / (fp > 1e-9 ? fp : 1e-9);
        E = (En > lo && En < hi) ? En : 0.5 * (lo + hi);
    }
    double se = sqrt(1.0 - e * e);
    double rE = a * (1.0 - e * cos(E));
    double ca = a * (cos(E) - e);
    double sa = a * se * sin(E);
    double vc = sqrt(mu * a) / rE;
    for (int k = 0; k < 3; k++) {
        out_r[k] = ca * ph[k] + sa * qh[k];
        out_v[k] = vc * (-sin(E) * ph[k] + se * cos(E) * qh[k]);
    }
}

/* Dust-tail centerline: a physical SYNDYNE.  A grain released τ ago does
 * NOT sit at the comet's past position (that would be a contrail — dust is
 * not exhaust): it keeps orbiting at nearly the comet's speed, but under
 * gravity reduced by radiation pressure, μ_eff = μ(1−β).  So each sample is
 * the comet's state τ back, then propagated FORWARD τ under μ(1−β): grains
 * lag the nucleus only slightly while drifting anti-sunward, giving the
 * real tail — mostly anti-sunward, curving toward the orbit with age, and
 * never dragging along the path.  β selects the grain size: one β per
 * centerline, several centerlines fan the tail.  Samples ordered head
 * (comet, t=0) → old end.  Returns 0 if there is no usable parent (caller
 * falls back to a straight ribbon). */
static int dust_centerline(const Body *b, const double cam_pos[3],
                           float max_len_au, double beta,
                           float ctr[DUST_SAMPLES_MAX][3],
                           float t_of[DUST_SAMPLES_MAX])
{
    int par = b->parent;
    if (par < 0 || par >= g_nbodies || !g_bodies[par].alive) return 0;
    const Body *pb = &g_bodies[par];
    double mu = G_CONST * (pb->mass + b->mass);
    if (mu <= 0.0 || beta <= 0.0 || beta >= 1.0) return 0;

    double r0[3] = { b->pos[0] - pb->pos[0], b->pos[1] - pb->pos[1],
                     b->pos[2] - pb->pos[2] };
    double v0[3] = { b->vel[0] - pb->vel[0], b->vel[1] - pb->vel[1],
                     b->vel[2] - pb->vel[2] };
    double r     = sqrt(r0[0]*r0[0] + r0[1]*r0[1] + r0[2]*r0[2]);
    double speed = sqrt(v0[0]*v0[0] + v0[1]*v0[1] + v0[2]*v0[2]);
    if (speed < 1e-3 || r < 1e3) return 0;

    /* March the grain release age in fine steps and TERMINATE the syndyne
     * by grain–nucleus SEPARATION, not by an age formula: near perihelion
     * orbital shear separates grains much faster than the differential-
     * acceleration estimate, and an age-terminated curve sweeps a quarter
     * of the sky.  τ step from the ½·β·(μ/r²)·τ² small-age estimate; the
     * fine march (4× the sample count) keeps sample spacing even in
     * separation, which is also what the brightness taper wants. */
    double L_m     = (double)max_len_au * AU;
    double tau_est = sqrt(2.0 * L_m * r * r / (beta * mu));
    {
        double a = 1.0 / (2.0 / r - speed * speed / mu);
        if (a > 0.0) {
            double period = 2.0 * PI * sqrt(a * a * a / mu);
            if (tau_est > 0.25 * period) tau_est = 0.25 * period;
        }
    }
    const int STEPS = DUST_SAMPLES_MAX * 4;
    double dtau = tau_est / (double)STEPS;

    int n = 0;
    double next_sep = 0.0;
    for (int s = 0; s <= STEPS && n < DUST_SAMPLES_MAX; s++) {
        double tau = s * dtau;
        double rr[3], rv[3], gr[3], gv[3];
        /* Comet state at release, then the grain's own reduced-μ orbit. */
        kepler_prop(r0, v0, mu, -tau, rr, rv);
        kepler_prop(rr, rv, mu * (1.0 - beta), tau, gr, gv);
        double sx = gr[0]-r0[0], sy = gr[1]-r0[1], sz = gr[2]-r0[2];
        double sep = sqrt(sx*sx + sy*sy + sz*sz);
        if (sep < next_sep && s != 0 && s != STEPS) continue;
        next_sep = sep + L_m / (double)(DUST_SAMPLES_MAX - 1);

        double p[3] = { gr[0] + pb->pos[0], gr[1] + pb->pos[1],
                        gr[2] + pb->pos[2] };
        ctr[n][0] = (float)(p[0] * RS - cam_pos[0]);
        ctr[n][1] = (float)(p[1] * RS - cam_pos[1]);
        ctr[n][2] = (float)(p[2] * RS - cam_pos[2]);
        t_of[n]   = (float)(sep / L_m);
        if (t_of[n] > 1.0f) t_of[n] = 1.0f;
        n++;
        if (sep >= L_m) break;
    }
    return n;
}

/* Draw the dust fan as a ruled surface between adjacent β syndynes.
 * The geometry itself spans the fan (no billboard span vectors needed —
 * which also sidesteps the down-tail frame instability that turned
 * per-sample billboard frames into a starburst).  uv: u = separation along
 * the tail, v = grain-size axis −1 (largest, orbit-hugging) → +1 (smallest,
 * anti-sunward), so the fragment gaussian softens the fan edges. */
static void draw_fan(int n, const float fan[DUST_BETAS][DUST_SAMPLES_MAX][3],
                     const float t_of[])
{
    float verts[2 * DUST_SAMPLES_MAX * 5];
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    for (int rrow = 0; rrow < DUST_BETAS - 1; rrow++) {
        float va_v = -1.0f + 2.0f * (float)rrow       / (float)(DUST_BETAS - 1);
        float vb_v = -1.0f + 2.0f * (float)(rrow + 1) / (float)(DUST_BETAS - 1);
        for (int j = 0; j < n; j++) {
            float *va = verts + (size_t)(2*j)     * 5;
            float *vb = verts + (size_t)(2*j + 1) * 5;
            for (int k = 0; k < 3; k++) {
                va[k] = fan[rrow][j][k];
                vb[k] = fan[rrow + 1][j][k];
            }
            va[3] = t_of[j]; va[4] = va_v;
            vb[3] = t_of[j]; vb[4] = vb_v;
        }
        glBufferData(GL_ARRAY_BUFFER, (size_t)(2*n) * 5 * sizeof(float),
                     verts, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 2 * n);
    }
}

/* Camera-facing span vector for a ribbon at `base` along `dir`:
 * side = dir × view(base), normalised. */
static void ribbon_side(float side[3], const float base[3], const float dir[3])
{
    side[0] = dir[1] * base[2] - dir[2] * base[1];
    side[1] = dir[2] * base[0] - dir[0] * base[2];
    side[2] = dir[0] * base[1] - dir[1] * base[0];
    float sl = sqrtf(side[0]*side[0] + side[1]*side[1] + side[2]*side[2]);
    if (sl < 1e-12f) { side[0] = 1.0f; side[1] = side[2] = 0.0f; sl = 1.0f; }
    side[0] /= sl; side[1] /= sl; side[2] /= sl;
}

/* Ribbon from `base` along `dir` for `len`, half-widths w0 (base) → w1 (tip),
 * spanned by `side`.  uv: u along the tail 0→1, v across −1→1. */
static void ribbon_verts(float v[20], const float base[3], const float dir[3],
                         const float side[3], float len, float w0, float w1)
{
    const float tip[3] = { base[0] + dir[0] * len,
                           base[1] + dir[1] * len,
                           base[2] + dir[2] * len };
    for (int k = 0; k < 3; k++) {
        v[0  + k] = base[k] - side[k] * w0;   /* u=0 v=-1 */
        v[5  + k] = base[k] + side[k] * w0;   /* u=0 v=+1 */
        v[10 + k] = tip[k]  - side[k] * w1;   /* u=1 v=-1 */
        v[15 + k] = tip[k]  + side[k] * w1;   /* u=1 v=+1 */
    }
    v[3]  = 0.0f; v[4]  = -1.0f;
    v[8]  = 0.0f; v[9]  =  1.0f;
    v[13] = 1.0f; v[14] = -1.0f;
    v[18] = 1.0f; v[19] =  1.0f;
}

void comet_render(const float vp_camrel[16],
                  const float cam_right[3], const float cam_up[3],
                  const float cam_fwd[3], const double cam_pos[3],
                  float time)
{
    if (!s_ok) return;

    comet_list_ensure();
    int begun = 0;
    for (int ci = 0; ci < s_n_comets; ci++) {
        int i = s_comets[ci];
        const Body *b = &g_bodies[i];
        if (!b->alive || !b->is_comet) continue;

        /* Camera-relative position, double subtract then float (AU). */
        double rxd = b->pos[0] * RS - cam_pos[0];
        double ryd = b->pos[1] * RS - cam_pos[1];
        double rzd = b->pos[2] * RS - cam_pos[2];
        double dist = sqrt(rxd*rxd + ryd*ryd + rzd*rzd);
        if (dist > COMET_MAX_DIST_AU) continue;

        /* Sublimation: incident flux at the nucleus → activity. */
        RadianceContrib top[1];
        if (radiance_field_top(b->pos, i, 1, top) < 1 || top[0].irr <= 0.0)
            continue;
        float act = (float)((log10(top[0].irr)      - log10(COMET_FLUX_LO)) /
                            (log10(COMET_FLUX_HI)   - log10(COMET_FLUX_LO)));
        if (act <= 0.02f) continue;                 /* frozen in the deep   */
        if (act > COMET_ACT_CAP) act = COMET_ACT_CAP;

        /* Anti-sunward unit vector (tail direction), from the emitter that
         * actually dominates here — a comet rounding a foreign sun points
         * away from THAT sun. */
        float anti[3] = { (float)((b->pos[0] - top[0].pos[0]) * RS),
                          (float)((b->pos[1] - top[0].pos[1]) * RS),
                          (float)((b->pos[2] - top[0].pos[2]) * RS) };
        float al = sqrtf(anti[0]*anti[0] + anti[1]*anti[1] + anti[2]*anti[2]);
        if (al < 1e-9f) continue;
        anti[0] /= al; anti[1] /= al; anti[2] /= al;

        /* Dust lags along the orbit: blend anti-sun toward −velocity. */
        float dust_dir[3];
        {
            double vl = sqrt(b->vel[0]*b->vel[0] + b->vel[1]*b->vel[1] +
                             b->vel[2]*b->vel[2]);
            float bv[3] = { 0, 0, 0 };
            if (vl > 1e-6) {
                bv[0] = (float)(-b->vel[0] / vl);
                bv[1] = (float)(-b->vel[1] / vl);
                bv[2] = (float)(-b->vel[2] / vl);
            }
            for (int k = 0; k < 3; k++)
                dust_dir[k] = anti[k] * 0.78f + bv[k] * 0.22f;
            float dl = sqrtf(dust_dir[0]*dust_dir[0] + dust_dir[1]*dust_dir[1]
                             + dust_dir[2]*dust_dir[2]);
            dust_dir[0] /= dl; dust_dir[1] /= dl; dust_dir[2] /= dl;
        }

        float base[3] = { (float)rxd, (float)ryd, (float)rzd };

        if (!begun) {
            glUseProgram(s_shader);
            glUniformMatrix4fv(s_u_vp, 1, GL_FALSE, vp_camrel);
            glUniform3f(s_u_cam_fwd, cam_fwd[0], cam_fwd[1], cam_fwd[2]);
            glUniform1f(s_u_time, time);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glBindVertexArray(s_vao);
            begun = 1;
        }
        glUniform1f(s_u_seed, (float)(i % 512) * 0.217f);

        float v[20];
        float ion_len  = ION_LEN_MAX  * act;
        float dust_len = DUST_LEN_MAX * act;

        /* Each tail draws as CROSSED planes (camera-facing + perpendicular)
         * at reduced gain — a single flat quad shows its silhouette as a
         * hard wedge whenever the camera nears its plane (worst looking
         * straight down the tail). */
        float s1[3], s2[3];

        /* Dust tail first (broad, behind), then ion, then the coma on top.
         * The centerline is the syndyne curve built from the body's real
         * trail history, so the tail bends with the orbit and visibly
         * sweeps as the comet rounds perihelion.  Straight-ribbon fallback
         * only when there is no history yet (first frames after load). */
        glUniform1i(s_u_kind, 2);
        glUniform1f(s_u_act,  act);
        glUniform3f(s_u_col,  1.00f, 0.93f, 0.80f);
        glUniform1f(s_u_curve, 0.0f);
        {
            /* The fan IS the grain-size spread: small grains (high β) hug
             * the anti-sun line, large grains (low β) lag toward the orbit.
             * A grid of syndynes spans it — each row a Kepler-exact
             * centerline, drawn as a ruled surface so the fan is one
             * continuous sheet of dust, not discrete ribbons. */
            static const double BETAS[DUST_BETAS] =
                { 0.09, 0.14, 0.21, 0.32, 0.46, 0.63, 0.80 };
            static float fan[DUST_BETAS][DUST_SAMPLES_MAX][3];
            float t_of[DUST_SAMPLES_MAX];
            int   n_min = DUST_SAMPLES_MAX;
            int   ok = 1;
            for (int s = 0; s < DUST_BETAS && ok; s++) {
                int n = dust_centerline(b, cam_pos, dust_len, BETAS[s],
                                        fan[s], t_of);
                if (n < 2) ok = 0;
                else if (n < n_min) n_min = n;
            }
            if (ok) {
                glUniform1f(s_u_gain, 0.55f);
                glUniform1f(s_u_seed, (float)(i % 512) * 0.217f);
                draw_fan(n_min, fan, t_of);
            } else {
                glUniform1f(s_u_gain, 0.62f);
                ribbon_side(s1, base, dust_dir);
                ribbon_verts(v, base, dust_dir, s1, dust_len,
                             COMA_R_MAX * (0.4f + 0.6f * act),
                             DUST_TIP_W * act);
                draw_quad(v);
            }
        }

        glUniform1i(s_u_kind, 1);
        glUniform3f(s_u_col,  0.42f, 0.60f, 1.00f);
        glUniform1f(s_u_curve, 0.0f);
        ribbon_side(s1, base, anti);
        s2[0] = anti[1]*s1[2] - anti[2]*s1[1];
        s2[1] = anti[2]*s1[0] - anti[0]*s1[2];
        s2[2] = anti[0]*s1[1] - anti[1]*s1[0];
        ribbon_verts(v, base, anti, s1, ion_len,
                     COMA_R_MAX * (0.25f + 0.45f * act),
                     ION_TIP_W * act);
        draw_quad(v);
        ribbon_verts(v, base, anti, s2, ion_len,
                     COMA_R_MAX * (0.25f + 0.45f * act),
                     ION_TIP_W * act);
        draw_quad(v);

        /* Coma: camera-facing billboard. */
        glUniform1i(s_u_kind, 0);
        glUniform1f(s_u_gain, 1.0f);
        {
            float r = COMA_R_MAX * (0.25f + 0.75f * act);
            for (int c = 0; c < 4; c++) {
                float su = (c & 1) ? 1.0f : -1.0f;
                float sv = (c & 2) ? 1.0f : -1.0f;
                v[c*5+0] = base[0] + (cam_right[0]*su + cam_up[0]*sv) * r;
                v[c*5+1] = base[1] + (cam_right[1]*su + cam_up[1]*sv) * r;
                v[c*5+2] = base[2] + (cam_right[2]*su + cam_up[2]*sv) * r;
                v[c*5+3] = su; v[c*5+4] = sv;
            }
            draw_quad(v);
        }
    }

    if (begun) {
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUseProgram(0);
    }
}

void comet_shutdown(void)
{
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    s_vbo = s_vao = s_shader = 0;
    s_ok = 0;
    free(s_comets); s_comets = NULL; s_n_comets = -1;
}
