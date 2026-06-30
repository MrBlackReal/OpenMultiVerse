# Visual upgrade roadmap

Sequenced visual improvements for the galaxy-scale view. Each lands and is
verified on its own before the next starts (the app can't be eyeball-checked
headless, so every step needs a quick look in the running build).

Status legend: ✅ done · 🟡 in progress · ⛔ todo

---

## 1. Star size & brightness by type 🟡
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

## 2. Star glow / bloom (HDR) 🟡
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

## 3. Planet & atmosphere detail 🟡
**Atmospheres done** (`atm.frag`, shader-only): added a scattering model on top
of the existing limb glow — a brighter rim line, a warm **sunset** tint through
the twilight band at the terminator, soft day/night falloff, and a **forward-
scatter halo** so a planet backlit by its star gets a bright glowing ring (which
also blooms nicely with feature #2). No C/uniform changes.
**Still TODO (surface):** day/night terminator + night-side emissive and subtle
surface variation in `phong.frag`. Left as a follow-up.
**Risk:** low — contained shader work, no pipeline change.

## 4. Black holes / compact objects ⛔  (new feature, not just visual)
**Goal:** the compact objects you expected. A `black_hole` (and maybe
`neutron_star`) body type with an accretion-disk + gravitational-lensing-style
shader.
**Approach:** add the type to the loader + a `mass`-driven event-horizon radius;
a dedicated billboard shader (lensing ring + glowing disk). Optionally let a
massive supernova remnant collapse into one.
**Files:** `body.h`/`universe.c` (type), new `black_hole.*` shader, `render.c`
pass, a demo preset.
**Risk:** moderate — new body type touches loader/physics classification.

## 5. Nebulae ⛔
**Goal:** emission-nebula backdrops so deep space isn't empty black.
**Approach:** a handful of large additive, soft volumetric billboards (or a
noise-driven sky dome contribution) placed at catalogue nebula positions or
procedurally; drawn behind everything, camera-relative.
**Files:** new `nebula.*` shader + a small placement table; drawn in the
far-field pass region of `render.c`.
**Risk:** moderate — additive transparency + sorting against the starfield.

---

Order rationale: 1 and 3 are low-risk, high-return shader/VBO tweaks; 2 (bloom)
is the marquee effect but introduces an offscreen pipeline so it goes after the
cheap wins; 4 and 5 are additive new content. Re-order on request.
