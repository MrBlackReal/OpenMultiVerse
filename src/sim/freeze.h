/*
 * freeze.h — systems outside the active region keep their shape and their clock.
 *
 * Only systems within the active radius are integrated; the rest sit still.
 * That used to cost two things. A frozen system never aged: fly away for a
 * year and come back, and every planet was where you left it. And its members
 * rode every floating-origin rebase (frame.h) as absolute local positions, so
 * far from home the rebases rounded them to the local spacing of doubles:
 * seen from M87 that is 67,000 km, and Earth's orbit would not survive the
 * trip back.
 *
 * So when a system leaves the active set, each member's state is recorded
 * relative to its parent, where it is exact whatever the distance, along
 * with the sim time. When it comes back, every member is placed again from its
 * parent by kepler_propagate() over the gap, and spun forward. Perturbations
 * between members are ignored while frozen, the same approximation starsys
 * deltas make. Single-body systems need no record: nothing is relative.
 */
#pragma once

/* Once per frame, paused or not, with the active system slots
 * (physics_active_systems). Thaws systems that came back and freezes those
 * that left; after a body-set change, also freezes every inactive system
 * that has no record yet. */
void freeze_update(const int *active_slots, int n);

/* Drop every record (universe load). */
void freeze_reset(void);

/* Systems currently frozen with a record. */
int  freeze_count(void);

/* Sol round trip to M87 and back (--selftest-frame). Returns 1 on pass. */
int  freeze_selftest(void);
