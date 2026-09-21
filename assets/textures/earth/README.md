# Earth imagery

Real satellite imagery mapped onto the body named "Earth" by
`src/render/earth_tex.c` and `assets/shaders/phong.frag`. Both images are
NASA products in the public domain (NASA imagery is not copyrighted; credit
requested).

| File | Source | Size |
|---|---|---|
| `earth_day_5400.jpg` | NASA Visible Earth, *Blue Marble: Next Generation*, July 2004, true colour without topographic shading — `world.200407.3x5400x2700.jpg` from <https://visibleearth.nasa.gov/images/74092> | 5400×2700 |
| `earth_night_3600.jpg` | NASA Earth Observatory, *Black Marble 2016* (0.1°), city lights — `BlackMarble_2016_01deg.jpg` from <https://earthobservatory.nasa.gov/features/NightLights> | 3600×1800 |

Credit: NASA Earth Observatory / NASA Goddard Space Flight Center (Reto Stöckli,
Blue Marble NG); NASA Earth Observatory (Joshua Stevens, Miguel Román, Black
Marble).

Both are standard equirectangular maps: north up, 180° W at the left edge.

## Higher (or lower) resolution

Point `earth_day_texture` / `earth_night_texture` in `settings.json` at any
equirectangular JPG or PNG, for example NASA's 21600×10800 Blue Marble or the
13500×6750 Black Marble. `earth_texture_max_px` (also the "Earth texture" menu
control) caps the loaded width to save GPU memory; `0` loads full resolution.
A missing or unreadable file falls back to the procedural Earth.

The night map is not lights-only: it has a faint blue moonlit base. The shader
keeps only the warm excess above it, so oceans and unlit land stay dark.
