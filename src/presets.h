/*
 * presets.h — curated list of selectable universes ("the multiverse").
 *
 * Each preset is just a display name plus the JSON file that defines its bodies
 * and its "laws" block.  The universe picker menu enumerates these; main.c loads
 * the chosen path through switch_universe().
 */
#pragma once

typedef struct {
    const char *name;   /* display name shown in the picker        */
    const char *path;   /* universe JSON file to load              */
    const char *blurb;  /* one-line description of its physics      */
} UniversePreset;

/* Number of registered presets. */
int preset_count(void);

/* Preset at index i (0..preset_count()-1), or NULL if out of range. */
const UniversePreset *preset_at(int i);

/* Index of the preset whose path matches `path`, or 0 (default) if none. */
int preset_index_of_path(const char *path);
