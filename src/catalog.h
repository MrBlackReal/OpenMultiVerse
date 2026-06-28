/*
 * catalog.h — convert real astronomical catalogs into universe JSON.
 *
 * One conversion core, two front-ends:
 *   - tools/catalogtool.c    (offline CLI)
 *   - in-app "Import real data" (runtime: convert to a temp file, then load it)
 *
 * Every importer emits a universe JSON file that the existing loader
 * (universe.c) already understands: a "laws" block (Newtonian defaults) plus a
 * "bodies" array of stars (pos_ly) and planets (keplerian elements). This means
 * real-data universes get the multiverse menu, rings/asteroids passes, and
 * everything else for free — no special-case loader path.
 *
 * The module is deliberately free of SDL/OpenGL so it builds as a standalone
 * command-line tool and can be unit-tested without a graphics context.
 */
#pragma once

typedef enum {
    CATALOG_EXOPLANETS = 0,  /* NASA Exoplanet Archive "Planetary Systems" CSV */
    CATALOG_HORIZONS,        /* JPL Horizons heliocentric state-vector CSV     */
    CATALOG_GAIA             /* Gaia / Hipparcos nearby-star CSV               */
} CatalogType;

/* Map "exoplanets" | "horizons" | "gaia" to a CatalogType, or -1 if unknown. */
int catalog_type_from_name(const char *name);

/*
 * catalog_convert — read `in_path`, write a universe JSON to `out_path`.
 *
 *   max_items : cap on systems (exoplanets) or stars (gaia); <= 0 means no cap.
 *
 * Returns the number of bodies written (>= 0) on success, or -1 on failure
 * (a diagnostic is printed to stderr).
 */
int catalog_convert(CatalogType type, const char *in_path,
                    const char *out_path, int max_items);
