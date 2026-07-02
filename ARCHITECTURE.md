# OpenMultiVerse Architecture

This document is the contributor-facing architecture reference for OpenMultiVerse
(a fork of [OpenVerse](https://github.com/ortanaV2/OpenVerse)).
It reflects the current C99/OpenGL codebase and is meant to help humans and
LLM assistants understand where each subsystem lives, what owns what state, and
which invariants matter when changing the simulator.

The fork adds, on top of the base simulator: **per-universe physical laws** (a
selectable "multiverse"), **real-astronomical-data import**, **galaxy-scale
rendering** (camera-driven active region + far-field points, ~16k bodies in real
time), a **stellar lifecycle** system, and visual systems (HDR bloom, volumetric
nebulae, black holes). Forward-looking plans and in-progress work live in `docs/`
(`UNIFIED_ROADMAP_REFINED.md` — the single unified roadmap; it absorbed the former
`SCALING_HANDOFF.md` and `VISUALS_ROADMAP.md`).

---

## Table of Contents

1. [Overview](#1-overview)
2. [Repository Layout](#2-repository-layout)
3. [Build and Runtime Dependencies](#3-build-and-runtime-dependencies)
4. [Runtime Flow](#4-runtime-flow)
5. [Core State and Units](#5-core-state-and-units)
6. [Module Reference](#6-module-reference)
7. [Universe Data Format](#7-universe-data-format)
8. [Physics Architecture](#8-physics-architecture)
9. [Collision and Supernova Flow](#9-collision-and-supernova-flow)
10. [Rendering Pipeline](#10-rendering-pipeline)
11. [Shader Reference](#11-shader-reference)
12. [File Dependency Map](#12-file-dependency-map)
13. [Where to Make Changes](#13-where-to-make-changes)

---

## 1. Overview

OpenMultiVerse is a real-time, data-driven universe simulator written in C99.
It combines a hierarchical N-body gravity simulation with an OpenGL 3.3 Core
renderer that can show bodies from planet-surface scale to interstellar scale,
under physical laws that are configurable per universe.

Primary technologies:

| Area | Technology |
|---|---|
| Language | C99 |
| Window/input/audio bootstrap | SDL2 |
| Text rendering | SDL2_ttf |
| Music playback | SDL2_mixer |
| Rendering | OpenGL 3.3 Core, GLSL 330, GLEW |
| Physics | 2R-RESPA split integrator with per-star-system timestep limits |
| Physical laws | Per-universe `g_laws` (G, softening, force exponent, Λ, PN, gravity isolation, timestep model) |
| App settings | Global, cross-universe `g_settings` (FOV, starfield, warm-up/active radii, fades, controls, overlay, trails); persisted to `settings.json` |
| Optional menu | Dear ImGui via the `extern/cimgui` submodule (`make IMGUI=1`); inert stubs otherwise |
| Data | `assets/universe.json` + `assets/universes/*.json` presets; real catalogs via `catalog.c` |
| Parallelism | OpenMP for warmup across independent star systems |

Design goals:

- Keep simulation state in SI units for physical correctness.
- Convert to AU/camera-relative coordinates only at render boundaries.
- Keep body indices stable across collisions and runtime additions.
- Prefer data-driven universe content over hard-coded solar-system data.
- Let visual systems query physics/collision state instead of owning it.
- Keep physical constants in `g_laws`, not compile-time `#define`s, so each
  universe can run different rules.
- Stay real-time at galaxy scale: only the camera's neighbourhood is fully
  simulated; everything else is a cheap far-field point.

---

## 2. Repository Layout

```text
OpenMultiVerse/
├── Makefile
├── package_linux.sh
├── package_windows.sh
├── README.md
├── ARCHITECTURE.md
├── CONTRIBUTING.md
├── docs/                   In-progress + planned work (UNIFIED_ROADMAP_REFINED.md)
├── tools/                  catalogtool.c (offline) + build_known_universe.py
├── extern/cimgui/          Dear ImGui C binding submodule (only used by IMGUI=1)
├── assets/
│   ├── universe.json
│   ├── universes/          Selectable presets (registered in src/presets.c)
│   ├── catalogs/           Real catalog CSVs (full ones gitignored)
│   ├── bright_star_catalog.csv
│   ├── soundtrack.ogg
│   ├── window_icon.bmp
│   └── shaders/
│       ├── phong.vert / phong.frag
│       ├── atm.vert / atm.frag
│       ├── color.vert / color.frag
│       ├── solid.vert / solid.frag
│       ├── ring.vert
│       ├── asteroid_particle.vert
│       ├── impact_particle.vert / impact_particle.frag
│       ├── label.vert / label.frag
│       ├── star_glare.vert / star_glare.frag
│       ├── build_line.vert / build_line.frag
│       ├── ui.vert / ui.frag
│       ├── supernova_billboard.vert / supernova_core.frag / supernova_cloud.frag
│       ├── bh.vert / bh.frag                    (black hole: disk + shadow + photon ring)
│       ├── nebula.vert / nebula.frag            (volumetric raymarched nebulae)
│       ├── star_dot.vert                        (per-point sized star dots)
│       └── post_quad.vert + bloom_bright/blur/composite.frag   (HDR bloom)
└── src/
    ├── main.c              App init, event loop, per-frame scheduling
    ├── common.h            Shared includes, constants, global window size
    ├── math3d.h            Header-only vector/matrix helpers
    ├── gl_utils.c/.h       Shader and buffer helper functions
    ├── json.c/.h           Minimal JSON parser with comments/trailing commas
    ├── laws.c/.h           Per-universe physical laws (g_laws) + pair-force factor
    ├── presets.c/.h        Registry of selectable universe JSON files
    ├── catalog.c/.h        Real-catalog → universe JSON conversion (no SDL/GL)
    ├── body.c/.h           Body array, Keplerian conversion, body helpers
    ├── universe.c/.h       JSON loader (incl. "laws" block), runtime body alloc/reuse
    ├── physics.c/.h        RESPA gravity and trail sampling engine
    ├── lifecycle.c/.h      Stellar evolution state machine (phases + death events)
    ├── collision.c/.h      Collision detection, merges, scars, debris
    ├── supernova.c/.h      Star-star collision aftermath + lifecycle detonation
    ├── camera.c/.h         Global free-look camera state
    ├── render.c/.h         Main scene compositor
    ├── post.c/.h           Offscreen HDR target + bloom post-processing
    ├── nebula.c/.h         World-space volumetric nebulae
    ├── trails.c/.h         GL upload/draw layer for trail buffers
    ├── labels.c/.h         SDL_ttf label textures and overlap avoidance
    ├── starfield.c/.h      Catalog-backed skybox stars
    ├── rings.c/.h          Keplerian ring particle systems
    ├── asteroids.c/.h      Gravity-integrated asteroid belt particles
    ├── build.c/.h          Runtime sandbox body placement
    ├── inspect.c/.h        Inspection and orbit-camera mode
    ├── menu.c/.h           Dear ImGui multiverse menu (USE_IMGUI; stubs otherwise)
    ├── ui.c/.h             HUD and pause menu overlay
    ├── ui_theme.c/.h       Shared font lookup and UI accent constants
    └── audio.c/.h          SDL_mixer soundtrack wrapper
```

---

## 3. Build and Runtime Dependencies

The `Makefile` builds every `src/*.c` file into one executable named `verse`
or `verse.exe`.

Compiler flags:

```text
-Wall -Wextra -O2 -std=c99 -I$(SRCDIR) -fopenmp -MMD -MP
```

`-MMD -MP` emit a `.d` file per object listing its included headers, so editing a
header forces every dependent `.c` to recompile (a struct-layout change otherwise
silently leaves stale objects — a memory-corruption footgun).

### Build variants

| Command | Result |
|---|---|
| `make` | Default build. `menu.c` compiles to inert stubs; no C++/cimgui needed. |
| `git submodule update --init --recursive` then `make IMGUI=1` | Adds the Dear ImGui multiverse menu (defines `USE_IMGUI`, compiles `extern/cimgui` + Dear ImGui C++ TUs, links `libstdc++`). |
| `make catalogtool` | Standalone offline catalog converter (no SDL/GL); reuses `src/catalog.c`. |

**`make clean` is required when toggling `IMGUI`** — the Makefile tracks file
timestamps, not the flag value, so switching without a clean links stale objects.

The Known-Universe preset is generated/resized by
`python3 tools/build_known_universe.py --max-systems N` (`N=0` = the full
~16k-body catalog).

Linked libraries:

| Platform | Libraries |
|---|---|
| Linux | `sdl2-config --libs`, `SDL2_ttf`, `SDL2_mixer`, `GL`, `GLEW`, `m`, OpenMP |
| macOS | `sdl2-config --libs`, `SDL2_ttf`, `SDL2_mixer`, `GLEW`, OpenGL framework, `m`, OpenMP |
| Windows/MSYS2 | `SDL2`, `SDL2_ttf`, `SDL2_mixer`, `glew32`, `opengl32`, `glu32`, `m`, OpenMP |

The source build dependencies documented in the README are:

- SDL2
- SDL2_ttf
- SDL2_mixer
- GLEW/OpenGL
- OpenMP-capable GCC/MinGW toolchain

Packaging scripts:

- `package_linux.sh` copies the executable as `verse.bin`, bundles shared
  libraries found through `ldd`, creates a launcher wrapper, and emits
  `dist/verse-linux-x64-<version>.tar.gz`.
- `package_windows.sh` runs in MSYS2/MinGW, copies `verse.exe`, bundles DLLs
  found through `ldd`, and emits `dist/verse-windows-x64-<version>.zip`.

---

## 4. Runtime Flow

`main.c` owns the application lifecycle.

Startup sequence:

1. `app_init()`
   - Initializes SDL video/audio.
   - Creates an OpenGL 3.3 Core context.
   - Enables multisampling and vsync.
   - Initializes GLEW.
   - Loads the window icon.
   - Starts audio through `audio_init()`.
2. `collision_reset()` and `supernova_reset()`.
3. `ui_init()`.
4. `init_runtime_world()`
   - `universe_load("assets/universe.json")`
   - `cam_reset()`
   - `starfield_init()`
   - `trails_gl_init()`
   - `render_init()`
   - `rings_init("assets/universe.json")`
   - `asteroids_init("assets/universe.json")`
   - `labels_init()`
   - `build_init()`
   - `inspect_init()`
   - `physics_refresh_timestep_model()`
   - `warmup_universe()`

Main loop:

1. Clamp real frame delta to at most 100 ms.
2. Poll SDL events and route them through `handle_event()`.
3. Move the camera unless the pause menu or inspection orbit is active.
4. If unpaused, integrate physics:
   - Refresh timestep model.
   - Cap per-frame simulation time to `MAX_OUTER_STEPS` per constrained system.
   - Snapshot trails and collision positions.
   - Integrate each star system with RESPA.
   - Refine close-approach systems with collision subdivision.
   - Step rings, trails, collision checks, supernova events, and asteroids.
5. Update inspection orbit camera, if active.
6. Build projection/view matrices.
7. Clear, call `render_frame()`, call `ui_render()`, then swap.

Pause menu actions:

- Continue
- Reset Universe
- Toggle VSync
- Leave

Resetting the universe tears down and rebuilds runtime world resources while
resetting camera, warp, pause, simulation speed, collision, and supernova state.

---

## 5. Core State and Units

### Units

| Quantity | Unit |
|---|---|
| Body position/velocity | metres, metres per second |
| Body mass/radius | kilograms, metres |
| Camera position/speed | AU, AU per real second |
| Render-space body position | AU, camera-relative float |
| Keplerian planet input | AU, degrees, AU^3/day^2 |
| Keplerian moon input | kilometres, degrees, m^3/s^2 |

Important constants in `common.h`:

| Constant | Meaning |
|---|---|
| `AU` | metres per astronomical unit |
| `LY` | metres per light-year |
| `DAY` | seconds per day |
| `G_CONST` | gravitational constant |
| `RS` | `1.0 / AU`, metres to AU render scale |
| `LY` | metres per light-year |
| `MAX_BODIES` | growth seed for heap tables, and the fixed bound for collision/labels (see Galaxy-scale §8.1) |
| `TRAIL_LEN` | power-of-two trail ring-buffer length per body |
| `FOV` | vertical field of view in degrees |

### Body

`Body` in `body.h` is the central simulation entity. It stores:

- Display name, mass, radius, color, star flag.
- `is_black_hole` — flags a star-root body for accretion-disk/shadow rendering
  (excluded from the normal sphere/glare/dot passes).
- Stellar lifecycle fields (stars only, lazily initialised by `lifecycle.c`):
  `star_phase` (`StarPhase` enum) plus `base_*` main-sequence appearance so a
  phase change scales/tints off it and is reversible.
- SI position, velocity, slow acceleration, and fast parent acceleration.
- Stable lifecycle fields: `alive`, `parent`.
- Per-body timestep estimates.
- Rotation, obliquity, atmosphere parameters.
- Trail ring buffer and frame-snapshot fields.

Invariants:

- `g_nbodies` is the high-water slot count, not the number of living bodies.
- Dead/absorbed slots remain addressable and may be reused by
  `universe_add_body()`.
- `g_nbodies` can exceed `MAX_BODIES` (the full catalog is ~16k). Physics and
  render tables grow on the heap; **collision and labels are still bounded by
  `MAX_BODIES`** and currently cover the first 128 body *indices* (see §8.1).
  Do not just bump `MAX_BODIES` — `collision.c` has `[MAX_BODIES][MAX_BODIES]`
  stack arrays.
- Stars normally have `parent == -1`.
- Planets/dwarf planets/asteroids normally parent to a star.
- Moons parent to a non-star body.
- Modules should check `alive` before using a body for simulation or rendering.

### Camera

`Camera g_cam` stores:

- `pos[3]` in AU as double precision.
- `yaw` and `pitch` in degrees.
- `speed` in AU/s.

`main.c` handles movement and mouse look. `camera.c` only resets the state and
computes the forward direction.

### BodyRenderInfo

`BodyRenderInfo` in `labels.h` is computed in `render_frame()` and shared with
labels and inspection:

- camera-relative body center in AU
- visual radius in AU
- camera distance in AU
- whether the body should be label-visible as a small target

---

## 6. Module Reference

### `main.c`

Owns process-level orchestration:

- SDL/OpenGL initialization and shutdown.
- VSync, fullscreen, window resize, pause menu.
- Event routing for free-look, build mode, inspection mode, warp, pause, speed.
- Runtime-world initialization/reset.
- Warmup pre-simulation.
- Per-frame physics scheduling across star systems.
- Matrix construction and final render/UI calls.

Notable controls:

- `B` toggles build mode and exits inspection first.
- `I` toggles inspection mode and exits build mode first.
- `Escape` exits build, exits inspection, or opens/closes the pause menu.
- `Tab + Scroll` cycles build presets.
- Scroll changes camera speed unless build preset cycling or inspection zoom is active.

### `body.c` / `body.h`

Owns the global body array and orbital-mechanics helpers.

Key functions:

| Function | Purpose |
|---|---|
| `keplerian_to_state()` | JPL-style star-orbiting elements to GL-frame SI state |
| `moon_to_state()` | Planetocentric moon elements to parent-relative SI state |
| `nearest_star_idx()` | Living star nearest to the camera |
| `body_root_star()` | Walk parent links to the root star |
| `body_world_to_local_surface_dir()` | Convert impact direction to body-fixed surface coordinates |

Frame convention:

- The loader uses JPL/ecliptic orbital inputs.
- Conversion ends with an ecliptic-to-GL frame swap: GL X = ecliptic X,
  GL Y = ecliptic Z, GL Z = ecliptic Y.

### `universe.c` / `universe.h`

Loads `assets/universe.json` and manages runtime body allocation.

Load passes:

1. Stars
   - Absolute `pos_ly` converted to metres.
   - Optional `velocity_km_s` stored as system bulk velocity.
2. Planets, dwarf planets, asteroids
   - Parent star must already exist.
   - `keplerian` elements produce star-relative state.
3. Moons
   - Parent body must already exist.
   - `moon_keplerian` produces parent-relative state.

Post-processing:

- Each star's velocity is adjusted to remove net internal system momentum.
- Optional bulk velocity is then applied uniformly to every body in that system.

Runtime API:

| Function | Purpose |
|---|---|
| `universe_add_body()` | Add a body at runtime: reuse a dead slot (scanning all slots) or grow the heap array via `ensure_capacity()` |
| `universe_live_body_count()` | Count `alive` bodies |
| `universe_can_add_body()` | Always true — runtime adds grow the heap; there is no `MAX_BODIES` cap on creation |
| `universe_rebind_to_nearest_stars()` | Reassign star-orbiting bodies after sandbox star additions |
| `universe_shutdown()` | Free trail buffers and the body array |

> Runtime body creation (build mode, supernova remnants, collision spawns) grows
> the body array on the heap exactly like the loader, so it works in the full
> ~16k-body universe. `MAX_BODIES` only bounds the fixed collision/labels tables,
> not how many bodies can exist. (Earlier these paths carried a stale 128-body
> cap that silently failed in large universes.)

### `physics.c` / `physics.h`

Owns gravitational integration and trail sampling. The file also implements
`trails_begin_frame_snapshot()`, `trails_tick()`, `trails_tick_system()`, and
`trails_cut_body_at_time()`, while `trails.c` owns the GL drawing resources.

Public simulation state:

- `g_sim_time`
- `g_sim_speed`
- `g_paused`

Integrator API:

| Function | Purpose |
|---|---|
| `physics_refresh_timestep_model()` | Rebuild per-body and per-system timestep data |
| `physics_respa_begin_system()` | Slow half-kick for one root-star system |
| `physics_respa_inner_system()` | Fast KDK substeps for moon-parent forces |
| `physics_respa_end_system()` | Slow half-kick, rotation update |
| `physics_system_count()` | Number of root-star systems |
| `physics_system_root()` | Root body index for a system slot |
| `physics_system_outer_dt_limit()` | Tightest outer timestep for a system |
| `physics_system_inner_dt_limit()` | Tightest inner timestep for a system |
| `physics_advance_time()` | Advance global simulation clock once per frame/warmup |

The non-system variants remain available for reference or one-off use.

### `collision.c` / `collision.h`

Owns solid-body collision detection, merge lifecycle, visual scars, heat glow,
debris particles, and dense trail/cut support around impacts.

Important concepts:

- `collision_snapshot_positions()` captures start-of-step positions and
  velocities for swept checks and trail rollback.
- Primary bodies are checked with adaptive pair intervals.
- Close approaches can request smaller outer steps in `main.c`.
- Merges are time-based events, not instant deletion.
- Collision visuals are returned as `CollisionSpot` arrays for `phong.frag`.
- Absorbed body slots are marked dead but remembered through
  `collision_body_absorbed_by()`.

Public queries used by rendering:

- `collision_spots_for_body()`
- `collision_visual_radius()`
- `collision_body_heat_glow()`
- `collision_body_star_heat()`
- `collision_body_has_active_merge()`
- `collision_particles()`

### `supernova.c` / `supernova.h`

Triggered from the collision system when two living stars collide.

Lifecycle:

1. Reconstruct the swept collision center.
2. Insert a remnant star through `universe_add_body()`.
3. Rebind planets from both source stars to the remnant.
4. Retire source stars and disable their labels/trails.
5. Immediately destroy nearby or over-fluenced bodies.
6. Later, apply one-shot shock impulses to surviving root-orbiting subtrees.
7. Expose render data until the cloud lifetime ends.

The visible blast is anchored at its birth position. The remnant body can drift
away physically.

### `render.c` / `render.h`

The scene compositor. It owns body sphere billboards, body dots, star glare,
supernova shaders, collision particle buffers, build preview overlays, and the
inspection target ring. It delegates specialized systems to their own modules.

`render_init()` loads:

- `phong`
- `atm`
- `color`
- `impact_particle`
- `star_glare`
- `supernova_core`
- `supernova_cloud`
- `build_line`
- `ui` for build distance labels and inspection ring

Precision rule:

- Most geometry is rendered camera-relative with `vp_camrel = proj * view_rot`.
- `g_cam.pos` is subtracted on the CPU in double precision, then cast to float.
- This avoids float cancellation when the camera is several light-years from origin.

### `trails.c` / `trails.h`

OpenGL layer for trail drawing. Physics-side sampling lives in `physics.c`.

Responsibilities:

- Allocate one VAO/VBO per possible body slot.
- Upload camera-relative trail points each frame.
- Draw retained orbital trails.
- Add/reset/remove trail GL resources for runtime body changes.

Trail data is stored in each `Body`:

- circular buffer of AU positions
- per-segment lengths
- retained total length
- frame snapshots for precise collision cuts
- fade/emission state for absorbed bodies

Sampling is spatial, not purely time-based. Constants in `common.h` control
minimum/maximum segment length, satellite trail length, retained world length,
curve subdivision error, and close-approach densification.

### `labels.c` / `labels.h`

SDL_ttf label renderer with texture caching and overlap avoidance.

Responsibilities:

- Open a shared UI font through `ui_theme_open_font()`.
- Pre-render body names to GL textures.
- Add/remove cached labels when bodies are created or absorbed.
- Project labels to screen space and avoid overlaps.
- Render billboard quads with `label.vert/frag`.

The label feed is the camera's active set (see §8.1) **plus a pinned nearest
set**: the nearest `g_settings.label_pin_systems` star systems and
`label_pin_planets` planets are always labelled — pinned systems even outside
the active region, pinned planets past `label_max_dist_au` — so the closest
names never vanish while flying between systems (Menu → Settings → Labels).

### `starfield.c` / `starfield.h`

Catalog-backed skybox starfield.

Flow:

1. Try to load `assets/bright_star_catalog.csv`.
2. Convert RA/Dec to GL direction vectors.
3. Convert B-V/color temperature and magnitude to color/brightness.
4. Sort/pack the stars into a GL point buffer.
5. Fall back to procedural stars if the catalog is unavailable.

Rendered with translation stripped from the view matrix so the field behaves as
a skybox.

### `rings.c` / `rings.h`

Owns planetary ring particle systems for entries in `assets/universe.json`.

Responsibilities:

- Parse the `"rings"` array.
- Generate full and reduced particle sets from radial zones.
- Build ring-plane bases from parent obliquity.
- Advance mean anomalies.
- Apply collision/tidal response state.
- Retune or remove rings when bodies are absorbed.
- Render ring particles with distance-based LOD.

Particle attributes are consumed by `ring.vert`:

```text
M, a, e, omega, h, r, g, b
```

### `asteroids.c` / `asteroids.h`

Owns asteroid belt test particles.

Responsibilities:

- Parse the `"asteroid_belts"` array.
- Generate randomized Keplerian-like particles.
- Integrate particles under gravity.
- Render as GL points with distance fade.

Asteroid particles are not `Body` instances; they do not participate in
body-body collision or labels.

### `build.c` / `build.h`

Interactive sandbox body placement.

Presets:

- Rocky Planet
- Gas Giant
- Ice Planet
- Moon
- Dwarf Planet
- Star

Responsibilities:

- Pause/resume previous simulation state when toggled.
- Maintain selected preset.
- Compute preview position in front of the camera.
- Find the nearest three reference bodies for UI guide lines.
- Choose a parent based on preset type and local context.
- Give non-star bodies a circular-ish orbital velocity around the parent.
- Add bodies through `universe_add_body()`.
- Notify trails, labels, collision, and physics timestep refresh.
- Rebind planets after placing a star.

### `inspect.c` / `inspect.h`

Inspection mode and target orbit camera.

Flow:

1. `I` enters inspection mode and pauses the simulation.
2. `render_frame()` computes `BodyRenderInfo` and calls `inspect_pick_center()`.
3. The body nearest the screen center within a tolerance becomes highlighted.
4. Left-click calls `inspect_begin_orbit()`.
5. Orbit mode eases the camera to a visual distance around the target.
6. Mouse rotates around the target; scroll zooms.
7. Movement keys or Escape cancel back to free-look.

Rendering integration:

- `inspect_ring_params()` exposes the highlighted/target body to `render.c`.
- `render.c` draws a screen-space dashed ring around that body.

### `ui.c` / `ui.h`

2D HUD and pause menu overlay.

HUD elements:

- Top camera-speed bar with logarithmic fill.
- Current movement speed.
- Simulation speed.
- Nearest body/distance readout.
- FPS counter with exponential smoothing.
- Build preset strip while build mode is active.
- Pause menu modal with mouse/keyboard hit testing.

Text rendering is cached per string/color to avoid repeated SDL_ttf uploads.

### `ui_theme.c` / `ui_theme.h`

Shared UI theme helpers:

- Accent color constants.
- `ui_theme_open_font(size)`, which searches common Windows and Linux font paths.

### `audio.c` / `audio.h`

Small SDL_mixer wrapper:

- Initializes OGG support.
- Opens an audio device.
- Loads `assets/soundtrack.ogg`.
- Plays it in a loop.
- Provides volume control and cleanup.

Audio failure is non-fatal; the simulator continues without music.

### `json.c` / `json.h`

Minimal recursive-descent JSON parser.

Supports:

- Objects, arrays, strings, numbers, booleans, null.
- `//` line comments.
- Trailing commas before `]` and `}`.
- Safe accessors with default values.

The parsed tree is heap-allocated and must be released with `json_free()`.

### `gl_utils.c` / `gl_utils.h`

Small OpenGL utility layer:

- Read/compile/link shader programs.
- Create VAOs.
- Create VBOs.
- Create EBOs.

Modules still own their shader uniform locations and vertex attribute layouts.

### `math3d.h`

Header-only vector/matrix utilities:

- Basic Vec3 operations.
- Column-major Mat4 operations.
- Perspective and look-at matrices.
- Translation stripping for skybox/camera-relative rendering.
- Projection helper for world-to-screen tests.

### `laws.c` / `laws.h`

Owns `g_laws` (`UniverseLaws`), the single mutable set of **per-universe**
physical constants the physics hot loop reads instead of compile-time `#define`s.
Fields: `G`, `softening`, `time_scale`, `force_exp`, `lambda`, `pn_factor`,
`c_light`, `gravity_isolation`, plus the adaptive-timestep model
(`outer_period_divisor`, `inner_period_divisor`, `outer_dt_min`, `inner_dt_min`,
`inner_dt_max`, `outer_dt_default` — see §8). `laws_reset()` restores Newtonian
defaults (so JSON fields omitted by a universe fall back to standard physics).
`laws_pair_factor(r2, r)` returns the pairwise acceleration scale, keeping the
inverse-square fast path (no `pow()`) when `force_exp == 2`. See §8 for how each
field enters the force kernel.

### `settings.c` / `settings.h`

Owns `g_settings` (`AppSettings`), the **global, cross-universe** counterpart to
`g_laws`: the app-level tunables that used to be compile-time `#define`s —
starfield density, camera FOV, warm-up / active-region radii, far-field fade
distances, control sensitivities, loading-overlay look, and trail sampling
geometry. Like `g_laws`, call sites read it through the original macro names
(`FOV`, `NUM_STARS`, `ACTIVE_RADIUS_LY`, `SYS_*_FADE_*`, `TRAIL_*`, …) aliased
in `common.h`. Persisted to `settings.json` in the working directory:
`settings_load()` runs first in `main()`; `settings_save()` runs on quit but
only when `settings_dirty()` (a snapshot `memcmp`) reports a change. Two fields
own external resources and apply via the menu's buttons rather than live:
`num_stars` (`settings_apply_starfield()` → re-runs `starfield_init`) and the
overlay font sizes (`settings_apply_fonts()` → `loading_reload_fonts()`). The
"Settings" menu tab edits every field; **per-universe** physics stays in `g_laws`
and the universe JSON, not here.

### `presets.c` / `presets.h`

The "multiverse" registry: an array of `{name, path, blurb}` universes. The menu
enumerates them (`preset_count()`/`preset_at()`); `main.c`'s `switch_universe()`
loads the chosen JSON. `preset_index_of_path()` maps the loaded path back to an
index for menu highlighting. Preset JSON lives in `assets/universes/`.

### `catalog.c` / `catalog.h`

Converts real astronomical catalogs (NASA Exoplanet Archive, JPL Horizons, Gaia)
into ordinary universe JSON the existing loader understands — so imported data
gets laws/rings/asteroids/menu for free. Deliberately free of SDL/OpenGL: it
backs both the offline `tools/catalogtool.c` CLI and the in-app "Import real
data" buttons (which convert to a temp file, then load it). `catalog_convert()`
returns the body count written.

### `lifecycle.c` / `lifecycle.h`

Stellar evolution state machine, on its own clock decoupled from the integrator
(the orbital sim is never sped up). A star's radius/colour is a function of age;
the only moment it perturbs the simulation is death. Phases: `MAIN_SEQUENCE →
SUBGIANT → RED_GIANT → death`; high-mass (≥ ~8 M☉) stars core-collapse to a
neutron star or black hole, low-mass stars puff a planetary nebula to a white
dwarf. `lifecycle_advance_phase()` / `lifecycle_trigger_death()` are driven from
the Inspect panel; `lifecycle_step(dt)` does continuous auto-aging when
`g_stellar_years_per_sec > 0` (default 0 = manual only). Death calls
`supernova_detonate()`, which retires the star and spawns the remnant body.

### `post.c` / `post.h`

HDR bloom post-processing. `post_begin()` binds an offscreen RGBA16F target
before `render_frame()`; `post_end()` runs bright-pass → separable Gaussian blur
→ additive composite to the screen. Both are no-ops when bloom is unavailable or
disabled, so the caller wraps every frame unconditionally. Threshold/intensity
are exposed via `post_get/set_bloom()` (Visuals menu). Targets rebuild on resize.

### `nebula.c` / `nebula.h`

Real-catalogue nebulae as world-space volumetric raymarched clouds. Each has a
true J2000 position + physical radius; one representation at all ranges (a soft
blob when far, an enveloping volume when flown into — no LOD switch). Beyond
`NEBULA_MAX_DIST` the centre/radius are clamped to a shell preserving angular
size, so distant nebulae stay legible as backdrops. (The star-dot / glare /
black-hole passes formerly did the same but now render at true depth and
fade/cull at `farfield_horizon_au` — nebulae keep the shell clamp by design.)
Drawn camera-relative after
opaque geometry with depth test on / writes off. Exposes a Visuals toggle +
density/steps and a Navigate-tab enumeration.

### `galaxy.c` / `galaxy.h`

Catalogue galaxies as world-space volumetric structures (roadmap Layer 4.2,
first iteration) — the same architecture as `nebula.c`: real J2000
RA/Dec + distance + apparent size place the **Milky Way itself** (centred
26 kly toward Sgr A*, disc axis = the real galactic north pole, explicit
50 kly radius, reduced inside-veil brightness that blends back to full once
the camera is outside the volume — from Earth it is the Milky Way band,
from 100 kly out a spiral, no mode switch) plus 10 Local Group / nearby
galaxies (LMC/SMC, M31, M33, M81, NGC 253, Cen A, M51, M104, M87) as
navigable 3D objects; one raymarch representation (`galaxy.frag`, carried by
the reused `nebula.vert` billboard/fullscreen quad) covers backdrop through
fly-through, with the angular-size-preserving far-clamp shell at 1400 AU
(just inside the nebula shell so overlaps sort). The density model is
per-type: **spiral** (exponential thin disc + warm bulge, two logarithmic
arms with FBM star-forming knots, absorbing dust lanes slightly below the
midplane that also extinguish embedded starlight — edge-on discs get the
classic dark stripe; flat-rotation-curve shear on `u_time`, noise in the
co-rotating frame; rays are clipped to the disc slab so the fixed step
budget samples the disc, not empty bounding sphere), **elliptical**
(steep-cored smooth glow), **irregular** (clumpy squashed FBM with pink HII
knots). Each disc axis is tilted off the Earth sightline by the catalogued
inclination (or uses an explicit catalogued pole), so the iconic Earth views
are right. Drawn immediately before the nebulae (farther translucents first).

**Procedural resolved stars** (`galaxy_stars.vert/.frag`,
`galaxy_render_stars()`, the roadmap §0.1 galaxy → stars step): when the
camera is inside or entering a galaxy (< 1.35× radius), attribute-less
point-draw cascades resolve the volume into individual stars. Six cubic
lattices centred on the camera (cells 2 → 2048 ly, 20³ cells × 5 candidates
each) hash stable star positions from absolute galaxy-frame cell coordinates
(grid corner precomputed in double on the CPU — floats stay camera-relative);
a candidate lives if a hash beats the local emission density evaluated by a
port of `galaxy.frag`'s model (the two must stay in sync), so stars trace
the same arms/bulge/knots as the glow. Luminosity is a power-law tail scaled
per cascade (~cell²) so every scale shows its brightest members; each
cascade excludes the next-finer one's box and fades at its rim. Additive
blend at log depth (occluded by planets, no depth writes), crossfaded in by
`render.c` as the painted skybox fades out (47 ly → 4.7 kly from origin), so
the neighbourhood sky hands over to the procedural field with no double
counting. The galaxy anchors in `main.c camera_move()`'s adaptive warp make
arrival at any galaxy auto-decelerate into this resolved regime.

Consumers: Navigate → "Galaxies (fly to)", RadianceField integrated
emitters, and field-graph galaxy nodes.

### `cosmic_field.c` / `cosmic_field.h`

The unified **CosmicField** density/variance field (roadmap Phase A #3): one
queryable spatial field over all content, classifying it as discrete /
continuous / hybrid and reporting local number/mass density, a clumpiness
(spatial-variance) metric, and nebular medium fill at any point. It is the
foundation the continuous-LOD selection (Phase A #2) will consume for its
"local density and field variance" inputs.

Internals: a uniform spatial **hash** over `g_bodies` (open-addressed table of
occupied cells + a CSR body-index pool, built by counting-sort exactly like
`physics.c`'s member-pool build), so only occupied cells cost memory — empty
interstellar space is free. Cells are 1 ly on a side; positions are SI metres
(double), accumulation done in light-years for precision. Rebuilt O(N) on
universe load and throttled per frame (`cosmic_field_tick`, immediate on a
body-count change). Nebulae contribute a continuous `fill` term (linear scan of
the 18), so the abstraction genuinely unifies discrete points with volumetric
fields. Query (`cosmic_field_sample`) walks the sample sphere's cell box (linear
fallback for degenerate huge radii). **Main-thread only** — never call inside the
physics OpenMP warmup. Verified by a live HUD line (`ui.c`) and a headless
`[CosmicField] …` stdout print (`main.c`, `--headless`).

### `radiance_field.c` / `radiance_field.h`

The unified **RadianceField** (roadmap Phase A #4): one queryable
representation of emitted light. Every emitter — thermal stars
(Stefan-Boltzmann; T_eff estimated from the display colour's blue−red balance,
calibrated so Sol → L☉), black holes/AGN (accretion-powered: η·Ṁ·c² from
`accretion.c`, else the authored Eddington ratio, floored at 1% Eddington for
quiet disk-dressed holes), and **transient supernovae** (harvested from
`supernova_render_events()`, ~1e36 W peak, anchored at the detonation point,
`body = -1` in query results) — reduces to a luminosity in watts, a position
and a chromaticity. `radiance_field_sample(pos_m, exclude, out)` answers total
incident irradiance (W/m²; Sun @ 1 AU ≈ 1361) plus the dominant source;
`radiance_field_dominant()` is the cheap argmax used per lit body;
`radiance_field_top()` returns the k brightest contributors (with position +
colour, so body-less transients work identically).

Consumers: `render.c` body/atmosphere lighting (`body_lights()` — the field's
**top two** emitters replace the old parent-chain root-star walk, so binaries,
foreign suns and accreting holes light bodies correctly; `phong.frag`/`atm.frag`
shade a weighted, chromaticity-tinted secondary light via
`u_sun2_rel`/`u_light2`/`u_light2_col`, bit-identical to single-sun when the
weight is 0); `nebula.c` illumination (each nebula samples the field at its
centre — strong incident flux brightens the glow log-compressed, capped ×3.5,
and tints it toward the source: `nebula.frag u_boost`/`u_boost_col`; ordinary
catalogue starlight is orders of magnitude below the 1e-3 W/m² threshold, so
existing scenes are untouched); the HUD field line; and a headless
`[RadianceField]` stdout print. Emitter *positions* are read live from
`g_bodies` each query; membership/luminosity is cached (rebuilt throttled —
every tick while a supernova is active, since its flash decays in real time).
**Main-thread only**, same contract as `cosmic_field`.

Later additions: star temperatures come from `spectral.c` (physical mass+phase
T_eff; the display-colour estimate survives only as the massless-catalog-row
fallback); every light's chromaticity is the blackbody tint of its T_eff
relative to Sol's (white for a Sun twin — established looks preserved; M
dwarfs light their planets warm orange via `u_sun_col` in `phong`/`atm`);
and **nebulae are emitters** (L ∝ area, Orion-class reference, body = -1 with
`RadianceContrib.nebula` carrying the index so a cloud's receiver query can
skip its own glow; `RadianceSample.dom_label` names body-less dominants in
the HUD/headless print).

### `spectral.c` / `spectral.h`

Stellar **spectral classification** (roadmap §1.1) and the RadianceField's
effective-temperature source of truth (§0.3): pure functions of a `Body`
mapping mass + lifecycle phase → T_eff and an MK-style class. Main-sequence T
uses a piecewise mass–temperature slope calibrated so Sol lands exactly on
G2V (TRAPPIST-1 → M6V with its real luminosity, Sirius-mass stars → A);
subgiants cool toward IV, red giants converge on ~3600 K III, and remnants
get compact classes (DA white dwarf, PN nucleus, NS). Stars without a usable
mass fall back to the display-colour estimate the RadianceField used
originally (same Sol calibration). Consumers: `radiance_field.c
star_luminosity()` (Stefan-Boltzmann h = T/T☉), the Inspect panel's
"Class:" line, and the headless `[RadianceField] cls=` print. Stateless and
thread-agnostic.

### `field_graph.c` / `field_graph.h`

The **universe field graph** (roadmap Phase A #5, §0.4): one queryable graph
of typed nodes and edges over relations that already exist implicitly in the
sim, and the backbone later systems attach to (orbit prediction reads gravity
edges, timeline scrubbing replays the event log, galaxy formation adds field
nodes). Nodes are the existing entities (alive `g_bodies` + nebula field
nodes, counted but edge-less so far). Stored edges are harvested in one O(N)
pass on a throttled rebuild (0.25 s, immediate on body-count change):
**GRAVITY** child→parent from `Body.parent`, and **GAS_FLOW** donor→hole from
the accretion model's Roche streams (`accretion_flows()`, with a live kg/s
rate) plus tidal-disruption streams (`Body.tidal_frac`/`tidal_hole`).
**RADIATION edges are deliberately not stored** — at ~16k bodies an all-pairs
body×emitter table is unaffordable — they are computed lazily per query via
`radiance_field_top()` (`field_graph_radiation_top()`).

Evolution/event transitions land in a fixed 256-entry ring
(`FieldGraphEvent`), fed by explicit notify calls at the moment a transition
happens: `lifecycle.c` phase changes, `supernova.c` detonation→remnant (both
the lifecycle and the star-merger path), and `collision.c` merges and tidal
consumptions (`finalize_absorb_body()` carries a `tidal` flag to tell them
apart). Body slots are reused after death, so every event snapshots the
participants' **names**; `field_graph_body_events()` matches index + name, so
a reused slot drops — never misattributes — the previous tenant's history.
Each event also snapshots both clocks (`g_sim_time` and the subject's
`age_yr`) and prints one grep-able `[FieldGraph] event=…` stdout line.

Consumers: the Inspect panel's **Relations** block (orbit chain, gas-flow
edges with Msun/yr rates, top incident lights, per-body history) plus a
collapsed **Recent events** log (menu.c, IMGUI only), and headless
`[FieldGraph] nodes=… edges=… events=…` stats lines at boot and at shot time
(gas flows/events only exist once stellar time has run). Universe reload
clears the log via `field_graph_reset()` (`main.c reset_universe_state()`).
**Main-thread only**, same contract as `cosmic_field`.

### `menu.c` / `menu.h`

The Dear ImGui (cimgui) multiverse overlay, compiled only under `USE_IMGUI`
(`make IMGUI=1`); every function is a no-op stub otherwise, so `main.c` calls
them unconditionally. `menu_render()` runs one ImGui frame after the world draws
and before swap, returning a preset index to switch to (or -1), signalling law
changes, and optionally a JSON path to load (e.g. a just-imported catalog). It
also hosts the per-star Inspect panel that drives `lifecycle.c`. `menu_process_event()`
reports whether ImGui consumed an SDL event so it isn't also treated as game input.

---

## 7. Universe Data Format

`assets/universe.json` (and the `assets/universes/*.json` presets) is the content
source for laws, bodies, rings, and asteroid belts. The local JSON parser accepts
comments and trailing commas.

### Laws block

An optional top-level `"laws"` object overrides the Newtonian defaults for that
universe; any omitted field falls back to the `LAWS_DEFAULT_*` value (so existing
files keep working). Parsed and round-tripped (save/load) by `universe.c`.

```jsonc
"laws": {
  "G": 6.674e-11,          // gravitational constant (m^3 kg^-1 s^-2)
  "softening": 1e5,        // Plummer softening length (m)
  "time_scale": 1.0,       // multiplier on simulated time
  "force_exp": 2.0,        // radial falloff exponent (2 = inverse-square)
  "lambda": 0.0,           // cosmological outward push ∝ distance (dark-energy analogue)
  "pn_factor": 0.0,        // post-Newtonian perihelion precession (1 = physical)
  "gravity_isolation": 1.0,// 1 = star systems gravitate only internally (galaxy-scale; see §8)

  // adaptive-timestep model (all SI seconds; 1 day = 86400):
  "outer_period_divisor": 24,    // slow step = orbital period / this
  "inner_period_divisor": 96,    // fast substep = orbital period / this
  "outer_dt_min": 4320,          // floor on the slow step
  "inner_dt_min": 60,            // floor on the fast substep (below ~60 s physics diverges)
  "inner_dt_max": 1728,          // ceiling on the fast substep
  "outer_dt_default": 86400      // ceiling / fallback for the slow step
}
```

App-level tunables (FOV, starfield count, warm-up radius, fades, …) are **not**
here — they are global, cross-universe, and live in `settings.json` via
`g_settings` (see `settings.c` in §6).

### Body Types

Supported body `type` values:

- `star`
- `black_hole` — loaded/grouped like a star (massive system root) but flagged
  `is_black_hole` for the accretion-disk/shadow render pass instead of glare.
- `planet`
- `dwarf_planet`
- `asteroid`
- `moon`

### Star

```json
{
  "name": "Sun",
  "type": "star",
  "pos_ly": [0.0, 0.0, 0.0],
  "mass": 1.989e30,
  "radius_km": 696000.0,
  "color": [1.0, 0.92, 0.23],
  "obliquity_deg": 7.25,
  "rotation_period_days": 25.38,
  "velocity_km_s": [0.0, 0.0, 0.0]
}
```

`velocity_km_s` is optional. When present, it is applied as system bulk motion
after center-of-mass correction.

### Planet, Dwarf Planet, or Asteroid

```json
{
  "name": "Earth",
  "type": "planet",
  "parent": "Sun",
  "mass": 5.972e24,
  "radius_km": 6371.0,
  "color": [0.27, 0.55, 0.95],
  "obliquity_deg": 23.44,
  "rotation_period_days": 0.99727,
  "keplerian": {
    "a": 1.00000261,
    "e": 0.01671123,
    "i": -0.00001531,
    "Omega": 0.0,
    "omega_tilde": 102.93768193,
    "L": 100.46457166
  },
  "atmosphere": {
    "color": [0.45, 0.65, 1.00],
    "intensity": 0.60,
    "scale": 1.30
  }
}
```

Planet-style Keplerian keys use the compact names currently present in
`universe.json`: `a`, `e`, `i`, `Omega`, `omega_tilde`, and `L`.

### Moon

```json
{
  "name": "Luna",
  "type": "moon",
  "parent": "Earth",
  "mass": 7.342e22,
  "radius_km": 1737.4,
  "color": [0.72, 0.72, 0.68],
  "moon_keplerian": {
    "a_km": 384400.0,
    "e": 0.0549,
    "i_deg": 5.145,
    "Omega_deg": 125.08,
    "omega_deg": 318.15,
    "M0_deg": 115.3654
  }
}
```

### Ring System

```json
{
  "body": "Saturn",
  "n_full": 25000,
  "n_lod": 2000,
  "seed_full": 1234,
  "seed_lod": 5678,
  "e_max": 0.008,
  "h_scale": 5e-7,
  "zones": [
    {
      "r_min_km": 74658,
      "r_max_km": 92000,
      "density": 0.10,
      "color": [0.60, 0.56, 0.50]
    }
  ]
}
```

### Asteroid Belt

```json
{
  "name": "Main Belt",
  "a_min_au": 2.2,
  "a_max_au": 3.2,
  "e_max": 0.25,
  "i_max_deg": 20.0,
  "n_particles": 4000,
  "color": [0.52, 0.49, 0.45],
  "fade_start_au": 5.0,
  "fade_end_au": 12.0,
  "seed": 2712384468
}
```

---

## 8. Physics Architecture

The integrator is a 2R-RESPA split scheme.

Force split:

| Force class | Bodies | Step rate |
|---|---|---|
| Slow | star-star, star-planet, planet-planet, non-parent perturbations | outer timestep |
| Fast | dominant non-star parent to satellite, plus reaction force | inner timestep |

Only moons/satellites use fast parent forces. Star-orbiting planets are treated
as primaries and are integrated by slow forces.

One outer step:

```text
physics_respa_begin_system(root, dt_outer)
    compute slow accelerations
    v += 0.5 * slow_acc * dt_outer
    prime fast accelerations

for each inner substep:
    physics_respa_inner_system(root, dt_inner)
        v += 0.5 * fast_acc * dt_inner
        x += v * dt_inner
        recompute fast_acc
        v += 0.5 * fast_acc * dt_inner

physics_respa_end_system(root, dt_outer)
    recompute slow accelerations
    v += 0.5 * slow_acc * dt_outer
    rotation_angle += rotation_rate * dt_outer
```

`physics_advance_time(dt)` is called outside the per-system loop so global time
advances once after all systems have reached the same simulation time.

Timestep model:

1. Rebuild root-star system slots.
2. Assign every live body to its root system.
3. For each non-star body, choose a timestep anchor:
   - explicit parent if alive
   - otherwise strongest gravitational influence by `r^2 / mass`
4. Estimate period with `T = 2*pi*sqrt(r^3 / GM)`.
5. Compute:
   - `dt_outer = T / 24`, clamped to `[0.05 day, 1 day]`
   - `dt_inner = T / 96`, clamped to `[60 s, 0.02 day]`
6. Tighten satellite outer dt when third-body perturbations are significant.
7. Store the tightest outer/inner limits per system.

Warmup:

- Runs two simulation years before the first frame.
- Uses OpenMP to parallelize independent star systems.
- Ticks trails during warmup so orbits are visible immediately.
- Calls `physics_advance_time(WARMUP_DT)` once after all systems complete.

Trail sampling:

- Stored per body in `Body`.
- Driven by travelled distance and Hermite curve subdivision, not just time.
- Uses frame snapshots so collision code can cut trails at an impact time.
- Dead bodies can stop emitting and fade retained trails.

### Configurable laws

The force kernel reads `g_laws` (see `laws.c`) rather than constants:

- **G / softening** thread through every pairwise acceleration. `laws_pair_factor()`
  computes the pair scale, keeping the no-`pow()` fast path for inverse-square.
- **force_exp** changes the radial falloff exponent (2 = Newtonian; only 1 and 2
  give closed orbits — others precess).
- **lambda** adds a cosmological outward term `a += lambda * r_vec`.
- **pn_factor / c_light** add a post-Newtonian perihelion-precession term.
- **time_scale** multiplies simulated dt.

`laws_reset()` runs before each universe load so unspecified JSON fields are
physically standard.

### 8.1 Galaxy-scale: active region and far-field rendering

A live N-body sim can't brute-force a galaxy. The model (Space Engine / Celestia
style): star systems are gravitationally independent islands, so only the camera's
neighbourhood is fully simulated and everything else is a cheap point. The
essentials:

- **Gravity isolation** (`g_laws.gravity_isolation`, default on): a body only feels
  others in its own system, turning the force kernel from O(active × N) into
  ≈ O(N). Turn it off only for deliberately-coupled scenarios (clusters).
- **Active region** (`main.c`): only systems within `ACTIVE_RADIUS_LY` of the
  camera are stepped; distant systems freeze. Warmup pre-simulates only systems
  within `WARMUP_RADIUS_LY` (just the Solar System at boot, so boot is fast).
- **Heap-grown tables**: physics member tables (CSR layout) and `render.c` scratch
  + dot VBO grow with `g_nbodies`, which can be ~16k.
- **Near/far dot split** (`render.c`): near bodies get full per-dot treatment
  (priority sort, overlap dedup, glare occlusion); the ~16k far bodies go through
  one O(N) pass projected camera-relative in *double* (floating origin), emitted
  into the dot VBO with a single draw call.
- **Labels follow the camera** (B2-labels ✅): `physics_active_bodies()` is the
  single source of truth for "which bodies are near the camera" (built from the
  CSR member pools, nearest systems first); `labels.c` treats its fixed
  `MAX_BODIES` arrays as a slot *cache* keyed by cache slot, evicting slots as
  bodies leave the active region — so any of the ~16k systems gets labelled as
  you approach.
- **Caveat (B2-collision, still TODO)**: `collision.c` still uses fixed
  `[MAX_BODIES]` tables covering the first 128 body *indices*, not the 128
  nearest the camera — collisions don't yet follow you across the galaxy. Its
  per-pair state (`s_pair_next[MAX_BODIES][MAX_BODIES]` cooldowns, rollback
  snapshots) is index-keyed and must survive across frames, so the labels-style
  slot-cache remap doesn't apply; it needs sparse per-pair tables first.
  **Do not just raise `MAX_BODIES`** — `collision.c` has
  `[MAX_BODIES][MAX_BODIES]` *stack* arrays that would explode.

---

## 9. Collision and Supernova Flow

`main.c` and `collision.c` cooperate to avoid missing fast impacts:

1. At the start of a physics frame, `trails_begin_frame_snapshot()` and
   `collision_snapshot_positions()` capture state.
2. For each system, `collision_system_close_approach_subdivide()` can multiply
   the outer-step count.
3. Before each outer step, `collision_system_maybe_has_encounter()` can request
   a fresh local snapshot.
4. After integrating that local step, `collision_step_system()` checks local
   encounters.
5. After all systems advance, `collision_step(effective_sim_dt)` handles broad
   global collision work and event updates.

Collision classes:

- Minor crater/heat events.
- Major impacts with longer heat and larger scars.
- Merge events where bodies visually intersect and then absorb.
- Star-star collisions, delegated to `supernova_try_trigger()`.

On normal merge completion:

- Momentum and mass are transferred.
- Radius/spin/obliquity are updated.
- The absorbed body is marked dead.
- Labels/rings/trails are notified.
- Physics timestep model is refreshed.

On star-star supernova:

- A remnant star is inserted.
- Source stars are retired.
- Child systems are rebound to the remnant.
- Nearby vulnerable subtrees may be destroyed immediately.
- Surviving root-orbiting subtrees receive delayed shock pushes.
- Render events drive volumetric shaders until the event expires.

The same machinery is reused by the stellar lifecycle: a star reaching end of
life calls `supernova_detonate(star)`, which retires the star and spawns the
mass-appropriate remnant — black hole, neutron star, or (low-mass) a gentle
planetary-nebula puff to a white dwarf — scaling the blast accordingly.

### Shock-push performance (galaxy scale)

The shock-kick path is kept linear so a supernova in a 16k-body universe stays
real-time:

- **Bounded reach.** The maximum blast radius is fixed at detonation —
  `shock_speed × CLOUD_DURATION`, and `shock_speed` scales with progenitor mass,
  so a heavier star reaches farther. A single O(N) scan at detonation collects
  the root-orbiting bodies within that radius into a per-event candidate list;
  `supernova_step()` then visits only that list each frame instead of all bodies.
- **Subtree via child index.** `body_subtree_mass()` / `apply_velocity_to_subtree()`
  / `destroy_subtree()` traverse a cached CSR child index (`children_ensure()`,
  rebuilt lazily at most once per frame) rather than scanning all bodies to find
  descendants — O(subtree) instead of O(N) per kicked body.

Together these turn what was O(N²) (every body × all-bodies descendant scan, every
frame) into O(N), so even a massive blast in a dense cluster — or with
`gravity_isolation` off — only does work proportional to the bodies actually hit.

---

## 10. Rendering Pipeline

`main.c` builds:

- `proj`: perspective projection.
- `view`: full camera look-at with float eye.
- `view_rot`: rotation-only look-at from origin.

`render_frame(view, proj, view_rot, dt)` primarily uses
`vp_camrel = proj * view_rot`, with body positions already camera-relative.

`main.c` wraps the scene in the bloom pipeline: `post_begin()` binds an offscreen
HDR target, `render_frame()` draws into it, `post_end()` blooms and composites to
the screen — then the ImGui menu and UI draw on top so they don't bloom. When
bloom is disabled both wrappers are no-ops and rendering goes straight to the
default framebuffer.

Current pass order (within `render_frame`):

| Order | Pass | Notes |
|---|---|---|
| 1 | Starfield | Catalog/procedural GL points, rotation-only skybox |
| 2 | Body render info | Compute visual radii, distances, screen metrics |
| 3 | Inspection pick | Updates highlighted body from screen center |
| 4 | Spheres | Ray-sphere billboard shader (excludes black holes) |
| 5 | Atmospheres/heat glows | Additive atmospheric shell shader |
| 6 | Black holes | `bh` billboard: accretion disk + shadow + photon ring |
| 7 | Nebulae | World-space volumetric raymarch (depth test on, writes off) |
| 8 | Supernova cloud | Volumetric ejecta raymarch — rendered to a **half-res** target then composited back (`vol_composite`); core/flash stays full-res |
| 9 | Supernova core/flash | Hot core and flash shader |
| 10 | Collision particles | Additive GL point debris |
| 11 | Center dots | Near/far split: sized star dots + far-field O(N) pass |
| 12 | Rings | Keplerian ring particles |
| 13 | Asteroids | Belt GL points |
| 14 | Trails | Retained orbital paths |
| 15 | Star glare | Additive star halo billboards (sub-pixel glares culled) |
| 16 | Build preview | Ghost dot, guide lines, distance labels |
| 17 | Inspection ring | Screen-space dashed target ring |
| 18 | Labels | Billboard text labels |
| — | UI / ImGui | Drawn by `main.c` after `post_end()`, so they don't bloom |

(Exact ordering/section numbers live in `render.c`; treat this as the conceptual
order. The nebula/black-hole passes share the camera-relative double-precision
floating-origin trick described above.)

Depth and blending:

- Spheres and all world passes use logarithmic depth in shaders, normalised
  against a single shared range: `RENDER_DEPTH_FAR` (`src/common.h`, currently
  1e10 AU ≈ 158 kly). `gl_shader_load()` (`src/gl_utils.c`) splices `#define
  DEPTH_FAR <value>` after each shader's `#version` line, so every depth-writing
  pass — including `bh.frag`/`torus.frag` (now log, previously standard
  `0.5+0.5·z/w`) and the additive `jet.frag`/`agncore.frag` depth *tests* —
  sorts on the identical metric. The CPU perspective far plane (`main.c`) uses
  the same `RENDER_DEPTH_FAR`; log depth preserves near precision across the huge
  range, so geometry from planet surface to interstellar distance renders in one
  pass with no mode switch. The three volumetric shaders (`nebula`,
  `supernova_core`, `supernova_cloud`) keep a separate `VOL_FAR` (2000 AU) for
  their opacity/fade falloff — decoupled from the depth range so raising the
  latter doesn't push their fades out.
- Additive effects use `GL_SRC_ALPHA, GL_ONE` or `GL_ONE, GL_ONE`.
- Labels/UI use alpha blending.
- Some overlays disable depth testing by design.
- `GL_DEPTH_CLAMP` is enabled so nearby billboard spheres do not clip through
  the near plane.

Small-body rendering (continuous LOD):

- Bodies **crossfade** between dot and ray-sphere rendering based on projected
  pixel size: the sphere's opacity (`phong.frag u_opacity`) is a smoothstep over
  the `BODY_DOT_FADE_START/END_PX` window and the dot's alpha is its exact
  complement, so the handoff conserves brightness — no pop. A transitioning
  sphere alpha-blends without writing depth (so it can't depth-kill its own
  fading dot); only a fully-resolved sphere writes depth and occludes dots.
  Star glare crossfades against the star dot the same way over the
  `STAR_DOT_*_GLARE_PX` window, and the atmosphere glow fades in with the
  sphere instead of snapping on. `info[i].show` remains a binary routing flag
  (picking, far dot pass).
- The crossfade windows are scaled once per frame by a **density LOD factor**
  (`lod_update_density_scale()` in `render.c`), sampled from the CosmicField at
  the camera (local density × clumpiness, log-compressed, capped): in dense
  fields representations resolve later so the detail budget follows the field
  — LOD driven by camera distance *and* the field (roadmap Phase A #2).
- Dots split near vs. far by camera distance (`NEAR_DOT_DIST`). **Near** dots get
  the full treatment: priority-ordered overlap dedup (stars, planets, moons),
  glare occlusion, dot↔sphere fade. **Far** dots (the ~16k bulk) go through one
  O(N) pass — projected camera-relative in double precision (floating origin),
  no O(N²) work, one draw call.
- Star dots are sized per-point (`star_dot.vert`) from an apparent-magnitude
  estimate, so brighter/nearer stars read larger.
- Distant stars, glare, and black holes render at their **true** camera-relative
  depth and fade/cull past `g_settings.farfield_horizon_au` (a live, persisted
  menu setting, "Far-field horizon" under *Far-field fade*). This replaces the old
  pin-to-shell clamp that pasted far objects onto a fixed ~1500 AU shell; see
  `farfield_horizon_fade()` in `render.c`. The horizon is clamped ≤
  `RENDER_DEPTH_FAR` so nothing renders past the depth range.

---

## 11. Shader Reference

| Shader | Purpose |
|---|---|
| `phong.vert/frag` | Billboard ray-sphere planets/stars, procedural surfaces, collision scars |
| `atm.vert/frag` | Ray-shell atmospheric and collision heat glow |
| `color.vert/frag` | Starfield and body/dot GL points |
| `star_dot.vert` + `color.frag` | Per-point-sized star dots (`gl_PointSize` from magnitude) |
| `solid.vert/frag` | Trail line strips |
| `ring.vert` + `color.frag` | Keplerian ring particle positions and colors |
| `asteroid_particle.vert` + `color.frag` | Asteroid belt point size/fade |
| `impact_particle.vert/frag` | Collision debris particles |
| `label.vert/frag` | Textured label billboards |
| `star_glare.vert/frag` | Additive star corona quads |
| `build_line.vert/frag` | Build-mode guide lines |
| `ui.vert/frag` | HUD, pause menu, text quads, simple screen-space primitives |
| `supernova_billboard.vert` | Shared billboard vertex shader for supernova passes |
| `supernova_core.frag` | Supernova flash/core rendering |
| `supernova_cloud.frag` | Supernova ejecta cloud rendering |
| `bh.vert/frag` | Black hole: inclined accretion disk, opaque shadow, photon ring, Doppler beaming |
| `nebula.vert/frag` | World-space volumetric raymarched nebulae (domain-warped FBM) |
| `post_quad.vert` + `bloom_bright/blur/composite.frag` | HDR bloom: bright-pass → separable Gaussian → additive composite |
| `post_quad.vert` + `vol_composite.frag` | Upscale + composite the half-res supernova-cloud layer back over the scene (premultiplied over) |

`phong.frag` receives:

- camera ray basis and screen/FOV data
- body center/radius/color/emission
- rotation/obliquity
- planet type
- top-two light sources from the RadianceField (primary position + secondary
  position/weight/chromaticity; see `radiance_field.c`)
- star heat
- up to `COLLISION_MAX_SPOTS` impact/scar uniforms

---

## 12. File Dependency Map

High-level dependency map:

```text
main.c
|-- common.h, math3d.h
|-- camera.h, body.h, universe.h, physics.h
|-- collision.h, supernova.h
|-- render.h, starfield.h, trails.h, labels.h
|-- rings.h, asteroids.h, build.h, inspect.h
|-- ui.h, audio.h

render.c
|-- body.h, camera.h, math3d.h, gl_utils.h
|-- collision.h, supernova.h
|-- starfield.h, rings.h, asteroids.h, trails.h, labels.h
|-- build.h, inspect.h, ui_theme.h

physics.c
|-- physics.h, body.h, collision.h
|-- also implements physics-side trail sampling helpers

collision.c
|-- collision.h, body.h, labels.h, physics.h
|-- rings.h, supernova.h, trails.h

supernova.c
|-- supernova.h, body.h, collision.h
|-- labels.h, trails.h, universe.h

universe.c
|-- universe.h, body.h, json.h

build.c
|-- build.h, body.h, camera.h, physics.h
|-- universe.h, trails.h, labels.h, collision.h

inspect.c
|-- inspect.h, body.h, camera.h, physics.h
|-- labels.h, math3d.h

ui.c
|-- ui.h, camera.h, physics.h, build.h
|-- body.h, gl_utils.h, ui_theme.h

rings.c
|-- rings.h, body.h, json.h, gl_utils.h, math3d.h

asteroids.c
|-- asteroids.h, body.h, json.h, gl_utils.h

labels.c
|-- labels.h, body.h, camera.h, gl_utils.h
|-- math3d.h, ui_theme.h

starfield.c
|-- starfield.h, gl_utils.h

trails.c
|-- trails.h, body.h, camera.h, gl_utils.h, math3d.h

json.c, gl_utils.c, camera.c, audio.c, ui_theme.c
|-- mostly standalone utility modules
```

---

## 13. Where to Make Changes

### Add or change universe content

Edit `assets/universe.json` (or a file under `assets/universes/`).

- Add stars before their planets.
- Add planets/dwarf planets/asteroids before their moons.
- Use `keplerian` for star-orbiting bodies.
- Use `moon_keplerian` for moons.
- Add rings under the top-level `"rings"` array.
- Add belts under the top-level `"asteroid_belts"` array.
- Add an optional `"laws"` block to give the universe non-Newtonian physics.

### Add a universe to the multiverse menu

Edit `src/presets.c` — add a `{name, path, blurb}` entry pointing at a JSON file
under `assets/universes/`. It then appears in the `U` menu picker automatically.

### Add or change a physical law

Edit `src/laws.h` (struct field + `LAWS_DEFAULT_*`), `src/laws.c` (`laws_reset`),
and `src/universe.c` (parse + save in the `"laws"` block). Apply the field in the
force kernel in `src/physics.c`; if it affects stability, update timestep
selection. Expose it on a slider in `src/menu.c` if useful.

### Add a new build preset

Edit `src/build.c`.

- Add a `BuildPreset` entry.
- Add color/rotation behavior if the existing visual type enum is not enough.
- Notify rendering if the preset needs a new procedural surface class.

### Add a new procedural planet look

Edit:

- `src/render.c`, `get_planet_type()`
- `assets/shaders/phong.frag`

Keep the integer type mapping synchronized between CPU and shader.

### Add a new body-level render effect

Edit:

- `assets/shaders/phong.frag` or a new shader.
- Uniform lookup/upload in `src/render.c`.
- Any state producer module, if the effect depends on simulation state.

Follow the collision/supernova pattern: simulation modules expose compact render
data; `render.c` owns GL resources and shader calls.

### Add a new HUD or menu element

Edit `src/ui.c`.

- Add text cache entries for text.
- Use the existing `ui.vert/frag` path.
- Keep hit testing and layout structs together for menu items.

### Add a new physics force

Edit `src/physics.c`.

- Decide whether the force is fast or slow.
- Add it to the matching acceleration computation.
- Update timestep selection if it changes stability limits.
- Consider whether trails/collisions need denser sampling around the new force.

### Add a new collision visual

Edit:

- `src/collision.h` for a new `CollisionVisualKind`.
- `src/collision.c` for event creation/lifetime.
- `src/render.c` for uniform upload if new data is required.
- `assets/shaders/phong.frag` for visual interpretation.

### Add a new shader

Edit:

- Add shader files under `assets/shaders/`.
- Load them in the owning module's init function.
- Store uniform locations near other GL state.
- Release the program in the module shutdown function.

### Add a new platform font path

Edit `src/ui_theme.c` and append the path to `s_ui_font_paths` before `NULL`.

### Change coordinate conventions

Be careful. The convention is baked into:

- `body.c` orbital conversion.
- `camera.c` yaw/pitch direction.
- `math3d.h` look-at/projection helpers.
- Ring bases in `rings.c`.
- Shader assumptions about body-local rotation and obliquity.

Update all of them together or avoid changing the convention.

---

## Current Reference Date

This document reflects the codebase in this repository as of June 2026, after the
multiverse-laws, real-data-import, galaxy-scaling, and stellar-lifecycle work. The
source files remain the authoritative reference when behavior and documentation
disagree; `docs/` tracks in-progress and planned work.
