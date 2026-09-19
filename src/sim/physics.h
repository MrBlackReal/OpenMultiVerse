/*
 * physics.h — N-body gravitational physics (RESPA hierarchical integrator)
 *
 * Force decomposition:
 *   Slow forces — primary-body interactions (planets among themselves, plus
 *                 tidal perturbations from non-parent primaries on satellites).
 *                 Updated once per outer step (~1 day).
 *   Fast forces — dominant parent→satellite force.
 *                 Updated every inner step (0.02 days) for orbital accuracy.
 *
 * Usage (each frame):
 *   for each outer step:
 *       physics_respa_begin(dt_outer);
 *       for each inner step:
 *           physics_respa_inner(dt_inner);
 *       physics_respa_end(dt_outer);
 *       trails_tick(dt_outer);       // arc-length-driven trail sampling
 */
#pragma once
#include "common.h"

extern double g_sim_time;
extern double g_sim_speed;
extern int    g_paused;

/* RESPA split-step integrator — call begin/inner.../end per outer step */
void physics_respa_begin(double dt_outer);  /* slow half-kick + prime fast acc  */
void physics_respa_inner(double dt_inner);  /* fast KDK + drift all bodies      */
void physics_respa_end  (double dt_outer);  /* slow half-kick + rotation + time */
void physics_respa_begin_system(int root, double dt_outer);
/* Like physics_respa_begin_system, but skips recomputing the slow force when
 * recompute_slow==0 (acc[] must already be valid at the current positions). */
void physics_respa_begin_system_ex(int root, double dt_outer, int recompute_slow);
void physics_respa_inner_system(int root, double dt_inner);
void physics_respa_end_system(int root, double dt_outer);

void physics_refresh_timestep_model(void);
/* Flag the timestep model stale (call after any body add/remove). */
void physics_mark_timestep_dirty(void);
/* Per-frame: rebuild only if dirty or the safety throttle elapsed. */
void physics_refresh_timestep_model_if_needed(double real_dt);
/* Throttled per-body timestep refresh for the active systems only (slot
 * indices as returned by the main loop's active scan). Frozen systems never
 * integrate, so only active orbits can drift from the stored model. */
void physics_refresh_active_timesteps(const int *slots, int n, double real_dt);

/* Recommended maximum outer-step for slow-force stability. */
double physics_outer_dt_limit(void);
double physics_inner_dt_limit(void);
int    physics_system_count(void);
int    physics_system_root(int idx);

/* Fill `out` with up to `max` body indices from systems within `radius_m` of
 * `cam_m` (metres), nearest systems first; returns the count written.  The
 * camera-following active set used by labels.c and render.c's near-field query.
 * Backed by the movement-gated near-system cache, so calling it every frame is
 * cheap even with a galaxy-scale system count. */
int    physics_active_bodies(const double cam_m[3], double radius_m,
                             int *out, int max);

/* (Re)build the near-system candidate cache to cover `radius_m` around `cam_m`.
 * Optional to call directly — physics_active_bodies/_systems call it — but a
 * once-per-frame call at the widest radius any consumer needs guarantees a
 * single scan per frame. The O(system_count) scan runs only on camera movement
 * past a threshold or a body-set change. */
void   physics_update_active_cache(const double cam_m[3], double radius_m);

/* Return (via *out, internal storage) the SLOT indices of systems within
 * `radius_m` of `cam_m`, nearest first; returns the count. Cache-backed
 * replacement for the per-frame O(system_count) active-system scan in main.c. */
int    physics_active_systems(const double cam_m[3], double radius_m,
                              const int **out);
double physics_system_outer_dt_limit(int idx);
double physics_system_inner_dt_limit(int idx);
void   physics_advance_time(double dt);

/* Safety net: retire any body whose position/velocity has become non-finite
 * (NaN/inf). A single such body corrupts the camera-relative render math and
 * freezes the view ("whole screen flicker"), so we remove it instead. Returns
 * the number of bodies retired this call. */
int    physics_sanitize_state(void);
int    physics_sanitize_system(int root);   /* one system's members only */

/* Legacy single-step KDK (kept for reference / one-off use) */
void physics_step(double dt);

/* Trail helpers */
void trails_begin_frame_snapshot(void);
void trails_begin_frame_snapshot_system(int root);   /* one system's members only */
void trails_tick(double dt);
void trails_tick_system(int root, double dt);
void trails_cut_body_at_time(int body_idx, double hit_dt, double frame_dt,
                             const double cut_pos[3]);
