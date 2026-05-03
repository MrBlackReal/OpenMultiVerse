/*
 * common.h — shared constants, macros, and includes for all modules
 */
#pragma once

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* GLEW must come before any other GL/GLU headers */
#include <GL/glew.h>
#include <GL/glu.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ window */
#define DEFAULT_WIN_W  1280
#define DEFAULT_WIN_H   720
extern int g_win_w;
extern int g_win_h;
#define WIN_W  g_win_w
#define WIN_H  g_win_h
#define FOV    60.0f

/* ------------------------------------------------------------------ math */
#define PI  3.14159265358979323846

/* ------------------------------------------------------------------ physics */
#define AU         1.496e11          /* m per AU                         */
#define DAY        86400.0           /* s per day                        */
#define G_CONST    6.674e-11         /* m^3 kg^-1 s^-2                   */
#define GM_SUN     2.9591220828559093e-4  /* AU^3/day^2                 */
#define SOFTENING  1e5               /* collision softening radius (m)   */
#define GRAV_EPSILON 1e-14           /* m/s² — skip pair if acceleration below this */

/* ------------------------------------------------------------------ sim */
#define MAX_BODIES         128
#define TRAIL_LEN          16384   /* trail circular buffer size (per body, all bodies) */
#define TRAIL_MASK         (TRAIL_LEN - 1)  /* for fast power-of-2 modulo */
#define NUM_STARS          4000

/* Trail sampling: fixed global spatial resolution. */
#define TRAIL_MIN_SEGMENT_LEN       2.0e4        /* m */
#define TRAIL_MAX_SEGMENT_LEN       4.5e8        /* m */
#define TRAIL_BASE_SEGMENT_LEN      1.0e8        /* m */
#define TRAIL_SATELLITE_SEGMENT_LEN  2.0e6       /* m */
#define TRAIL_CLOSE_APPROACH_FACTOR  0.35        /* denser sampling only near collisions */
#define TRAIL_TARGET_WORLD_LEN     (128.0 * AU)  /* fixed retained trail length */
#define TRAIL_SATELLITE_WORLD_LEN   (1.0 * AU)
#define TRAIL_CURVE_ERROR_RATIO     0.22         /* allowed chord error vs segment length */
#define TRAIL_CURVE_MIN_ERROR       5.0e3        /* m */
#define TRAIL_CURVE_MAX_ERROR       2.0e7        /* m */
#define TRAIL_CURVE_MAX_DEPTH       6

/* 1 AU → 1.0 GL unit */
#define RS  (1.0 / AU)

#define LY  9.461e15   /* meters per light-year */

/* ------------------------------------------------------------------ system LOD (AU)
 * Distances at which rendering elements fade when flying away from the system. */
#define SYS_TRAIL_FADE_START  240.0f   /* trails at full opacity below this */
#define SYS_TRAIL_FADE_END   1200.0f   /* trails fully invisible above this */
#define SYS_DOT_FADE_START    240.0f   /* non-star dots begin fading        */
#define SYS_DOT_FADE_END     1200.0f   /* non-star dots fully gone          */
