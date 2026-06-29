/*
 * universe.c — flat-body universe loader and runtime management
 *
 * JSON schema (top-level keys):
 *
 *   "bodies"         — flat array of all bodies (stars, planets, moons)
 *   "rings"          — ring-system descriptors (passed through unchanged)
 *   "asteroid_belts" — belt descriptors (passed through unchanged)
 *
 * Body placement rules by "type":
 *
 *   "star"                       — placed at absolute world position given by
 *                                  "pos_ly" ([x,y,z] in light-years from origin).
 *                                  Optional "velocity_km_s" sets bulk proper-motion
 *                                  velocity for the whole system (applied last).
 *
 *   "planet" / "dwarf_planet" /
 *   "asteroid"                   — Keplerian orbit around "parent" star.
 *                                  "parent" must name a star already loaded in Pass 1.
 *
 *   "moon"                       — parent-relative orbit around "parent" planet/moon
 *                                  using "moon_keplerian" elements (a in km, GM of parent).
 *
 * Three-pass load order (why passes matter):
 *   Pass 1 — stars first, so absolute world positions exist before any body tries
 *             to reference a star as its parent.
 *   Pass 2 — planets/dwarf_planets/asteroids: find_body_index() can locate the parent star
 *             because all stars are already in g_bodies[].
 *   Pass 3 — moons: parent planets are fully positioned so moon_to_state() receives
 *             the correct world-space parent GM and can add the parent offset.
 *
 * Post-processing per star (after all bodies loaded):
 *   1. Centre-of-mass velocity correction: the star's velocity is adjusted so the
 *      total linear momentum of the system is zero in the star's frame.  Only the
 *      star is adjusted because M_star >> M_planets, making the correction exact
 *      enough for long-term stability without touching every planet velocity.
 *   2. Apply bulk velocity (proper motion): "velocity_km_s" from the JSON is added
 *      uniformly to every body in the system, translating the whole system through
 *      world space at the correct stellar drift speed.
 *
 * Parent chain convention (Body.parent field):
 *   stars        — parent = -1   (root of the chain)
 *   planets      — parent = star body index
 *   moons        — parent = planet body index
 *   The root_star_of() helper walks this chain upward until parent == -1.
 */
#include "universe.h"
#include "body.h"
#include "json.h"
#include "common.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

/* ------------------------------------------------------------------ helpers */

/*
 * ensure_capacity — grow g_bodies[] to hold at least `needed` entries.
 *
 * Doubling strategy: capacity starts at MAX_BODIES, then doubles each time the
 * limit is reached.  This amortises realloc cost to O(1) per insertion.
 * The process exits on allocation failure — there is no graceful recovery path
 * since a partially-loaded universe is unusable.
 */
static void ensure_capacity(int needed)
{
    if (needed <= g_bodies_cap) return;
    int new_cap = g_bodies_cap ? g_bodies_cap * 2 : MAX_BODIES;
    while (new_cap < needed) new_cap *= 2;
    Body *p = (Body*)realloc(g_bodies, new_cap * sizeof(Body));
    if (!p) { fprintf(stderr, "[universe] out of memory\n"); exit(1); }
    g_bodies     = p;
    g_bodies_cap = new_cap;
}

/*
 * alloc_trail — allocate and zero-initialise all trail state for a body.
 *
 * Two parallel trail systems live side-by-side:
 *
 *   Simulation trail ("trail_*"):
 *     The authoritative, accumulated path.  TRAIL_LEN double[3] positions plus
 *     per-segment arc lengths.  Used by trails_render() for the visible ribbon.
 *
 *   Frame-snapshot trail ("trail_frame_*"):
 *     A snapshot of the trail state as it was at the start of the current
 *     physics frame.  The collision system uses this to roll back and re-emit
 *     the trail from its pre-impact state when two bodies merge, preventing
 *     a visual glitch where the trail suddenly jumps to the collision point.
 *
 * Both systems share the same circular-buffer structure (head, count, accum,
 * total_len) and a "prev" sample for Hermite spline tangent computation.
 * Initialising prev_pos/vel to the body's current position and velocity at
 * load time ensures the first trail segment has a valid tangent.
 */
static void alloc_trail(Body *bo)
{
    if (bo->trail)
        memset(bo->trail, 0, TRAIL_LEN * 3 * sizeof(double));
    else
        bo->trail = (double(*)[3])calloc(TRAIL_LEN, 3 * sizeof(double));
    if (bo->trail_seg_len)
        memset(bo->trail_seg_len, 0, TRAIL_LEN * sizeof(double));
    else
        bo->trail_seg_len = (double*)calloc(TRAIL_LEN, sizeof(double));
    if (!bo->trail || !bo->trail_seg_len) {
        fprintf(stderr, "[universe] trail alloc failed\n");
        exit(1);
    }
    bo->trail_head  = 0;
    bo->trail_count = 0;
    bo->trail_accum = 0.0;
    bo->trail_total_len = 0.0;
    bo->trail_fade  = 1.0;
    bo->trail_emitting = 1;
    bo->trail_prev_pos[0] = bo->pos[0];
    bo->trail_prev_pos[1] = bo->pos[1];
    bo->trail_prev_pos[2] = bo->pos[2];
    bo->trail_prev_vel[0] = bo->vel[0];
    bo->trail_prev_vel[1] = bo->vel[1];
    bo->trail_prev_vel[2] = bo->vel[2];
    bo->trail_frame_accum = 0.0;
    bo->trail_frame_head = 0;
    bo->trail_frame_count = 0;
    bo->trail_frame_total_len = 0.0;
    bo->trail_frame_pos[0] = bo->pos[0];
    bo->trail_frame_pos[1] = bo->pos[1];
    bo->trail_frame_pos[2] = bo->pos[2];
    bo->trail_frame_vel[0] = bo->vel[0];
    bo->trail_frame_vel[1] = bo->vel[1];
    bo->trail_frame_vel[2] = bo->vel[2];
    bo->trail_frame_prev_pos[0] = bo->trail_prev_pos[0];
    bo->trail_frame_prev_pos[1] = bo->trail_prev_pos[1];
    bo->trail_frame_prev_pos[2] = bo->trail_prev_pos[2];
    bo->trail_frame_prev_vel[0] = bo->trail_prev_vel[0];
    bo->trail_frame_prev_vel[1] = bo->trail_prev_vel[1];
    bo->trail_frame_prev_vel[2] = bo->trail_prev_vel[2];
}

/* O(n) linear search through g_bodies[0..n-1] by name.  Only called during
 * loading (Pass 2 and Pass 3), never in the hot path. */
static int find_body_index(const char *name, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(g_bodies[i].name, name) == 0) return i;
    return -1;
}

/* True if some alive body in [0, limit) is named exactly `name`. */
static int name_taken(const char *name, int limit)
{
    for (int i = 0; i < limit; i++)
        if (g_bodies[i].alive && strcmp(g_bodies[i].name, name) == 0)
            return 1;
    return 0;
}

/*
 * dedupe_body_names — guarantee every alive body has a unique name.
 *
 * Names are the lookup key for parent links, rings, asteroid belts, build-mode
 * rebinding and labels, so collisions silently attach things to the wrong body.
 * The first occurrence of a name keeps it; each later duplicate gets the next
 * free " (2)", " (3)", ... suffix.  No-op when names are already unique, so the
 * common case is untouched.  Call once after a universe is fully loaded.
 */
static void dedupe_body_names(void)
{
    int renamed = 0;
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        if (!name_taken(g_bodies[i].name, i)) continue;   /* unique so far */

        char base[32];
        snprintf(base, sizeof base, "%s", g_bodies[i].name);
        for (int suffix = 2; ; suffix++) {
            char cand[32];
            /* Truncate the base so "<base> (NN)" always fits the 31-char field
             * (18 + " (" + up to 10 digits + ")" = 31). */
            snprintf(cand, sizeof cand, "%.18s (%d)", base, suffix);
            if (!name_taken(cand, g_nbodies)) {           /* unique vs everyone */
                snprintf(g_bodies[i].name, sizeof g_bodies[i].name, "%s", cand);
                renamed++;
                break;
            }
        }
    }
    if (renamed)
        fprintf(stdout, "[universe] de-duplicated %d body name%s for safe lookup\n",
                renamed, renamed == 1 ? "" : "s");
}

/* True if any alive body other than `except` is named exactly `name`. */
static int name_taken_by_other(const char *name, int except)
{
    for (int i = 0; i < g_nbodies; i++)
        if (i != except && g_bodies[i].alive
            && strcmp(g_bodies[i].name, name) == 0)
            return 1;
    return 0;
}

/* Suffix g_bodies[idx].name until it is unique among all other alive bodies.
 * Used when a body is created at runtime (build mode) so a user-typed name that
 * collides with an existing body cannot confuse name-keyed lookups. */
static void ensure_unique_name(int idx)
{
    if (idx < 0 || idx >= g_nbodies) return;
    if (!name_taken_by_other(g_bodies[idx].name, idx)) return;

    char base[32];
    snprintf(base, sizeof base, "%s", g_bodies[idx].name);
    for (int suffix = 2; ; suffix++) {
        char cand[32];
        snprintf(cand, sizeof cand, "%.18s (%d)", base, suffix);
        if (!name_taken_by_other(cand, idx)) {
            snprintf(g_bodies[idx].name, sizeof g_bodies[idx].name, "%s", cand);
            return;
        }
    }
}

int universe_live_body_count(void)
{
    int n = 0;
    for (int i = 0; i < g_nbodies; i++) {
        if (g_bodies[i].alive) n++;
    }
    return n;
}

int universe_can_add_body(void)
{
    return universe_live_body_count() < MAX_BODIES;
}

static int find_reusable_body_slot(void)
{
    int n = g_nbodies < MAX_BODIES ? g_nbodies : MAX_BODIES;
    for (int i = 0; i < n; i++) {
        if (!g_bodies[i].alive) return i;
    }
    return -1;
}

/* Read a [r, g, b] JSON array into a float[3].  Missing elements default to 0. */
static void read_color(const JsonNode *arr, float col[3])
{
    col[0] = (float)json_num(json_idx(arr, 0), 0.0);
    col[1] = (float)json_num(json_idx(arr, 1), 0.0);
    col[2] = (float)json_num(json_idx(arr, 2), 0.0);
}

/* Zero a Body struct and set safe scalar defaults: alive=1, parent=-1,
 * atm_scale=1.0 (no atmosphere still renders correctly at unit scale). */
static void body_defaults(Body *bo)
{
    memset(bo, 0, sizeof(*bo));
    bo->alive     = 1;
    bo->parent    = -1;
    bo->atm_scale = 1.0f;
}

/* Read obliquity and rotation period from JSON.
 * rotation_rate is stored in rad/s: 2π / (period_days × DAY). */
static void read_rotation(const JsonNode *bn, Body *bo)
{
    JsonNode *obl = json_get(bn, "obliquity_deg");
    JsonNode *rot = json_get(bn, "rotation_period_days");
    if (obl) bo->obliquity = json_num(obl, 0.0);
    if (rot && json_num(rot, 0.0) != 0.0)
        bo->rotation_rate = (2.0 * PI) / (json_num(rot, 1.0) * DAY);
}

/* Read the optional "atmosphere" sub-object: color, intensity, and scale. */
static void read_atmosphere(const JsonNode *bn, Body *bo)
{
    JsonNode *atm = json_get(bn, "atmosphere");
    if (!atm) return;
    float ac[3]; read_color(json_get(atm, "color"), ac);
    bo->atm_color[0]  = ac[0];
    bo->atm_color[1]  = ac[1];
    bo->atm_color[2]  = ac[2];
    bo->atm_intensity = (float)json_num(json_get(atm, "intensity"), 0.0);
    bo->atm_scale     = (float)json_num(json_get(atm, "scale"),     1.0);
}

/*
 * root_star_of — thin wrapper for body_root_star() (defined in body.c).
 *   Stars (parent=-1) return themselves in zero hops.
 *   Planets (parent=star) return the star in one hop.
 *   Moons (parent=planet, parent=star) return the star in two hops.
 */
static int root_star_of(int i) { return body_root_star(i); }

int g_universe_is_snapshot = 0;

/* Path of the source universe JSON most recently passed to universe_load().
 * universe_save() re-reads it to carry the procedural "rings"/"asteroid_belts"
 * definitions into the snapshot (those are regenerated from seeds on reload,
 * so copying the definition blocks is sufficient to preserve them). */
static char s_loaded_path[1024] = "";

/*
 * load_snapshot — load a "snapshot" universe: every body carries an absolute
 * "state" (pos_m, vel_ms) and an "is_star" flag, so placement is direct and no
 * Keplerian derivation, CoM correction, bulk velocity, or warm-up is applied.
 *
 * Parent links are resolved in a second pass.  The authoritative form is the
 * "parent_index" field (the parent's position in this same array), which is
 * immune to duplicate names; the legacy "parent" name is used only as a
 * fallback for snapshots written before parent_index existed.
 */
static void load_snapshot(const JsonNode *bodies_arr)
{
    int n = 0;
    for (const JsonNode *bn = bodies_arr->first_child; bn; bn = bn->next) n++;

    char (*parent_names)[32] = (char(*)[32])calloc((size_t)(n > 0 ? n : 1), 32);
    int  *parent_idx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!parent_names || !parent_idx) {
        fprintf(stderr, "[universe] snapshot alloc failed\n"); exit(1);
    }

    int idx = 0;
    for (const JsonNode *bn = bodies_arr->first_child; bn; bn = bn->next, idx++) {
        ensure_capacity(g_nbodies + 1);
        Body *bo = &g_bodies[g_nbodies];
        body_defaults(bo);

        strncpy(bo->name, json_str(json_get(bn, "name"), "body"), 31);
        bo->name[31] = '\0';
        bo->is_star = (int)json_num(json_get(bn, "is_star"), 0.0);
        bo->mass    = json_num(json_get(bn, "mass"), 0.0);
        bo->radius  = json_num(json_get(bn, "radius_km"), 1.0) * 1000.0;
        read_color(json_get(bn, "color"), bo->col);
        read_rotation(bn, bo);
        read_atmosphere(bn, bo);
        bo->rotation_angle = json_num(json_get(bn, "rotation_angle_rad"), 0.0);

        JsonNode *st = json_get(bn, "state");
        JsonNode *p  = st ? json_get(st, "pos_m") : NULL;
        JsonNode *v  = st ? json_get(st, "vel_ms") : NULL;
        bo->pos[0] = json_num(json_idx(p, 0), 0.0);
        bo->pos[1] = json_num(json_idx(p, 1), 0.0);
        bo->pos[2] = json_num(json_idx(p, 2), 0.0);
        bo->vel[0] = json_num(json_idx(v, 0), 0.0);
        bo->vel[1] = json_num(json_idx(v, 1), 0.0);
        bo->vel[2] = json_num(json_idx(v, 2), 0.0);

        snprintf(parent_names[idx], 32, "%s", json_str(json_get(bn, "parent"), ""));
        JsonNode *pi = json_get(bn, "parent_index");
        parent_idx[idx] = pi ? (int)json_num(pi, -1) : INT_MIN;  /* INT_MIN = absent */

        alloc_trail(bo);
        g_nbodies++;
    }

    /* Resolve parent links now that every body exists.  Prefer parent_index
     * (array position, unambiguous even with duplicate names); fall back to the
     * name for older snapshots.  Out-of-range or self-referential parents become
     * "no parent" so body_root_star()'s chain walk can never recurse forever. */
    for (int k = 0; k < g_nbodies; k++) {
        int p;
        if (parent_idx[k] != INT_MIN)
            p = parent_idx[k];
        else
            p = parent_names[k][0] ? find_body_index(parent_names[k], g_nbodies) : -1;
        g_bodies[k].parent = (p < 0 || p >= g_nbodies || p == k) ? -1 : p;
    }

    free(parent_names);
    free(parent_idx);

    /* Names are still the key for rings/asteroid belts/build, so disambiguate
     * any duplicates after parent links (which no longer depend on names). */
    dedupe_body_names();

    g_universe_is_snapshot = 1;
    fprintf(stdout, "[universe] snapshot: loaded %d bodies\n", g_nbodies);
    fflush(stdout);
}

/* ------------------------------------------------------------------ public */

/* Write s as a JSON string literal with the mandatory escapes, so names
 * containing a quote or backslash still produce valid, reloadable JSON. */
static void fput_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else          fputc(c, f);
        }
    }
    fputc('"', f);
}

/* Replace non-finite (NaN/Inf) values with 0 so a diverged simulation still
 * saves to parseable JSON (the loader's number parser rejects inf/nan tokens). */
static double fin(double x) { return isfinite(x) ? x : 0.0; }

/* Re-serialize a parsed JSON subtree (used to copy the source universe's
 * "rings"/"asteroid_belts" blocks verbatim into a saved snapshot). */
static void json_emit(FILE *f, const JsonNode *n, int indent)
{
    switch (n->type) {
        case JSON_NULL:   fputs("null", f); break;
        case JSON_BOOL:   fputs(n->boolean ? "true" : "false", f); break;
        case JSON_NUMBER: fprintf(f, "%.10g", fin(n->number)); break;
        case JSON_STRING: fput_json_str(f, n->string ? n->string : ""); break;
        case JSON_ARRAY:
        case JSON_OBJECT: {
            int obj = (n->type == JSON_OBJECT);
            int first = 1;
            fputc(obj ? '{' : '[', f);
            for (const JsonNode *c = n->first_child; c; c = c->next) {
                if (!first) fputc(',', f);
                first = 0;
                fputc('\n', f);
                for (int k = 0; k <= indent; k++) fputs("  ", f);
                if (obj && c->key) { fput_json_str(f, c->key); fputs(": ", f); }
                json_emit(f, c, indent + 1);
            }
            if (!first) {
                fputc('\n', f);
                for (int k = 0; k < indent; k++) fputs("  ", f);
            }
            fputc(obj ? '}' : ']', f);
            break;
        }
    }
}

int universe_save(const char *path)
{
    /* Carry the source universe's procedural rings/asteroid-belt definitions
     * into the snapshot.  Parse the source BEFORE opening the output (fopen
     * "wb" truncates), in case the user saves over the file being loaded. */
    JsonNode *src   = s_loaded_path[0] ? json_parse_file(s_loaded_path) : NULL;
    JsonNode *rings = src ? json_get(src, "rings") : NULL;
    JsonNode *belts = src ? json_get(src, "asteroid_belts") : NULL;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[universe] cannot write snapshot '%s'\n", path);
        json_free(src);
        return -1;
    }

    fprintf(f, "{\n  // OpenMultiVerse snapshot — absolute live state. Reloads as saved.\n");
    fprintf(f, "  \"format\": \"snapshot\",\n\n");
    fprintf(f,
        "  \"laws\": {\n"
        "    \"G\": %.10g, \"softening\": %.10g, \"time_scale\": %.10g,\n"
        "    \"force_exp\": %.10g, \"lambda\": %.10g, \"pn_factor\": %.10g, \"c_light\": %.10g,\n"
        "    \"gravity_isolation\": %.10g\n"
        "  },\n\n",
        fin(g_laws.G), fin(g_laws.softening), fin(g_laws.time_scale),
        fin(g_laws.force_exp), fin(g_laws.lambda), fin(g_laws.pn_factor),
        fin(g_laws.c_light), fin(g_laws.gravity_isolation));

    /* Map each g_bodies index to its position in the saved (alive-only) array,
     * so parent links can be written as an index and reload unambiguously even
     * when two bodies share a name. */
    int *slot = (int *)malloc((size_t)(g_nbodies > 0 ? g_nbodies : 1) * sizeof(int));
    if (!slot) {
        fprintf(stderr, "[universe] snapshot slot alloc failed\n");
        fclose(f); json_free(src); return -1;
    }
    int next_slot = 0;
    for (int i = 0; i < g_nbodies; i++)
        slot[i] = g_bodies[i].alive ? next_slot++ : -1;

    fprintf(f, "  \"bodies\": [\n");
    int first = 1, count = 0;
    for (int i = 0; i < g_nbodies; i++) {
        Body *b = &g_bodies[i];
        if (!b->alive) continue;
        double period = (b->rotation_rate != 0.0)
                      ? (2.0 * PI) / (b->rotation_rate * DAY) : 0.0;
        int parent_alive = (b->parent >= 0 && b->parent < g_nbodies
                            && g_bodies[b->parent].alive);
        const char *parent = parent_alive ? g_bodies[b->parent].name : "";
        int parent_slot    = parent_alive ? slot[b->parent] : -1;

        if (!first) fprintf(f, ",\n");
        first = 0;
        fprintf(f, "    { \"name\": ");
        fput_json_str(f, b->name);
        fprintf(f, ", \"is_star\": %d, \"parent_index\": %d, \"parent\": ",
                b->is_star ? 1 : 0, parent_slot);
        fput_json_str(f, parent);
        fprintf(f, ",\n");
        fprintf(f, "      \"mass\": %.10e, \"radius_km\": %.6f,\n",
                fin(b->mass), fin(b->radius / 1000.0));
        fprintf(f, "      \"color\": [%.4f, %.4f, %.4f],\n", b->col[0], b->col[1], b->col[2]);
        fprintf(f, "      \"obliquity_deg\": %.6f, \"rotation_period_days\": %.8g, "
                   "\"rotation_angle_rad\": %.8f,\n",
                fin(b->obliquity), fin(period), fin(b->rotation_angle));
        if (b->atm_intensity > 0.0f)
            fprintf(f, "      \"atmosphere\": { \"color\": [%.4f, %.4f, %.4f], "
                       "\"intensity\": %.4f, \"scale\": %.4f },\n",
                    b->atm_color[0], b->atm_color[1], b->atm_color[2],
                    b->atm_intensity, b->atm_scale);
        fprintf(f, "      \"state\": { \"pos_m\": [%.10e, %.10e, %.10e], "
                   "\"vel_ms\": [%.10e, %.10e, %.10e] } }",
                fin(b->pos[0]), fin(b->pos[1]), fin(b->pos[2]),
                fin(b->vel[0]), fin(b->vel[1]), fin(b->vel[2]));
        count++;
    }
    fprintf(f, "\n  ]");
    if (rings && rings->type == JSON_ARRAY && rings->first_child) {
        fprintf(f, ",\n\n  \"rings\": ");
        json_emit(f, rings, 1);
    }
    if (belts && belts->type == JSON_ARRAY && belts->first_child) {
        fprintf(f, ",\n\n  \"asteroid_belts\": ");
        json_emit(f, belts, 1);
    }
    fprintf(f, "\n}\n");
    fclose(f);
    free(slot);
    json_free(src);
    fprintf(stdout, "[universe] saved snapshot: %d bodies -> %s\n", count, path);
    fflush(stdout);
    return 0;
}

int universe_validate(const char *path)
{
    JsonNode *root = json_parse_file(path);
    if (!root) return -1;
    JsonNode *bodies = json_get(root, "bodies");
    int ok = (bodies && bodies->type == JSON_ARRAY);
    json_free(root);
    return ok ? 0 : -1;
}

void universe_load(const char *path)
{
    int i, s;
    fprintf(stdout, "[Boot] Loading universe data from %s\n", path);
    fflush(stdout);
    snprintf(s_loaded_path, sizeof(s_loaded_path), "%s", path ? path : "");
    JsonNode *root = json_parse_file(path);
    if (!root) {
        fprintf(stderr, "[universe] failed to open or parse '%s'\n", path);
        exit(1);
    }

    /* Per-universe physical laws: start from Newtonian defaults, then override
     * with whatever the optional "laws" block specifies.  Missing fields keep
     * their default so existing universe files load unchanged. */
    laws_reset();
    JsonNode *laws = json_get(root, "laws");
    if (laws && laws->type == JSON_OBJECT) {
        g_laws.G          = json_num(json_get(laws, "G"),          g_laws.G);
        g_laws.softening  = json_num(json_get(laws, "softening"),  g_laws.softening);
        g_laws.time_scale = json_num(json_get(laws, "time_scale"), g_laws.time_scale);
        g_laws.force_exp  = json_num(json_get(laws, "force_exp"),  g_laws.force_exp);
        g_laws.lambda     = json_num(json_get(laws, "lambda"),     g_laws.lambda);
        g_laws.pn_factor  = json_num(json_get(laws, "pn_factor"),  g_laws.pn_factor);
        g_laws.c_light    = json_num(json_get(laws, "c_light"),    g_laws.c_light);
        g_laws.gravity_isolation = json_num(json_get(laws, "gravity_isolation"),
                                            g_laws.gravity_isolation);
        fprintf(stdout, "[Boot] Universe laws: G=%.4g softening=%.4g force_exp=%.4g "
                        "lambda=%.4g pn=%.4g\n",
                g_laws.G, g_laws.softening, g_laws.force_exp,
                g_laws.lambda, g_laws.pn_factor);
        fflush(stdout);
    }

    /* Softening is the only thing keeping the 1/r^2 force denominator non-zero
     * when two bodies coincide; a JSON file asking for 0 would divide by zero.
     * Clamp to a small positive floor. */
    if (!(g_laws.softening >= LAWS_MIN_SOFTENING))
        g_laws.softening = LAWS_MIN_SOFTENING;

    JsonNode *bodies_arr = json_get(root, "bodies");
    if (!bodies_arr || bodies_arr->type != JSON_ARRAY) {
        fprintf(stderr, "[universe] 'bodies' array not found in '%s'\n", path);
        json_free(root); exit(1);
    }

    g_nbodies = 0;
    g_universe_is_snapshot = 0;

    /* Snapshot universes (saved live state) take a direct placement path that
     * skips Keplerian derivation and all post-processing. */
    if (strcmp(json_str(json_get(root, "format"), ""), "snapshot") == 0) {
        load_snapshot(bodies_arr);
        json_free(root);
        return;
    }

    /* Bulk velocity per body slot.  Set for star indices during Pass 1;
     * zero everywhere else.  Applied in post-processing after CoM correction.
     *
     * Heap-allocated to the JSON body count rather than a fixed MAX_BODIES
     * array: ensure_capacity() grows g_bodies past MAX_BODIES, so an import
     * with >= MAX_BODIES stars would write bv[g_nbodies] off the end of a
     * fixed stack array.  Star slot indices are always < g_nbodies <= n_total. */
    int n_total = 0;
    for (JsonNode *bc = bodies_arr->first_child; bc; bc = bc->next) n_total++;
    double (*bv)[3] = (double(*)[3])calloc((size_t)(n_total > 0 ? n_total : 1),
                                           sizeof(*bv));
    if (!bv) {
        fprintf(stderr, "[universe] bulk-velocity alloc failed\n");
        json_free(root); exit(1);
    }

    /* ================================================================
     * Pass 1 — Stars
     *
     * Stars are placed at absolute world positions (pos_ly × LY → metres).
     * The bulk velocity vector (velocity_km_s) is stashed in bv[star_idx]
     * and applied after the full system is loaded so that the CoM correction
     * can be computed first against the orbital velocities alone.
     * ================================================================ */
    fprintf(stdout, "[Boot] Universe pass 1/3: stars\n");
    fflush(stdout);
    {
        JsonNode *bn;
        for (bn = bodies_arr->first_child; bn; bn = bn->next) {
            const char *type = json_str(json_get(bn, "type"), "");
            if (strcmp(type, "star") != 0) continue;

            const char *name   = json_str(json_get(bn, "name"),      "unknown");
            double      mass   = json_num(json_get(bn, "mass"),       0.0);
            double      rad_km = json_num(json_get(bn, "radius_km"),  1.0);
            float       col[3];
            read_color(json_get(bn, "color"), col);

            double px = 0.0, py = 0.0, pz = 0.0;
            JsonNode *ply = json_get(bn, "pos_ly");
            if (ply) {
                px = json_num(json_idx(ply, 0), 0.0) * LY;
                py = json_num(json_idx(ply, 1), 0.0) * LY;
                pz = json_num(json_idx(ply, 2), 0.0) * LY;
            }

            ensure_capacity(g_nbodies + 1);
            Body *bo = &g_bodies[g_nbodies];
            body_defaults(bo);
            strncpy(bo->name, name, 31); bo->name[31] = '\0';
            bo->mass           = mass;
            bo->radius         = rad_km * 1000.0;
            bo->pos[0]         = px; bo->pos[1] = py; bo->pos[2] = pz;
            bo->col[0]         = col[0]; bo->col[1] = col[1]; bo->col[2] = col[2];
            bo->is_star        = 1;
            read_rotation(bn, bo);

            /* Stash bulk velocity; convert km/s → m/s */
            JsonNode *vn = json_get(bn, "velocity_km_s");
            if (vn) {
                bv[g_nbodies][0] = json_num(json_idx(vn, 0), 0.0) * 1000.0;
                bv[g_nbodies][1] = json_num(json_idx(vn, 1), 0.0) * 1000.0;
                bv[g_nbodies][2] = json_num(json_idx(vn, 2), 0.0) * 1000.0;
            }

            alloc_trail(bo);
            g_nbodies++;
        }
    }

    /* ================================================================
     * Pass 2 — Planets, dwarf_planets, and asteroids
     *
     * keplerian_to_state() returns a heliocentric position/velocity in SI
     * units (metres, m/s) relative to the parent star.  The star's world
     * position is then added to convert to absolute world coordinates.
     *
     * GM of the parent star must be expressed in AU³/day² to match the
     * JPL table convention used by keplerian_to_state():
     *   gm_star_au2 = G × M_star / AU³ × DAY²
     *
     * All parent data is read before the ensure_capacity() call: while
     * ensure_capacity uses an integer index (par_idx) that survives realloc,
     * taking a pointer to g_bodies[par_idx] before realloc would be UB.
     * ================================================================ */
    fprintf(stdout, "[Boot] Universe pass 2/3: planets, dwarf planets, and asteroids\n");
    fflush(stdout);
    {
        JsonNode *bn;
        for (bn = bodies_arr->first_child; bn; bn = bn->next) {
            const char *type = json_str(json_get(bn, "type"), "");
            if (strcmp(type, "planet") != 0 &&
                strcmp(type, "dwarf_planet") != 0 &&
                strcmp(type, "asteroid") != 0)
                continue;

            const char *name     = json_str(json_get(bn, "name"),      "unknown");
            double      mass     = json_num(json_get(bn, "mass"),       0.0);
            double      rad_km   = json_num(json_get(bn, "radius_km"),  1.0);
            const char *par_name = json_str(json_get(bn, "parent"),     "");
            float       col[3];
            read_color(json_get(bn, "color"), col);

            int par_idx = find_body_index(par_name, g_nbodies);
            if (par_idx < 0) {
                fprintf(stderr, "[universe] orbiting body '%s': parent '%s' not found\n",
                        name, par_name);
                json_free(root); exit(1);
            }

            /* GM of parent star in AU³/day² for keplerian_to_state() */
            double gm_star_au2 = G_CONST * g_bodies[par_idx].mass
                                 / (AU * AU * AU) * (DAY * DAY);

            double p[3] = {0,0,0}, v[3] = {0,0,0};
            double a = 1.0;
            JsonNode *kep = json_get(bn, "keplerian");
            if (kep) {
                a        = json_num(json_get(kep, "a"),            1.0);
                double e = json_num(json_get(kep, "e"),            0.0);
                double ii= json_num(json_get(kep, "i"),            0.0);
                double O = json_num(json_get(kep, "Omega"),        0.0);
                double w = json_num(json_get(kep, "omega_tilde"),  0.0);
                double L = json_num(json_get(kep, "L"),            0.0);
                keplerian_to_state(a, e, ii, O, w, L, gm_star_au2, p, v);
            }
            /* Convert heliocentric → world coordinates */
            p[0] += g_bodies[par_idx].pos[0];
            p[1] += g_bodies[par_idx].pos[1];
            p[2] += g_bodies[par_idx].pos[2];

            ensure_capacity(g_nbodies + 1);
            Body *bo = &g_bodies[g_nbodies++];
            body_defaults(bo);
            strncpy(bo->name, name, 31); bo->name[31] = '\0';
            bo->mass           = mass;
            bo->radius         = rad_km * 1000.0;
            bo->pos[0]         = p[0]; bo->pos[1] = p[1]; bo->pos[2] = p[2];
            bo->vel[0]         = v[0]; bo->vel[1] = v[1]; bo->vel[2] = v[2];
            bo->col[0]         = col[0]; bo->col[1] = col[1]; bo->col[2] = col[2];
            bo->parent         = par_idx;
            read_rotation(bn, bo);
            read_atmosphere(bn, bo);
            alloc_trail(bo);
        }
    }

    /* ================================================================
     * Pass 3 — Moons
     *
     * moon_to_state() returns a parent-relative position/velocity (metres,
     * m/s) in the GL frame.  The parent's world position and velocity are
     * added to get absolute state.
     *
     * The parent's position and velocity are cached in local arrays BEFORE
     * ensure_capacity() is called.  ensure_capacity() may realloc g_bodies[],
     * invalidating any raw pointer taken from it.  Using cached scalar copies
     * avoids any dependency on the old pointer after realloc.
     * ================================================================ */
    fprintf(stdout, "[Boot] Universe pass 3/3: moons\n");
    fflush(stdout);
    {
        JsonNode *bn;
        for (bn = bodies_arr->first_child; bn; bn = bn->next) {
            const char *type = json_str(json_get(bn, "type"), "");
            if (strcmp(type, "moon") != 0) continue;

            const char *name     = json_str(json_get(bn, "name"),     "unknown");
            double      mass     = json_num(json_get(bn, "mass"),      0.0);
            double      rad_km   = json_num(json_get(bn, "radius_km"), 1.0);
            const char *par_name = json_str(json_get(bn, "parent"),    "");
            float       col[3];
            read_color(json_get(bn, "color"), col);

            int par_idx = find_body_index(par_name, g_nbodies);
            if (par_idx < 0) {
                fprintf(stderr, "[universe] moon '%s': parent '%s' not found\n",
                        name, par_name);
                json_free(root); exit(1);
            }

            /* Cache parent state before potential realloc in ensure_capacity() */
            double gm_par   = G_CONST * g_bodies[par_idx].mass;
            double par_p[3] = { g_bodies[par_idx].pos[0],
                                g_bodies[par_idx].pos[1],
                                g_bodies[par_idx].pos[2] };
            double par_v[3] = { g_bodies[par_idx].vel[0],
                                g_bodies[par_idx].vel[1],
                                g_bodies[par_idx].vel[2] };

            JsonNode *mk = json_get(bn, "moon_keplerian");
            double rel_p[3] = {0,0,0}, rel_v[3] = {0,0,0};
            double a_km = 0.0;
            if (mk) {
                a_km             = json_num(json_get(mk, "a_km"),      0.0);
                double e         = json_num(json_get(mk, "e"),          0.0);
                double i_deg     = json_num(json_get(mk, "i_deg"),      0.0);
                double Omega_deg = json_num(json_get(mk, "Omega_deg"),  0.0);
                double omega_deg = json_num(json_get(mk, "omega_deg"),  0.0);
                double M0_deg    = json_num(json_get(mk, "M0_deg"),     0.0);
                moon_to_state(a_km, e, i_deg, Omega_deg, omega_deg,
                              M0_deg, gm_par, rel_p, rel_v);
            }

            ensure_capacity(g_nbodies + 1);
            Body *bo = &g_bodies[g_nbodies++];
            body_defaults(bo);
            strncpy(bo->name, name, 31); bo->name[31] = '\0';
            bo->mass           = mass;
            bo->radius         = rad_km * 1000.0;
            bo->pos[0]         = par_p[0] + rel_p[0];
            bo->pos[1]         = par_p[1] + rel_p[1];
            bo->pos[2]         = par_p[2] + rel_p[2];
            bo->vel[0]         = par_v[0] + rel_v[0];
            bo->vel[1]         = par_v[1] + rel_v[1];
            bo->vel[2]         = par_v[2] + rel_v[2];
            bo->col[0]         = col[0]; bo->col[1] = col[1]; bo->col[2] = col[2];
            bo->parent         = par_idx;
            read_rotation(bn, bo);
            read_atmosphere(bn, bo);
            alloc_trail(bo);
        }
    }

    /* ================================================================
     * Post-processing — per star system:
     *
     * Step 1 — Centre-of-mass velocity correction.
     *   After keplerian_to_state(), each planet has a heliocentric velocity
     *   that assumes the star is stationary.  Summing p·v over all planets
     *   gives a net momentum; this is removed by nudging the star velocity:
     *     v_star -= Σ (M_i / M_star) × v_i
     *   Only the star is adjusted (M_star >> M_planets), so the correction
     *   is negligible for the planets and exact for the total momentum.
     *
     * Step 2 — Bulk velocity (proper motion).
     *   The stellar "velocity_km_s" from JSON is the system's velocity through
     *   the galaxy.  After CoM correction, this is added uniformly to every
     *   body in the system so the system drifts as a rigid unit.
     * ================================================================ */
    fprintf(stdout, "[Boot] Universe post-processing: system velocities\n");
    fflush(stdout);
    int n_stars = 0;
    for (s = 0; s < g_nbodies; s++) {
        if (!g_bodies[s].is_star) continue;
        n_stars++;

        /* CoM correction: zero net internal momentum by adjusting only the star */
        for (i = 0; i < g_nbodies; i++) {
            if (i == s || root_star_of(i) != s) continue;
            g_bodies[s].vel[0] -=
                g_bodies[i].mass * g_bodies[i].vel[0] / g_bodies[s].mass;
            g_bodies[s].vel[1] -=
                g_bodies[i].mass * g_bodies[i].vel[1] / g_bodies[s].mass;
            g_bodies[s].vel[2] -=
                g_bodies[i].mass * g_bodies[i].vel[2] / g_bodies[s].mass;
        }

        /* Apply bulk proper-motion velocity uniformly to the whole system */
        if (bv[s][0] != 0.0 || bv[s][1] != 0.0 || bv[s][2] != 0.0) {
            for (i = 0; i < g_nbodies; i++) {
                if (root_star_of(i) != s) continue;
                g_bodies[i].vel[0] += bv[s][0];
                g_bodies[i].vel[1] += bv[s][1];
                g_bodies[i].vel[2] += bv[s][2];
            }
        }

        int cnt = 0;
        for (i = 0; i < g_nbodies; i++)
            if (root_star_of(i) == s) cnt++;
        fprintf(stdout,
                "[universe] '%s' at (%.3g, %.3g, %.3g) ly  -  %d bod%s\n",
                g_bodies[s].name,
                g_bodies[s].pos[0] / LY,
                g_bodies[s].pos[1] / LY,
                g_bodies[s].pos[2] / LY,
                cnt, cnt == 1 ? "y" : "ies");
    }

    fprintf(stdout, "[universe] total: %d bodies across %d star%s\n",
            g_nbodies, n_stars, n_stars == 1 ? "" : "s");
    fflush(stdout);

    /* Parent links are already resolved; make names unique so the downstream
     * name-keyed subsystems (rings, asteroid belts, build-mode rebind, labels)
     * can never bind to the wrong body. */
    dedupe_body_names();

    free(bv);
    json_free(root);
}

/*
 * universe_add_body — create a body at runtime from a BodyCreateSpec.
 *
 * Used by the build system to add stars, planets, and moons interactively.
 * Mirrors the three-pass loader but operates on a single pre-filled spec.
 * Returns the new body's index in g_bodies[], or -1 on failure.
 */
int universe_add_body(const BodyCreateSpec *spec)
{
    int idx, reused_slot;
    double (*old_trail)[3] = NULL;
    double *old_trail_seg_len = NULL;
    Body *bo;

    if (!spec) return -1;
    if (!universe_can_add_body()) {
        fprintf(stderr, "[universe] cannot add body '%s': live MAX_BODIES reached\n",
                spec->name ? spec->name : "unknown");
        return -1;
    }

    idx = find_reusable_body_slot();
    reused_slot = (idx >= 0);
    if (!reused_slot) {
        if (g_nbodies >= MAX_BODIES) {
            fprintf(stderr, "[universe] cannot add body '%s': no reusable body slots\n",
                    spec->name ? spec->name : "unknown");
            return -1;
        }
        ensure_capacity(g_nbodies + 1);
        idx = g_nbodies++;
    }

    bo = &g_bodies[idx];
    if (reused_slot) {
        old_trail = bo->trail;
        old_trail_seg_len = bo->trail_seg_len;
    }
    body_defaults(bo);
    if (reused_slot) {
        bo->trail = old_trail;
        bo->trail_seg_len = old_trail_seg_len;
    }

    strncpy(bo->name, spec->name ? spec->name : "Body", 31);
    bo->name[31] = '\0';
    bo->mass = spec->mass;
    bo->radius = spec->radius;
    bo->pos[0] = spec->pos[0];
    bo->pos[1] = spec->pos[1];
    bo->pos[2] = spec->pos[2];
    bo->vel[0] = spec->vel[0];
    bo->vel[1] = spec->vel[1];
    bo->vel[2] = spec->vel[2];
    bo->col[0] = spec->col[0];
    bo->col[1] = spec->col[1];
    bo->col[2] = spec->col[2];
    bo->is_star = spec->is_star;
    bo->parent = spec->parent;
    bo->obliquity = spec->obliquity;
    bo->rotation_rate = spec->rotation_rate;
    bo->atm_color[0] = spec->atm_color[0];
    bo->atm_color[1] = spec->atm_color[1];
    bo->atm_color[2] = spec->atm_color[2];
    bo->atm_intensity = spec->atm_intensity;
    bo->atm_scale = spec->atm_scale > 0.0f ? spec->atm_scale : 1.0f;
    alloc_trail(bo);

    /* Keep names unique so labels and name-keyed lookups stay unambiguous even
     * if the user names a new body the same as an existing one. */
    ensure_unique_name(idx);

    return idx;
}

/*
 * universe_rebind_to_nearest_stars — reassign planet parent pointers after a
 * star has been added or moved by the build system.
 *
 * Only star-orbiting bodies are eligible for rebinding:
 *   - Skips dead bodies and stars (they manage their own parent = -1).
 *   - Skips moons: if a body's current parent is not a star (i.e. parent is a
 *     planet or another moon), its orbital hierarchy is left intact.
 *   - Parentless non-stars (parent == -1) are also candidates — they adopt the
 *     nearest star.
 *
 * Use case: the user drops a new star near an existing solar system in build
 * mode.  The planets whose nearest star is now the new one should switch their
 * parent pointer so that physics grouping and LOD decisions stay correct.
 */
void universe_rebind_to_nearest_stars(void)
{
    for (int i = 0; i < g_nbodies; i++) {
        int best_star = -1;
        double best_d2 = 1e300;

        if (!g_bodies[i].alive || g_bodies[i].is_star) continue;
        if (g_bodies[i].parent >= 0 && !g_bodies[g_bodies[i].parent].is_star)
            continue;

        for (int s = 0; s < g_nbodies; s++) {
            double dx, dy, dz, d2;
            if (!g_bodies[s].alive || !g_bodies[s].is_star) continue;
            dx = g_bodies[s].pos[0] - g_bodies[i].pos[0];
            dy = g_bodies[s].pos[1] - g_bodies[i].pos[1];
            dz = g_bodies[s].pos[2] - g_bodies[i].pos[2];
            d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_star = s;
            }
        }

        if (best_star >= 0)
            g_bodies[i].parent = best_star;
    }
}

/* Free all trail buffers and the body array itself, then reset globals. */
void universe_shutdown(void)
{
    int i;
    for (i = 0; i < g_nbodies; i++) {
        free(g_bodies[i].trail);
        g_bodies[i].trail = NULL;
        free(g_bodies[i].trail_seg_len);
        g_bodies[i].trail_seg_len = NULL;
    }
    free(g_bodies);
    g_bodies     = NULL;
    g_nbodies    = 0;
    g_bodies_cap = 0;
}
