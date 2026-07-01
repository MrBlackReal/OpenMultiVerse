# Galaxy-scale ("load everything") — handoff & TODO

Status doc for the work that lets OpenMultiVerse load far more than the old
128-body limit and simulate Space-Engine-style: only the region around the
camera is fully simulated; everything else is a cheap far-field point.

Updated after the perf pass that took the full ~16k-body catalog from ~3 to
~30 FPS. Read this top-to-bottom before continuing.

---

## The goal / architecture

A live N-body sim can't brute-force a galaxy (O(N²) gravity, O(N²) collision).
The plan, like Space Engine / Celestia:

- **Stars don't meaningfully gravitate each other** — interstellar forces are
  negligible. Each star system is an island.
- **Only the system(s) the camera is in are fully simulated**; distant systems
  freeze and render as points.
- Render/collision/labels operate on a **bounded active set**, not all N.

Phasing:
- **Phase 1** — remove the hard 128-body cap from the core. ✅ done
- **Safety floor** — make render/collision/labels not overflow past 128. ✅ done
- **Phase 2** — camera-driven active region; freeze distant systems. ✅ done
- **Phase 3** — far-field points + floating origin. 🟡 B1 done & verified
  (near/far dot split + glare draw-call cull took the full 16k-body catalog from
  ~3 to ~30 FPS); B2 (camera-following active-128 for collision/labels) still
  TODO.
- Plus follow-ups (below).

---

## What is DONE

### 1. Physics core is no longer capped at 128 (`src/physics.c`)
- The fixed `s_system_*[MAX_BODIES]` arrays and the O(N²)
  `s_system_members[MAX_BODIES][MAX_BODIES]` matrix were replaced with
  **heap-allocated, grow-on-demand** tables, sized to `g_nbodies` via
  `physics_ensure_capacity()`. Members now use a **CSR layout**:
  `s_member_off[]` (offsets) + `s_member_pool[]` (flat indices), accessed by
  `sys_member(slot, k)`.
- Bounds checks that read `MAX_BODIES` now use the dynamic `s_cap` /
  `g_nbodies`. `physics_refresh_timestep_model()` rebuilds members in two passes
  (count → offsets → fill).
- For ≤128-body universes behaviour is identical (same grouping/order).

### 2. Safety floor for >128 bodies
- `src/render.c` — `render_frame()`'s per-frame scratch arrays (`info`,
  `body_px`, all `dot_*`, `dot_data`) are now heap buffers grown by
  `render_scratch_ensure()`; the dot VBO grows past its initial `MAX_BODIES`
  sizing (`s_dot_vbo_cap`). So **all** bodies render.
- `src/collision.c` — every `g_nbodies` reference replaced with `cnb()` =
  `min(g_nbodies, MAX_BODIES)`. Collision still uses fixed `[MAX_BODIES]`
  tables (incl. the `s_pair_next[MAX_BODIES][MAX_BODIES]` schedule), so it
  safely processes only the first `MAX_BODIES` bodies. No overflow.
- `src/labels.c` — `labels_render()` clamps its fixed scratch to
  `nb = min(g_nbodies, MAX_BODIES)`.

### 3. Phase 2 — camera active region (`src/main.c`)
- Helpers: `camera_world_m()`, `system_cam_dist2()`, and
  `#define ACTIVE_RADIUS_LY 2.0`.
- **Live loop** (`main()` physics block, ~line 980+): the global timestep-cap
  loop and the per-system step loop both `continue` past systems farther than
  `ACTIVE_RADIUS_LY` from the camera. Distant systems **freeze** and no longer
  drag down the timestep of the system you're in.
- **Warm-up** (`warmup_universe()`): only pre-simulates systems within
  `WARMUP_RADIUS_LY = 1.5` of the camera (plus always the nearest). At boot
  that's just the Solar System, so boot is fast.

### 4. Data + tooling
- `tools/build_known_universe.py` merges the Solar System + nearest N real
  exoplanet/Gaia systems into `assets/universes/known_universe.json`
  (registered as the "Known Universe" preset in `src/presets.c`).
  - `--max-systems N` controls size; **the default is now 0 = everything**, so
    the shipped preset is the full catalog: ~16,000 bodies (9,709 stars / 6,277
    exoplanets). Pass an explicit `--max-systems N` for a smaller slice.
  - `merge()` dedups whole systems case-insensitively and re-parents planets to
    the surviving star (fixes the Barnard's Star double).
  - `drop_unplaced()` removes catalogue systems with **no real distance** (the
    importer parks those at the origin, on top of the Sun — that caused the
    "28 systems warm up" bug). Only the Sun is at the origin now.
- Full catalogs downloaded to `assets/catalogs/{exoplanets,gaia}_full.csv`
  (gitignored). NASA: 6298 planets; Gaia: 5000 nearest stars.

### 5. Menu redesign + navigation (`src/menu.c`)
- `menu_apply_style()` adds rounding/padding + an indigo-cyan accent.
- `menu_render()` is split into two tabs (toggle the panel with `U`):
  - **Universe** — presets, Physics laws (sliders + "Isolate star systems"
    toggle + "Reset to Newtonian"), Import real data, Save/load snapshot.
  - **Navigate** — case-insensitive name search over every live body with a
    click-to-teleport result list. `teleport_to_body()` frames the camera to the
    target's radius and looks straight at it, so you can reach any of the ~16k
    objects instantly; the destination system becomes active on arrival.

### 6. Runtime body creation works past 128 (`src/universe.c`)
`universe_add_body()` (build mode, supernova remnants, collision spawns) carried
three stale `MAX_BODIES` caps left over from before the heap-growth work, so it
silently returned -1 in the full ~16k catalogue (e.g. the **Trigger Supernova**
button did nothing). Fixed: `universe_can_add_body()` now always allows (the array
grows on the heap via `ensure_capacity()`, exactly like the loader),
`find_reusable_body_slot()` scans all slots (not just the first 128) so retired
slots anywhere get reused, and the `g_nbodies >= MAX_BODIES` growth guard is gone.

### 7. Stellar lifecycle + supernova run at galaxy scale
New `src/lifecycle.{c,h}` (main-sequence → giant → white dwarf / neutron star /
black hole; manual via the Inspect panel or continuous via `g_stellar_years_per_sec`).
Death routes through `supernova_detonate()`. Two perf passes keep a supernova
real-time in a 16k-body universe:
- **Render** — the volumetric cloud raymarch is fragment-bound and fills the
  screen near the blast. It now renders to a **half-res** target and composites
  back (`render.c` + `vol_composite.frag`); core/flash stays full-res. (This is
  the half-res volumetrics follow-up the nebula pass also wants — see
  VISUALS_ROADMAP; the nebula could reuse the same target.)
- **Shock physics** — was O(N²): `supernova_step()` scanned all bodies every
  frame and each kicked body did an O(N) descendant scan. Now the reachable set
  (bodies within `shock_speed × CLOUD_DURATION`, mass-dependent) is gathered once
  at detonation into a per-event candidate list, and the subtree ops walk a cached
  CSR child index (`children_ensure()`), so the whole path is O(N).

### Earlier correctness fixes (already shipped, for context)
bv stack overflow, PN softening, JSON-escape/finite-guard on snapshot save,
`universe_validate()` so a bad menu path doesn't `exit(1)`, rings/belts in
snapshots, softening floor, snapshot parent-by-index + name dedup. See git.

---

## What is LEFT TODO (in priority order)

### A. Decouple cross-system gravity  ✅ DONE
**Was:** Even though only active systems are *stepped*, an active body's force
kernel looped **all** `g_nbodies` for cross-system pairs (negligible interstellar
gravity) — O(active × N) per substep, run ~240×/frame, the wall before thousands.
**Done:** Added law field `g_laws.gravity_isolation` (default **1.0 = on**;
`LAWS_DEFAULT_GRAV_ISOLATION` in `laws.h`, reset in `laws.c`, parsed/saved in the
JSON "laws" block in `universe.c`, toggle "Isolate star systems" in the Physics
laws menu). When on, the system-aware path of `compute_acc_slow_system()`
(`src/physics.c`) iterates the inner `j` loop over the **same system's member
list** instead of all bodies, so cross-system pairs vanish — O(active × N) →
O(Σ Nᵢ²) ≈ O(N). Same-system semantics (Newton-3rd `j<i` dedup, satellite/
ancestor exclusions, softening cutoff) are byte-identical to before. For a
single-system universe every body is one system, so behaviour is unchanged. Set
`gravity_isolation:0` in JSON (or untick the menu box) for a deliberately-coupled
scenario (cluster / galaxy collision). The legacy/warmup all-bodies path
(`root<0`) was left as-is (runs once, nearby systems only).

### B. Phase 3 — far-field points + the "active 128" for collision/labels

> **B1 substantially DONE** (near/far dot split + glare draw-call gating). The
> dot path in `render_frame()` (`src/render.c`) now classifies every body by its
> camera distance against `NEAR_DOT_DIST` (= `3.0 * LY * RS`, ~190k AU ≈ 3 ly):
> - **Near** bodies (the system you're in + nearest neighbours) keep the full
>   per-dot treatment: priority sort, greedy overlap dedup, glare-corona
>   occlusion, dot↔sphere transition fade. Set is tiny, so its O(near²) and
>   O(near×stars) costs are negligible.
> - **Far** bodies (the 16k bulk) go through one new **O(N) far pass**: a single
>   camera-relative projection (in double, so light-year precision is kept — this
>   is the "floating origin"; it was already in place via `vp_camrel`), the same
>   `system_dot_fade_for_body()` fade + far-plane clamp, emitted into the same
>   dot VBO, one `glDrawArrays(GL_POINTS)`. No O(N²) work. Far planets/moons fade
>   to zero and drop out, leaving star points (Space-Engine backdrop).
>
> Also fixed a separate **draw-call storm**: the star-glare billboard pass
> (`render_frame()` §6) issued one `glDrawElements` + 4 uniform updates **per
> star per frame** (thousands at 16k bodies). It now skips stars whose glare
> billboard is sub-pixel (`body_px[i]*STAR_GLARE_BILL_SCALE <
> STAR_DOT_FULL_GLARE_PX`) — those are already represented by their dot, so only
> the few nearby stars draw a billboard.
>
> **Net:** per-frame body work and draw calls go from O(N²)/O(N-draw-calls) to
> O(N) with a handful of draw calls. **Not yet visually verified** (no headless
> GL here) — boot the Known Universe and confirm the far starfield looks right
> and FPS is up. Trade-offs to eyeball: far stars no longer get overlap-dedup
> thinning (denser field, like `starfield.c`) and far-star coronas vanish a touch
> earlier. Tune `NEAR_DOT_DIST` if the near set ever gets big (dense clusters).
>
> **B1 leftover / B2 still TODO** below.

**Remaining in B1:** the near pass's `body_point_star_glare_visibility()` still
loops all stars per *near* dot (O(near×N)) — fine now (near is tiny) but is the
thing to fold into an instanced pass if the near set ever grows. A true GPU
instanced far-field (positions uploaded once) is **not** a clean win here: the
camera-relative subtract must stay in CPU double or distant points jitter (the
float32-cancellation note above), so the O(N) CPU far pass is the right shape.
**B2 — make collision & labels follow the camera (the real remaining work).**
The near/far split already gated the expensive per-dot work (overlap dedup,
glare occlusion) to the tiny active set, so the old O(N²)/O(dots×stars) render
terms are gone. What's left is collision and labels: they still operate on the
**first 128 body indices**, not the 128 nearest the camera, so collisions and
name labels do not follow you across the galaxy. Make `cnb()`/`nb` mean "the
≤128 bodies in active systems" via a compaction map (active body → slot 0..M-1)
computed each frame from the camera, OR have collision/labels iterate the active
system member lists from physics.
- Cleanest: expose the active set from one place. Consider moving
  `ACTIVE_RADIUS_LY` + active-system computation into `physics.c` and exposing
  `physics_system_active(slot)` / an active body iterator, then have `main.c`,
  `render.c`, `collision.c`, `labels.c` all consume it (single source of truth
  instead of recomputing distance in `main.c`).

### C. Fix origin-stacking in the importer itself  (small, do early)
**Why:** The in-app **Import real data** menu path still parks distance-less
catalogue rows at (0,0,0) — same bug `drop_unplaced()` works around in the
generator. Anyone importing a full catalog in-app hits it.
**Where:** `src/catalog.c` — `convert_exoplanets()` (and check `convert_gaia`).
When `sy_dist`/parallax is missing/≤0, **skip the row** instead of emitting a
star at the origin. (Search for where the host position defaults when distance
is NaN.) This makes the generator's `drop_unplaced()` redundant but harmless.

### D. Inactive star drift (nice-to-have)
Distant systems currently **fully freeze**. Optionally advance inactive stars
along their proper-motion velocity once per frame (`pos += vel*dt`) so the
galaxy visibly drifts when zoomed out. Cheap O(stars). Do in the frozen branch
of `main.c`'s step loop (or a dedicated pass). Planets can stay frozen relative
to their star.

### E. Deep-space dt sanity (minor)
When the camera is >2 ly from every system, no system is active so
`effective_sim_dt` is uncapped; `supernova_step/asteroids_step/rings_tick` then
advance on a large dt. Harmless so far, but if anything jumps, clamp
`effective_sim_dt` to a global max when no system is active. See `main.c` step
block.

### F. Scale up the preset  ✅ DONE
With A + B1 landed, `build_known_universe.py` now defaults to `--max-systems 0`
(everything) and the shipped "Known Universe" preset is the full ~16k-body
catalog, confirmed real-time (~30 FPS). Further scaling (a denser galaxy preset)
is gated on B2 so collisions/labels track the camera.

---

## Key references

- Body cap constant: `MAX_BODIES` in `src/common.h` (still used as the bound for
  collision/labels fixed tables and as a growth seed). Do **not** just bump it —
  collision has `[MAX_BODIES][MAX_BODIES]` **stack** arrays
  (`src/collision.c` ~line 2196) that would explode.
- Active region radius: `ACTIVE_RADIUS_LY` (live) / `WARMUP_RADIUS_LY` (boot) in
  `src/main.c`.
- Physics system tables + `physics_ensure_capacity()` + `sys_member()`:
  top of `src/physics.c`.
- Render scratch growth: `render_scratch_ensure()` in `src/render.c`.
- Known-universe generator: `tools/build_known_universe.py`.

## Build / run / test
- Build (with menu): `make clean && make IMGUI=1`  (always `make clean` when
  switching `IMGUI` on/off — Makefile keys off timestamps, not flags).
- Default build (menu stubs): `make`.
- Run: `./verse` → press `U` → pick a universe.
- No automated visual tests; verify by running and watching the boot log
  (`[Boot] Warm-up: ... across N of M nearby systems`) and frame smoothness.
- Regenerate the preset: `python3 tools/build_known_universe.py --max-systems N`.

## Known limitations right now
- Collision & labels only cover the first 128 body **indices** (not yet the 128
  nearest the camera) — see TODO B2.
- ~~An active system's gravity kernel still loops all N~~ — **fixed** (TODO A).
- ~~The render dot path is O(N²) per frame~~ — **fixed** (B1: near/far split +
  glare draw-call cull; ~3 → ~30 FPS at 16k bodies).
- Far stars skip overlap-dedup thinning and their coronas vanish a touch earlier
  — intended near/far trade-off; tune `NEAR_DOT_DIST` if needed.
- In-app importer still origin-stacks distance-less rows — see TODO C.
