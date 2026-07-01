/*
 * supernova.c - generic star-star supernova events.
 *
 * This module owns the full non-persistent lifecycle of a stellar collision
 * aftermath:
 *
 *   trigger path
 *     collision.c detects a star-star impact and calls supernova_try_trigger()
 *
 *   simulation path
 *     the two source stars are retired, a remnant star is inserted, nearby
 *     bodies may be destroyed immediately by the flash, and later receive a
 *     one-shot outward kick once the shock front reaches them
 *
 *   rendering path
 *     render.c asks supernova_render_events() for camera-relative radii and
 *     intensities that drive the dedicated volumetric shaders
 *
 * The event itself intentionally stays anchored at the birth location of the
 * explosion. The remnant body can move away physically, but the visible cloud
 * and shock timings continue to reference the original detonation point.
 */
#include "supernova.h"
#include "body.h"
#include "accretion.h"
#include "collision.h"
#include "labels.h"
#include "trails.h"
#include "universe.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SOLAR_MASS 1.98847e30
#define FLASH_DURATION (DAY * 0.90)
#define CORE_DURATION  (DAY * 7.0)
#define CLOUD_DURATION (DAY * 320.0)
#define SHOCK_SPEED_BASE 9.0e6
#define CLOUD_SPEED_BASE 5.5e6
#define FLASH_ENERGY_BASE 9.0e43

typedef struct {
    int active;
    int star_a;
    int star_b;
    int remnant_idx;
    double age;
    double pos[3];
    double vel[3];
    double flash_energy;
    double total_mass;
    double remnant_mass;
    double ejecta_mass;
    double shock_speed;
    double cloud_speed;
    double initial_radius;
    float color[3];
    float seed;
    unsigned char *push_done;
    int push_done_cap;
    /* Root-orbiting bodies the shock can ever reach (within the maximum blast
     * radius), collected once at detonation so supernova_step iterates only this
     * set rather than re-scanning all bodies every frame. push_done stays indexed
     * by body index; cand holds the body indices to visit. */
    int *cand;
    int  n_cand;
} SupernovaEvent;

static SupernovaEvent s_events[SUPERNOVA_MAX_EVENTS];

static double clampd_local(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double smoothstepd_local(double edge0, double edge1, double x)
{
    double t;
    if (edge0 == edge1) return x >= edge1 ? 1.0 : 0.0;
    t = (x - edge0) / (edge1 - edge0);
    t = clampd_local(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static double rise_and_fall(double age, double rise_seconds, double decay_seconds)
{
    double rise;
    double decay;

    if (age <= 0.0) return 0.0;
    if (rise_seconds <= 1e-9) rise_seconds = 1e-9;
    if (decay_seconds <= 1e-9) decay_seconds = 1e-9;

    rise = 1.0 - exp(-age / rise_seconds);
    decay = exp(-age / decay_seconds);
    return rise * decay;
}

/* ---- Subtree traversal via a cached child index (CSR) --------------------
 * Bodies carry a parent index but no child list, so finding a body's
 * descendants meant scanning all N bodies per query — O(N^2) when many bodies
 * are kicked by one blast. Instead derive a child index (CSR: offsets + a flat
 * pool) once and walk only the subtree. It is rebuilt lazily: invalidated each
 * frame, then (re)built on the first subtree op that frame and reused for the
 * rest, so frames with no kicks pay nothing. */
static int *s_child_off   = NULL;   /* CSR offsets, length nodes+1            */
static int *s_child_pool  = NULL;   /* CSR child indices, length nodes        */
static int *s_trav        = NULL;   /* subtree-collection work buffer (nodes) */
static int  s_child_nodes = 0;
static int  s_child_valid = 0;

static void children_invalidate(void) { s_child_valid = 0; }

static void children_free(void)
{
    free(s_child_off);  s_child_off  = NULL;
    free(s_child_pool); s_child_pool = NULL;
    free(s_trav);       s_trav       = NULL;
    s_child_nodes = 0;
    s_child_valid = 0;
}

/* (Re)build the child index from current parent pointers. Returns 0 on OOM. */
static int children_ensure(void)
{
    int n = g_nbodies;
    int *cur;

    if (s_child_valid && s_child_nodes == n) return 1;
    if (n <= 0) { s_child_nodes = n; s_child_valid = 1; return 1; }

    {
        int *off  = (int*)realloc(s_child_off,  (size_t)(n + 1) * sizeof(int));
        int *pool = (int*)realloc(s_child_pool, (size_t)n * sizeof(int));
        int *trav = (int*)realloc(s_trav,       (size_t)n * sizeof(int));
        if (!off || !pool || !trav) return 0;
        s_child_off = off; s_child_pool = pool; s_trav = trav;
    }

    for (int i = 0; i <= n; i++) s_child_off[i] = 0;
    for (int i = 0; i < n; i++) {
        int p = g_bodies[i].parent;
        if (p >= 0 && p < n) s_child_off[p + 1]++;
    }
    for (int i = 0; i < n; i++) s_child_off[i + 1] += s_child_off[i];

    cur = (int*)malloc((size_t)n * sizeof(int));
    if (!cur) return 0;
    for (int i = 0; i < n; i++) cur[i] = s_child_off[i];
    for (int i = 0; i < n; i++) {
        int p = g_bodies[i].parent;
        if (p >= 0 && p < n) s_child_pool[cur[p]++] = i;
    }
    free(cur);

    s_child_nodes = n;
    s_child_valid = 1;
    return 1;
}

/* Collect root + all of its descendants into s_trav; returns the count (>=1),
 * or 0 if the index is unavailable. Assumes an acyclic parent hierarchy (each
 * body appears once), like the original descendant walk. */
static int subtree_collect(int root)
{
    int head = 0, top = 0;
    if (!children_ensure()) return 0;
    if (root < 0 || root >= s_child_nodes) return 0;
    s_trav[top++] = root;
    while (head < top) {
        int node = s_trav[head++];
        for (int c = s_child_off[node]; c < s_child_off[node + 1]; c++)
            s_trav[top++] = s_child_pool[c];
    }
    return top;
}

/* Root-orbiting bodies are planets / moons whose top-level motion should react
 * to the blast, but stars themselves are excluded from the generic push path. */
static int body_is_root_orbiting_body(int body_idx)
{
    if (body_idx < 0 || body_idx >= g_nbodies) return 0;
    if (!g_bodies[body_idx].alive || g_bodies[body_idx].is_star) return 0;
    return g_bodies[body_idx].parent < 0 ||
           g_bodies[g_bodies[body_idx].parent].is_star;
}

/* Sum a root body plus all of its descendants so shock pushes move the whole
 * local subtree together instead of tearing moons away from their parent. */
static double body_subtree_mass(int root_idx)
{
    double total = 0.0;
    int cnt, k;
    if (root_idx < 0 || root_idx >= g_nbodies) return 0.0;
    cnt = subtree_collect(root_idx);
    if (cnt <= 0)   /* index unavailable: fall back to the root alone */
        return g_bodies[root_idx].alive ? g_bodies[root_idx].mass : 0.0;
    for (k = 0; k < cnt; k++) {
        int i = s_trav[k];
        if (g_bodies[i].alive) total += g_bodies[i].mass;
    }
    return total;
}

static void supernova_event_release(SupernovaEvent *e)
{
    if (!e) return;
    free(e->push_done);
    e->push_done = NULL;
    e->push_done_cap = 0;
    free(e->cand);
    e->cand = NULL;
    e->n_cand = 0;
}

/*
 * supernova_build_candidates — collect the root-orbiting bodies the shock front
 * can ever reach, once, at detonation.
 *
 * The shock radius grows as shock_speed * age and the event ends at
 * age == CLOUD_DURATION, so the maximum reach is shock_speed * CLOUD_DURATION —
 * a function of progenitor mass (shock_speed scales with it). Any body farther
 * than that can never be kicked, so it is excluded permanently. The blast is
 * anchored in world space while bodies drift during the (long) event, so the
 * scan radius is padded generously; the per-frame cost is then proportional to
 * the bodies actually near the blast, not the whole universe. Requires e->pos
 * and e->shock_speed to be set first.
 */
static int supernova_build_candidates(SupernovaEvent *e)
{
    double r_max  = e->shock_speed * CLOUD_DURATION;
    double r_scan = r_max * 1.5 + 50.0 * AU;   /* pad for body motion over event */
    double r2 = r_scan * r_scan;
    int n = 0;

    free(e->cand);
    e->cand = NULL;
    e->n_cand = 0;
    if (g_nbodies <= 0) return 1;

    e->cand = (int*)malloc((size_t)g_nbodies * sizeof(int));
    if (!e->cand) return 0;

    for (int i = 0; i < g_nbodies; i++) {
        double dx, dy, dz;
        if (!body_is_root_orbiting_body(i)) continue;
        dx = g_bodies[i].pos[0] - e->pos[0];
        dy = g_bodies[i].pos[1] - e->pos[1];
        dz = g_bodies[i].pos[2] - e->pos[2];
        if (dx*dx + dy*dy + dz*dz <= r2)
            e->cand[n++] = i;
    }
    e->n_cand = n;
    return 1;
}

/* push_done tracks which root-orbiting bodies have already received their
 * one-shot shock kick. Keep it sized to the current body count. */
static int supernova_event_ensure_push_capacity(SupernovaEvent *e, int needed)
{
    unsigned char *resized;
    int new_cap;

    if (!e) return 0;
    if (needed <= 0) return 1;
    if (e->push_done_cap >= needed && e->push_done) return 1;

    new_cap = needed;
    resized = (unsigned char*)realloc(e->push_done, (size_t)new_cap * sizeof(unsigned char));
    if (!resized) return 0;

    if (new_cap > e->push_done_cap)
        memset(resized + e->push_done_cap, 0, (size_t)(new_cap - e->push_done_cap));

    e->push_done = resized;
    e->push_done_cap = new_cap;
    return 1;
}

static void apply_velocity_to_subtree(int root_idx, const double dv[3])
{
    int cnt, k;
    if (!dv) return;
    cnt = subtree_collect(root_idx);
    if (cnt <= 0) {   /* index unavailable: kick the root alone */
        if (root_idx >= 0 && root_idx < g_nbodies && g_bodies[root_idx].alive) {
            g_bodies[root_idx].vel[0] += dv[0];
            g_bodies[root_idx].vel[1] += dv[1];
            g_bodies[root_idx].vel[2] += dv[2];
        }
        return;
    }
    for (k = 0; k < cnt; k++) {
        int i = s_trav[k];
        if (!g_bodies[i].alive) continue;
        g_bodies[i].vel[0] += dv[0];
        g_bodies[i].vel[1] += dv[1];
        g_bodies[i].vel[2] += dv[2];
    }
}

/* Stop stale labels / trails from continuing to represent consumed stars. */
static void disable_body_visuals(int body_idx)
{
    if (body_idx < 0 || body_idx >= g_nbodies) return;
    g_bodies[body_idx].trail_emitting = 0;
    g_bodies[body_idx].trail_accum = 0.0;
    labels_remove_body(body_idx);
}

/* Destroy a top-level survivor and any moons or sub-bodies attached to it. */
static void destroy_subtree(int root_idx)
{
    int cnt = subtree_collect(root_idx), k;
    if (cnt <= 0) {   /* index unavailable: destroy the root alone */
        if (root_idx >= 0 && root_idx < g_nbodies && g_bodies[root_idx].alive) {
            g_bodies[root_idx].alive = 0;
            g_bodies[root_idx].mass = 0.0;
            disable_body_visuals(root_idx);
        }
        return;
    }
    for (k = 0; k < cnt; k++) {
        int i = s_trav[k];
        if (!g_bodies[i].alive) continue;
        g_bodies[i].alive = 0;
        g_bodies[i].mass = 0.0;
        disable_body_visuals(i);
    }
}

static int alloc_event_slot(void)
{
    for (int i = 0; i < SUPERNOVA_MAX_EVENTS; i++) {
        if (!s_events[i].active) return i;
    }
    return -1;
}

/* Reconstruct the barycentric impact center at the swept hit time rather than
 * using the already-advanced end-of-step positions. */
static void event_center_from_pair(int a, int b, double hit_t, double frame_dt,
                                   double out_pos[3], double out_vel[3])
{
    double total = g_bodies[a].mass + g_bodies[b].mass;
    double back_dt = 0.0;
    double pos_a[3], pos_b[3];
    if (total <= 0.0) total = 1.0;

    if (frame_dt > 0.0) {
        back_dt = frame_dt - hit_t;
        back_dt = clampd_local(back_dt, 0.0, frame_dt);
    }

    pos_a[0] = g_bodies[a].pos[0] - g_bodies[a].vel[0] * back_dt;
    pos_a[1] = g_bodies[a].pos[1] - g_bodies[a].vel[1] * back_dt;
    pos_a[2] = g_bodies[a].pos[2] - g_bodies[a].vel[2] * back_dt;
    pos_b[0] = g_bodies[b].pos[0] - g_bodies[b].vel[0] * back_dt;
    pos_b[1] = g_bodies[b].pos[1] - g_bodies[b].vel[1] * back_dt;
    pos_b[2] = g_bodies[b].pos[2] - g_bodies[b].vel[2] * back_dt;

    out_pos[0] = (pos_a[0] * g_bodies[a].mass + pos_b[0] * g_bodies[b].mass) / total;
    out_pos[1] = (pos_a[1] * g_bodies[a].mass + pos_b[1] * g_bodies[b].mass) / total;
    out_pos[2] = (pos_a[2] * g_bodies[a].mass + pos_b[2] * g_bodies[b].mass) / total;

    out_vel[0] = (g_bodies[a].vel[0] * g_bodies[a].mass +
                  g_bodies[b].vel[0] * g_bodies[b].mass) / total;
    out_vel[1] = (g_bodies[a].vel[1] * g_bodies[a].mass +
                  g_bodies[b].vel[1] * g_bodies[b].mass) / total;
    out_vel[2] = (g_bodies[a].vel[2] * g_bodies[a].mass +
                  g_bodies[b].vel[2] * g_bodies[b].mass) / total;
}

/* Apply the prompt flash kill test immediately at detonation time. This is a
 * coarse survivability heuristic rather than a full physical ablation model. */
static void immediate_flash_effects(const SupernovaEvent *e)
{
    if (!e) return;

    for (int i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        double dx, dy, dz, dist2, dist, fluence, bind_energy;

        if (!body_is_root_orbiting_body(i)) continue;
        if (i == e->remnant_idx) continue;

        dx = b->pos[0] - e->pos[0];
        dy = b->pos[1] - e->pos[1];
        dz = b->pos[2] - e->pos[2];
        dist2 = dx*dx + dy*dy + dz*dz;
        if (dist2 <= 1.0) dist2 = 1.0;
        dist = sqrt(dist2);

        fluence = e->flash_energy * (b->radius * b->radius) / (4.0 * dist2);
        bind_energy = 3.0 * G_CONST * b->mass * b->mass /
                      (5.0 * fmax(b->radius, 1.0));

        if (dist <= e->initial_radius * 8.0 || fluence >= bind_energy * 1.10)
            destroy_subtree(i);
    }
}

void supernova_reset(void)
{
    for (int i = 0; i < SUPERNOVA_MAX_EVENTS; i++)
        supernova_event_release(&s_events[i]);
    memset(s_events, 0, sizeof(s_events));
    children_free();
}

int supernova_try_trigger(int star_a, int star_b, double rel_speed,
                          double hit_t, double frame_dt)
{
    BodyCreateSpec spec;
    SupernovaEvent *e;
    double total_mass, remnant_mass, ejecta_mass;
    double pos[3], vel[3];
    double mass_scale, initial_radius, remnant_radius;
    int slot, remnant_idx;
    char name[32];

    if (star_a < 0 || star_b < 0 || star_a >= g_nbodies || star_b >= g_nbodies)
        return 0;
    if (star_a == star_b) return 0;
    if (!g_bodies[star_a].alive || !g_bodies[star_b].alive) return 0;
    if (!g_bodies[star_a].is_star || !g_bodies[star_b].is_star) return 0;

    for (int i = 0; i < SUPERNOVA_MAX_EVENTS; i++) {
        if (!s_events[i].active) continue;
        if (s_events[i].star_a == star_a || s_events[i].star_a == star_b ||
            s_events[i].star_b == star_a || s_events[i].star_b == star_b)
            return 0;
    }

    slot = alloc_event_slot();
    if (slot < 0) return 0;

    e = &s_events[slot];
    supernova_event_release(e);
    memset(e, 0, sizeof(*e));
    if (!supernova_event_ensure_push_capacity(e, g_nbodies + 1))
        return 0;

    total_mass = g_bodies[star_a].mass + g_bodies[star_b].mass;
    if (total_mass <= 0.0) return 0;

    event_center_from_pair(star_a, star_b, hit_t, frame_dt, pos, vel);
    mass_scale = total_mass / SOLAR_MASS;
    if (mass_scale < 0.5) mass_scale = 0.5;

    /* The remnant/ejecta split is intentionally stylized: plausible enough to
     * read like a stellar catastrophe without simulating compact-object classes
     * or detailed nucleosynthesis outcomes. */
    remnant_mass = total_mass * 0.72;
    remnant_mass = clampd_local(remnant_mass, 1.10 * SOLAR_MASS,
                                fmin(total_mass * 0.92, 3.2 * SOLAR_MASS));
    if (remnant_mass >= total_mass) remnant_mass = total_mass * 0.88;
    if (remnant_mass <= 0.0) remnant_mass = total_mass * 0.60;
    ejecta_mass = total_mass - remnant_mass;
    if (ejecta_mass <= 0.0) ejecta_mass = total_mass * 0.12;

    initial_radius = fmax(g_bodies[star_a].radius, g_bodies[star_b].radius);
    if (initial_radius < 0.045 * AU) initial_radius = 0.045 * AU;
    remnant_radius = fmax(1.1e8, fmax(g_bodies[star_a].radius, g_bodies[star_b].radius) * 0.18);

    memset(&spec, 0, sizeof(spec));
    snprintf(name, sizeof(name), "Remnant %d", g_nbodies + 1);
    spec.name = name;
    spec.mass = remnant_mass;
    spec.radius = remnant_radius;
    spec.pos[0] = pos[0];
    spec.pos[1] = pos[1];
    spec.pos[2] = pos[2];
    spec.vel[0] = vel[0];
    spec.vel[1] = vel[1];
    spec.vel[2] = vel[2];
    spec.col[0] = 0.82f;
    spec.col[1] = 0.90f;
    spec.col[2] = 1.00f;
    spec.is_star = 1;
    spec.parent = -1;
    spec.rotation_rate = (g_bodies[star_a].rotation_rate + g_bodies[star_b].rotation_rate) * 0.5;
    spec.obliquity = (g_bodies[star_a].obliquity + g_bodies[star_b].obliquity) * 0.5;

    remnant_idx = universe_add_body(&spec);
    if (remnant_idx < 0) {
        supernova_event_release(e);
        memset(e, 0, sizeof(*e));
        return 0;
    }
    trails_add_body(remnant_idx);
    labels_add_body(remnant_idx);
    collision_on_body_added(remnant_idx);

    /* Any planets/moons previously owned by the source stars are rebound to the
     * newly spawned remnant so the existing system hierarchy keeps working. */
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || i == remnant_idx) continue;
        if (g_bodies[i].parent == star_a || g_bodies[i].parent == star_b)
            g_bodies[i].parent = remnant_idx;
    }

    disable_body_visuals(star_a);
    disable_body_visuals(star_b);
    g_bodies[star_a].alive = 0;
    g_bodies[star_b].alive = 0;
    g_bodies[star_a].mass = 0.0;
    g_bodies[star_b].mass = 0.0;

    e->active = 1;
    e->star_a = star_a;
    e->star_b = star_b;
    e->remnant_idx = remnant_idx;
    e->age = 0.0;
    e->pos[0] = pos[0];
    e->pos[1] = pos[1];
    e->pos[2] = pos[2];
    e->vel[0] = vel[0];
    e->vel[1] = vel[1];
    e->vel[2] = vel[2];
    e->flash_energy = FLASH_ENERGY_BASE * pow(mass_scale, 1.18);
    e->total_mass = total_mass;
    e->remnant_mass = remnant_mass;
    e->ejecta_mass = ejecta_mass;
    e->shock_speed = SHOCK_SPEED_BASE * pow(mass_scale, 0.08);
    e->cloud_speed = CLOUD_SPEED_BASE * pow(mass_scale, 0.05);
    e->initial_radius = initial_radius;
    e->color[0] = spec.col[0];
    e->color[1] = spec.col[1];
    e->color[2] = spec.col[2];
    e->seed = (float)(((star_a + 1) * 17 + (star_b + 3) * 31) % 997) / 997.0f;
    supernova_build_candidates(e);

    children_invalidate();   /* body set just changed (remnant added, stars retired) */
    immediate_flash_effects(e);

    fprintf(stderr,
            "[supernova] %s + %s -> %s (rel=%.0f m/s, ejecta=%.2f Msun)\n",
            g_bodies[star_a].name, g_bodies[star_b].name, g_bodies[remnant_idx].name,
            rel_speed, ejecta_mass / SOLAR_MASS);
    return 1;
}

int supernova_detonate(int star_idx)
{
    BodyCreateSpec spec;
    SupernovaEvent *e;
    double total_mass, remnant_mass, ejecta_mass, msun, mass_scale;
    double pos[3], vel[3], initial_radius, remnant_radius, violence;
    int slot, remnant_idx, remnant_phase, remnant_is_bh = 0;
    float rcol[3];
    char name[32];

    if (star_idx < 0 || star_idx >= g_nbodies) return 0;
    if (!g_bodies[star_idx].alive || !g_bodies[star_idx].is_star) return 0;
    if (g_bodies[star_idx].is_black_hole) return 0;

    /* Don't stack a second event on a star already mid-detonation. */
    for (int i = 0; i < SUPERNOVA_MAX_EVENTS; i++) {
        if (!s_events[i].active) continue;
        if (s_events[i].star_a == star_idx || s_events[i].star_b == star_idx)
            return 0;
    }

    total_mass = g_bodies[star_idx].mass;
    if (total_mass <= 0.0) return 0;

    slot = alloc_event_slot();
    if (slot < 0) return 0;
    e = &s_events[slot];
    supernova_event_release(e);
    memset(e, 0, sizeof(*e));
    if (!supernova_event_ensure_push_capacity(e, g_nbodies + 1))
        return 0;

    msun = total_mass / SOLAR_MASS;
    mass_scale = msun < 0.5 ? 0.5 : msun;

    pos[0] = g_bodies[star_idx].pos[0];
    pos[1] = g_bodies[star_idx].pos[1];
    pos[2] = g_bodies[star_idx].pos[2];
    vel[0] = g_bodies[star_idx].vel[0];
    vel[1] = g_bodies[star_idx].vel[1];
    vel[2] = g_bodies[star_idx].vel[2];

    /* Remnant class by progenitor mass. Stylized, not a full compact-object
     * model: enough to read as a distinct outcome. */
    if (msun >= 20.0) {
        remnant_is_bh  = 1;
        remnant_phase  = STAR_BLACK_HOLE_REMNANT;
        remnant_mass   = clampd_local(msun * 0.30, 5.0, 25.0) * SOLAR_MASS;
        remnant_radius = fmax(g_bodies[star_idx].radius * 1.5, 0.02 * AU);
        rcol[0] = 0.85f; rcol[1] = 0.92f; rcol[2] = 1.00f;
        violence = 1.0;
    } else if (msun >= 8.0) {
        remnant_phase  = STAR_NEUTRON_STAR;
        remnant_mass   = 1.4 * SOLAR_MASS;
        remnant_radius = 2.0e7;            /* visibility, not 12 km reality */
        rcol[0] = 0.78f; rcol[1] = 0.88f; rcol[2] = 1.00f;
        violence = 0.9;
    } else {
        /* Low mass: gentle planetary-nebula puff leaving a white dwarf. */
        remnant_phase  = STAR_WHITE_DWARF;
        remnant_mass   = clampd_local(total_mass * 0.55, 0.30 * SOLAR_MASS,
                                      1.20 * SOLAR_MASS);
        remnant_radius = 7.0e6;           /* ~Earth-sized */
        rcol[0] = 0.85f; rcol[1] = 0.90f; rcol[2] = 1.00f;
        violence = 0.15;
    }
    if (remnant_mass >= total_mass) remnant_mass = total_mass * 0.5;
    ejecta_mass = total_mass - remnant_mass;
    if (ejecta_mass <= 0.0) ejecta_mass = total_mass * 0.1;

    initial_radius = fmax(g_bodies[star_idx].radius, 0.045 * AU);

    memset(&spec, 0, sizeof(spec));
    snprintf(name, sizeof(name), "%.22s Remnant", g_bodies[star_idx].name);
    spec.name = name;
    spec.mass = remnant_mass;
    spec.radius = remnant_radius;
    spec.pos[0] = pos[0]; spec.pos[1] = pos[1]; spec.pos[2] = pos[2];
    spec.vel[0] = vel[0]; spec.vel[1] = vel[1]; spec.vel[2] = vel[2];
    spec.col[0] = rcol[0]; spec.col[1] = rcol[1]; spec.col[2] = rcol[2];
    spec.is_star = 1;
    spec.parent = -1;
    spec.rotation_rate = g_bodies[star_idx].rotation_rate;
    spec.obliquity = g_bodies[star_idx].obliquity;

    remnant_idx = universe_add_body(&spec);
    if (remnant_idx < 0) {
        supernova_event_release(e);
        memset(e, 0, sizeof(*e));
        return 0;
    }
    trails_add_body(remnant_idx);
    labels_add_body(remnant_idx);
    collision_on_body_added(remnant_idx);

    g_bodies[remnant_idx].is_black_hole = remnant_is_bh;
    g_bodies[remnant_idx].star_phase    = remnant_phase;
    g_bodies[remnant_idx].base_radius   = remnant_radius;
    /* A freshly collapsed hole is fed by the infalling stellar envelope — give
     * it an accretion disk (the raymarched BH pass draws it). */
    if (remnant_is_bh) {
        g_bodies[remnant_idx].accretion_disk = 1.0f;
        /* Clean accretion state for the reused slot (quiet: no authored
         * activity → empty reservoir; a real fallback-fed model is future work). */
        accretion_init_body(&g_bodies[remnant_idx]);
    }
    g_bodies[remnant_idx].base_col[0]   = rcol[0];
    g_bodies[remnant_idx].base_col[1]   = rcol[1];
    g_bodies[remnant_idx].base_col[2]   = rcol[2];

    /* Rebind the dying star's planets/moons onto the remnant. */
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive || i == remnant_idx) continue;
        if (g_bodies[i].parent == star_idx)
            g_bodies[i].parent = remnant_idx;
    }

    disable_body_visuals(star_idx);
    g_bodies[star_idx].alive      = 0;
    g_bodies[star_idx].mass       = 0.0;
    g_bodies[star_idx].star_phase = STAR_DEAD;

    e->active       = 1;
    e->star_a       = star_idx;
    e->star_b       = -1;
    e->remnant_idx  = remnant_idx;
    e->age          = 0.0;
    e->pos[0] = pos[0]; e->pos[1] = pos[1]; e->pos[2] = pos[2];
    e->vel[0] = vel[0]; e->vel[1] = vel[1]; e->vel[2] = vel[2];
    e->flash_energy = FLASH_ENERGY_BASE * pow(mass_scale, 1.18) * violence;
    e->total_mass   = total_mass;
    e->remnant_mass = remnant_mass;
    e->ejecta_mass  = ejecta_mass;
    e->shock_speed  = SHOCK_SPEED_BASE * pow(mass_scale, 0.08) * (0.5 + 0.5 * violence);
    e->cloud_speed  = CLOUD_SPEED_BASE * pow(mass_scale, 0.05);
    e->initial_radius = initial_radius;
    e->color[0] = rcol[0]; e->color[1] = rcol[1]; e->color[2] = rcol[2];
    e->seed = (float)(((star_idx + 1) * 23 + 7) % 997) / 997.0f;
    supernova_build_candidates(e);

    children_invalidate();   /* body set just changed (remnant added, stars retired) */
    immediate_flash_effects(e);

    fprintf(stderr, "[supernova] %s -> %s (%.1f Msun progenitor, %s)\n",
            g_bodies[star_idx].name, g_bodies[remnant_idx].name, msun,
            remnant_is_bh ? "black hole" :
            (remnant_phase == STAR_NEUTRON_STAR ? "neutron star" : "white dwarf"));
    return remnant_idx;
}

void supernova_step(double dt)
{
    if (dt <= 0.0) return;

    /* Rebuild the child index at most once this frame, lazily on first use, so
     * it reflects any body/parent changes since the previous frame. */
    children_invalidate();

    for (int ei = 0; ei < SUPERNOVA_MAX_EVENTS; ei++) {
        SupernovaEvent *e = &s_events[ei];
        if (!e->active) continue;

        e->age += dt;
        /* Intentionally keep the supernova anchored at its birth position in
         * world space. The remnant body can move away, but the visible blast
         * cloud and its shock effects stay centered on the original event. */

        {
            double shock_radius = e->shock_speed * e->age;
            if (!supernova_event_ensure_push_capacity(e, g_nbodies)) {
                supernova_event_release(e);
                e->active = 0;
                continue;
            }

            /* Bodies are kicked once, when the expanding shock first reaches
             * their orbital subtree root. The impulse magnitude is deliberately
             * conservative so it nudges survivor orbits instead of dominating
             * the post-remnant gravitational response. Only the candidate set
             * (bodies within the maximum blast radius, gathered at detonation) is
             * visited — never the whole universe. */
            for (int k = 0; k < e->n_cand; k++) {
                int i = e->cand[k];
                Body *b = &g_bodies[i];
                double dx, dy, dz, dist2, dist, subtree_mass, momentum, dv_mag;
                double dir[3], dv[3];

                if (!body_is_root_orbiting_body(i)) continue;
                if (!b->alive || (e->push_done && e->push_done[i])) continue;
                if (i == e->remnant_idx) continue;

                dx = b->pos[0] - e->pos[0];
                dy = b->pos[1] - e->pos[1];
                dz = b->pos[2] - e->pos[2];
                dist2 = dx*dx + dy*dy + dz*dz;
                if (dist2 <= 1.0) dist2 = 1.0;
                dist = sqrt(dist2);
                if (dist > shock_radius) continue;

                subtree_mass = body_subtree_mass(i);
                if (subtree_mass <= 0.0) {
                    if (e->push_done) e->push_done[i] = 1;
                    continue;
                }

                dir[0] = dx / dist;
                dir[1] = dy / dist;
                dir[2] = dz / dist;

                momentum = 0.42 * e->ejecta_mass * e->shock_speed *
                           (b->radius * b->radius) / (4.0 * dist2);
                dv_mag = momentum / subtree_mass;
                dv_mag = clampd_local(dv_mag, 0.0, e->shock_speed * 0.06);

                dv[0] = dir[0] * dv_mag;
                dv[1] = dir[1] * dv_mag;
                dv[2] = dir[2] * dv_mag;
                apply_velocity_to_subtree(i, dv);
                if (e->push_done) e->push_done[i] = 1;
            }
        }

        if (e->age >= CLOUD_DURATION) {
            supernova_event_release(e);
            e->active = 0;
        }
    }
}

int supernova_render_events(SupernovaRenderEvent *out, int max_events,
                            const double cam_pos[3])
{
    int n = 0;
    if (!out || max_events <= 0 || !cam_pos) return 0;

    for (int ei = 0; ei < SUPERNOVA_MAX_EVENTS && n < max_events; ei++) {
        SupernovaEvent *e = &s_events[ei];
        SupernovaRenderEvent *r;
        double cloud_t;
        double flash_alpha, core_alpha, cloud_alpha, hot_alpha;
        double flash_radius, core_radius, cloud_radius, inner_ratio;
        double core_growth, cloud_growth, cavity_growth;
        double cloud_rise, cloud_fade;
        double cloud_impulse, cloud_spread, end_fade, tail_fade;
        double age = e->age;

        if (!e->active) continue;

        /* These curves are tuned to keep the flash fast, the core lingering for
         * days, and the ejecta cloud expanding for much longer before a steeper
         * late fade clears the final low-quality tail. */
        cloud_t = age / CLOUD_DURATION;
        core_growth = 1.0 - exp(-age / (DAY * 0.40));
        cloud_growth = 1.0 - exp(-age / (DAY * 14.0));
        cavity_growth = 1.0 - exp(-age / (DAY * 24.0));
        cloud_rise = 1.0 - exp(-age / (DAY * 0.16));
        cloud_fade = exp(-age / (DAY * 110.0));
        cloud_impulse = 1.0 - exp(-age / (DAY * 0.11));
        cloud_spread = pow(1.0 + age / (DAY * 0.85), 0.82) - 1.0;
        end_fade = 1.0 - smoothstepd_local(0.58, 1.0, cloud_t);
        tail_fade = 1.0 - smoothstepd_local(0.60, 1.0, cloud_t);
        tail_fade = tail_fade * tail_fade * tail_fade;

        flash_alpha = 1.28 * rise_and_fall(age, DAY * 0.012, DAY * 0.42)
                    + 0.18 * exp(-age / (DAY * 2.2));
        core_alpha = 0.96 * rise_and_fall(age, DAY * 0.05, DAY * 7.5)
                   + 0.10 * exp(-age / (DAY * 32.0));
        cloud_alpha = 0.90 * (0.26 + 0.74 * cloud_rise) * cloud_fade * end_fade * end_fade * tail_fade;
        hot_alpha = 1.00 * rise_and_fall(age, DAY * 0.03, DAY * 18.0) * end_fade * end_fade * tail_fade;
        core_alpha *= end_fade * end_fade;
        flash_alpha *= end_fade * end_fade;

        flash_radius = fmax(0.12 * AU,
                            e->initial_radius * (4.0 + 7.2 * (1.0 - exp(-age / (DAY * 0.05))))
                          + e->shock_speed * age * 0.13);
        core_radius = fmax(0.08 * AU,
                           e->initial_radius * (1.9 + 4.4 * core_growth)
                         + e->cloud_speed * age * 0.045);
        cloud_radius = e->initial_radius * (0.58 + 0.86 * cloud_rise)
                     + e->cloud_speed * DAY * (0.30 * cloud_impulse + 0.56 * cloud_spread)
                     + e->initial_radius * 2.6 * cloud_growth;
        if (cloud_radius < e->initial_radius * 0.55)
            cloud_radius = e->initial_radius * 0.55;
        if (cloud_radius < core_radius * 0.70)
            cloud_radius = core_radius * 0.70;

        inner_ratio = 0.06 + 0.70 * cavity_growth;
        if (inner_ratio > 0.88) inner_ratio = 0.88;

        r = &out[n++];
        /* Render payloads are camera-relative AU values because the renderer
         * works in view-space-friendly units rather than raw world metres. */
        r->pos[0] = (float)(e->pos[0] * RS - cam_pos[0]);
        r->pos[1] = (float)(e->pos[1] * RS - cam_pos[1]);
        r->pos[2] = (float)(e->pos[2] * RS - cam_pos[2]);
        r->flash_radius = (float)(flash_radius * RS);
        r->core_radius = (float)(core_radius * RS);
        r->cloud_radius = (float)(cloud_radius * RS);
        r->cloud_inner_radius = (float)(cloud_radius * inner_ratio * RS);
        r->flash_intensity = clampf_local((float)flash_alpha, 0.0f, 1.0f);
        r->core_intensity = clampf_local((float)core_alpha, 0.0f, 1.0f);
        r->cloud_intensity = clampf_local((float)cloud_alpha, 0.0f, 1.0f);
        r->hot_shell_intensity = clampf_local((float)hot_alpha, 0.0f, 1.0f);
        r->color[0] = e->color[0];
        r->color[1] = e->color[1];
        r->color[2] = e->color[2];
        r->seed = e->seed;
        r->time_days = (float)(e->age / DAY);
    }

    return n;
}
