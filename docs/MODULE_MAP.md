# Module Map — function → line index for the large source files

Navigation aid so edits can jump to a **window** (`Read` with `offset`/`limit`)
instead of loading a 2–4k-line file whole. Only the biggest files are indexed —
smaller ones are cheap to read in full. Line numbers drift as code changes; treat
them as *approximate anchors*, then confirm with a quick `grep -n '<name>'`.

**Regenerate after large edits** (from repo root):

```bash
for f in render/render sim/collision fx/rings sim/physics main core/universe ui/menu; do
  echo "== src/$f.c =="; grep -nE '^(static +)?[A-Za-z_][A-Za-z0-9_ *]*[ *][a-z_][A-Za-z0-9_]*\(' src/$f.c \
    | grep -vE ';\s*$' | grep -vE '\b(if|for|while|switch|return|sizeof|else)\b'
done
```

---

## src/render/render.c (~4340 ln) — scale-continuous renderer
| Line | Function | What |
|-----|----------|------|
| 655  | `clusters_render` | cluster/hybrid LOD aggregate draw |
| 882  | `body_lights` | per-body radiance contributions |
| 948  | `aurora_storm` | aurora noise/animation |
| 1263 | `bh_scales` | black-hole Schwarzschild/ISCO radii |
| 1284 | `visual_radius` | apparent draw radius for a body |
| 1316–1358 | `star_dot_apparent_mag` / `_pixel_size` / `_hdr_gain` | far-field star-dot photometry |
| 1535 | `render_init` | GL objects, VAOs, shaders |
| 1986 | `render_build_preview` | build-mode preview geometry |
| 2281 | `field_stars_ensure` | static Gaia field VBO (centroid-relative) |
| 2349 | `render_frame` | **main per-frame draw** (huge) |
| 4276 | `render_shutdown` | teardown |

## src/sim/collision.c (~3335 ln) — merges, craters, BH tidal disruption
| Line | Function | What |
|-----|----------|------|
| 509  | `collision_reset` | reset collision state |
| 547  | `collision_on_body_added` | register new body |
| 582  | `collision_system_maybe_has_encounter` | broad-phase per system |
| 1424 | `spawn_impact_particles` | impact debris |
| 1078 | `add_permanent_crater` | bake crater scar |
| 1827 | `absorb_body_into_star` | body → star merge |
| 1865–1887 | `tidal_radius` / `bh_tidal_pass` | black-hole tidal disruption |
| 2031 | `update_merge_events` | advance active merges |
| 2282 | `begin_merge_event` | start a merge animation |
| 2465 | `classify_collision` | merge vs graze vs bounce |
| 2705 | `absorb_body` | generic absorb |
| 2762 | `collision_step_system` | per-root serial resolve (hot path) |
| 2847 | `collision_step` | **collision entry point** |
| 3175 | `collision_body_heat_glow` | render hook: heat color |
| 3294 | `collision_particles` | render hook: particle export |

## src/fx/rings.c (~2196 ln) — ring particle discs
| Line | Function | What |
|-----|----------|------|
| 189–190 | `s_seed` / `s_randf` | **damage RNG — serial collision path only, never cold path** |
| 633  | `disc_update_response` | ring perturbation response |
| 757  | `bake_particles` | generate disc particles |
| 942  | `apply_disc_damage` | carve damage into a disc |
| 1199 | `apply_disc_tidal_gravity` | tidal shear on particles |
| 1595 | `render_disc` | draw one disc |
| 1796 | `rings_init` | load/init discs |
| 1913 | `rings_step_system` | per-system physics (cold path) |
| 1969 | `rings_tick` | global tick |
| 2040 | `rings_render` | draw all discs |
| 2054 | `rings_on_collision` | collision hook |

## src/sim/physics.c (~1937 ln) — RESPA integrator + trails
| Line | Function | What |
|-----|----------|------|
| 397  | `refresh_system_timesteps` | adaptive dt per system |
| 479  | `physics_refresh_timestep_model` | rebuild timestep model |
| 679  | `physics_active_systems` | camera-proximity active set |
| 856  | `add_cosmological_acc` | lambda term |
| 891  | `add_relativistic_acc` | PN correction |
| 952  | `compute_acc_slow_system` | slow (star↔planet) forces |
| 1078 | `compute_acc_fast_system` | fast (moon↔parent) forces |
| 1164 | `physics_respa_begin` / `_inner` / `_end` (+ `_system` variants) | RESPA outer/inner steps |
| 1360 | `physics_step` | **integrator entry point** |
| 1630 | `trail_rebuild_segment` | trail curve rebuild |
| 1767 | `trails_cut_body_at_time` | truncate trail on collision |
| 1855 | `trails_tick` | advance trails |

## src/main.c (~2542 ln) — loop, integration dispatch, headless
| Line | Function | What |
|-----|----------|------|
| 313  | `warmup_universe` | pre-roll on load |
| 413  | `init_runtime_world` | build runtime state |
| 520  | `switch_universe` | swap active universe |
| 562  | `app_init` | SDL/GL/audio init |
| 699  | `handle_event` | input dispatch |
| 1050 | `camera_move` | camera integration |
| 1138 | `save_screenshot_ppm` | `--shot` writer (PPM) |
| 1176 | `system_step_schedule` | outer/inner step counts |
| 1199–1223 | `integrate_system_cold` / `_hot` | **cold (OpenMP) vs hot (serial) dispatch** |
| 1466 | `print_usage` | CLI flags |
| 1549 | `main` | entry, arg parse, main loop |

## src/core/universe.c (~1755 ln) — loader, add-body, catalogs
| Line | Function | What |
|-----|----------|------|
| 257  | `dedupe_body_names` | unique-name pass |
| 451  | `load_snapshot` | save/load restore |
| 603  | `universe_save` | write snapshot JSON |
| 723  | `universe_export_body_catalog` | write BodyBin (`--export-body-catalog`) |
| 845  | `load_star_catalog` | StarBin far-field stars |
| 1037 | `load_body_catalog` | BodyBin bulk interactive bodies |
| 1123 | `universe_load` | **universe entry point** (JSON + catalogs) |
| 1627 | `universe_add_body` | runtime add / dead-slot reuse |
| 1712 | `universe_rebind_to_nearest_stars` | re-parent after load |

## src/ui/menu.c (~1822 ln) — Dear ImGui menu (U key; stubs when IMGUI=0)
| Line | Function | What |
|-----|----------|------|
| 114  | `menu_init` | ImGui context + SDL/GL backends |
| 163  | `menu_process_event` | route SDL events to ImGui |
| 207  | `teleport_to_body` | Navigate: fly camera to a body (also nebula/galaxy variants) |
| 346  | `menu_render_navigate` | Navigate tab: searchable body/nebula/galaxy list |
| 509  | `menu_render_inspect` | Inspect tab: selected-body readout, lifecycle triggers |
| 728  | `menu_render_profiler` | Profiler tab (`--profile`) |
| 914  | `menu_render_settings` | Settings tab (visuals, cinematic, orbit prediction) |
| 1354 | `menu_render` | **menu entry point**; draws the Universe tab inline (presets, law sliders, import, save/load) |
