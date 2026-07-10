/*
 * catalogtool — offline converter from real astronomical catalogs to universe
 * JSON. Shares the conversion core with the simulator (src/catalog.c).
 *
 * Build:  make catalogtool
 * Usage:  ./catalogtool <exoplanets|horizons|gaia|blackholes> <in.csv> <out.json> [max]
 *
 *   exoplanets  NASA Exoplanet Archive "Planetary Systems" CSV
 *   horizons    JPL Horizons heliocentric state-vector CSV
 *   gaia        Gaia / Hipparcos nearby-star CSV
 *   blackholes  curated real black-hole CSV (assets/catalogs/black_holes.csv)
 *   max         optional cap on systems (exoplanets) / stars (gaia) / holes
 */
#include "catalog.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <exoplanets|horizons|gaia|blackholes> <in.csv> <out.json> [max]\n",
            argv[0]);
        return 2;
    }
    int type = catalog_type_from_name(argv[1]);
    if (type < 0) {
        fprintf(stderr, "catalogtool: unknown catalog type '%s'\n", argv[1]);
        return 2;
    }
    int max = (argc >= 5) ? atoi(argv[4]) : 0;
    int n = catalog_convert((CatalogType)type, argv[2], argv[3], max);
    return (n >= 0) ? 0 : 1;
}
