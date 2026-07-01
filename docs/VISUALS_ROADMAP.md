# Visual upgrade roadmap

Sequenced visual improvements for the galaxy-scale view. Each lands and is
verified on its own before the next starts (the app can't be eyeball-checked
headless, so every step needs a quick look in the running build).

Status legend: ✅ done · 🟡 in progress · ⛔ todo

---

## 1. Star size & brightness by type ✅
**Goal:** the 16k-star field should read like real sky — bright/near/luminous
stars pop as larger, brighter points; faint ones recede. Today every dot is a
flat 2.5 px regardless of the star.
**Approach (no new render pass):** stars already carry a blackbody colour from
effective temperature (`teff_color` at import). Add a per-point **size**
attribute to the dot VBO and a `star_dot.vert` that sets `gl_PointSize` from it.
Size is derived on the CPU from an apparent-magnitude estimate
(luminosity proxy `(R/R☉)²` ÷ distance²) and clamped. The background
`starfield.c` is untouched (separate shader program).
**Files:** `assets/shaders/star_dot.vert` (new), `src/render.c` (dot VBO layout
7→8 floats, both emit passes, draw with `GL_PROGRAM_POINT_SIZE`).
**Risk:** low-moderate (VBO stride change). No new framebuffer.

## 2. Star glow / bloom (HDR) ✅
**Goal:** bright stars bleed light; the field glows instead of looking like flat
pixels — the biggest single "wow".
**Done:** new `src/post.{c,h}` module renders the scene into a full-res RGBA16F
target (`post_begin()` before `render_frame`, `post_end()` after), then runs a
bright-pass → half-res separable Gaussian (10-pass ping-pong) → additive
composite to the screen. Additive star glare already exceeds 1.0 in the HDR
buffer, so luminous things bloom. Targets rebuild on window resize. Shaders:
`post_quad.vert`, `bloom_bright/blur/composite.frag`. Menu → Universe →
**Visuals** exposes a Bloom toggle + threshold/intensity sliders. Wired in
`main.c` (init + render wrap); UI/menu draw after the composite so they don't
bloom. **Needs visual verification + tuning of the default threshold (0.80) /
intensity (1.10).**

## 3. Planet & atmosphere detail ✅
**Atmospheres done** (`atm.frag`, shader-only): added a scattering model on top
of the existing limb glow — a brighter rim line, a warm **sunset** tint through
the twilight band at the terminator, soft day/night falloff, and a **forward-
scatter halo** so a planet backlit by its star gets a bright glowing ring (which
also blooms nicely with feature #2). No C/uniform changes.
**Surface day/night done** (`phong.frag`, shader-only): the final lighting was a
flat Lambertian (`ambient + (1-ambient)·diff`) — a razor terminator and a grey,
washed-out night side. Replaced with a softened terminator (`smoothstep` light-
wrap), a warm **sunset reddening** band riding the dusk line, and a faint **cool
earthshine** night ambient instead of flat grey, so shadowed hemispheres read as
shadowed. Applies to every body; stars (emissive) and lava/city emission are
unaffected. Night-side emissive (Earth city lights, hot-world lava glow) and
per-type FBM surface variation were already present.
**Visual check (verified):** terminator/night look confirmed good across planet
types in-build.
**Risk:** low — contained shader work, no pipeline change.

## 4. Black holes / compact objects ✅
**Done:** new `is_black_hole` body flag (`body.h`). The loader (`universe.c`)
accepts `"type": "black_hole"` in the star pass — placed/grouped like a star
(massive system root that bodies orbit) but flagged for special rendering;
saved/loaded in snapshots. A dedicated billboard pass (`render.c`, shaders
`bh.vert`/`bh.frag`). Black holes are excluded from the normal sphere/glare/dot
paths. New **"Black Hole"** preset (`assets/universes/black_hole.json`,
registered in `presets.c`): a ~4e6 M☉ hole with bodies on tight
relativistic-speed orbits.
**Visual check (verified):** evolved through two rewrites. (1) A face-on
concentric bullseye — scrapped. (2) A camera-facing billboard with a *faked*
inclination (`SQUASH`) in the Gargantua/EHT idiom — looked right head-on but was
flat: the disk never tilted as you flew around it (it was a 2D texture, not 3D).
(3) **Now a full raymarcher** (`bh.frag`): each fragment seeds a real view ray
and integrates the Schwarzschild null geodesic
(`acc = -1.5·h²·p/|p|⁵`, horizon = 1 Rs) with adaptive stepping. This gives a
**view-correct 3D** hole — the accretion disk is a real annulus in the equatorial
plane (`u_disk_normal`, from the body's obliquity), so orbiting shows it face-on
(circular ring) → 3/4 (Interstellar over-the-top arc) → edge-on (thin line + full
lensed halo), all for real. Includes the photon ring (grazing pile-up near
1.5 Rs), the shadow (swallowed rays), Doppler beaming (approaching side brighter
+ bluer), and a faint lensed procedural starfield that fades to transparent where
undeflected so the real background shows. The disk itself is **animated FBM
turbulence with Keplerian differential rotation** (`u_time`, ω ∝ r^-1.5, so it
winds into trailing spirals — inner fast, outer slow) plus a bright inner rim,
instead of a static band pattern. Opaque first-surface disk (front-to-back
correct, no far-side bleed-through). `bh.vert` passes the camera-relative world
pos for the ray seed; `render.c` passes `u_disk_normal`. **Verified via headless
offscreen renders** (see memory `headless-render`): clean across face-on / 3/4 /
edge-on / close-up, 54–79 fps. Tunables at the top of `bh.frag`
(`DISK_IN/OUT`, `STEPS`, `BOUND`).
**TODO later:** background lens uses procedural stars (real starfield isn't a
sampleable texture at BH-draw time); neutron-star variant; let a massive
supernova remnant collapse into one; the higher-order photon-image substructure
inside the shadow is faint but slightly aliased on extreme close-ups.
**Risk:** moderate — new body type touches loader/render; raymarch cost scales
with on-screen coverage (bounded billboard + adaptive steps keep it ~50+ fps).

## 4b. Quasars / AGN / blazars ✅
**Goal:** active-galactic-nucleus phenomena on top of the raymarched hole.
**Done:** new `float agn_activity` on `Body` (0 = quiet, >0 = active). The loader
accepts `"type": "quasar"` (activity defaults 1.0) and an optional `"activity"`
on any black hole; persisted in snapshots (`agn_activity`). Four effects, all
verified via headless renders (`tools/shot.sh`, quiet + quasar + blazar,
face-on / 3/4 / edge-on / pole-on, ~50–105 fps):
- **Supercharged disk** (`bh.frag` `u_activity`): active holes get a broader
  (`disk_out` → 8 Rs), hotter/bluer, HDR-bright blazing disk. `act=0` is
  bit-identical to the quiet hole.
- **Frame-dragging photon ring** (`bh.frag` `u_spin`): the limb rotating toward
  us is brighter/bluer (subtle when quiet, strong when active). Spin sense from
  `rotation_rate`; closest-approach position tracked for the asymmetry.
- **Relativistic jets** (`jet.vert`/`jet.frag`, new additive pass in `render.c`):
  twin collimated beams along the spin axis — flaring cone, two-octave FBM
  filaments advecting outward, and **travelling shock knots**; per-lobe Doppler
  beaming (approaching lobe blazes bright + blue, receding one dims). Axis-aligned
  cylindrical billboard (reuses the sphere-quad VAO); fades out as it goes edge-on
  so the pole-on view stays clean.
- **Blazar**: same jets, jet aimed near the viewer → beaming makes the approaching
  jet outshine everything. Off-axis is dramatic; pole-on shows the face-on disk +
  beamed core.
Since expanded into a full composable AGN engine — see UNIFIED_ROADMAP_REFINED
§1.4 for the authoritative writeup (decoupled `disk`/`torus` ring-elements, dust
torus, helical jets, physics-based sizing from mass/spin, remnant-collapse disk)
and the deferred TODOs.
New presets: `quasar.json`, `blazar.json` (+ existing `black_hole.json`), all in
`presets.c`.
**Risk:** low-moderate — additive jet/torus passes + body fields; quiet BH unchanged.

## 5. Nebulae ✅
**Goal:** real, visitable emission nebulae — far away a backdrop, up close a
volume you fly through, with no LOD transition.
**Done:** new `src/nebula.{c,h}` + `nebula.vert`/`nebula.frag`. 18 real
catalogue nebulae (Orion, Carina, Lagoon, Eagle, Veil, Helix, Pleiades, North
America, …) given **true 3D world positions** from J2000 RA/Dec + **real
distance**, and **real physical radius** from distance × apparent angular size.
Tinted by emission type (emission = red/pink Hα, reflection = blue,
planetary/SNR = teal).
**One representation, no transition** (per the brief): each nebula is a
screen-space **volumetric raymarch** (modelled on the supernova cloud pass) —
domain-warped FBM with a dense knotty core feathering to wispy filaments. A
camera-facing billboard carries it when far; a fullscreen quad takes over when
the camera is near/inside (so it envelops you). Distances dwarf the 2000 AU
render far-plane, so beyond `NEBULA_MAX_DIST` (1500 AU) the centre+radius are
**clamped to a shell with radius scaled by the same factor** (the BH / star-dot
trick) — angular size preserved, clamp is identity at the boundary, so flying in
from far blob → enveloping volume is seamless. Drawn in `render.c` §2.7 after
opaque geometry (premultiplied "over", depth-tested so planets/stars occlude or
embed). Menu → Visuals **Nebulae** toggle; Navigate tab **fly-to** each nebula.
**Data vs. artistic:** positions, physical sizes and per-type colours are real;
only `NEBULA_DENSITY` (opacity/brightness) is exaggerated — real nebulae are far
too faint to see in colour.
**Visual check (verified):** shape archetypes and the seamless far→near fly-in
confirmed good in-build.
**Known follow-ups (deferred):** the volumetric pass costs ~21 fps with a large
nebula filling the screen — render it at half resolution (≈4× fewer fragments,
standard for volumetrics) to recover it. **The half-res technique now exists**:
the supernova cloud renders into a half-res target and composites back via
`vol_composite.frag` (see below) — the nebula pass can reuse the same
target/approach. Near clouds read a touch soft; a small `s_density` bump or
sharper FBM contrast would help. Both adjustable live in Visuals.
**TODO later:** optional CSV asset (`assets/nebulae.csv`) for user-extensible
placement; a faint Milky Way band; per-sample depth compositing for embedded
opaque bodies (currently near-surface depth approximation).
**Risk:** moderate — volumetric transparency + multi-scale clamping.

## 6. Stellar lifecycle + supernova (galaxy-scale) ✅
**Done:** `src/lifecycle.{c,h}` evolves a star main-sequence → subgiant → red
giant → death; high-mass stars core-collapse to a neutron star / black hole, low
-mass stars puff a planetary nebula to a white dwarf. Driven manually from the
Inspect panel (Age to next phase / Trigger Supernova) or continuously via a
**Stellar time** slider (`g_stellar_years_per_sec`, default 0 = manual). Death
routes through `supernova_detonate()` (`supernova.c`), reusing the existing
flash/core/cloud blast + remnant machinery.
**Performance (so it survives the 16k-body "Known Universe"):**
- Cloud raymarch → **half-res** target + composite (`vol_composite.frag`); the
  screen-filling explosion no longer drops to single-digit fps.
- Shock kick is O(N), not O(N²): the reachable set (radius = `shock_speed ×
  CLOUD_DURATION`, mass-dependent) is gathered once at detonation; subtree
  mass/push/destroy walk a cached CSR child index. See `docs/SCALING_HANDOFF.md`.
**Visual check:** needs eyes on a running build — confirm the explosion framerate
and that nearby bodies are still kicked/destroyed.
**Risk:** moderate — offscreen volumetric pass + body-lifecycle events.

---

Order rationale: 1 and 3 are low-risk, high-return shader/VBO tweaks; 2 (bloom)
is the marquee effect but introduces an offscreen pipeline so it goes after the
cheap wins; 4 and 5 are additive new content; 6 adds time evolution and the
half-res volumetric optimisation the nebula pass can reuse. Re-order on request.
