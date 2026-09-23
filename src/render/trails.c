/*
 * trails.c — per-body orbital trail rendering
 *
 * Each body has a fixed-size circular buffer (TRAIL_LEN samples).
 * Samples are emitted by the physics layer using acceleration-driven timing,
 * then uploaded here for rendering.
 *
 * Render strategy:
 *   - VBO stores positions relative to a per-body reference point (the body's
 *     world position at the time of last upload). This keeps VBO values small,
 *     preserving float precision for the trail shape.
 *   - Camera offset is applied in the shader via u_body_offset = ref - cam_pos,
 *     computed CPU-side in double precision to avoid float cancellation jitter.
 *   - Dirty check: re-upload ONLY when new trail samples were recorded.
 *     Camera movement no longer triggers VBO re-uploads — only a cheap
 *     glUniform3fv call per body per frame is needed instead.
 *   - The scratch buffer linearises the circular buffer (oldest→newest)
 *     and appends the live planet position as the final vertex.
 */
#include "trails.h"
#include "profiler.h"
#include "universe.h"   /* g_field_star_begin/end */
#include "body.h"
#include "camera.h"
#include "gl_utils.h"
#include "math3d.h"
#include <stdlib.h>
#include <string.h>
#include "physics.h"   /* g_orbits_off, physics_active_bodies */
#include "settings.h"  /* g_settings.active_radius_ly */

static GLuint *s_vao = NULL;
static GLuint *s_vbo = NULL;
static int     s_n   = 0;

/* Dirty tracking — skip upload when no new samples */
static int *s_last_head  = NULL;
static int *s_last_count = NULL;
/* Vertices actually uploaded per body after decimation (<= trail_count). */
static int *s_emitted    = NULL;

static GLuint s_shader          = 0;
static GLint  s_loc_vp          = -1;
static GLint  s_loc_color       = -1;
static GLint  s_loc_body_offset = -1;

/* Per-body reference position (world units) used as VBO origin.
 * Stored at the time of each VBO upload; valid until the next upload. */
static double (*s_ref_pos)[3] = NULL;

/* Scratch: linearised trail + live position, interleaved xyz + alpha. */
static float s_scratch[(TRAIL_LEN + 1) * 4];

static void trail_gl_alloc(int i);

/* ---------------------------------------------------------------- public */

void trails_gl_init(void)
{
    s_shader = gl_shader_load("assets/shaders/solid.vert",
                              "assets/shaders/solid.frag");
    if (!s_shader) return;

    s_loc_vp          = glGetUniformLocation(s_shader, "u_vp");
    s_loc_color       = glGetUniformLocation(s_shader, "u_color");
    s_loc_body_offset = glGetUniformLocation(s_shader, "u_body_offset");
    s_n          = g_nbodies;
    s_vao        = (GLuint*)malloc(s_n * sizeof(GLuint));
    s_vbo        = (GLuint*)malloc(s_n * sizeof(GLuint));
    s_last_head  = (int*)   malloc(s_n * sizeof(int));
    s_last_count = (int*)   malloc(s_n * sizeof(int));
    s_emitted    = (int*)   malloc(s_n * sizeof(int));
    s_ref_pos    = (double(*)[3])calloc(s_n, sizeof(*s_ref_pos));
    if (!s_vao || !s_vbo || !s_last_head || !s_last_count || !s_emitted || !s_ref_pos) return;

    for (int i = 0; i < s_n; i++) {
        s_last_head[i]  = 0;
        s_last_count[i] = 0;
        s_emitted[i]    = 0;

        /* No body owns a trail buffer at init any more — trails_render()
         * allocates on entry to the active region (see trail_residency()).
         * A zero handle is safe: it is never bound, and glDelete* ignores it. */
        s_vao[i] = 0;
        s_vbo[i] = 0;
        if (i < g_nbodies && !g_bodies[i].is_star && g_bodies[i].trail)
            trail_gl_alloc(i);
    }
}

/* Create this body's VAO + dynamic VBO.  Idempotent: a live handle is kept. */
static void trail_gl_alloc(int i)
{
    if (s_vbo[i]) return;
    s_vao[i] = gl_vao_create();
    s_vbo[i] = gl_vbo_create((TRAIL_LEN + 1) * 4 * sizeof(float),
                             NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

/* Grow the per-body side tables to cover at least `need` indices.
 *
 * Capacity DOUBLES rather than growing to exactly `need`: promotion adds a star
 * and its planets one at a time, and growing by one each call made this an
 * O(n) copy per body.  Returns 0 if it could not grow, leaving s_n — and so
 * every existing entry — untouched and valid.
 *
 * A partial realloc is safe: each successful realloc is stored back, so the
 * array is merely larger than s_n claims and the next call retries the rest. */
static int trails_grow(int need)
{
    if (need <= s_n) return 1;
    int cap = s_n ? s_n : MAX_BODIES;
    while (cap < need) cap *= 2;

    GLuint *vao = (GLuint*)realloc(s_vao, (size_t)cap * sizeof(GLuint));
    if (vao) s_vao = vao;
    GLuint *vbo = (GLuint*)realloc(s_vbo, (size_t)cap * sizeof(GLuint));
    if (vbo) s_vbo = vbo;
    int *hd = (int*)realloc(s_last_head, (size_t)cap * sizeof(int));
    if (hd) s_last_head = hd;
    int *ct = (int*)realloc(s_last_count, (size_t)cap * sizeof(int));
    if (ct) s_last_count = ct;
    int *em = (int*)realloc(s_emitted, (size_t)cap * sizeof(int));
    if (em) s_emitted = em;
    double (*rp)[3] = (double(*)[3])realloc(s_ref_pos,
                                            (size_t)cap * sizeof(*s_ref_pos));
    if (rp) s_ref_pos = rp;
    if (!vao || !vbo || !hd || !ct || !em || !rp) return 0;

    for (int i = s_n; i < cap; i++) {
        s_vao[i] = 0;  s_vbo[i] = 0;
        s_last_head[i] = 0;  s_last_count[i] = 0;  s_emitted[i] = 0;
        s_ref_pos[i][0] = s_ref_pos[i][1] = s_ref_pos[i][2] = 0.0;
    }
    s_n = cap;
    return 1;
}

/* Make room for a body added at runtime (starsys promotion, a user-placed
 * body, a supernova remnant).
 *
 * This used to bail on `body_idx >= MAX_BODIES` (128), which silently denied a
 * trail to every one of those bodies: with a galaxy-scale catalog loaded they
 * all land tens of thousands of indices above that cap, so a promoted planet
 * got trail state but never a VBO and drew nothing.  MAX_BODIES is a legacy
 * bound the rest of the engine has already moved off (render.c:2210,
 * collision.c:144, physics.c:118); this was the last place it still capped a
 * body index rather than a fixed cache.
 *
 * No GL object is created here — trail_acquire() makes one on demand when the
 * body enters the active region. */
void trails_add_body(int body_idx)
{
    if (!s_shader || body_idx < 0) return;
    trails_grow(body_idx + 1);
}

/* ── trail residency ──────────────────────────────────────────────────────
 *
 * A trail buffer is allocated when its body enters the simulated region and
 * released once it leaves.  Only bodies inside ACTIVE_RADIUS_LY ever record a
 * sample — main.c drives trails_tick_system() per active system root — so a
 * resident trail anywhere further out is 512 KiB of heap plus a 256 KiB
 * GL_DYNAMIC_DRAW VBO holding a path that cannot change.  Allocating those at
 * load cost ~3.2 GiB of heap and ~1.6 GiB of requested VRAM across the shipped
 * catalog's ~6.3k non-star bodies.
 *
 * Release is hysteretic: a body keeps its trail for RESIDENCY_GRACE frames
 * after it stops being active, so one hovering at the activation boundary does
 * not thrash 768 KiB every frame.  Re-entry after that is a fresh trail, which
 * is also the honest result — nothing was recorded while it was frozen.
 */
#define RESIDENCY_GRACE 120

typedef struct { int body; unsigned stamp; } TrailResident;
static TrailResident *s_res = NULL;
static int      s_res_n = 0, s_res_cap = 0;
static unsigned s_res_frame = 0;

static void trail_free_body(int i)
{
    if (i < 0 || i >= g_nbodies) return;
    Body *b = &g_bodies[i];
    free(b->trail);          b->trail = NULL;
    free(b->trail_seg_len);  b->trail_seg_len = NULL;
    b->trail_head = 0;
    b->trail_count = 0;
    b->trail_accum = 0.0;
    b->trail_total_len = 0.0;
    b->trail_frame_head = 0;
    b->trail_frame_count = 0;
    b->trail_frame_accum = 0.0;
    b->trail_frame_total_len = 0.0;
    if (i < s_n) {
        if (s_vbo[i]) { glDeleteBuffers(1, &s_vbo[i]);      s_vbo[i] = 0; }
        if (s_vao[i]) { glDeleteVertexArrays(1, &s_vao[i]); s_vao[i] = 0; }
        s_last_head[i]  = 0;
        s_last_count[i] = 0;
        s_emitted[i]    = 0;
    }
}

/* Give body `i` a trail if it lacks one, and refresh its residency stamp. */
static void trail_acquire(int i)
{
    if (i < 0 || i >= g_nbodies) return;
    Body *b = &g_bodies[i];
    if (b->is_star || !b->alive) return;
    if (i >= s_n && !trails_grow(i + 1)) return;

    if (!b->trail) {
        b->trail = (double(*)[3])calloc(TRAIL_LEN, 3 * sizeof(double));
        b->trail_seg_len = (double*)calloc(TRAIL_LEN, sizeof(double));
        if (!b->trail || !b->trail_seg_len) {   /* out of memory: stay trail-less */
            free(b->trail);          b->trail = NULL;
            free(b->trail_seg_len);  b->trail_seg_len = NULL;
            return;
        }
        /* Seed the Hermite tangent from where the body is NOW, not from its
         * load-time state — arbitrarily much sim time may have passed. */
        b->trail_head = 0;  b->trail_count = 0;
        b->trail_accum = 0.0;  b->trail_total_len = 0.0;
        b->trail_fade = 1.0;
        for (int k = 0; k < 3; k++) {
            b->trail_prev_pos[k] = b->pos[k];
            b->trail_prev_vel[k] = b->vel[k];
            b->trail_frame_pos[k] = b->pos[k];
            b->trail_frame_vel[k] = b->vel[k];
            b->trail_frame_prev_pos[k] = b->pos[k];
            b->trail_frame_prev_vel[k] = b->vel[k];
        }
        b->trail_frame_head = 0;  b->trail_frame_count = 0;
        b->trail_frame_accum = 0.0;  b->trail_frame_total_len = 0.0;
        s_last_head[i] = 0;  s_last_count[i] = 0;  s_emitted[i] = 0;
        trail_gl_alloc(i);
    }

    for (int r = 0; r < s_res_n; r++)
        if (s_res[r].body == i) { s_res[r].stamp = s_res_frame; return; }

    if (s_res_n == s_res_cap) {
        int cap = s_res_cap ? s_res_cap * 2 : 64;
        TrailResident *t = (TrailResident*)realloc(s_res, (size_t)cap * sizeof(*t));
        if (!t) return;
        s_res = t;  s_res_cap = cap;
    }
    s_res[s_res_n].body = i;
    s_res[s_res_n].stamp = s_res_frame;
    s_res_n++;
}

/* Once per frame: acquire for everything in the simulated region, and drop
 * residents that have been out of it for longer than the grace period. */
static void trail_residency(void)
{
    s_res_frame++;

    static int *near = NULL;  static int near_cap = 0;
    const int NEAR_MAX = 8192;
    if (near_cap < NEAR_MAX) {
        int *t = (int*)realloc(near, (size_t)NEAR_MAX * sizeof(int));
        if (!t) return;
        near = t;  near_cap = NEAR_MAX;
    }
    double cam_m[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
    double r_m = g_settings.active_radius_ly * LY;
    int nn = physics_active_bodies(cam_m, r_m, near, near_cap);
    for (int j = 0; j < nn; j++) trail_acquire(near[j]);

    for (int r = 0; r < s_res_n; ) {
        int i = s_res[r].body;
        int stale = (s_res_frame - s_res[r].stamp) > RESIDENCY_GRACE;
        int gone  = (i >= g_nbodies) || !g_bodies[i].alive || g_bodies[i].is_star;
        if (stale || gone) {
            trail_free_body(i);
            s_res[r] = s_res[--s_res_n];
        } else {
            r++;
        }
    }
}

void trails_remove_body(int body_idx)
{
    if (body_idx < 0 || body_idx >= s_n) return;
    s_last_head[body_idx] = 0;
    s_last_count[body_idx] = 0;
    if (body_idx < g_nbodies) {
        g_bodies[body_idx].trail_head = 0;
        g_bodies[body_idx].trail_count = 0;
        g_bodies[body_idx].trail_accum = 0.0;
        g_bodies[body_idx].trail_total_len = 0.0;
        g_bodies[body_idx].trail_emitting = 0;
    }
}

void trails_reset_body(int body_idx)
{
    Body *b;
    double x, y, z;

    if (body_idx < 0 || body_idx >= g_nbodies) return;
    b = &g_bodies[body_idx];
    if (!b->trail) return;

    x = b->pos[0] * RS;
    y = b->pos[1] * RS;
    z = b->pos[2] * RS;
    for (int i = 0; i < TRAIL_LEN; i++) {
        b->trail[i][0] = x;
        b->trail[i][1] = y;
        b->trail[i][2] = z;
    }
    b->trail_head = 2 % TRAIL_LEN;
    b->trail_count = 2;
    b->trail_accum = 0.0;
    if (b->trail_seg_len) {
        for (int i = 0; i < TRAIL_LEN; i++) b->trail_seg_len[i] = 0.0;
    }
    b->trail_total_len = 0.0;
    b->trail_emitting = 1;
    b->trail_prev_pos[0] = b->pos[0];
    b->trail_prev_pos[1] = b->pos[1];
    b->trail_prev_pos[2] = b->pos[2];
    b->trail_prev_vel[0] = b->vel[0];
    b->trail_prev_vel[1] = b->vel[1];
    b->trail_prev_vel[2] = b->vel[2];
    b->trail_frame_accum = 0.0;
    b->trail_frame_head = b->trail_head;
    b->trail_frame_count = b->trail_count;
    b->trail_frame_total_len = b->trail_total_len;
    b->trail_frame_pos[0] = b->pos[0];
    b->trail_frame_pos[1] = b->pos[1];
    b->trail_frame_pos[2] = b->pos[2];
    b->trail_frame_vel[0] = b->vel[0];
    b->trail_frame_vel[1] = b->vel[1];
    b->trail_frame_vel[2] = b->vel[2];
    b->trail_frame_prev_pos[0] = b->trail_prev_pos[0];
    b->trail_frame_prev_pos[1] = b->trail_prev_pos[1];
    b->trail_frame_prev_pos[2] = b->trail_prev_pos[2];
    b->trail_frame_prev_vel[0] = b->trail_prev_vel[0];
    b->trail_frame_prev_vel[1] = b->trail_prev_vel[1];
    b->trail_frame_prev_vel[2] = b->trail_prev_vel[2];

    if (body_idx < s_n) {
        s_last_head[body_idx] = -1;
        s_last_count[body_idx] = -1;
    }
}

/* Below this on-screen length a trail cannot render anything meaningful; it is
 * a sub-pixel smear that still costs a full draw call. */
#define TRAIL_MIN_PIXELS 2.0

static int s_drawn = 0, s_culled = 0, s_reup = 0;
static long s_reup_verts = 0;

void trails_render(const float vp[16])
{
    if (!s_shader || g_orbits_off) return;
    s_drawn = s_culled = s_reup = 0; s_reup_verts = 0;

    /* Before the fade early-out below: residency must be maintained even when
     * trails are faded to nothing, or a body would record no path while out of
     * sight and show an empty trail the moment it faded back in. */
    trail_residency();

    /* Distance from camera to nearest star — controls LOD fade.
     * Computed in render units (AU).  Returns early if trails are fully faded. */
    float trail_fade = 1.0f;
    if (g_nbodies > 0 && g_cam_prox.star >= 0) {
        /* Nearest-star distance from the shared per-frame proximity cache
         * (g_cam_prox) — no private O(N) scan. */
        float dist = (float)g_cam_prox.star_dist_au;
        /* Smooth (Hermite) fade, same endpoints as the old linear ramp —
         * eases in/out so the fade rate has no visible kink (continuous LOD). */
        float t = (dist - SYS_TRAIL_FADE_START)
                / (SYS_TRAIL_FADE_END - SYS_TRAIL_FADE_START);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        trail_fade = 1.0f - t * t * (3.0f - 2.0f * t);
        if (trail_fade <= 0.0f) return;   /* fully faded — skip all work */
    }

    glUseProgram(s_shader);
    glUniformMatrix4fv(s_loc_vp, 1, GL_FALSE, vp);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    for (int i = 0; i < g_nbodies && i < s_n; i++) {
        /* Field stars are stars (no trail) — skip the whole range in O(1). */
        if (i >= g_field_star_begin && i < g_field_star_end) {
            i = g_field_star_end - 1;
            continue;
        }
        Body *b = &g_bodies[i];
        if (b->is_star || b->trail_count < 2 || !b->trail) continue;
        if (!b->alive && b->trail_fade <= 0.0) continue;

        /* Screen-space cull. Every surviving body below costs a VAO bind, a VBO
         * bind, at least one glBufferSubData and a draw call — thousands of tiny
         * state changes per frame at galaxy scale. A trail whose entire arc
         * subtends less than a pixel or two cannot show anything, so reject it
         * before touching GL at all.
         *
         * trail_total_len is world metres; positions here are AU. The global
         * trail_fade above is a fade for the system you are in — it does not
         * cull the thousands of other systems' trails behind it. */
        double px = 1e30;   /* on-screen length of the whole trail, pixels */
        {
            double dx = b->pos[0] * RS - g_cam.pos[0];
            double dy = b->pos[1] * RS - g_cam.pos[1];
            double dz = b->pos[2] * RS - g_cam.pos[2];
            double dist_au = sqrt(dx*dx + dy*dy + dz*dz);
            if (dist_au > 1e-9) {
                double extent_au = b->trail_total_len * RS;
                /* pixels ~= (extent / dist) / fov_rad * screen_height */
                px = (extent_au / dist_au) / (FOV * PI / 180.0) * (double)WIN_H;
                if (px < TRAIL_MIN_PIXELS) { s_culled++; continue; }
            }
        }
        s_drawn++;

        const int head  = b->trail_head;
        const int count = b->trail_count;

        /* Screen-space decimation. A trail holds up to TRAIL_LEN (16384)
         * samples, and appending one sample renormalises every vertex's alpha
         * against a shifted total length — so the whole buffer is re-linearised
         * and re-uploaded. Measured: ~350k vertices, ~5.6 MB per frame, 93% of
         * the trail pass.
         *
         * A polyline covering `px` pixels cannot resolve more than about two
         * vertices per pixel, so emit every stride-th sample. This is the same
         * continuous-LOD bargain the renderer already makes for dots, spheres
         * and glares — detail proportional to apparent size. Cumulative length
         * is still accumulated across skipped samples, so the alpha ramp is
         * unchanged, and the newest sample is always emitted so the tip meets
         * the live position. */
        int stride = 1;
        {
            double budget = px * 2.0;
            if (budget < 32.0) budget = 32.0;         /* floor: keep the shape */
            if ((double)count > budget) stride = (int)((double)count / budget);
            if (stride < 1) stride = 1;
        }


        glBindVertexArray(s_vao[i]);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo[i]);

        /* Re-upload only when new trail samples were recorded.
         * VBO stores positions relative to s_ref_pos[i] (the body's world
         * position at upload time), keeping values small for float precision.
         * Camera movement is handled cheaply via u_body_offset each frame. */
        double zt_up = profiler_enabled() ? profiler_now_ms() : 0.0;
        if (head != s_last_head[i] || count != s_last_count[i]) {
            s_reup++;
            /* Capture reference point: body's current world position. */
            s_ref_pos[i][0] = b->pos[0] * RS;
            s_ref_pos[i][1] = b->pos[1] * RS;
            s_ref_pos[i][2] = b->pos[2] * RS;

            /* Linearise circular buffer: oldest → newest, ref-relative.
             * Alpha follows cumulative world length, not vertex index. */
            double total_len = b->trail_total_len;
            double cumulative_len = 0.0;
            int oldest_idx = (head - count + TRAIL_LEN) & TRAIL_MASK;
            int e = 0;                                 /* emitted vertex count */
            for (int k = 0; k < count; k++) {
                int idx = (head - count + k + TRAIL_LEN) & TRAIL_MASK;
                float alpha_t;
                if (idx != oldest_idx && b->trail_seg_len) {
                    cumulative_len += b->trail_seg_len[idx];
                    if (cumulative_len > total_len) cumulative_len = total_len;
                }
                /* Always keep the first and last sample; thin the middle. */
                if (stride > 1 && k != 0 && k != count - 1 && (k % stride) != 0)
                    continue;
                if (total_len > 0.0) alpha_t = (float)(cumulative_len / total_len);
                else alpha_t = (k == count - 1) ? 1.0f : 0.0f;
                s_scratch[e*4+0] = (float)(b->trail[idx][0] - s_ref_pos[i][0]);
                s_scratch[e*4+1] = (float)(b->trail[idx][1] - s_ref_pos[i][1]);
                s_scratch[e*4+2] = (float)(b->trail[idx][2] - s_ref_pos[i][2]);
                /* alpha_t^1.5 (= alpha_t * sqrt(alpha_t)): power curve that
                 * keeps the tail nearly invisible and snaps bright only near
                 * the current position. Linear fade looks too uniform. */
                s_scratch[e*4+3] = alpha_t * sqrtf(alpha_t);
                e++;
            }
            s_emitted[i] = e;
            s_reup_verts += e;
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            e * 4 * sizeof(float), s_scratch);
            s_last_head[i]  = head;
            s_last_count[i] = count;
        }
        if (profiler_enabled())
            profiler_zone_add("  trail relinearise", profiler_now_ms() - zt_up);

        /* Pass per-body offset = ref_pos - cam_pos, computed in double precision
         * to avoid float cancellation jitter when the camera is near the body. */
        float off[3] = {
            (float)(s_ref_pos[i][0] - g_cam.pos[0]),
            (float)(s_ref_pos[i][1] - g_cam.pos[1]),
            (float)(s_ref_pos[i][2] - g_cam.pos[2])
        };
        glUniform3fv(s_loc_body_offset, 1, off);

        int draw_count;
        if (b->alive && b->trail_emitting) {
            /* Append the live planet position as the final vertex so the
             * trail tip follows the planet every frame without a new sample.
             * Stored ref-relative so it matches the VBO coordinate space. */
            float live[4] = {
                (float)(b->pos[0] * RS - s_ref_pos[i][0]),
                (float)(b->pos[1] * RS - s_ref_pos[i][1]),
                (float)(b->pos[2] * RS - s_ref_pos[i][2]),
                1.0f
            };
            glBufferSubData(GL_ARRAY_BUFFER,
                            s_emitted[i] * 4 * sizeof(float), sizeof(live), live);
            draw_count = s_emitted[i] + 1;
        } else {
            draw_count = s_emitted[i];
        }

        float alpha = 0.6f * (float)b->trail_fade * trail_fade;
        glUniform4f(s_loc_color, b->col[0], b->col[1], b->col[2], alpha);
        glDrawArrays(GL_LINE_STRIP, 0, draw_count);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);

    if (profiler_enabled()) {
        profiler_zone_add("  trails drawn", (double)s_drawn);
        profiler_zone_add("  trails culled", (double)s_culled);
        profiler_zone_add("  trails re-uploaded", (double)s_reup);
        profiler_zone_add("  reup verts (k)", (double)s_reup_verts / 1000.0);
    }
}

void trails_gl_shutdown(void)
{
    if (s_vbo) { glDeleteBuffers(s_n, s_vbo);       free(s_vbo);      s_vbo = NULL; }
    if (s_vao) { glDeleteVertexArrays(s_n, s_vao);  free(s_vao);      s_vao = NULL; }
    if (s_last_head)  { free(s_last_head);  s_last_head  = NULL; }
    if (s_last_count) { free(s_last_count); s_last_count = NULL; }
    if (s_ref_pos)    { free(s_ref_pos);    s_ref_pos    = NULL; }
    if (s_emitted)    { free(s_emitted);    s_emitted    = NULL; }
    if (s_res)        { free(s_res);        s_res = NULL; }
    s_res_n = s_res_cap = 0;
    glDeleteProgram(s_shader);
    s_shader = 0;
    s_n = 0;
}
