/*
 * cinema_tour.h — procedural tour + auto-director (docs/CINEMATIC.md §10).
 * Phase 4 of that plan.
 *
 * This is what `--cinematic --output film.mp4` does when no --shot-script is
 * given: point it at a universe and get a watchable film with zero authoring.
 *
 * ---- two halves ------------------------------------------------------------
 *   TOUR      Surveys what is actually in the loaded universe — curated bodies,
 *             stars with planets, black holes, nebulae, galaxies — scores each
 *             subject by interest and visual scale, orders them into a route
 *             that reads as a journey (close -> out -> across -> in) and
 *             composes each leg from a small vocabulary of moves.
 *   DIRECTOR  Watches the simulation's own event log and cuts away when
 *             something worth seeing happens (a supernova, a merger, a tidal
 *             disruption), covers it, then returns to the tour where it left
 *             off.
 *
 * The tour is the spine and the director interrupts it. The tour alone
 * guarantees footage in a quiet universe; the director alone gives
 * unpredictable runtime and dead air.
 *
 * ---- it generates a normal shot --------------------------------------------
 * Both halves emit CineKey[] and hand it to cinema_cam via cinema_shot_set().
 * Nothing here re-implements interpolation: anchors, look_at, Catmull-Rom,
 * easing and the keyframed fov/aperture/focus/shutter/timescale tracks all come
 * from §9.2 unchanged. Two things follow from that and both are deliberate:
 * a generated tour can be written out with cinema_shot_save() and then
 * hand-tuned, and any improvement to the interpolation improves the tour too.
 *
 * ---- determinism -----------------------------------------------------------
 * Subject scoring and ordering read only the loaded universe, and the one place
 * that wants variety (orbit phase) uses a fixed-seed hash of the subject rather
 * than rand(). The same universe therefore always produces the same tour, which
 * is what §5 requires of everything in the film-out path.
 */
#pragma once
#include "common.h"
#include "cinema_cam.h"

/* Select which halves run. Both default on; --tour and --director isolate one.
 * Call before cinema_tour_build(). */
void cinema_tour_set_mode(int tour_on, int director_on);

/* Survey the loaded universe and build a `duration`-second tour, installing it
 * as the active shot and starting playback. Returns 0 when the universe holds
 * nothing worth filming (in which case the caller should fall back to a static
 * camera rather than produce a black film). */
int  cinema_tour_build(double duration);

/* 1 once cinema_tour_build() has succeeded. */
int  cinema_tour_active(void);

/* Advance the tour by `dt` seconds and give the director its chance to cut
 * away. Owns the shot clock while active — the caller must not also drive
 * cinema_shot_set_time(). Call once per rendered frame, after the simulation
 * has advanced (so this frame's events are visible to the director). */
void cinema_tour_tick(double dt);

/* What the tour is currently showing, for the film's own logging and (phase 5)
 * for title cards. Returns "" before the first leg. */
const char *cinema_tour_subject(void);

/* What the tour is presenting now: the current leg's subject, or the event a
 * director cutaway covers (docs/CINEMATIC.md §11.1). 0 if none. */
int cinema_tour_current_subject(CineSubject *out);

void cinema_tour_shutdown(void);
