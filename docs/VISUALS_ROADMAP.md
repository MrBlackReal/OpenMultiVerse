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
**Visual check (verified):** the first `bh.frag` drew a face-on concentric
bullseye that didn't read as a black hole. Reworked to the Gargantua / EHT
idiom — the accretion disk is now an **inclined** elliptical annulus (vertical
`SQUASH`), the opaque shadow occludes its far half so it only shows as a bright
**arc lensed over the top**, the near half crosses in front of the shadow, plus
a tight photon ring and Doppler beaming (approaching side brighter + bluer).
Confirmed in-build: looks like a black hole. Pure fragment-shader change, no
uniform/pipeline edits. Tunables at the top of `bh.frag`.
**TODO later:** real gravitational lensing of the background; neutron-star
variant; let a massive supernova remnant collapse into one.
**Risk:** moderate — new body type touches loader/render.

## 5. Nebulae 🟡
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
**Needs visual check + tuning:** `NEBULA_DENSITY`, the raymarch `STEPS`/step
weighting, and the far-blob look from the solar system. Verify the seamless
fly-in (no pop crossing the clamp / billboard→fullscreen boundary) and that
embedded stars read correctly.
**TODO later:** optional CSV asset (`assets/nebulae.csv`) for user-extensible
placement; a faint Milky Way band; per-sample depth compositing for embedded
opaque bodies (currently near-surface depth approximation).
**Risk:** moderate — volumetric transparency + multi-scale clamping.

---

Order rationale: 1 and 3 are low-risk, high-return shader/VBO tweaks; 2 (bloom)
is the marquee effect but introduces an offscreen pipeline so it goes after the
cheap wins; 4 and 5 are additive new content. Re-order on request.
