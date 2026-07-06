/*
 * universe.h — data-driven universe loader
 *
 * Reads assets/universe.json and populates g_bodies[] / g_nbodies.
 * Call universe_load() from main.c instead of solar_system_init().
 */
#pragma once

#include "body.h"

typedef struct {
    const char *name;
    double mass;              /* kg */
    double radius;            /* m */
    double pos[3];            /* m */
    double vel[3];            /* m/s */
    float  col[3];
    int    is_star;
    int    parent;
    double obliquity;         /* degrees */
    double rotation_rate;     /* rad/s */
    float  atm_color[3];
    float  atm_intensity;
    float  atm_scale;
} BodyCreateSpec;

/*
 * universe_load — parse the given JSON file and populate the g_bodies
 * array with all bodies in the first stellar system found.
 *
 * On any error (file not found, parse failure, etc.) this function
 * prints an error to stderr and calls exit(1).
 */
void universe_load(const char *path);

/*
 * universe_validate — cheap pre-flight check that `path` can be loaded: the
 * file exists, parses as JSON, and contains a "bodies" array. Returns 0 if
 * loadable, -1 otherwise (no global state is touched). Callers use this before
 * tearing down the live world so a bad user-supplied path is a no-op instead
 * of aborting the process via universe_load()'s exit(1).
 */
int universe_validate(const char *path);

/*
 * universe_save — write the current live universe (g_laws + every alive body's
 * absolute state) to `path` as a "snapshot" universe JSON. The result reloads
 * exactly as saved (no warm-up, no re-derivation from Keplerian elements).
 * Returns 0 on success, -1 on failure (diagnostic printed to stderr).
 */
int universe_save(const char *path);

/* Set to 1 by universe_load() when it loaded a snapshot file, else 0. Snapshots
 * already hold settled state, so main.c skips the warm-up pre-simulation. */
extern int g_universe_is_snapshot;

/* Contiguous index range of "field stars" — bulk stars appended from a compact
 * binary star catalog (see load_star_catalog / the "star_catalog" preset field).
 * [g_field_star_begin, g_field_star_end) is that range; begin == end means the
 * universe has no field catalog (every preset without one). Field stars are
 * frozen scenery rendered by a static GPU path; only the few currently near the
 * camera are promoted back into the per-frame dynamic path. Build-mode additions
 * append past g_field_star_end and are therefore never mistaken for field stars. */
extern int g_field_star_begin;
extern int g_field_star_end;

/* Bumped by universe_load() on every (re)load so cached, universe-scoped GPU
 * resources (e.g. the static field-star VBO in render.c) know to rebuild. */
extern unsigned g_universe_generation;

/* Add a fully specified runtime body. Returns the new body index, or -1. */
int universe_add_body(const BodyCreateSpec *spec);

/* Number of currently alive bodies, excluding absorbed/reused slots. */
int universe_live_body_count(void);

/* Re-derive every black hole's radius from its mass (laws_schwarzschild_radius).
 * Call after live edits to g_laws (G) so horizons track the new physics. */
void universe_refresh_bh_radii(void);

/* True while another runtime body can be placed without exceeding MAX_BODIES. */
int universe_can_add_body(void);

/* Reassign planets/dwarf bodies to the nearest star after sandbox edits. */
void universe_rebind_to_nearest_stars(void);

/* universe_shutdown — free all body trail buffers and the g_bodies array. */
void universe_shutdown(void);
