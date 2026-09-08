/*
 * ui_theme.c — shared UI theme helpers
 *
 * Font resolution:
 *   Every piece of on-screen text (HUD, labels, build UI, pause menu, loading
 *   overlay) is drawn from a TTF_Font opened here, so a failed lookup makes the
 *   whole UI render blank — no text, no error the user can see. The search is
 *   therefore deliberately broad, in decreasing order of specificity:
 *
 *     1. $VERSE_FONT            — explicit override, wins over everything.
 *     2. assets/fonts/ui.ttf    — font shipped alongside the executable.
 *     3. A list of known-good system font paths (Windows, macOS, and the usual
 *        Linux distro layouts — Debian/Ubuntu, Arch, Fedora).
 *     4. A scan of the system font directories, picking the most UI-suitable
 *        face found. This is the catch-all for distributions that ship neither
 *        DejaVu nor Liberation at a path anyone has hard-coded.
 *
 *   The resolved path is cached: the callers open the same face at four or five
 *   sizes and the directory scan should run at most once per process.
 */
#include "ui_theme.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Known-good font paths, tried in order. */
static const char *s_ui_font_paths[] = {
    /* Windows */
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    /* macOS */
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial.ttf",
    /* Linux — Debian/Ubuntu layout */
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    /* Linux — Arch / Fedora / openSUSE layout */
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/gnu-free/FreeSans.ttf",
    "/usr/share/fonts/cantarell/Cantarell-Regular.otf",
    "/usr/share/fonts/TTF/Hack-Regular.ttf",
    NULL
};

/* Roots for the fallback scan (step 4). */
static const char *s_font_dirs[] = {
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "/Library/Fonts",
    "/System/Library/Fonts",
    "C:/Windows/Fonts",
    NULL
};

/* Faces we never want for UI text: they either lack the Latin coverage we use
 * or read badly at 12-20 px. */
static const char *s_scan_reject[] = {
    "emoji", "symbol", "icon", "awesome", "dingbat", "webding", "wingding",
    "italic", "oblique", "bold", "black", "thin", "light", "condensed",
    "cjk", "kana", "hangul", "nanum", "arab", "hebr", "thai", "deva",
    "music", "math", NULL
};

/* Preferred families, most preferred last (score = index + 1). */
static const char *s_scan_prefer[] = {
    "sans", "freesans", "cantarell", "roboto", "opensans", "ubuntu",
    "notosans", "arial", "segoeui", "liberationsans", "dejavusans", NULL
};

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

/* Lowercase, punctuation-stripped copy of a filename, for the substring tests
 * below ("DejaVuSans-Bold.ttf" -> "dejavusansbold.ttf"). */
static void normalize_name(const char *name, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '-' || c == '_' || c == ' ') continue;
        out[j++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[j] = '\0';
}

/* Rank a font file for UI use. 0 = unusable, higher = better. */
static int score_font_name(const char *filename)
{
    char norm[256];
    normalize_name(filename, norm, sizeof norm);

    const char *dot = strrchr(norm, '.');
    if (!dot) return 0;
    if (strcmp(dot, ".ttf") && strcmp(dot, ".otf") && strcmp(dot, ".ttc"))
        return 0;

    for (int i = 0; s_scan_reject[i]; i++)
        if (strstr(norm, s_scan_reject[i])) return 0;

    int score = 1;   /* any regular Latin face beats no text at all */
    for (int i = 0; s_scan_prefer[i]; i++)
        if (strstr(norm, s_scan_prefer[i])) score = i + 2;
    if (strstr(norm, "regular")) score += 1;
    return score;
}

/* Recursive best-match scan. `depth` bounds the walk so a pathological font
 * tree (or a symlink loop) cannot stall startup. */
static void scan_dir(const char *dir, int depth, char *best, size_t best_cap,
                     int *best_score, size_t *best_len)
{
    if (depth <= 0) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;   /* skip ., .. and dotdirs */

        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir, ent->d_name) >= (int)sizeof path)
            continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(path, depth - 1, best, best_cap, best_score, best_len);
            continue;
        }

        int score = score_font_name(ent->d_name);
        if (score <= 0) continue;

        /* Tie-break on the shorter filename: a family match is a prefix test,
         * so "NotoSans-Regular" and "NotoSansOldPermic-Regular" score alike and
         * the plain family is the one we actually want. Length is also a stable
         * ordering, so the pick does not depend on readdir() order. */
        size_t len = strlen(ent->d_name);
        if (score > *best_score || (score == *best_score && len < *best_len)) {
            *best_score = score;
            *best_len   = len;
            snprintf(best, best_cap, "%s", path);
        }
    }
    closedir(d);
}

/* Resolve the UI font path once; returns NULL if nothing usable was found. */
static const char *resolve_font_path(void)
{
    static char s_path[1024];
    static int  s_state = 0;    /* 0 = unresolved, 1 = found, -1 = none */

    if (s_state) return s_state > 0 ? s_path : NULL;
    s_state = -1;

    const char *env = getenv("VERSE_FONT");
    if (env && *env && path_exists(env)) {
        snprintf(s_path, sizeof s_path, "%s", env);
        s_state = 1;
    }

    if (s_state < 0 && path_exists("assets/fonts/ui.ttf")) {
        snprintf(s_path, sizeof s_path, "%s", "assets/fonts/ui.ttf");
        s_state = 1;
    }

    for (int i = 0; s_state < 0 && s_ui_font_paths[i]; i++) {
        if (!path_exists(s_ui_font_paths[i])) continue;
        snprintf(s_path, sizeof s_path, "%s", s_ui_font_paths[i]);
        s_state = 1;
    }

    if (s_state < 0) {
        int    best_score = 0;
        size_t best_len   = 0;
        char   best[1024];
        best[0] = '\0';
        for (int i = 0; s_font_dirs[i]; i++)
            scan_dir(s_font_dirs[i], 4, best, sizeof best, &best_score, &best_len);
        if (best[0]) {
            snprintf(s_path, sizeof s_path, "%s", best);
            s_state = 1;
        }
    }

    if (s_state > 0)
        fprintf(stdout, "[UI] font: %s\n", s_path);
    else
        fprintf(stderr, "[UI] no usable font found — on-screen text will be "
                        "blank. Set VERSE_FONT=/path/to/font.ttf or drop one "
                        "at assets/fonts/ui.ttf\n");
    return s_state > 0 ? s_path : NULL;
}

TTF_Font *ui_theme_open_font(int size)
{
    const char *path = resolve_font_path();
    if (path) {
        TTF_Font *f = TTF_OpenFont(path, size);
        if (f) return f;
        /* The path resolved but this size failed (a broken or unsupported
         * face): fall through to the plain list rather than losing all text. */
        fprintf(stderr, "[UI] TTF_OpenFont(%s, %d): %s\n", path, size, TTF_GetError());
    }
    for (int i = 0; s_ui_font_paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(s_ui_font_paths[i], size);
        if (f) return f;
    }
    return NULL;
}
