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
void rings_step_system(int root, double dt);
void rings_tick(double dt);         /* advance mean anomalies — call each physics sub-step */
void rings_render(const float vp[16]);
void rings_on_collision(int target_idx, int impactor_idx, double rel_speed,
                        const double dir[3], const double rel_vel[3]);
void rings_on_body_absorbed(int target_idx, int impactor_idx);
void rings_shutdown(void);
