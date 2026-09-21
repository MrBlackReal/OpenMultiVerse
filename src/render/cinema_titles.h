/*
 * cinema_titles.h — --cinematic-info presentation overlay (CINEMATIC.md §11.1).
 *
 * Lower-thirds that fade in when the camera arrives somewhere new (name, kind
 * and distance, simulation date, a scale bar at the subject's depth) and a
 * camera-speed readout in fractions of c. Drawn once per OUTPUT frame, after
 * the accumulation resolve and before capture, so text is never smeared
 * through the motion-blur samples. Off by default: film-out frames are clean.
 */
#pragma once

void cinema_titles_set_enabled(int on);
int  cinema_titles_enabled(void);

/* Advance by one output frame of dt seconds (the fixed film dt when filming)
 * and draw into the current framebuffer. */
void cinema_titles_frame(double dt);
