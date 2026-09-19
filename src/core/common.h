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

#include "settings.h"                /* global app tunables → g_settings */

/* ------------------------------------------------------------------ window */
#define DEFAULT_WIN_W  1280
#define DEFAULT_WIN_H   720
extern int g_win_w;
extern int g_win_h;
extern int g_hud_hidden;   /* 1 = suppress the 2D HUD overlay + body labels */
#define WIN_W  g_win_w
#define WIN_H  g_win_h
/*
 * FOV and the other former compile-time tunables below are now live globals
 * in g_settings (see settings.h). The original macro names are kept as
 * aliases so existing call sites read the current value unchanged — the same
 * pattern as G_CONST / SOFTENING.
 */
#define FOV    (g_settings.fov)

/* ------------------------------------------------------------------ math */
#define PI  3.14159265358979323846

/* ------------------------------------------------------------------ physics */
#include "laws.h"                    /* per-universe constants → g_laws  */
#define AU         1.496e11          /* m per AU                         */
#define DAY        86400.0           /* s per day                        */
#define GM_SUN     2.9591220828559093e-4  /* AU^3/day^2                 */
#define GRAV_EPSILON 1e-14           /* m/s² — skip pair if acceleration below this */
/*
 * G_CONST / SOFTENING are now per-universe state held in g_laws (see laws.h).
 * The original compile-time names are kept as aliases so existing call sites
 * read the live value; new code may use g_laws.G / g_laws.softening directly.
 */
#define G_CONST    (g_laws.G)
#define SOFTENING  (g_laws.softening)

/* ------------------------------------------------------------------ sim */
/* MAX_BODIES / TRAIL_LEN are structural (fixed-size stack tables, power-of-2
 * ring buffer) — they must stay compile-time. Everything tunable below is an
 * alias into g_settings. */
#define MAX_BODIES         128
#define TRAIL_LEN          16384   /* trail circular buffer size (per body, all bodies) */
#define TRAIL_MASK         (TRAIL_LEN - 1)  /* for fast power-of-2 modulo */
#define NUM_STARS          (g_settings.num_stars)  /* procedural fallback skybox count */

/* Trail sampling geometry (tunable; see settings.h). */
#define TRAIL_MIN_SEGMENT_LEN       (g_settings.trail_min_segment_len)
#define TRAIL_MAX_SEGMENT_LEN       (g_settings.trail_max_segment_len)
#define TRAIL_BASE_SEGMENT_LEN      (g_settings.trail_base_segment_len)
#define TRAIL_SATELLITE_SEGMENT_LEN (g_settings.trail_satellite_segment_len)
#define TRAIL_CLOSE_APPROACH_FACTOR (g_settings.trail_close_approach_factor)
#define TRAIL_TARGET_WORLD_LEN      (g_settings.trail_target_world_len)
#define TRAIL_SATELLITE_WORLD_LEN   (g_settings.trail_satellite_world_len)
#define TRAIL_CURVE_ERROR_RATIO     (g_settings.trail_curve_error_ratio)
#define TRAIL_CURVE_MIN_ERROR       (g_settings.trail_curve_min_error)
#define TRAIL_CURVE_MAX_ERROR       (g_settings.trail_curve_max_error)
#define TRAIL_CURVE_MAX_DEPTH       6           /* recursion-depth cap (structural) */

/* 1 AU → 1.0 GL unit */
#define RS  (1.0 / AU)

#define LY  9.461e15   /* meters per light-year */

/* ------------------------------------------------------------------ render depth
 * RENDER_DEPTH_FAR is the single source of truth for the logarithmic depth range:
 * both the CPU perspective far plane (main.c) and every depth-writing shader's
 * `gl_FragDepth = log2(eye+1)/log2(FAR+1)` normalisation (injected as DEPTH_FAR by
 * gl_shader_load) use it. Log depth keeps near-field precision even with a huge far
 * plane, so this can span planet → interstellar scale in one continuous transform.
 * Units: GL units (= AU, since RS = 1/AU). ~1.6e5 ly. Distant catalog stars can sit
 * light-years out (millions of AU) and still be inside this range.
 * NOTE: shader-side floats can't represent 1e10 exactly, but the log() of it is fine;
 * keep it a round power-friendly magnitude. */
#define RENDER_DEPTH_FAR  1.0e10f

/* ------------------------------------------------------------------ system LOD (AU)
 * Distances at which rendering elements fade when flying away from the system. */
#define SYS_TRAIL_FADE_START  (g_settings.sys_trail_fade_start)
#define SYS_TRAIL_FADE_END    (g_settings.sys_trail_fade_end)
#define SYS_DOT_FADE_START    (g_settings.sys_dot_fade_start)
#define SYS_DOT_FADE_END      (g_settings.sys_dot_fade_end)
