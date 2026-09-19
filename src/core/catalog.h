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
#include <stdint.h>

typedef enum {
    CATALOG_EXOPLANETS = 0,  /* NASA Exoplanet Archive "Planetary Systems" CSV */
    CATALOG_HORIZONS,        /* JPL Horizons heliocentric state-vector CSV     */
    CATALOG_GAIA,            /* Gaia / Hipparcos nearby-star CSV -> JSON        */
    CATALOG_GAIA_BIN,        /* Gaia CSV -> compact binary star catalog (.bin)  */
    CATALOG_BLACK_HOLES      /* curated real black-hole CSV -> JSON             */
} CatalogType;

/*
 * Compact binary star catalog (the "StarBin" format).
 *
 * A bulk star field (hundreds of thousands of Gaia points) is far too large to
 * ship as universe JSON (~400 bytes/star of text).  This is a fixed-record
 * binary alternative that catalogtool writes and universe.c streams straight
 * into g_bodies[] at load — stars are now cheap (no trail, no GL buffer).  A
 * preset opts in via an optional top-level "star_catalog" path in its JSON.
 *
 * On-disk layout: one StarBinHeader, then `count` StarBinRecord.  Little-endian
 * (the build target is x86); a cross-endian reader is out of scope and the magic
 * check rejects a mismatched file.  `record_size` is validated against
 * sizeof(StarBinRecord) on read so any ABI drift fails loudly.
 */
#define STARBIN_MAGIC   0x4F4D5653u   /* 'OMVS' */
#define STARBIN_VERSION 1u

typedef struct {
    uint32_t magic;        /* STARBIN_MAGIC                                    */
    uint32_t version;      /* STARBIN_VERSION                                  */
    uint32_t count;        /* number of StarBinRecord that follow             */
    uint32_t record_size;  /* = sizeof(StarBinRecord); reader validates       */
} StarBinHeader;

typedef struct {
    uint64_t source_id;    /* catalogue id -> body name (decimal string)      */
    float    pos_ly[3];    /* position, light-years, ecliptic->GL frame       */
    float    vel_kms[3];   /* bulk velocity, km/s, GL frame                    */
    float    mass_kg;      /* stellar mass, kg                                 */
    float    radius_km;    /* stellar radius, km                              */
    uint8_t  color[3];     /* display RGB, 0..255                             */
    uint8_t  _pad;         /* reserved; keeps the record 8-byte aligned       */
} StarBinRecord;

/* Map "exoplanets" | "horizons" | "gaia" | "gaia-bin" | "blackholes" to a
 * CatalogType, or -1. */
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
