#pragma once

/* asteroids_init — parse belt configs from the given universe.json path. */
void asteroids_init(const char *path);
void asteroids_step(double dt);   /* gravity integration — call once per outer step */
void asteroids_render(const float vp_camrel[16]);
/* Max on-screen particle motion (px) over sim_dt, relative to ref_vel (m/s),
 * for particles within cos_lim of fwd. See cinema_blur.c. */
double asteroids_max_screen_px(const double ref_vel[3], double sim_dt,
                               const float fwd[3], double cos_lim, double px_per_rad);
void asteroids_shutdown(void);
