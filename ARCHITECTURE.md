# OpenVerse Architecture

This document is the contributor-facing architecture reference for OpenVerse.
It reflects the current C99/OpenGL codebase and is meant to help humans and
LLM assistants understand where each subsystem lives, what owns what state, and
which invariants matter when changing the simulator.

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

OpenVerse is a real-time, data-driven universe simulator written in C99.
It combines a hierarchical N-body gravity simulation with an OpenGL 3.3 Core
renderer that can show bodies from planet-surface scale to interstellar scale.

Primary technologies:

| Area | Technology |
|---|---|
| Language | C99 |
| Window/input/audio bootstrap | SDL2 |
| Text rendering | SDL2_ttf |
| Music playback | SDL2_mixer |
| Rendering | OpenGL 3.3 Core, GLSL 330, GLEW |
| Physics | 2R-RESPA split integrator with per-star-system timestep limits |
| Data | `assets/universe.json`, parsed by the local JSON parser |
| Parallelism | OpenMP for warmup across independent star systems |

Design goals:

- Keep simulation state in SI units for physical correctness.
- Convert to AU/camera-relative coordinates only at render boundaries.
- Keep body indices stable across collisions and runtime additions.
- Prefer data-driven universe content over hard-coded solar-system data.
- Let visual systems query physics/collision state instead of owning it.

---

## 2. Repository Layout

```text
OpenVerse/
├── Makefile
├── package_linux.sh
├── package_windows.sh
├── README.md
├── ARCHITECTURE.md
├── CONTRIBUTING.md
├── assets/
│   ├── universe.json
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
│       └── supernova_billboard.vert / supernova_core.frag / supernova_cloud.frag
└── src/
    ├── main.c              App init, event loop, per-frame scheduling
    ├── common.h            Shared includes, constants, global window size
    ├── math3d.h            Header-only vector/matrix helpers
    ├── gl_utils.c/.h       Shader and buffer helper functions
    ├── json.c/.h           Minimal JSON parser with comments/trailing commas
    ├── body.c/.h           Body array, Keplerian conversion, body helpers
    ├── universe.c/.h       JSON loader, runtime body allocation/reuse
    ├── physics.c/.h        RESPA gravity and trail sampling engine
    ├── collision.c/.h      Collision detection, merges, scars, debris
    ├── supernova.c/.h      Star-star collision aftermath
    ├── camera.c/.h         Global free-look camera state
    ├── render.c/.h         Main scene compositor
    ├── trails.c/.h         GL upload/draw layer for trail buffers
    ├── labels.c/.h         SDL_ttf label textures and overlap avoidance
    ├── starfield.c/.h      Catalog-backed skybox stars
    ├── rings.c/.h          Keplerian ring particle systems
    ├── asteroids.c/.h      Gravity-integrated asteroid belt particles
    ├── build.c/.h          Runtime sandbox body placement
    ├── inspect.c/.h        Inspection and orbit-camera mode
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
-Wall -Wextra -O2 -std=c99 -I$(SRCDIR) -fopenmp
```

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
| `MAX_BODIES` | compile-time body-slot and per-frame array limit |
| `TRAIL_LEN` | power-of-two trail ring-buffer length per body |
| `FOV` | vertical field of view in degrees |

### Body

`Body` in `body.h` is the central simulation entity. It stores:

- Display name, mass, radius, color, star flag.
- SI position, velocity, slow acceleration, and fast parent acceleration.
- Stable lifecycle fields: `alive`, `parent`.
- Per-body timestep estimates.
- Rotation, obliquity, atmosphere parameters.
- Trail ring buffer and frame-snapshot fields.

Invariants:

- `g_nbodies` is the high-water slot count, not the number of living bodies.
- Dead/absorbed slots remain addressable and may be reused by
  `universe_add_body()`.
- Runtime additions never exceed `MAX_BODIES`.
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
| `universe_add_body()` | Add or reuse one body slot from a filled `BodyCreateSpec` |
| `universe_live_body_count()` | Count `alive` bodies |
| `universe_can_add_body()` | Check live count against `MAX_BODIES` |
| `universe_rebind_to_nearest_stars()` | Reassign star-orbiting bodies after sandbox star additions |
| `universe_shutdown()` | Free trail buffers and the body array |

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

---

## 7. Universe Data Format

`assets/universe.json` is the content source for bodies, rings, and asteroid
belts. The local JSON parser accepts comments and trailing commas.

### Body Types

Supported body `type` values:

- `star`
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

---

## 10. Rendering Pipeline

`main.c` builds:

- `proj`: perspective projection.
- `view`: full camera look-at with float eye.
- `view_rot`: rotation-only look-at from origin.

`render_frame(view, proj, view_rot, dt)` primarily uses
`vp_camrel = proj * view_rot`, with body positions already camera-relative.

Current pass order:

| Order | Pass | Notes |
|---|---|---|
| 1 | Starfield | Catalog/procedural GL points, rotation-only skybox |
| 2 | Body render info | Compute visual radii, distances, screen metrics |
| 3 | Inspection pick | Updates highlighted body from screen center |
| 4 | Spheres | Ray-sphere billboard shader for visible bodies |
| 5 | Atmospheres/heat glows | Additive atmospheric shell shader |
| 6 | Supernova clouds | Volumetric ejecta billboard shader |
| 7 | Supernova core/flash | Hot core and flash shader |
| 8 | Collision particles | Additive GL point debris |
| 9 | Center dots | Small-body/star point indicators with overlap pruning |
| 10 | Rings | Keplerian ring particles |
| 11 | Asteroids | Belt GL points |
| 12 | Trails | Retained orbital paths |
| 13 | Star glare | Additive star halo billboards |
| 14 | Build preview | Ghost dot, guide lines, distance labels |
| 15 | Inspection ring | Screen-space dashed target ring |
| 16 | Labels | Billboard text labels |
| 17 | UI overlay | Called by `main.c` after `render_frame()` |

Depth and blending:

- Spheres and many world passes use logarithmic depth in shaders.
- Additive effects use `GL_SRC_ALPHA, GL_ONE` or `GL_ONE, GL_ONE`.
- Labels/UI use alpha blending.
- Some overlays disable depth testing by design.
- `GL_DEPTH_CLAMP` is enabled so nearby billboard spheres do not clip through
  the near plane.

Small-body rendering:

- Bodies transition between ray-sphere rendering and dot rendering based on
  projected pixel size.
- Dot overlap is greedily resolved in priority order: stars, planets, moons.
- Stars can be clamped toward the far plane for visibility during warp travel.

---

## 11. Shader Reference

| Shader | Purpose |
|---|---|
| `phong.vert/frag` | Billboard ray-sphere planets/stars, procedural surfaces, collision scars |
| `atm.vert/frag` | Ray-shell atmospheric and collision heat glow |
| `color.vert/frag` | Starfield and body/dot GL points |
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

`phong.frag` receives:

- camera ray basis and screen/FOV data
- body center/radius/color/emission
- rotation/obliquity
- planet type
- nearest sun position
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

Edit `assets/universe.json`.

- Add stars before their planets.
- Add planets/dwarf planets/asteroids before their moons.
- Use `keplerian` for star-orbiting bodies.
- Use `moon_keplerian` for moons.
- Add rings under the top-level `"rings"` array.
- Add belts under the top-level `"asteroid_belts"` array.

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

This document reflects the codebase in this repository as of May 2026. The
source files remain the authoritative reference when behavior and documentation
disagree.
