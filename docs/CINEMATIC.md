# Cinematic Renderer — Design Specification

Status: **phases 1-5 implemented and verified.** This
document is the agreed design for the `--cinematic` feature set. It is the authority for this work; when it
disagrees with `docs/UNIFIED_ROADMAP_REFINED.md` §6 (Layer 6 — Camera &
Cinematic System), this document wins, and Layer 6 should be considered
superseded by it.

Companion reading: `ARCHITECTURE.md` §10 (Rendering Pipeline), §11 (Shader
Reference), §5 (Core State and Units). The invariants in `CLAUDE.md` —
camera-relative double precision, `MAX_BODIES`, the cold/hot threading split —
apply here unchanged and are restated in §12 where they bite.

---

## Table of Contents

1. [What "cinematic" means here](#1-what-cinematic-means-here)
2. [The two modes](#2-the-two-modes)
3. [CLI surface](#3-cli-surface)
4. [Accumulation sampling — the backbone](#4-accumulation-sampling--the-backbone)
5. [Deterministic time model](#5-deterministic-time-model)
6. [Render targets and resolution independence](#6-render-targets-and-resolution-independence)
7. [Encoder](#7-encoder)
8. [The look layer](#8-the-look-layer)
9. [Camera and shot system](#9-camera-and-shot-system)
10. [Procedural tour and auto-director](#10-procedural-tour-and-auto-director)
11. [Overlays and audio](#11-overlays-and-audio)
12. [Invariants this feature must not break](#12-invariants-this-feature-must-not-break)
13. [Phased build plan](#13-phased-build-plan)
14. [Verification](#14-verification)
15. [Risks and open questions](#15-risks-and-open-questions)
16. [References](#16-references)

---

## 1. What "cinematic" means here

Four things, all of which are in scope:

1. **Offline deterministic film-out** — render N frames at a fixed simulation
   timestep, decoupled from wall clock, so the same command always produces the
   same video regardless of machine speed.
2. **Camera choreography** — the camera stops being free-look and becomes
   directed: keyframed splines, easing, focus tracking of a body.
3. **Upgraded look/quality** — effects too expensive for realtime: ground-truth
   depth of field, motion blur, supersampled antialiasing, long-exposure astro
   look, film grade.
4. **Auto-director** — the renderer picks its own shots from what the simulation
   is actually doing (supernovae, collisions, mergers, close approaches).

### 1.1 What is *not* the gap

The renderer's **astrophysical** realism is already strong and is not what this
feature improves. `bh.frag` raymarches real Schwarzschild null geodesics with a
photon ring and true background lensing; `galaxy.frag` and `nebula.frag` are
volumetric raymarches with dust-lane absorption and differential rotation;
`post.c` has a three-level HDR bloom with ACES tonemapping, auto-exposure,
chromatic aberration, vignette and a lens-flare chain; `settings.relativistic`
already applies aberration and Doppler shift at warp.

The gap is **camera realism** — the optics and shutter of a physical camera, and
the motion of a directed one. That framing is what drives §4 and §8.

### 1.2 Decisions of record

| Question | Decision |
|---|---|
| Renderer backbone | Accumulation sampling (§4) |
| Live-mode performance contract | Same look, fewer samples — one quality dial, not two code paths |
| Time model | Fixed sim-seconds per output frame (§5) |
| Default output | 1920×1080 @ 60 fps; `--film` → 3840×2160 @ 24 fps, 180° shutter |
| Encoding | Raw RGB piped to an `ffmpeg` subprocess; PPM-sequence fallback (§7) |
| Default shot | Procedural tour as the spine, auto-director interrupts (§10) |
| Overlays | Clean frames by default; `--cinematic-info` opts in |
| Audio | Soundtrack muxed by default; `--no-audio` opts out |
| Authoring | Keyframe-drop keybind in the *normal* app; keyframes editable in ImGui |
| Build order | Film-out pipeline first (§13) |

---

## 2. The two modes

`--cinematic` selects the cinematic renderer. Whether it films depends purely on
whether `--output` is present.

### 2.1 Live mode — `--cinematic`

Everything about the app stays the same: a window opens, the free-look camera
works, keybinds work, the ImGui menu works, the sim runs on the wall clock.
**Only the renderer is swapped.** The accumulation sample count is low (default
4) so it stays broadly interactive; DOF, motion blur and the grade are live and
tunable by eye through the ImGui panel (§9.4).

This mode is the tuning surface. What you dial in here is what films.

### 2.2 Film-out mode — `--cinematic --output PATH`

Implies `--headless` unless a window was explicitly requested. The wall clock is
abandoned: the loop becomes a fixed-step render of exactly
`ceil(duration × fps)` frames. Each frame is accumulated from `--samples`
sub-frames, read back, and pushed to the encoder. Progress is reported to
stdout with an ETA. The camera is driven by a shot (§9) or the
tour/director (§10) rather than by input.

---

## 3. CLI surface

All flags are parsed in `main.c` alongside the existing `--headless` / `--shot`
block (currently around `main.c:1272`).

### 3.1 Mode

| Flag | Default | Meaning |
|---|---|---|
| `--cinematic` | off | Use the cinematic renderer |
| `--output PATH` | — | Film out to this file. Extension selects the container/codec. Implies headless |

### 3.2 Output spec

| Flag | Default | `--film` | Meaning |
|---|---|---|---|
| `--res WxH` | `1920x1080` | `3840x2160` | Render resolution, independent of window size |
| `--fps N` | `60` | `24` | Output framerate |
| `--duration S` | `30` | `30` | Seconds of film |
| `--samples N` | `32` (film) / `4` (live) | `64` | Accumulation sub-frames per output frame |
| `--crf N` | `16` | `16` | x264 quality (lower = better) |
| `--film` | off | — | Preset: the `--film` column above |
| `--draft` | off | — | Preset: `1280x720`, 30 fps, 4 samples — fast iteration |

### 3.3 Camera optics

| Flag | Default | Meaning |
|---|---|---|
| `--shutter DEG` | `180` | Shutter angle. Fraction of the frame interval the shutter is open; drives motion-blur length. `0` = no blur, `360` = continuous |
| `--aperture F` | `0` (off) | f-number. Smaller = shallower depth of field. `0` disables DOF entirely |
| `--focus BODY\|DIST` | auto | Focus target: a body name (tracks it, racking focus as it moves) or a literal distance in AU |

### 3.4 Shot source

| Flag | Meaning |
|---|---|
| `--shot-script PATH` | Play a shot JSON (§9.2). Its length sets `--duration` unless one is given |
| `--shot-save PATH` | Load `--shot-script`, rewrite it canonicalised, exit — validates a hand-written shot and makes the format's round-trip testable without the GUI |
| `--tour` | Procedural tour only, no event interruption |
| `--director` | Auto-director only, no procedural spine |
| *(none of the above)* | Tour as spine + director interrupts — the default (§10) |

### 3.5 Presentation

| Flag | Default | Meaning |
|---|---|---|
| `--cinematic-info` | off | Title cards and captions (§11.1) |
| `--letterbox [AR]` | off | Crop to aspect (default `2.39`) with bars, and bake the film grade |
| `--no-audio` | — | Do not mux the soundtrack |
| `--audio PATH` | `assets/soundtrack.ogg` | Alternate audio track |
| `--no-orbits` | off | No orbit trails or orbit prediction: not drawn, and not computed (the per-step trail tick, snapshots and roll-backs, and the predictor all return at once, warm-up included) |

### 3.6 Flag interactions

- `--output` without `--cinematic` is an error (the plain `--shot` path already
  covers single stills).
- `--film` and `--draft` are mutually exclusive; both are applied *before*
  individual flags, so `--film --fps 30` gives 4K @ 30 fps.
- `--samples 1` is legal and disables accumulation entirely (fastest, aliased).
- Existing `--fov`, `--exposure`, `--timescale`, `--stellar-rate`, `--no-hud`
  and `--preset` all continue to work and compose with cinematic mode.

---

## 4. Accumulation sampling — the backbone

This is the central architectural decision and the reason the rest of the design
is simple.

### 4.1 The idea

Each output frame is rendered as **N sub-frames**, each with three things
jittered, then averaged in an HDR accumulation buffer:

| Jitter | Range | Gives |
|---|---|---|
| Sub-pixel **rotation of the camera** | ±½ pixel | Antialiasing (supersampling) |
| Camera position across a lens disc, **re-aimed at the focal point** | aperture radius | Ground-truth depth of field |
| Simulation time within the shutter interval | `shutter/360 × 1/fps` | Ground-truth motion blur |

> **All three are applied by moving and rotating the camera — never by skewing
> the projection matrix.** This is not a stylistic preference; §4.2.1 has the
> measurement that forced it.

One integer — N — controls all of it. This is exactly the "progressive sampling"
model offline renderers use, and it is why the live and film-out modes can share
a single renderer with no divergence in look: live runs N=4, film-out runs N=64,
and nothing else differs.

### 4.2 Why this rather than screen-space post

Screen-space DOF and velocity-buffer motion blur are the realtime-standard
approximations, and both fail badly on this specific content:

- A starfield is a field of **sub-pixel high-contrast points**. TAA ghosts on it
  and FXAA erases it. Jittered supersampling is the only thing that resolves it
  cleanly, and it is the single largest quality win available here.
- Screen-space DOF reads a depth buffer, but this renderer uses **log-encoded
  depth** across a light-year-scale frustum, and much of the frame (nebulae,
  galaxies, the black-hole lens) is drawn with depth writes *off*. A
  circle-of-confusion blur driven by that depth buffer would be wrong precisely
  where it matters.
- Velocity-buffer motion blur needs per-pixel motion vectors, which the
  volumetric raymarch passes do not produce and cannot cheaply.

Accumulation sidesteps all three by construction: it is not an approximation of
what a camera does, it *is* what a camera does, sampled.

### 4.2.1 Why the camera moves instead of the frustum skewing

The textbook way to do accumulation AA and depth of field is to offset the eye
and *skew the projection matrix* — shift `m[8]`/`m[9]` to hold the focal plane
while everything else parallaxes. Phase 1 shipped the AA jitter that way and
phase 2 built depth of field on top of it. **It does not work in this
renderer.**

The planet, atmosphere and volumetric passes — `phong`/`atm`, `nebula.frag`,
`galaxy.frag`, `bh.frag` — reconstruct their camera ray *per fragment* from
`u_fov_tan` and `aspect` (see the comment at `render.c:2089`). They never
consult the projection matrix. A frustum skew therefore moves a volumetric's
bounding geometry but not the image drawn inside it, and moves a planet's
billboard but not the planet.

Measured: a projection skew large enough to sweep the starfield **96 px** moved
a planet **0.2 px**. Depth of field built on it blurred the background
correctly and blurred the subject *just as much*, because the compensation term
that was supposed to hold the focal plane never reached the subject at all.

Rotating the camera instead moves every path — vertex-projected and
ray-reconstructed alike — because the reconstructed rays are oriented by the
camera basis. So:

- **AA** is a sub-pixel *angular* offset (`cinematic_jitter`).
- **DOF** offsets the eye on the aperture disc and then *aims* it back at the
  focal point (`cinematic_lens_aim`), which is paraxially exact and leaves a
  sub-pixel residual at the frame edges (~1 px against a 15 px blur at 30° FOV)
  — far cheaper than teaching every ray-reconstructing shader about a skewed
  frustum.

This also fixed a phase-1 bug nobody had noticed: the skew-based AA jitter was
never antialiasing planets, atmospheres, nebulae, galaxies or the black-hole
lens. Only the vertex-projected paths (stars, dots, trails) were being
supersampled. Switching to angular jitter antialiases all of them.

### 4.3 Long exposure falls out for free

The "astro look" — faint stars building up, fast bodies leaving streaks — is the
same accumulator with the shutter opened past 360° and a different normalization
(sum rather than mean, with an exposure divisor). No separate system.

### 4.4 Cost

N× the render cost per output frame, and each sub-frame additionally re-runs the
sim by `Δt/N` when motion blur is on. At 4K × 64 samples that is roughly 130×
the pixel work of a normal 1080p frame — a 30-second 4K film is plausibly hours.
This is expected and is why `--draft` exists and why the default is 1080p.

### 4.5 Implementation sketch

New module `src/render/cinematic.c` / `.h`. It owns:

- an RGBA32F accumulation FBO at render resolution,
- an RGBA16F per-sub-frame scene target (the existing `post.c` chain renders
  into this instead of the back buffer),
- the jitter sequence (a low-discrepancy sequence — Halton (2,3) — not white
  noise, so N=4 in live mode is already well-distributed),
- resolve: average → tonemap/grade → letterbox → present or read back.

`post.c` already owns an offscreen HDR target and exposes `post_scene_fbo()`;
the cinematic path redirects its final composite into the accumulator rather
than the default framebuffer. That is the smallest possible incision into the
existing pipeline.

---

## 5. Deterministic time model

In film-out, each output frame advances the simulation by **exactly**

```
sim_dt_per_frame = timescale × g_laws.time_scale / fps
```

regardless of how long the frame took to compute. With motion blur on, that
interval is subdivided across the N sub-frames so each sub-frame sees the sim at
a slightly different instant.

Consequences:

- The same command produces a **bit-identical** video on any machine (modulo
  GPU driver differences and OpenMP reduction order — see §15).
- Time-lapse is not a special feature; it is a large `--timescale`.
- The existing 100 ms `dt` clamp in the main loop (`main.c:1520`) must be
  **bypassed** in film-out — it exists to stop a realtime spiral, and would
  silently corrupt a long-timescale film.
- `--stellar-rate` composes with this, so stellar-evolution films work.
- **The integrator's outer-step budget had to be raised for film-out.** The main
  loop caps a frame's sim advance to `MAX_OUTER_STEPS x dt_outer_max`, with
  `MAX_OUTER_STEPS = 120`. That cap is right for realtime — it stops a slow
  frame or a huge sim speed making one frame take unbounded time — but it
  silently truncated the "exactly" above. Measured before the fix: a frame
  asking for 1.08e10 sim-seconds got 518,400, a factor of ~20,000, and with the
  default 1 day/s speed preset *every* timescale above ~144 rendered
  identically. The keyframed `timescale` track (§9.2) saturated the same way.
  Film-out now uses a 200,000-step budget — integrator accuracy is bounded by
  `dt_outer`, not by the number of steps, so the ceiling costs wall time and
  nothing else — and warns once if it still binds, rather than quietly
  producing a slower film than the shot asked for.
- Wall-clock animation phases had to move too: corona shimmer, galaxy rotation,
  starfield twinkle, jet/torus/black-hole phases and `starsys_tick` all read
  `SDL_GetTicks()` directly. They now read `g_render_time` (`common.h`), which
  main.c advances by `dt` — i.e. by exactly 1/fps while filming. Without this
  the sim would be deterministic but the *picture* would not.

Live cinematic mode keeps the existing wall-clock `dt` untouched.

---

## 6. Render targets and resolution independence

Film-out resolution is decoupled from `WIN_W`/`WIN_H`. The clean way to get this
without auditing every `glViewport` call site is to **set `g_win_w`/`g_win_h` to
the render resolution** at film-out startup, before `render_init()`.

`WIN_W`/`WIN_H` are macros over the mutable globals `g_win_w`/`g_win_h`
(`common.h:32`), and every pass — `post.c`'s bloom chain, `render.c`'s volumetric
slots at `render.c:2640`, `2723`, `2826` — already derives its own sizes from
them and already handles resize. So the whole pipeline follows for free, and
filming 4K from a 720p window (or headless with no window at all) needs no
special casing. This also means a resize during live cinematic mode already
works through the existing path.

The accumulation buffer is allocated at the same resolution and re-allocated on
resize alongside `post.c`'s targets.

---

## 7. Encoder

### 7.1 Primary path — ffmpeg subprocess

`popen()` an ffmpeg process at film start and write each resolved frame's raw
RGB to its stdin:

```
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s WxH -r FPS -i -        \
       [-i AUDIO -shortest]                                     \
       -an|-c:a aac -b:a 192k                                   \
       -c:v libx264 -preset slow -crf CRF -pix_fmt yuv420p      \
       -movflags +faststart -vf vflip OUTPUT
```

Rationale: the project's entire dependency story is SDL2 / SDL2_ttf /
SDL2_mixer / GLEW / OpenGL, and linking libavcodec/libavformat would roughly
double it for one feature. A subprocess needs ffmpeg at *runtime only*, is ~40
lines, produces a literal `.mp4`, and supports `.mov`/`.webm`/`.mkv` by
switching codec arguments on the output extension.

The `-vf vflip` handles GL's bottom-up readback; equivalently the readback can
flip on the CPU as `save_screenshot_ppm()` already does (`main.c:1091`).

### 7.2 Fallback

If `ffmpeg` is not on `PATH`, write a numbered PPM/PNG sequence next to the
requested output and print the exact ffmpeg command to finish the job. The film
is never lost to a missing encoder.

### 7.3 Readback

`glReadPixels` on a 4K frame stalls the pipeline hard. Use a **two-deep PBO
ring** so frame *n*'s readback overlaps frame *n+1*'s render. Worth doing from
the start — it is a small amount of code and the stall is otherwise a large
fraction of film-out wall time at high resolution.

---

## 8. The look layer

Beyond what §4 gives for free:

### 8.1 Camera controls — familiar units, scene-scaled lens

Aperture (f-number) and shutter angle are the user-facing optics controls,
driving DOF radius and motion-blur length. Exposure stays where it already
lived (`settings.tonemap_exposure` / `--exposure`).

The original intent was a *literally* physically-based camera. That is not
achievable, and the reason is worth recording: **a real aperture produces
exactly zero depth of field at astronomical scale.** The circle of confusion
scales with the sensor's own size — ~24 mm, which is 1.6e-13 AU — so against a
scene measured in AU the blur sits many orders of magnitude below one pixel.
Physical units are simply the wrong model for this scene.

So the f-number is kept as a *control surface a cinematographer recognises*,
mapped onto a lens whose radius scales with the focus distance:

```
R = APERTURE_K * focus / N        (APERTURE_K = 0.032, cinematic.c)
```

Scale-invariance is the point: f/2.8 gives the same look framing a moon at
0.01 AU as framing a galaxy at 1e11 AU, which is exactly what a camera in a
scale-continuous renderer should do. `APERTURE_K` is calibrated so f/2.8 is
roughly a 15-pixel background blur at 1080p / 45° FOV.

### 8.2 Focus

Focus distance is one of three things, in priority order:

1. **Locked to a named body** (`--focus Earth`) — resolved by name every frame
   rather than cached as an index, because a tracked body can be absorbed
   mid-shot and `g_nbodies`' dead slots get reused, so a stale index could
   silently point at a different body. The bulk field-star range is skipped in
   O(1); frozen scenery is never a focus subject.
2. **Auto** (`settings.cine_focus_auto`, the default) — the nearest body, read
   from the shared `g_cam_prox` pass, which already handles the field-star
   range correctly.
3. **A literal distance** (`--focus 12.5`, in AU).

Focus pulls between two bodies become a keyframed track in the shot script
(§9.2) in phase 3.

### 8.3 Grade and film look

Applied at resolve, after the accumulation average and the existing ACES
tonemap: lift/gamma/gain (or a 3D LUT), film grain, optional halation on the
brightest highlights, and letterbox crop. Off by default; `--letterbox` turns on
bars + grade together since they are one stylistic choice.

### 8.3a Star veil — glare from a nearby star

A camera exposed for a nearby star cannot record the stars behind it: the Sun
from Earth is ~25 magnitudes brighter than Sirius, which is why Apollo photos
show a black sky. The background layers are tuned for visibility rather than
scaled physically, so beside a blazing sun they used to stay at full strength.

The first version was a uniform exposure cut, which dimmed the whole sky
evenly. That produced a black sky with a sun pasted into it and read as
unrealistic. The shipped model is **glare**, and one formula
(`src/render/star_veil.h`) runs in C and, through the `gl_utils.c` shader
prelude, in GLSL:

```
glare(theta) = E_psf * 100/theta^2  +  E_floor * 0.14      theta in degrees
visible      = smoothstep(-1, 1.5, log2(lum / glare))
haze(theta)  = E_psf * 100/theta^2                          added in post
E_psf        = 0.01 * (irradiance/Earth)        * frame weight * eclipse
E_floor      = 300  * (irradiance/Earth)^0.5    * frame weight * eclipse
```

- **Two scales, two jobs.** `E_psf` is the star's glare point-spread and
  follows its *linear* brightness. With a 1/θ² PSF, the angle at which the
  glare reaches any given level then shrinks as 1/d, exactly like the disc:
  the Sun's glow is huge from Mercury and a small bright point from Neptune,
  matching a planet-by-planet reference. `E_floor` is the overall exposure
  cut, on a square-root scale so the washout eases off smoothly as you leave a
  system: a black sky from Mercury to Saturn, only the brightest stars and the
  Milky Way core at Neptune, the Milky Way back by ~100 AU, nearly nothing by
  1500 AU.
- **Local.** The sky drowns nearest the star first, and a star just outside
  the frame washes out the edge it is about to enter.
- **By brightness.** Each source is tested against the glare at its own
  position, so faint stars go first while bright stars, and the Milky Way far
  from the star, hold on. Diffuse layers (galaxy, nebula) are tested four stops
  brighter than they are drawn, because the star sprites are overbright for
  visibility and a like-for-like test drowned the Milky Way far too early.
- **The glare you see is the glare that hides.** The visible haze and the
  mask's local term are one quantity, so a source is only hidden where glare of
  comparable brightness is on screen, and no black hole can form around a star.
  Two earlier versions broke this and are worth remembering: one saturated the
  haze as a camera stopping down would, which kept the Sun's glow the same size
  from Mars to Neptune; one scaled the mask far above the drawn haze, which cut
  a black disc around the Sun at Uranus and Neptune.
- **Frame weight and eclipses.** Full when the star is in frame, fading out
  30° past the edge (stray light, as behind a lens hood), none from behind. A
  planet covering the disc removes its glare, so stars come out during totality.
- **Exempt.** The dominant star's own system (planets, belts, companion) is
  what the exposure is set for, and is never veiled.
- **Capped at ~10x Earth's sunlight.** Past that a camera stops down rather than
  letting glare grow without bound; uncapped, 60 AU from an accreting black
  hole put the haze in the thousands and the frame went solid white.
- **Black holes cast no point glare.** Their light comes from an extended
  accretion disc, and the centre is the shadow; a point PSF there buried the
  shadow and photon ring under a white blob. They still set the exposure.
- **Supernovae glare while they flash.** A detonation has no body, and the veil
  used to skip body-less light entirely. So the moment a star died its glare
  vanished in one frame and the sky popped in, exactly when the flash should
  have washed it out. The supernova's transient emitter now glares like the
  star it replaced, weighted by the flash intensity alone (the core and ejecta
  cloud are resolved glows, not points), so the glare fades as the flash does
  and the expanding cloud comes through. The flash exceeds the cap from its
  first instant, so its glare is eased in once per output frame (held across
  accumulation sub-frames, like auto-exposure), as a camera's exposure would
  follow. Only rises are eased, and only for a supernova, so cuts and fly-bys
  keep instant glare. The flash itself also rises over ~0.3 sim-days instead of
  0.012, which at film timescales landed in a single frame.

Layers covered: skybox starfield, dynamic star dots and other stars' coronae
(on the CPU, which is how the exemption works), field stars, cluster glows,
galaxy volumes (volume composite) and nebulae. It has **no setting, slider or
flag**, deliberately: a real camera has no glare knob, the glare simply is.
Tuning lives in the constants in `star_veil.h` and `STAR_VEIL_SCALE` /
`STAR_VEIL_PSF` in `render.c`. It applies in the normal app too, not only cinematic mode.

### 8.4 Quality wins in film-out

Two of the four turned out to be free, and are **done**:

- `u_steps` on `nebula.frag` / `galaxy.frag` is scaled by
  `settings.cine_quality` (default 2x) during film-out. It was already a
  CPU-side uniform, and scaling the *base* before the on-screen-size falloff
  keeps the distance LOD intact.
- The dot-dedup LOD (`settings.dot_hide_px`) is forced off while filming. It
  exists to stop far-field stars shimmering as they cross; supersampling solves
  that properly now, and the dedup only costs stars.

The other two are **not free and are deferred**, with reasons:

- `bh.frag`'s march count is `const int STEPS = 160`, a compile-time constant
  the driver unrolls against. Making it a uniform would lose that in the live
  path too — a perf regression traded for an offline-only win. Wants a
  shader-variant or `#define`-injection mechanism first.
- `BLOOM_LEVELS` (`post.c:50`) sizes fixed arrays and a 3-component weight
  uniform. Raising it is a real refactor of `post.c`, not a constant bump.

### 8.4a Earth surface imagery

Earth is textured with real satellite imagery rather than the procedural
recipe: NASA *Blue Marble Next Generation* (day, 5400x2700) and *Black Marble
2016* (night lights, 3600x1800), both public domain; source and credits in
`assets/textures/earth/README.md`. Loaded by `src/render/earth_tex.c` through
a vendored `stb_image.h` (`extern/stb/`), stored as sRGB with mipmaps and
anisotropic filtering, and applied in `phong.frag` to the body named "Earth"
only (Earth-like exoplanets keep the procedural recipe).

- The imagery replaces the procedural continents, and a land/sea mask derived
  from it (ocean is dark and blue-dominant) drives the ocean glint. Procedural
  relief and mountain snow are off for textured Earth: fake mountains in the
  wrong places look worse than none, and the imagery already shows real snow.
- City lights come from Black Marble, keeping only the warm light above its
  faint blue moonlit base.
- The longitude seam uses a second, shifted parameterisation's gradients where
  they are smaller (Tarini's method), so mip selection never jumps there.
- Clouds stay procedural and animated, but are sparser and feathered over the
  imagery (hard-edged, they read as white blotches on a dark planet), and a
  day-side Rayleigh haze thickens toward the limb. Without it true-colour
  oceans looked nearly black; with it the disc reads as the familiar blue
  marble.
- Resolution is the user's choice: `earth_day_texture` / `earth_night_texture`
  in `settings.json` may point at higher-resolution NASA releases, and
  `earth_texture_max_px` (the "Earth texture" menu control) caps the loaded
  width. A missing file falls back to procedural Earth.

### 8.5 Explicitly out of scope

Ray-traced global illumination, spectral rendering, and true GRMHD accretion
physics. The existing approximations are good; none of these is the bottleneck
on perceived quality.

---

## 9. Camera and shot system

### 9.1 Where it hooks in

`benchmark.c` already proves the pattern: while active, it sets
`g_cam.pos/yaw/pitch` directly each frame instead of the free-look camera, and
`main.c` dispatches to it. The cinematic camera uses the same hook.
`cam_fly_to()` (`camera.h`) already implements eased point-to-point flight with
heading-rotate → travel → settle, and is the model for a single move.

Module: `src/render/cinema_cam.c` / `.h`. It owns the keys, the interpolation
and the shot's own playback clock — the clock lives there rather than in
main.c so the ImGui editor can scrub and preview without reaching into the
main loop's locals.

### 9.2 Shot script JSON

Lives in `assets/shots/*.json`, parsed by the existing `core/json.c` (which
already accepts `//` comments and trailing commas). Positions are **AU, doubles**
to match `Camera.pos`.

```jsonc
{
  "name": "Sol to Sgr A*",
  "fps": 60,                     // optional; CLI wins
  "duration": 45.0,              // total seconds; derived from keys if absent
  "keys": [
    {
      "t": 0.0,                  // seconds into the shot
      "pos": [0, 5, 20],         // AU, absolute — OR:
      "anchor": "Earth",         // ...track a body, pos becomes an offset
      "offset": [0, 0.001, 0.003],
      "look_at": "Sun",          // body name, or omit and use yaw/pitch
      "yaw": 0.0, "pitch": -15.0,
      "fov": 45.0,
      "focus": "Earth",          // body name or AU distance
      "aperture": 2.8,
      "timescale": 1.0,          // keyframable — real-time here...
      "ease": "inout"            // in | out | inout | linear | hold
    },
    { "t": 20.0, "anchor": "Sun", "offset": [0, 4000, 0],
      "timescale": 3.15e7 },     // ...one year per second here
    { "t": 45.0, "pos": [0, 8e8, 0], "fov": 60.0, "ease": "out" }
  ]
}
```

- Position interpolates as a **centripetal Catmull-Rom spline** through the
  keys (C1 continuous, passes through every key). Knots are spaced by the
  square root of each segment's length. The earlier uniform spline took each
  key's tangent from its neighbours regardless of their distance, so a key
  between a 0.01 AU segment and a 1e9 AU one overshot and the camera made a
  visible U-turn at every change of scale; the centripetal form cannot cusp or
  loop, whatever the ratio. Timing is unchanged (u is still the normalised
  segment time). Missing end neighbours are reflected, so a shot starts and
  ends moving along its first and last segments.
- **`subject`** (optional) names what a key presents, for the `--cinematic-info`
  title card: a body, nebula or galaxy by its catalogue name (`"Lagoon (M8)"`,
  `"Milky Way"`). Without it the title comes from `look_at`, else `anchor`; a
  key with none of the three (a transit aimed by yaw/pitch) shows no title, so
  cards appear on arrival rather than while the destination is still a dot.
- Direction: each key contributes a look direction evaluated **from the current
  interpolated position**, and the two are slerped. When both keys look at the
  same body this makes the subject *exactly* centred for the whole segment,
  which is the property you actually want from `look_at`.
- `shutter` is keyframable for the same reason `fov` is: on a hyper-fast leg a
  star crosses the frame within one frame interval, and a 180° shutter turns
  that into N discrete ghosts that no practical sample count resolves. Closing
  the shutter shortens the streak instead — what a camera operator would do.
- `timescale` and `focus` distance interpolate **geometrically**, not linearly:
  they routinely span many decades (1 to 3e7 seconds-per-second), and a linear
  ramp between 1 and 3e7 sits above 1.5e7 for half the segment — which reads as
  an instant jump followed by nothing. `fov` lerps normally.
- `aperture` also interpolates geometrically, because f-numbers are a log scale
  (the midpoint between f/2.8 and f/11 that a photographer expects is f/5.6).
  **Zero is special-cased**: it means "depth of field off", not "an infinitely
  wide aperture". Interpolating into it would ramp through f/2, f/1, f/0.1 —
  blurring harder and harder right up to the instant it switches off, the exact
  opposite of the intent. A segment with a zero at either end holds the near
  key's value and switches at the key. (Found by watching the sample shot do
  precisely this.)
- **Unset fields inherit within a move.** `fov`, `aperture`, `focus`,
  `timescale` and `shutter` are optional. Leaving one out takes the value of the
  nearest earlier key that sets it, stopping at a `cut`. With nothing to
  inherit, the value from before the shot began is used. It used to mean
  "leave whatever the last evaluated frame set", which made a frame depend on
  evaluation history and produced the black AGN legs (§13, phase 4).
  `"aperture": 0` is the explicit way to switch DOF off for a move.
- An `anchor` key is resolved *per frame* against the body's current position,
  so a shot anchored to a moving planet follows it. Names resolve through
  `body_find_named()` every frame rather than being cached as indices — a body
  can be absorbed mid-shot and `g_nbodies`' dead slots get reused, so a stale
  index can silently come back pointing at something else.
- **`detonate`** (optional) names a star to kill as playback crosses the key's
  `t`: a supernova, or a planetary nebula for a low-mass star, through
  `lifecycle_trigger_death()`. It fires from the shot clock (play, advance and
  film-out's per-frame set), never from evaluation, so scrubbing a preview
  cannot kill stars and a film-out fires on the same frame every run. The
  star is retired and replaced by `"<name> Remnant"`. Anchor and `look_at`
  names that no longer resolve fall back to that remnant, so keys after the
  detonation can keep naming the star. `assets/shots/supernova.json` is the
  worked example.
- A malformed shot is parsed into a scratch buffer and only committed on
  success, so a typo in a new file cannot destroy the shot you were authoring.

### 9.3 Authoring by keyframe drop

In the **normal** app (not only cinematic mode), a keybind drops a keyframe at
the current camera pose into an in-memory shot; another key saves it to
`assets/shots/`. Keyframe drop rather than continuous path capture: raw flight
paths are jittery, enormous, and uneditable, whereas a spline through a dozen
deliberate poses is clean, small and hand-editable afterwards.

### 9.4 ImGui panel

A "Cinematic" panel in the `U` menu (`src/ui/menu.c`, `USE_IMGUI`-gated,
following the existing `settings.h`/`menu.c` pattern used for render tunables):

- **Look**: samples, aperture, shutter, focus, grade, letterbox — live, applied
  immediately, persisted to `settings.json`.
- **Keyframes**: a list of the current shot's keys with editable `t`, position,
  yaw/pitch, fov, focus, timescale and easing; add / delete / reorder / jump the
  camera to a key / preview-scrub the shot.

The menu-less build (`make IMGUI=0`) keeps working — the panel compiles to the
same inert stubs the rest of `menu.c` already uses, and the CLI/JSON path is
fully functional without it.

---

## 10. Procedural tour and auto-director

Default behaviour with no `--shot-script`: **the tour is the spine, events
interrupt it.**

### 10.1 Tour (the spine)

Generate a shot list from what is actually in the loaded universe, the way
`benchmark.c` builds its legs from the galaxy catalogue: score candidate
subjects (curated bodies, stars with planets, black holes, nebulae, galaxies) by
interest and visual scale, order them into a route that reads as a journey —
close → out → across → in — and compose each leg from a small vocabulary of
moves (approach, orbit, reveal-pull-back, drift-by).

Must skip the Gaia field-star range `[g_field_star_begin, g_field_star_end)` —
those are frozen scenery, never subjects.

### 10.2 Director (the interrupt)

Watch for events the sim generates — supernovae (`fx/supernova.c`), collisions
(`sim/collision.c`), mergers, close approaches, accretion flares
(`sim/accretion.c`). Score each by spectacle and by whether it is reachable
within a plausible camera move. When one wins, cut to it, cover it for a
duration matched to the event, then resume the tour.

`field_graph_stats()` already logs events (`main.c:1915`), which is a natural
signal source to build on rather than adding a parallel event bus.

### 10.3 Why both

The tour alone guarantees watchable footage in a quiet universe; the director
alone produces unpredictable runtime and dead air. Spine-plus-interrupt gives a
film that always has something to show and still captures the drama when it
happens.

---

## 11. Overlays and audio

### 11.1 Titles — `--cinematic-info`, off by default

Lower-thirds that fade in when the camera arrives somewhere new: body name,
distance from camera, simulation date, scale bar. Reuses the existing SDL2_ttf
label pipeline (`render/labels.c`, `ui/ui.c`). Drawn at resolve time, after the
accumulation average — text must never be supersampled through the accumulator
or it will smear under motion blur.

Default output is **clean frames**: no text, no bars.

### 11.2 Audio — on by default

`assets/soundtrack.ogg` is muxed by ffmpeg as a second input, trimmed with
`-shortest`. `--no-audio` disables; `--audio PATH` substitutes. The audio is
handed to ffmpeg as a file — SDL2_mixer is not involved in film-out.

---

## 12. Invariants this feature must not break

Restated from `CLAUDE.md` because each one is specifically reachable from this
work:

- **Camera-relative double precision.** Any new geometry (title cards anchored
  in world space, focus-distance math, spline evaluation) must subtract the
  camera position *in double on the CPU* before casting to float. Spline
  control points are `double[3]` in AU for this reason. Getting this wrong
  produces jitter that only appears at light-year distances — exactly where the
  tour spends most of its time.
- **`MAX_BODIES` (=128)** bounds per-frame arrays; the director's candidate
  lists and the tour's subject lists must respect it or allocate separately.
- **Field stars** `[g_field_star_begin, g_field_star_end)` are frozen scenery —
  skip them in every per-body loop (subject selection, focus targets, labels).
- **`g_nbodies` is a high-water mark, not a live count** — iterate `[0,
  g_nbodies)` and skip `!alive`. A director tracking a body must handle that
  body being absorbed mid-shot.
- **Threading.** Nothing in the cinematic path runs on the OpenMP cold path
  today, and it must stay that way — no cinematic state written from anything
  reachable by the parallel integration loop.
- **`g_laws` is per-universe runtime state.** `time_scale` is read from it, not
  assumed to be 1.

---

## 13. Phased build plan

Film-out pipeline first: a real `.mp4` out of the tool before any choreography
or look work lands on top of it.

### Phase 1 — Film-out pipeline ✅ done

Deterministic fixed-step loop; offscreen render at arbitrary resolution; the
accumulation buffer with pixel jitter only (so: supersampled AA, no DOF, no
motion blur yet); PBO readback; ffmpeg pipe with PPM fallback; audio mux;
progress/ETA reporting. Camera is static or from `--cam`.

**Done when:** `./verse --cinematic --output out.mp4 --duration 10` produces a
watchable, clean, reproducible 10-second 1080p60 mp4. — **met.**

What landed:

| File | Change |
|---|---|
| `src/render/cinematic.c/.h` | New module: config, accumulation targets, Halton jitter, PBO ring, encoder |
| `assets/shaders/cinematic_blit.frag` | Accumulate/resolve pass (one shader, `u_scale` picks the role) |
| `src/render/post.c/.h` | `post_set_target()` — redirects the final composite off the back buffer |
| `src/core/common.h`, `src/render/render.c` | `g_render_time` replaces `SDL_GetTicks()` in 12 animation call sites |
| `src/main.c` | CLI surface, film-out setup, sub-frame loop, capture, clean-frame gating |

Measured on the development machine:

- **AA is real, not cosmetic.** At `samples=1` field stars are hard single-pixel
  squares; at `samples=32` they are soft and graded, and *more stars are
  visible* — sub-pixel stars that fell between sample points now get partial
  coverage. This is the §4.2 argument confirmed: no post-process AA can recover
  those.
- **Cost scales linearly with N**, as predicted: 720p ran 22.2 fps at
  `samples=1` and 1.12 fps at `samples=32`.
- **`--film` (4K24, 64 samples) costs ~18 s/frame** → a 30-second film is ~3.6
  hours. §15's "plausibly hours" was right; `--draft` and the 1080p default are
  the mitigation.
- **Determinism holds bit-for-bit** — see §15, which was more pessimistic than
  the measurement.
- Verified end to end: valid H.264 + AAC MP4 at the exact requested frame count
  and duration; PPM fallback when ffmpeg is hidden from `PATH`; the large-
  timescale time-lapse (confirming the `dt` clamp bypass); an intergalactic
  shot of Andromeda at ~3 Mly with no precision artifacts; `make IMGUI=0`
  builds and films; the plain `--shot` path is unaffected.

### Phase 2 — The look layer ✅ done

Shutter-interval time jitter (motion blur); lens-disc jitter (DOF) with
body-locked focus; camera optics controls; grade, grain, letterbox; the
film-out quality bumps in §8.4; the ImGui **Look** panel.

**Done when:** live `--cinematic` looks visibly better than the default
renderer and the film-out matches it at higher sample counts. — **met.**

What landed:

| File | Change |
|---|---|
| `src/render/cinematic.c/.h` | Angular jitter, lens sampling + aim, focus resolution, shutter fraction, quality scale, grade uniforms |
| `assets/shaders/cinematic_blit.frag` | Resolve-time grade: lift, contrast, saturation, warmth, midtone-weighted grain, letterbox |
| `src/core/settings.h/.c` | 13 persisted `cine_*` look fields |
| `src/main.c` | `advance_simulation()` extracted from the loop; per-sub-frame sim slices; angular jitter + DOF applied to the camera; look CLI flags |
| `src/render/post.c/.h` | `post_set_autoexposure_hold()` |
| `src/render/nebula.c`, `galaxy.c` | Film-out raymarch step multiplier |
| `src/ui/menu.c` | "Cinematic (look)" panel |

Measured:

- **Depth of field is correct**: focused on Earth at f/2.8, the subject's
  coastlines and clouds stay crisp (1.4% RMSE vs the aperture-0 reference —
  the sub-pixel residual) while the background blurs (2.2% RMSE, stars smeared
  away entirely). Before the §4.2.1 fix the subject blurred *as much as the
  background* (11.6%).
- **Motion blur is correct and does not smear what isn't moving**: with a
  frozen sim (`--timescale 0`), shutter 0° and 180° produce **byte-identical**
  output. Under a large timescale, orbiting asteroids streak while the distant
  field stars in the same frame stay points.
- **Grain is midtone-weighted as designed**: 0.018 mean delta on a lit planet
  vs 0.0004 in empty space, a 50x ratio.
- **Letterbox is exact**: 92 px bars at 1280x720 → 2.39:1, against 92.2 px
  predicted; content only inside the band.
- **Determinism survives all of it** — DOF, motion blur, grain and letterbox
  together still produce byte-identical MP4s across runs.
- `make IMGUI=0` builds and films; the plain `--shot` path is unaffected;
  warning-clean.

### Phase 3 — Camera system ✅ done

Shot-script JSON + Catmull-Rom splines + easing + anchors + look-at + keyframed
timescale, fov, aperture and focus. Keyframe-drop authoring in the normal app;
the ImGui **Keyframes** editor; save/load to `assets/shots/`.

**Done when:** a hand-authored or recorded shot films end to end. — **met.**

What landed:

| File | Change |
|---|---|
| `src/render/cinema_cam.c/.h` | New module: key model, Catmull-Rom + slerp + easing, anchor/look_at resolution, playback clock, JSON load/save |
| `src/core/body.c/.h` | `body_find_named()` — shared, field-star-safe name lookup; the focus code now uses it too |
| `src/main.c` | `--shot-script`, `--shot-save`, `K`/`Shift+K` keybinds, per-sub-frame shot evaluation, shot clock |
| `src/ui/menu.c` | "Cinematic (shot)" panel: load/save, play/stop, scrub, per-key editor |
| `assets/shots/sol_departure.json` | Reference shot exercising every feature of the format |

Measured:

- **`look_at` + `anchor` hold the subject exactly.** Orbiting Earth at a fixed
  0.0006 AU with `look_at: Earth`, the planet stays dead on the frame centre
  through the whole move while its terminator rotates. (Two earlier "drift"
  readings were a contaminated centroid — the Sun and the Milky Way band in
  frame — not a camera error. Measure blobs, not frame means.)
- **Camera motion blur works**, which is new in this phase: with the simulation
  *frozen* (`--timescale 0`), a fast pan renders sharp star points at shutter 0°
  and clean horizontal streaks at 180°. Sub-frame *s* re-evaluates the shot at
  `t + (s/nsub)·shutter_fraction/fps`, so the pose genuinely differs across the
  open shutter.
- **The scale-crossing regression test passes** (§14): one continuous 15-second
  move from 0.0004 AU off Earth to Andromeda at ~1.9e11 AU — fifteen orders of
  magnitude — with no jitter, z-fighting or popping at any point.
- **The format round-trips byte-stably**: load → save → load → save produces
  identical files, preserving anchors, `look_at`, focus-by-name and easing.
- **Determinism survives shots**: shot + DOF + motion blur + grade still
  produces byte-identical MP4s across runs.
- A shot with too few keys is rejected with a message and leaves the previous
  shot intact; `make IMGUI=0` builds and films; the plain `--shot` path is
  unaffected; warning-clean.

### Phase 4 — Tour and director ✅ done

Procedural tour generation from universe contents; event scoring and
interruption; resume logic. `--tour` / `--director` to isolate either.

**Done when:** `./verse --preset assets/universes/known_universe.json
--cinematic --output tour.mp4 --duration 120` yields a watchable film with zero
authoring. — **met** (verified at `--draft`: 3600 frames in 153 s; every leg
frames its subject, from Jupiter to the Milky Way finale).

Status: `src/render/cinema_tour.c/.h` is written and wired into `main.c` (tour
built when filming without `--shot-script`; `--tour` / `--director` isolate a
half and are now mutually exclusive). `--shot-save PATH` without
`--shot-script` writes the generated tour out as a normal shot, which is how to
inspect or hand-tune one without rendering it.

Fixed this pass:

- Cutaways parked the tour clock, pushing the rest of the tour later and
  truncating the finale under the fixed frame count. The spine clock now keeps
  running under a cutaway, and no cutaway starts if it would reach the finale.
- The director treated warm-up events as new (`s_last_event_time` started at 0)
  and could cut on frame 0; it now starts at `g_sim_time` when the tour is built.
- `build_cutaway` never set `vis_au`, so every cutaway framed at ~1e-9 AU.
- `leg_timescale` keyed on physical radius, so no ordinary star ever got the
  "orbits sweep" clock; it now keys on `vis_au`.

**Fixed — the black AGN legs.** In the 120 s known-universe tour the Messier
77 and NGC 1275 legs rendered black and M87 sat off-centre. The cause was not
the spline: those legs' keys set no `aperture` or `focus`, and
`cinema_shot_eval` used to *leave unset fields as they were*. So they inherited
the previous star leg's f/8 focus lock on **KOI-351**, ~1e12 AU away. With
`R = APERTURE_K · focus / N` that is a lens disc ~4e9 AU wide, and every
jittered sub-frame threw the camera billions of AU off a subject it was framing
from 100 AU. The tell was the sample count: black at `--samples 4`, perfect at
`--samples 1`, where there is no lens jitter. (The earlier "truncating the keys
fixes it" clue was a red herring; this is the measurement that separates it.)

The fix is in the format's semantics, not the tour. An unset optional field now
inherits from the nearest earlier key **in the same move** (a cut starts a new
one), and otherwise falls back to its pre-shot value (§9.2). A frame is a
function of `t` alone, never of which frames were evaluated before it, which
also makes scrubbing backwards in the editor honest. `cinema_shot_end` now
restores a `--focus` lock rather than clearing it.

**Director exercised.** With `--stellar-rate 1e6` the known universe produces a
supernova (HD 240237) at 50.8 s, and the director cuts to it and hands back
cleanly to the spine, which keeps its schedule. The first real cutaway showed
two framing bugs, both now fixed in `build_cutaway`:

- The key `timescale` multiplies `g_sim_speed`, which defaults to **1
  sim-day/s**. At timescale 1 the flash (gone by ~0.4 day) filled the whole
  cutaway, and the camera, framed on the progenitor's ~12 AU glare, sat
  inside the ~150 AU flash radius of a red giant, so it went white and then grey
  fog. A tried ramp of 1e4 s/s was *days* per second and pushed the entire
  320-day event through in one frame. The cutaway now ramps in absolute
  sim-days per second (0.1 → 3 → 10, divided by `g_sim_speed`), which plays
  flash → fireball → ejecta shell over ~18 sim-days in six seconds.
- A supernova is framed on its shell at the end of the cutaway
  (`SN_FRAME_AU` = 40 AU), and is pinned statically at the birth site. It is no
  longer anchored to the remnant, because the blast stays put in world space
  while the remnant drifts (`supernova.h`).

Also verified after these fixes: the replayed tour produces byte-identical MP4s
across runs; `--aperture 2.8` as a baseline renders every AGN leg; the shot
format still round-trips byte-stably; `make IMGUI=0` builds and films;
warning-clean.

Known limits, not blockers:

- `SN_FRAME_AU` is a constant sized for a red-giant progenitor. A red
  supergiant's flash radius scales with its initial radius and could still
  engulf the camera. The robust version asks `supernova.c` for the event's
  radius at the end of the cutaway.

Resolved after phase 4:

- **Radial streaks on nebula, galaxy and distant-star legs: fixed.** Confirmed
  as the warp aberration: its strength came from the camera's raw position
  change per frame (full at 60,000 AU/s), and a directed camera covering ~10⁹ c
  between keys pinned it at maximum, giving a radial "warp tunnel" and the frame
  shoved off-centre. It is now off whenever a shot or tour drives the camera
  (choreography, not a warp flight); manual warp flight keeps it. With a shot
  playing, frames are pixel-identical to `--relativistic 0`. The short dashed
  streaks that remain at low sample counts are genuine camera motion blur
  under-sampled (§15), and become smooth streaks at film sample counts.
- **Editor scrubbing: fixed.** Scrubbing the ImGui "Cinematic (shot)" timeline
  now starts a *preview* that holds the same snapshot of your settings as
  playback, so unset fields fall back exactly as they do when playing, in
  either scrub direction. "End preview" restores your settings, and Play
  continues from the previewed moment. Quitting mid-play or mid-preview
  restores the snapshot before settings are saved, so a shot's fov/aperture
  are never persisted as your preferences.

### Phase 5 — Presentation ✅ done

`--cinematic-info` title cards and captions.

**Done when:** a film with `--cinematic-info` names each subject as the camera
arrives, and the same command without it still produces clean frames. — **met.**

What landed:

| File | Change |
|---|---|
| `src/render/cinema_titles.c/.h` | New module: subject tracking, lower-third timing, distance/date/scale-bar/speed formatting |
| `src/ui/ui.c/.h` | `ui_cine_title()` / `ui_cine_speed()`: output-resolution fonts, shadowed text, feathered scrim, accent rule, scale bar |
| `src/render/cinema_cam.c/.h` | `CineSubject`, `cinema_shot_subject()`, `cinema_body_kind()`, `cinema_shot_cut_between()` |
| `src/render/cinema_tour.c/.h` | Per-leg subject record, cutaway event subject, `cinema_tour_current_subject()` |
| `src/render/cinematic.c/.h` | `cinematic_picture_band()`: overlays sit inside the letterbox |
| `src/main.c` | `--cinematic-info`; overlay drawn after the resolve, before capture |

How it behaves:

- **Subject** comes from the tour (current leg, or the event a director
  cutaway covers), from a shot (the current key's `look_at`, else its
  `anchor`), or, for the free camera in live mode, from the nearest body once
  you have arrived at it (within ~60 radii, or 60 glare radii for a star) and
  stayed for a second.
- **Lower-third**, lower left of the picture: name; kind and live distance
  ("Supermassive black hole · 242,167 light-years away"); the date; and a
  scale bar giving a round length at the subject's depth ("100,000 km",
  "20 AU", "20,000 light-years"). It waits 0.6 s after a cut, fades in over
  0.9 s, holds 4.5 s and fades out over 1.2 s, all on the film clock, so it is
  deterministic.
- **Date** is J2000.0 + `g_sim_time`, because the Solar System is seeded from
  J2000.0 elements and the orbital clock counts from load. A universe seeded
  from another epoch would show a date offset by that difference. There is no
  per-universe epoch field yet. Past ~7000 years it switches to "Year N".
- **Speed readout**, lower right, always on with `--cinematic-info` while the
  camera moves. It is measured **relative to the subject** in film seconds: an
  anchored camera rides a body that the time-lapse moves thousands of times
  faster than real (a star's galactic drift at 30 sim-days/s is ~1,700 c),
  which is true displacement but meaningless on screen. Hard cuts are skipped,
  and it is smoothed in log space (~0.25 s). Format: km/s below 0.01% c,
  `0.25% c` up to 10%, `0.85 c` / `3.2 c` / `1,240 c` beyond, then words
  (`4.1 million c`). Nothing caps it: the camera is a massless observer. The
  honest numbers are large, because a two-minute tour of the known universe is
  wildly superluminal: ~1 c orbiting Jupiter, ~600 c across an exoplanet
  system, ~10⁹ c around a nebula and ~10¹² c on the M87 leg.
- **Readability**: fonts are opened at output resolution (4K text is rendered
  at 4K, not upscaled), with a soft shadow and a feathered dark scrim so text
  holds over a supernova as well as over black sky.
- Drawn once per output frame after `cinematic_frame_resolve()` and before
  capture, so text never goes through the accumulation (no motion-blur
  smear), and it composites after the grade (grain does not crawl over it).
- Without the flag the overlay code never runs, so output stays clean frames.

---

## 13.1 Authoring a watchable shot

`assets/shots/showcase.json` is now **generated** by `tools/make_showcase.py`:
one continuous 90 s move with no hard cuts, outward from Earth (Earth ->
Saturn -> the Solar System -> Alpha Centauri -> the Lagoon nebula ->
Sagittarius A* -> the Milky Way face-on). Positions come from catalogue
coordinates in the simulator's frame; edit the generator, not the JSON. Its
header carries the render command (1080p30, 32 samples, letterbox, grain,
titles, `--no-orbits`, `soundtrack_1.ogg`).

Lessons from building it, which are not obvious from the format and cost
several renders each:

- **One origin per continuous move.** A draft placed the near keys relative to
  the Sun and the far keys relative to the galactic centre; the camera
  teleported from 22 AU off the Sun to 63 light years from Sgr A* — inside the
  core — and the frame blew out to white. The whiteout looked like a renderer
  bug and was an authoring bug.
- **Space far keys geometrically** (each ~20x the last). Catmull-Rom takes a
  key's tangent from its neighbours, so adjacent segments of wildly different
  length make the spline overshoot.
- **Rising along the galactic pole from the Sun arrives face-on to the disc**
  with no separate move. The Sun's 26 kly offset from the centre stops
  mattering billions of AU out. The pole is RA 192.859, Dec 27.128, converted
  with the same `equatorial_to_gl()` as `galaxy.c`.
- **The galactic interior is a featureless haze.** Compress the transit through
  it and hold the reveal; the eye wants to rest on the payoff, not the transit.
- **Drop timescale to 1 once past the system.** Nothing visibly animates at
  galactic scale, and sim time is the dominant render cost under motion blur.
- **`--relativistic 0` for film work.** The warp aberration is a flourish for
  flying the app by hand; on a fast pull-back it saturates and smears the
  frame. (Now automatic: it is off whenever a shot drives the camera.)
- **Never turn toward something just off frame.** A turn that swings a bright
  star past the frame edge leaves a moment where its glare has emptied the sky
  but the star itself is not yet in view: a black frame. Leave Saturn by
  finishing its orbit on the backlit side and pulling straight back along the
  Sun line instead: no turn, and the Sun stays in frame throughout.
- **Looking back is looking sunward.** Leaving the inner system outward and
  looking back at a planet shows its night side against the Sun's glare.
- **Black holes need the camera within a few AU.** Sgr A*'s horizon is
  ~0.08 AU; from 60 AU it is two pixels. At ~2.5 AU the shadow, photon ring,
  lensed disc and the swirl of lensed galactic-centre stars fill the frame.
- **Aim transits by direction, not by name.** A transit key with `look_at`
  would raise the destination's title card while it is still a dot; aim with
  yaw/pitch (the generator's `look_pos`) and name the arrival key instead.
- **Nebulae are soft volumes.** The Lagoon reads as a pink cloud at any
  distance; keep its pass short and close (2.2 -> 1.5 radii), where its cavity
  and the galactic band behind it carry the frame.
- **Earth's face is predictable, so aim at a place.** The spin after warm-up is
  exactly `2π · (warm-up days + timescale · t) / 0.99727 d` (the universe sets
  no initial angle). The generator's `earth_site(lat, lon, t)` inverts
  `phong.frag`'s texture mapping to put the opening camera over Europe. This
  only held once the film stopped stepping its first frame at the universe's
  default 1 day/s: the shot is now evaluated before the sim step, so every
  frame advances at its own key's timescale.

---

## 14. Verification

There is no unit-test suite; correctness is verified by running it. For this
feature specifically:

- **Every phase ends with a headless film-out** that is actually viewed. A still
  frame is not sufficient evidence for a feature whose entire point is motion.
- **Determinism check:** render the same command twice and compare output hashes
  (see §15 for the caveat).
- **Scale check:** film a shot that crosses from planet surface to Local Group in
  one continuous move. This is the precision-invariant regression test — jitter
  or z-fighting shows up immediately and nowhere else.
- **`make IMGUI=0` must still build and film.** The CLI/JSON path cannot depend
  on the menu. Remember `make clean` when toggling `IMGUI`.
- Builds stay warning-clean under `-Wall -Wextra`.

---

## 15. Risks and open questions

- **Bit-exact determinism is better than expected, but still not guaranteed.**
  Measured in phase 1: two identical `--cinematic --output` runs produced
  byte-identical MP4s (same MD5). The caveat stands anyway — OpenMP reduction
  ordering in the parallel integration path is not *specified* to be stable, and
  GPU driver differences vary across machines, so the contract to rely on
  remains *reproducible on the same machine, visually identical across
  machines*. Do not promise hash-identity to a user; it is an observation, not
  a guarantee. Making it one would need a deterministic reduction order on the
  cold path, which is a separate change.
- **4K × 64-sample film-out is slow** — measured at ~18 s/frame, i.e. ~3.6 hours
  for 30 seconds. `--draft` and the 1080p default mitigate, and the progress
  line prints a live ETA. An up-front estimate before the first frame is still
  worth adding so a long run is a choice rather than a surprise.
- **Motion blur re-runs the sim N× per frame. Now automatic camera-only blur**
  (`src/render/cinema_blur.c`). The camera's own blur never needed the sim
  sliced, since each sample re-poses the camera on the shutter anyway. So per
  frame the renderer estimates how far any object moves on screen *relative to
  what the camera rides* (the current key's anchor) while the shutter is open:
  bodies in or near view, ringed bodies (orbital speed at the inner edge), and
  sampled asteroid-belt particles. If nothing would move more than half a
  pixel, the sim advances once and only the camera blurs; otherwise the full
  sliced blur runs. No setting. Measured on the 120 s known-universe tour at
  8 samples: 624 of 960 frames went camera-only, 200 s → 141 s (30% faster),
  and those frames were pixel-identical to full blur. Frames that differ are
  the full-blur ones, by sub-pixel drift, because advancing the integrator in a
  different number of steps shifts moons slightly. Film-out prints the split
  (`motion blur: N/M frames needed object blur`).
- **Per-sample CPU cost was the real bottleneck, not shading.** With the
  catalogues, `known_universe_bodies.bin` adds ~2.1e5 real bodies to the
  renderer's dynamic set, and every accumulation sample walked all of them.
  GPU timestamp queries on the Earth opening showed ~0.7 ms of sphere shading
  per sample against ~80 ms of CPU; halving the resolution barely changed the
  frame time. Three fixes, all exact:
  - the eclipse-occluder scan (`render.c`) and `nearest_star_to_body`
    (`collision.c`, reached twice per drawn sphere via the star-heat glow)
    scanned the universe per drawn sphere per sample; their results are now
    kept for the output frame (keyed on `g_render_time`, re-validated if a
    cached body died);
  - the frame's first sample records which dynamic bodies any pass can still
    draw. Later samples skip the rest, which are past the far-field horizon and
    sub-pixel. The reuse holds while camera travel plus the farthest a dropped
    body can have moved stays within 1% of the horizon, so nothing can cross it
    unseen.

  Verified over the whole 90 s showcase (2700 frames, 960x540, 8 samples, grain
  and titles): a 64-bit hash of every frame's raw RGB readback matched with and
  without the fixes, frame for frame. Speed: 2.14 → 4.81 fps there. At 1080p
  with 32 samples, 0.33 → 1.48 fps on the Earth opening, and 142 s → 77 s on a
  4 s transit.
- **Adaptive sample counts were tried and dropped.** The idea was to size each
  frame's samples to its longest streak (one per pixel), its largest blur
  disc, and an antialiasing floor. On the showcase the real frames still
  averaged 31.8 of 32: orbiting stars and binaries smear far enough under
  object blur, and f/2.8 background discs are ~20 px wide. The saving did not
  cover the cost of estimating it.
- **Accumulating in RGBA32F at 4K** is 132 MB for the accumulator alone, plus
  the scene target and bloom chain. Fine on a desktop GPU, worth watching.
- ~~**Auto-exposure interacts badly with accumulation.**~~ **Fixed in phase 2.**
  `post_set_autoexposure_hold()` freezes the adaptation after the first
  sub-frame, so one output frame renders at one exposure and the adaptation
  rate stays per-frame as it always was.
- **The `--output` extension → codec mapping** needs a sane default for unknown
  extensions (fall back to H.264 in MP4 and warn).
- **A film-out run never writes `settings.json`.** Its CLI look overrides and
  the film-only quality forcings (`dot_hide_px`) are properties of that one
  render, not preferences to remember. Live `--cinematic` persists normally.
- **A high timescale is not automatically watchable.** Two separate limits bite
  before the integrator does. A body crossing many pixels per frame strobes, and
  accumulation renders that strobe as N discrete copies — "double vision" —
  rather than a streak, so the blur wants `--samples` of the same order as the
  pixel displacement across the open shutter. And motion blur re-runs the
  integrator once per sample, so fast sim time is also the dominant render cost.
  The practical rules: raise `--samples` before raising timescale, and close the
  shutter (keyframed, §9.2) on hyper-fast legs where no sample count would be
  enough.
- **DOF's aim-based focal plane is paraxially exact, not exact.** The residual
  grows toward the frame corners and with FOV (~1 px against a 15 px blur at
  30°). At very wide FOV with a very shallow aperture, the frame edges will be
  fractionally softer than the centre at the focal plane.

---

## 16. References

Techniques informing §4 and §8:

- Jittered accumulation / progressive sampling as the unified route to AA, DOF
  and motion blur — the standard offline-renderer model (Cook-style distributed
  ray tracing; Maxon Physical Renderer's progressive sampler is a familiar
  modern expression of it).
- 180° shutter angle as the default cinematic motion-blur convention.
- James et al., *Gravitational Lensing by Spinning Black Holes in Astrophysics,
  and in the Movie Interstellar*, CQG 32 065001 (2015) —
  <https://arxiv.org/abs/1502.03808>. The DNGR renderer integrated *ray bundles*
  rather than single rays specifically to get IMAX-quality smoothness without
  flicker; that is the same argument as §4.2 for accumulation over
  post-process approximation. Also the source for treating Doppler shift,
  gravitational redshift and relativistic aberration as camera-side effects.
- *Real-time High-Quality Rendering of Non-Rotating Black Holes* —
  <https://arxiv.org/abs/2010.08735>.
- SpaceEngine's HDR/auto-exposure/bloom notes —
  <https://spaceengine.org/news/blog170312/> — and Universe Sandbox's 2024
  graphics rework on temperature-driven emission and localised (not uniform)
  bloom — <https://universesandbox.com/blog/2024/11/next-gen-graphics-update/>.
  Both confirm the existing `post.c` approach; neither suggests a change.
- Henyey-Greenstein phase functions for directional scattering in volumetric
  media — relevant only if `nebula.frag`/`galaxy.frag` ever gain real light
  scattering, which §8.5 puts out of scope.
