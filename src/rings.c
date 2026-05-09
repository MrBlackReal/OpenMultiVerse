/*
 * rings.c — Keplerian ring particle system, data-driven from universe.json
 *
 * ── Physics model ────────────────────────────────────────────────────────
 *
 * Each ring particle follows a Keplerian elliptical orbit:
 *   - The CPU advances the mean anomaly M each physics step: M += n × dt
 *     where n = √(GM/a³) is the mean motion in rad/s.
 *   - The GPU (ring.vert) solves the Kepler equation E − e·sin(E) = M
 *     via Newton-Raphson each frame to compute the particle's screen position.
 *     This offloads the O(N) per-particle position computation entirely to the GPU.
 *
 * ── Per-particle data layout (8 floats per particle) ────────────────────
 *
 *   [0] M0    — current mean anomaly (radians), updated by rings_tick()
 *   [1] a_au  — semi-major axis (AU)
 *   [2] e     — eccentricity
 *   [3] omega — argument of periapsis (radians)
 *   [4] h     — vertical displacement from ring plane (AU), small for thin disc
 *   [5] r     — red channel (particle color)
 *   [6] g     — green channel
 *   [7] b     — blue channel
 *
 * The corresponding n_arr[] stores the pre-computed mean motion (rad/s) for
 * each particle at the same index.
 *
 * ── Semi-major axis distribution ─────────────────────────────────────────
 *
 * Uniform sampling in a² (instead of in a) gives equal particle area density
 * across the disc annulus:
 *   a = √(r²_min + u × (r²_max − r²_min))   where u ~ Uniform[0,1]
 * This mirrors the asteroids.c belt distribution; area-uniform sampling avoids
 * over-density near the inner edge.
 *
 * ── Ring-plane orientation ────────────────────────────────────────────────
 *
 * Ring plane = perpendicular to the planet's rotation pole.
 * The basis {b1, b2, pole} is built by rotating the ecliptic frame around X
 * by the planet's obliquity angle:
 *   b1   = (1, 0, 0)                  — fixed ecliptic X
 *   b2   = (0, sin(obl), −cos(obl))   — 90° from pole in the ring plane
 *   pole = (0, cos(obl),  sin(obl))   — planet's rotation axis
 *
 * ── LOD switching ─────────────────────────────────────────────────────────
 *
 *   dist > SPRITE_DIST  →  flat sprite quad (single textured disc billboard)
 *   dist ∈ [LOD_DIST, SPRITE_DIST] →  reduced particle count (n_lod)
 *   dist ≤ LOD_DIST     →  full particle count (n_full)
 *
 * The sprite quad uses ring_sprite.frag (Saturn texture) or
 * ring_sprite_generic.frag (procedural ring drawn by inner/outer radius),
 * selected by the JSON "shader_type" field.
 */
#include "rings.h"
#include "body.h"
#include "collision.h"
#include "physics.h"
#include "gl_utils.h"
#include "json.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define SPRITE_DIST   0.2f    /* AU: farther than this → flat sprite */
#define LOD_DIST      0.05f   /* AU: farther than this → reduced count */
#define MAX_ZONES     16      /* maximum annulus zones per ring descriptor */
#define DAMAGE_MIN_WIDTH     0.18f
#define DAMAGE_MAX_WIDTH     1.00f
#define DAMAGE_MAX_ECC       0.080f
#define DAMAGE_MIN_A_SCALE   0.88f
#define DAMAGE_MAX_A_SCALE   1.18f
#define HIT_SEGMENT_COOLDOWN_SECONDS (DAY * 0.18)
#define RING_SWEEP_MAX_SAMPLES 5
#define MORPH_SCALE_RELAX_MIN (60.0f * 8.0f)
#define MORPH_SCALE_RELAX_MAX ((float)DAY * 0.20f)
#define MORPH_PUFF_RELAX_MIN  (60.0f * 4.0f)
#define MORPH_PUFF_RELAX_MAX  ((float)DAY * 0.10f)
#define MORPH_DECAY_MIN       (60.0f * 30.0f)
#define MORPH_DECAY_MAX       ((float)DAY * 1.50f)
#define PARENT_TRACK_MIN      (60.0f * 20.0f)
#define PARENT_TRACK_MAX      ((float)DAY * 0.35f)
#define RESPONSE_VISUAL_DT_MAX (60.0f * 12.0f)
#define TIDAL_MAX_DT          ((double)DAY * 0.015)
#define TIDAL_VISUAL_GAIN     5.5
#define TIDAL_MAX_DA_FRAC     0.0015f
#define TIDAL_MAX_DE          0.0012f
#define TIDAL_WARP_MIN_WIDTH  0.14f
#define TIDAL_WARP_MAX_WIDTH  0.95f
#define RING_GLOBAL_SCALE_MAX 1.65f

/* ── Zone ── one concentric annulus band of a ring disc ────────────────── */
typedef struct {
    float r_min, r_max;   /* inner / outer radius in km */
    float density;        /* fraction of total particles allocated to this zone */
    float r, g, b;        /* base particle color */
} Zone;

/* ── Disc instances loaded from universe.json ───────────────────────────── */
static ParticleDisc *s_discs   = NULL;
static int           s_n_discs = 0;

/* ── XorShift32 PRNG ─────────────────────────────────────────────────────
 * Used for deterministic ring particle generation.  The seed is set per-disc
 * from "seed_full" / "seed_lod" in the JSON so regenerating (e.g. after a
 * collision retune) gives the same ring layout.                            */
static uint32_t s_rng = 1;
static void  s_seed(uint32_t seed) { s_rng = seed ? seed : 1; }
static float s_randf(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) * (1.0f / (float)(1 << 24));
}

/* ── Ring collision helpers and smooth response state ───────────────────── */
static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static float wrap_angle_pi(float a)
{
    const float TWO_PI = 6.28318530718f;
    while (a >  (float)PI) a -= TWO_PI;
    while (a < -(float)PI) a += TWO_PI;
    return a;
}

static float angular_falloff(float dphi, float half_width)
{
    float t;
    dphi = fabsf(dphi);
    if (half_width <= 1e-5f || dphi >= half_width) return 0.0f;
    t = 1.0f - dphi / half_width;
    return t * t * (3.0f - 2.0f * t);
}

static float dot3f_local(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void normalize3f_local(float v[3])
{
    float len = sqrtf(dot3f_local(v, v));
    if (len <= 1e-8f) {
        v[0] = 1.0f;
        v[1] = 0.0f;
        v[2] = 0.0f;
        return;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

static int body_is_ring_perturber_planet(int idx)
{
    if (idx < 0 || idx >= g_nbodies) return 0;
    if (!g_bodies[idx].alive || g_bodies[idx].is_star) return 0;
    if (g_bodies[idx].parent < 0 || !g_bodies[g_bodies[idx].parent].is_star) return 0;

    /*
     * Body.type is not kept after JSON loading.  This threshold keeps real and
     * build-created planets, while excluding moons and dwarf/asteroid-sized
     * bodies such as Ceres/Pluto-style objects.
     */
    return g_bodies[idx].radius >= 2000.0e3 || g_bodies[idx].mass >= 1.0e23;
}

static void disc_update_mid_motion(ParticleDisc *d)
{
    double gm;
    double a_mid_m;

    if (!d || d->parent_idx < 0 || d->parent_idx >= g_nbodies) return;
    gm = G_CONST * g_bodies[d->parent_idx].mass;
    a_mid_m = ((double)d->ring_r_inner_km + (double)d->ring_r_outer_km) * 500.0;
    d->mean_motion_mid = (gm > 0.0 && a_mid_m > 0.0)
        ? (float)sqrt(gm / (a_mid_m * a_mid_m * a_mid_m))
        : 0.0f;
}

static float disc_mid_period_seconds(const ParticleDisc *d)
{
    if (!d || d->mean_motion_mid <= 1e-8f) return (float)DAY * 0.5f;
    return (2.0f * (float)PI) / d->mean_motion_mid;
}

static void disc_clear_hit_cooldown(ParticleDisc *d)
{
    if (!d) return;
    for (int i = 0; i < RING_COLLISION_SEGMENTS * RING_COLLISION_RADIAL_BINS; i++)
        d->hit_cooldown[i] = 0.0f;
}

static void disc_reset_response(ParticleDisc *d)
{
    if (!d) return;
    disc_clear_hit_cooldown(d);
    d->scale_cur = 1.0f;
    d->scale_target = 1.0f;
    d->puff_cur = 0.0f;
    d->puff_target = 0.0f;
    d->shock_phase = 0.0f;
    d->shock_width = 0.35f;
    d->shock_amp = 0.0f;
    d->shock_spin = 0.0f;
    d->contact_norm = 0.5f;
    d->contact_width = 0.25f;
    d->contact_strength = 0.0f;
    d->tide_phase = 0.0f;
    d->tide_radius_norm = 0.5f;
    d->tide_width = 0.35f;
    d->tide_strength = 0.0f;
    d->tide_dir_u = 1.0f;
    d->tide_dir_v = 0.0f;
    d->tide_dir_n = 0.0f;
    d->body_u_au = 0.0f;
    d->body_v_au = 0.0f;
    d->body_n_au = 0.0f;
    d->body_radius_au = 0.0f;
    d->body_strength = 0.0f;
}

static void disc_queue_scale_target(ParticleDisc *d, float target)
{
    if (!d) return;
    target = clampf(target, 0.85f, RING_GLOBAL_SCALE_MAX);
    if (target > d->scale_target) d->scale_target = target;
}

static float smooth_raise(float cur, float target, float rate)
{
    if (target <= cur) return cur;
    return cur + (target - cur) * clampf(rate, 0.0f, 1.0f);
}

static void disc_drive_response(ParticleDisc *d, float phase, float half_width,
                                float severity, float overlap,
                                float radial_center, float radial_width)
{
    float shock_target;
    float contact_target;

    if (!d) return;
    severity = clampf(severity, 0.0f, 1.0f);
    overlap = clampf(overlap, 0.0f, 1.0f);

    if (d->shock_amp <= 0.001f) {
        d->shock_phase = phase;
    } else {
        float phase_blend = 0.035f + 0.080f * overlap;
        d->shock_phase = wrap_angle_pi(d->shock_phase +
                         wrap_angle_pi(phase - d->shock_phase) * phase_blend);
    }
    d->shock_width = fmaxf(d->shock_width, half_width);
    shock_target = (0.060f + 0.170f * severity) * (0.35f + 0.65f * overlap);
    d->shock_amp = smooth_raise(d->shock_amp, shock_target, 0.12f + 0.12f * overlap);
    d->shock_spin = d->mean_motion_mid * (0.22f + 0.60f * severity);
    d->puff_target = fmaxf(d->puff_target, 0.16f + 0.62f * severity * overlap);

    radial_center = clampf(radial_center, 0.0f, 1.0f);
    radial_width = clampf(radial_width, 0.06f, 0.80f);
    if (d->contact_strength <= 0.001f) {
        d->contact_norm = radial_center;
        d->contact_width = radial_width;
    } else {
        float blend = 0.08f + 0.16f * overlap;
        d->contact_norm += (radial_center - d->contact_norm) * blend;
        d->contact_width += (radial_width - d->contact_width) * blend;
    }
    contact_target = (0.22f + 0.78f * severity) * overlap;
    d->contact_strength = smooth_raise(d->contact_strength,
                                       contact_target,
                                       0.10f + 0.18f * overlap);
}

/*
 * disc_drive_tide_response - smooth 3D visual pull from a nearby planet.
 *
 * The swept hitbox pass already knows the perturber position in the ring's
 * local frame.  Instead of spawning real ring segments, keep one dominant,
 * damped response vector and let the vertex shader bend only the particles
 * under that local influence.  This preserves the original ring rendering
 * path while making close planet encounters read as volumetric, not 2D.
 */
static void disc_drive_tide_response(ParticleDisc *d,
                                     double ru, double rv, double rh,
                                     double proj_r_km, double h_km,
                                     double other_r_km, double reach_km)
{
    double ring_inner_km, ring_outer_km, ring_width_km;
    double radial_gap_km, proximity_km, local_len;
    float phase, radial_norm, width, strength, blend, body_blend;
    float body_u_au, body_v_au, body_n_au, body_radius_au;
    float dir_u, dir_v, dir_n;

    if (!d || reach_km <= 1.0 || other_r_km <= 1.0) return;

    ring_inner_km = (double)d->ring_r_inner_km;
    ring_outer_km = (double)d->ring_r_outer_km * (double)fmaxf(d->scale_cur, 1.0f);
    ring_width_km = fmax(ring_outer_km - ring_inner_km, 1.0);

    if (proj_r_km < ring_inner_km)
        radial_gap_km = ring_inner_km - proj_r_km;
    else if (proj_r_km > ring_outer_km)
        radial_gap_km = proj_r_km - ring_outer_km;
    else
        radial_gap_km = 0.0;

    proximity_km = sqrt(radial_gap_km * radial_gap_km + h_km * h_km);
    strength = clampf((float)(1.0 - proximity_km / fmax(reach_km, 1.0)), 0.0f, 1.0f);
    if (strength <= 0.0f) return;

    local_len = sqrt(ru*ru + rv*rv + rh*rh);
    if (local_len <= 1.0) return;

    phase = atan2f((float)rv, (float)ru);
    radial_norm = clampf((float)((proj_r_km - ring_inner_km) / ring_width_km), 0.0f, 1.0f);
    width = clampf((float)(other_r_km / ring_width_km) * 1.65f + strength * 0.20f,
                   TIDAL_WARP_MIN_WIDTH, TIDAL_WARP_MAX_WIDTH);

    dir_u = (float)(ru / local_len);
    dir_v = (float)(rv / local_len);
    dir_n = (float)(rh / local_len);

    strength *= clampf((float)(other_r_km / fmax(ring_width_km * 0.18, 1.0)),
                       0.35f, 1.30f);
    strength = clampf(strength * 0.80f, 0.0f, 1.0f);
    body_u_au = (float)(ru * RS);
    body_v_au = (float)(rv * RS);
    body_n_au = (float)(rh * RS);
    body_radius_au = (float)(other_r_km / 1.496e8) * 1.08f;

    if (d->tide_strength <= 0.001f) {
        d->tide_phase = phase;
        d->tide_radius_norm = radial_norm;
        d->tide_width = width;
        d->tide_dir_u = dir_u;
        d->tide_dir_v = dir_v;
        d->tide_dir_n = dir_n;
        d->body_u_au = body_u_au;
        d->body_v_au = body_v_au;
        d->body_n_au = body_n_au;
        d->body_radius_au = body_radius_au;
    } else {
        blend = 0.045f + 0.155f * strength;
        d->tide_phase = wrap_angle_pi(d->tide_phase +
                         wrap_angle_pi(phase - d->tide_phase) * blend);
        d->tide_radius_norm += (radial_norm - d->tide_radius_norm) * blend;
        d->tide_width += (width - d->tide_width) * blend;
        d->tide_dir_u += (dir_u - d->tide_dir_u) * blend;
        d->tide_dir_v += (dir_v - d->tide_dir_v) * blend;
        d->tide_dir_n += (dir_n - d->tide_dir_n) * blend;
        body_blend = 0.18f + 0.32f * strength;
        d->body_u_au += (body_u_au - d->body_u_au) * body_blend;
        d->body_v_au += (body_v_au - d->body_v_au) * body_blend;
        d->body_n_au += (body_n_au - d->body_n_au) * body_blend;
        d->body_radius_au += (body_radius_au - d->body_radius_au) * body_blend;

        local_len = sqrt((double)d->tide_dir_u * d->tide_dir_u +
                         (double)d->tide_dir_v * d->tide_dir_v +
                         (double)d->tide_dir_n * d->tide_dir_n);
        if (local_len > 1e-6) {
            d->tide_dir_u = (float)(d->tide_dir_u / local_len);
            d->tide_dir_v = (float)(d->tide_dir_v / local_len);
            d->tide_dir_n = (float)(d->tide_dir_n / local_len);
        }
    }

    d->tide_strength = smooth_raise(d->tide_strength,
                                    strength,
                                    0.08f + 0.20f * strength);
    d->body_strength = smooth_raise(d->body_strength,
                                    clampf(strength * 1.25f, 0.0f, 1.0f),
                                    0.16f + 0.26f * strength);
}

static void disc_drive_transfer_response(ParticleDisc *d,
                                         float old_parent_radius_km,
                                         float new_parent_radius_km,
                                         float severity_bias)
{
    float ratio;
    float severity;
    float min_scale;

    if (!d || new_parent_radius_km <= 0.0f) return;
    if (old_parent_radius_km <= 0.0f) old_parent_radius_km = new_parent_radius_km;

    ratio = new_parent_radius_km / fmaxf(old_parent_radius_km, 1.0f);
    severity = clampf(fabsf(ratio - 1.0f) * 0.85f + severity_bias, 0.0f, 1.0f);
    min_scale = (new_parent_radius_km * (1.10f + 0.08f * severity))
              / fmaxf(d->ring_r_inner_km, 1.0f);
    if (ratio > 1.0f)
        min_scale = fmaxf(min_scale, 1.0f + (ratio - 1.0f) * (0.30f + 0.32f * severity));

    disc_queue_scale_target(d, min_scale);
    d->puff_target = fmaxf(d->puff_target, 0.12f + 0.30f * severity);
    d->shock_amp = fmaxf(d->shock_amp, 0.035f + 0.070f * severity);
    d->shock_width = fmaxf(d->shock_width, 0.65f + 0.35f * severity);
    if (d->mean_motion_mid > 1e-8f)
        d->shock_spin = d->mean_motion_mid * (0.14f + 0.34f * severity);
    d->parent_radius_ref_km = old_parent_radius_km;
}

static void disc_update_response(ParticleDisc *d, float dt)
{
    float period, scale_tau, puff_tau, decay_tau, relax, parent_radius_km;
    if (!d || dt <= 0.0f) return;
    if (dt > RESPONSE_VISUAL_DT_MAX) dt = RESPONSE_VISUAL_DT_MAX;

    period = disc_mid_period_seconds(d);
    scale_tau = clampf(period * 0.10f, MORPH_SCALE_RELAX_MIN, MORPH_SCALE_RELAX_MAX);
    puff_tau  = clampf(period * 0.06f, MORPH_PUFF_RELAX_MIN,  MORPH_PUFF_RELAX_MAX);
    decay_tau = clampf(period * 0.65f, MORPH_DECAY_MIN,       MORPH_DECAY_MAX);

    parent_radius_km = (float)(collision_visual_radius(d->parent_idx,
                                                       g_bodies[d->parent_idx].radius) * 0.001);
    if (parent_radius_km > 0.0f) {
        if (d->parent_radius_ref_km <= 0.0f) d->parent_radius_ref_km = parent_radius_km;
        if (parent_radius_km > d->parent_radius_ref_km * 1.002f)
            disc_drive_transfer_response(d, d->parent_radius_ref_km, parent_radius_km, 0.12f);
        else {
            float min_scale = (parent_radius_km * 1.08f) / fmaxf(d->ring_r_inner_km, 1.0f);
            if (min_scale > 1.0f) disc_queue_scale_target(d, min_scale);
        }
        {
            float parent_tau = clampf(period * 0.18f, PARENT_TRACK_MIN, PARENT_TRACK_MAX);
            d->parent_radius_ref_km += (parent_radius_km - d->parent_radius_ref_km)
                                     * (1.0f - expf(-dt / parent_tau));
        }
    }

    relax = 1.0f - expf(-dt / scale_tau);
    d->scale_cur += (d->scale_target - d->scale_cur) * relax;
    d->scale_target = 1.0f + (d->scale_target - 1.0f) * expf(-dt / decay_tau);

    relax = 1.0f - expf(-dt / puff_tau);
    d->puff_cur += (d->puff_target - d->puff_cur) * relax;
    d->puff_target *= expf(-dt / decay_tau);

    d->shock_phase = wrap_angle_pi(d->shock_phase + d->shock_spin * dt);
    d->shock_width += dt * d->mean_motion_mid * 0.07f;
    if (d->shock_width > (float)PI * 0.95f) d->shock_width = (float)PI * 0.95f;
    d->shock_amp *= expf(-dt / decay_tau);
    d->shock_spin *= expf(-dt / decay_tau);
    d->contact_strength *= expf(-dt / decay_tau);
    d->tide_strength *= expf(-dt / clampf(period * 0.38f, MORPH_DECAY_MIN, MORPH_DECAY_MAX));
    d->body_strength *= expf(-dt / clampf(period * 0.30f, MORPH_DECAY_MIN, MORPH_DECAY_MAX));
    if (d->shock_amp < 0.001f) d->shock_amp = 0.0f;
    if (d->contact_strength < 0.001f) d->contact_strength = 0.0f;
    if (d->tide_strength < 0.001f) d->tide_strength = 0.0f;
    if (d->body_strength < 0.001f) d->body_strength = 0.0f;
}

/* build_basis — ring-plane orthonormal frame from planet obliquity. */
static void build_basis(ParticleDisc *d, float obl_deg)
{
    float obl = obl_deg * (float)(PI / 180.0);
    d->b1[0] = 1.0f; d->b1[1] = 0.0f;        d->b1[2] = 0.0f;
    d->b2[0] = 0.0f; d->b2[1] =  sinf(obl);  d->b2[2] = -cosf(obl);
    d->pole[0] = 0.0f; d->pole[1] = cosf(obl); d->pole[2] = sinf(obl);
}

/*
 * retune_disc_parent — reattach a disc to a new parent body after absorption.
 *
 * Called by rings_on_body_absorbed() when the impactor carries a ring and the
 * target absorbs it.  Updates the basis vectors from the new parent's obliquity
 * and recomputes every particle's mean motion n = √(GM/a³) from the new GM.
 *
 * Must recompute n for all particles because n ∝ √(GM) — the new parent has
 * a different mass, so all orbital periods change.
 */
static void retune_disc_parent(ParticleDisc *d, int parent_idx)
{
    if (!d || parent_idx < 0 || parent_idx >= g_nbodies) return;
    d->parent_idx = parent_idx;
    build_basis(d, (float)g_bodies[parent_idx].obliquity);
    disc_update_mid_motion(d);

    double gm = G_CONST * g_bodies[parent_idx].mass;
    if (gm <= 0.0) {
        d->initialized = 0;
        return;
    }
    for (int i = 0; i < d->n_full; i++) {
        if (d->data_full[i*8+1] <= 0.0f) {
            d->n_arr_full[i] = 0.0f;
            continue;
        }
        double a_m = (double)d->data_full[i*8+1] * AU;
        d->n_arr_full[i] = (float)sqrt(gm / (a_m * a_m * a_m));
    }
    for (int i = 0; i < d->n_lod; i++) {
        if (d->data_lod[i*8+1] <= 0.0f) {
            d->n_arr_lod[i] = 0.0f;
            continue;
        }
        double a_m = (double)d->data_lod[i*8+1] * AU;
        d->n_arr_lod[i] = (float)sqrt(gm / (a_m * a_m * a_m));
    }
    d->initialized = 1;
}

/*
 * bake_particles — fill a particle data array by sampling orbits within zones.
 *
 * For each zone, particles are distributed by `density` fraction.  The last
 * zone absorbs rounding remainder so all n particles are placed.
 *
 * Per particle:
 *   a_au = √(r²_min_au + u × (r²_max_au − r²_min_au))  [area-uniform in AU]
 *   M0   = uniform in [0, 2π]
 *   e    = uniform in [0, e_max]
 *   omega= uniform in [0, 2π]
 *   h    = uniform in [−h_scale/2, h_scale/2]  (disc thickness in AU)
 *   color= zone base color × brightness jitter [0.85, 1.0]
 *   n    = √(GM / a³)  pre-computed in rad/s
 */
static void bake_particles(float *data, float *n_arr, int n,
                            const Zone *zones, int n_zones,
                            double gm, float e_max, float h_scale)
{
    int idx = 0;
    for (int z = 0; z < n_zones; z++) {
        int cnt = (z == n_zones - 1)
                  ? (n - idx)
                  : (int)(zones[z].density * n);

        float r_min_au = zones[z].r_min / 1.496e8f;   /* km → AU */
        float r_max_au = zones[z].r_max / 1.496e8f;
        float r2_min = r_min_au * r_min_au;
        float r2_max = r_max_au * r_max_au;

        for (int k = 0; k < cnt && idx < n; k++, idx++) {
            float a_au = sqrtf(r2_min + s_randf() * (r2_max - r2_min));
            float M0   = s_randf() * 2.0f * (float)PI;
            float e    = s_randf() * e_max;
            float omega= s_randf() * 2.0f * (float)PI;
            float h    = (s_randf() - 0.5f) * h_scale;

            data[idx*8+0] = M0;
            data[idx*8+1] = a_au;
            data[idx*8+2] = e;
            data[idx*8+3] = omega;
            data[idx*8+4] = h;

            float j = 0.85f + 0.15f * s_randf();   /* brightness jitter */
            data[idx*8+5] = zones[z].r * j;
            data[idx*8+6] = zones[z].g * j;
            data[idx*8+7] = zones[z].b * j;

            double a_m = (double)a_au * 1.496e11;   /* AU → metres */
            n_arr[idx] = (float)sqrt(gm / (a_m * a_m * a_m));
        }
    }
}

static float damage_phase_from_world_dir(const ParticleDisc *d,
                                         const double dir[3],
                                         const double rel_vel[3])
{
    float proj[3];
    float pole_dot;

    if (!d || !dir) return 0.0f;
    proj[0] = (float)dir[0];
    proj[1] = (float)dir[1];
    proj[2] = (float)dir[2];
    pole_dot = dot3f_local(proj, d->pole);
    proj[0] -= pole_dot * d->pole[0];
    proj[1] -= pole_dot * d->pole[1];
    proj[2] -= pole_dot * d->pole[2];

    if (dot3f_local(proj, proj) <= 1e-8f && rel_vel) {
        proj[0] = (float)rel_vel[0];
        proj[1] = (float)rel_vel[1];
        proj[2] = (float)rel_vel[2];
        pole_dot = dot3f_local(proj, d->pole);
        proj[0] -= pole_dot * d->pole[0];
        proj[1] -= pole_dot * d->pole[1];
        proj[2] -= pole_dot * d->pole[2];
    }

    normalize3f_local(proj);
    return atan2f(dot3f_local(proj, d->b2), dot3f_local(proj, d->b1));
}

static uint32_t damage_seed_for_disc(const ParticleDisc *d,
                                     int body_idx,
                                     double rel_speed,
                                     float phase)
{
    uint32_t seed = 0x9e3779b9u;
    uint32_t speed_bits = (uint32_t)(rel_speed * 0.25 + 0.5);
    uint32_t phase_bits = (uint32_t)((phase + (float)PI) * 1000.0f);

    seed ^= (uint32_t)(body_idx + 1) * 0x85ebca6bu;
    seed ^= speed_bits * 0xc2b2ae35u;
    seed ^= phase_bits * 0x27d4eb2du;
    if (d) {
        seed ^= (uint32_t)(d->ring_r_inner_km * 7.0f + d->ring_r_outer_km * 3.0f);
        seed ^= (uint32_t)(d->parent_idx + 17) * 0x165667b1u;
    }
    return seed ? seed : 1u;
}

/*
 * apply_damage_to_array — local one-shot orbit shear for a struck ring band.
 *
 * The contact grid decides when to call this.  We then perturb only the
 * particles near that azimuth and let Keplerian drift stretch the scar into
 * arcs, which keeps runtime cost low and avoids per-frame particle collisions.
 */
static void apply_damage_to_array(const ParticleDisc *d,
                                  float *data, float *n_arr, int n,
                                  double gm, float phase, float half_width,
                                  float severity, float tangential_sign,
                                  float vertical_sign, uint32_t seed)
{
    float ring_width_km;
    float width_au;
    float puff_scale;

    if (!d || !data || !n_arr || n <= 0 || gm <= 0.0) return;

    ring_width_km = fmaxf(d->ring_r_outer_km - d->ring_r_inner_km, 1.0f);
    width_au = ring_width_km / 1.496e8f;
    puff_scale = fmaxf(d->base_h_scale * (12.0f + 28.0f * severity),
                       width_au * (0.0012f + 0.0025f * severity));
    s_seed(seed);

    for (int i = 0; i < n; i++) {
        float *p = data + i * 8;
        float a0 = p[1];
        float theta = p[0] + p[3];
        float dphi = wrap_angle_pi(theta - phase);
        float ang_w = angular_falloff(dphi, half_width);
        float r_km, r_norm, radial_w, w, jitter, a_scale;

        if (a0 <= 0.0f || ang_w <= 0.0f) continue;

        r_km = p[1] * 1.496e8f;
        r_norm = clampf((r_km - d->ring_r_inner_km) / ring_width_km, 0.0f, 1.0f);
        radial_w = 0.55f + 0.45f * (1.0f - fabsf(r_norm - 0.56f) * 1.55f);
        radial_w = clampf(radial_w, 0.22f, 1.0f);
        w = severity * ang_w * radial_w;
        if (w <= 0.0f) continue;

        jitter = s_randf() - 0.5f;
        a_scale = 1.0f + tangential_sign * (0.030f + 0.028f * r_norm) * w
                        + 0.012f * jitter * w;
        a_scale = clampf(a_scale, DAMAGE_MIN_A_SCALE, DAMAGE_MAX_A_SCALE);
        p[1] = clampf(p[1] * a_scale, a0 * DAMAGE_MIN_A_SCALE, a0 * DAMAGE_MAX_A_SCALE);

        p[2] = fminf(DAMAGE_MAX_ECC,
                     p[2] + (0.010f + 0.040f * severity) * w
                          + fabsf(jitter) * 0.008f * w);
        p[3] += tangential_sign * (0.050f + 0.115f * severity) * w
              + jitter * 0.055f * w;
        p[4] += (vertical_sign * 0.22f + jitter * 0.28f) * puff_scale * w;

        {
            double a_m = (double)p[1] * AU;
            n_arr[i] = (float)sqrt(gm / (a_m * a_m * a_m));
        }
    }
}

static void apply_disc_damage(ParticleDisc *d, int body_idx, double rel_speed,
                              float phase, float half_width,
                              float severity, const double rel_vel[3])
{
    float tangent_dir[3];
    float tangential = 0.0f;
    float vertical = 0.0f;
    float tangential_sign;
    float vertical_sign;
    double gm;
    uint32_t seed;

    if (!d || !d->initialized || d->parent_idx != body_idx) return;
    if (body_idx < 0 || body_idx >= g_nbodies || !g_bodies[body_idx].alive) return;

    gm = G_CONST * g_bodies[body_idx].mass;
    if (gm <= 0.0) return;

    tangent_dir[0] = -sinf(phase) * d->b1[0] + cosf(phase) * d->b2[0];
    tangent_dir[1] = -sinf(phase) * d->b1[1] + cosf(phase) * d->b2[1];
    tangent_dir[2] = -sinf(phase) * d->b1[2] + cosf(phase) * d->b2[2];
    if (rel_vel) {
        tangential = (float)(rel_vel[0] * tangent_dir[0] +
                             rel_vel[1] * tangent_dir[1] +
                             rel_vel[2] * tangent_dir[2]);
        vertical = (float)(rel_vel[0] * d->pole[0] +
                           rel_vel[1] * d->pole[1] +
                           rel_vel[2] * d->pole[2]);
    }
    tangential_sign = tangential >= 0.0f ? 1.0f : -1.0f;
    vertical_sign = vertical >= 0.0f ? 1.0f : -1.0f;
    seed = damage_seed_for_disc(d, body_idx, rel_speed, phase);

    apply_damage_to_array(d, d->data_full, d->n_arr_full, d->n_full, gm,
                          phase, half_width, severity, tangential_sign,
                          vertical_sign, seed);
    apply_damage_to_array(d, d->data_lod, d->n_arr_lod, d->n_lod, gm,
                          phase, half_width, severity, tangential_sign,
                          vertical_sign, seed ^ 0x7f4a7c15u);
}

/*
 * Ring particle collision despawn helpers.
 *
 * Confirmed sphere/ring contact samples can tombstone swallowed particles
 * without compacting the fixed-size render buffers.
 */
/*
 * despawn_contact_particles_array - remove particles swallowed by a planet.
 *
 * A negative semi-major axis is used as an immutable tombstone.  That keeps
 * the VBO shape and draw count stable while allowing the shader to skip the
 * particle entirely.  This pass is only run for confirmed sphere/ring contact
 * samples, not every frame.
 */
static int despawn_contact_particles_array(const ParticleDisc *d,
                                           float *data, float *n_arr, int n,
                                           double ru, double rv, double rh,
                                           double other_r_km,
                                           float severity, uint32_t seed)
{
    double cu = ru * RS;
    double cv = rv * RS;
    double cn = rh * RS;
    double radius_au = other_r_km / 1.496e8;
    double ring_width_au;
    double core_r, soft_r;
    int killed = 0;

    if (!d || !data || !n_arr || n <= 0 || radius_au <= 0.0) return 0;

    ring_width_au = fmax((double)(d->ring_r_outer_km - d->ring_r_inner_km) / 1.496e8, 1e-9);
    core_r = radius_au * (0.96 + 0.04 * clampf(severity, 0.0f, 1.0f));
    soft_r = radius_au * 1.08 + ring_width_au * 0.012;
    if (soft_r < core_r) soft_r = core_r;

    s_seed(seed);
    for (int i = 0; i < n; i++) {
        float *p = data + i * 8;
        float a = p[1];
        float sinM, cosM, nu, r, phi;
        double du, dv, dn, dist;
        int remove_particle = 0;

        if (a <= 0.0f) continue;

        sinM = sinf(p[0]);
        cosM = cosf(p[0]);
        nu = p[0] + 2.0f * p[2] * sinM;
        r = a * (1.0f - p[2] * cosM);
        phi = nu + p[3];

        du = (double)(r * cosf(phi)) - cu;
        dv = (double)(r * sinf(phi)) - cv;
        dn = (double)p[4] - cn;
        dist = sqrt(du*du + dv*dv + dn*dn);

        if (dist <= core_r) {
            remove_particle = 1;
        } else if (dist < soft_r) {
            double t = (dist - core_r) / fmax(soft_r - core_r, 1e-12);
            double chance = (1.0 - t * t * (3.0 - 2.0 * t))
                          * (0.45 + 0.40 * clampf(severity, 0.0f, 1.0f));
            remove_particle = s_randf() < (float)chance;
        }

        if (!remove_particle) continue;

        p[1] = -fabsf(a);
        p[2] = 0.0f;
        p[4] = 0.0f;
        p[5] = 0.0f;
        p[6] = 0.0f;
        p[7] = 0.0f;
        n_arr[i] = 0.0f;
        killed++;
    }
    return killed;
}

static int despawn_disc_contact_particles(ParticleDisc *d,
                                          int other_idx, double rel_speed,
                                          double ru, double rv, double rh,
                                          double other_r_km, float severity)
{
    float phase;
    uint32_t seed;
    int killed = 0;

    if (!d || !d->initialized || other_idx < 0 || other_idx >= g_nbodies) return 0;
    phase = atan2f((float)rv, (float)ru);
    seed = damage_seed_for_disc(d, other_idx, rel_speed, phase);

    killed += despawn_contact_particles_array(d, d->data_full, d->n_arr_full, d->n_full,
                                              ru, rv, rh, other_r_km, severity, seed);
    killed += despawn_contact_particles_array(d, d->data_lod, d->n_arr_lod, d->n_lod,
                                              ru, rv, rh, other_r_km, severity,
                                              seed ^ 0x4d9f3b21u);
    return killed;
}

/*
 * apply_tidal_gravity_to_array - cheap 3D perturbation from a nearby body.
 *
 * This applies differential acceleration: the pull on each ring particle minus
 * the pull on the parent planet.  The result is projected into radial,
 * tangential, and normal components and folded back into the compact Keplerian
 * particle state.  It is only called for nearby bodies, so the O(N) pass stays
 * out of the normal frame path.
 */
static void apply_tidal_gravity_to_array(ParticleDisc *d,
                                         float *data, float *n_arr, int n,
                                         int other_idx, double dt,
                                         const double rel[3],
                                         double influence_m)
{
    const Body *other;
    double gm_parent;
    double gm_other;
    double rel_len2;
    double rel_inv3;
    double parent_acc[3];
    double dt_eff;
    double ring_width_au;

    if (!d || !data || !n_arr || n <= 0 || other_idx < 0 || other_idx >= g_nbodies) return;
    if (d->parent_idx < 0 || d->parent_idx >= g_nbodies) return;
    other = &g_bodies[other_idx];
    if (!other->alive || other->mass <= 0.0) return;

    gm_parent = G_CONST * g_bodies[d->parent_idx].mass;
    gm_other = G_CONST * other->mass;
    if (gm_parent <= 0.0 || gm_other <= 0.0 || influence_m <= 1.0) return;

    rel_len2 = rel[0]*rel[0] + rel[1]*rel[1] + rel[2]*rel[2]
             + fmax(other->radius * other->radius * 0.04, SOFTENING * SOFTENING);
    rel_inv3 = 1.0 / (sqrt(rel_len2) * rel_len2);
    parent_acc[0] = gm_other * rel[0] * rel_inv3;
    parent_acc[1] = gm_other * rel[1] * rel_inv3;
    parent_acc[2] = gm_other * rel[2] * rel_inv3;

    dt_eff = dt > TIDAL_MAX_DT ? TIDAL_MAX_DT : dt;
    if (dt_eff <= 0.0) return;
    ring_width_au = fmax((double)(d->ring_r_outer_km - d->ring_r_inner_km) / 1.496e8, 1e-9);

    for (int i = 0; i < n; i++) {
        float *p = data + i * 8;
        double theta = (double)p[0] + (double)p[3];
        double c = cos(theta);
        double s = sin(theta);
        double a_m;
        if (p[1] <= 0.0f) continue;
        a_m = (double)p[1] * AU;
        double h_m = (double)p[4] * AU;
        double radial[3] = {
            c * d->b1[0] + s * d->b2[0],
            c * d->b1[1] + s * d->b2[1],
            c * d->b1[2] + s * d->b2[2]
        };
        double tangential[3] = {
            -s * d->b1[0] + c * d->b2[0],
            -s * d->b1[1] + c * d->b2[1],
            -s * d->b1[2] + c * d->b2[2]
        };
        double particle_pos[3] = {
            a_m * radial[0] + h_m * d->pole[0],
            a_m * radial[1] + h_m * d->pole[1],
            a_m * radial[2] + h_m * d->pole[2]
        };
        double q[3] = {
            rel[0] - particle_pos[0],
            rel[1] - particle_pos[1],
            rel[2] - particle_pos[2]
        };
        double q_len2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2]
                      + fmax(other->radius * other->radius * 0.04, SOFTENING * SOFTENING);
        double q_len = sqrt(q_len2);
        double weight = clampf((float)((influence_m - q_len) / fmax(influence_m, 1.0)), 0.0f, 1.0f);
        double q_inv3;
        double tidal[3];
        double ar, at, an;
        double v_orb;
        double parent_grav;
        double accel_cap;
        float da_frac;
        float de;
        float dh_au;

        if (weight <= 0.0) continue;
        weight = weight * weight * (3.0 - 2.0 * weight);
        q_inv3 = 1.0 / (q_len * q_len2);
        tidal[0] = (gm_other * q[0] * q_inv3 - parent_acc[0]) * weight * TIDAL_VISUAL_GAIN;
        tidal[1] = (gm_other * q[1] * q_inv3 - parent_acc[1]) * weight * TIDAL_VISUAL_GAIN;
        tidal[2] = (gm_other * q[2] * q_inv3 - parent_acc[2]) * weight * TIDAL_VISUAL_GAIN;

        parent_grav = gm_parent / fmax(a_m * a_m, 1.0);
        accel_cap = parent_grav * 0.060;
        for (int k = 0; k < 3; k++) {
            if (tidal[k] >  accel_cap) tidal[k] =  accel_cap;
            if (tidal[k] < -accel_cap) tidal[k] = -accel_cap;
        }

        ar = tidal[0]*radial[0] + tidal[1]*radial[1] + tidal[2]*radial[2];
        at = tidal[0]*tangential[0] + tidal[1]*tangential[1] + tidal[2]*tangential[2];
        an = tidal[0]*d->pole[0] + tidal[1]*d->pole[1] + tidal[2]*d->pole[2];
        v_orb = sqrt(gm_parent / fmax(a_m, 1.0));
        if (v_orb <= 1e-6) continue;

        da_frac = clampf((float)(2.0 * at * dt_eff / v_orb), -TIDAL_MAX_DA_FRAC, TIDAL_MAX_DA_FRAC);
        de = clampf((float)(fabs(ar) * dt_eff / v_orb * 0.055), 0.0f, TIDAL_MAX_DE);
        dh_au = clampf((float)(an * dt_eff / v_orb * p[1] * 0.070),
                       (float)(-ring_width_au * 0.004),
                       (float)( ring_width_au * 0.004));

        {
            float old_a = p[1];
            p[1] = clampf(old_a * (1.0f + da_frac),
                          old_a * 0.9985f,
                          old_a * 1.0015f);
        }
        p[2] = fminf(DAMAGE_MAX_ECC, p[2] + de);
        p[3] += clampf((float)(ar * dt_eff / v_orb * 0.010), -0.0025f, 0.0025f);
        p[4] += dh_au;

        {
            double new_a_m = (double)p[1] * AU;
            n_arr[i] = (float)sqrt(gm_parent / (new_a_m * new_a_m * new_a_m));
        }
    }
}

static void apply_disc_tidal_gravity(ParticleDisc *d, int other_idx, double dt,
                                     const double rel[3], double influence_m)
{
    if (!d || !d->initialized) return;
    apply_tidal_gravity_to_array(d, d->data_full, d->n_arr_full, d->n_full,
                                 other_idx, dt, rel, influence_m);
    apply_tidal_gravity_to_array(d, d->data_lod, d->n_arr_lod, d->n_lod,
                                 other_idx, dt, rel, influence_m);
}

static void damage_disc(ParticleDisc *d, int body_idx, double rel_speed,
                        const double dir[3], const double rel_vel[3])
{
    float phase;
    float severity;
    float half_width;
    float overlap;

    if (!d || !d->initialized || d->parent_idx != body_idx) return;
    if (body_idx < 0 || body_idx >= g_nbodies || !g_bodies[body_idx].alive) return;

    phase = damage_phase_from_world_dir(d, dir, rel_vel);
    severity = clampf((float)(rel_speed / 15000.0), 0.18f, 1.0f);
    half_width = clampf(0.34f + 0.42f * severity, DAMAGE_MIN_WIDTH, DAMAGE_MAX_WIDTH);
    overlap = 0.85f;

    disc_drive_response(d, phase, half_width, severity, overlap, 0.5f, 0.65f);
    apply_disc_damage(d, body_idx, rel_speed, phase, half_width,
                      0.50f + 0.45f * severity, rel_vel);
}

static int disc_probe_contact_sample(ParticleDisc *d, int other_idx,
                                     double rel_speed, double sample_t)
{
    Body *parent;
    Body *other;
    double rel[3], rel_vel[3];
    double ru, rv, rh;
    double proj_r_km, plane_h_km, other_r_km, cross_r_km;
    double ring_width_km;
    float phase, half_width, inner_norm, outer_norm;
    float radial_overlap, plane_overlap, overlap, severity;
    float sector_cooldown;

    if (!d || !d->initialized || other_idx < 0 || other_idx >= g_nbodies) return 0;
    if (d->parent_idx < 0 || d->parent_idx >= g_nbodies) return 0;
    if (!body_is_ring_perturber_planet(other_idx)) return 0;

    parent = &g_bodies[d->parent_idx];
    other = &g_bodies[other_idx];
    if (!parent->alive || !other->alive || other->is_star) return 0;

    rel_vel[0] = other->vel[0] - parent->vel[0];
    rel_vel[1] = other->vel[1] - parent->vel[1];
    rel_vel[2] = other->vel[2] - parent->vel[2];
    rel[0] = (other->pos[0] - parent->pos[0]) + rel_vel[0] * sample_t;
    rel[1] = (other->pos[1] - parent->pos[1]) + rel_vel[1] * sample_t;
    rel[2] = (other->pos[2] - parent->pos[2]) + rel_vel[2] * sample_t;

    ru = rel[0] * d->b1[0] + rel[1] * d->b1[1] + rel[2] * d->b1[2];
    rv = rel[0] * d->b2[0] + rel[1] * d->b2[1] + rel[2] * d->b2[2];
    rh = rel[0] * d->pole[0] + rel[1] * d->pole[1] + rel[2] * d->pole[2];

    proj_r_km = sqrt(ru*ru + rv*rv) * 0.001;
    plane_h_km = fabs(rh) * 0.001;
    other_r_km = collision_visual_radius(other_idx, other->radius) * 0.001;
    if (other_r_km <= 1e-6) return 0;
    if (plane_h_km >= other_r_km) return 0;

    cross_r_km = sqrt(other_r_km*other_r_km - plane_h_km*plane_h_km);
    if (proj_r_km + cross_r_km < d->ring_r_inner_km) return 0;
    if (proj_r_km - cross_r_km > d->ring_r_outer_km * fmaxf(d->scale_cur, 1.0f)) return 0;

    ring_width_km = fmax(d->ring_r_outer_km - d->ring_r_inner_km, 1.0);
    phase = atan2f((float)rv, (float)ru);
    if (proj_r_km <= cross_r_km + 1.0)
        half_width = (float)PI;
    else
        half_width = asinf(clampf((float)(cross_r_km / proj_r_km), 0.0f, 1.0f));

    inner_norm = clampf((float)((proj_r_km - cross_r_km - d->ring_r_inner_km) / ring_width_km),
                        0.0f, 1.0f);
    outer_norm = clampf((float)((proj_r_km + cross_r_km - d->ring_r_inner_km) / ring_width_km),
                        0.0f, 1.0f);

    sector_cooldown = HIT_SEGMENT_COOLDOWN_SECONDS;
    if (d->mean_motion_mid > 1e-8f) {
        float sector_time = ((2.0f * (float)PI) / (float)RING_COLLISION_SEGMENTS) / d->mean_motion_mid;
        sector_cooldown = clampf(sector_time * 0.28f, 60.0f * 8.0f, (float)(DAY * 0.20));
    }

    {
        const float TWO_PI = 6.28318530718f;
        const float SEG_W = TWO_PI / (float)RING_COLLISION_SEGMENTS;
        const float RADIAL_BIN_W = 1.0f / (float)RING_COLLISION_RADIAL_BINS;
        int radial_first = (int)floorf(inner_norm / RADIAL_BIN_W);
        int radial_last  = (int)floorf((outer_norm - 1e-4f) / RADIAL_BIN_W);
        if (radial_first < 0) radial_first = 0;
        if (radial_last >= RING_COLLISION_RADIAL_BINS) radial_last = RING_COLLISION_RADIAL_BINS - 1;
        if (radial_last < radial_first) radial_last = radial_first;

        for (int i = 0; i < RING_COLLISION_SEGMENTS; i++) {
            float seg_phase = -((float)PI) + ((float)i + 0.5f) * SEG_W;
            float dphi = wrap_angle_pi(seg_phase - phase);
            if (angular_falloff(dphi, half_width + SEG_W * 0.65f) <= 0.0f)
                continue;

            for (int rbin = radial_first; rbin <= radial_last; rbin++) {
                int cell = i * RING_COLLISION_RADIAL_BINS + rbin;
                d->hit_cooldown[cell] = sector_cooldown;
            }
        }
    }

    radial_overlap = clampf((float)((fmin(proj_r_km + cross_r_km, d->ring_r_outer_km) -
                                     fmax(proj_r_km - cross_r_km, d->ring_r_inner_km))
                                    / ring_width_km),
                            0.0f, 1.0f);
    plane_overlap = clampf(1.0f - (float)(plane_h_km / other_r_km), 0.0f, 1.0f);
    overlap = clampf(0.25f + 0.75f * plane_overlap * (0.35f + 0.65f * radial_overlap),
                     0.0f, 1.0f);
    {
        float speed_severity = clampf((float)(rel_speed / 18000.0), 0.08f, 0.85f);
        float size_severity = clampf((float)(cross_r_km / fmax(ring_width_km * 0.20, 1.0)), 0.0f, 0.85f);
        severity = clampf(speed_severity * 0.55f + size_severity * 0.65f, 0.10f, 0.95f);
    }

    {
        float radial_center = clampf((float)((proj_r_km - d->ring_r_inner_km) / ring_width_km),
                                     0.0f, 1.0f);
        float radial_width = clampf((float)(cross_r_km / ring_width_km) * 1.35f,
                                    0.08f, 0.85f);
        int killed = despawn_disc_contact_particles(d, other_idx, rel_speed,
                                                    ru, rv, rh, other_r_km,
                                                    severity);
        if (killed > 0) {
            severity = clampf(severity + 0.08f, 0.0f, 1.0f);
            overlap = clampf(overlap + 0.10f, 0.0f, 1.0f);
        }
        disc_drive_response(d,
                            phase,
                            fminf(half_width + (2.0f * (float)PI / (float)RING_COLLISION_SEGMENTS) * 0.75f,
                                  (float)PI * 0.92f),
                            severity,
                            overlap,
                            radial_center,
                            radial_width);
    }
    return 1;
}

static void add_contact_time(double *times, int *count, double t, double dt)
{
    if (!times || !count) return;
    if (t < -dt) t = -dt;
    if (t > 0.0) t = 0.0;
    for (int i = 0; i < *count; i++) {
        if (fabs(times[i] - t) < dt * 0.015 + 1e-6) return;
    }
    if (*count < 16) {
        times[*count] = t;
        (*count)++;
    }
}

static void add_radial_crossing_times(double *times, int *count, double dt,
                                      double ru, double rv,
                                      double vu, double vv,
                                      double radius_m)
{
    double a = vu*vu + vv*vv;
    double b = 2.0 * (ru*vu + rv*vv);
    double c = ru*ru + rv*rv - radius_m*radius_m;
    double disc;

    if (radius_m <= 0.0 || a <= 1e-12) return;
    disc = b*b - 4.0*a*c;
    if (disc < 0.0) return;
    disc = sqrt(disc);
    add_contact_time(times, count, (-b - disc) / (2.0*a), dt);
    add_contact_time(times, count, (-b + disc) / (2.0*a), dt);
}

static void update_disc_swept_contact(ParticleDisc *d, int other_idx, double dt)
{
    Body *parent;
    Body *other;
    double rel[3], rel_vel[3];
    double ru, rv, rh, vu, vv, vh;
    double proj_r_km, h_km, other_r_km, v_mps, speed, reach_km;
    double ring_width_km, tidal_reach_km, motion_km;
    double times[16];
    int time_count = 0;

    if (!d || !d->initialized || dt <= 0.0) return;
    if (d->parent_idx < 0 || d->parent_idx >= g_nbodies) return;
    if (other_idx < 0 || other_idx >= g_nbodies || other_idx == d->parent_idx) return;
    if (!body_is_ring_perturber_planet(other_idx)) return;

    parent = &g_bodies[d->parent_idx];
    other = &g_bodies[other_idx];
    if (!parent->alive || !other->alive || other->is_star) return;

    rel[0] = other->pos[0] - parent->pos[0];
    rel[1] = other->pos[1] - parent->pos[1];
    rel[2] = other->pos[2] - parent->pos[2];
    rel_vel[0] = other->vel[0] - parent->vel[0];
    rel_vel[1] = other->vel[1] - parent->vel[1];
    rel_vel[2] = other->vel[2] - parent->vel[2];
    speed = sqrt(rel_vel[0]*rel_vel[0] + rel_vel[1]*rel_vel[1] + rel_vel[2]*rel_vel[2]);

    ru = rel[0] * d->b1[0] + rel[1] * d->b1[1] + rel[2] * d->b1[2];
    rv = rel[0] * d->b2[0] + rel[1] * d->b2[1] + rel[2] * d->b2[2];
    rh = rel[0] * d->pole[0] + rel[1] * d->pole[1] + rel[2] * d->pole[2];
    vu = rel_vel[0] * d->b1[0] + rel_vel[1] * d->b1[1] + rel_vel[2] * d->b1[2];
    vv = rel_vel[0] * d->b2[0] + rel_vel[1] * d->b2[1] + rel_vel[2] * d->b2[2];
    vh = rel_vel[0] * d->pole[0] + rel_vel[1] * d->pole[1] + rel_vel[2] * d->pole[2];
    proj_r_km = sqrt(ru*ru + rv*rv) * 0.001;
    h_km = fabs(rh) * 0.001;
    other_r_km = collision_visual_radius(other_idx, other->radius) * 0.001;
    v_mps = speed;
    motion_km = v_mps * dt * 0.001;
    reach_km = other_r_km + motion_km;
    ring_width_km = fmax(d->ring_r_outer_km - d->ring_r_inner_km, 1.0);
    tidal_reach_km = fmax(other_r_km * 8.0, ring_width_km * 1.35) + motion_km;

    if (h_km > tidal_reach_km) return;
    if (proj_r_km + tidal_reach_km < d->ring_r_inner_km) return;
    if (proj_r_km - tidal_reach_km > d->ring_r_outer_km * fmaxf(d->scale_cur, 1.0f)) return;

    disc_drive_tide_response(d, ru, rv, rh, proj_r_km, h_km,
                             other_r_km, tidal_reach_km);
    apply_disc_tidal_gravity(d, other_idx, dt, rel, tidal_reach_km * 1000.0);

    if (h_km > reach_km) return;
    if (proj_r_km + reach_km < d->ring_r_inner_km) return;
    if (proj_r_km - reach_km > d->ring_r_outer_km * fmaxf(d->scale_cur, 1.0f)) return;

    add_contact_time(times, &time_count, 0.0, dt);
    add_contact_time(times, &time_count, -dt, dt);

    if (fabs(vh) > 1e-9) {
        double r_m = other_r_km * 1000.0;
        add_contact_time(times, &time_count, -rh / vh, dt);
        add_contact_time(times, &time_count, ( r_m - rh) / vh, dt);
        add_contact_time(times, &time_count, (-r_m - rh) / vh, dt);
    }

    {
        double radial_v2 = vu*vu + vv*vv;
        if (radial_v2 > 1e-12)
            add_contact_time(times, &time_count, -(ru*vu + rv*vv) / radial_v2, dt);
    }

    {
        double inner_m = d->ring_r_inner_km * 1000.0;
        double outer_m = d->ring_r_outer_km * fmaxf(d->scale_cur, 1.0f) * 1000.0;
        double body_m = other_r_km * 1000.0;
        add_radial_crossing_times(times, &time_count, dt, ru, rv, vu, vv, inner_m - body_m);
        add_radial_crossing_times(times, &time_count, dt, ru, rv, vu, vv, inner_m + body_m);
        add_radial_crossing_times(times, &time_count, dt, ru, rv, vu, vv, outer_m - body_m);
        add_radial_crossing_times(times, &time_count, dt, ru, rv, vu, vv, outer_m + body_m);
    }

    if (time_count < RING_SWEEP_MAX_SAMPLES) {
        int needed = RING_SWEEP_MAX_SAMPLES - time_count;
        for (int s = 1; s <= needed; s++) {
            double t = -dt + dt * ((double)s / (double)(needed + 1));
            add_contact_time(times, &time_count, t, dt);
        }
    }

    for (int s = 0; s < time_count; s++) {
        disc_probe_contact_sample(d, other_idx, speed, times[s]);
    }
}

/* Set up the 8-float-per-particle VAO attribute pointers (loc 0..5).
 * Locations 0..4 are individual floats; loc 5 is vec3 (rgb). */
static void setup_particle_attribs(void) {
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(0*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(1*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(4*sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(5*sizeof(float)));
}

/* Compile shaders and upload initial particle data for one disc. */
static void init_disc_gl(ParticleDisc *d)
{
    /* ring.vert solves the Kepler equation per-particle on the GPU */
    d->shader = gl_shader_load("assets/shaders/ring.vert",
                               "assets/shaders/color.frag");
    if (!d->shader) return;
    d->loc_vp     = glGetUniformLocation(d->shader, "u_vp");
    d->loc_center = glGetUniformLocation(d->shader, "u_center");
    d->loc_b1     = glGetUniformLocation(d->shader, "u_b1");
    d->loc_b2     = glGetUniformLocation(d->shader, "u_b2");
    d->loc_pole   = glGetUniformLocation(d->shader, "u_pole");
    d->loc_morph0 = glGetUniformLocation(d->shader, "u_morph0");
    d->loc_morph1 = glGetUniformLocation(d->shader, "u_morph1");
    d->loc_morph2 = glGetUniformLocation(d->shader, "u_morph2");
    d->loc_tide0  = glGetUniformLocation(d->shader, "u_tide0");
    d->loc_tide1  = glGetUniformLocation(d->shader, "u_tide1");
    d->loc_body0  = glGetUniformLocation(d->shader, "u_body0");
    d->loc_body1  = glGetUniformLocation(d->shader, "u_body1");

    /* Full-count VAO/VBO */
    d->vao_full = gl_vao_create();
    d->vbo_full = gl_vbo_create(d->n_full * 8 * sizeof(float),
                                d->data_full, GL_DYNAMIC_DRAW);
    setup_particle_attribs();
    glBindVertexArray(0);

    /* LOD VAO/VBO */
    d->vao_lod = gl_vao_create();
    d->vbo_lod = gl_vbo_create(d->n_lod * 8 * sizeof(float),
                               d->data_lod, GL_DYNAMIC_DRAW);
    setup_particle_attribs();
    glBindVertexArray(0);

    /* Sprite shader: "saturn" → textured disc; anything else → procedural */
    const char *frag = d->use_generic_sprite
                       ? "assets/shaders/ring_sprite_generic.frag"
                       : "assets/shaders/ring_sprite.frag";
    d->sprite_shader = gl_shader_load("assets/shaders/ring_sprite.vert", frag);
    if (!d->sprite_shader) return;

    d->sp_loc_vp     = glGetUniformLocation(d->sprite_shader, "u_vp");
    d->sp_loc_center = glGetUniformLocation(d->sprite_shader, "u_center");
    d->sp_loc_b1     = glGetUniformLocation(d->sprite_shader, "u_b1");
    d->sp_loc_b2     = glGetUniformLocation(d->sprite_shader, "u_b2");
    d->sp_loc_morph0 = glGetUniformLocation(d->sprite_shader, "u_morph0");
    d->sp_loc_morph1 = glGetUniformLocation(d->sprite_shader, "u_morph1");
    d->sp_loc_morph2 = glGetUniformLocation(d->sprite_shader, "u_morph2");
    d->sp_loc_tide0  = glGetUniformLocation(d->sprite_shader, "u_tide0");
    d->sp_loc_tide1  = glGetUniformLocation(d->sprite_shader, "u_tide1");

    if (d->use_generic_sprite) {
        d->sp_loc_r_inner    = glGetUniformLocation(d->sprite_shader, "u_r_inner_km");
        d->sp_loc_r_outer    = glGetUniformLocation(d->sprite_shader, "u_r_outer_km");
        d->sp_loc_ring_color = glGetUniformLocation(d->sprite_shader, "u_ring_color");
        d->sp_loc_alpha_max  = glGetUniformLocation(d->sprite_shader, "u_alpha_max");
    }

    /* Sprite quad VBO: 6 vertices × 3 floats (one quad, two triangles) */
    d->sprite_vao = gl_vao_create();
    d->sprite_vbo = gl_vbo_create(6 * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glBindVertexArray(0);
}

/*
 * build_sprite_quad — construct a flat ring disc quad in world space.
 *
 * The disc lies in the ring plane (spanned by b1, b2).  The quad extends
 * from (cx−R, cy−R) to (cx+R, cy+R) in ring-plane coordinates.  R is set
 * to the disc's outer sprite radius so the sprite covers the full ring extent.
 */
static void build_sprite_quad(float *verts,
                               float cx, float cy, float cz,
                               float R,
                               const float b1[3], const float b2[3])
{
    float c[4][3];
    for (int i = 0; i < 4; i++) {
        float sb1 = (i == 1 || i == 2) ?  R : -R;
        float sb2 = (i == 2 || i == 3) ?  R : -R;
        c[i][0] = cx + sb1*b1[0] + sb2*b2[0];
        c[i][1] = cy + sb1*b1[1] + sb2*b2[1];
        c[i][2] = cz + sb1*b1[2] + sb2*b2[2];
    }
    memcpy(verts+0,  c[0], 12);
    memcpy(verts+3,  c[1], 12);
    memcpy(verts+6,  c[2], 12);
    memcpy(verts+9,  c[0], 12);
    memcpy(verts+12, c[2], 12);
    memcpy(verts+15, c[3], 12);
}

/*
 * render_disc — render one ring disc at the appropriate LOD.
 *
 * Camera-relative distance (computed in double → float) determines which path:
 *
 *   SPRITE path (dist > SPRITE_DIST):
 *     Build a flat quad in the ring plane and draw it with the sprite shader.
 *     This replaces thousands of individual particles with one textured disc
 *     that is indistinguishable at the viewing distance.
 *
 *   PARTICLE path (dist ≤ SPRITE_DIST):
 *     Upload updated mean anomaly data (M0 array) to the VBO via glBufferSubData.
 *     ring.vert solves Kepler's equation on the GPU and positions each particle.
 *     Full particle count within LOD_DIST; reduced count beyond that.
 */
static void render_disc(const ParticleDisc *d, const float vp[16])
{
    Body *par = &g_bodies[d->parent_idx];
    float px = (float)(par->pos[0] * RS);
    float py = (float)(par->pos[1] * RS);
    float pz = (float)(par->pos[2] * RS);
    /* Camera-relative distance in double → float to avoid float32 cancellation */
    float dx   = (float)(par->pos[0] * RS - g_cam.pos[0]);
    float dy   = (float)(par->pos[1] * RS - g_cam.pos[1]);
    float dz   = (float)(par->pos[2] * RS - g_cam.pos[2]);
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);

    if (dist > SPRITE_DIST) {
        if (!d->sprite_shader) return;

        float quad[18];
        build_sprite_quad(quad, px, py, pz, d->sprite_r * fmaxf(d->scale_cur, 1.0f), d->b1, d->b2);

        glUseProgram(d->sprite_shader);
        glUniformMatrix4fv(d->sp_loc_vp,     1, GL_FALSE, vp);
        glUniform3f       (d->sp_loc_center,  px, py, pz);
        glUniform3fv      (d->sp_loc_b1,    1, d->b1);
        glUniform3fv      (d->sp_loc_b2,    1, d->b2);
        glUniform4f       (d->sp_loc_morph0,
                           d->scale_cur,
                           d->puff_cur,
                           d->shock_amp,
                           d->shock_phase);
        glUniform4f       (d->sp_loc_morph1,
                           d->shock_width,
                           d->shock_spin,
                           d->ring_r_inner_km,
                           d->ring_r_outer_km);
        glUniform4f       (d->sp_loc_morph2,
                           d->contact_norm,
                           d->contact_width,
                           d->contact_strength,
                           0.0f);
        glUniform4f       (d->sp_loc_tide0,
                           d->tide_phase,
                           d->tide_radius_norm,
                           d->tide_width,
                           d->tide_strength);
        glUniform4f       (d->sp_loc_tide1,
                           d->tide_dir_u,
                           d->tide_dir_v,
                           d->tide_dir_n,
                           0.0f);

        if (d->use_generic_sprite) {
            glUniform1f (d->sp_loc_r_inner,    d->sp_r_inner_km);
            glUniform1f (d->sp_loc_r_outer,    d->sp_r_outer_km);
            glUniform3fv(d->sp_loc_ring_color, 1, d->sp_color);
            glUniform1f (d->sp_loc_alpha_max,  d->sp_alpha_max);
        }

        glBindVertexArray(d->sprite_vao);
        glBindBuffer(GL_ARRAY_BUFFER, d->sprite_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
        glBindVertexArray(0);

    } else {
        if (!d->shader) return;

        /* Select full or LOD particle buffer by distance */
        int    n   = (dist > LOD_DIST) ? d->n_lod    : d->n_full;
        float *data= (dist > LOD_DIST) ? d->data_lod  : d->data_full;
        GLuint vao = (dist > LOD_DIST) ? d->vao_lod   : d->vao_full;
        GLuint vbo = (dist > LOD_DIST) ? d->vbo_lod   : d->vbo_full;

        glUseProgram(d->shader);
        glUniformMatrix4fv(d->loc_vp,     1, GL_FALSE, vp);
        glUniform3f       (d->loc_center,  px, py, pz);
        glUniform3fv      (d->loc_b1,    1, d->b1);
        glUniform3fv      (d->loc_b2,    1, d->b2);
        glUniform3fv      (d->loc_pole,  1, d->pole);
        glUniform4f       (d->loc_morph0,
                           d->scale_cur,
                           d->puff_cur,
                           d->shock_amp,
                           d->shock_phase);
        glUniform4f       (d->loc_morph1,
                           d->shock_width,
                           d->shock_spin,
                           d->ring_r_inner_km / 1.496e8f,
                           d->ring_r_outer_km / 1.496e8f);
        glUniform4f       (d->loc_morph2,
                           d->contact_norm,
                           d->contact_width,
                           d->contact_strength,
                           0.0f);
        glUniform4f       (d->loc_tide0,
                           d->tide_phase,
                           d->tide_radius_norm,
                           d->tide_width,
                           d->tide_strength);
        glUniform4f       (d->loc_tide1,
                           d->tide_dir_u,
                           d->tide_dir_v,
                           d->tide_dir_n,
                           0.0f);
        glUniform4f       (d->loc_body0,
                           d->body_u_au,
                           d->body_v_au,
                           d->body_n_au,
                           d->body_radius_au);
        glUniform4f       (d->loc_body1,
                           d->body_strength,
                           (float)(collision_visual_radius(d->parent_idx, par->radius) * RS),
                           0.0f,
                           0.0f);

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        /* Upload updated M0 values (and unchanged a,e,omega,h,rgb) each frame */
        glBufferSubData(GL_ARRAY_BUFFER, 0, n * 8 * sizeof(float), data);

        glEnable(GL_DEPTH_TEST);
        glPointSize(1.0f);
        glDrawArrays(GL_POINTS, 0, n);
        glBindVertexArray(0);
    }
}

/* ── public API ─────────────────────────────────────────────────────────── */

/*
 * rings_init — parse "rings" array from universe.json and build all disc data.
 *
 * For each ring entry:
 *   1. Find the parent body by name.
 *   2. Parse zones (annulus bands with density, color, radii).
 *   3. Allocate particle arrays (full and LOD counts).
 *   4. Build ring-plane basis from parent obliquity.
 *   5. Bake particle orbits (bake_particles) for both full and LOD arrays.
 *   6. Upload to GPU (init_disc_gl).
 */
void rings_init(const char *path)
{
    JsonNode *root = json_parse_file(path);
    if (!root) {
        fprintf(stderr, "[Rings] cannot parse '%s'\n", path);
        return;
    }

    JsonNode *rings_arr = json_get(root, "rings");
    if (!rings_arr || rings_arr->type != JSON_ARRAY) {
        fprintf(stderr, "[Rings] no 'rings' array in '%s' — skipping\n", path);
        json_free(root);
        return;
    }

    int count = 0;
    { JsonNode *n = rings_arr->first_child; while (n) { count++; n = n->next; } }
    if (count == 0) { json_free(root); return; }

    s_discs   = (ParticleDisc*)calloc(count, sizeof(ParticleDisc));
    s_n_discs = 0;
    if (!s_discs) { json_free(root); return; }

    JsonNode *rnode = rings_arr->first_child;
    while (rnode) {
        const char *body_name   = json_str(json_get(rnode, "body"),        "");
        const char *shader_type = json_str(json_get(rnode, "shader_type"), "generic");
        int    n_full      = (int)json_num(json_get(rnode, "n_full"),       1000);
        int    n_lod       = (int)json_num(json_get(rnode, "n_lod"),         200);
        uint32_t seed_full = (uint32_t)json_num(json_get(rnode, "seed_full"), 1234);
        uint32_t seed_lod  = (uint32_t)json_num(json_get(rnode, "seed_lod"),  5678);
        float  e_max       = (float)json_num(json_get(rnode, "e_max"),       0.01);
        float  h_scale     = (float)json_num(json_get(rnode, "h_scale"),     1e-6);
        float  sprite_r    = (float)json_num(json_get(rnode, "sprite_r_au"), 0.001);

        int par_idx = -1;
        for (int i = 0; i < g_nbodies; i++) {
            if (strcmp(g_bodies[i].name, body_name) == 0) { par_idx = i; break; }
        }
        if (par_idx < 0) {
            fprintf(stderr, "[Rings] body '%s' not found — ring skipped\n", body_name);
            rnode = rnode->next;
            continue;
        }

        Zone zones[MAX_ZONES];
        int  n_zones = 0;
        JsonNode *zones_arr = json_get(rnode, "zones");
        if (zones_arr) {
            JsonNode *zn = zones_arr->first_child;
            while (zn && n_zones < MAX_ZONES) {
                zones[n_zones].r_min    = (float)json_num(json_get(zn, "r_min_km"), 0.0);
                zones[n_zones].r_max    = (float)json_num(json_get(zn, "r_max_km"), 1.0);
                zones[n_zones].density  = (float)json_num(json_get(zn, "density"),  1.0);
                JsonNode *col = json_get(zn, "color");
                zones[n_zones].r = (float)json_num(json_idx(col, 0), 0.7);
                zones[n_zones].g = (float)json_num(json_idx(col, 1), 0.7);
                zones[n_zones].b = (float)json_num(json_idx(col, 2), 0.7);
                n_zones++;
                zn = zn->next;
            }
        }
        if (n_zones == 0) { rnode = rnode->next; continue; }

        ParticleDisc *disc = &s_discs[s_n_discs++];
        disc->parent_idx         = par_idx;
        disc->n_full             = n_full;
        disc->n_lod              = n_lod;
        disc->sprite_r           = sprite_r;
        disc->use_generic_sprite = (strcmp(shader_type, "saturn") != 0);
        disc->base_h_scale       = h_scale;
        disc->ring_r_inner_km    = zones[0].r_min;
        disc->ring_r_outer_km    = zones[0].r_max;
        for (int zi = 1; zi < n_zones; zi++) {
            if (zones[zi].r_min < disc->ring_r_inner_km)
                disc->ring_r_inner_km = zones[zi].r_min;
            if (zones[zi].r_max > disc->ring_r_outer_km)
                disc->ring_r_outer_km = zones[zi].r_max;
        }
        disc->parent_radius_ref_km = (float)(g_bodies[par_idx].radius * 0.001);
        disc_reset_response(disc);

        if (disc->use_generic_sprite) {
            disc->sp_r_inner_km = (float)json_num(json_get(rnode, "sprite_r_inner_km"), 0.0);
            disc->sp_r_outer_km = (float)json_num(json_get(rnode, "sprite_r_outer_km"), 1.0);
            JsonNode *sc = json_get(rnode, "sprite_color");
            disc->sp_color[0]   = (float)json_num(json_idx(sc, 0), 0.7);
            disc->sp_color[1]   = (float)json_num(json_idx(sc, 1), 0.7);
            disc->sp_color[2]   = (float)json_num(json_idx(sc, 2), 0.7);
            disc->sp_alpha_max  = (float)json_num(json_get(rnode, "sprite_alpha_max"), 0.3);
        }

        disc->data_full  = (float*)malloc(n_full * 8 * sizeof(float));
        disc->data_lod   = (float*)malloc(n_lod  * 8 * sizeof(float));
        disc->n_arr_full = (float*)malloc(n_full * sizeof(float));
        disc->n_arr_lod  = (float*)malloc(n_lod  * sizeof(float));
        if (!disc->data_full || !disc->data_lod ||
            !disc->n_arr_full || !disc->n_arr_lod) {
            rnode = rnode->next; continue;
        }

        build_basis(disc, (float)g_bodies[par_idx].obliquity);
        disc_update_mid_motion(disc);

        double gm = G_CONST * g_bodies[par_idx].mass;
        s_seed(seed_full);
        bake_particles(disc->data_full, disc->n_arr_full, n_full,
                       zones, n_zones, gm, e_max, h_scale);

        s_seed(seed_lod);
        bake_particles(disc->data_lod, disc->n_arr_lod, n_lod,
                       zones, n_zones, gm, e_max, h_scale);

        init_disc_gl(disc);
        disc->initialized = 1;

        rnode = rnode->next;
    }

    json_free(root);
}

/*
 * rings_step_system — swept, low-frequency ring hitbox pass for one system.
 *
 * Called once per outer physics step.  The detector sweeps the impactor over
 * the last dt instead of polling every inner RESPA tick, so ring contacts stay
 * responsive without turning the hot integrator loop into a ring broadphase.
 */
void rings_step_system(int root, double dt)
{
    if (root < 0 || root >= g_nbodies || s_n_discs <= 0 || dt <= 0.0) return;
    if (!g_bodies[root].alive) return;

    for (int d = 0; d < s_n_discs; d++) {
        ParticleDisc *disc = &s_discs[d];
        int parent_idx;

        if (!disc->initialized) continue;
        parent_idx = disc->parent_idx;
        if (parent_idx < 0 || parent_idx >= g_nbodies) continue;
        if (!g_bodies[parent_idx].alive) continue;
        if (body_root_star(parent_idx) != root) continue;

        for (int c = 0; c < RING_COLLISION_SEGMENTS * RING_COLLISION_RADIAL_BINS; c++) {
            if (disc->hit_cooldown[c] > 0.0f) {
                disc->hit_cooldown[c] -= (float)dt;
                if (disc->hit_cooldown[c] < 0.0f) disc->hit_cooldown[c] = 0.0f;
            }
        }

        for (int i = 0; i < g_nbodies; i++) {
            if (i == parent_idx) continue;
            if (!body_is_ring_perturber_planet(i)) continue;
            if (body_root_star(i) != root) continue;
            update_disc_swept_contact(disc, i, dt);
        }
    }
}

/*
 * rings_tick — advance mean anomaly for all ring particles: M += n × dt.
 *
 * M is kept in [0, 2π) by subtracting 2π × floor(M / 2π) rather than using
 * fmod, which can be slow for many particles.  The result is passed to the
 * GPU each frame via glBufferSubData in render_disc().
 */
void rings_tick(double dt)
{
    const float TWO_PI = 6.28318530718f;

    for (int d = 0; d < s_n_discs; d++) {
        ParticleDisc *disc = &s_discs[d];
        if (!disc->initialized) continue;
        if (disc->parent_idx < 0 || disc->parent_idx >= g_nbodies) continue;
        if (!g_bodies[disc->parent_idx].alive) continue;
        disc_update_response(disc, dt > 0.0 ? (float)dt : 0.0f);

        for (int i = 0; i < disc->n_full; i++) {
            if (disc->data_full[i*8+1] <= 0.0f) continue;
            float M = disc->data_full[i*8+0]
                    + (float)((double)disc->n_arr_full[i] * dt);
            if (M >= TWO_PI) M -= TWO_PI * (float)(int)(M / TWO_PI);
            disc->data_full[i*8+0] = M;
        }
        for (int i = 0; i < disc->n_lod; i++) {
            if (disc->data_lod[i*8+1] <= 0.0f) continue;
            float M = disc->data_lod[i*8+0]
                    + (float)((double)disc->n_arr_lod[i] * dt);
            if (M >= TWO_PI) M -= TWO_PI * (float)(int)(M / TWO_PI);
            disc->data_lod[i*8+0] = M;
        }
    }
}

/* Draw all initialized, alive ring discs at their current LOD. */
void rings_render(const float vp[16])
{
    for (int d = 0; d < s_n_discs; d++) {
        if (s_discs[d].initialized &&
            s_discs[d].parent_idx >= 0 &&
            s_discs[d].parent_idx < g_nbodies &&
            g_bodies[s_discs[d].parent_idx].alive)
            render_disc(&s_discs[d], vp);
    }
}

/*
 * rings_on_body_absorbed — handle ring ownership transfer after a collision.
 *
 * Two cases:
 *   - Impactor had a ring (disc->parent_idx == impactor_idx):
 *       If the target already has its own ring, just disable this one.
 *       Otherwise, retune it to orbit the target using the target's mass and obliquity.
 *
 *   - Target had a ring (disc->parent_idx == target_idx):
 *       Retune it to match the target's updated mass/obliquity after absorption.
 *
 * Stars and already-ringed targets don't inherit a second ring because overlapping
 * disc geometry would not look correct.
 */
/*
 * rings_on_collision — strong one-shot ring response at body impact start.
 */
void rings_on_collision(int target_idx, int impactor_idx, double rel_speed,
                        const double dir[3], const double rel_vel[3])
{
    double inv_dir[3];
    double inv_vel[3];

    if (!dir) return;
    inv_dir[0] = -dir[0];
    inv_dir[1] = -dir[1];
    inv_dir[2] = -dir[2];
    if (rel_vel) {
        inv_vel[0] = -rel_vel[0];
        inv_vel[1] = -rel_vel[1];
        inv_vel[2] = -rel_vel[2];
    } else {
        inv_vel[0] = inv_vel[1] = inv_vel[2] = 0.0;
    }

    for (int d = 0; d < s_n_discs; d++) {
        ParticleDisc *disc = &s_discs[d];
        if (!disc->initialized) continue;
        if (disc->parent_idx == target_idx && body_is_ring_perturber_planet(impactor_idx))
            damage_disc(disc, target_idx, rel_speed, dir, rel_vel);
        else if (disc->parent_idx == impactor_idx && body_is_ring_perturber_planet(target_idx))
            damage_disc(disc, impactor_idx, rel_speed, inv_dir,
                        rel_vel ? inv_vel : NULL);
    }
}

void rings_on_body_absorbed(int target_idx, int impactor_idx)
{
    for (int d = 0; d < s_n_discs; d++) {
        ParticleDisc *disc = &s_discs[d];
        if (!disc->initialized) continue;

        if (disc->parent_idx == impactor_idx) {
            int target_has_ring = 0;
            float old_parent_radius_km = disc->parent_radius_ref_km;
            for (int k = 0; k < s_n_discs; k++) {
                if (k == d || !s_discs[k].initialized) continue;
                if (s_discs[k].parent_idx == target_idx) {
                    target_has_ring = 1;
                    break;
                }
            }
            if (target_has_ring || target_idx < 0 || target_idx >= g_nbodies ||
                !g_bodies[target_idx].alive || g_bodies[target_idx].is_star) {
                disc->initialized = 0;
            } else {
                retune_disc_parent(disc, target_idx);
                disc_clear_hit_cooldown(disc);
                disc_drive_transfer_response(disc,
                                             old_parent_radius_km,
                                             (float)(g_bodies[target_idx].radius * 0.001),
                                             0.30f);
            }
        } else if (disc->parent_idx == target_idx) {
            float old_parent_radius_km = disc->parent_radius_ref_km;
            /* Target's own ring: retune for updated mass after absorption */
            retune_disc_parent(disc, target_idx);
            disc_clear_hit_cooldown(disc);
            disc_drive_transfer_response(disc,
                                         old_parent_radius_km,
                                         (float)(g_bodies[target_idx].radius * 0.001),
                                         0.18f);
        }
    }
}

/* Free all CPU-side particle data and GPU resources. */
void rings_shutdown(void)
{
    for (int d = 0; d < s_n_discs; d++) {
        ParticleDisc *disc = &s_discs[d];
        free(disc->data_full);   free(disc->data_lod);
        free(disc->n_arr_full);  free(disc->n_arr_lod);
        if (disc->vbo_full)      glDeleteBuffers(1,      &disc->vbo_full);
        if (disc->vao_full)      glDeleteVertexArrays(1, &disc->vao_full);
        if (disc->vbo_lod)       glDeleteBuffers(1,      &disc->vbo_lod);
        if (disc->vao_lod)       glDeleteVertexArrays(1, &disc->vao_lod);
        if (disc->shader)        glDeleteProgram(disc->shader);
        if (disc->sprite_vbo)    glDeleteBuffers(1,      &disc->sprite_vbo);
        if (disc->sprite_vao)    glDeleteVertexArrays(1, &disc->sprite_vao);
        if (disc->sprite_shader) glDeleteProgram(disc->sprite_shader);
    }
    free(s_discs);
    s_discs   = NULL;
    s_n_discs = 0;
}
