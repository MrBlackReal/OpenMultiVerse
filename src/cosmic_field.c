/*
 * cosmic_field.c — uniform spatial hash + density/variance query.
 *
 * See cosmic_field.h for the role.  The structure mirrors the CSR member-pool
 * build in physics.c (physics_active_bodies): a counting-sort scatter of body
 * indices into cells, with an open-addressed hash for the (enormous, sparse)
 * cell coordinate space so only occupied cells cost memory — empty interstellar
 * space is free.
 *
 * All positions in SI metres, double.  Accumulation is done in light-years to
 * keep magnitudes small and precise (radius ~ few ly, not ~1e16 m).
 */
#include "cosmic_field.h"
#include "body.h"
#include "camera.h"
#include "nebula.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tunables ─────────────────────────────────────────────────────────────── */
#define COSMIC_CELL_LY      1.0                    /* cell edge, light-years    */
#define COSMIC_CELL_M       (COSMIC_CELL_LY * LY)  /* cell edge, metres         */
#define COSMIC_HUD_RADIUS_LY 3.0                   /* default camera-sample R   */
#define COSMIC_REBUILD_SEC  0.25                   /* throttle between rebuilds */
#define COSMIC_MIN_BODIES   2                      /* discrete-signal threshold */
#define COSMIC_FILL_EPS     0.05                   /* continuous-signal thresh. */

/* 21 bits per axis, biased to unsigned → range ±(2^20) ly, far past a galaxy. */
#define CELL_BITS   21
#define CELL_BIAS   (1 << 20)                      /* 1048576                   */
#define CELL_MIN    (-(1 << 20))
#define CELL_MAX    ((1 << 20) - 1)
#define CELL_MASK   ((1u << CELL_BITS) - 1u)

/* Guard: if a query's bounding box spans more cells than this, fall back to a
 * linear scan over all bodies instead of walking a huge empty cell grid. */
#define CELL_BOX_CAP  200000

#define KEY_EMPTY   ((uint64_t)~0ull)

typedef struct {
    uint64_t key;    /* packed cell key, or KEY_EMPTY                          */
    int      start;  /* offset of this cell's bodies in s_cell_body            */
    int      count;  /* body count in this cell                               */
    int      fill;   /* scatter cursor during build (== start + written)      */
} Cell;

static Cell  *s_table   = NULL;   /* open-addressed hash, power-of-two size    */
static int    s_table_cap = 0;    /* slot count (power of two)                 */
static int   *s_cell_body = NULL; /* CSR values: alive body indices by cell    */
static int    s_cell_body_cap = 0;
static int    s_built_nbodies = 0;/* g_nbodies at last rebuild (change → dirty)*/
static int    s_built = 0;        /* 1 once a build has populated the table    */
static double s_rebuild_accum = 0.0;

/* splitmix64 finaliser — good dispersion for the packed integer cell key. */
static inline uint64_t hash64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* Pack integer cell coordinates into a single key.  Caller guarantees each
 * component is within [CELL_MIN, CELL_MAX]. */
static inline uint64_t pack_cell(int cx, int cy, int cz)
{
    uint64_t ux = (uint64_t)(uint32_t)((cx + CELL_BIAS) & (int)CELL_MASK);
    uint64_t uy = (uint64_t)(uint32_t)((cy + CELL_BIAS) & (int)CELL_MASK);
    uint64_t uz = (uint64_t)(uint32_t)((cz + CELL_BIAS) & (int)CELL_MASK);
    return ux | (uy << CELL_BITS) | (uz << (2 * CELL_BITS));
}

/* Cell coordinate of a metre position along one axis (floor division). */
static inline int cell_coord(double pos_m)
{
    double c = floor(pos_m / COSMIC_CELL_M);
    if (c < (double)CELL_MIN) return CELL_MIN;
    if (c > (double)CELL_MAX) return CELL_MAX;
    return (int)c;
}

static int next_pow2(int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* Find the slot holding `key`, or the first empty slot if absent (open
 * addressing, linear probe).  Returns a slot index; caller checks its key. */
static int table_slot(uint64_t key)
{
    uint32_t mask = (uint32_t)s_table_cap - 1u;
    uint32_t i = (uint32_t)hash64(key) & mask;
    while (s_table[i].key != KEY_EMPTY && s_table[i].key != key)
        i = (i + 1u) & mask;
    return (int)i;
}

void cosmic_field_init(void)
{
    s_table = NULL;         s_table_cap = 0;
    s_cell_body = NULL;     s_cell_body_cap = 0;
    s_built_nbodies = 0;    s_built = 0;
    s_rebuild_accum = 0.0;
}

void cosmic_field_shutdown(void)
{
    free(s_table);      s_table = NULL;      s_table_cap = 0;
    free(s_cell_body);  s_cell_body = NULL;  s_cell_body_cap = 0;
    s_built = 0;        s_built_nbodies = 0;
}

void cosmic_field_rebuild(void)
{
    s_built = 0;
    s_built_nbodies = g_nbodies;
    s_rebuild_accum = 0.0;

    /* Count alive bodies to size the structures. */
    int alive = 0;
    for (int i = 0; i < g_nbodies; i++)
        if (g_bodies[i].alive) alive++;
    if (alive == 0) return;   /* nothing to index; queries return empty       */

    /* Table sized for load factor <= 0.5 against the worst case (every body in
     * its own cell). */
    int want = next_pow2(2 * alive + 1);
    if (want > s_table_cap) {
        Cell *t = realloc(s_table, (size_t)want * sizeof(Cell));
        if (!t) { fprintf(stderr, "[cosmic_field] table alloc failed\n"); return; }
        s_table = t; s_table_cap = want;
    }
    for (int i = 0; i < s_table_cap; i++) { s_table[i].key = KEY_EMPTY; }

    if (alive > s_cell_body_cap) {
        int cap = s_cell_body_cap ? s_cell_body_cap : 1;
        while (cap < alive) cap <<= 1;
        int *cb = realloc(s_cell_body, (size_t)cap * sizeof(int));
        if (!cb) { fprintf(stderr, "[cosmic_field] cell pool alloc failed\n"); return; }
        s_cell_body = cb; s_cell_body_cap = cap;
    }

    /* Pass 1 — count bodies per cell (find-or-insert). */
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        uint64_t key = pack_cell(cell_coord(g_bodies[i].pos[0]),
                                 cell_coord(g_bodies[i].pos[1]),
                                 cell_coord(g_bodies[i].pos[2]));
        int s = table_slot(key);
        if (s_table[s].key == KEY_EMPTY) {
            s_table[s].key = key;
            s_table[s].count = 0;
        }
        s_table[s].count++;
    }

    /* Prefix-sum per-cell offsets, and seed each cell's scatter cursor. */
    int cursor = 0;
    for (int i = 0; i < s_table_cap; i++) {
        if (s_table[i].key == KEY_EMPTY) continue;
        s_table[i].start = cursor;
        s_table[i].fill  = cursor;
        cursor += s_table[i].count;
    }

    /* Pass 2 — scatter body indices into the CSR pool. */
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        uint64_t key = pack_cell(cell_coord(g_bodies[i].pos[0]),
                                 cell_coord(g_bodies[i].pos[1]),
                                 cell_coord(g_bodies[i].pos[2]));
        int s = table_slot(key);
        s_cell_body[s_table[s].fill++] = i;
    }

    s_built = 1;
}

void cosmic_field_tick(double dt)
{
    if (dt < 0.0) dt = 0.0;
    s_rebuild_accum += dt;
    if (!s_built || g_nbodies != s_built_nbodies ||
        s_rebuild_accum >= COSMIC_REBUILD_SEC)
        cosmic_field_rebuild();
}

/* Accumulate one body's contribution (position relative to the sample centre,
 * in light-years) into the running sums if it lies inside the sample sphere. */
static inline void accum_body(int b, const double centre_m[3], double r2_ly,
                              double *N, double *mass_sum,
                              double sum[3], double *sumsq)
{
    if (!g_bodies[b].alive) return;   /* stable indices: slot may now be dead  */
    double rx = (g_bodies[b].pos[0] - centre_m[0]) / LY;
    double ry = (g_bodies[b].pos[1] - centre_m[1]) / LY;
    double rz = (g_bodies[b].pos[2] - centre_m[2]) / LY;
    double d2 = rx*rx + ry*ry + rz*rz;
    if (d2 > r2_ly) return;
    *N += 1.0;
    *mass_sum += g_bodies[b].mass;
    sum[0] += rx; sum[1] += ry; sum[2] += rz;
    *sumsq += d2;
}

int cosmic_field_sample(const double pos_m[3], double radius_m, CosmicSample *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->dominant = COSMIC_CONTINUOUS;   /* vacuum default                     */
    if (radius_m <= 0.0) return 0;

    const double radius_ly = radius_m / LY;
    const double r2_ly     = radius_ly * radius_ly;

    double N = 0.0, mass_sum = 0.0, sumsq = 0.0;
    double sum[3] = { 0.0, 0.0, 0.0 };

    if (s_built) {
        int cmin[3], cmax[3];
        for (int k = 0; k < 3; k++) {
            cmin[k] = cell_coord(pos_m[k] - radius_m);
            cmax[k] = cell_coord(pos_m[k] + radius_m);
        }
        /* Cell-box size (guarding against int overflow on huge radii). */
        double boxx = (double)cmax[0] - cmin[0] + 1.0;
        double boxy = (double)cmax[1] - cmin[1] + 1.0;
        double boxz = (double)cmax[2] - cmin[2] + 1.0;
        double box_cells = boxx * boxy * boxz;

        if (box_cells > (double)CELL_BOX_CAP) {
            /* Degenerate huge query — linear scan is cheaper than walking a
             * mostly-empty cell box. */
            for (int b = 0; b < g_nbodies; b++)
                accum_body(b, pos_m, r2_ly, &N, &mass_sum, sum, &sumsq);
        } else {
            for (int cz = cmin[2]; cz <= cmax[2]; cz++)
            for (int cy = cmin[1]; cy <= cmax[1]; cy++)
            for (int cx = cmin[0]; cx <= cmax[0]; cx++) {
                uint64_t key = pack_cell(cx, cy, cz);
                int s = table_slot(key);
                if (s_table[s].key != key) continue;   /* empty cell → free    */
                int start = s_table[s].start, cnt = s_table[s].count;
                for (int k = 0; k < cnt; k++)
                    accum_body(s_cell_body[start + k], pos_m, r2_ly,
                               &N, &mass_sum, sum, &sumsq);
            }
        }
    }

    /* Volume of the sample sphere in ly^3. */
    double V_ly3 = (4.0 / 3.0) * PI * radius_ly * radius_ly * radius_ly;
    if (V_ly3 <= 0.0) V_ly3 = 1.0;

    out->body_count     = (int)(N + 0.5);
    out->number_density = N / V_ly3;
    out->mass_density   = mass_sum / V_ly3;

    /* Clumpiness: RMS dispersion of member positions about their centroid,
     * normalised against a uniform sphere's RMS radius (sqrt(3/5)·R).  ~0 for
     * uniform scatter, ~1 for a tight cluster collapsed near a point. */
    if (N >= 2.0) {
        double mx = sum[0] / N, my = sum[1] / N, mz = sum[2] / N;
        double var = sumsq / N - (mx*mx + my*my + mz*mz);
        if (var < 0.0) var = 0.0;
        double rms = sqrt(var);
        double uniform_rms = 0.7745966692 * radius_ly;   /* sqrt(3/5)·R        */
        double c = uniform_rms > 0.0 ? 1.0 - rms / uniform_rms : 0.0;
        out->clumpiness = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
    } else {
        out->clumpiness = 0.0;   /* dispersion undefined for < 2 bodies        */
    }

    /* Continuous medium: peak nebular coverage at the point.  Only 18 nebulae,
     * so a linear scan is fine and needs no spatial structure. */
    double fill = 0.0;
    int nhit = 0;
    int nn = nebula_count();
    for (int i = 0; i < nn; i++) {
        double c_au[3];
        nebula_position(i, c_au);
        double nr_m = nebula_radius_au(i) * AU;
        if (nr_m <= 0.0) continue;
        double dx = c_au[0] * AU - pos_m[0];
        double dy = c_au[1] * AU - pos_m[1];
        double dz = c_au[2] * AU - pos_m[2];
        double d  = sqrt(dx*dx + dy*dy + dz*dz);
        if (d < nr_m) {
            double f = 1.0 - d / nr_m;   /* 1 at centre → 0 at rim             */
            if (f > fill) fill = f;
            nhit++;
        }
    }
    out->continuous_fill = fill;
    out->nebulae_hit     = nhit;

    /* Dominant class from the two signals. */
    int discrete   = out->body_count >= COSMIC_MIN_BODIES;
    int continuous = out->continuous_fill >= COSMIC_FILL_EPS;
    if (discrete && continuous) out->dominant = COSMIC_HYBRID;
    else if (discrete)          out->dominant = COSMIC_DISCRETE;
    else                        out->dominant = COSMIC_CONTINUOUS;

    return (out->body_count > 0 || nhit > 0) ? 1 : 0;
}

int cosmic_field_sample_camera(CosmicSample *out)
{
    double pos_m[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
    return cosmic_field_sample(pos_m, COSMIC_HUD_RADIUS_LY * LY, out);
}

const char *cosmic_field_class_name(CosmicClass c)
{
    switch (c) {
    case COSMIC_DISCRETE:   return "DISCRETE";
    case COSMIC_HYBRID:     return "HYBRID";
    case COSMIC_CONTINUOUS:
    default:                return "CONTINUOUS";
    }
}
