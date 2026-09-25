/*
 * terrain.c — cube-sphere mesh terrain (terrain.h).
 *
 * Six quadtrees, one per cube face, in the body-local frame (the one the
 * surface shader paints in, so tiles turn with the planet). The tree is not
 * stored: every frame walks it from the six roots, splitting a tile while its
 * quads span more than SPLIT_PX on screen and its four children are baked.
 * A child that is still missing is requested and its parent drawn meanwhile,
 * so the planet is always fully covered and detail streams in coarse-first.
 *
 * Baked tiles live in one R32F texture array, a layer per tile, found through
 * a hash of (body, face, level, x, y). A layer is recycled least-recently-used,
 * never from a tile the current frame touched. The cache is bounded, whatever
 * the number of bodies: only the few planets close enough for relief to show
 * hold tiles at all.
 *
 * Precision: every quantity that must be exact to the metre (the tile centre
 * on the sphere, relative to the camera) is formed here in double and handed
 * to the GPU as a small anchor; the shaders only add offsets from it
 * (terrain_tile.glsl).
 */
#include "terrain.h"
#include "gl_utils.h"

#include <stdint.h>

#define TILE_N       TERRAIN_TILE_N
#define TEX_N        (TILE_N + 2)       /* + a border texel for normals      */
#define MAX_LEVEL    20                 /* quads ~0.2 m on an Earth-size world */
#define MAX_OCT      24                 /* terrain_gen.frag MAX_OCT          */
#define MAX_LAYERS   2048               /* 37 MB of R32F at 67×67            */
#define HASH_CAP     8192               /* power of two, > 2 × MAX_LAYERS    */
#define GEN_BUDGET   24                 /* tiles baked per frame             */
#define MAX_DRAW     4096
#define MAX_REQ      1024
#define HEIGHT_UNIT  8                  /* texture unit of the height cache  */
#define GBUF_DEPTH_UNIT 9               /* ...of the mesh depth + normal     */
#define GBUF_NRM_UNIT   10
#define MAX_TERRAIN_BODIES 16             /* mesh on/off memory                */

/* Split while a tile's quads span more than SPLIT_PX; once split, merge only
 * below MERGE_PX, so a tile at the threshold does not flicker. Both scale by
 * s_lod_scale, which rises while the cache cannot hold the selection (a
 * grazing view near the ground asks for hundreds of tiles) and eases back once
 * it can: detail degrades evenly instead of stalling under the camera. */
#define SPLIT_PX     8.0
#define MERGE_PX     6.4

/* Handover: mesh from a pixel of relief on screen, back to the sphere below
 * RELIEF_OFF_PX; tiles start baking from RELIEF_PREFETCH_PX so the full
 * cover is ready when the switch comes. */
#define RELIEF_ON_PX       1.0
#define RELIEF_OFF_PX      0.7
#define RELIEF_PREFETCH_PX 0.5

/* Lowest point any tile reaches, in units of the relief amplitude
 * (planet_noise.glsl relief_shape minus the full detail sum). */
#define TERRAIN_HMIN (-1.2)

/* Detail octaves (terrain_gen.frag): 64 cycles per radius and up, amplitude
 * falling 0.55× per doubling — slopes steepen gently toward small scales, as
 * real relief does: on the Moon ~7° at tens of km, ~20° at tens of metres. */
#define OCT_F0   64.0
#define OCT_A0   0.08
#define OCT_GAIN 0.55

/* Cube faces: normal n, axes u, v with u × v = n. */
static const double FACE[6][3][3] = {
    { {  1, 0, 0 }, {  0, 0, -1 }, { 0, 1,  0 } },
    { { -1, 0, 0 }, {  0, 0,  1 }, { 0, 1,  0 } },
    { {  0, 1, 0 }, {  1, 0,  0 }, { 0, 0, -1 } },
    { {  0,-1, 0 }, {  1, 0,  0 }, { 0, 0,  1 } },
    { {  0, 0, 1 }, {  1, 0,  0 }, { 0, 1,  0 } },
    { {  0, 0,-1 }, { -1, 0,  0 }, { 0, 1,  0 } },
};

typedef struct {
    int          body;
    unsigned int name_hash;
    int          face, level, x, y;
} TileKey;

typedef struct {
    TileKey k;
    int     live;
    int     used;    /* last frame the walk touched it          */
    int     split;   /* last frame its children were drawn      */
    int     baked;   /* frame it was baked                      */
    int     ranged;  /* hmin/hmax read back from the GPU        */
    float   hmin, hmax;   /* height range, body radii          */
} Tile;

typedef struct {
    double sc, tc, w;
    double pc[3];        /* centre direction                    */
    double cos_ang;      /* cos of the widest centre–corner angle */
    double chord;        /* widest centre–corner chord (radii)  */
    double edge;         /* longest edge chord (radii)          */
    double hlo, hhi;     /* height bounds, radii (see visit)    */
} TileGeo;

typedef struct {
    TileKey k;
    int     layer;
    float   anchor[3];
    float   pc[3];
    float   skirt;
} DrawTile;

typedef struct { TileKey k; int level; double px; } Req;

typedef struct { int body; unsigned int name_hash; int on; int seen; } BodyState;

/* Per-update walk context: the camera in the body frame, cull bounds. */
typedef struct {
    const TerrainBody *tb;
    double cam_dir[3];     /* camera direction from the centre, body-local */
    double cam_d;          /* camera distance from the centre, radii       */
    double horizon;        /* widest visible angle from cam_dir, radians   */
    double planes[4][4];   /* side planes of the frustum, camera-relative  */
    double hext;           /* height extent for bounds, radii              */
} Walk;

typedef struct {
    GLint vp, cam_fwd, face_n, face_u, face_v, tile, anchor, l2w, pc, body_r, skirt, layer;
} MeshLocs;

static GLuint s_gen_prog, s_mesh_prog, s_tex, s_fbo, s_gen_vao;
/* The mesh's depth + normal buffer, sized to the viewport it is drawn in. */
static GLuint s_gb_fbo, s_gb_depth, s_gb_nrm;
static int    s_gb_w, s_gb_h;
static GLuint s_grid_vao, s_grid_vbo, s_grid_ebo;
static GLsizei s_grid_count;
static int    s_layers;

static GLint s_g_face_n, s_g_face_u, s_g_face_v, s_g_tile, s_g_ptype, s_g_amp,
             s_g_noct, s_g_oct_f, s_g_oct_i, s_g_oct_r, s_g_oct_a, s_g_oct_rot;
static double s_oct_rot[MAX_OCT][9];   /* per-octave lattice rotations, row-major */
static MeshLocs s_mesh_locs;

static Tile      s_tile[MAX_LAYERS];
static int       s_hash[HASH_CAP];
static int       s_free[MAX_LAYERS], s_nfree;
static int       s_frame = 1;
static double    s_lod_scale = 1.0;
static int       s_cache_full;      /* a bake found no free layer this frame */
static DrawTile  s_draw[MAX_DRAW];
static int       s_ndraw;
static Req       s_req[MAX_REQ];
static int       s_nreq;
static BodyState s_bs[MAX_TERRAIN_BODIES];

/* ------------------------------------------------------------ tile cache */

static unsigned int key_hash(const TileKey *k)
{
    uint64_t h = 1469598103934665603ULL;
    const uint64_t v[6] = { (uint64_t)(uint32_t)k->body, k->name_hash,
                            (uint64_t)k->face, (uint64_t)k->level,
                            (uint64_t)(uint32_t)k->x, (uint64_t)(uint32_t)k->y };
    for (int i = 0; i < 6; i++) { h ^= v[i]; h *= 1099511628211ULL; }
    return (unsigned int)(h ^ (h >> 32));
}

static int key_eq(const TileKey *a, const TileKey *b)
{
    return a->body == b->body && a->name_hash == b->name_hash &&
           a->face == b->face && a->level == b->level &&
           a->x == b->x && a->y == b->y;
}

static int lookup(const TileKey *k)
{
    unsigned int i = key_hash(k) & (HASH_CAP - 1);
    while (s_hash[i] >= 0) {
        if (key_eq(&s_tile[s_hash[i]].k, k)) return s_hash[i];
        i = (i + 1) & (HASH_CAP - 1);
    }
    return -1;
}

static void hash_insert(int layer)
{
    unsigned int i = key_hash(&s_tile[layer].k) & (HASH_CAP - 1);
    while (s_hash[i] >= 0) i = (i + 1) & (HASH_CAP - 1);
    s_hash[i] = layer;
}

/* Linear-probing delete by backward shift: later entries of the probe run
 * move up into the hole, so lookups never need tombstones. */
static void hash_remove(int layer)
{
    unsigned int i = key_hash(&s_tile[layer].k) & (HASH_CAP - 1);
    while (s_hash[i] != layer) i = (i + 1) & (HASH_CAP - 1);
    for (;;) {
        s_hash[i] = -1;
        unsigned int j = i;
        for (;;) {
            j = (j + 1) & (HASH_CAP - 1);
            if (s_hash[j] < 0) return;
            unsigned int home = key_hash(&s_tile[s_hash[j]].k) & (HASH_CAP - 1);
            int stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
            if (!stays) break;
        }
        s_hash[i] = s_hash[j];
        i = j;
    }
}

/* A free layer, or the least recently used one not touched this frame. */
static int alloc_layer(void)
{
    if (s_nfree > 0) return s_free[--s_nfree];
    int best = -1;
    for (int l = 0; l < s_layers; l++) {
        if (!s_tile[l].live || s_tile[l].used >= s_frame) continue;
        if (best < 0 || s_tile[l].used < s_tile[best].used) best = l;
    }
    if (best >= 0) {
        hash_remove(best);
        s_tile[best].live = 0;
    }
    return best;
}

/* ---------------------------------------------------------------- geometry */

static void v_norm(double v[3])
{
    double l = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;
}

static double v_dist(const double a[3], const double b[3])
{
    double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return sqrt(dx*dx + dy*dy + dz*dz);
}

static void cube_dir(int face, double s, double t, double out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = FACE[face][0][i] + s * FACE[face][1][i] + t * FACE[face][2][i];
    v_norm(out);
}

static void tile_geo(const TileKey *k, TileGeo *g)
{
    g->w  = 2.0 / (double)(1 << k->level);
    g->sc = -1.0 + g->w * (k->x + 0.5);
    g->tc = -1.0 + g->w * (k->y + 0.5);
    cube_dir(k->face, g->sc, g->tc, g->pc);
    double c[4][3], h = 0.5 * g->w;
    cube_dir(k->face, g->sc - h, g->tc - h, c[0]);
    cube_dir(k->face, g->sc + h, g->tc - h, c[1]);
    cube_dir(k->face, g->sc + h, g->tc + h, c[2]);
    cube_dir(k->face, g->sc - h, g->tc + h, c[3]);
    g->cos_ang = 1.0; g->chord = 0.0; g->edge = 0.0;
    for (int i = 0; i < 4; i++) {
        double d = g->pc[0]*c[i][0] + g->pc[1]*c[i][1] + g->pc[2]*c[i][2];
        if (d < g->cos_ang) g->cos_ang = d;
        double ch = v_dist(g->pc, c[i]);
        if (ch > g->chord) g->chord = ch;
        double e = v_dist(c[i], c[(i + 1) & 3]);
        if (e > g->edge) g->edge = e;
    }
}

static void local_to_world(const TerrainBody *tb, const double l[3], double w[3])
{
    const double *m = tb->l2w;
    w[0] = m[0]*l[0] + m[1]*l[1] + m[2]*l[2];
    w[1] = m[3]*l[0] + m[4]*l[1] + m[5]*l[2];
    w[2] = m[6]*l[0] + m[7]*l[1] + m[8]*l[2];
}

/* Tile bounding sphere, camera-relative world frame (AU): every point
 * P·(1 + h) with P within `chord` of the centre direction and h in
 * [hlo, hhi] lies within chord·(1 + hhi) + (hhi − hlo)/2 of pc·(1 + mid). */
static void tile_bounds(const Walk *wk, const TileGeo *g, double c[3], double *r)
{
    const TerrainBody *tb = wk->tb;
    double cw[3], mid = 0.5 * (g->hlo + g->hhi);
    local_to_world(tb, g->pc, cw);
    for (int i = 0; i < 3; i++) c[i] = tb->center_rel[i] + cw[i] * tb->radius * (1.0 + mid);
    *r = (g->chord * (1.0 + g->hhi) + 0.5 * (g->hhi - g->hlo)) * tb->radius;
}

/* A tile's height bounds: its own once read back; until then its parent's,
 * widened by what the finer octaves can add; for a root, the global bound. */
static void tile_range(const Walk *wk, const TileGeo *parent, int layer, TileGeo *g)
{
    if (layer >= 0 && s_tile[layer].ranged) {
        g->hlo = s_tile[layer].hmin;
        g->hhi = s_tile[layer].hmax;
    } else if (parent) {
        double m = 0.25 * wk->tb->amp;
        g->hlo = fmax(parent->hlo - m, -wk->hext);
        g->hhi = fmin(parent->hhi + m,  wk->hext);
    } else {
        g->hlo = -wk->hext;
        g->hhi =  wk->hext;
    }
}

static int culled(const Walk *wk, const TileGeo *g)
{
    /* Horizon: past the widest angle at which any point of the relief can
     * still peek over the planet's lowest ground. */
    if (wk->horizon < PI) {
        double d = g->pc[0]*wk->cam_dir[0] + g->pc[1]*wk->cam_dir[1] + g->pc[2]*wk->cam_dir[2];
        double a = acos(fmax(-1.0, fmin(1.0, d)));
        double half = acos(fmax(-1.0, fmin(1.0, g->cos_ang)));
        if (a - half > wk->horizon) return 1;
    }
    double c[3], r;
    tile_bounds(wk, g, c, &r);
    for (int p = 0; p < 4; p++) {
        const double *pl = wk->planes[p];
        if (pl[0]*c[0] + pl[1]*c[1] + pl[2]*c[2] + pl[3] < -r) return 1;
    }
    return 0;
}

/* Projected size of one quad of the tile, in pixels, at the tile's nearest
 * point to the camera. */
static double quad_px(const Walk *wk, const TileGeo *g)
{
    double c[3], r;
    tile_bounds(wk, g, c, &r);
    double R = wk->tb->radius;
    double d = sqrt(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]) - r;
    if (d < R * 1e-9) d = R * 1e-9;
    return g->edge * R / (TILE_N - 1) / d * wk->tb->px_per_rad;
}

/* ------------------------------------------------------------------ baking */

static void request(const TileKey *k, double px)
{
    for (int i = 0; i < s_nreq; i++)
        if (key_eq(&s_req[i].k, k)) return;
    if (s_nreq >= MAX_REQ) return;
    s_req[s_nreq].k = *k;
    s_req[s_nreq].level = k->level;
    s_req[s_nreq].px = px;
    s_nreq++;
}

static int req_cmp(const void *a, const void *b)
{
    const Req *x = a, *y = b;
    if (x->level != y->level) return x->level - y->level;   /* coarse first */
    return (x->px < y->px) - (x->px > y->px);               /* then biggest */
}

static void set_face_uniforms(GLint n, GLint u, GLint v, GLint tile,
                              const TileKey *k, const TileGeo *g)
{
    const double (*F)[3] = FACE[k->face];
    glUniform3f(n, (float)F[0][0], (float)F[0][1], (float)F[0][2]);
    glUniform3f(u, (float)F[1][0], (float)F[1][1], (float)F[1][2]);
    glUniform3f(v, (float)F[2][0], (float)F[2][1], (float)F[2][2]);
    glUniform3f(tile, (float)g->sc, (float)g->tc, (float)g->w);   /* dyadic: exact */
}

static void bake(const TerrainBody *tb, const TileKey *k, int layer)
{
    TileGeo g;
    tile_geo(k, &g);
    set_face_uniforms(s_g_face_n, s_g_face_u, s_g_face_v, s_g_tile, k, &g);
    glUniform1i(s_g_ptype, tb->ptype);
    glUniform1f(s_g_amp, (float)tb->amp);

    /* Detail octaves down to four texels a cycle (at the two-texel Nyquist
     * limit, neighbouring vertex normals alternate and the grid shows); the
     * last one fades in over a doubling so a split adds detail smoothly. The
     * same level always gets the same set, so neighbours agree. */
    float of[MAX_OCT], orr[MAX_OCT * 3], oa[MAX_OCT], orot[MAX_OCT * 9];
    int   oi[MAX_OCT * 3], n = 0;
    double fmax = (TILE_N - 1) / (4.0 * g.w);
    for (int o = 0; o < MAX_OCT; o++) {
        double f = OCT_F0 * ldexp(1.0, o);
        double t = log2(fmax / f);
        if (t <= 0.0) break;
        of[n] = (float)f;
        oa[n] = (float)(OCT_A0 * pow(OCT_GAIN, o) * fmin(t, 1.0));
        const double *m = s_oct_rot[o];
        for (int i = 0; i < 9; i++) orot[n*9 + i] = (float)m[i];
        for (int i = 0; i < 3; i++) {
            double c  = (m[i*3] * g.pc[0] + m[i*3 + 1] * g.pc[1] + m[i*3 + 2] * g.pc[2]) * f;
            double fl = floor(c);
            /* A per-body, per-octave lattice offset (< 2^20), so worlds and
             * octaves do not share the same rocks. */
            uint32_t s = (tb->name_hash ^ (uint32_t)(o * 0x9E3779B9u)) * (uint32_t)(2654435761u + 2u * (uint32_t)i);
            oi[n*3 + i]  = (int)fl + (int)(s >> 12);
            orr[n*3 + i] = (float)(c - fl);
        }
        n++;
    }
    glUniform1i(s_g_noct, n);
    if (n > 0) {
        glUniform1fv(s_g_oct_f, n, of);
        glUniform3iv(s_g_oct_i, n, oi);
        glUniform3fv(s_g_oct_r, n, orr);
        glUniform1fv(s_g_oct_a, n, oa);
        glUniformMatrix3fv(s_g_oct_rot, n, GL_TRUE, orot);
    }
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_tex, 0, layer);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    Tile *t = &s_tile[layer];
    t->k = *k;
    t->live = 1;
    t->used = s_frame;
    t->split = 0;
    t->baked = s_frame;
    t->ranged = 0;
    hash_insert(layer);
}

/* ------------------------------------------------------------ height ranges
 * Each baked tile's height range is reduced on the GPU into its texel of a
 * one-row strip, and the strip read back through a small ring of pixel
 * buffers, each fenced: a read is taken only once the GPU has finished it, so
 * the CPU never waits. Tiles are bounded by the global relief until then. */

#define MM_RING 3

static GLuint s_mm_prog, s_mm_tex, s_mm_fbo, s_mm_pbo[MM_RING];
static GLsync s_mm_fence[MM_RING];
static int    s_mm_issued[MM_RING];   /* frame of the read in the slot    */
static int    s_mm_head, s_mm_dirty;
static GLint  s_mm_layer;

static void ranges_poll(void)
{
    for (int k = 0; k < MM_RING; k++) {
        if (!s_mm_fence[k]) continue;
        if (glClientWaitSync(s_mm_fence[k], 0, 0) == GL_TIMEOUT_EXPIRED) continue;
        glDeleteSync(s_mm_fence[k]);
        s_mm_fence[k] = 0;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_mm_pbo[k]);
        const float *r = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                          (GLsizeiptr)s_layers * 2 * sizeof(float),
                                          GL_MAP_READ_BIT);
        if (r) {
            /* A layer re-baked since the read holds a different tile now. */
            for (int l = 0; l < s_layers; l++) {
                Tile *t = &s_tile[l];
                if (!t->live || t->ranged || t->baked > s_mm_issued[k]) continue;
                t->hmin = r[l * 2];
                t->hmax = r[l * 2 + 1];
                t->ranged = 1;
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
}

/* ------------------------------------------------------------ ground height
 * The surface a walker stands on must be the one drawn, not the noise it was
 * baked from, so the ground comes from the drawn tile itself: each frame a
 * PATCH_N² texel patch of the finest baked tile under the camera is read back
 * (fenced, like the ranges) and heights between its texels are interpolated
 * across the same triangle diagonal the mesh uses. A patch spans ±16 quads —
 * at the finest level a metre or so, several frames of running. */

#define PATCH_N    32
#define PATCH_RING 3
#define PATCH_KEEP 4

typedef struct {
    TileKey k;
    int     ti0, tj0;                   /* texel origin in the tile layer */
    float   h[PATCH_N * PATCH_N];
} Patch;

static GLuint s_pt_pbo[PATCH_RING];
static GLsync s_pt_fence[PATCH_RING];
static Patch  s_pt_pending[PATCH_RING];  /* what each read in flight holds */
static int    s_pt_head;
static Patch  s_pt_done[PATCH_KEEP];     /* newest first                  */
static int    s_pt_ndone;

/* Cube face and face coordinates of a body-local direction. */
static int dir_face(const double d[3], double *s, double *t)
{
    int best = 0;
    double bd = -2.0;
    for (int f = 0; f < 6; f++) {
        double v = d[0]*FACE[f][0][0] + d[1]*FACE[f][0][1] + d[2]*FACE[f][0][2];
        if (v > bd) { bd = v; best = f; }
    }
    const double (*F)[3] = FACE[best];
    *s = (d[0]*F[1][0] + d[1]*F[1][1] + d[2]*F[1][2]) / bd;
    *t = (d[0]*F[2][0] + d[1]*F[2][1] + d[2]*F[2][2]) / bd;
    return best;
}

/* The tile of a level holding face coordinates (s, t), and the mesh grid
 * position there (0..TILE_N-1). */
static void tile_at(int level, double s, double t, int *x, int *y, double *gx, double *gy)
{
    int n = 1 << level;
    double w = 2.0 / n;
    *x = (int)floor((s + 1.0) / w);
    *y = (int)floor((t + 1.0) / w);
    if (*x < 0) *x = 0;
    if (*x > n - 1) *x = n - 1;
    if (*y < 0) *y = 0;
    if (*y > n - 1) *y = n - 1;
    *gx = (s - (-1.0 + *x * w)) / w * (TILE_N - 1);
    *gy = (t - (-1.0 + *y * w)) / w * (TILE_N - 1);
}

/* The finest baked tile holding a direction, or -1. */
static int finest_tile(int body, unsigned int hash, const double d[3], TileKey *k,
                       double *gx, double *gy)
{
    double s, t;
    int f = dir_face(d, &s, &t);
    for (int L = MAX_LEVEL; L >= 0; L--) {
        TileKey c = { body, hash, f, L, 0, 0 };
        tile_at(L, s, t, &c.x, &c.y, gx, gy);
        int l = lookup(&c);
        if (l >= 0) { *k = c; return l; }
    }
    return -1;
}

static void ground_poll(void)
{
    for (int r = 0; r < PATCH_RING; r++) {
        if (!s_pt_fence[r]) continue;
        if (glClientWaitSync(s_pt_fence[r], 0, 0) == GL_TIMEOUT_EXPIRED) continue;
        glDeleteSync(s_pt_fence[r]);
        s_pt_fence[r] = 0;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pt_pbo[r]);
        const float *h = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                          PATCH_N * PATCH_N * sizeof(float), GL_MAP_READ_BIT);
        if (h) {
            memmove(&s_pt_done[1], &s_pt_done[0], (PATCH_KEEP - 1) * sizeof(Patch));
            s_pt_done[0] = s_pt_pending[r];
            memcpy(s_pt_done[0].h, h, sizeof s_pt_done[0].h);
            if (s_pt_ndone < PATCH_KEEP) s_pt_ndone++;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
}

/* Read the patch of the finest tile under the camera. */
static void ground_request(const Walk *wk)
{
    if (s_pt_fence[s_pt_head]) return;               /* slot still in flight */
    TileKey k;
    double gx, gy;
    int layer = finest_tile(wk->tb->body, wk->tb->name_hash, wk->cam_dir, &k, &gx, &gy);
    if (layer < 0) return;
    Patch *p = &s_pt_pending[s_pt_head];
    p->k   = k;
    p->ti0 = (int)floor(gx + 1.0) - PATCH_N / 2;     /* texel = grid + 1 */
    p->tj0 = (int)floor(gy + 1.0) - PATCH_N / 2;
    if (p->ti0 < 0) p->ti0 = 0;
    if (p->tj0 < 0) p->tj0 = 0;
    if (p->ti0 > TEX_N - PATCH_N) p->ti0 = TEX_N - PATCH_N;
    if (p->tj0 > TEX_N - PATCH_N) p->tj0 = TEX_N - PATCH_N;

    GLint fbo;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_tex, 0, layer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pt_pbo[s_pt_head]);
    glReadPixels(p->ti0, p->tj0, PATCH_N, PATCH_N, GL_RED, GL_FLOAT, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)fbo);
    s_pt_fence[s_pt_head] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s_pt_head = (s_pt_head + 1) % PATCH_RING;
}

int terrain_ground(int body, unsigned int name_hash, const double dir[3], double *h, int *level)
{
    double s, t;
    int f = dir_face(dir, &s, &t);
    for (int n = 0; n < s_pt_ndone; n++) {
        const Patch *p = &s_pt_done[n];
        if (p->k.body != body || p->k.name_hash != name_hash || p->k.face != f) continue;
        int x, y;
        double gx, gy;
        tile_at(p->k.level, s, t, &x, &y, &gx, &gy);
        if (x != p->k.x || y != p->k.y) continue;
        int i = (int)floor(gx), j = (int)floor(gy);
        if (i > TILE_N - 2) i = TILE_N - 2;
        if (j > TILE_N - 2) j = TILE_N - 2;
        int pi = i + 1 - p->ti0, pj = j + 1 - p->tj0;   /* cell corner in the patch */
        if (pi < 0 || pj < 0 || pi + 1 >= PATCH_N || pj + 1 >= PATCH_N) continue;
        double fx = gx - i, fy = gy - j;
        double ha = p->h[pj * PATCH_N + pi],           hb = p->h[pj * PATCH_N + pi + 1];
        double hc = p->h[(pj + 1) * PATCH_N + pi + 1], hd = p->h[(pj + 1) * PATCH_N + pi];
        /* The mesh splits each quad along a→c (terrain.c build_grid). */
        *h = (fx >= fy) ? ha + fx * (hb - ha) + fy * (hc - hb)
                        : ha + fy * (hd - ha) + fx * (hc - hd);
        if (level) *level = p->k.level;
        return 1;
    }
    /* No patch here yet: the finest tile's top is at least a safe floor. */
    TileKey k;
    double gx, gy;
    int l = finest_tile(body, name_hash, dir, &k, &gx, &gy);
    if (l >= 0 && s_tile[l].ranged) {
        *h = s_tile[l].hmax;
        if (level) *level = k.level;
        return 2;
    }
    return 0;
}

unsigned int terrain_name_hash(const char *name)
{
    unsigned int h = 2166136261u;
    for (; *name; name++) h = (h ^ (unsigned char)*name) * 16777619u;
    return h;
}

void terrain_local_to_world(double rotation_angle, double obliquity_deg, double m[9])
{
    /* phong.frag local_surface_dir_to_world(): spin about y, then tilt. */
    double rot = fmod(rotation_angle, 2.0 * PI), obl = obliquity_deg * (PI / 180.0);
    double cr = cos(rot), sr = sin(rot), co = cos(obl), so = sin(obl);
    const double l2w[9] = { co * cr,  so, -co * sr,
                            -so * cr, co,  so * sr,
                            sr,      0.0,  cr };
    memcpy(m, l2w, sizeof l2w);
}

static void ranges_issue(void)
{
    if (!s_mm_dirty || s_mm_fence[s_mm_head]) return;   /* slot still in flight */
    GLint fbo;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_mm_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_mm_pbo[s_mm_head]);
    glReadPixels(0, 0, s_layers, 1, GL_RG, GL_FLOAT, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)fbo);
    s_mm_fence[s_mm_head]  = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s_mm_issued[s_mm_head] = s_frame;
    s_mm_head = (s_mm_head + 1) % MM_RING;
    s_mm_dirty = 0;
}

/* Bake the most urgent requests, within the frame budget. */
static void bake_requests(const TerrainBody *tb)
{
    if (s_nreq == 0) return;
    qsort(s_req, (size_t)s_nreq, sizeof(Req), req_cmp);

    GLint prog, fbo, vao, vp[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, TEX_N, TEX_N);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_gen_prog);
    glBindVertexArray(s_gen_vao);

    int baked = 0, layers[GEN_BUDGET];
    for (int i = 0; i < s_nreq && baked < GEN_BUDGET; i++) {
        if (lookup(&s_req[i].k) >= 0) continue;
        int layer = alloc_layer();
        if (layer < 0) { s_cache_full = 1; break; }   /* all in use this frame */
        bake(tb, &s_req[i].k, layer);
        layers[baked++] = layer;
    }
    s_nreq = 0;

    /* Their height ranges, one texel each, for the read-back. */
    if (baked > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_mm_fbo);
        glUseProgram(s_mm_prog);
        glActiveTexture(GL_TEXTURE0 + HEIGHT_UNIT);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_tex);
        glActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < baked; i++) {
            glViewport(layers[i], 0, 1, 1);
            glUniform1i(s_mm_layer, layers[i]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        s_mm_dirty = 1;
    }
    ranges_issue();

    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
}

/* ---------------------------------------------------------------- the walk */

static void push_draw(const Walk *wk, const TileKey *k, const TileGeo *g, int layer)
{
    if (s_ndraw >= MAX_DRAW) return;
    const TerrainBody *tb = wk->tb;
    DrawTile *d = &s_draw[s_ndraw++];
    double cw[3];
    local_to_world(tb, g->pc, cw);
    for (int i = 0; i < 3; i++) {
        d->anchor[i] = (float)(tb->center_rel[i] + cw[i] * tb->radius);
        d->pc[i]     = (float)g->pc[i];
    }
    d->k = *k;
    d->layer = layer;
    /* Deep enough to cover the step to any coarser neighbour: the tree does
     * not keep neighbours within a level of each other, and a neighbour many
     * levels coarser runs straight across relief this tile resolves. Its
     * surface along the shared edge stays within this tile's height range, so
     * a skirt as deep as that range (plus a quad of slope) always meets it. */
    d->skirt = (float)(g->hhi - g->hlo + g->edge / (TILE_N - 1));
}

static void visit(const Walk *wk, const TileKey *k, const TileGeo *parent)
{
    TileGeo g;
    tile_geo(k, &g);
    int layer = lookup(k);
    tile_range(wk, parent, layer, &g);
    if (culled(wk, &g)) return;
    if (layer < 0) return;                /* only reachable for an unbaked root */
    Tile *t = &s_tile[layer];
    t->used = s_frame;

    double px  = quad_px(wk, &g);
    double thr = s_lod_scale * ((t->split == s_frame - 1) ? MERGE_PX : SPLIT_PX);
    if (k->level < MAX_LEVEL && px > thr) {
        TileKey ch[4];
        int ready = 1;
        for (int c = 0; c < 4; c++) {
            ch[c] = *k;
            ch[c].level = k->level + 1;
            ch[c].x = 2 * k->x + (c & 1);
            ch[c].y = 2 * k->y + (c >> 1);
            TileGeo cg;
            tile_geo(&ch[c], &cg);
            int cl = lookup(&ch[c]);
            tile_range(wk, &g, cl, &cg);
            if (culled(wk, &cg)) continue;   /* invisible: needs no bake */
            if (cl >= 0) s_tile[cl].used = s_frame;   /* keep while siblings bake */
            else { ready = 0; request(&ch[c], px); }
        }
        if (ready) {
            t->split = s_frame;
            for (int c = 0; c < 4; c++) visit(wk, &ch[c], &g);
            return;
        }
    }
    push_draw(wk, k, &g, layer);
}

static BodyState *body_state(const TerrainBody *tb)
{
    BodyState *old = &s_bs[0];
    for (int i = 0; i < MAX_TERRAIN_BODIES; i++) {
        if (s_bs[i].seen && s_bs[i].body == tb->body && s_bs[i].name_hash == tb->name_hash)
            return &s_bs[i];
        if (s_bs[i].seen < old->seen) old = &s_bs[i];
    }
    old->body = tb->body;
    old->name_hash = tb->name_hash;
    old->on = 0;
    old->seen = s_frame;
    return old;
}

int terrain_update(const TerrainBody *tb, double relief_px)
{
    s_ndraw = 0;
    if (!s_mesh_prog || tb->amp <= 0.0) return 0;
    ranges_poll();
    ground_poll();
    BodyState *bs = body_state(tb);
    /* A body not seen last frame starts from the sphere again. */
    if (bs->seen < s_frame - 1) bs->on = 0;
    bs->seen = s_frame;
    bs->on = relief_px >= (bs->on ? RELIEF_OFF_PX : RELIEF_ON_PX);
    if (!bs->on && relief_px < RELIEF_PREFETCH_PX) return 0;

    Walk wk;
    wk.tb = tb;
    double R = tb->radius;
    /* Camera in the body frame: −centre_rel through the inverse rotation. */
    const double *m = tb->l2w;
    double cw[3] = { -tb->center_rel[0] / R, -tb->center_rel[1] / R, -tb->center_rel[2] / R };
    double cl[3] = { m[0]*cw[0] + m[3]*cw[1] + m[6]*cw[2],
                     m[1]*cw[0] + m[4]*cw[1] + m[7]*cw[2],
                     m[2]*cw[0] + m[5]*cw[1] + m[8]*cw[2] };
    wk.cam_d = sqrt(cl[0]*cl[0] + cl[1]*cl[1] + cl[2]*cl[2]);
    for (int i = 0; i < 3; i++) wk.cam_dir[i] = cl[i] / (wk.cam_d > 0.0 ? wk.cam_d : 1.0);
    double r_min = 1.0 + tb->amp * TERRAIN_HMIN;
    double r_max = 1.0 + tb->amp * TERRAIN_HMAX;
    wk.horizon = (wk.cam_d > r_min) ? acos(r_min / wk.cam_d) + acos(r_min / r_max) : PI;
    wk.hext = tb->amp * fmax(TERRAIN_HMAX, -TERRAIN_HMIN);

    /* Side planes of the frustum (Gribb–Hartmann, column-major vp); the near
     * and far planes are left out — depth is clamped and logarithmic. */
    const float *v = tb->vp;
    for (int p = 0; p < 4; p++) {
        int row = p >> 1;
        double sg = (p & 1) ? -1.0 : 1.0;
        for (int i = 0; i < 4; i++)
            wk.planes[p][i] = (double)v[i*4 + 3] + sg * (double)v[i*4 + row];
        double l = sqrt(wk.planes[p][0]*wk.planes[p][0] + wk.planes[p][1]*wk.planes[p][1] +
                        wk.planes[p][2]*wk.planes[p][2]);
        if (l > 0.0) for (int i = 0; i < 4; i++) wk.planes[p][i] /= l;
    }

    int roots = 1;
    for (int f = 0; f < 6; f++) {
        TileKey k = { tb->body, tb->name_hash, f, 0, 0, 0 };
        int l = lookup(&k);
        if (l < 0) { roots = 0; request(&k, 1e30); }
        else s_tile[l].used = s_frame;
    }
    if (roots) {
        for (int f = 0; f < 6; f++) {
            TileKey k = { tb->body, tb->name_hash, f, 0, 0, 0 };
            visit(&wk, &k, NULL);
        }
    }
    bake_requests(tb);
    if (roots) ground_request(&wk);
    if (!roots || !bs->on) s_ndraw = 0;
    return roots && bs->on;
}

static void draw_tiles(const TerrainBody *tb, const MeshLocs *L)
{
    float l2w[9];
    for (int i = 0; i < 9; i++) l2w[i] = (float)tb->l2w[i];
    glUniformMatrix4fv(L->vp, 1, GL_FALSE, tb->vp);
    glUniform3fv(L->cam_fwd, 1, tb->cam_fwd);
    glUniformMatrix3fv(L->l2w, 1, GL_TRUE, l2w);
    glUniform1f(L->body_r, (float)tb->radius);
    for (int i = 0; i < s_ndraw; i++) {
        const DrawTile *d = &s_draw[i];
        TileGeo g;
        tile_geo(&d->k, &g);
        set_face_uniforms(L->face_n, L->face_u, L->face_v, L->tile, &d->k, &g);
        glUniform3fv(L->anchor, 1, d->anchor);
        glUniform3fv(L->pc, 1, d->pc);
        glUniform1f(L->skirt, d->skirt);
        glUniform1i(L->layer, d->layer);
        glDrawElements(GL_TRIANGLES, s_grid_count, GL_UNSIGNED_SHORT, 0);
    }
}

static void gbuf_release(void)
{
    if (s_gb_fbo)   glDeleteFramebuffers(1, &s_gb_fbo);
    if (s_gb_depth) glDeleteTextures(1, &s_gb_depth);
    if (s_gb_nrm)   glDeleteTextures(1, &s_gb_nrm);
    s_gb_fbo = s_gb_depth = s_gb_nrm = 0;
    s_gb_w = s_gb_h = 0;
}

static GLuint gbuf_tex(GLint ifmt, GLenum fmt, GLenum type, int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

/* (Re)allocate the buffer to cover the viewport: gl_FragCoord must address
 * the same texel in the mesh pass and the shading pass. Made on first use,
 * so a session that never comes close to a planet never pays for it. */
static int gbuf_ensure(int w, int h)
{
    if (s_gb_fbo && s_gb_w == w && s_gb_h == h) return 1;
    gbuf_release();
    s_gb_depth = gbuf_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, w, h);
    s_gb_nrm   = gbuf_tex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, w, h);
    glGenFramebuffers(1, &s_gb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_gb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_gb_nrm, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_gb_depth, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        fprintf(stderr, "[terrain] mesh buffer incomplete; planets stay spheres\n");
        gbuf_release();
        return 0;
    }
    s_gb_w = w;
    s_gb_h = h;
    return 1;
}

int terrain_render(const TerrainBody *tb, GLuint surface_prog)
{
    if (s_ndraw == 0) return 0;

    GLint fbo, vao, prog, vp[4];
    GLfloat clear_col[4], clear_depth;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
    if (!gbuf_ensure(vp[0] + vp[2], vp[1] + vp[3])) { s_ndraw = 0; return 0; }

    glBindFramebuffer(GL_FRAMEBUFFER, s_gb_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0 + HEIGHT_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tex);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(s_mesh_prog);
    glBindVertexArray(s_grid_vao);
    draw_tiles(tb, &s_mesh_locs);

    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glClearColor(clear_col[0], clear_col[1], clear_col[2], clear_col[3]);
    glClearDepth(clear_depth);

    glActiveTexture(GL_TEXTURE0 + GBUF_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, s_gb_depth);
    glActiveTexture(GL_TEXTURE0 + GBUF_NRM_UNIT);
    glBindTexture(GL_TEXTURE_2D, s_gb_nrm);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(surface_prog, "u_terrain_depth"), GBUF_DEPTH_UNIT);
    glUniform1i(glGetUniformLocation(surface_prog, "u_terrain_nrm"),   GBUF_NRM_UNIT);
    s_ndraw = 0;
    return 1;
}

void terrain_frame_begin(void)
{
    s_frame++;
    /* Coarsen quickly while the cache overflows; refine slowly while a
     * quarter of it is free. */
    if (s_cache_full) s_lod_scale = fmin(s_lod_scale * 1.08, 16.0);
    else if (s_nfree > s_layers / 4) s_lod_scale = fmax(s_lod_scale / 1.01, 1.0);
    s_cache_full = 0;
}

double terrain_slope_weight(double relief_px)
{
    double t = (relief_px - RELIEF_ON_PX) / (4.0 * RELIEF_ON_PX - RELIEF_ON_PX);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

/* ------------------------------------------------------------------- setup */

double terrain_relief_amp(int ptype, int earth_imagery)
{
    switch (ptype) {
    case 14: return 4.0e-3;                         /* Moon, Mercury: ±7 km    */
    case 1:  return earth_imagery ? 0.0 : 1.2e-3;   /* land to ~9 km           */
    case 2:  return 5.0e-3;                         /* Mars: ±15 km            */
    case 0:
    case 10: return 3.0e-3;                         /* generic rocky           */
    case 7:  return 2.0e-3;                         /* Io                      */
    case 9:  return 5.0e-4;                         /* Europa: smooth ice      */
    default: return 0.0;                            /* cloud tops, gas, stars  */
    }
}

static void build_grid(void)
{
    const int N = TILE_N;
    int nv = N * N + 4 * N;
    float *v = malloc((size_t)nv * 3 * sizeof(float));
    unsigned short *ix = malloc((size_t)((N - 1) * (N - 1) * 6 + 4 * (N - 1) * 6) * sizeof(unsigned short));
    if (!v || !ix) { free(v); free(ix); return; }
    int n = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) { v[n*3] = (float)i; v[n*3+1] = (float)j; v[n*3+2] = 0.0f; n++; }
    /* Skirt rings: bottom, top, left, right edges, one flagged copy each. */
    int base[4];
    for (int e = 0; e < 4; e++) {
        base[e] = n;
        for (int s = 0; s < N; s++) {
            int i = (e == 0 || e == 1) ? s : (e == 2 ? 0 : N - 1);
            int j = (e == 2 || e == 3) ? s : (e == 0 ? 0 : N - 1);
            v[n*3] = (float)i; v[n*3+1] = (float)j; v[n*3+2] = 1.0f; n++;
        }
    }
    int m = 0;
    for (int j = 0; j < N - 1; j++)
        for (int i = 0; i < N - 1; i++) {
            unsigned short a = (unsigned short)(j * N + i), b = (unsigned short)(a + 1),
                           c = (unsigned short)(a + N + 1), d = (unsigned short)(a + N);
            ix[m++] = a; ix[m++] = b; ix[m++] = c;
            ix[m++] = a; ix[m++] = c; ix[m++] = d;
        }
    for (int e = 0; e < 4; e++)
        for (int s = 0; s < N - 1; s++) {
            int i0 = (e == 0 || e == 1) ? s : (e == 2 ? 0 : N - 1);
            int j0 = (e == 2 || e == 3) ? s : (e == 0 ? 0 : N - 1);
            int di = (e == 0 || e == 1), dj = !di;
            unsigned short a  = (unsigned short)(j0 * N + i0);
            unsigned short b  = (unsigned short)((j0 + dj) * N + i0 + di);
            unsigned short a2 = (unsigned short)(base[e] + s), b2 = (unsigned short)(base[e] + s + 1);
            ix[m++] = a; ix[m++] = b; ix[m++] = b2;
            ix[m++] = a; ix[m++] = b2; ix[m++] = a2;
        }
    s_grid_count = m;

    glGenVertexArrays(1, &s_grid_vao);
    glBindVertexArray(s_grid_vao);
    glGenBuffers(1, &s_grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * 3 * sizeof(float)), v, GL_STATIC_DRAW);
    glGenBuffers(1, &s_grid_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_grid_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)m * sizeof(unsigned short)), ix, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glBindVertexArray(0);
    free(v);
    free(ix);
}

static void mesh_locs_fetch(GLuint prog, MeshLocs *L)
{
    L->vp      = glGetUniformLocation(prog, "u_vp");
    L->cam_fwd = glGetUniformLocation(prog, "u_cam_fwd");
    L->face_n = glGetUniformLocation(prog, "u_face_n");
    L->face_u = glGetUniformLocation(prog, "u_face_u");
    L->face_v = glGetUniformLocation(prog, "u_face_v");
    L->tile   = glGetUniformLocation(prog, "u_tile");
    L->anchor = glGetUniformLocation(prog, "u_anchor");
    L->l2w    = glGetUniformLocation(prog, "u_l2w");
    L->pc     = glGetUniformLocation(prog, "u_pc");
    L->body_r = glGetUniformLocation(prog, "u_body_r");
    L->skirt  = glGetUniformLocation(prog, "u_skirt");
    L->layer  = glGetUniformLocation(prog, "u_layer");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_heights"), HEIGHT_UNIT);
    glUseProgram(0);
}

int terrain_init(void)
{
    s_gen_prog  = gl_shader_load("assets/shaders/terrain_gen.vert", "assets/shaders/terrain_gen.frag");
    s_mesh_prog = gl_shader_load("assets/shaders/terrain.vert", "assets/shaders/terrain_gbuf.frag");
    if (!s_gen_prog || !s_mesh_prog) {
        fprintf(stderr, "[terrain] shaders failed; planets stay spheres\n");
        terrain_shutdown();
        return 0;
    }
    s_g_face_n = glGetUniformLocation(s_gen_prog, "u_face_n");
    s_g_face_u = glGetUniformLocation(s_gen_prog, "u_face_u");
    s_g_face_v = glGetUniformLocation(s_gen_prog, "u_face_v");
    s_g_tile   = glGetUniformLocation(s_gen_prog, "u_tile");
    s_g_ptype  = glGetUniformLocation(s_gen_prog, "u_ptype");
    s_g_amp    = glGetUniformLocation(s_gen_prog, "u_amp");
    s_g_noct   = glGetUniformLocation(s_gen_prog, "u_noct");
    s_g_oct_f  = glGetUniformLocation(s_gen_prog, "u_oct_f[0]");
    s_g_oct_i  = glGetUniformLocation(s_gen_prog, "u_oct_i[0]");
    s_g_oct_r  = glGetUniformLocation(s_gen_prog, "u_oct_r[0]");
    s_g_oct_a  = glGetUniformLocation(s_gen_prog, "u_oct_a[0]");
    s_g_oct_rot = glGetUniformLocation(s_gen_prog, "u_oct_rot[0]");
    /* A fixed, different rotation per octave: axis walks the sphere by the
     * golden angle, angle by the golden ratio. */
    for (int o = 0; o < MAX_OCT; o++) {
        double z = 1.0 - (2.0 * o + 1.0) / MAX_OCT, r = sqrt(1.0 - z * z);
        double ph = o * 2.399963229728653, x = r * cos(ph), y = r * sin(ph);
        double a = fmod(o * 0.6180339887 * 2.0 * PI, 2.0 * PI) + 0.5;
        double c = cos(a), sn = sin(a), t = 1.0 - c;
        double m[9] = { t*x*x + c,   t*x*y - sn*z, t*x*z + sn*y,
                        t*x*y + sn*z, t*y*y + c,   t*y*z - sn*x,
                        t*x*z - sn*y, t*y*z + sn*x, t*z*z + c };
        memcpy(s_oct_rot[o], m, sizeof m);
    }

    mesh_locs_fetch(s_mesh_prog, &s_mesh_locs);

    GLint max_layers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    s_layers = max_layers < MAX_LAYERS ? max_layers : MAX_LAYERS;
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R32F, TEX_N, TEX_N, s_layers, 0,
                 GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_tex, 0, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[terrain] height cache not renderable (0x%x); planets stay spheres\n", st);
        terrain_shutdown();
        return 0;
    }
    glGenVertexArrays(1, &s_gen_vao);
    build_grid();

    s_mm_prog = gl_shader_load("assets/shaders/terrain_gen.vert", "assets/shaders/terrain_minmax.frag");
    if (!s_mm_prog) {
        fprintf(stderr, "[terrain] range shader failed; planets stay spheres\n");
        terrain_shutdown();
        return 0;
    }
    s_mm_layer = glGetUniformLocation(s_mm_prog, "u_layer");
    glUseProgram(s_mm_prog);
    glUniform1i(glGetUniformLocation(s_mm_prog, "u_heights"), HEIGHT_UNIT);
    glUseProgram(0);
    glGenTextures(1, &s_mm_tex);
    glBindTexture(GL_TEXTURE_2D, s_mm_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, s_layers, 1, 0, GL_RG, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &s_mm_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_mm_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_mm_tex, 0);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[terrain] range strip not renderable (0x%x); planets stay spheres\n", st);
        terrain_shutdown();
        return 0;
    }
    glGenBuffers(PATCH_RING, s_pt_pbo);
    for (int r = 0; r < PATCH_RING; r++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pt_pbo[r]);
        glBufferData(GL_PIXEL_PACK_BUFFER, PATCH_N * PATCH_N * sizeof(float), NULL, GL_STREAM_READ);
    }
    glGenBuffers(MM_RING, s_mm_pbo);
    for (int k = 0; k < MM_RING; k++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_mm_pbo[k]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)s_layers * 2 * sizeof(float), NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    for (int i = 0; i < HASH_CAP; i++) s_hash[i] = -1;
    s_nfree = 0;
    for (int l = s_layers - 1; l >= 0; l--) s_free[s_nfree++] = l;
    return 1;
}

void terrain_shutdown(void)
{
    if (s_gen_prog)  glDeleteProgram(s_gen_prog);
    if (s_mesh_prog) glDeleteProgram(s_mesh_prog);
    if (s_mm_prog)   glDeleteProgram(s_mm_prog);
    if (s_mm_tex)    glDeleteTextures(1, &s_mm_tex);
    if (s_mm_fbo)    glDeleteFramebuffers(1, &s_mm_fbo);
    for (int k = 0; k < MM_RING; k++) {
        if (s_mm_fence[k]) glDeleteSync(s_mm_fence[k]);
        s_mm_fence[k] = 0;
    }
    if (s_mm_pbo[0]) glDeleteBuffers(MM_RING, s_mm_pbo);
    for (int r = 0; r < PATCH_RING; r++) {
        if (s_pt_fence[r]) glDeleteSync(s_pt_fence[r]);
        s_pt_fence[r] = 0;
    }
    if (s_pt_pbo[0]) glDeleteBuffers(PATCH_RING, s_pt_pbo);
    for (int r = 0; r < PATCH_RING; r++) s_pt_pbo[r] = 0;
    s_pt_ndone = 0;
    s_mm_prog = s_mm_tex = s_mm_fbo = 0;
    for (int k = 0; k < MM_RING; k++) s_mm_pbo[k] = 0;
    gbuf_release();
    if (s_tex)       glDeleteTextures(1, &s_tex);
    if (s_fbo)       glDeleteFramebuffers(1, &s_fbo);
    if (s_gen_vao)   glDeleteVertexArrays(1, &s_gen_vao);
    if (s_grid_vao)  glDeleteVertexArrays(1, &s_grid_vao);
    if (s_grid_vbo)  glDeleteBuffers(1, &s_grid_vbo);
    if (s_grid_ebo)  glDeleteBuffers(1, &s_grid_ebo);
    s_gen_prog = s_mesh_prog = s_tex = s_fbo = s_gen_vao = 0;
    s_grid_vao = s_grid_vbo = s_grid_ebo = 0;
}
