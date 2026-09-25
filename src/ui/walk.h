/*
 * walk.h — walking on planets, and the ground limit for the free camera.
 *
 * G steps out onto the ground under the camera of a world with mesh terrain
 * (terrain.h): the camera becomes a walker's eyes, riding the body's spin and
 * orbit, with the local horizon as its look frame. WASD walk, Shift runs, E
 * jumps; the fall is the body's own surface gravity, so a jump on the Moon
 * carries six times as high as on Earth. G again returns to free flight.
 *
 * Free flight cannot pass below the surface either: walk_limit_camera keeps
 * the camera an eye height above the ground as drawn.
 */
#pragma once

typedef struct {
    int forward, back, left, right, jump, run;
} WalkInput;

int  walk_active(void);

/* Toggle walking. Starting needs a world with terrain below the camera; if
 * there is none yet (tiles still streaming in) the request is retried for a
 * few seconds before giving up. */
void walk_toggle(void);

/* Once per frame, after the simulation has moved the bodies and before the
 * view is built: walk, or keep the free camera above the ground. */
void walk_update(float dt, const WalkInput *in);

/* Leave walking immediately (a fly-to, orbit camera or cinematic shot takes
 * the camera). */
void walk_exit(void);
