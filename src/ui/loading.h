/*
 * loading.h — full-screen loading / universe-switch overlay
 *
 * A self-contained 2D overlay drawn directly to the default framebuffer while
 * the world is (re)built. Because world construction is single-threaded and
 * blocking, the loader itself drives presentation: long phases call
 * loading_tick() periodically to keep the bar animating, and the overlay
 * presents its own frames (V-Sync is forced off for the duration so ticking
 * inside tight loops costs almost nothing).
 *
 * Determinate vs indeterminate:
 *   - loading_progress(frac) — bar eases toward a known 0..1 target.
 *   - loading_indeterminate() — bar shows a sweeping highlight for work whose
 *     duration is unknown.
 *
 * Typical use:
 *   loading_begin();
 *   loading_status("Parsing universe data");  loading_indeterminate();  loading_tick();
 *   ... // long work calls loading_status/progress/tick as it goes
 *   loading_end();   // fades the overlay out and restores V-Sync
 */
#pragma once
#include "common.h"

/* One-time init. Call after the GL context and TTF are ready (after ui_init). */
void loading_init(SDL_Window *win);
void loading_shutdown(void);

/* Reopen the overlay fonts at the current g_settings sizes (used by the
 * settings "Apply" action after the font sizes change). */
void loading_reload_fonts(void);

/* Begin / end a loading session. begin fades in; end fades out and presents
 * the closing frames. Both are no-ops if loading_init() failed. */
void loading_begin(void);
void loading_end(void);

/* Status line shown above the bar (printf-style). */
void loading_status(const char *fmt, ...);

/* Determinate progress in [0,1]; the displayed bar eases toward this value. */
void loading_progress(double frac);

/* Switch the bar to indeterminate (sweeping) mode. */
void loading_indeterminate(void);

/* Advance the animation and present one frame, throttled to ~90 Hz so it is
 * cheap to call from tight loops. Safe to call as often as you like. */
void loading_tick(void);

/* True while a session is active (between begin and end). */
int  loading_active(void);
