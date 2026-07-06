/*
 * labels.c — body label rendering
 *
 * Pipeline each frame:
 *   1. SDL_TTF renders each body name to an RGBA surface → GL texture (once,
 *      on labels_init or labels_add_body).
 *   2. Project label anchor to screen; compute axis-aligned bounding rects.
 *   3. Sort candidates by priority (stars > planets > moons, within each
 *      tier by camera distance).
 *   4. Greedy AABB overlap removal: iterate in priority order; a label is
 *      rejected if its rect intersects any already-confirmed label's rect.
 *   5. Hysteresis debounce: labels must be continuously eligible for
 *      SHOW_DELAY seconds before appearing, and continuously blocked for
 *      HIDE_DELAY seconds before disappearing.  Prevents flickering when a
 *      label sits on the overlap boundary.
 *   6. Draw surviving labels as billboards using camera right/up vectors.
 *
 * ── Label sizing ─────────────────────────────────────────────────────────
 *
 * Labels maintain a constant pixel height (LABEL_PX_H) on screen, regardless
 * of body distance.  The shader computes world-space width/height from eye_z:
 *   fh = eye_z × 2 × LABEL_PX_H × tan(FOV/2) / WIN_H
 *
 * eye_z is used instead of Euclidean dcam because eye_z is stable under
 * pure camera rotation: rotating does not change the forward-axis depth of a
 * stationary body.  dcam = eye_z / cos(θ) — rotating the camera changes θ
 * and therefore dcam, causing the label size to change when only the camera
 * rotates, not when the body actually moves.
 *
 * ── Camera-relative positioning ──────────────────────────────────────────
 *
 * Anchor positions are computed as (body_pos × RS − cam_pos) in double
 * precision before casting to float.  The vp passed in is proj × view_rot
 * (no translation), matching the convention used by the sphere and dot passes.
 *
 * ── Label appearance ─────────────────────────────────────────────────────
 *
 * Moon labels are rendered in italic to visually distinguish them from planets.
 * Label color is body_col × 1.4 + 0.15, clamped to 1.0 — brightened above
 * the body's diffuse color so labels remain legible against dark backgrounds.
 *
 * ── Camera-following slot cache (galaxy scale) ─────────────────────────────
 *
 * There are only MAX_BODIES texture/state slots but a galaxy holds ~16k bodies,
 * so slots are a *cache keyed by cache-slot index, not body index*.  Each frame
 * physics_active_bodies() returns the bodies in systems near the camera; every
 * active body is assigned a slot (its name texture built lazily on first use),
 * and slots holding bodies that have left the active region are evicted to make
 * room.  The result: labels follow the camera across the galaxy — a system gets
 * labelled as you approach and releases its slots as you leave — instead of
 * being pinned to the first MAX_BODIES body indices.  s_slot_body[slot] records
 * which body each slot holds (-1 = empty); everything below is indexed by slot.
 */
#include "labels.h"
#include "body.h"
#include "universe.h"   /* g_field_star_begin/end */
#include "camera.h"
#include "gl_utils.h"
#include "math3d.h"
#include "physics.h"
#include "settings.h"
#include "ui_theme.h"

/* Hard far cutoff: non-star labels are suppressed beyond g_settings.label_max_dist_au
 * (AU), adjustable in the menu.  Stars are always shown regardless of distance
 * (they are visible at any range within the camera's active region).
 *
 * Pinned nearest set: the nearest g_settings.label_pin_planets planets and
 * g_settings.label_pin_systems star systems are additionally fed into the
 * cache every frame and bypass both gates above — pinned planets ignore
 * label_max_dist_au, and pinned systems are labelled even when their system
 * is outside the camera's active region — so the closest names never vanish
 * while flying between systems. */

/* Upper bound on each pinned count (menu sliders stay within this). */
#define LABEL_PIN_MAX 16

/* Close cutoff in body radii: suppresses the label when the camera is too close
 * to the body disc.  Stars use a larger threshold because glare dominates earlier. */
#define MIN_LABEL_RADII       10
#define STAR_MIN_LABEL_RADII  80

#define LBL_PAD      6.0f   /* extra pixels added to rect for overlap comparison */
#define LABEL_PX_H  14.0f   /* desired label height in pixels                    */

/* ── GL resources ───────────────────────────────────────────────────────── */

static GLuint s_shader   = 0;
static GLuint s_vao      = 0;
static GLuint s_vbo      = 0;
static GLuint s_ebo      = 0;

static GLint  s_loc_vp     = -1;
static GLint  s_loc_anchor = -1;
static GLint  s_loc_right  = -1;
static GLint  s_loc_up     = -1;
static GLint  s_loc_tex    = -1;

/* Per-slot SDL_TTF-rendered name textures (see "slot cache" note above). */
static GLuint s_tex[MAX_BODIES];
static int    s_tex_w[MAX_BODIES];
static int    s_tex_h[MAX_BODIES];

/* Body index occupying each cache slot, or -1 when the slot is empty. */
static int    s_slot_body[MAX_BODIES];

/* ── Hysteresis state ───────────────────────────────────────────────────── */
/*
 * Labels use a two-threshold hysteresis to prevent rapid show/hide cycling
 * when a label sits on the edge of the overlap-rejection zone:
 *
 *   s_show_accum[i] — seconds the label has continuously been eligible.
 *                     Once this reaches SHOW_DELAY, s_active[i] is set to 1.
 *   s_hide_accum[i] — seconds the label has continuously been blocked/absent.
 *                     Once this reaches HIDE_DELAY, s_active[i] is set to 0.
 *
 * The show threshold is longer (0.20 s) to absorb momentary occlusion from
 * passing bodies without causing a flash.  The hide threshold is shorter
 * (0.06 s) so labels disappear promptly when the camera approaches a body.
 */
#define SHOW_DELAY 0.20f
#define HIDE_DELAY 0.06f

static float s_show_accum[MAX_BODIES];   /* indexed by cache slot */
static float s_hide_accum[MAX_BODIES];   /* indexed by cache slot */
static int   s_active[MAX_BODIES];       /* indexed by cache slot */

static TTF_Font *s_font = NULL;

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Upload an SDL_Surface as a GL_RGBA texture and free the surface. */
static GLuint surface_to_texture(SDL_Surface *surf, int *w, int *h) {
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!conv) return 0;

    *w = conv->w; *h = conv->h;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, conv->w, conv->h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, conv->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(conv);
    return tex;
}

/*
 * build_label_texture — render body `b`'s name into cache slot `slot` via
 * SDL_TTF.
 *
 * Color: body_col × 1.4 + 0.15 clamped to [0,1], converted to uint8.
 * Brightens the body color so the label is legible on dark backgrounds.
 *
 * Style: moons (parent exists and parent is not a star) are rendered italic
 * to visually distinguish them from planets and stars.
 *
 * Any previous texture in this slot is deleted before the new one is created.
 */
static void build_label_texture(int slot, int b)
{
    if (slot < 0 || slot >= MAX_BODIES) return;
    if (b < 0 || b >= g_nbodies || !s_font) return;

    if (s_tex[slot]) {
        glDeleteTextures(1, &s_tex[slot]);
        s_tex[slot] = 0;
    }

    SDL_Color col;
    col.r = (Uint8)(fminf(g_bodies[b].col[0]*1.4f+0.15f, 1.0f)*255);
    col.g = (Uint8)(fminf(g_bodies[b].col[1]*1.4f+0.15f, 1.0f)*255);
    col.b = (Uint8)(fminf(g_bodies[b].col[2]*1.4f+0.15f, 1.0f)*255);
    col.a = 255;
    int is_moon = (g_bodies[b].parent >= 0 &&
                   !g_bodies[g_bodies[b].parent].is_star);
    TTF_SetFontStyle(s_font, is_moon ? TTF_STYLE_ITALIC : TTF_STYLE_NORMAL);
    SDL_Surface *surf = TTF_RenderText_Blended(s_font, g_bodies[b].name, col);
    if (!surf) { TTF_SetFontStyle(s_font, TTF_STYLE_NORMAL); return; }
    s_tex[slot] = surface_to_texture(surf, &s_tex_w[slot], &s_tex_h[slot]);
    TTF_SetFontStyle(s_font, TTF_STYLE_NORMAL);
}

/* Insertion top-k by ascending distance²: place body b (distance² d2) into the
 * nearest-k lists best_d2[]/out[], updating the running count *n. */
static inline void pin_insert(double d2, int b, int k,
                              double *best_d2, int *out, int *n)
{
    int i;
    if (*n < k) i = (*n)++;
    else if (d2 >= best_d2[k-1]) return;
    else i = k - 1;
    while (i > 0 && best_d2[i-1] > d2) {
        best_d2[i] = best_d2[i-1];
        out[i]     = out[i-1];
        i--;
    }
    best_d2[i] = d2;
    out[i]     = b;
}

/*
 * nearest_pinned — top-k selection of the living bodies nearest the camera,
 * for BOTH pinned tiers in a single O(N) pass over g_bodies (folding what were
 * two separate full scans).  Systems = root stars (anchors, incl. black holes);
 * planets = non-star bodies whose parent is a star or which are parentless
 * rogues (moons excluded).  Writes the nearest k_sys systems followed by the
 * nearest k_pl planets into out[] (nearest first within each tier); returns the
 * total count.  Each k is bounded by LABEL_PIN_MAX.
 */
static int nearest_pinned(const double cam_m[3], int k_sys, int k_pl, int *out)
{
    if (k_sys > LABEL_PIN_MAX) k_sys = LABEL_PIN_MAX;
    if (k_pl  > LABEL_PIN_MAX) k_pl  = LABEL_PIN_MAX;
    if (k_sys < 0) k_sys = 0;
    if (k_pl  < 0) k_pl  = 0;

    double sd2[LABEL_PIN_MAX], pd2[LABEL_PIN_MAX];
    int    sys[LABEL_PIN_MAX], pl[LABEL_PIN_MAX];
    int    ns = 0, np = 0;

    for (int b = 0; b < g_nbodies; b++) {
        /* Skip the bulk field-star range (frozen catalog scenery).  Those are
         * never "pinned nearest" labels — they would just flood the pins with
         * catalogue ids; a field star you approach is still labelled via the
         * active feed (physics_active_bodies) below.  Jumping the whole range
         * keeps this an O(non-field) scan, not O(hundreds of thousands). */
        if (b >= g_field_star_begin && b < g_field_star_end) {
            b = g_field_star_end - 1;
            continue;
        }
        const Body *bd = &g_bodies[b];
        if (!bd->alive) continue;

        int is_sys, is_pl;
        if (bd->is_star) {
            is_sys = (bd->parent < 0);   /* root star = system anchor */
            is_pl  = 0;
        } else {
            /* planet: parent is a star, or a parentless rogue (moons excluded) */
            is_sys = 0;
            is_pl  = !(bd->parent >= 0 && !g_bodies[bd->parent].is_star);
        }
        if (!is_sys && !is_pl) continue;

        double dx = bd->pos[0] - cam_m[0];
        double dy = bd->pos[1] - cam_m[1];
        double dz = bd->pos[2] - cam_m[2];
        double d2 = dx*dx + dy*dy + dz*dz;

        if (is_sys && k_sys) pin_insert(d2, b, k_sys, sd2, sys, &ns);
        else if (is_pl && k_pl) pin_insert(d2, b, k_pl, pd2, pl, &np);
    }

    for (int i = 0; i < ns; i++) out[i]      = sys[i];
    for (int i = 0; i < np; i++) out[ns + i] = pl[i];
    return ns + np;
}

/* Cache slot currently holding body `b`, or -1 if none.  Linear scan over the
 * MAX_BODIES-entry cache — cheap since the cache is small and bounded. */
static int slot_of_body(int b)
{
    if (b < 0) return -1;
    for (int s = 0; s < MAX_BODIES; s++)
        if (s_slot_body[s] == b) return s;
    return -1;
}

/* Free a cache slot: drop its texture and reset its per-slot state. */
static void free_slot(int slot)
{
    if (slot < 0 || slot >= MAX_BODIES) return;
    if (s_tex[slot]) {
        glDeleteTextures(1, &s_tex[slot]);
        s_tex[slot] = 0;
    }
    s_tex_w[slot]     = 0;
    s_tex_h[slot]     = 0;
    s_slot_body[slot] = -1;
    s_show_accum[slot] = 0.0f;
    s_hide_accum[slot] = 0.0f;
    s_active[slot]     = 0;
}

/* ── public API ─────────────────────────────────────────────────────────── */

void labels_init(void) {
    s_shader = gl_shader_load("assets/shaders/label.vert",
                              "assets/shaders/label.frag");
    if (!s_shader) { fprintf(stderr,"[Labels] shader failed\n"); return; }

    s_loc_vp     = glGetUniformLocation(s_shader, "u_vp");
    s_loc_anchor = glGetUniformLocation(s_shader, "u_anchor");
    s_loc_right  = glGetUniformLocation(s_shader, "u_right");
    s_loc_up     = glGetUniformLocation(s_shader, "u_up");
    s_loc_tex    = glGetUniformLocation(s_shader, "u_tex");

    /* Unit quad: each vertex carries its UV position (0..1 range).
     * label.vert expands this to world-space using the cam right/up vectors
     * and world-space label dimensions computed from eye_z. */
    static const float quad_verts[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };
    static const unsigned int quad_idx[] = { 0,1,2, 0,2,3 };

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);
    s_ebo = gl_ebo_create(sizeof(quad_idx), quad_idx);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glBindVertexArray(0);

    if (TTF_Init() < 0) {
        fprintf(stderr, "[Labels] TTF_Init: %s\n", TTF_GetError());
        return;
    }
    s_font = ui_theme_open_font(16);
    if (!s_font) {
        fprintf(stderr, "[Labels] no usable font found — labels disabled\n");
        return;
    }

    /* The cache starts empty; slot textures are built lazily in labels_render
     * as bodies enter the camera's active region. */
    memset(s_tex,        0, sizeof(s_tex));
    memset(s_tex_w,      0, sizeof(s_tex_w));
    memset(s_tex_h,      0, sizeof(s_tex_h));
    memset(s_show_accum, 0, sizeof(s_show_accum));
    memset(s_hide_accum, 0, sizeof(s_hide_accum));
    memset(s_active,     0, sizeof(s_active));
    for (int s = 0; s < MAX_BODIES; s++) s_slot_body[s] = -1;
}

/* Register a newly added body.  Its texture is normally built lazily when the
 * body enters the camera's active region; but body indices are reused, so if a
 * cache slot still holds this index (from a previously-freed body) its texture
 * is stale — rebuild it now so the label reflects the new body. */
void labels_add_body(int body_idx)
{
    if (body_idx < 0 || body_idx >= g_nbodies) return;
    int slot = slot_of_body(body_idx);
    if (slot < 0) return;   /* not cached — lazy path will build it */
    s_show_accum[slot] = 0.0f;
    s_hide_accum[slot] = 0.0f;
    s_active[slot]     = 0;
    build_label_texture(slot, body_idx);
}

/* Deregister a body (e.g. after collision merge): release its cache slot so the
 * stale texture is freed and the reusable body index starts clean. */
void labels_remove_body(int body_idx)
{
    if (body_idx < 0) return;
    int slot = slot_of_body(body_idx);
    if (slot >= 0) free_slot(slot);
}

void labels_render(const float view[16], const float proj[16],
                   const float vp[16], const BodyRenderInfo *info,
                   float dt) {
    (void)proj;
    if (!s_shader || !s_font) return;

    Vec3 cam_right, cam_up, cam_fwd;
    mat4_get_right(view, cam_right);
    mat4_get_up   (view, cam_up);
    mat4_get_fwd  (view, cam_fwd);

    float half_fov_tan = tanf(FOV * 0.5f * (float)(PI / 180.0));

    /* ---- Step 0: refresh the camera-following slot cache ----
     * Ask physics which bodies are in systems near the camera, add the pinned
     * nearest planets/systems, then ensure each holds a cache slot (building
     * its name texture on first use).  Slots whose body has left the feed set
     * are evicted to make room.  Everything below is indexed by cache slot,
     * with s_slot_body[slot] giving the body. */
    int active[MAX_BODIES];
    double cam_m[3] = { (double)g_cam.pos[0] * AU,
                        (double)g_cam.pos[1] * AU,
                        (double)g_cam.pos[2] * AU };
    int n_active = physics_active_bodies(cam_m,
                                         g_settings.active_radius_ly * LY,
                                         active, MAX_BODIES);

    /* Pinned nearest set: always label the nearest M star systems and N
     * planets, regardless of the active region / far cutoff (see top note). */
    int pinned[2 * LABEL_PIN_MAX];
    int n_pinned = nearest_pinned(cam_m, g_settings.label_pin_systems,
                                  g_settings.label_pin_planets, pinned);

    /* Feed list for the slot cache: pinned bodies first (so they can never be
     * squeezed out by a full active set), then the active bodies not already
     * pinned, truncated at MAX_BODIES (active is ordered nearest-system-first,
     * so truncation drops the farthest).  feed_pinned[] marks pinned entries. */
    int feed[MAX_BODIES];
    int feed_pinned[MAX_BODIES];
    int n_feed = 0;
    for (int p = 0; p < n_pinned; p++) {
        feed_pinned[n_feed] = 1;
        feed[n_feed++] = pinned[p];
    }
    for (int a = 0; a < n_active && n_feed < MAX_BODIES; a++) {
        int b = active[a], dup = 0;
        for (int q = 0; q < n_pinned; q++)
            if (pinned[q] == b) { dup = 1; break; }
        if (dup) continue;
        feed_pinned[n_feed] = 0;
        feed[n_feed++] = b;
    }

    /* slot_needed[s] = slot s holds a body in this frame's feed set;
     * slot_pinned[s] = that body is pinned (bypasses the far cutoff below). */
    int slot_needed[MAX_BODIES];
    int slot_pinned[MAX_BODIES];
    memset(slot_needed, 0, sizeof(slot_needed));
    memset(slot_pinned, 0, sizeof(slot_pinned));

    /* Feed bodies without a slot yet; assigned to free/evictable slots below. */
    int pending[MAX_BODIES];
    int pending_pinned[MAX_BODIES];
    int n_pending = 0;
    for (int a = 0; a < n_feed; a++) {
        int b = feed[a];
        int slot = slot_of_body(b);
        if (slot >= 0) {
            slot_needed[slot] = 1;
            slot_pinned[slot] = feed_pinned[a];
        } else {
            pending_pinned[n_pending] = feed_pinned[a];
            pending[n_pending++]      = b;
        }
    }

    /* Assign each pending body a slot: prefer an empty slot, else evict one
     * whose occupant is not in this frame's feed set.  Room is guaranteed:
     * n_feed <= MAX_BODIES, so (needed + pending) <= MAX_BODIES. */
    {
        int scan = 0;
        for (int p = 0; p < n_pending; p++) {
            while (scan < MAX_BODIES &&
                   s_slot_body[scan] != -1 && slot_needed[scan]) scan++;
            if (scan >= MAX_BODIES) break;      /* defensive; shouldn't happen */
            int slot = scan++;
            if (s_slot_body[slot] != -1) free_slot(slot);   /* evict occupant */
            s_slot_body[slot] = pending[p];
            build_label_texture(slot, pending[p]);
            slot_needed[slot] = 1;
            slot_pinned[slot] = pending_pinned[p];
        }
    }

    /* ---- Step 1: project anchor to screen and build label AABB ---- */
    float lsx[MAX_BODIES], lsy[MAX_BODIES];
    float lsw[MAX_BODIES], lsh[MAX_BODIES];
    int   lvis[MAX_BODIES];
    int   order[MAX_BODIES];

    for (int s = 0; s < MAX_BODIES; s++) {
        lvis[s] = 0;
        if (!slot_needed[s]) continue;
        int b = s_slot_body[s];
        if (b < 0 || !g_bodies[b].alive) continue;
        if (!s_tex[s]) continue;
        /* Far cutoff: planet/moon labels are only useful in their local system
         * — except pinned nearest planets, which stay labelled at any range. */
        if (!g_bodies[b].is_star && !slot_pinned[s] &&
            info[b].dcam > g_settings.label_max_dist_au) continue;

        /* Close cutoff: hide label when camera is inside the body's glow region */
        {
            float min_radii = g_bodies[b].is_star
                            ? (float)STAR_MIN_LABEL_RADII
                            : (float)MIN_LABEL_RADII;
            if (info[b].dcam < min_radii * info[b].dr) continue;
        }

        /* Camera-relative anchor in double → float.  vp is proj×view_rot (no translation). */
        double cx = (double)g_cam.pos[0];
        double cy = (double)g_cam.pos[1];
        double cz = (double)g_cam.pos[2];
        float ax = (float)(g_bodies[b].pos[0] * RS - cx);
        float ay = (float)(g_bodies[b].pos[1] * RS - cy) + info[b].dr * 1.4f;
        float az = (float)(g_bodies[b].pos[2] * RS - cz);

        float sx, sy;
        if (!mat4_project(vp, ax, ay, az, WIN_W, WIN_H, &sx, &sy)) continue;

        /* Convert from GL (y=0 bottom) to SDL (y=0 top) for screen-space comparison */
        float screen_y = (float)WIN_H - sy;

        float ph = LABEL_PX_H;
        float pw = ph * (float)s_tex_w[s] / (float)s_tex_h[s];

        lsx[s] = sx;
        lsy[s] = screen_y - ph;
        lsw[s] = pw + LBL_PAD;
        lsh[s] = ph + LBL_PAD;
        lvis[s] = 1;
    }

    /* ---- Step 2: priority order (stars > planets > moons, nearest first) ----
     * Tiers hold cache-slot indices, sorted by the occupant body's dcam. */
    {
        int ns = 0, np = 0, nm = 0;
        int stars[MAX_BODIES], planets[MAX_BODIES], moons[MAX_BODIES];
        for (int s = 0; s < MAX_BODIES; s++) {
            if (!slot_needed[s]) continue;
            int b = s_slot_body[s];
            if (b < 0 || !g_bodies[b].alive) continue;
            if      (g_bodies[b].is_star) stars[ns++] = s;
            else if (g_bodies[b].parent < 0 ||
                     g_bodies[g_bodies[b].parent].is_star) planets[np++] = s;
            else                                           moons[nm++] = s;
        }
        /* Insertion sort each tier by the occupant body's dcam ascending */
        for (int i = 1; i < ns; i++) {
            int tmp = stars[i], k = i;
            while (k > 0 && info[s_slot_body[stars[k-1]]].dcam > info[s_slot_body[tmp]].dcam)
                { stars[k] = stars[k-1]; k--; }
            stars[k] = tmp;
        }
        for (int i = 1; i < np; i++) {
            int tmp = planets[i], k = i;
            while (k > 0 && info[s_slot_body[planets[k-1]]].dcam > info[s_slot_body[tmp]].dcam)
                { planets[k] = planets[k-1]; k--; }
            planets[k] = tmp;
        }
        for (int i = 1; i < nm; i++) {
            int tmp = moons[i], k = i;
            while (k > 0 && info[s_slot_body[moons[k-1]]].dcam > info[s_slot_body[tmp]].dcam)
                { moons[k] = moons[k-1]; k--; }
            moons[k] = tmp;
        }
        for (int i = 0; i < ns; i++) order[i]          = stars[i];
        for (int i = 0; i < np; i++) order[ns + i]      = planets[i];
        for (int i = 0; i < nm; i++) order[ns + np + i] = moons[i];
        for (int i = ns + np + nm; i < MAX_BODIES; i++) order[i] = -1;
    }

    /* ---- Step 3: greedy AABB overlap removal (order[] holds slot indices) ---- */
    for (int i = 0; i < MAX_BODIES; i++) {
        int idx = order[i];
        if (idx < 0) continue;
        if (!lvis[idx]) continue;
        /* Reject if rect overlaps any previously accepted label */
        for (int j = 0; j < i; j++) {
            int jdx = order[j];
            if (jdx < 0) continue;
            if (!lvis[jdx]) continue;
            if (lsx[idx]          < lsx[jdx]+lsw[jdx] &&
                lsx[idx]+lsw[idx] > lsx[jdx]           &&
                lsy[idx]          < lsy[jdx]+lsh[jdx] &&
                lsy[idx]+lsh[idx] > lsy[jdx]) {
                lvis[idx] = 0; break;
            }
        }
    }

    /* ---- Step 4: hysteresis debounce (per slot) ---- */
    for (int s = 0; s < MAX_BODIES; s++) {
        /* Slots not in the active set this frame are not drawn; reset their
         * debounce so re-entering the region starts cleanly (SHOW_DELAY again). */
        if (!slot_needed[s]) {
            s_show_accum[s] = 0.0f;
            s_hide_accum[s] = 0.0f;
            s_active[s]     = 0;
            continue;
        }
        if (lvis[s]) {
            s_show_accum[s] += dt;
            s_hide_accum[s]  = 0.0f;
            if (s_show_accum[s] >= SHOW_DELAY)
                s_active[s] = 1;
        } else {
            s_hide_accum[s] += dt;
            s_show_accum[s]  = 0.0f;
            if (s_hide_accum[s] >= HIDE_DELAY)
                s_active[s] = 0;
        }
    }

    /* ---- Step 5: draw surviving labels as camera-aligned billboards ---- */
    glUseProgram(s_shader);
    glUniformMatrix4fv(s_loc_vp, 1, GL_FALSE, vp);
    glUniform1i(s_loc_tex, 0);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(s_vao);

    for (int s = 0; s < MAX_BODIES; s++) {
        if (!s_active[s]) continue;
        if (!s_tex[s])    continue;
        int b = s_slot_body[s];
        if (b < 0 || !g_bodies[b].alive) continue;

        /* Camera-relative body anchor in double → float */
        double cx2 = (double)g_cam.pos[0];
        double cy2 = (double)g_cam.pos[1];
        double cz2 = (double)g_cam.pos[2];
        float bx = (float)(g_bodies[b].pos[0] * RS - cx2);
        float by = (float)(g_bodies[b].pos[1] * RS - cy2);
        float bz = (float)(g_bodies[b].pos[2] * RS - cz2);

        /* eye_z: forward-axis depth.  Stable under rotation (dcam is not). */
        float eye_z = bx*cam_fwd[0] + by*cam_fwd[1] + bz*cam_fwd[2];
        if (eye_z <= 0.0f) continue;

        /* World-space dimensions: constant pixel height regardless of distance */
        float fh = (eye_z * 2.0f * LABEL_PX_H * half_fov_tan) / (float)WIN_H;
        float fw = fh * (float)s_tex_w[s] / (float)s_tex_h[s];

        /* Anchor: body centre + up×dr_off, then nudge by right+up so the quad
         * sits clear of the body disc.  Vector math avoids per-component jumps
         * when the camera is tilted (a tilt changes all three components together). */
        float dr_off = info[b].dr * 1.4f;
        float ax = bx + cam_up[0]*dr_off + cam_right[0]*(fw*0.1f) + cam_up[0]*(fh*0.1f);
        float ay = by + cam_up[1]*dr_off + cam_right[1]*(fw*0.1f) + cam_up[1]*(fh*0.1f);
        float az = bz + cam_up[2]*dr_off + cam_right[2]*(fw*0.1f) + cam_up[2]*(fh*0.1f);

        Vec3 right_scaled, up_scaled;
        vec3_scale(right_scaled, cam_right, fw);
        vec3_scale(up_scaled,   cam_up,    fh);

        glUniform3f(s_loc_anchor, ax, ay, az);
        glUniform3f(s_loc_right,  right_scaled[0], right_scaled[1], right_scaled[2]);
        glUniform3f(s_loc_up,     up_scaled[0],    up_scaled[1],    up_scaled[2]);

        glBindTexture(GL_TEXTURE_2D, s_tex[s]);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void labels_shutdown(void) {
    glDeleteTextures(MAX_BODIES, s_tex);
    glDeleteBuffers(1, &s_vbo);
    glDeleteBuffers(1, &s_ebo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    if (s_font) TTF_CloseFont(s_font);
    TTF_Quit();
    s_shader = s_vao = s_vbo = s_ebo = 0;
    s_font = NULL;
}
