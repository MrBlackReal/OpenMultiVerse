/*
 * physics.c — N-body gravitational physics (RESPA hierarchical integrator)
 *
 * 2R-RESPA split:
 *   Slow forces: primary-primary pairs + non-parent-primary → satellite
 *                (338 pairs for 14 primaries + 19 moons)
 *   Fast forces: parent → satellite dominant force (19 pairs)
 *
 * Each outer step costs  2 × 338  slow pair evaluations.
 * Each inner step costs        19  fast pair evaluations.
 * At 3650 days/s (59 outer × 50 inner):
 *   New:  59×2×338 + 59×51×19  ≈  97 000  pair evaluations / frame
 *   Old:  2920 × 2 × 528       ≈  3 083 000 pair evaluations / frame
 *   Speedup: ~32×
 */
#include "physics.h"
#include "body.h"
#include <math.h>

double g_sim_time  = 0.0;
double g_sim_speed = DAY;
int    g_paused    = 0;

#define OUTER_PERIOD_DIVISOR 24.0
#define INNER_PERIOD_DIVISOR 96.0
#define OUTER_DT_MIN 60.0
#define INNER_DT_MIN 60.0
#define INNER_DT_MAX (DAY * 0.02)
#define OUTER_DT_DEFAULT DAY

static double s_outer_dt_limit = OUTER_DT_DEFAULT;
static double s_inner_dt_limit = INNER_DT_MAX;
static int    s_system_roots[MAX_BODIES];
static double s_system_outer_dt[MAX_BODIES];
static double s_system_inner_dt[MAX_BODIES];
static int    s_nsystems = 0;

static int is_satellite(int i);
static int is_ancestor_of(int ancestor, int child);

static int in_system(int i, int root)
{
    return root < 0 || body_root_star(i) == root;
}

static int ensure_system_slot(int root)
{
    for (int i = 0; i < s_nsystems; i++)
        if (s_system_roots[i] == root) return i;
    if (s_nsystems >= MAX_BODIES) return -1;
    s_system_roots[s_nsystems] = root;
    s_system_outer_dt[s_nsystems] = OUTER_DT_DEFAULT;
    s_system_inner_dt[s_nsystems] = INNER_DT_MAX;
    return s_nsystems++;
}

static int timestep_anchor(int i)
{
    if (i < 0 || i >= g_nbodies || !g_bodies[i].alive) return -1;
    if (g_bodies[i].parent >= 0 && g_bodies[g_bodies[i].parent].alive)
        return g_bodies[i].parent;

    int best = -1;
    double best_score = 1e300;
    for (int j = 0; j < g_nbodies; j++) {
        if (j == i || !g_bodies[j].alive) continue;
        if (g_bodies[j].mass <= 0.0) continue;
        double dx = g_bodies[j].pos[0] - g_bodies[i].pos[0];
        double dy = g_bodies[j].pos[1] - g_bodies[i].pos[1];
        double dz = g_bodies[j].pos[2] - g_bodies[i].pos[2];
        double r2 = dx*dx + dy*dy + dz*dz;
        if (r2 <= 0.0) continue;
        double score = r2 / g_bodies[j].mass;
        if (score < best_score) {
            best_score = score;
            best = j;
        }
    }
    return best;
}

static double estimate_period_about(int i, int anchor)
{
    if (i < 0 || anchor < 0 || i >= g_nbodies || anchor >= g_nbodies) return 0.0;
    if (!g_bodies[i].alive || !g_bodies[anchor].alive) return 0.0;
    double dx = g_bodies[i].pos[0] - g_bodies[anchor].pos[0];
    double dy = g_bodies[i].pos[1] - g_bodies[anchor].pos[1];
    double dz = g_bodies[i].pos[2] - g_bodies[anchor].pos[2];
    double r = sqrt(dx*dx + dy*dy + dz*dz);
    double gm = G_CONST * (g_bodies[i].mass + g_bodies[anchor].mass);
    if (r <= 0.0 || gm <= 0.0) return 0.0;
    return 2.0 * PI * sqrt(r * r * r / gm);
}

static double satellite_perturbation_ratio(int i)
{
    int parent;
    double pdx, pdy, pdz, pr2, parent_acc;
    double max_other_acc = 0.0;

    if (i < 0 || i >= g_nbodies || !is_satellite(i)) return 0.0;
    parent = g_bodies[i].parent;
    if (parent < 0 || parent >= g_nbodies || !g_bodies[parent].alive) return 0.0;

    pdx = g_bodies[parent].pos[0] - g_bodies[i].pos[0];
    pdy = g_bodies[parent].pos[1] - g_bodies[i].pos[1];
    pdz = g_bodies[parent].pos[2] - g_bodies[i].pos[2];
    pr2 = pdx*pdx + pdy*pdy + pdz*pdz + SOFTENING*SOFTENING;
    if (pr2 <= 0.0) return 0.0;
    parent_acc = G_CONST * g_bodies[parent].mass / pr2;
    if (parent_acc <= 0.0) return 0.0;

    for (int j = 0; j < g_nbodies; j++) {
        double dx, dy, dz, r2, acc;
        if (j == i || j == parent || !g_bodies[j].alive) continue;
        if (is_ancestor_of(i, j)) continue;

        dx = g_bodies[j].pos[0] - g_bodies[i].pos[0];
        dy = g_bodies[j].pos[1] - g_bodies[i].pos[1];
        dz = g_bodies[j].pos[2] - g_bodies[i].pos[2];
        r2 = dx*dx + dy*dy + dz*dz + SOFTENING*SOFTENING;
        if (r2 <= 0.0) continue;
        acc = G_CONST * g_bodies[j].mass / r2;
        if (acc > max_other_acc) max_other_acc = acc;
    }

    return max_other_acc / parent_acc;
}

/* ── helpers ─────────────────────────────────────────────────────────── */
/* A body is a satellite (moon) if it has a parent and that parent is not a star.
 * Stars   (parent=-1)          → false  — primary body
 * Planets (parent=star_idx)    → false  — primary body, fast forces not used
 * Moons   (parent=planet_idx)  → true   — handled by fast forces */
static int is_satellite(int i) {
    if (!g_bodies[i].alive) return 0;
    return g_bodies[i].parent >= 0 && !g_bodies[g_bodies[i].parent].is_star;
}

/* Any explicit parent-child pair is integrated at the inner cadence. */
static int has_fast_parent(int i) {
    return is_satellite(i);
}

void physics_refresh_timestep_model(void)
{
    double best_outer = OUTER_DT_DEFAULT;
    double best_inner = INNER_DT_MAX;
    s_nsystems = 0;

    for (int i = 0; i < g_nbodies; i++) {
        if (g_bodies[i].alive && g_bodies[i].is_star)
            ensure_system_slot(i);
    }

    for (int i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        b->dyn_period = 0.0;
        b->dyn_dt_outer = OUTER_DT_DEFAULT;
        b->dyn_dt_inner = INNER_DT_MAX;
        b->dyn_bucket = 0;

        if (!b->alive || b->is_star) continue;

        int anchor = timestep_anchor(i);
        double T = estimate_period_about(i, anchor);
        if (T <= 0.0) continue;

        double dt_outer = T / OUTER_PERIOD_DIVISOR;
        double dt_inner = T / INNER_PERIOD_DIVISOR;
        double perturb = 0.0;
        if (dt_outer < OUTER_DT_MIN) dt_outer = OUTER_DT_MIN;
        if (dt_outer > OUTER_DT_DEFAULT) dt_outer = OUTER_DT_DEFAULT;
        if (dt_inner < INNER_DT_MIN) dt_inner = INNER_DT_MIN;
        if (dt_inner > INNER_DT_MAX) dt_inner = INNER_DT_MAX;

        if (is_satellite(i)) {
            perturb = satellite_perturbation_ratio(i);

            /* Strong third-body forcing means the "slow" component is no longer
             * truly slow for this moon, so tighten the outer cadence toward the
             * inner cadence before the orbit can numerically collapse. */
            if (perturb > 0.20) dt_outer = fmin(dt_outer, dt_inner * 1.5);
            else if (perturb > 0.10) dt_outer = fmin(dt_outer, dt_inner * 2.0);
            else if (perturb > 0.05) dt_outer = fmin(dt_outer, dt_inner * 3.0);
            else if (perturb > 0.02) dt_outer = fmin(dt_outer, dt_inner * 4.0);
            else if (perturb > 0.01) dt_outer = fmin(dt_outer, dt_inner * 6.0);

            if (dt_outer < OUTER_DT_MIN) dt_outer = OUTER_DT_MIN;
        }

        b->dyn_period = T;
        b->dyn_dt_outer = dt_outer;
        b->dyn_dt_inner = dt_inner;
        if (dt_inner <= 10.0 * 60.0) b->dyn_bucket = 3;
        else if (dt_inner <= 60.0 * 60.0) b->dyn_bucket = 2;
        else if (dt_inner <= 6.0 * 60.0 * 60.0) b->dyn_bucket = 1;

        {
            int root = body_root_star(i);
            int slot = ensure_system_slot(root);
            if (slot >= 0) {
                if (dt_outer < s_system_outer_dt[slot]) s_system_outer_dt[slot] = dt_outer;
                if (is_satellite(i) && dt_inner < s_system_inner_dt[slot]) s_system_inner_dt[slot] = dt_inner;
            }
        }

        if (dt_outer < best_outer) best_outer = dt_outer;
        if (is_satellite(i) && dt_inner < best_inner) best_inner = dt_inner;
    }

    s_outer_dt_limit = best_outer;
    s_inner_dt_limit = best_inner;
}

double physics_outer_dt_limit(void)
{
    return s_outer_dt_limit;
}

double physics_inner_dt_limit(void)
{
    return s_inner_dt_limit;
}

int physics_system_count(void)
{
    return s_nsystems;
}

int physics_system_root(int idx)
{
    if (idx < 0 || idx >= s_nsystems) return -1;
    return s_system_roots[idx];
}

double physics_system_outer_dt_limit(int idx)
{
    if (idx < 0 || idx >= s_nsystems) return OUTER_DT_DEFAULT;
    return s_system_outer_dt[idx];
}

double physics_system_inner_dt_limit(int idx)
{
    if (idx < 0 || idx >= s_nsystems) return INNER_DT_MAX;
    return s_system_inner_dt[idx];
}

void physics_advance_time(double dt)
{
    g_sim_time += dt;
}

static int is_ancestor_of(int ancestor, int child) {
    int p = g_bodies[child].parent;
    while (p >= 0) {
        if (p == ancestor) return 1;
        p = g_bodies[p].parent;
    }
    return 0;
}

/* ── slow forces: primary-primary + non-parent tidal on satellites ───── */
static void compute_acc_slow_system(int root) {
    int i, j;
    for (i = 0; i < g_nbodies; i++)
        if (in_system(i, root))
            g_bodies[i].acc[0] = g_bodies[i].acc[1] = g_bodies[i].acc[2] = 0.0;

    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        for (j = 0; j < g_nbodies; j++) {
            int same_system;
            double dx, dy, dz, r2, r, f;

            if (j == i || !g_bodies[j].alive) continue;

            same_system = in_system(j, root);
            if (same_system && j < i) continue;

            /* skip satellite-satellite (negligible and expensive) */
            if (is_satellite(i) && is_satellite(j)) continue;
            /* skip only true moon-parent chains here — planet-star stays slow */
            if (same_system &&
                ((is_satellite(j) && is_ancestor_of(i, j)) ||
                 (is_satellite(i) && is_ancestor_of(j, i)))) continue;

            dx = g_bodies[j].pos[0] - g_bodies[i].pos[0];
            dy = g_bodies[j].pos[1] - g_bodies[i].pos[1];
            dz = g_bodies[j].pos[2] - g_bodies[i].pos[2];
            r2 = dx*dx + dy*dy + dz*dz + SOFTENING*SOFTENING;
            if (G_CONST * g_bodies[j].mass / r2 < GRAV_EPSILON &&
                G_CONST * g_bodies[i].mass / r2 < GRAV_EPSILON) continue;
            r  = sqrt(r2);
            f  = G_CONST / (r2 * r);

            g_bodies[i].acc[0] += f * g_bodies[j].mass * dx;
            g_bodies[i].acc[1] += f * g_bodies[j].mass * dy;
            g_bodies[i].acc[2] += f * g_bodies[j].mass * dz;

            if (same_system) {
                g_bodies[j].acc[0] -= f * g_bodies[i].mass * dx;
                g_bodies[j].acc[1] -= f * g_bodies[i].mass * dy;
                g_bodies[j].acc[2] -= f * g_bodies[i].mass * dz;
            }
        }
    }
}

/* ── fast forces: dominant parent → satellite (+ Newton 3rd reaction) ── */
static void compute_acc_fast_system(int root) {
    int i;
    for (i = 0; i < g_nbodies; i++)
        if (in_system(i, root))
            g_bodies[i].fast_acc[0] = g_bodies[i].fast_acc[1] =
            g_bodies[i].fast_acc[2] = 0.0;

    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        if (!has_fast_parent(i)) continue;
        for (int p = g_bodies[i].parent; p >= 0; p = g_bodies[p].parent) {
            if (!g_bodies[p].alive || !in_system(p, root)) continue;
            double dx = g_bodies[p].pos[0] - g_bodies[i].pos[0];
            double dy = g_bodies[p].pos[1] - g_bodies[i].pos[1];
            double dz = g_bodies[p].pos[2] - g_bodies[i].pos[2];
            double r2 = dx*dx + dy*dy + dz*dz + SOFTENING*SOFTENING;
            if (G_CONST * g_bodies[p].mass / r2 < GRAV_EPSILON) continue;
            double r  = sqrt(r2);
            double f  = G_CONST / (r2 * r);

        /* satellite accelerated toward parent */
            g_bodies[i].fast_acc[0] += f * g_bodies[p].mass * dx;
            g_bodies[i].fast_acc[1] += f * g_bodies[p].mass * dy;
            g_bodies[i].fast_acc[2] += f * g_bodies[p].mass * dz;

        /* reaction on parent (Newton 3rd — small but correct) */
            g_bodies[p].fast_acc[0] -= f * g_bodies[i].mass * dx;
            g_bodies[p].fast_acc[1] -= f * g_bodies[i].mass * dy;
            g_bodies[p].fast_acc[2] -= f * g_bodies[i].mass * dz;
        }
    }
}

/* ── RESPA public API ────────────────────────────────────────────────── */

/*
 * physics_respa_begin — slow half-kick, then pre-compute fast forces
 * for the carry-over into the first inner step.
 */
void physics_respa_begin(double dt_outer) {
    physics_respa_begin_system(-1, dt_outer);
}

void physics_respa_begin_system(int root, double dt_outer) {
    int i;
    compute_acc_slow_system(root);
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].acc[0] * dt_outer;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].acc[1] * dt_outer;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].acc[2] * dt_outer;
    }
    /* Pre-compute fast forces so physics_respa_inner can use them
     * immediately without an extra evaluation (carry-over trick). */
    compute_acc_fast_system(root);
}

/*
 * physics_respa_inner — one inner KDK step using fast forces only.
 * fast_acc must already be valid on entry (set by begin or previous inner).
 * Leaves fast_acc valid for the next call (carry-over).
 */
void physics_respa_inner(double dt_inner) {
    physics_respa_inner_system(-1, dt_inner);
}

void physics_respa_inner_system(int root, double dt_inner) {
    int i;
    /* fast half-kick (uses fast_acc from previous compute_acc_fast) */
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].fast_acc[0] * dt_inner;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].fast_acc[1] * dt_inner;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].fast_acc[2] * dt_inner;
    }
    /* drift all bodies */
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].pos[0] += g_bodies[i].vel[0] * dt_inner;
        g_bodies[i].pos[1] += g_bodies[i].vel[1] * dt_inner;
        g_bodies[i].pos[2] += g_bodies[i].vel[2] * dt_inner;
    }
    /* recompute fast forces at new positions, then fast half-kick */
    compute_acc_fast_system(root);
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].fast_acc[0] * dt_inner;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].fast_acc[1] * dt_inner;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].fast_acc[2] * dt_inner;
    }
}

/*
 * physics_respa_end — slow half-kick at final position, rotation update,
 * sim-time advance.
 */
void physics_respa_end(double dt_outer) {
    physics_respa_end_system(-1, dt_outer);
    physics_advance_time(dt_outer);
}

void physics_respa_end_system(int root, double dt_outer) {
    int i;
    compute_acc_slow_system(root);
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].acc[0] * dt_outer;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].acc[1] * dt_outer;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].acc[2] * dt_outer;
    }
    /* axial rotation */
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || !in_system(i, root)) continue;
        g_bodies[i].rotation_angle = fmod(
            g_bodies[i].rotation_angle + g_bodies[i].rotation_rate * dt_outer,
            2.0 * PI);
    }
}

/* ── legacy single-step KDK (not used in main loop) ─────────────────── */
static void compute_acc(void) {
    int i, j;
    for (i = 0; i < g_nbodies; i++)
        g_bodies[i].acc[0] = g_bodies[i].acc[1] = g_bodies[i].acc[2] = 0.0;
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        for (j = i + 1; j < g_nbodies; j++) {
            if (!g_bodies[j].alive) continue;
            double dx = g_bodies[j].pos[0] - g_bodies[i].pos[0];
            double dy = g_bodies[j].pos[1] - g_bodies[i].pos[1];
            double dz = g_bodies[j].pos[2] - g_bodies[i].pos[2];
            double r2 = dx*dx + dy*dy + dz*dz + SOFTENING*SOFTENING;
            double r  = sqrt(r2);
            double f  = G_CONST / (r2 * r);
            double ai = f * g_bodies[j].mass;
            double aj = f * g_bodies[i].mass;
            g_bodies[i].acc[0] += ai * dx; g_bodies[i].acc[1] += ai * dy; g_bodies[i].acc[2] += ai * dz;
            g_bodies[j].acc[0] -= aj * dx; g_bodies[j].acc[1] -= aj * dy; g_bodies[j].acc[2] -= aj * dz;
        }
    }
}

void physics_step(double dt) {
    int i;
    compute_acc();
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].acc[0] * dt;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].acc[1] * dt;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].acc[2] * dt;
    }
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        g_bodies[i].pos[0] += g_bodies[i].vel[0] * dt;
        g_bodies[i].pos[1] += g_bodies[i].vel[1] * dt;
        g_bodies[i].pos[2] += g_bodies[i].vel[2] * dt;
    }
    compute_acc();
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        g_bodies[i].vel[0] += 0.5 * g_bodies[i].acc[0] * dt;
        g_bodies[i].vel[1] += 0.5 * g_bodies[i].acc[1] * dt;
        g_bodies[i].vel[2] += 0.5 * g_bodies[i].acc[2] * dt;
    }
    for (i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        g_bodies[i].rotation_angle = fmod(
            g_bodies[i].rotation_angle + g_bodies[i].rotation_rate * dt,
            2.0 * PI);
    }
    g_sim_time += dt;
}

/* ── trail helpers ───────────────────────────────────────────────────── */
static void sample_body_pos(Body *b, const double pos[3]) {
    int write_idx = b->trail_head;
    double seg_len = 0.0;
    double target_world_len = TRAIL_TARGET_WORLD_LEN;
    int body_idx = (int)(b - g_bodies);

    if (body_idx >= 0 && body_idx < g_nbodies && is_satellite(body_idx))
        target_world_len = TRAIL_SATELLITE_WORLD_LEN;

    if (b->trail_count > 0) {
        int prev_idx = (b->trail_head - 1 + TRAIL_LEN) % TRAIL_LEN;
        double dx = pos[0] * RS - b->trail[prev_idx][0];
        double dy = pos[1] * RS - b->trail[prev_idx][1];
        double dz = pos[2] * RS - b->trail[prev_idx][2];
        seg_len = sqrt(dx*dx + dy*dy + dz*dz) * AU;
    }

    b->trail[b->trail_head][0] = pos[0] * RS;
    b->trail[b->trail_head][1] = pos[1] * RS;
    b->trail[b->trail_head][2] = pos[2] * RS;
    if (b->trail_seg_len) b->trail_seg_len[write_idx] = seg_len;
    b->trail_head = (b->trail_head + 1) % TRAIL_LEN;
    if (b->trail_count < TRAIL_LEN) {
        b->trail_count++;
        b->trail_total_len += seg_len;
    } else {
        int oldest_idx = b->trail_head;
        int next_oldest_idx = (oldest_idx + 1) % TRAIL_LEN;
        if (b->trail_seg_len)
            b->trail_total_len += seg_len - b->trail_seg_len[next_oldest_idx];
        else
            b->trail_total_len += seg_len;
    }

    while (b->trail_count > 2 && b->trail_total_len > target_world_len) {
        int oldest_idx = (b->trail_head - b->trail_count + TRAIL_LEN) % TRAIL_LEN;
        int next_oldest_idx = (oldest_idx + 1) % TRAIL_LEN;
        double drop_len = b->trail_seg_len ? b->trail_seg_len[next_oldest_idx] : 0.0;
        b->trail_total_len -= drop_len;
        if (b->trail_total_len < 0.0) b->trail_total_len = 0.0;
        b->trail_count--;
    }
}

static double trail_segment_len_for_accel(const Body *b)
{
    double segment_len = TRAIL_BASE_SEGMENT_LEN;
    (void)b;

    if (segment_len < TRAIL_MIN_SEGMENT_LEN) segment_len = TRAIL_MIN_SEGMENT_LEN;
    if (segment_len > TRAIL_MAX_SEGMENT_LEN) segment_len = TRAIL_MAX_SEGMENT_LEN;
    return segment_len;
}

static void trail_curve_eval(const double p0[3], const double v0[3],
                             const double p1[3], const double v1[3],
                             double dt, double t, double out[3])
{
    double t2 = t * t;
    double t3 = t2 * t;
    double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    double h10 = t3 - 2.0 * t2 + t;
    double h01 = -2.0 * t3 + 3.0 * t2;
    double h11 = t3 - t2;

    out[0] = h00 * p0[0] + h10 * dt * v0[0] + h01 * p1[0] + h11 * dt * v1[0];
    out[1] = h00 * p0[1] + h10 * dt * v0[1] + h01 * p1[1] + h11 * dt * v1[1];
    out[2] = h00 * p0[2] + h10 * dt * v0[2] + h01 * p1[2] + h11 * dt * v1[2];
}

static void trail_curve_eval_vel(const double p0[3], const double v0[3],
                                 const double p1[3], const double v1[3],
                                 double dt, double t, double out[3])
{
    double t2 = t * t;
    double dh00 = 6.0 * t2 - 6.0 * t;
    double dh10 = 3.0 * t2 - 4.0 * t + 1.0;
    double dh01 = -6.0 * t2 + 6.0 * t;
    double dh11 = 3.0 * t2 - 2.0 * t;
    double inv_dt = (dt > 0.0) ? (1.0 / dt) : 0.0;

    out[0] = (dh00 * p0[0] + dh10 * dt * v0[0] + dh01 * p1[0] + dh11 * dt * v1[0]) * inv_dt;
    out[1] = (dh00 * p0[1] + dh10 * dt * v0[1] + dh01 * p1[1] + dh11 * dt * v1[1]) * inv_dt;
    out[2] = (dh00 * p0[2] + dh10 * dt * v0[2] + dh01 * p1[2] + dh11 * dt * v1[2]) * inv_dt;
}

static void trail_curve_append_point(double points[][3], int *count,
                                     const double p[3])
{
    if (*count >= (1 << TRAIL_CURVE_MAX_DEPTH) + 1) return;
    points[*count][0] = p[0];
    points[*count][1] = p[1];
    points[*count][2] = p[2];
    (*count)++;
}

static void trail_curve_flatten_recursive(const double p0[3], const double v0[3],
                                          const double p1[3], const double v1[3],
                                          double dt, double max_err, int depth,
                                          double points[][3], int *count)
{
    double mid_curve[3], mid_line[3], diff[3], err;

    trail_curve_eval(p0, v0, p1, v1, dt, 0.5, mid_curve);
    mid_line[0] = 0.5 * (p0[0] + p1[0]);
    mid_line[1] = 0.5 * (p0[1] + p1[1]);
    mid_line[2] = 0.5 * (p0[2] + p1[2]);
    diff[0] = mid_curve[0] - mid_line[0];
    diff[1] = mid_curve[1] - mid_line[1];
    diff[2] = mid_curve[2] - mid_line[2];
    err = sqrt(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);

    if (depth >= TRAIL_CURVE_MAX_DEPTH || err <= max_err) {
        trail_curve_append_point(points, count, p1);
        return;
    }

    {
        double mid_vel[3];
        trail_curve_eval_vel(p0, v0, p1, v1, dt, 0.5, mid_vel);
        trail_curve_flatten_recursive(p0, v0, mid_curve, mid_vel,
                                      dt * 0.5, max_err, depth + 1,
                                      points, count);
        trail_curve_flatten_recursive(mid_curve, mid_vel, p1, v1,
                                      dt * 0.5, max_err, depth + 1,
                                      points, count);
    }
}

static void trail_rebuild_segment(Body *b,
                                  const double start[3], const double start_vel[3],
                                  const double end[3], const double end_vel[3],
                                  double dt, double segment_len, double max_err)
{
    double curve_points[(1 << TRAIL_CURVE_MAX_DEPTH) + 1][3];
    int curve_count = 0;

    trail_curve_append_point(curve_points, &curve_count, start);
    trail_curve_flatten_recursive(start, start_vel, end, end_vel,
                                  dt, max_err, 0,
                                  curve_points, &curve_count);

    for (int k = 1; k < curve_count; k++) {
        double seg_start[3], seg_end[3];
        double dx, dy, dz, seg_dist, traveled;

        seg_start[0] = curve_points[k - 1][0];
        seg_start[1] = curve_points[k - 1][1];
        seg_start[2] = curve_points[k - 1][2];
        seg_end[0] = curve_points[k][0];
        seg_end[1] = curve_points[k][1];
        seg_end[2] = curve_points[k][2];
        dx = seg_end[0] - seg_start[0];
        dy = seg_end[1] - seg_start[1];
        dz = seg_end[2] - seg_start[2];
        seg_dist = sqrt(dx*dx + dy*dy + dz*dz);
        traveled = 0.0;

        while (b->trail_accum + (seg_dist - traveled) >= segment_len) {
            double need = segment_len - b->trail_accum;
            double u;
            double sample_pos[3];

            if (need < 0.0) need = 0.0;
            if (seg_dist <= 0.0) break;
            u = (traveled + need) / seg_dist;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            sample_pos[0] = seg_start[0] + (seg_end[0] - seg_start[0]) * u;
            sample_pos[1] = seg_start[1] + (seg_end[1] - seg_start[1]) * u;
            sample_pos[2] = seg_start[2] + (seg_end[2] - seg_start[2]) * u;
            sample_body_pos(b, sample_pos);
            traveled += need;
            if (traveled > seg_dist) traveled = seg_dist;
            b->trail_accum = 0.0;
        }

        b->trail_accum += seg_dist - traveled;
    }
}

void trails_begin_frame_snapshot(void)
{
    for (int i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        b->trail_frame_accum = b->trail_accum;
        b->trail_frame_head = b->trail_head;
        b->trail_frame_count = b->trail_count;
        b->trail_frame_total_len = b->trail_total_len;
        b->trail_frame_pos[0] = b->pos[0];
        b->trail_frame_pos[1] = b->pos[1];
        b->trail_frame_pos[2] = b->pos[2];
        b->trail_frame_vel[0] = b->vel[0];
        b->trail_frame_vel[1] = b->vel[1];
        b->trail_frame_vel[2] = b->vel[2];
        b->trail_frame_prev_pos[0] = b->trail_prev_pos[0];
        b->trail_frame_prev_pos[1] = b->trail_prev_pos[1];
        b->trail_frame_prev_pos[2] = b->trail_prev_pos[2];
        b->trail_frame_prev_vel[0] = b->trail_prev_vel[0];
        b->trail_frame_prev_vel[1] = b->trail_prev_vel[1];
        b->trail_frame_prev_vel[2] = b->trail_prev_vel[2];
    }
}

void trails_cut_body_at_time(int body_idx, double hit_dt, double frame_dt,
                             const double cut_pos[3])
{
    Body *b;
    double tau, cut_vel[3], segment_len, max_err;
    double dx, dy, dz, dist2;

    if (body_idx < 0 || body_idx >= g_nbodies || !cut_pos) return;
    b = &g_bodies[body_idx];
    if (!b->trail) return;

    b->trail_head = b->trail_frame_head;
    b->trail_count = b->trail_frame_count;
    b->trail_accum = b->trail_frame_accum;
    b->trail_total_len = b->trail_frame_total_len;
    b->trail_prev_pos[0] = b->trail_frame_prev_pos[0];
    b->trail_prev_pos[1] = b->trail_frame_prev_pos[1];
    b->trail_prev_pos[2] = b->trail_frame_prev_pos[2];
    b->trail_prev_vel[0] = b->trail_frame_prev_vel[0];
    b->trail_prev_vel[1] = b->trail_frame_prev_vel[1];
    b->trail_prev_vel[2] = b->trail_frame_prev_vel[2];

    if (frame_dt <= 0.0 || hit_dt <= 0.0) {
        cut_vel[0] = b->trail_frame_prev_vel[0];
        cut_vel[1] = b->trail_frame_prev_vel[1];
        cut_vel[2] = b->trail_frame_prev_vel[2];
    } else {
        tau = hit_dt / frame_dt;
        if (tau < 0.0) tau = 0.0;
        if (tau > 1.0) tau = 1.0;
        trail_curve_eval_vel(b->trail_frame_pos, b->trail_frame_vel,
                             b->pos, b->vel, frame_dt, tau, cut_vel);
        segment_len = trail_segment_len_for_accel(b);
        max_err = segment_len * TRAIL_CURVE_ERROR_RATIO;
        if (max_err < TRAIL_CURVE_MIN_ERROR) max_err = TRAIL_CURVE_MIN_ERROR;
        if (max_err > TRAIL_CURVE_MAX_ERROR) max_err = TRAIL_CURVE_MAX_ERROR;
        trail_rebuild_segment(b,
                              b->trail_frame_prev_pos, b->trail_frame_prev_vel,
                              cut_pos, cut_vel, hit_dt, segment_len, max_err);
    }

    if (b->trail_count > 0) {
        int last_idx = (b->trail_head - 1 + TRAIL_LEN) % TRAIL_LEN;
        dx = b->trail[last_idx][0] - cut_pos[0] * RS;
        dy = b->trail[last_idx][1] - cut_pos[1] * RS;
        dz = b->trail[last_idx][2] - cut_pos[2] * RS;
        dist2 = dx*dx + dy*dy + dz*dz;
        if (dist2 <= (TRAIL_MIN_SEGMENT_LEN * RS) * (TRAIL_MIN_SEGMENT_LEN * RS)) {
            b->trail[last_idx][0] = cut_pos[0] * RS;
            b->trail[last_idx][1] = cut_pos[1] * RS;
            b->trail[last_idx][2] = cut_pos[2] * RS;
        } else {
            sample_body_pos(b, cut_pos);
        }
    } else {
        sample_body_pos(b, cut_pos);
    }

    b->trail_prev_pos[0] = cut_pos[0];
    b->trail_prev_pos[1] = cut_pos[1];
    b->trail_prev_pos[2] = cut_pos[2];
    b->trail_prev_vel[0] = cut_vel[0];
    b->trail_prev_vel[1] = cut_vel[1];
    b->trail_prev_vel[2] = cut_vel[2];
}

void trails_tick(double dt) {
    trails_tick_system(-1, dt);
}

void trails_tick_system(int root, double dt) {
    int i;
    if (dt <= 0.0) return;

    for (i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        double segment_len, max_err, start[3], start_vel[3], end[3], end_vel[3];

        if (!b->alive || !in_system(i, root) || !b->trail || !b->trail_emitting) {
            continue;
        }
        segment_len = trail_segment_len_for_accel(b);
        max_err = segment_len * TRAIL_CURVE_ERROR_RATIO;
        if (max_err < TRAIL_CURVE_MIN_ERROR) max_err = TRAIL_CURVE_MIN_ERROR;
        if (max_err > TRAIL_CURVE_MAX_ERROR) max_err = TRAIL_CURVE_MAX_ERROR;
        start[0] = b->trail_prev_pos[0];
        start[1] = b->trail_prev_pos[1];
        start[2] = b->trail_prev_pos[2];
        start_vel[0] = b->trail_prev_vel[0];
        start_vel[1] = b->trail_prev_vel[1];
        start_vel[2] = b->trail_prev_vel[2];
        end[0] = b->pos[0];
        end[1] = b->pos[1];
        end[2] = b->pos[2];
        end_vel[0] = b->vel[0];
        end_vel[1] = b->vel[1];
        end_vel[2] = b->vel[2];
        trail_rebuild_segment(b, start, start_vel, end, end_vel,
                              dt, segment_len, max_err);

        b->trail_prev_pos[0] = end[0];
        b->trail_prev_pos[1] = end[1];
        b->trail_prev_pos[2] = end[2];
        b->trail_prev_vel[0] = end_vel[0];
        b->trail_prev_vel[1] = end_vel[1];
        b->trail_prev_vel[2] = end_vel[2];
    }
}
