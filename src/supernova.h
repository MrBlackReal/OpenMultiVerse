/*
 * supernova.h - star-star collision aftermath and rendering data.
 */
#pragma once
#include "common.h"

#define SUPERNOVA_MAX_EVENTS 8

typedef struct {
    float pos[3];              /* camera-relative AU */
    float flash_radius;        /* AU */
    float core_radius;         /* AU */
    float cloud_radius;        /* AU */
    float cloud_inner_radius;  /* AU */
    float flash_intensity;     /* 0..1 */
    float core_intensity;      /* 0..1 */
    float cloud_intensity;     /* 0..1 */
    float hot_shell_intensity; /* 0..1 */
    float color[3];            /* remnant / hot gas tint */
    float seed;
    float time_days;
} SupernovaRenderEvent;

void supernova_reset(void);
void supernova_step(double dt);
int supernova_try_trigger(int star_a, int star_b, double rel_speed,
                          double hit_t, double frame_dt);
int supernova_render_events(SupernovaRenderEvent *out, int max_events,
                            const double cam_pos[3]);
