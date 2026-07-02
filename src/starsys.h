/*
 * starsys.h — procedural star → real system promotion (roadmap §0.1, final
 * scale-continuity step).
 *
 * The galaxy_stars pass draws procedural point stars wherever the galaxy
 * density model puts them. This module makes the closest of those points
 * *real*: when the camera comes within ~1 ly of a procedural star, the same
 * hash pipeline is re-evaluated on the CPU (it must match
 * galaxy_stars.vert exactly — same cells, candidates, hashes, density
 * accept), and the star is promoted to a live body with a deterministic
 * planetary system (universe_add_body). Fly away (> ~2.5 ly, hysteresis)
 * and it demotes back to a point; return later and the identical system
 * regenerates from the same lattice-cell seed.
 *
 * Promoted stars are handed to the shader as suppressed cells so the point
 * sprite disappears while the real body exists. Because promotion only
 * happens where the skybox crossfade is fully done (> ~4.7 kly from origin),
 * it can never collide with the real catalogue around Sol.
 *
 * MAIN-THREAD ONLY (mutates g_bodies via universe_add_body).
 */
#pragma once

/* Forget all promotions (universe reload wiped the bodies). */
void starsys_reset(void);

/* Per-frame: demote out-of-range systems, promote newly-near procedural
 * stars. cam_pos_au = camera in AU (g_cam.pos), time_s = the same clock
 * render.c hands the galaxy shaders (drives the arm-shear term). */
void starsys_tick(const double cam_pos_au[3], float time_s);

/* Suppressed candidates of galaxy `gal` for the finest star cascade:
 * fills out[i][4] = {cellx, celly, cellz, sub}. Returns the count (<= max).
 * Read by galaxy.c each frame to hide promoted stars' point sprites. */
int starsys_suppressed(int gal, int out[][4], int max);
