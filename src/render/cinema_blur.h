/*
 * cinema_blur.h — choose between full and camera-only motion blur
 * (CINEMATIC.md §4.1, §15).
 *
 * Full motion blur advances the simulation in one slice per accumulation
 * sample, so moving bodies smear; that re-runs the integrator N times a frame
 * and dominates render cost at high sample counts. The camera's own blur needs
 * none of it: each sample re-poses the camera on the shutter regardless. So per
 * frame this asks whether any object's motion — relative to what the camera
 * rides — would move more than half a pixel while the shutter is open. If not,
 * slicing the sim changes nothing visible and the frame is rendered with
 * camera-only blur, advancing the sim once. Automatic: no setting.
 */
#pragma once

/* sim_open_s: simulation seconds the open shutter spans this frame.
 * Returns 1 if the sim must be sliced across the shutter. */
int  cinema_blur_objects_needed(double sim_open_s);

/* Film-out summary: how many frames needed object blur. */
void cinema_blur_report(void);
