/*
 * rings.h — Keplerian ring particle system (Saturn, Uranus, Neptune)
 *
 * rings.c owns all particle buffers, LOD state, collision response, and GPU
 * resources.  Callers only initialize, step, notify collision events, render,
 * and shut the system down.
 */
#pragma once

/* rings_init — parse ring configs from the given universe.json path. */
void rings_init(const char *path);

/* rings_query — ring geometry for a body (for ring-shadow shading).
 * Returns 1 and fills the outputs if body_idx owns a live ring: radii in AU,
 * pole = ring-plane normal in the GL frame.  Returns 0 otherwise. */
int  rings_query(int body_idx, float *inner_au, float *outer_au, float pole[3]);
void rings_step_system(int root, double dt);
void rings_tick(double dt);         /* advance mean anomalies — call each physics sub-step */
void rings_render(const float vp_camrel[16]);
void rings_on_collision(int target_idx, int impactor_idx, double rel_speed,
                        const double dir[3], const double rel_vel[3]);
void rings_on_body_absorbed(int target_idx, int impactor_idx);
void rings_shutdown(void);
