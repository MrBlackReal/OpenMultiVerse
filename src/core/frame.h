/*
 * frame.h — the floating origin.
 *
 * Positions are doubles, and a double carries ~16 significant digits: stored
 * from the Sun, a body at the galactic centre is resolved to 33 km, at M87 to
 * 11 Earth radii. The render pass already draws camera-relative, but it
 * subtracts two numbers that were rounded before it got them, so it cannot
 * recover what storage lost.
 *
 * So storage is camera-centred. Everything that is SIMULATED -- g_bodies,
 * the camera, trails, collision and physics caches -- lives in a LOCAL frame
 * whose origin sits near the camera. When the camera drifts FRAME_REBASE_AU
 * from the origin, frame_rebase() shifts every local-frame position by one
 * exact vector and moves the origin: the world moves around the player, in
 * steps, so everything near the camera keeps sub-millimetre precision
 * anywhere in the universe.
 *
 * Everything anchored to the SUN stays in the Sun frame: star catalogs, the
 * field store, galaxies, nebulae, the dust cube, the survey, the procedural
 * lattices. Those consumers take frame_cam_sun() instead of g_cam.pos, and
 * data crossing between the frames converts with frame_sun_to_local() /
 * frame_local_to_sun().
 *
 * The origin itself is a plain double in AU (Sun frame). Far from home it is
 * coarse, but that only offsets Sun-frame data by the same amount that data is
 * already quantised to; local positions relative to each other are exact.
 *
 * Main thread only: rebase between frames, never inside physics.
 */
#pragma once

/* Sun-frame position of the local frame's origin, AU. 0 until the first
 * rebase, so a session that never leaves home is exactly as before. */
extern double g_frame_origin_au[3];

/* The camera in the Sun frame, AU. */
void frame_cam_sun(double out_au[3]);

/* Sun frame <-> local frame, metres. */
void frame_sun_to_local_m(const double sun_m[3], double out_local_m[3]);
void frame_local_to_sun_m(const double local_m[3], double out_sun_m[3]);

/* A subsystem that stores local-frame positions across frames registers one
 * of these: it must subtract d_m (metres) from every such position. Bodies
 * and the camera are shifted by frame_rebase() itself. */
typedef void (*FrameShiftFn)(const double d_m[3]);
void frame_on_rebase(FrameShiftFn fn);

/* Called with the same d_m BEFORE anything moves, while positions still hold
 * their full precision (freeze.c snapshots systems a jump leaves behind). */
void frame_on_pre_rebase(FrameShiftFn fn);

/* Once per frame, before physics and rendering: rebase if the camera has
 * drifted FRAME_REBASE_AU from the origin. Returns 1 if it rebased. */
int  frame_rebase_if_needed(void);

/* Shift the origin by delta_au (Sun frame, AU): every local-frame position
 * moves by -delta_au, the camera included. */
void frame_rebase(const double delta_au[3]);

/* Put the camera at a Sun-frame position (navigation, benchmark, film): moves
 * the origin there and the camera to the local origin. */
void frame_place_camera_sun(const double sun_au[3]);

/* Round-trip precision test (--selftest-frame). Returns 1 on pass. */
int  frame_selftest(void);
