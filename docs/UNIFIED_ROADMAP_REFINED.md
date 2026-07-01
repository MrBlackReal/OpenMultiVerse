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
    **Remaining for ✅:** cluster/hybrid aggregation (drawing a dense clump as
    an aggregate impostor instead of N dots) once galaxy-scale presets need it.
* **Stable floating-origin / camera-relative transforms**

  * essential for local precision
  * must coexist with log-space projection

### Target behavior

The camera can zoom from a planet’s surface to a galaxy cluster and beyond while maintaining a coherent visual and physical transition.

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
`[CosmicField]` print. **Remaining:** #2 LOD consumer; and richer content
tagging / hybrid structures as later presets need them.

### Why this matters

This becomes the bridge between:

* deterministic local mechanics
* field-driven visual systems
* large-scale statistical cosmology

---

## 0.3 Radiance transport abstraction

Introduce a shared lighting model:

### `RadianceField`

A unified representation for emitted, absorbed, scattered, and shifted light.

### Covers

* star emission
* atmospheric scattering
* nebular glow
* accretion disks
* galaxy light distribution
* relativistic color shifts

### Benefits

* avoids separate “special-case shaders” for each object class
* makes lighting behavior consistent across scales
* simplifies integration of HDR, bloom, and optics

---

## 0.4 Universe field graph

Introduce a simulation graph that links object classes and fields:

### Node types

* star node
* planet node
* black hole node
* nebula field node
* galaxy field node
* event node

### Edge types

* gravitational coupling
* radiation coupling
* gas-flow coupling
* evolution/event transitions

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

**Status:** 🟡 partial — twinkle + corona done; starspots wired but not visibly
working; data-model additions remain. Tunables under Menu → Visuals → Stars,
persisted in `settings.json`.

### Additions

* spectral classification labels ⛔
* wavelength-dependent emission curves ⛔
* improved luminosity model ⛔
* apparent magnitude calibration ✅ (apparent-magnitude dot sizing in `render.c`)

### Visual upgrades

* corona shader for hot stars ✅ (`star_glare.frag` `u_corona`, additive animated
  streamers weighted by blue−red colour; `g_settings.star_corona`)
* micro-twinkle without atmosphere dependence ✅ (`star_dot.vert` `u_twinkle`,
  colour-hashed per-star phase; `g_settings.star_twinkle`)
* starspot masking for cooler stars ⛔ **NOT WORKING** — code in place
  (`phong.frag` emissive branch granulation + temperature-scaled spots,
  `u_starspots`/`g_settings.starspots`, wired in `render.c`) but spots are not
  visible in-app even at strength 1.0 on a red giant. Default forced to 0.
  Suspected causes to chase: bloom/tonemap washing out the darkening on the
  bright emissive disc, or close stars not taking the sphere-disc path. The
  menu slider and plumbing are kept for the fix.
* rotational modulation ⛔ — mechanism implemented (spots live in the body-local
  frame so they'd rotate with `u_rotation`) but unobservable until starspots
  render.

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

> All four lens optics default **off** (0); tonemap defaults **on** (ACES). The
> "Tonemap Off" path still reproduces the pre-1.3 linear look bit-for-bit.

---

## 1.4 Black hole & AGN engine

**Status:** ✅ — a raymarched black hole with composable, physically-sized AGN
elements (accretion disk, dust torus, relativistic jets). Verified across every
combination via headless offscreen renders (`tools/shot.sh`), ~50–105 fps.

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

**Status:** ⛔ todo

### Features

* ion tail
* dust tail
* sublimation intensity
* tidal fragmentation

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

**Status:** 🟡 partial

### Current

* limb glow ✅
* sunset tint ✅
* forward scattering ✅
* cloud layer ✅ (Earth, `phong.frag` L584)
* city lights ✅ (Earth night side, `phong.frag` L883)

### Additions

* Rayleigh scattering ⛔ (no named Rayleigh term)
* Mie scattering ⛔
* dynamic cloud layer ⛔ (current cloud layer is static, Earth-only)
* atmospheric density gradients ⛔
* aurora system ⛔
* lightning emission ⛔

---

## 3.2 Planet surface materials system

**Status:** 🟡 partial — `phong.frag` has a `u_planet_type` material system
covering most classes below; only metallic worlds and methane seas remain.

### Material classes

* lava ✅ (`lava_color()`, molten/crust/white-hot; `phong.frag` L89+)
* ice ✅ (ice giants type 12; polar caps on Earth/type 2)
* desert ✅ (Earth land/desert recipe, type 1)
* ocean ✅ (Earth ocean recipe, type 1)
* metallic worlds ⛔
* methane seas ⛔

---

## 3.3 Planet rings

**Status:** 🟡 partial

### Additions

* self-shadowing
* forward scattering
* density waves
* shepherd moon perturbations
* particle size variance

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

**Status:** ⛔ todo

### Types

* spiral
* elliptical
* irregular
* active galactic nuclei — *the AGN central engine already exists* (§1.4:
  raymarched hole + accretion disk + dust torus + relativistic jets); what
  remains here is the surrounding galaxy (stellar disk, dust lanes) to host it.

### Visual components

* dust lanes
* star density gradients
* rotational shear
* central black hole glow

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
2. Continuous LOD system 🟡 (per-body crossfades + CosmicField-driven windows landed;
   cluster/hybrid aggregation remains for dense galaxy presets)
4. Shared `RadianceField` abstraction ← next foundation step
5. Universe field graph

*(Note: #3 was built before #2 — continuous LOD needs a density/variance field, which #3 provides.)*

## Phase B — Visual realism completion

6. Relativistic optical effects 🟡 (aberration/Doppler/beaming done; global lensing a dead end)
7. Filmic camera and HDR optics ✅
8. Improved atmospheres
9. Stellar lifecycle system ✅

## Phase C — Universe dynamism

10. Supernova and remnant system ✅
11. Comets and minor bodies
12. Binary / multi-star overlays
13. Orbit prediction visuals

## Phase D — Cosmic expansion

14. Galaxy system
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
