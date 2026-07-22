# OpenMultiVerse — Unified Roadmap

## Status legend

* ✅ done
* 🟡 partial
* ⛔ todo

---

# 0. CORE ARCHITECTURAL PRINCIPLES

## 0.1 Scale-continuous universe model

**Goal:** one renderer that transitions smoothly from planetary detail to cosmological structure without mode switching.

### Core requirement

Replace discrete “planet view / solar system view / galaxy view” modes with a **continuous logarithmic scale model**.

### Required primitives

* **Logarithmic spatial transform** 🟡 *(depth transform landed)*

  * preserves precision across extreme scales
  * prevents zoom discontinuities
  * supports planetary → interstellar → galactic → cosmological transitions
  * **Status:** the logarithmic *depth* transform is unified and real. One shared
    range `RENDER_DEPTH_FAR` (`common.h`, 1e10 AU ≈ 158 kly) drives every
    depth-writing shader via a `#define DEPTH_FAR` prelude injected in
    `gl_shader_load()` and the CPU perspective far plane (`main.c`); `bh`/`torus`
    were converted from standard to log depth and `jet`/`agncore` given a log
    depth test, so all passes sort on one metric. Volumetric fades kept a
    separate `VOL_FAR`. The far field no longer pins distant stars/glare/BHs to a
    ~1500 AU shell — they render at true camera-relative depth and fade/cull at
    `g_settings.farfield_horizon_au` (live "Far-field horizon" menu slider,
    persisted). **Remaining for full #1→#2:** continuous LOD selection (below).
* **Continuous LOD selection** 🟡 *(per-body crossfades + field-driven windows landed)*

  * driven by camera distance, local density, and field variance
  * no hard scene mode toggles
  * **Status:** every per-body representation handoff is now a crossfade, not a
    pop: dot↔sphere (new `phong.frag u_opacity`, dot alpha = exact complement,
    transitioning spheres blend without depth writes), dot↔star-glare (billboard
    fades in over the same window the dot fades out), and the atmosphere glow
    fades in with the sphere instead of snapping on. The remaining linear fades
    (`system_dot_fade_for_body`, trail fade) are now smoothstep. The crossfade
    windows are scaled per frame by a **CosmicField density factor**
    (`render.c lod_update_density_scale()`: local density × clumpiness,
    log-compressed, capped ×4) — LOD driven by distance *and* the field.
    Remaining hard switches are non-visual implementation routing (near/far dot
    path at 3 ly, supernova/nebula billboard↔fullscreen raster selection).
    **Cluster/hybrid aggregation (landed — first iteration):** dense field-star
    clumps are now drawn as a single aggregate impostor glow instead of N dots.
    `cosmic_field.c cluster_extract()` coarse-bins the frozen field-star
    partition (25 ly cells, ≥24 stars → one cluster) once per universe load,
    caching a `CosmicCluster` list (centroid, RMS extent, member count, mean
    colour) at zero recurring cost. `render.c clusters_render()` projects each
    cached clump and draws one additive soft-glow point sprite
    (`cluster.frag`), sized to the clump's on-screen extent. The impostor is a
    true LOD *crossfade*, not a pop: its alpha is driven by the projected mean
    member separation, so it is full only where members are sub-pixel/culled
    (the ~16 ly dot horizon) and fades to zero as the clump resolves into
    individual stars on approach — complementary to the per-star dots/field
    points, never double-drawing. Live `g_settings.cluster_impostors` intensity
    (Menu → Visuals, persisted; 0 = off) + headless `[Clusters]` verification
    print. Verified on `known_universe` (3 clumps, richest 74 stars / 8.7 ly):
    absent among the stars, resolving into soft glows from ~1000 ly out.
    **Remaining:** dynamic-body clustering (currently field stars only) and
    richer per-cluster morphology (impostor shape from the member distribution).
* **Stable floating-origin / camera-relative transforms**

  * essential for local precision
  * must coexist with log-space projection

### Target behavior

The camera can zoom from a planet’s surface to a galaxy cluster and beyond while maintaining a coherent visual and physical transition.

### The zoom-out experience 🟡 *(landed: hold W from a planet to outside the galaxy)*

The user-facing form of this layer — "zoom out from where I stand until my
surroundings become a galaxy, then the Local Group" — now works end to end:

* **Milky Way as the home volume** — the galaxy catalogue's first entry is the
  Milky Way itself (centred 26 kly toward Sgr A*, disc axis = the real galactic
  north pole, explicit 50 kly radius, reduced inside-veil brightness that blends
  to full once the camera leaves the volume). From Earth it renders as the
  Milky Way band, brightest toward Sagittarius; from 100 kly out it is a spiral
  seen from outside — same object, no switch.
* **Adaptive warp** (`g_settings.adaptive_warp`, on by default, menu toggle) —
  in warp, speed also scales with distance to the nearest *anchor* (any alive
  body, and every galaxy: distance to its volume edge, floored inside at 2% of
  its radius). v = dist/8 → each decade of scale is a fixed ~18 s, so
  interstellar space, the galaxy rim, and Andromeda are minutes away by just
  holding W — and arrival anywhere (including at a galaxy) auto-decelerates.
* **Skybox crossfade** — the painted magnitude-catalogue sky is a
  direction-only backdrop of the stellar neighbourhood, so it fades out
  47 ly → 4.7 kly from origin (`starfield.frag u_fade`)…
* **…into procedural resolved stars** (`galaxy_stars.vert/.frag`,
  `galaxy_render_stars()`): cascaded camera-centred lattices (6 cascades,
  2 ly → 2048 ly cells) scatter stable point stars on the GPU following the
  *same* density model as the galaxy volume glow, accepted per-star against
  local emission density, luminosity power-law scaled per cascade so each
  scale contributes its brightest members. Flying toward a spiral arm resolves
  it into individual stars; the glow stays as the unresolved remainder.

* **…and stars into real systems** (`starsys.c`): come within ~1 ly of a
  procedural star and it is *promoted* to a live body with a deterministic
  planetary system — a float-exact CPU port of the shader's hash/density
  pipeline finds exactly the star the sprite showed (the sprite is
  suppressed via uniforms while the body exists), mass follows the same
  luminosity hash that brightened it, colour comes from the spectral
  pipeline, and planets spawn on circular orbits near the disc plane (rocky
  inside the snow line, giants outside, temperate worlds get atmospheres).
  **The density model lives in three synchronized ports** — `galaxy.frag`
  `galaxy_sample()` (volume glow), `galaxy_stars.vert` `star_density()`
  (sprite placement), `starsys.c` `density_cpu()` (promotion) — change one,
  change all three, or stars detach from the glow / promotions mismatch
  sprites (verified: the same star promotes at the same position and mass
  after the 2026-07 density retune).
  Fly away ≳2.6 ly and it demotes; return and the identical system
  regenerates from the same lattice seed. Promoted stars are real bodies, so
  labels, Inspect, RadianceField lighting, adaptive-warp deceleration, and
  physics all pick them up with zero special cases — verified headless
  (`[StarSys] promoted 'OMV 107.188.1938.3' (0.40 Msun, 5 planets)…`,
  planets orbiting on trails, star as dominant HUD light at 0.5 AU).

**Remaining:** richer per-cascade populations (open clusters, OB
associations along the arms), binaries/multiples, and moons + rings for
promoted giants.

---

## 0.2 Unified scene representation

Introduce a shared abstraction for all universe content:

### `CosmicField` 🟡 *(density/variance field landed)*

Everything in the engine resolves to one of three categories:

* **Discrete bodies**

  * stars, planets, moons, black holes
* **Continuous fields**

  * nebulae, gas clouds, dust lanes, gravitational potentials
* **Hybrid structures**

  * star clusters, spiral arms, accretion regions, galaxy cores

**Status:** the first iteration is implemented as `src/cosmic_field.{c,h}` — a
queryable spatial field (uniform spatial hash over `g_bodies` + nebular fill)
exposing `cosmic_field_sample(pos, radius) → {number/mass density, clumpiness,
continuous fill, dominant DISCRETE/CONTINUOUS/HYBRID class}`. This was built
*ahead of #2* precisely so continuous LOD has the "local density and field
variance" it needs. Verified via a live HUD readout and a headless
`[CosmicField]` print. **#2 LOD consumer landed:** `cosmic_field_clusters()`
now extracts dense field-star clumps as `CosmicCluster` hybrid aggregates,
consumed by the renderer's cluster-impostor pass (see §0.1). **Remaining:**
richer content tagging / hybrid structures as later presets need them.

### Why this matters

This becomes the bridge between:

* deterministic local mechanics
* field-driven visual systems
* large-scale statistical cosmology

---

## 0.3 Radiance transport abstraction

Introduce a shared lighting model:

### `RadianceField` 🟡 *(emitter field + dominant-light consumer landed)*

A unified representation for emitted, absorbed, scattered, and shifted light.

**Status:** first iteration implemented as `src/radiance_field.{c,h}` — every
light emitter reduces to one record (body, luminosity in watts, chromaticity):
thermal stars via Stefan-Boltzmann (L = L☉·(R/R☉)²·(T/T☉)⁴, T estimated from
the display colour's blue−red balance, calibrated so Sol → L☉; verified
1378 W/m² at 1 AU vs the real 1361) and black holes/AGN via accretion
(L = η·Ṁ·c² when `accretion.c` is running, else the authored Eddington ratio;
quiet disk-dressed holes floor at 1% Eddington so Gargantua lights its
planets). `radiance_field_sample(pos)` returns total incident irradiance
(W/m²) + the dominant source (direction/colour/flux). **First consumers:**
`render.c` body + atmosphere lighting now uses the *dominant emitter* instead
of walking to the parent-chain root star — so a binary companion, a nearer
foreign sun, or an accreting hole lights a body when it genuinely wins there —
plus a HUD readout (irradiance + dominant name) and a headless
`[RadianceField]` stdout print. Emitter positions are read live from
`g_bodies`; luminosities refresh on a throttle (they only drift on the stellar
clock). Main-thread only, like `CosmicField`.

### Covers

* star emission ✅ (Stefan-Boltzmann from radius + **spectral T** — physical
  mass+phase temperature from `spectral.c` §1.1, colour-estimate fallback)
* atmospheric scattering ⛔ (atm shader still local; field feeds its light dir)
* nebular glow ✅ (receivers *and* emitters — each cloud emits L ∝ area,
  referenced to an Orion-class nebula; calibrated so a cloud's interior flux
  sits just below its own receiver threshold — no self-boost feedback, and
  the receiver query skips its own contribution (`RadianceContrib.nebula`).
  A body drifting through a cloud picks up a faint coloured glow when no
  star outshines it; the HUD names body-less dominants via `dom_label`)
* accretion disks ✅ (accretion-powered luminosity incl. Eddington floor)
* supernovae ✅ (transient emitters: flash/core/cloud intensities → ~1e36 W
  peak, anchored at the detonation point, body = -1 in query results; the
  field rebuilds every tick while an event runs so the decay lights smoothly)
* galaxy light distribution 🟡 (each catalogue galaxy is an integrated
  emitter — L ∝ area at Milky-Way-class reference, named in the HUD when
  dominant; per-region light within a galaxy remains)
* relativistic color shifts ⛔ (post pass exists §1.2; not yet field-driven)

### Nebulae as light receivers (landed — Layer 4.1 "dynamic energy injection")

Each nebula samples the field at its centre per draw (`nebula.c`): incident
flux above ~1e-3 W/m² brightens the emission log-compressed (capped ×3.5) and
pulls the tint toward the source colour (`nebula.frag u_boost`/`u_boost_col`).
Catalogue starlight at interstellar range is ~1e-7 W/m², so every existing
scene renders bit-identically; an embedded star, an AGN, or a supernova going
off nearby visibly lights the cloud up. Verified headless: two 10 M☉ stars
embedded at Orion's centre raise the mean frame luminance +34% vs clean HEAD.
`--stellar-rate R` (new CLI flag) drives the stellar clock headless so
lifecycle-driven events (aging → supernova → remnant) are testable end-to-end.

### Benefits

* avoids separate “special-case shaders” for each object class
* makes lighting behavior consistent across scales
* simplifies integration of HDR, bloom, and optics

**Multi-light shading (landed):** `phong.frag`/`atm.frag` now take the field's
**top two** emitters (`radiance_field_top()`), not just the dominant one: the
secondary light gets its own soft terminator + dusk band, weighted by its
incident flux relative to the primary (`u_light2`) and tinted by its
chromaticity (`u_light2_col`) — so a planet between binary suns shows two lit
hemispheres meeting in a double dusk instead of a false night side.  Ambient
keys off "any sun up".  Secondary below 2% of primary is skipped (continuous
fade, no pop); `u_light2 = 0` is bit-identical to the single-sun path.

**Physical light chromaticity (landed):** every light's tint is now the
blackbody colour of its spectral T_eff *relative to Sol's* (`spectral.c
spectral_light_tint()`), applied to both the primary (`phong.frag`/`atm.frag`
`u_sun_col` multiplies the art-directed warm-white/dusk ramp) and the
secondary. A Sun-like star's tint is exactly white, so every Sol-lit scene is
bit-identical; an M dwarf's planets are lit warm orange (verified headless:
neutral-grey probe renders tan at an M dwarf, neutral at a Sun twin, with
physically correct irradiance 152 W/m² at 3 AU), an A star's faintly blue.

**Remaining for ✅:** per-shader adoption beyond `phong`/`atm`/`nebula`
(glare brightness — deferred deliberately: the dot↔glare handoff mirrors its
falloff formula between CPU and shader, so rebasing glare on field luminosity
needs its own careful pass), and wavelength-dependent emission curves (§1.1).
*(Spectral classification as the T source of truth: DONE — `spectral.c`
§1.1 feeds the Stefan-Boltzmann term and the light tints.)*

> **Pre-existing bug found & FIXED during this work:** headless runs went
> fully black (HUD included) after a few hundred frames in some scenes.
> Root cause: SDL's offscreen driver makes `SDL_GL_SwapWindow` a no-op, so
> nothing ever synced the GPU — at thousands of fps the driver command queue
> grew without bound until frames came back corrupted. Diagnosed by noticing
> that inserting any pipeline sync (frame readbacks, `glFinish`) made the same
> runs perfectly stable. Fix: one `glFinish()` per frame in `--headless` mode
> (`main.c`); windowed mode is untouched (vsync/swap paces the pipeline).
> Headless fps is now honest (paced), so wall-clock-driven things — label
> hysteresis, `--stellar-rate` — need fewer `--frames` than before.

---

## 0.4 Universe field graph 🟡 *(graph + event log landed)*

Introduce a simulation graph that links object classes and fields:

### Node types

* star node ✅
* planet node ✅
* black hole node ✅
* nebula field node 🟡 (counted as field nodes; no edges yet)
* galaxy field node 🟡 (counted as field nodes since Layer 4.2 landed;
  no edges yet)
* event node ✅ (256-entry ring of typed transitions)

### Edge types

* gravitational coupling ✅ (child→parent from `Body.parent`)
* radiation coupling ✅ (lazy — `field_graph_radiation_top()` wraps
  `radiance_field_top()` per query; deliberately not stored, an all-pairs
  body×emitter table is unaffordable at 16k bodies)
* gas-flow coupling ✅ (Roche donor→hole streams with live kg/s rates via the
  new `accretion_flows()`, + tidal-disruption streams)
* evolution/event transitions ✅ (notify hooks: lifecycle phase changes,
  supernova detonation→remnant incl. the star-merger path, collision merges,
  tidal consumptions — `collision.c finalize_absorb_body()` gained a `tidal`
  flag to tell the last two apart)

**Status:** first iteration implemented as `src/field_graph.{c,h}`, following
the `cosmic_field`/`radiance_field` module pattern (O(N) throttled rebuild,
main-thread only). Events snapshot participant **names** (body slots are
reused, so per-body history matches index + name and drops — never
misattributes — a previous tenant's events) and both clocks (`g_sim_time` +
stellar `age_yr`). Consumers: Inspect panel **Relations** block (orbit chain,
gas flows with rates, top incident lights, per-body history) + collapsed
**Recent events** log; headless `[FieldGraph]` stats at boot/shot time and one
stdout line per event. Verified headless end-to-end: a 10 M☉ star ages
subgiant → red giant → supernova (`event=phase`, `event=phase`,
`event=supernova`, cross-checked against the `[supernova]` stderr line), a
Roche-overflow binary shows a `flow=1` GAS_FLOW edge, and Gargantua's
"Doomed" body shows the tidal stream then `event=tde` at consumption.
**Remaining:** galaxy field nodes (Layer 4.2), nebula edges (gas-flow /
radiation into clouds), and the downstream consumers (orbit prediction,
timeline scrubbing §6.3).

> **Pre-existing bug found & FIXED during verification:** the
> `microquasar.json` demo's Roche feeding was not actually occurring — the
> donor is 0.67× the hole's mass, and the keplerian loader converted elements
> heliocentrically (GM of the parent only) while the CoM correction then gave
> the parent a compensating velocity, inflating the relative speed by (1+q):
> the authored a = 0.15 AU orbit ran at ~0.8 AU separation, outside the Roche
> lobe. Fix (`universe.c` pass 2): elements are converted with the two-body
> GM = G·(M+m) and the body takes its barycentric velocity share M/(M+m), so
> the realised *relative* orbit matches the authored elements exactly.
> Planet-mass companions are numerically unaffected (q ≲ 1e-3); the
> microquasar now feeds as designed (verified: `[FieldGraph] … flow=1` with
> the shipped preset). **Still open:** orbits authored with periapsis
> near/inside the primary's radius blow up during warmup (physics runs
> without collision there — a test body at a=0.05 AU, e=0.95 was ejected to
> ~69 AU); needs a warmup guard for deep periapsis passes if such content is
> ever authored.

### Purpose

This provides the backbone for:

* stellar evolution
* supernova events
* gravitational lensing fields
* galaxy formation
* orbit prediction
* timeline scrubbing later

---

# LAYER 0 — BASE RENDERING CORE

## 0.1 Starfield + point rendering system

**Status:** ✅

* VBO-based star dots
* temperature to color mapping
* distance-based attenuation
* faint background star dust ✅ (2026-07: `starfield.c build_background()` —
  `g_settings.bg_star_count` (default 18000, slider 0–60000, 0 = off)
  sub-catalog stars m ≈ 5–8.5, 70% weighted toward the galactic plane via an
  exponential sin-latitude falloff about the real pole so the dust thickens
  along the Milky Way band; fixed-seed xorshift RNG (never perturbs libc
  `rand()`), colours desaturated 40% toward white, packed as a 4th block in
  the skybox VBO so it shares the skybox crossfade)

---

## 0.2 HDR + post-processing pipeline

**Status:** ✅

* RGBA16F HDR buffer
* bloom pipeline
* UI-safe compositing

---

## 0.3 N-body system + camera space

**Status:** ✅

* Newtonian gravity core
* relativistic orbital corrections, partial
* camera-relative transforms

---

# LAYER 1 — PHOTON & RADIATIVE VISUAL SYSTEM

## 1.1 Stellar appearance system

**Status:** 🟡 partial — twinkle + corona + spectral classification +
physical luminosity done; starspots wired but not visibly working; emission
curves remain. Tunables under Menu → Visuals → Stars, persisted in
`settings.json`.

### Additions

* spectral classification ✅ — `src/spectral.{c,h}`: physical T_eff from mass
  (piecewise mass–temperature slope calibrated so Sol → G2V exactly;
  TRAPPIST-1 → M6V, its real L within 15%) shifted by lifecycle phase
  (subgiant IV, red giant III at ~3600 K, DA/PN/NS compact classes), with
  the old display-colour estimate as fallback for massless catalog rows.
  Shown in the Inspect panel ("Class: G2V, T_eff") and the headless
  `[RadianceField] cls=` print. *(3D name-label suffix not done — the labels
  stay name-only.)*
* wavelength-dependent emission curves ⛔
* improved luminosity model ✅ — the RadianceField's Stefan-Boltzmann term
  now runs on the spectral T (physical mass+phase) instead of guessing T
  from the art-directed colour; Sol's output is calibration-identical.
* apparent magnitude calibration ✅ (apparent-magnitude dot sizing in `render.c`,
  plus **HDR overbright**: below m≈2.5 the star's colour keeps rising past 1.0
  on a compressed magnitude scale (×6 cap) in all three point-star sources —
  body dots, the BSC5 skybox, and the procedural galaxy-star cascades — so the
  bloom pass blazes Sirius/Canopus/Vega in their own colour instead of capping
  them at an LDR dot (the SpaceEngine bright-star look). Near-star dots roll
  the gain back to 1 across the dot↔glare window, so that handoff stays
  brightness-conserving.)

### Visual upgrades

* corona shader for hot stars ✅ (`star_glare.frag` `u_corona`, additive animated
  streamers weighted by blue−red colour; `g_settings.star_corona`)
* micro-twinkle without atmosphere dependence ✅ (`star_dot.vert` `u_twinkle`,
  colour-hashed per-star phase; `g_settings.star_twinkle`)
* starspot masking for cooler stars ✅ **FIXED 2026-07** — both suspected
  causes were real: (1) `render.c` skipped stars in the sphere pass entirely
  ("glare only"), so the phong.frag emissive branch was dead code — a
  resolved star had no photosphere at all; (2) the glare billboard's
  additive wash (`shine ≈ 2.1` over the disc face) buried any surface
  detail.  Fixes: stars now take the sphere path (black holes still
  excluded), and `star_glare.frag u_resolve` clears the glow off the disc
  interior as the star resolves (0 below 30 px disc — the CPU-mirrored
  dot↔glare handoff regime stays bit-identical).  Spots re-tuned: isolated
  peaks in the active-latitude bands (butterfly zone), few-percent coverage.
  Default `starspots` 0 → 0.35.  Verified: close Sun shows limb-darkened
  photosphere + spot groups + corona outside the limb.
* rotational modulation ✅ — spots live in the body-local frame and now
  actually render, so they rotate with `u_rotation` for free.
* granulation churn ✅ (2026-07) — two counter-drifting noise fields blended
  on `u_time`, so convective cells visibly churn instead of being a static
  pattern.

---

## 1.2 Relativistic optical effects layer

**Status:** 🟡 partial — screen-space aberration + Doppler + beaming done as
post passes. Black-hole lensing lives object-local in `bh.frag`; a *global*
screen-space lens was tried and reverted (see below). Optional time-dilation
debug overlay deferred.

> **Object-local, and the right place for it:** `assets/shaders/bh.frag` is now a
> full **Schwarzschild raymarcher** — real null-geodesic bending per view ray, so
> each black hole lenses its own accretion disk (over-the-top Einstein arc),
> shows the photon ring + shadow, Doppler-beams the disk, and is view-correct 3D
> from any angle. It also lenses a faint procedural background starfield. This is
> where gravitational lensing lives; the global screen-space attempt below was a
> dead end.

### Features

* stellar aberration ✅ (screen-space, `bloom_composite.frag` `u_rel_beta` — the
  field bunches toward the heading; heading is the camera **velocity vector**
  projected to screen via `proj × view_rot` in `main.c` → `u_rel_center`, eased,
  so strafing/off-axis travel offsets the focus from the look axis)
* relativistic Doppler shift ✅ (blueshift ahead / redshift behind, same pass)
* relativistic beaming ✅ (brighten ahead / dim behind, same pass)
* gravitational lensing 🟡 — BH-local only (`bh.frag`). A global screen-space
  warp (`u_lens_*` + `gather_lenses()` + a redshift halo) was implemented and
  **reverted**: see the dead-end note below.
* optional time-dilation debug visualization ⛔ (debug overlay, deferred)

β is derived from the camera's **actual** speed (position delta / dt) in
`main.c`, ramped stylistically across the warp range (warp velocities are ≫ c so
it is not a literal v/c); strength = `g_settings.relativistic`, slider under
Menu → Visuals. Effect is only visible while moving fast.

### Notes / stylistic choices

* heading is the projected velocity vector (`u_rel_center`); strafing/off-axis
  travel offsets the aberration + Doppler focus, clamped to 0.28 UV from centre
  so it can't reach a corner

### Dead end: global screen-space gravitational lensing (reverted)

A post-pass Einstein-mapping warp around compact masses was built (CPU gathers
lenses → composite shader warps the scene + reddens near each well) and removed
because it **double-lenses the black hole's own accretion disk**. The disk is
the brightest thing on screen and sits exactly where the Einstein ring forms, so
the warp pulls the bright disk pixels into concentric rings — a "bullseye/eye"
artifact fighting the already-good `bh.frag` look. A post pass can't separate the
BH's disk from the background behind it, and a background-*only* lens of a small
BH is physically near-invisible (lensing concentrates right at the object). If
revisited, the right approach is to enhance `bh.frag`'s own disk lensing and add
a *separate* background pass that samples only the pre-BH scene, not a global
warp over the final composite.

---

## 1.3 HDR camera optics upgrade

**Status:** ✅ — bloom + filmic tonemap + exposure + lens optics. All controls
live under Menu → Visuals and persist in `settings.json`.

### Additions

* filmic tonemapping ✅ (ACES + Reinhard in `bloom_composite.frag`, selectable;
  `post_get/set_tonemap`. Default **ACES @ 0.76 exposure**; sRGB-encoded output)
* automatic exposure adaptation ✅ (`post.c auto_exposure_factor()` — averages
  scene luminance from the 1×1 scene mip, eases a factor clamped to [0.3×, 3×] of
  the manual exposure so the mostly-black void can't blow out)
* chromatic aberration ✅ (radial channel split in `bloom_composite.frag`,
  `u_chromatic`)
* lens diffraction spikes ✅ (opt-in 4-point spikes in `star_glare.frag`
  `u_spike`, driven by `g_settings.lens_spikes`; additive — leaves the
  dot/glare handoff mirror in `render.c` untouched)
* optical vignetting ✅ (post-tonemap corner falloff, `u_vignette`)
* **multi-scale bloom** ✅ (2026-07: the single half-res 10-pass chain became
  three ping-pong levels at 1/2, 1/4, 1/8 res recombined with sum-normalised
  weights 0.50/0.30/0.20 — wide smooth photographic halos, ~40% cheaper;
  1/8-res level upsampled with a 4-tap tent to kill bilinear diamonds)
* **true black point** ✅ (2026-07: clear colour was (0,0,0.02) navy — now
  pure black, plus a 0.002 display-linear black-point subtract post-tonemap
  so auto-exposure can't lift bloom spill into a grey void floor; measured
  empty-sky RGB (0,0,0), band gradients intact)
* **sun lens flare** ✅ (2026-07: `lens_flare.frag`, additive LDR overlay
  drawn after the composite — post-tonemap is where a lens artifact lives,
  and it makes the scene depth texture legally sampleable. Four chromatic
  ghost sprites mirrored through screen centre, halo ring, blue anamorphic
  streak, warm anchor glow. Fed per-frame by `render.c`: dominant
  RadianceField emitter projected to NDC (behind-camera + clip-w guards),
  intensity = `g_settings.lens_flare` (default 0.25) × a flux ramp
  saturating at Earth-like 1361 W/m². Occlusion: 3×3 scene-depth taps with
  an absolute log-depth epsilon (= constant ~5% distance-ratio tolerance, so
  the sun's own disc never self-occludes) → soft limb fade; verified gone
  when the sun sets behind a planet. Slider under Menu → Visuals; 0 skips
  the pass entirely.)

> All four lens optics default **off** (0); tonemap defaults **on** (ACES). The
> "Tonemap Off" path still reproduces the pre-1.3 linear look bit-for-bit.

---

## 1.4 Black hole & AGN engine

**Status:** ✅ — a raymarched black hole with composable, physically-sized AGN
elements (accretion disk, dust torus, relativistic jets). Verified across every
combination via headless offscreen renders (`tools/shot.sh`), ~50–105 fps. This
engine is now also **hosted inside galaxies** — `galaxy_spawn_agn()` drops an
AGN `Body` at a host galaxy's centre and these same passes draw it (§4.2).

### Core: raymarched Schwarzschild hole (`bh.vert`/`bh.frag`)

Per view ray, integrate the null geodesic (`acc = -1.5·h²·p/|p|⁵`, horizon = 1 Rs)
with adaptive stepping → view-correct 3D from any angle: event-horizon **shadow**,
grazing **photon ring** (~1.5 Rs), and the **accretion disk** lensed over the top
into the Einstein arc. Disk is animated FBM turbulence with Keplerian differential
rotation (`u_time`), opaque first-surface. Faint procedural lensed starfield for
escaped rays. (This is also the home of BH gravitational lensing — see §1.2.)

### Composable "ring-like" elements (decoupled from `is_black_hole`)

Two independent body properties, so a hole can be bare or dressed in any mix:

* **`accretion_disk`** (JSON `"disk"`, default 1 for any BH) — the raymarched disk;
  gated in `bh.frag` by `u_disk`. `"disk": 0` → a **bare hole** (shadow + ring only).
* **`dust_torus`** (JSON `"torus"`, default 1 for quasars) — a **raymarched
  volumetric dust doughnut** (`torus.vert`/`torus.frag`, new alpha-over pass) in
  the equatorial plane, Beer-Lambert absorption, glowing inner rim. Implements the
  **AGN unified model**: edge-on it obscures the core (**radio-galaxy** view),
  pole-on you see through the hole to the bright engine (**quasar** view). The
  doughnut hole aligns with the core, so no explicit depth sort is needed.

Enables: bare hole · hole+disk · quasar (disk+torus+jets) · quasar without disk ·
quasar without torus · blazar · edge-on radio-galaxy.

### AGN activity (`agn_activity`, JSON `"type":"quasar"` or `"activity"`)

* **Supercharged disk** (`bh.frag` `u_activity`) — active holes get a broader,
  hotter/bluer, HDR-bright blazing disk. `act=0` is bit-identical to a quiet hole.
* **Frame-dragging photon ring** (`u_spin`) — the limb rotating toward us is
  brighter/bluer (closest-approach position tracked for the asymmetry).
* **Relativistic jets** (`jet.vert`/`jet.frag`, additive pass) — twin collimated
  beams along the spin axis: tight base flaring outward, two-octave FBM filaments,
  **helical/braided** strands, travelling **shock knots**, a hot-white→electric-blue
  colour gradient (synchrotron cooling), per-lobe **Doppler beaming** (approaching
  lobe blazes bright+blue). Skipped when near pole-on (degenerate billboard).
* **Beamed core** (`agncore.vert`/`agncore.frag`, additive) — a camera-facing glow
  at the nucleus that lights up as the jet points at the viewer, scaled by
  activity·spin. Gives a **blazar** / jets-only pole-on view a punchy beamed core
  even with the disk off.

### Perfection pass

Photon ring crisp + per-pixel dither killing the higher-order-image aliasing in
the shadow; disk given a warm-white→gold→orange temperature ramp with a sharpened
Doppler limb asymmetry; dust torus made filamentary (two-scale noise, feathered
edge, warm inner rim, reddened shadow) and perf-tuned (16 steps + aggressive
early-out, ~50–90 fps); jets recoloured/collimated with stronger knots.

### Physics-based sizing (`render.c bh_scales()`)

Everything is expressed in Rs, and Rs itself comes from physics, so the whole
engine scales realistically with the hole's mass and spin:

* **Rs = 2GM/c²** from mass drives the render scale (not the hand-set `radius_km`).
* **Spin** a* = Ω·Rs/c (from `rotation_rate`) drives the **Kerr ISCO** (Bardeen
  1972) = the inner disk edge: 3 Rs at a*=0 down to ~0.5 Rs near-maximal
  (`u_disk_in`). A spinning hole's disk hugs the shadow (Interstellar look).
* **Jet power/length ∝ a*** (Blandford–Znajek): a non-spinning hole barely jets.
  Note: a real spinning SMBH's horizon rotates in **minutes**, so the presets use
  `rotation_period_days` ≈ 0.003, not day-scale.
* **Torus size ∝ luminosity** (mass·activity) — a more active nucleus puffs the
  torus outward.
* **Disk colour = temperature from mass + accretion** (`u_disk_temp`,
  Shakura-Sunyaev T ∝ (Ṁ/M²)^¼): a **stellar-mass** hole runs blue-white hot; a
  **supermassive** one cools to orange-red. So different-mass holes look genuinely
  different, not recoloured constants.
* **Disk swirl rate = Keplerian from mass** (`u_disk_rate`, ω ∝ 1/M
  log-compressed): a supermassive disk turns slowly, a stellar one fast — both
  visibly animated. Jet filaments/knots advect and the helix winds continuously.

### Quasar vs blazar

Same engine, different orientation — made visually distinct from the default
view: the **quasar** sits side-on (torus edge-on, vertical jets), the **blazar**
tilts its jet toward the camera (`obliquity_deg` 33) so you look down the barrel
at a blazing beamed core with a face-on torus. The look responds to viewing
angle, mass and spin rather than being a fixed image.

### Lifecycle hook

A massive star that collapses to a black-hole remnant (`supernova.c`) is born
**with an accretion disk** (`accretion_disk = 1`), so it renders as a real hole
immediately, not a bare shadow.

### Presets

`assets/universes/black_hole.json` (near-maximal-spin Gargantua), `quasar.json`
(active SMBH + torus + jets), `blazar.json` (jet toward viewer). All in `presets.c`.
`black_hole_zoo.json` is a gallery of every supported look (bare Schwarzschild,
spinning Kerr, stellar-mass hot-disk, quasar, blazar, edge-on radio-galaxy,
disk-off, torus-off), one hole per system spaced along +X.

### Disk & torus rotation

Both the accretion disk and the dust torus now visibly **orbit the spin axis**.
The disk (`bh.frag`) adds coherent trailing spiral arms sweeping at the local
Keplerian rate (`omega ∝ rdk^-1.5`) on top of the pre-existing turbulence swirl,
so orbital motion reads clearly instead of as fine-filament shimmer. The dust
torus (`torus.frag` `u_rate`, from `render.c`) previously only drifted its noise
axially — it now rotates azimuthally at its (large-radius, slow) Keplerian rate,
matching the disk's spin sense.

### Depth, lensing, and photon-ring fixes

* **Photon ring removed** (`bh.frag`) — the grazing-light ring and the bloom
  halo it produced were disabled by request; the shadow now reads as a clean
  dark disk. min_r/p_min tracking is left in place, unused.
* **True per-fragment depth** — `bh.frag` and `torus.frag` now write
  `gl_FragDepth` from the real 3D ray-hit distance (opaque hits only: horizon,
  dense disk, dense dust; faint fragments write the far plane so they don't punch
  holes in what's behind). The passes run with `glDepthMask(GL_TRUE)`. This
  replaces the old flat billboard-plane depth, so the shadow/disk/torus/jets and
  orbiting bodies sort correctly in 3D (fixes the z-fighting and "disk slices the
  hole" interpenetration).
* **Full double-arc lensing** — the disk is composited **front-to-back across
  crossings** instead of stopping opaquely at the first surface, so the
  secondary lensed image of the far disk (the arc wrapping over/under the shadow)
  is no longer occluded by the near disk. Previously the far-side arc was cut
  flat and flipped with the viewing hemisphere.

The `black_hole_zoo.json` preset holes are **scattered in 3D**, not collinear, so
flying toward one never stacks another hole behind it.

### Emergent accretion — AGN grounded in simulation (`accretion.c`)

AGN activity is no longer an authored constant; it is an **output of a physics
model**. Each black hole holds a `gas_reservoir` that drains at a viscous rate
into an accretion rate Ṁ, from which:

* **L = η·Ṁ·c²** and **Eddington ratio = L / L_edd** (L_edd = 1.26e31 W·M/M☉).
  This dimensionless ratio *is* `agn_activity` — the number that already drives
  the disk brightness, jet power, and torus size in `render.c`. So the whole AGN
  look now responds to real fuel and mass instead of a JSON tag.
* **Feedback:** the hole grows (`mass += Ṁ·dt`) and the reservoir depletes, so a
  quasar **fades and grows over cosmic time** — a bright Eddington-ratio-1 quasar
  e-folds down over the viscous time (~1 Myr) into a quiescent hole.

Runs on the **stellar clock** (`g_stellar_years_per_sec`), decoupled from the
orbital integrator, like `lifecycle.c`. A no-op at rate 0, and `agn_activity` is
**not** overwritten at seed time, so every existing preset looks bit-identical at
t=0 and only comes alive when the user advances stellar time. The authored
`"activity"` tag is reinterpreted as the *initial* Eddington ratio (it seeds the
reservoir). Verified numerically: a 4e6 M☉ quasar seeds at Ṁ ≈ 0.089 M☉/yr and
fades 1.0 → 0.08 over ~2.4 Myr with mass conserved. Inspect panel (IMGUI) shows
Eddington ratio, Ṁ, and reservoir.

**Phase 2/3 (done):**

* **Spin evolution → live ISCO.** `Body.spin_a` (dimensionless a*, signed) is
  seeded from `rotation_rate` and spun up by accretion: matter from the prograde
  ISCO carries specific angular momentum ℓ = L̃·GM/c (Bardeen-Press-Teukolsky), so
  J += ℓ·Ṁ·dt and a* → the Thorne limit (0.998). `render.c bh_scales()` now reads
  `spin_a` as the source of truth for the ISCO, so a spinning-up hole's disk edge
  visibly tightens. (Sign also drives frame-drag / disk-swirl direction.)
* **Disk temperature from real Ṁ.** `render.c` computes the Shakura-Sunyaev peak
  effective temperature T ≈ 0.488·(3GMṀ/8πσr_in³)^¼ (r_in = ISCO) and maps
  log₁₀T → the shader's blue↔red hotness. A fading quasar's disk visibly reddens
  as Ṁ drops; a stellar-mass hole runs blue-white (T ∝ (Ṁ/M²)^¼). Holes with no
  accretion data fall back to the old mass+activity proxy, so presets are
  unchanged at t=0 (calibrated to match).
* **Companion Roche-lobe feeding.** `accretion.c roche_feed()` transfers mass from
  any bound child overflowing its Eggleton Roche lobe into the hole's reservoir,
  removing it from the donor — genuine N-body coupling. A fed hole sustains its
  activity and grows instead of fading. Demo: `microquasar.json` (registered) — a
  12 M☉ hole fed by a giant donor; verified numerically (donor 5.00→4.91 M☉,
  reservoir sustained, a* climbing, mass conserved).

* **Tidal disruption events (TDE).** `collision.c bh_tidal_pass()` shreds any body
  that strays within a black hole's tidal radius (`r_t = 1.3·R·(M_bh/M_body)^⅓`):
  mass drains into the hole (growing it + fueling the disk/flare), the body shrinks
  (`r ∝ m^⅓`), and it is consumed. Because a close orbit around a supermassive hole
  is numerically unstable (relativistic perihelion, coarse steps), a shredding body
  is removed from the integrator (`tidal_frac` guards in `physics.c`'s RESPA loops)
  and collision art-directs a smooth spiral-in instead. Render: the victim draws as
  a prolate ellipsoid stretched along the line to the hole (spaghettification) with
  a hot glow (`phong.frag`/`.vert` `u_stretch_*`/`u_tidal_glow`; per-body state
  `Body.tidal_frac`/`tidal_hole`), force-rendered at a minimum on-screen size so the
  strand stays visible at black-hole-viewing distances. Demo: the "Doomed" body in
  `black_hole.json` on a plunging orbit. Tunables at the top of `collision.c`
  (`TIDAL_RADIUS_K`, `TIDAL_CONSUME_SEC`, `TIDAL_DESCENT_SEC`).

**Still deferred:** Bondi accretion from an ambient gas field (needs a gas
density field — ties into the `CosmicField` work). TDE feeding is now handled for
bodies on plunging orbits (above); a general "orbits gradually decay" drag for all
BH-bound bodies is still not implemented — real orbits are stable, so disruption
requires a plunging trajectory (as in nature).

### TODO / deferred (introduced by this work)

* **Full neutron-star / microquasar / pulsar variant** ⛔ — neutron stars take the
  sphere/glare path, not the BH shadow raymarch, so disk+jets can't attach yet.
  Needs a compact-object render path that draws a disk/jets around a *surface*
  (no horizon shadow). The `disk`/`torus`/jet machinery is ready to reuse.
* **Jet termination lobes / gas interaction** ⛔ — jets don't yet inflate radio
  lobes or interact with surrounding gas.
* **Background lensing uses procedural stars** ⛔ — the real starfield is GL points
  drawn before the BH into the same FBO, so it isn't a sampleable texture at
  BH-draw time. A true lensed background needs a star cubemap rendered up front.
* **Dead-pole-on blazar seam** 🟡 — a faint rectangular discontinuity appears in
  the torus dust only when looking almost exactly down the axis (not the jets —
  survives skipping them; not the billboard edge — survives widening it). Every
  other angle (3/4, edge-on, off-axis blazar, close-up) is clean. Source still
  unidentified; low priority (one extreme viewpoint).

---

# LAYER 2 — STELLAR & SYSTEM EVOLUTION VISUALIZATION

## 2.1 Stellar lifecycle system

**Status:** ✅ (core) — `src/lifecycle.{c,h}`

### Lifecycle states

* ~~protostar~~ (not modelled)
* main sequence ✅
* subgiant ✅
* red giant ✅
* white dwarf ✅ (low-mass death)
* neutron star ✅ (high-mass death)
* black hole ✅ (high-mass death)

### Event hook

* death routes through `supernova_detonate()`; phase changes via
  `lifecycle_advance_phase()` / `lifecycle_trigger_death()`.

### Visual mapping

* radius evolution ✅ (per-phase scale off captured main-sequence appearance)
* color-temperature shift ✅
* mass loss particles ⛔ (reuses the supernova/nebula blast, no dedicated wind)
* supernova transition ✅

Driven from the Inspect panel, or continuously via the **Stellar time** slider
(`g_stellar_years_per_sec`, default 0 = manual). The orbital integrator is never
sped up — evolution runs on its own clock.

---

## 2.2 Supernova and remnant system

**Status:** ✅ — `src/supernova.c`

### Features

* shockwave expansion ✅ (one-shot mass-scaled shock kick to nearby subtrees)
* ejecta particle shell ✅ (volumetric cloud raymarch, rendered half-res)
* transient nebula formation ✅ (the cloud itself; low-mass = soft nebula puff)
* remnant replacement ✅ (black hole / neutron star / white dwarf by mass)
* auto-inspect remnant mode ✅ (death refocuses the orbit camera on the remnant)

Galaxy-scale safe: blast reach is bounded by mass and gathered into a per-event
candidate list; subtree ops use a cached child index, so the path is O(N) not
O(N²). See ARCHITECTURE.md §8.1.

---

## 2.3 Comets and minor bodies

**Status:** 🟡 — coma + both tails + physical sublimation landed (2026-07);
fragmentation remains

A comet is a normal body (`"type": "comet"` → `Body.is_comet`; the nucleus
takes the ordinary dot/sphere/physics/label path).  `src/comet.{c,h}` +
`comet.vert/.frag` draw the volatile display: three additive elements per
comet, camera-relative floats, per-fragment log depth (the ribbons span a
large depth range).  Tails draw as crossed ribbon pairs so no viewing angle
sees a flat quad silhouette.

### Features

* ion tail ✅ — straight, dead anti-sunward from the *dominant RadianceField
  emitter* (a comet rounding a foreign sun points away from THAT sun), blue,
  narrow, filamentary streaks advecting tailward on `u_time`
* dust tail ✅ — **physically simulated syndynes**: each sample is a real
  grain, back-propagated to its release point on the comet's orbit and then
  forward-propagated under radiation-pressure-reduced gravity μ(1−β)
  (two-body Kepler both ways — bracketed Newton, survives e ≈ 0.97 at
  perihelion where naive Newton scatters; unbound high-β grains take a
  ballistic fallback).  Grains keep orbiting at nearly the comet's speed,
  so the tail points mostly anti-sunward and curves toward the orbit with
  age — it is NOT a contrail marking the path (the first implementation
  froze grains at their release points; watching it move made the error
  obvious).  Seven β curves (0.09–0.80, the grain-size spectrum) drawn as
  one ruled surface = a continuous McNaught-style curved fan; syndynes are
  terminated by grain–nucleus separation, not age (orbital shear near
  perihelion separates grains far faster than the differential-acceleration
  estimate).  Verified: boot + 30-day shots show the fan sweeping with the
  orbit, ion tail staying sun-locked through it.
* sublimation intensity ✅ — PHYSICAL: activity ramps with incident flux at
  the nucleus (log-scaled ~30 W/m² → 1361 W/m², i.e. ~6.7 AU → 1 AU around a
  Sol-class star, capped ×1.5 inside).  Tail length/brightness/coma all
  scale with it — no authored keyframes.  Verified: the demo comet blazes at
  q = 0.586 AU; Halley in the default universe sits quiet at ~26 AU.
* tidal fragmentation ⛔ (deferred — collision.c's tidal machinery is the
  natural host when this is picked up)

**Content:** 1P/Halley added to the default universe (real J2000 retrograde
elements, currently ~26 AU out — frozen nucleus that will light up on
approach).  New preset "Comet at Perihelion" (`comet.json`, registered):
orbit authored so the 2-year warm-up lands exactly at perihelion — the app
opens on the full display.

---

## 2.4 Binary and multi-star systems

**Status:** 🟡 partial — *data support only.* Multi-star systems load and
integrate correctly (see presets), but none of the visualization additions
below exist in source.

### Additions

* barycenter toggle ⛔
* orbital resonance rendering ⛔
* Lagrange point overlays ⛔
* Trojan point highlighting ⛔

---

# LAYER 3 — PLANETARY RENDERING SYSTEM

## 3.1 Atmospheres

**Status:** 🟡 partial — **physical single scattering landed** (2026-07)

### Current

* limb glow ✅ (now emergent from the scattering integral, not art-directed)
* sunset tint ✅ (emergent: sun transmittance through the shell reddens the
  terminator; the backlit planet shows the refracted-sunset ring)
* forward scattering ✅ (Henyey-Greenstein Mie, g = 0.76)
* cloud layer ✅ (Earth, `phong.frag` L584)
* city lights ✅ (Earth night side, `phong.frag` L883)

### Additions

* Rayleigh scattering ✅ — `atm.frag` rewritten as a real single-scattering
  march: 14 view samples through the shell, each with a 4-sample secondary
  march to the sun for per-channel transmittance (correct at the terminator
  for any planet/shell ratio). β_R = 3.2 · (normalised JSON atmosphere
  colour)² — the spectrum is *derived* from the authored colour, not a blue
  constant tinted by it: Earth's blue² reproduces physical Rayleigh, Mars
  actually scatters red (blue terminator, as on real Mars), Titan is orange
  haze (verified: 4-planet frozen rig, same illumination, four clearly
  distinct shells). `u_atm_intensity` stays the gain. Path lengths in shell
  units so one
  constant set serves every planet; thin shells (gas giants) scale β by
  min(1, 3.3/(R/H)) so the limb never blows out (verified on Jupiter).
  Both RadianceField lights get the full integral (`u_light2` = 0 is
  bit-identical single-sun). Carrier/discards/log-depth kept verbatim; the
  blend now outputs radiance at alpha 1 (was alpha-squared). Verified
  headless on a frozen Earth-clone rig: blue limb, terminator shadow,
  backlit sunset ring.
* Mie scattering ✅ (grey β_M = 0.25, HG phase, half the Rayleigh scale height)
* dynamic cloud layer 🟡 (2026-07: generalized from static Earth-only to
  **every solid world with an atmosphere** — `phong.frag u_cloud_amount` is
  data-driven from the body's authored atmosphere intensity (Earth half
  covered, Mars translucent CO2 wisps, promoted temperate exoplanets get
  theirs automatically; gas giants/Venus/Titan excluded — their recipes ARE
  cloud tops).  Coverage threshold + opacity scale with the amount, decks
  drift at their own rotation rate, and clouds now **cast shadows** on the
  surface (deck resampled toward the sun's tangential offset).  Remaining
  for ✅: time-evolving pattern, not just drift)
* atmospheric density gradients 🟡 (exponential ρ(h) in the march; no
  per-planet scale-height authoring yet)
* aurora system ✅ (2026-07: emission integrated INTO the atmosphere march —
  `aurora_emission()` in `atm.frag` accumulates per view sample, attenuated by
  the air in front of it, so curtains occlude/frame for free.  Oval = gaussian
  ring of magnetic latitude around the spin pole; curtains = 3-octave
  longitude value-noise drifting on `u_time`; altitude bands violet fringe →
  green body → red top (red gain kept low — oblique rays integrate the tall
  red column far longer than the thin green layer, and a photo-accurate red
  turned the whole oval pink).  Rays that hit the planet face take an
  emission-only march shell-entry → surface (depth from shell entry so the
  sphere's depth test passes) — that's what makes the classic orbital view of
  the oval against the night-side disc work; scattering pixels are untouched.
  Strength is a PHYSICAL proxy, no art tags: dynamo ∝ |rotation_rate| × mass
  (Earth-normalised, clamped) × √(dominant RadianceField flux / 1361) —
  Earth ≈ 1, Venus' 243-day spin ≈ 0, tidally-locked moons ≈ 0, promoted
  exoplanets inherit automatically; same philosophy as cloud decks and comet
  activity.  NOT always on: `aurora_storm()` (render.c) modulates it 0.22–2.5×
  with multi-octave noise over the SIM clock (substorm ~40 min / storm ~5 h /
  sector ~22 h), seeded per emitting star and evaluated at the wind's arrival
  time (t − dist/450 km/s — a gust sweeps outward, Earth flares days before
  Jupiter).  Cubed noise ⇒ quiet oval ~half the time, real storms ~8%; storms
  also push the oval equatorward and thicken it (shader derives centre/width
  from strength).  τ≈1.2 s real-clock low-pass so high sim rates breathe
  instead of strobing.  Verified headless on the frozen Earth-clone rig:
  polar view = green curtain fan with red tops, limb view = oval edge-on
  through the shell, day side buried by sunlight; storm distribution checked
  offline (median 0.5, p95 1.35); default-universe boot unchanged.  All
  look/storm parameters (gain, oval lat/width, storm expansion, band colours,
  storm floor/amplitude/timescale/smoothing) are live Visuals-menu settings
  persisted in settings.json, as are the lens-flare element gains
  (ghosts/halo/halo radius/streak/streak length/core).)
* lightning emission ⛔

---

## 3.2 Planet surface materials system

**Status:** 🟡 partial — `phong.frag` has a `u_planet_type` material system
covering most classes below; only metallic worlds and methane seas remain.

### Material classes

* lava ✅ (`lava_color()`, molten/crust/white-hot; `phong.frag` L89+)
* ice ✅ (ice giants type 12; polar caps on Earth/type 2)
* desert ✅ (Earth land/desert recipe, type 1)
* ocean ✅ (Earth ocean recipe, type 1 — **+ specular sun glint 2026-07**:
  sharp HDR Blinn lobe + Fresnel sheen on open water (no land/ice/cloud),
  both RadianceField lights; the bloom pass blazes the sun's reflection)
* metallic worlds ⛔
* methane seas ⛔

### Surface relief (landed 2026-07)

Solid worlds now catch light: fBM terrain normals (`terrain_height()` /
`terrain_normal()`, keyed on the same fbm(NL·3.5) as the colour recipes so
relief follows the painted landforms — ridged mountain chains + fine
roughness), per-type strength (Earth land-only so oceans stay glassy for the
glint, Mars/rocky strongest, Europa subtle cracks; gas giants/Venus/Titan
none), faded out beyond ~10–45 body radii before it can alias.  Earth gets
altitude/latitude mountain snow.  Airless bodies keep the existing dedicated
crater model (`moon_height`/`moon_normal`).

---

## 3.3 Planet rings

**Status:** 🟡 partial — **ring lighting landed 2026-07**

### Additions

* self-shadowing ✅ — both directions: the planet's shadow cylinder darkens
  the ring sector behind the globe (`ring.vert u_shadow_strength`, existed),
  and NEW: the ring annulus stripes its shadow across the globe —
  `phong.frag u_ring`/`u_ring_pole` (fed by the new `rings_query()` API)
  casts the sun ray from each surface point through the ring plane and
  attenuates direct light with a banded profile.  Verified on Saturn: broad
  banded shadow across the northern hemisphere with the sun 24° below the
  ring plane — the signature seasonal look.
* forward scattering ✅ — backlit ring dust blazes (`ring.vert`: forward
  phase lobe on view·sun, respecting the planet shadow).  Verified: Cassini
  style bright ring annulus around the dark night-side globe.
* density waves ⛔
* shepherd moon perturbations ⛔
* particle size variance ⛔

---

## 3.4 Body–body eclipse shadows

**Status:** ✅ core (landed 2026-07)

Analytic soft sphere shadows in `phong.frag` (`u_ecl[6]`/`u_sun_radius`):
each drawn sphere tests its **family** occluders (parent, children, siblings
— the only physically plausible eclipsers; gathered per frame in `render.c`,
O(N) int compares) against the primary sun.  Coverage compares the
occluder's angular radius vs the sun disc's vs their separation — a real
penumbra ramp that caps at the disc-area ratio, so small moons cast soft
annular-transit spots while a planet fully extinguishes a moon inside its
umbra.  One test gives both directions: moon transit shadows striping the
planet AND moons going dark in the planet's shadow.  Verified on a frozen
3-moon rig (warmup_years=0 for exact authored geometry): sunward moon casts
a ~92%-deep soft spot at the subsolar point (A/B pixel-diff confirmed),
anti-sunward moon renders fully dark in the umbra, 90°-control moon stays
half-lit.  Primary light only (a secondary-emitter eclipse is a vanishing
corner case); lens-flare occlusion already agrees via the depth buffer.

---

# LAYER 4 — COSMIC STRUCTURE SYSTEM

## 4.1 Nebula system

**Status:** ✅ near complete

### Additions

* velocity field evolution
* star formation regions
* dynamic energy injection from nearby stars
* multi-frequency emission bands

---

## 4.2 Galaxy system

**Status:** 🟡 *(catalogue galaxies as volumetric structures landed)*

### Types

* spiral ✅ (exponential disc + bulge + two log-spiral arms + FBM knots)
* elliptical ✅ (steep-cored smooth glow, old warm population)
* irregular ✅ (torn clumpy cloud: ragged noise-warped outline, off-centre
  warm stellar bar, pink HII complexes, dark dust patches — LMC/SMC read as
  structured Magellanic Clouds close-up and from Earth, not cotton balls)
* active galactic nuclei ✅ — the §1.4 engine is now **hosted inside galaxies**.
  `GalaxyDef` carries an optional central-SMBH mass + Eddington ratio + torus;
  `galaxy_spawn_agn()` (called from `init_runtime_world` after `galaxy_init`)
  drops a black-hole `Body` at each host's photometric centre, so the existing
  four AGN passes draw its accretion disk + jets **unchanged** — a pure data-
  level composition, no render-pass edits. Hosts: **M87** + **Centaurus A**
  active (jets + dust torus), **Sgr A\*** + **Andromeda** quiescent (bare shadow
  + faint disk). Everything is Rs-scaled from mass via `bh_scales()`, so the
  nucleus is a physically-sized point you **fly to** (culled past
  `farfield_horizon_au` from afar): approach a galactic centre and its disk +
  relativistic jets bloom inside the surrounding galaxy glow. Gated by the
  `galaxy_agn` setting (default on). *Deferred:* an artistic galaxy-scale jet
  (a `u_visual_scale` multiplier + full 3-D jet axis) for the kpc "beam across
  the galaxy" poster — a follow-up on this proven base, since it would touch the
  four AGN render passes.

### Visual components

* dust lanes ✅ (pure absorption riding the arms' inner edges, slightly below
  the midplane, and extinguishing embedded starlight — edge-on discs get the
  dark stripe across the bulge, verified on the Sombrero. Plus a ragged
  equatorial **Great Rift** sheet independent of arm phase: seen from inside,
  the Milky Way band is split lengthwise by the dark lane, with star-cloud
  mottling and a golden bulge glow toward Sagittarius. Dust extinction scales
  with √density so the inside-veil dimming — which is artistic, emission-only
  — cannot wash the rift out)
* star density gradients ✅ (disc/bulge/arm population mix: warm bulge, cool
  blue arms, pink HII knots)
* rotational shear ✅ (flat rotation curve, ω ∝ 1/r on `u_time`; noise is
  sampled in the co-rotating frame so clumps ride the shear)
* central black hole ✅ (the bulge core glows, and host galaxies now carry a
  real embedded AGN engine — a black-hole `Body` at the photometric centre; see
  the "active galactic nuclei" bullet above)

**Status detail:** first iteration implemented as `src/galaxy.{c,h}` +
`assets/shaders/galaxy.frag`, mirroring the nebula architecture: 10 real
Local Group / nearby galaxies (SIMBAD/NED positions, distances, sizes,
**inclinations** — the disc axis is tilted off the Earth sightline by the
catalogued inclination, so Andromeda leans and the Sombrero is edge-on),
one volumetric raymarch representation from a few-pixel backdrop to a
fly-through (billboard→fullscreen carrier is nebula.vert, reused; same
angular-size-preserving 1400 AU clamp shell), premultiplied "over" at log
depth, drawn before the nebulae so translucent overlaps sort back-to-front.
Consumers: Navigate → "Galaxies (fly to)", RadianceField integrated emitters
(§0.3 "galaxy light distribution" first step: L ∝ area at Milky-Way-class
reference — the HUD names "Andromeda (M31)" as the dominant light when you
fly out there), field-graph galaxy nodes (`[FieldGraph] … galaxies=10`).
Verified headless: Magellanic Clouds + Tarantula neighbourhood shot, M31
tilted disc, M51 face-on spiral with arms, M104 edge-on dust stripe.
The raymarch renders to the shared **half-res target** (the Milky Way volume
makes it fullscreen in every in-galaxy scene) and composites back
depth-correctly: `post.c` exposes the scene depth as a texture
(`post_scene_depth_tex()`) and `galaxy.frag` clips its march to scene depth,
so planets embed properly in the glow — better than the old full-res path,
which blended the whole band over them. Full-res depth-tested fallback when
bloom/post is off.
**Remaining:** per-region galaxy light (the single integrated emitter is a
point approximation), authored galaxies in universe JSON, and the deferred
galaxy-scale artistic AGN jet (see the AGN bullet). *(AGN hosted inside a
galaxy — landed; see the "active galactic nuclei" bullet.)*

---

# LAYER 5 — GENERAL RELATIVITY VISUALIZATION LAYER

## 5.1 Gravitational field visualization

**Status:** ⛔ todo

### Modes

* spacetime grid deformation
* field lines
* equipotential surfaces
* acceleration vectors

---

## 5.2 Gravitational waves

**Status:** ⛔ todo

### Effects

* ripple distortion field
* transient lensing oscillation
* background wobble
* intensity modulation sweep

---

## 5.3 Lagrange / orbital mechanics overlay

**Status:** ⛔ todo

### Features

* L1–L5 visualization
* Hill sphere rendering
* Roche limit boundaries
* stable orbit heatmaps

---

## 5.4 Orbit prediction system

**Status:** ⛔ todo

### Features

* future trajectory ghost lines
* decay / escape prediction
* resonance detection visualization

---

# LAYER 6 — CAMERA & CINEMATIC SYSTEM

## 6.1 Cinematic camera mode

**Status:** ⛔ todo

### Features

* automated orbit paths
* spline-based motion
* focus tracking
* depth of field
* slow zoom / dolly shots

---

## 6.2 Recording and presentation mode

**Status:** ⛔ todo

### Features

* UI suppression
* highlight auto-events
* title overlays
* snapshot keyframes

---

## 6.3 Universe timeline system

**Status:** ⛔ todo

### Features

* reversible simulation state
* time scrubbing UI
* event timeline markers

### Markers

* collisions
* supernovae
* mergers
* instability events

---

# LAYER 7 — EDUCATIONAL / DEBUG VISUALIZATION MODE

## 7.1 Physics overlay system

**Status:** ⛔ todo

### Toggles

* velocity vectors
* acceleration vectors
* gravitational force vectors
* momentum indicators

---

## 7.2 System analysis tools

**Status:** ⛔ todo

* barycenter visualization
* orbital resonance markers
* energy conservation display
* stability heatmaps

---

# PRIORITY STRUCTURE

## Phase A — Foundation for scale continuity

1. Logarithmic spatial transform 🟡 (depth transform + true-depth far field done)
3. Unified `CosmicField` abstraction ✅ (density/variance field landed — built ahead of #2)
2. Continuous LOD system 🟡 (per-body crossfades + CosmicField-driven windows +
   cluster/hybrid aggregation landed; dynamic-body clustering + richer impostor
   morphology remain)
4. Shared `RadianceField` abstraction 🟡 (emitter field, multi-light shading,
   spectral T + physical chromaticity, nebulae as receivers *and* emitters
   all landed; galaxy light distribution, glare adoption and relativistic
   shifts remain)
5. Universe field graph 🟡 (graph + edges + event log + Inspect/headless
   consumers landed; galaxy field nodes and the orbit-prediction/timeline
   consumers remain)

*(Note: #3 was built before #2 — continuous LOD needs a density/variance field, which #3 provides.)*

**Phase A is now foundation-complete** — every abstraction exists and is
consumed by at least one real system; what remains in each is expansion
(cluster aggregation, spectral T, galaxy nodes), not groundwork.

## Phase B — Visual realism completion

6. Relativistic optical effects 🟡 (aberration/Doppler/beaming done; global lensing a dead end)
7. Filmic camera and HDR optics ✅ (+ multi-scale bloom, true black point, lens flare 2026-07)
8. Improved atmospheres 🟡 (physical Rayleigh+Mie single scattering landed 2026-07;
   auroras landed 2026-07; dynamic-cloud evolution / lightning remain)
9. Stellar lifecycle system ✅

## Phase C — Universe dynamism

10. Supernova and remnant system ✅
11. Comets and minor bodies 🟡 (coma + ion/dust tails + physical sublimation
    landed 2026-07; tidal fragmentation remains)
12. Binary / multi-star overlays
13. Orbit prediction visuals

## Phase D — Cosmic expansion

14. Galaxy system 🟡 (catalogue galaxies as volumetric spiral/elliptical/
    irregular structures with dust lanes + shear; AGN-in-host and per-region
    light remain)
15. Gravitational field visualization
16. Gravitational waves

## Phase E — Experience layer

17. Cinematic camera system
18. Universe timeline system
19. Physics overlay debug layer

## Phase F — Signature differentiator

20. Universe comparison mode

---

# FINAL SYSTEM SUMMARY

## What the engine is

A **logarithmic, scale-continuous cosmological renderer** with:

* unified spatial abstraction
* unified radiance transport
* field-based representation across scales
* relativistic camera optics
* deterministic local simulation
* statistical large-scale structure

## What it is not

* not a collection of disconnected view modes
* not a fixed-scale space game renderer
* not a pure object-centric scene graph

## Target behavior

The user should be able to:

* inspect a planet
* zoom to its system
* zoom to neighboring systems
* zoom to a galaxy
* zoom to a local group
* continue outward
* never encounter a hard mode switch
* always remain in one continuous universe
