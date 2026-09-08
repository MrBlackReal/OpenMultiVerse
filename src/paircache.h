/*
 * paircache.h — sparse symmetric (body,body) -> cooldown map.
 *
 * collision.c needs to remember "these two bodies were just resolved, don't
 * re-resolve them for a while" for every pair that has recently touched. The
 * obvious table is [N][N], which is what this replaces: at the ~16k bodies a
 * galaxy-scale universe carries, a dense pair table is 2 GB of doubles that is
 * very nearly all zero, because the number of pairs in cooldown at any instant
 * is tiny — a handful, even during a busy merge.
 *
 * So the state is stored sparsely: an open-addressed hash map keyed on the
 * unordered pair, holding an absolute expiry time. Memory is O(pairs actually
 * in cooldown), not O(bodies^2), and lookup stays O(1).
 *
 * Time base: entries store an ABSOLUTE expiry on whatever clock the caller
 * passes as `now` (collision.c accumulates simulated seconds). The dense table
 * this replaces decremented a pair's cooldown only on the frames it happened to
 * be visited, so an unvisited pair kept its cooldown indefinitely — the
 * cooldown counted visits rather than time. Absolute expiry makes it elapse in
 * simulated time as intended; in the common case (a pair visited every frame
 * while its system is dirty) the two behave identically.
 *
 * Keys are body indices. Indices are stable for a body's lifetime but ARE
 * reused when a dead slot is refilled, so paircache_forget_body() must be
 * called from collision_on_body_added() — the same contract the dense table
 * had, where the row and column were cleared on reuse.
 *
 * Not thread-safe; collision_step is single-threaded.
 */
#pragma once

/* Drop every entry. */
void paircache_reset(void);

/* Seconds of cooldown left on the unordered pair (a,b) at time `now`, or 0 if
 * the pair is unknown or already expired. Order of a and b is irrelevant. */
double paircache_remaining(int a, int b, double now);

/* Block (a,b) until now + cooldown. Overwrites any existing entry, including
 * shortening one — matching the dense table's plain assignment. */
void paircache_arm(int a, int b, double now, double cooldown);

/* Reclaim entries that expired at or before `now`. Cheap (one pass over the
 * table) and optional for correctness — expired entries read as 0 either way —
 * but it keeps the table small enough that it never has to grow. */
void paircache_sweep(double now);

/* Drop every entry touching body `i`. Call when a body index is recycled. */
void paircache_forget_body(int i);

/* Live (unexpired at last sweep) entry count, and current capacity. For
 * diagnostics — a live count that climbs without bound is a leak. */
int paircache_live(void);
int paircache_capacity(void);
