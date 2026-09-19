/*
 * supernova.h - star-star collision aftermath and rendering data.
 *
 * A supernova event is triggered by the collision system when two root stars
 * overlap. The simulation then:
 *
 *   1. Spawns a remnant star at the swept collision point.
 *   2. Keeps a transient event record for flash/core/cloud rendering.
 *   3. Applies one-shot flash destruction and delayed shock interaction to
 *      surviving root-orbiting bodies.
 *   4. Exposes camera-relative render data to render.c each frame.
 *
 * The visual blast stays anchored at its birth position in world space even
 * after the remnant itself continues drifting through the simulation.
 */
#pragma once
#include "common.h"

#define SUPERNOVA_MAX_EVENTS 8
typedef struct {
    float pos[3];              /* camera-relative event center in AU */
    float flash_radius;        /* early white-hot blast radius in AU */
    float core_radius;         /* bright inner fireball radius in AU */
    float cloud_radius;        /* outer ejecta cloud radius in AU */
    float cloud_inner_radius;  /* excavated cavity radius inside cloud */
    float flash_intensity;     /* 0..1 fullscreen/near-core flash weight */
    float core_intensity;      /* 0..1 hot core contribution */
    float cloud_intensity;     /* 0..1 outer volumetric cloud contribution */
    float hot_shell_intensity; /* 0..1 rim / shock-shell accent */
    float color[3];            /* remnant / ejecta tint */
    float seed;                /* stable per-event shader variation seed */
    float time_days;           /* event age in simulation days */
} SupernovaRenderEvent;

void supernova_reset(void);
void supernova_step(double dt);
int supernova_try_trigger(int star_a, int star_b, double rel_speed,
                          double hit_t, double frame_dt);

/* Detonate a single star in place (end-of-life core collapse, or the gentle
 * planetary-nebula puff of a low-mass star). Retires the star and spawns the
 * appropriate remnant body — black hole, neutron star, or white dwarf — by
 * progenitor mass, scaling the blast so a low-mass death reads as a soft
 * nebula. Returns the new remnant body index (>0) on success, 0 on failure.
 * Used by the stellar lifecycle (lifecycle.c). */
int supernova_detonate(int star_idx);
int supernova_render_events(SupernovaRenderEvent *out, int max_events,
                            const double cam_pos[3]);
