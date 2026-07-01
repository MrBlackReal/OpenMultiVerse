![image](https://github.com/user-attachments/assets/2878c365-11df-4f4a-af8e-3a141d69ae85)

# OpenMultiVerse - simulate universes with different laws of physics

**OpenMultiVerse** is a fork of [ortanaV2/OpenVerse](https://github.com/ortanaV2/OpenVerse)
that turns the single open-world universe simulator into a **multiverse**: many
universes, each with its own physical laws, plus tools to build them from real
astronomical data.

The original goal stands — simulate as much of the universe as possible (multiple
star systems, planets, moons, asteroid belts, rings) under real N-body gravity.
OpenMultiVerse adds the ability to change the gravity itself.

Not a screensaver. Not a game. A sandbox for curiosity.

### What's new vs. OpenVerse

- **Data-driven physical laws.** Every universe has a `"laws"` block — gravitational
  constant `G`, softening, and time scale are tunable per universe.
- **Custom force laws.** Non-inverse-square gravity via a `force_exp` exponent
  (inverse-cube and friends → precessing, plunging, escaping orbits).
- **Exotic terms.** A cosmological repulsion term (`lambda`, a dark-energy analogue)
  and post-Newtonian perihelion precession (`pn_factor`).
- **Multiverse menu (Dear ImGui).** Press **U** to pick between universes and drag
  live sliders for the active laws. Built on cimgui (optional `IMGUI=1` build).
- **Real astronomical data import.** Build universes from the NASA Exoplanet
  Archive, JPL Horizons state vectors, or Gaia/Hipparcos star catalogs — both via
  the `catalogtool` CLI and in-app import buttons.
- **Save / load.** Snapshot the live universe (laws + every body's exact state) to
  a file and reload it precisely, from the same menu.

See [Multiverse](#multiverse--different-laws-of-physics) and
[Real astronomical data](#real-astronomical-data) below for details.

---

## Showcase

<table>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/68655bf4-ddfc-48e9-80a6-be727c7c4ed4" alt="Planets"/><br/><sub><b>Discover planets and other celestial bodies</b></sub></td>
    <td><img src="https://github.com/user-attachments/assets/2d2e0cfc-ca05-46a5-9c1c-76d5b31fec02" alt="Solar system"/><br/><sub><b>Simulate entire solar systems</b></sub></td>
  </tr>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/3c1df437-4e81-4a35-b3c6-45ea37121afd" alt="Collision"/><br/><sub><b>Collision of bodies using orbital mechanics</b></sub></td>
    <td><img src="https://github.com/user-attachments/assets/7340ad70-9054-48a4-8943-88ab3a7d8392" alt="Build mode"/><br/><sub><b>Build your own systems using build mode</b></sub></td>
  </tr>
</table>

---

## Installation

> No build tools required — just download and run.

> **Note:** the pre-built downloads below are upstream **OpenVerse** binaries and
> do **not** include the OpenMultiVerse features (custom laws, multiverse menu,
> real-data import, save/load). For those, [build from source](#building-from-source)
> — the multiverse menu needs the `IMGUI=1` build.

### Download Links
[Download latest Windows release](https://github.com/ortanaV2/OpenVerse/releases/download/v1.0.3/verse-windows-x64-v1.0.3.zip)

[Download latest Linux release](https://github.com/ortanaV2/OpenVerse/releases/download/v1.0.3/verse-linux-x64-v1.0.3.tar.gz)

### Alternative Installations
<details>
<summary><strong>Step 1 — Go to the Releases page</strong></summary>

Visit [github.com/ortanaV2/OpenVerse/releases](https://github.com/ortanaV2/OpenVerse/releases)

</details>

<details>
<summary><strong>Step 2 — Select the most recent version</strong></summary>

<img width="1031" height="636" alt="openverse_installation_step_1" src="https://github.com/user-attachments/assets/e5ff55d2-dc9c-4cf6-8804-9fc2ee798d51" />

</details>

<details>
<summary><strong>Step 3 — Download the package for your system</strong></summary>

Click the `.zip` for Windows or the `.tar.gz` for Linux under the **Assets** section.

<img width="1082" height="233" alt="openverse_installation_step_2" src="https://github.com/user-attachments/assets/80cbce65-4cad-4855-9c93-0a7141e701b4" />

</details>

<details>
<summary><strong>Step 4 — Unzip the downloaded archive</strong></summary>

Extract the folder to any location on your machine.

<img width="776" height="709" alt="openverse_installation_step_3" src="https://github.com/user-attachments/assets/c3f7b65d-6cf4-4b4c-85cc-8aa1adbe6d9e" />

</details>

<details>
<summary><strong>Step 5 — Run the executable</strong></summary>

- **Windows:** Double-click `verse.exe`
- **Linux:** Open a terminal in the folder and run `./verse`

<img width="797" height="158" alt="image" src="https://github.com/user-attachments/assets/62e1ebfb-949a-420c-b9e3-0928380c2c59" />

</details>

---

## Controls

| Key / Input | Action |
|---|---|
| Left-click | Enter free-look (captures mouse) |
| Escape | Open system menu / exit build mode / exit inspection mode |
| W / S | Move forward / backward |
| A / D | Strafe left / right |
| Q / E | Move down / up |
| Mouse | Look around |
| Scroll | Adjust camera speed |
| T | Toggle warp mode |
| B | Toggle build mode |
| Tab + Scroll | Cycle build presets (in build mode) |
| I | Toggle inspection mode |
| U | Toggle the multiverse menu (requires the ImGui build — see below) |
| F11 / Alt+Enter | Toggle fullscreen |
| `+` / `-` | Simulation speed up / down |
| Space | Pause / resume |
| R | Reset camera near the Sun |

**Simulation speeds:** `0 → 0.1 → 0.25 → 0.5 → 1 → 2 → 5 → 10 → 30 → 60 → 100 → 365` days/s

---

## Vision

Space is incomprehensibly large. Most simulations either abstract that away or confine you to a single solar system. OpenMultiVerse doesn't.

- **Real scale.** Every distance, mass, and orbital period is physically accurate. The emptiness between planets is real. Flying from Earth to Neptune takes time.
- **Real dynamics.** Bodies move under genuine N-body gravity — no baked animations, no shortcuts. Disrupt the solar system and watch it react.
- **Open world.** Fly anywhere. Approach an asteroid from 10 km. Pull back until the entire solar system fits on screen. Eventually, jump to another star.
- **Sandbox.** Spawn scenarios, collide objects, break things. Curiosity should have no guardrails.

---

## Current State

| Feature | Status |
|---|---|
| N-body gravity — RESPA hierarchical integrator, adaptive per-system timestep | ✓ |
| Full solar system — Sun, 8 planets, dwarf planets, large asteroids, and major moons | ✓ |
| Procedural planet textures, axial tilt & rotation | ✓ |
| Data-driven universe config (JSON) | ✓ |
| Multiple star systems & nearby exoplanet systems | ✓ |
| Ring systems — Saturn (Keplerian particles), Uranus & Neptune rings | ✓ |
| Asteroid belts — Main Belt & Kuiper Belt with gravity-integrated particles | ✓ |
| Planet collision & merge — animation, particle spray, persistent craters, spin transfer | ✓ |
| Build mode — spawn and place bodies at runtime | ✓ |
| Inspection mode — highlight bodies and orbit selected targets | ✓ |
| Catalog-backed skybox — Yale Bright Star Catalog J2000 stars | ✓ |
| Background soundtrack | ✓ |
| Supernovae | ✓ |
| Data-driven physical laws — per-universe G, softening, time scale | ✓ |
| Custom force laws — non-inverse-square gravity (configurable exponent) | ✓ |
| Exotic terms — cosmological repulsion (Λ) & post-Newtonian precession | ✓ |
| Multiverse — multiple selectable universes, each with its own laws | ✓ |
| ImGui universe picker + live law sliders (optional `IMGUI=1` build) | ✓ |
| Galaxy-scale rendering — camera-driven active region + far-field points, ~16k bodies in real time | ✓ |
| Real-data presets — full NASA Exoplanet / Gaia catalog as the "Known Universe" | ✓ |
| Stellar lifecycle — main-sequence → giant → white dwarf / neutron star / black hole | ✓ |
| Black holes — accretion disk + shadow + photon ring rendering | ✓ |
| HDR bloom, enhanced atmospheres (scattering, day/night), volumetric nebulae | ✓ |

---

## Multiverse — different laws of physics

Every universe is a JSON file under `assets/` with an optional `"laws"` block.
Omitted fields fall back to Newtonian defaults, so existing files keep working.

```jsonc
"laws": {
  "G": 6.674e-11,    // gravitational constant (m^3 kg^-1 s^-2)
  "softening": 1e5,  // Plummer softening length (m)
  "time_scale": 1.0, // multiplier on simulated time
  "force_exp": 2.0,  // radial falloff exponent (2 = inverse-square, 3 = inverse-cube, ...)
  "lambda": 0.0,     // cosmological term: outward push ∝ distance (dark-energy analogue)
  "pn_factor": 0.0   // post-Newtonian perihelion precession (1 = physical, higher = exaggerated)
}
```

Bundled example universes live in `assets/universes/`: **Strong Gravity**,
**Inverse-Cube Forces**, **Expanding Cosmos**, and **Relativistic Precession**.
Build with `IMGUI=1` (below) and press **U** in-app to pick a universe or drag the
live law sliders and watch the dynamics change.

**Save / load.** The same menu can snapshot the running universe — current laws
plus every body's exact position and velocity — to a JSON file, and load it back
to that precise instant (snapshots skip warm-up so nothing drifts). Handy for
capturing a collision setup or a tweaked law configuration to revisit later.

---

## Real astronomical data

Universes can be built from real catalogs. The converter (`catalogtool`) turns a
catalog into a universe JSON the simulator loads like any other; the same code
also powers the in-app **Import real astronomical data** buttons in the `U` menu
(ImGui build), which import a catalog and load it on the spot.

```bash
make catalogtool
./catalogtool exoplanets assets/catalogs/exoplanets_sample.csv assets/universes/my_systems.json
./catalogtool horizons   assets/catalogs/horizons_sample.csv   assets/universes/my_solar.json
./catalogtool gaia       assets/catalogs/gaia_sample.csv        assets/universes/my_stars.json [max]
```

| Source | What it reads | Where to get it |
|---|---|---|
| **NASA Exoplanet Archive** | `hostname`, `pl_name`, `sy_dist`, `ra`, `dec`, `st_mass/rad/teff`, `pl_orbsmax`/`pl_orbper`, `pl_orbeccen`, `pl_orbincl`, `pl_bmasse`, `pl_rade` → one star + Keplerian planets per host | [Planetary Systems CSV](https://exoplanetarchive.ipac.caltech.edu/) |
| **JPL Horizons** | heliocentric **Ecliptic of J2000.0** state vectors (`x/y/z_km`, `vx/vy/vz_kms`) → converted to orbital elements | [ssd.jpl.nasa.gov/horizons](https://ssd.jpl.nasa.gov/horizons/) (VECTORS, km & km/s) |
| **Gaia / Hipparcos** | `ra`, `dec`, `parallax` (mas), `pmra`, `pmdec`, `radial_velocity`, `teff` → positioned, drifting stars | [Gaia Archive](https://gea.esac.esa.int/archive/) |

Small real samples live in `assets/catalogs/`, and the bundled presets
**TRAPPIST-1 (real)**, **Stellar Neighborhood (real)**, **Solar System
(Horizons)**, and **Real Stars (Gaia)** are generated from them. The
**Known Universe** preset merges the Solar System with the full NASA Exoplanet +
Gaia catalogs into a single ~16,000-body universe — generate or resize it with
`python3 tools/build_known_universe.py --max-systems N` (`N=0` = everything). See
[ARCHITECTURE.md](ARCHITECTURE.md) §8.1 for how the renderer keeps that many
bodies real-time.

---

## Building from Source

*For contributors and developers. End users should use the [pre-built releases](#installation) above.*

**Windows (MSYS2 / MinGW-w64)**
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make \
          mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf \
          mingw-w64-x86_64-SDL2_mixer mingw-w64-x86_64-glew
mingw32-make
./verse.exe
```

**Linux**
```bash
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev libglew-dev
make
./verse
```
(On Arch/CachyOS: `sudo pacman -S sdl2 sdl2_ttf sdl2_mixer glew`.)

**Optional — the ImGui multiverse menu**

The universe picker and live law sliders are built on [cimgui](https://github.com/cimgui/cimgui)
(a C binding for Dear ImGui) and are compiled only on request:

```bash
git submodule update --init --recursive   # fetch extern/cimgui + Dear ImGui
make IMGUI=1                               # links libstdc++; needs g++
./verse                                    # press U for the multiverse menu
```

Without `IMGUI=1` the menu code compiles to inert stubs and the simulator builds
exactly as before (no C++ toolchain or cimgui required). Universes can still be
selected by editing the path the app loads.

For a full technical reference of the codebase, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Contributing

OpenMultiVerse is open source and early in development. The physics engine, rendering pipeline, and coordinate system are all designed to scale beyond a single solar system — and, increasingly, beyond a single set of physical laws. If you want to help push toward a truly open multiverse, contributions are welcome.

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on reporting bugs, requesting features, and submitting pull requests.

---

## License

This project is licensed under the [MIT License](LICENSE).
