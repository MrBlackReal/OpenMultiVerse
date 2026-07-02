/*
 * field_graph.h — universe field graph over bodies, fields, and events.
 *
 * This is the roadmap Phase A #5 foundation (§0.4 Universe field graph): one
 * queryable graph linking the engine's object classes, so relations that so
 * far live implicitly in scattered state (who orbits whom, who feeds mass to
 * whom, what exploded into what) become an explicit, inspectable structure.
 * It is the backbone later work attaches to: orbit prediction reads gravity
 * edges, timeline scrubbing replays the event log, galaxy formation adds
 * field nodes.
 *
 * Nodes are the existing entities themselves — discrete bodies (g_bodies
 * indices) and continuous fields (nebulae, counted but edge-less in this
 * iteration).  Edges are typed couplings harvested from live sim state on an
 * O(N) rebuild (same throttle pattern as cosmic_field/radiance_field):
 *
 *   GRAVITY   child → parent, from Body.parent — the orbital hierarchy.
 *   GAS_FLOW  donor → hole: Roche-lobe overflow streams (accretion.c, with a
 *             live transfer rate) and tidal-disruption streams (collision.c
 *             tidal_frac/tidal_hole).
 *
 * RADIATION edges are intentionally NOT stored: at ~16k bodies an
 * all-pairs body×emitter table is unaffordable and almost never read.  They
 * are computed lazily per query via radiance_field_top() — the field already
 * answers "which emitters win here" in O(emitters).
 *
 * Evolution/event transitions are a fixed ring buffer of FieldGraphEvent fed
 * by explicit notify calls from lifecycle.c (phase changes), supernova.c
 * (detonation → remnant), and collision.c (merges, tidal disruptions).  Body
 * slots are reused after death (g_nbodies is a high-water count), so every
 * event snapshots the participants' names at log time; per-body history
 * queries match index + name and thus drop — never misattribute — events
 * from a slot's previous tenant.
 *
 * Threading: build, notify, and query are MAIN-THREAD ONLY (same contract as
 * cosmic_field.h / radiance_field.h) — never call from inside the physics
 * OpenMP warmup.
 */
#pragma once
#include "common.h"
#include "radiance_field.h"   /* RadianceContrib for the lazy radiation query */

/* ── stored edges ─────────────────────────────────────────────────────────── */

typedef enum {
    FG_EDGE_GRAVITY = 0,   /* from = child, to = parent (Body.parent)        */
    FG_EDGE_GAS_FLOW       /* from = donor/victim, to = accreting black hole */
} FieldGraphEdgeType;

/* GAS_FLOW flavours (FieldGraphEdge.flow_kind). */
enum { FG_FLOW_ROCHE = 0, FG_FLOW_TIDAL = 1 };

typedef struct {
    short  type;           /* FieldGraphEdgeType                             */
    short  flow_kind;      /* GAS_FLOW only: FG_FLOW_ROCHE / FG_FLOW_TIDAL   */
    int    from, to;       /* g_bodies indices (alive at rebuild time)       */
    double rate_kg_s;      /* Roche transfer rate; 0 = unknown (tidal)       */
} FieldGraphEdge;

/* ── event log ────────────────────────────────────────────────────────────── */

typedef enum {
    FG_EVENT_SUPERNOVA = 0, /* a = progenitor, b = remnant, detail = remnant
                             * StarPhase (neutron star / BH / white dwarf)   */
    FG_EVENT_PHASE,         /* a = star, detail = new StarPhase              */
    FG_EVENT_MERGE,         /* a = survivor, b = absorbed body               */
    FG_EVENT_TDE            /* a = black hole, b = tidally consumed victim   */
} FieldGraphEventType;

typedef struct {
    int    type;            /* FieldGraphEventType                           */
    int    a, b;            /* body indices AT EVENT TIME (slots get reused);
                             * b = -1 when the event has one participant     */
    char   a_name[32];      /* name snapshots — sized to Body.name           */
    char   b_name[32];      /* "" when b < 0                                 */
    int    detail;          /* StarPhase for SUPERNOVA/PHASE, else 0         */
    double sim_time_s;      /* g_sim_time snapshot (orbital clock)           */
    double age_yr;          /* subject a's stellar age snapshot (0 if n/a)   */
    double pos[3];          /* where it happened, SI m                       */
} FieldGraphEvent;

/* ── aggregate stats (headless print / HUD) ───────────────────────────────── */

typedef struct {
    int nodes;              /* alive bodies + nebula/galaxy field nodes      */
    int stars, planets, black_holes, nebulae, galaxies;
    int edges, grav_edges, flow_edges;
    int events_logged;      /* total ever logged (ring keeps the newest)     */
} FieldGraphStats;

/* ── lifecycle ────────────────────────────────────────────────────────────── */

/* One-time init (zeroes state); call once at boot before the first rebuild. */
void field_graph_init(void);
void field_graph_shutdown(void);

/* Clear edges + the event log (universe reload — history belongs to the old
 * universe). The next tick/rebuild re-harvests edges from the new one. */
void field_graph_reset(void);

/* (Re)harvest edges + stats from the current g_bodies / accretion snapshot.
 * O(N). Call after a universe load and whenever the body set changed. */
void field_graph_rebuild(void);

/* Per-frame maintenance (real frame dt, seconds): rebuilds immediately when
 * the body high-water count changes, else on a time throttle. Cheap when it
 * does nothing. */
void field_graph_tick(double dt);

/* ── queries ──────────────────────────────────────────────────────────────── */

void field_graph_stats(FieldGraphStats *out);

/* Stored edges incident to `body` (either endpoint). Returns count written. */
int  field_graph_body_edges(int body, FieldGraphEdge *out, int max);

/* Lazy RADIATION edges: the k strongest emitters at `body`'s live position,
 * excluding its own emission. Thin wrapper over radiance_field_top(); an
 * entry's body is -1 for a transient source (supernova flash). */
int  field_graph_radiation_top(int body, int k, RadianceContrib *out);

/* Event log, newest first. body_events filters to events where `body` was a
 * participant AND the name snapshot still matches (slot-reuse guard). */
int  field_graph_events(FieldGraphEvent *out, int max);
int  field_graph_body_events(int body, FieldGraphEvent *out, int max);

/* Human-readable event-type name ("supernova", "phase", "merge", "tde"). */
const char *field_graph_event_name(int type);

/* ── notify hooks (MAIN THREAD ONLY) ──────────────────────────────────────────
 * Called by supernova.c / lifecycle.c / collision.c at the moment a
 * transition happens. Names/positions/clocks are snapshotted immediately and
 * one grep-able "[FieldGraph] event=..." line goes to stdout (events are
 * rare, and the print is what makes them verifiable headless). */
void field_graph_notify_supernova(int progenitor, int remnant);
void field_graph_notify_phase(int star, int new_phase);
void field_graph_notify_merge(int survivor, int absorbed);
void field_graph_notify_tde(int hole, int victim);
