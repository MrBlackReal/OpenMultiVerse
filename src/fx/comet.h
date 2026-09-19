/*
 * comet.h — comet coma + ion/dust tail rendering (roadmap §2.3)
 *
 * comet.c owns the GL resources and draws every alive `is_comet` body's
 * coma and two tails as camera-facing additive ribbons.  Sublimation
 * activity is physical: it ramps with the RadianceField's incident flux at
 * the nucleus, so a comet grows its tails approaching perihelion and goes
 * quiet in the outer system — no authored keyframes.
 */
#pragma once

void comet_init(void);

/* Invalidate the cached comet index list. Call after a universe (re)load —
 * the loader is the only code that ever sets is_comet, so the cache stays
 * valid across every runtime event except a full reload (slot reuse only
 * *removes* comets, which the per-entry alive/is_comet check handles). */
void comet_notify_bodies_changed(void);

void comet_render(const float vp_camrel[16],
                  const float cam_right[3], const float cam_up[3],
                  const float cam_fwd[3], const double cam_pos[3],
                  float time);
void comet_shutdown(void);
