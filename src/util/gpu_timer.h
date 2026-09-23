/*
 * gpu_timer.h — GPU-side pass timing with GL timestamp queries.
 *
 * The CPU profiler (profiler.h) times how long the CPU spends *issuing* each
 * render pass, which says little about what the GPU spends *executing* it: a
 * pass can submit in 0.1 ms and keep the GPU busy for 10, or the reverse.
 *
 * Enabled by --profile-gpu (which implies --profile). Marks are sequential,
 * like render.c's CPU zones: gpu_timer_mark("X") ends whatever zone was open
 * and starts "X". Each zone gets its own begin and end timestamp with a
 * glFinish() between zones, so a zone's commands reach the GPU as one batch
 * bracketed by its own stamps and the gap is that pass's execution time alone.
 *
 * Without the finish, a stamp is recorded when the GPU reaches it, and GPU
 * idle time -- waiting while the CPU issues -- lands in whichever zone happens
 * to contain a driver flush; measured that way the galaxy pass read 28 ms and
 * Spheres nothing. The price of the finish is that CPU and GPU no longer
 * overlap, so frame rate under --profile-gpu is not the real frame rate; the
 * CPU zone timers stay honest because each mark finishes before the next CPU
 * zone starts its clock.
 */
#pragma once

/* On or off (off by default: no queries, no finishes). */
void gpu_timer_set_enabled(int on);
int  gpu_timer_enabled(void);

/* Open a frame's GPU timeline. Call once per frame, before any GL work. */
void gpu_timer_frame_begin(void);

/* End the open zone and start `name` (a string literal: stored by pointer).
 * Call before starting the matching CPU zone's clock. */
void gpu_timer_mark(const char *name);

/* Close the frame, after the last GL command before swap. */
void gpu_timer_frame_end(void);

/* Readback for display. gpu_timer_sort() orders zones heaviest first; `index`
 * is a position in that order. Times are per frame, in ms. */
int  gpu_timer_count(void);
void gpu_timer_sort(void);
int  gpu_timer_get(int index, const char **name, double *avg_ms, double *max_ms);
void gpu_timer_frame_stats(double *avg_ms, double *max_ms);

void gpu_timer_dump(void);       /* stdout report, like profiler_dump_stdout */
void gpu_timer_shutdown(void);
