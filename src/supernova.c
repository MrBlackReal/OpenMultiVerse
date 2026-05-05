/*
 * supernova.c - generic star-star supernova events.
 */
#include "supernova.h"
#include "body.h"
#include "collision.h"
#include "labels.h"
#include "trails.h"
#include "universe.h"
#include <math.h>
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
    unsigned char push_done[MAX_BODIES];
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

static int body_is_descendant_of_local(int body_idx, int ancestor_idx)
{
    if (body_idx < 0 || ancestor_idx < 0) return 0;
    for (int p = g_bodies[body_idx].parent; p >= 0; p = g_bodies[p].parent)
        if (p == ancestor_idx) return 1;
    return 0;
}

static int body_is_root_orbiting_body(int body_idx)
{
    if (body_idx < 0 || body_idx >= g_nbodies) return 0;
    if (!g_bodies[body_idx].alive || g_bodies[body_idx].is_star) return 0;
    return g_bodies[body_idx].parent < 0 ||
           g_bodies[g_bodies[body_idx].parent].is_star;
}

static double body_subtree_mass(int root_idx)
{
    double total = 0.0;
    if (root_idx < 0 || root_idx >= g_nbodies) return 0.0;
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        if (i == root_idx || body_is_descendant_of_local(i, root_idx))
            total += g_bodies[i].mass;
    }
    return total;
}

static void apply_velocity_to_subtree(int root_idx, const double dv[3])
{
    if (!dv) return;
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        if (i != root_idx && !body_is_descendant_of_local(i, root_idx))
            continue;
        g_bodies[i].vel[0] += dv[0];
        g_bodies[i].vel[1] += dv[1];
        g_bodies[i].vel[2] += dv[2];
    }
}

static void disable_body_visuals(int body_idx)
{
    if (body_idx < 0 || body_idx >= g_nbodies) return;
    g_bodies[body_idx].trail_emitting = 0;
    g_bodies[body_idx].trail_accum = 0.0;
    labels_remove_body(body_idx);
}

static void destroy_subtree(int root_idx)
{
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        if (i != root_idx && !body_is_descendant_of_local(i, root_idx))
            continue;
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

static void event_center_from_pair(int a, int b, double out_pos[3], double out_vel[3])
{
    double total = g_bodies[a].mass + g_bodies[b].mass;
    if (total <= 0.0) total = 1.0;

    out_pos[0] = (g_bodies[a].pos[0] * g_bodies[a].mass +
                  g_bodies[b].pos[0] * g_bodies[b].mass) / total;
    out_pos[1] = (g_bodies[a].pos[1] * g_bodies[a].mass +
                  g_bodies[b].pos[1] * g_bodies[b].mass) / total;
    out_pos[2] = (g_bodies[a].pos[2] * g_bodies[a].mass +
                  g_bodies[b].pos[2] * g_bodies[b].mass) / total;

    out_vel[0] = (g_bodies[a].vel[0] * g_bodies[a].mass +
                  g_bodies[b].vel[0] * g_bodies[b].mass) / total;
    out_vel[1] = (g_bodies[a].vel[1] * g_bodies[a].mass +
                  g_bodies[b].vel[1] * g_bodies[b].mass) / total;
    out_vel[2] = (g_bodies[a].vel[2] * g_bodies[a].mass +
                  g_bodies[b].vel[2] * g_bodies[b].mass) / total;
}

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
    memset(s_events, 0, sizeof(s_events));
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

    (void)hit_t;
    (void)frame_dt;

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

    total_mass = g_bodies[star_a].mass + g_bodies[star_b].mass;
    if (total_mass <= 0.0) return 0;

    event_center_from_pair(star_a, star_b, pos, vel);
    mass_scale = total_mass / SOLAR_MASS;
    if (mass_scale < 0.5) mass_scale = 0.5;
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
    if (remnant_idx < 0) return 0;
    trails_add_body(remnant_idx);
    labels_add_body(remnant_idx);
    collision_on_body_added(remnant_idx);

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

    e = &s_events[slot];
    memset(e, 0, sizeof(*e));
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

    immediate_flash_effects(e);

    fprintf(stderr,
            "[supernova] %s + %s -> %s (rel=%.0f m/s, ejecta=%.2f Msun)\n",
            g_bodies[star_a].name, g_bodies[star_b].name, g_bodies[remnant_idx].name,
            rel_speed, ejecta_mass / SOLAR_MASS);
    return 1;
}

void supernova_step(double dt)
{
    if (dt <= 0.0) return;

    for (int ei = 0; ei < SUPERNOVA_MAX_EVENTS; ei++) {
        SupernovaEvent *e = &s_events[ei];
        if (!e->active) continue;

        e->age += dt;
        if (e->remnant_idx >= 0 && e->remnant_idx < g_nbodies &&
            g_bodies[e->remnant_idx].alive) {
            e->pos[0] = g_bodies[e->remnant_idx].pos[0];
            e->pos[1] = g_bodies[e->remnant_idx].pos[1];
            e->pos[2] = g_bodies[e->remnant_idx].pos[2];
            e->vel[0] = g_bodies[e->remnant_idx].vel[0];
            e->vel[1] = g_bodies[e->remnant_idx].vel[1];
            e->vel[2] = g_bodies[e->remnant_idx].vel[2];
        } else {
            e->pos[0] += e->vel[0] * dt;
            e->pos[1] += e->vel[1] * dt;
            e->pos[2] += e->vel[2] * dt;
        }

        {
            double shock_radius = e->shock_speed * e->age;
            for (int i = 0; i < g_nbodies; i++) {
                Body *b = &g_bodies[i];
                double dx, dy, dz, dist2, dist, subtree_mass, momentum, dv_mag;
                double dir[3], dv[3];

                if (!body_is_root_orbiting_body(i)) continue;
                if (!b->alive || e->push_done[i]) continue;
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
                    e->push_done[i] = 1;
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
                e->push_done[i] = 1;
            }
        }

        if (e->age >= CLOUD_DURATION)
            e->active = 0;
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
        double cloud_impulse, cloud_spread, end_fade;
        double age = e->age;

        if (!e->active) continue;

        cloud_t = age / CLOUD_DURATION;
        core_growth = 1.0 - exp(-age / (DAY * 0.40));
        cloud_growth = 1.0 - exp(-age / (DAY * 14.0));
        cavity_growth = 1.0 - exp(-age / (DAY * 24.0));
        cloud_rise = 1.0 - exp(-age / (DAY * 0.16));
        cloud_fade = exp(-age / (DAY * 110.0));
        cloud_impulse = 1.0 - exp(-age / (DAY * 0.11));
        cloud_spread = pow(1.0 + age / (DAY * 0.85), 0.82) - 1.0;
        end_fade = 1.0 - smoothstepd_local(0.58, 1.0, cloud_t);

        flash_alpha = 1.28 * rise_and_fall(age, DAY * 0.012, DAY * 0.42)
                    + 0.18 * exp(-age / (DAY * 2.2));
        core_alpha = 0.96 * rise_and_fall(age, DAY * 0.05, DAY * 7.5)
                   + 0.10 * exp(-age / (DAY * 32.0));
        cloud_alpha = 0.90 * (0.26 + 0.74 * cloud_rise) * cloud_fade * end_fade * end_fade;
        hot_alpha = 1.00 * rise_and_fall(age, DAY * 0.03, DAY * 18.0) * end_fade * end_fade;
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
