/*
 * ui_theme.c - shared UI theme helpers
 */
#include "ui_theme.h"

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
