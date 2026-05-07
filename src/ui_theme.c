/*
 * ui_theme.c — shared UI theme helpers
 *
 * Font resolution:
 *   ui_theme_open_font() walks a priority list of system font paths and opens
 *   the first one that exists. The list covers:
 *     - Windows: Segoe UI (preferred — matches system UI), Arial (fallback)
 *     - Linux: DejaVu Sans at two common installation paths
 *   The first successful TTF_OpenFont() call wins; subsequent paths are skipped.
 *   If no font is found, NULL is returned and the caller logs a warning.
 *
 *   Adding a new platform is as simple as appending its font path before NULL.
 *   The list must remain NULL-terminated.
 */
#include "ui_theme.h"

/* Font search order: Windows first (Segoe preferred), then Linux fallbacks. */
static const char *s_ui_font_paths[] = {
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    NULL
};

TTF_Font *ui_theme_open_font(int size)
{
    for (int i = 0; s_ui_font_paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(s_ui_font_paths[i], size);
        if (f) return f;
    }
    return NULL;
}
