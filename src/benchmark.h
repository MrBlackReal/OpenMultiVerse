/*
 * benchmark.h — scripted cinematic flythrough + FPS benchmark.
 *
 * A game-style "benchmark run": the camera is flown along a scripted tour that
 * starts at the Sun, pulls out of the solar system, rises above the galactic
 * disc to reveal the Milky Way, and then flies to a sequence of other galaxies
 * (LMC, Andromeda, Triangulum, …).  Each leg is timed independently; on
 * completion a per-stage and overall FPS report (avg / min / max / 1% low) is
 * printed to stdout.
 *
 * Started only from the CLI: `./verse --preset <file> --benchmark`.  main.c
 * drives it: while benchmark_active(), it calls benchmark_update(dt) each frame
 * (which sets g_cam.pos/yaw/pitch directly, the same way cam_fly does) instead
 * of the free-look camera, and quits when the tour finishes.
 *
 * Owns its own tour + timing state and exposes compact HUD data (module
 * ownership pattern); ui.c reads benchmark_hud() to draw the overlay.
 */
#pragma once

/* Build the tour from the loaded galaxy catalogue and begin.  Call once, after
 * the world (and galaxy.c) is initialised. */
void benchmark_start(void);

/* 1 while the tour is playing OR the end-of-run summary is still on screen. */
int  benchmark_active(void);
/* 1 only while the camera is still flying (false once the summary lingers). */
int  benchmark_running(void);

/* Advance the scripted camera and accumulate this frame's timing.  Call once
 * per frame with the real frame delta (seconds) while benchmark_active(). */
void benchmark_update(float dt_real);

/* HUD feed for ui.c.  Fills `stage` (current leg name), `line` (a live
 * "avg 142 · min 61 fps" readout) and `progress` (0..1 over the whole tour).
 * Returns 1 if the overlay should be drawn. */
int  benchmark_hud(char *stage, int stage_n, char *line, int line_n,
                   float *progress);

/* Run the second (galaxies-OFF) pass, which prices the galaxy render layer by
 * differencing the two. Off by default — it doubles a ~7-minute run, and the
 * per-stage fps numbers come from the first pass either way. Call before
 * benchmark_start(). */
void benchmark_set_ab(int on);

/* Enable per-stage screenshots into `dir` (created by the caller). Marked
 * stages capture one frame mid-hold, during the galaxies-ON pass only — the
 * galaxies-OFF pass would produce misleading images of the same scene. Call
 * before benchmark_start(). */
void benchmark_set_shot_dir(const char *dir);

/* If a screenshot is due this frame, returns its path and clears the request;
 * NULL otherwise. main.c must call this AFTER render_frame()/post_end() and
 * before the buffer swap, since the capture reads GL_BACK. The frames around a
 * capture are excluded from timing — glReadPixels stalls the pipeline and would
 * otherwise show up as a fake 1% low. */
const char *benchmark_take_shot_path(void);

/* Free the tour allocation. */
void benchmark_shutdown(void);
