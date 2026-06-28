/*
 * menu.h — Dear ImGui (cimgui) universe picker + live-laws overlay.
 *
 * This is an optional feature compiled only when the build defines USE_IMGUI
 * (i.e. `make IMGUI=1`, which also pulls in the extern/cimgui submodule).  When
 * USE_IMGUI is not defined every function below is a no-op stub, so main.c can
 * call them unconditionally and the default build needs no C++ / cimgui.
 */
#pragma once
#include "common.h"

/* Create the ImGui context and SDL2/OpenGL3 backends. Safe to call once after
 * the GL context exists. No-op stub when USE_IMGUI is undefined. */
void menu_init(SDL_Window *win, SDL_GLContext gl);

/* Destroy backends + context. */
void menu_shutdown(void);

/* Feed one SDL event to ImGui. Returns 1 if ImGui wants to consume it (so the
 * caller should not treat it as game input), 0 otherwise. */
int  menu_process_event(const SDL_Event *e);

/* Show/hide the picker window, and query visibility. */
void menu_set_visible(int visible);
int  menu_visible(void);
void menu_toggle(void);

/*
 * menu_render — run one ImGui frame and draw the overlay.
 *
 * Must be called once per rendered frame (it owns ImGui's NewFrame/Render),
 * after the world is drawn and before SDL_GL_SwapWindow.
 *
 *   current_preset : index of the currently loaded preset (for highlighting).
 *   laws_changed   : set to 1 if a law slider was moved this frame.
 *   out_load_path  : set to a universe JSON path to load directly (e.g. a
 *                    just-imported real-data catalog), or left untouched.
 *
 * Returns the preset index the user clicked to switch to, or -1 if none.
 */
int  menu_render(int current_preset, int *laws_changed, const char **out_load_path);
