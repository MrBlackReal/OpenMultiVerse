/*
 * presets.c — the registered multiverse.
 *
 * To add a universe: drop a JSON file in assets/ (or assets/universes/) with an
 * optional "laws" block and append an entry here.
 */
#include "presets.h"
#include <string.h>

static const UniversePreset s_presets[] = {
    { "Solar System (Newtonian)", "assets/universe.json",
      "Our real Solar System under standard inverse-square gravity." },
    { "Known Universe (everything)", "assets/universes/known_universe.json",
      "The full catalog: the Solar System plus ~9,700 real stars and ~6,300 "
      "exoplanets at their true light-year positions (~16,000 bodies), running "
      "in real time. Rebuild a smaller slice with "
      "tools/build_known_universe.py --max-systems N." },
    { "Strong Gravity", "assets/universes/strong_gravity.json",
      "G is ~4x stronger — orbits are much faster and tighter." },
    { "Inverse-Cube Forces", "assets/universes/inverse_cube.json",
      "Gravity falls off as 1/r^3: rosettes, plunges, and escapes." },
    { "Expanding Cosmos", "assets/universes/expanding.json",
      "A dark-energy-like outward push slowly unbinds wide orbits." },
    { "Relativistic Precession", "assets/universes/relativistic.json",
      "Post-Newtonian precession exaggerated until rosettes are visible." },
    { "TRAPPIST-1 (real)", "assets/universes/real_trappist1.json",
      "The real 7-planet TRAPPIST-1 system, from NASA Exoplanet Archive data." },
    { "Stellar Neighborhood (real)", "assets/universes/real_neighborhood.json",
      "Real nearby exoplanet systems at their true sky positions and distances." },
    { "Solar System (Horizons)", "assets/universes/real_solar_horizons.json",
      "Planets, Halley & Eros rebuilt from JPL Horizons state vectors." },
    { "Real Stars (Gaia)", "assets/universes/real_stars.json",
      "Nearby stars positioned by Gaia parallax, drifting by proper motion." },
};

int preset_count(void)
{
    return (int)(sizeof(s_presets) / sizeof(s_presets[0]));
}

const UniversePreset *preset_at(int i)
{
    if (i < 0 || i >= preset_count()) return 0;
    return &s_presets[i];
}

int preset_index_of_path(const char *path)
{
    if (path) {
        for (int i = 0; i < preset_count(); i++)
            if (strcmp(s_presets[i].path, path) == 0) return i;
    }
    return 0;
}
