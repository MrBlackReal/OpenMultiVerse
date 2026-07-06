/*
 * field_graph.c — universe field graph (roadmap Phase A #5). See field_graph.h
 * for the design contract. Plain CPU state: no GL, no SDL, no ImGui.
 */
#include "field_graph.h"
#include "body.h"
#include "universe.h"   /* g_field_star_begin/end */
#include "physics.h"     /* g_sim_time                       */
#include "lifecycle.h"   /* lifecycle_phase_name for prints  */
#include "accretion.h"   /* accretion_flows (Roche streams)  */
#include "nebula.h"      /* nebula_count (field nodes)       */
#include "galaxy.h"      /* galaxy_count (field nodes)       */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rebuild throttle (matches cosmic_field). */
#define FG_REBUILD_SEC 0.25

/* Event ring: newest FG_LOG_LEN events are retained. */
#define FG_LOG_LEN 256

/* ── state ────────────────────────────────────────────────────────────────── */

static FieldGraphEdge *s_edges = NULL;
static int             s_edge_count = 0;
static int             s_edge_cap   = 0;

static FieldGraphEvent s_log[FG_LOG_LEN];
static int             s_log_head  = 0;   /* next write slot        */
static int             s_log_total = 0;   /* total ever logged      */

static FieldGraphStats s_stats;

static double s_since_rebuild = 0.0;
static int    s_last_nbodies  = -1;

static FieldGraphEdge *edges_push(void)
{
    if (s_edge_count >= s_edge_cap) {
        int ncap = s_edge_cap > 0 ? s_edge_cap * 2 : 64;
        s_edges = (FieldGraphEdge *)realloc(s_edges,
                                            (size_t)ncap * sizeof(FieldGraphEdge));
        s_edge_cap = ncap;
    }
    return &s_edges[s_edge_count++];
}

static int body_valid_alive(int i)
{
    return i >= 0 && i < g_nbodies && g_bodies[i].alive;
}

/* ── lifecycle ────────────────────────────────────────────────────────────── */

void field_graph_init(void)
{
    s_edges = NULL; s_edge_count = 0; s_edge_cap = 0;
    memset(s_log, 0, sizeof(s_log));
    s_log_head = s_log_total = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_since_rebuild = 0.0;
    s_last_nbodies  = -1;
}

void field_graph_shutdown(void)
{
    free(s_edges);
    s_edges = NULL;
    s_edge_count = s_edge_cap = 0;
}

void field_graph_reset(void)
{
    s_edge_count = 0;
    memset(s_log, 0, sizeof(s_log));
    s_log_head = s_log_total = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_since_rebuild = 0.0;
    s_last_nbodies  = -1;   /* force a rebuild on the next tick */
}

void field_graph_rebuild(void)
{
    s_edge_count = 0;
    int events_logged = s_stats.events_logged;   /* survives rebuilds */
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.events_logged = events_logged;

    for (int i = 0; i < g_nbodies; i++) {
        /* Skip the bulk field-star range: 262k frozen catalogue points have no
         * meaningful relations (they are lone stars), so making them graph nodes
         * — each running a radiance query — was a top per-frame cost.  The
         * Relations view concerns the curated systems near the camera. */
        if (i >= g_field_star_begin && i < g_field_star_end) {
            i = g_field_star_end - 1;
            continue;
        }
        const Body *b = &g_bodies[i];
        if (!b->alive) continue;

        if (b->is_black_hole)   s_stats.black_holes++;
        else if (b->is_star)    s_stats.stars++;
        else                    s_stats.planets++;
        s_stats.nodes++;

        /* GRAVITY: the orbital hierarchy. */
        if (body_valid_alive(b->parent)) {
            FieldGraphEdge *e = edges_push();
            e->type      = FG_EDGE_GRAVITY;
            e->flow_kind = 0;
            e->from      = i;
            e->to        = b->parent;
            e->rate_kg_s = 0.0;
            s_stats.grav_edges++;
        }

        /* GAS_FLOW (tidal): a black hole is actively shredding this body. */
        if (b->tidal_frac > 0.0f && body_valid_alive(b->tidal_hole)) {
            FieldGraphEdge *e = edges_push();
            e->type      = FG_EDGE_GAS_FLOW;
            e->flow_kind = FG_FLOW_TIDAL;
            e->from      = i;
            e->to        = b->tidal_hole;
            e->rate_kg_s = 0.0;   /* art-directed spiral-in; no closed-form rate */
            s_stats.flow_edges++;
        }
    }

    /* GAS_FLOW (Roche): donor → hole streams observed by the accretion model
     * during its most recent step (empty while the stellar clock is 0). */
    {
        const AccretionFlow *fl;
        int n = accretion_flows(&fl);
        for (int k = 0; k < n; k++) {
            if (!body_valid_alive(fl[k].donor) ||
                !body_valid_alive(fl[k].hole)) continue;
            FieldGraphEdge *e = edges_push();
            e->type      = FG_EDGE_GAS_FLOW;
            e->flow_kind = FG_FLOW_ROCHE;
            e->from      = fl[k].donor;
            e->to        = fl[k].hole;
            e->rate_kg_s = fl[k].rate_kg_s;
            s_stats.flow_edges++;
        }
    }

    s_stats.edges    = s_edge_count;
    s_stats.nebulae  = nebula_count();  /* field nodes: counted, no edges yet */
    s_stats.galaxies = galaxy_count();
    s_stats.nodes   += s_stats.nebulae + s_stats.galaxies;

    s_since_rebuild = 0.0;
    s_last_nbodies  = g_nbodies;
}

void field_graph_tick(double dt)
{
    /* Accrue staleness only — the rebuild is now lazy, triggered by
     * field_graph_body_edges when the Relations panel (its sole per-frame
     * consumer) is actually open.  Previously this rebuilt the whole ~16k-node
     * graph 4x/sec (and on every body-set change) even when nothing displayed
     * it. */
    s_since_rebuild += dt;
}

/* ── queries ──────────────────────────────────────────────────────────────── */

void field_graph_stats(FieldGraphStats *out)
{
    *out = s_stats;
}

int field_graph_body_edges(int body, FieldGraphEdge *out, int max)
{
    if (body < 0 || max <= 0) return 0;
    /* Lazy rebuild: refresh the graph on demand (panel open) if the body set
     * changed or the staleness timer elapsed. */
    if (g_nbodies != s_last_nbodies || s_since_rebuild >= FG_REBUILD_SEC)
        field_graph_rebuild();
    int n = 0;
    for (int e = 0; e < s_edge_count && n < max; e++)
        if (s_edges[e].from == body || s_edges[e].to == body)
            out[n++] = s_edges[e];
    return n;
}

int field_graph_radiation_top(int body, int k, RadianceContrib *out)
{
    if (body < 0 || body >= g_nbodies || !g_bodies[body].alive) return 0;
    return radiance_field_top(g_bodies[body].pos, body, k, out);
}

int field_graph_events(FieldGraphEvent *out, int max)
{
    int avail = s_log_total < FG_LOG_LEN ? s_log_total : FG_LOG_LEN;
    int n = 0;
    for (int i = 0; i < avail && n < max; i++) {
        int slot = (s_log_head - 1 - i + FG_LOG_LEN) % FG_LOG_LEN;
        out[n++] = s_log[slot];
    }
    return n;
}

int field_graph_body_events(int body, FieldGraphEvent *out, int max)
{
    if (body < 0 || body >= g_nbodies) return 0;
    const char *name = g_bodies[body].name;
    int avail = s_log_total < FG_LOG_LEN ? s_log_total : FG_LOG_LEN;
    int n = 0;
    for (int i = 0; i < avail && n < max; i++) {
        int slot = (s_log_head - 1 - i + FG_LOG_LEN) % FG_LOG_LEN;
        const FieldGraphEvent *ev = &s_log[slot];
        /* Index + name must both match: a reused slot carries a new name, so
         * the previous tenant's events are dropped, never misattributed. */
        if ((ev->a == body && strncmp(ev->a_name, name, sizeof(ev->a_name)) == 0) ||
            (ev->b == body && strncmp(ev->b_name, name, sizeof(ev->b_name)) == 0))
            out[n++] = *ev;
    }
    return n;
}

const char *field_graph_event_name(int type)
{
    switch (type) {
        case FG_EVENT_SUPERNOVA: return "supernova";
        case FG_EVENT_PHASE:     return "phase";
        case FG_EVENT_MERGE:     return "merge";
        case FG_EVENT_TDE:       return "tde";
        default:                 return "unknown";
    }
}

/* ── notify hooks ─────────────────────────────────────────────────────────── */

/* Shared writer: snapshot names/clocks/position now (participants may be
 * dead or reused moments later) and print the grep-able event line. */
static void log_event(int type, int a, int b, int detail)
{
    FieldGraphEvent *ev = &s_log[s_log_head];
    memset(ev, 0, sizeof(*ev));
    ev->type       = type;
    ev->a          = a;
    ev->b          = b;
    ev->detail     = detail;
    ev->sim_time_s = g_sim_time;
    if (a >= 0 && a < g_nbodies) {
        snprintf(ev->a_name, sizeof(ev->a_name), "%s", g_bodies[a].name);
        ev->age_yr = g_bodies[a].age_yr;
        ev->pos[0] = g_bodies[a].pos[0];
        ev->pos[1] = g_bodies[a].pos[1];
        ev->pos[2] = g_bodies[a].pos[2];
    }
    if (b >= 0 && b < g_nbodies)
        snprintf(ev->b_name, sizeof(ev->b_name), "%s", g_bodies[b].name);

    s_log_head = (s_log_head + 1) % FG_LOG_LEN;
    s_log_total++;
    s_stats.events_logged++;

    if (type == FG_EVENT_SUPERNOVA || type == FG_EVENT_PHASE)
        fprintf(stdout,
                "[FieldGraph] event=%s a=\"%s\" b=\"%s\" detail=\"%s\" "
                "sim_t=%.4e s age=%.4e yr\n",
                field_graph_event_name(type), ev->a_name, ev->b_name,
                lifecycle_phase_name(detail), ev->sim_time_s, ev->age_yr);
    else
        fprintf(stdout,
                "[FieldGraph] event=%s a=\"%s\" b=\"%s\" sim_t=%.4e s\n",
                field_graph_event_name(type), ev->a_name, ev->b_name,
                ev->sim_time_s);
}

void field_graph_notify_supernova(int progenitor, int remnant)
{
    int detail = (remnant >= 0 && remnant < g_nbodies)
               ? g_bodies[remnant].star_phase : 0;
    log_event(FG_EVENT_SUPERNOVA, progenitor, remnant, detail);
}

void field_graph_notify_phase(int star, int new_phase)
{
    log_event(FG_EVENT_PHASE, star, -1, new_phase);
}

void field_graph_notify_merge(int survivor, int absorbed)
{
    log_event(FG_EVENT_MERGE, survivor, absorbed, 0);
}

void field_graph_notify_tde(int hole, int victim)
{
    log_event(FG_EVENT_TDE, hole, victim, 0);
}
