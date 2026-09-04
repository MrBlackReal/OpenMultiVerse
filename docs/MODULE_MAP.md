# Module Map — function → line index for the large source files

Navigation aid so edits can jump to a **window** (`Read` with `offset`/`limit`)
instead of loading a 2–4k-line file whole. Only the biggest files are indexed —
smaller ones are cheap to read in full. Line numbers drift as code changes; treat
them as *approximate anchors*, then confirm with a quick `grep -n '<name>'`.

**Regenerate after large edits** (from repo root):

```bash
for f in render collision rings physics main universe; do
  echo "== src/$f.c =="; grep -nE '^(static +)?[A-Za-z_][A-Za-z0-9_ *]*[ *][a-z_][A-Za-z0-9_]*\(' src/$f.c \
    | grep -vE ';\s*$' | grep -vE '\b(if|for|while|switch|return|sizeof|else)\b'
done
```

---

## src/render.c (~3807 ln) — scale-continuous renderer
| Line | Function | What |
|-----|----------|------|
| 441  | `clusters_render` | cluster/hybrid LOD aggregate draw |
| 553  | `body_lights` | per-body radiance contributions |
| 619  | `aurora_storm` | aurora noise/animation |
| 934  | `bh_scales` | black-hole Schwarzschild/ISCO radii |
| 955  | `visual_radius` | apparent draw radius for a body |
| 987–1054 | `star_dot_apparent_mag` / `_pixel_size` / `_hdr_gain` | far-field star-dot photometry |
| 1200 | `render_init` | GL objects, VAOs, shaders |
| 1650 | `render_build_preview` | build-mode preview geometry |
| 1945 | `field_stars_ensure` | static Gaia field VBO (centroid-relative) |
| 2009 | `render_frame` | **main per-frame draw** (huge) |
| 3749 | `render_shutdown` | teardown |

## src/collision.c (~2889 ln) — merges, craters, BH tidal disruption
| Line | Function | What |
|-----|----------|------|
| 175  | `collision_reset` | reset collision state |
| 207  | `collision_on_body_added` | register new body |
| 242  | `collision_system_maybe_has_encounter` | broad-phase per system |
| 1044 | `spawn_impact_particles` | impact debris |
| 1306 | `add_permanent_crater` | bake crater scar |
| 1443 | `absorb_body_into_star` | body → star merge |
| 1481–1503 | `tidal_radius` / `bh_tidal_pass` | black-hole tidal disruption |
| 1647 | `update_merge_events` | advance active merges |
| 1898 | `begin_merge_event` | start a merge animation |
| 2081 | `classify_collision` | merge vs graze vs bounce |
| 2321 | `absorb_body` | generic absorb |
| 2378 | `collision_step_system` | per-root serial resolve (hot path) |
| 2446 | `collision_step` | **collision entry point** |
| 2732 | `collision_body_heat_glow` | render hook: heat color |
| 2849 | `collision_particles` | render hook: particle export |

## src/rings.c (~2193 ln) — ring particle discs
| Line | Function | What |
|-----|----------|------|
| 187–188 | `s_seed` / `s_randf` | **damage RNG — serial collision path only, never cold path** |
| 631  | `disc_update_response` | ring perturbation response |
| 755  | `bake_particles` | generate disc particles |
| 940  | `apply_disc_damage` | carve damage into a disc |
| 1197 | `apply_disc_tidal_gravity` | tidal shear on particles |
| 1593 | `render_disc` | draw one disc |
| 1794 | `rings_init` | load/init discs |
| 1911 | `rings_step_system` | per-system physics (cold path) |
| 1967 | `rings_tick` | global tick |
| 2038 | `rings_render` | draw all discs |
| 2052 | `rings_on_collision` | collision hook |

## src/physics.c (~1931 ln) — RESPA integrator + trails
| Line | Function | What |
|-----|----------|------|
| 396  | `refresh_system_timesteps` | adaptive dt per system |
| 478  | `physics_refresh_timestep_model` | rebuild timestep model |
| 678  | `physics_active_systems` | camera-proximity active set |
| 855  | `add_cosmological_acc` | lambda term |
| 890  | `add_relativistic_acc` | PN correction |
| 951  | `compute_acc_slow_system` | slow (star↔planet) forces |
| 1077 | `compute_acc_fast_system` | fast (moon↔parent) forces |
| 1163–1289 | `physics_respa_begin` / `_inner` / `_end` (+ `_system` variants) | RESPA outer/inner steps |
| 1359 | `physics_step` | **integrator entry point** |
| 1629 | `trail_rebuild_segment` | trail curve rebuild |
| 1764 | `trails_cut_body_at_time` | truncate trail on collision |
| 1851 | `trails_tick` | advance trails |

## src/main.c (~1857 ln) — loop, integration dispatch, headless
| Line | Function | What |
|-----|----------|------|
| 270  | `warmup_universe` | pre-roll on load |
| 370  | `init_runtime_world` | build runtime state |
| 477  | `switch_universe` | swap active universe |
| 519  | `app_init` | SDL/GL/audio init |
| 645  | `handle_event` | input dispatch |
| 989  | `camera_move` | camera integration |
| 1077 | `save_screenshot_ppm` | `--shot` writer (PPM) |
| 1115 | `system_step_schedule` | outer/inner step counts |
| 1138–1162 | `integrate_system_cold` / `_hot` | **cold (OpenMP) vs hot (serial) dispatch** |
| 1192 | `print_usage` | CLI flags |
| 1228 | `main` | entry, arg parse, main loop |

## src/universe.c (~1697 ln) — loader, add-body, catalogs
| Line | Function | What |
|-----|----------|------|
| 257  | `dedupe_body_names` | unique-name pass |
| 451  | `load_snapshot` | save/load restore |
| 603  | `universe_save` | write snapshot JSON |
| 723  | `universe_export_body_catalog` | write BodyBin (`--export-body-catalog`) |
| 824  | `load_star_catalog` | StarBin far-field stars |
| 980  | `load_body_catalog` | BodyBin bulk interactive bodies |
| 1066 | `universe_load` | **universe entry point** (JSON + catalogs) |
| 1570 | `universe_add_body` | runtime add / dead-slot reuse |
| 1655 | `universe_rebind_to_nearest_stars` | re-parent after load |
