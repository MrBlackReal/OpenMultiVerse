# Field-Star Scaling & Performance

Status of the compact-catalog star field (`known_universe.json` + a binary Gaia
catalog) and the render/physics work that makes ~262k stars tractable. Covers
what was done, the measured numbers, and the two open items (real-GPU frame rate
and scene brightness).

All timings below are **headless software GL (llvmpipe)**, which measures the
**CPU** side of the frame. That is what was being optimised; it does *not*
reflect real-GPU frame rate (see "Open item: frame rate").

---

## 1. What shipped

- **Compact binary star catalog** (`StarBin`, `src/catalog.h`): `catalogtool
  gaia-bin` emits a fixed-record `.bin`; a preset references it via an optional
  top-level `"star_catalog"` path; `universe.c load_star_catalog()` streams the
  stars into `g_bodies[]` at load with 0.1 ly positional de-dup against the JSON
  stars. Missing file → skipped gracefully. The `.bin` is gitignored and
  regenerated locally. `known_universe.json` uses it (266,536 Gaia DR3 stars
  within 100 pc, ~12 MB).
- **Field-star range** (`g_field_star_begin/end`, `g_universe_generation` in
  `universe.{h,c}`): the appended catalog stars form a contiguous "field" range —
  frozen scenery, drawn by a static GPU path, promoted into the per-frame dynamic
  path only when within `NEAR_DOT_DIST` of the camera (so an approached field
  star still resolves, glares, labels, and reads out in the HUD).
- **Static field-star VBO** (`render.c` + `assets/shaders/star_field.vert`):
  absolute positions + baked absolute magnitude uploaded once per universe load;
  the vertex shader does the camera-relative transform, apparent-magnitude→size,
  HDR gain, twinkle, and near/horizon cull on the GPU. One draw call, zero
  per-star CPU, zero re-upload.

## 2. The load-time and memory ceilings (earlier work, done)

- Skip trail allocation for stars (no trail buffer, no per-star GL VBO).
- Linearised the O(n²) universe post-processing, `dedupe_body_names`, and
  parent-resolution passes (hash map).
- Loader skips an orphaned planet/moon with a warning instead of `exit(1)`.

Result on the shipped preset: load 5.9 s → ~4 s and RAM 2.34 GB → ~1.2 GB while
going from 16k to 278k bodies.

## 3. The per-frame CPU ceiling (this investigation)

**Symptom:** frame rate did not improve after the first render-loop changes.

**Method:** `perf record -g` on a headless run (the CPU profile is valid even
under software GL). This is the reliable tool — earlier hypotheses (GPU sprite
cost, rebuilds firing every frame, radiance emitters) were each **measured and
ruled out** before the real cause was found.

**Root cause:** the frozen field stars stayed in `g_bodies[]`, so every
subsystem that scanned "all bodies" each frame now scanned 262k. perf named them
one tier at a time:

| Function | What it did | Fix |
|---|---|---|
| `body_is_ring_perturber_planet` (14.6% of frame) | `rings_step_system` scanned **all 262k bodies per ring disc per outer step** to find ring perturbers | skip the field range (field stars are never perturbers) — `rings.c` |
| `table_slot` + `cosmic_field_rebuild` (~9%) | re-hashing 262k **frozen** star positions every 0.25 s | throttle 0.25 s → 3 s (`cosmic_field.c`); the hash is quasi-static and still rebuilds immediately on any body-set change |

**Follow-up (2026-07): static/dynamic hash split.** The throttle above bounded the
timer path but not the *on-change* path: a single body add/remove (supernova,
promotion, collision) still forced a full 262k rehash via the `g_nbodies !=
s_built_nbodies` trigger — bursting several times/sec during stellar evolution.
`cosmic_field.c` now hashes the frozen `[g_field_star_begin, g_field_star_end)`
field stars into a **separate partition built once per universe load**
(`field_partition_ensure`, guarded by `g_universe_generation`), while the timer /
on-change **dynamic** rebuild re-hashes only the ~16k curated + promoted bodies.
`cosmic_field_sample` queries both partitions per cell. Effect: per-rebuild work
dropped ~16×, and the benchmark's near-camera 1% lows roughly **doubled** (the
every-3s rehash stutter is gone).
| `body_update_cam_proximity` (3.5%) | full-body nearest-star/-body scan every frame | skip the field range + fold in near-field stars from the cache (`body.c`) |
| `trails_render` (2.6%) | scanned all bodies (field stars have no trail) | skip the field range (`trails.c`) |
| `field_graph_rebuild` | made all 262k stars graph nodes, each running a radiance query | skip the field range (`field_graph.c`) |
| `radiance_field_rebuild` | made **all 262k stars light emitters**, so every `radiance_field_sample/_top` (per body, per comet) was O(262k) | emit non-field stars + near-field stars only (`radiance_field.c`) |

Supporting change: a **movement-gated near-system cache** in `physics.c`
(`physics_update_active_cache` / `physics_active_systems`) so the O(system-count)
active-region scan runs only when the camera moves > 0.25 ly or the body set
changes — used by `main.c` integration, `render.c`'s near-field query, labels,
and the proximity/radiance near-field lookups.

**Result (CPU, headless):**

| | ms/frame |
|---|---|
| Before | 116.6 |
| After ring-perturber fix | 72.9 |
| After all fixes | **47.8** |
| 16k no-catalog baseline | 50.6 |

The 262k field is now **effectively free per frame** — it costs the same as the
old 16k preset. Every O(262k) scan is gone from the profile; what remains is
legitimate physics/render work at the 16k level.

## 4. Bug fixed along the way: distant stars as black pixels

`star_field.vert` initially put the far-field horizon fade in the **RGB**
channel with alpha hardcoded to 1.0, so a distant star near the horizon faded to
**opaque black** (blend is `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`). The dynamic dot
path keeps the fade in **alpha** (`dot_data[...+6] = f`); the shader now matches
(`v_color = vec4(col, fade)`), so distant stars blend into the background
instead of punching black holes.

---

## Open item: frame rate (real GPU)

Reported: **~15 fps** on the real GPU with the 262k field.

The static field draw is **not** the cause — drawing the 262k sprites costs
~nothing even in software (field-draw on vs off: 6.0 vs 6.2 s over 30 frames). At
15 fps (~66 ms) the frame is **GPU-bound on something star-independent** — the
prime suspects are the **volumetric galaxy + nebula raymarch** (`galaxy.frag`,
18 steps/pixel fullscreen; `nebula.frag`) and the multi-level **bloom**
(`post.c`). Those cost the same at 16k or 262k, which is why the CPU work (real,
2.4×) did not move the frame rate — the frame was never CPU-bound on the GPU.

Also note **vsync is on by default** and 15 = 60/4 exactly, so a ~55 ms frame can
be getting quantised down to 15 fps.

**To localise (user):**
1. Does the 16k preset (or catalog removed) also run ~15 fps? If yes → fixed
   GPU cost, not the field.
2. Toggle vsync off — is the uncapped number much higher?
3. In the Visuals menu, turn off nebulae and the galaxy volume — if fps jumps,
   the volumetric raymarch is the wall.

**Likely fixes (GPU-side, star-count-independent):** lower `galaxy` `u_steps` /
run the volumetrics at half-res (a half-res path exists in `galaxy.frag`) / trim
bloom levels or resolution.

**Proposed next step:** wire `GL_TIME_ELAPSED` timer queries around each render
pass (galaxy march, nebula march, bloom, geometry) so one run prints the exact
per-pass GPU milliseconds — the GPU-side equivalent of what perf did for the CPU.

## Open item: scene brightness

The denser field looks brighter, and it also brightens frame-over-frame.

- **Exposure is fixed** (`tonemap_exposure = 0.76`, ACES) and **auto-exposure is
  OFF by default** (`auto_exposure = 0` in `settings.c`). So there is no
  automatic compensation: a 27× denser star field is genuinely brighter (more
  dots → more bloom), exactly as expected.
- The frame-over-frame ramp is **pre-existing** — the 16k preset ramps too — so
  it is not caused by the catalog or the field-star work.

The built-in "brightness correction based on star count" already exists: the
**auto-exposure** system adapts exposure to scene luminance. Options:
1. Enable auto-exposure (Visuals menu / `"auto_exposure": 1`) — the general fix;
   also tames the frame-over-frame ramp.
2. Lower the fixed `tonemap_exposure` (0.76 → ~0.5) for this preset.
3. A manual density scale on star-dot brightness — same idea, less general.

---

## Regeneration & files

- Rebuild the catalog: `make catalogtool && ./catalogtool gaia-bin <gaia.csv>
  assets/catalogs/gaia_stars.bin` (the `.csv` is a Gaia DR3 100 pc export with
  columns `source_id,ra,dec,parallax,pmra,pmdec,radial_velocity,teff`).
- Touched (this perf pass): `src/render.c`, `src/physics.{c,h}`, `src/main.c`,
  `src/rings.c`, `src/cosmic_field.c`, `src/field_graph.c`, `src/trails.c`,
  `src/body.c`, `src/radiance_field.c`, `src/labels.c`, `src/inspect.c`,
  `src/universe.{c,h}`, `assets/shaders/star_field.vert`.
- **Not yet committed.** Suggested split: (a) compact binary catalog + loader,
  (b) static field-star render path + per-frame scan fixes.
