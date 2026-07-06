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
#include "universe.h"   /* g_field_star_begin/end */
#include "body.h"
#include "camera.h"
#include "gl_utils.h"
#include "math3d.h"
#include <stdlib.h>
#include <string.h>

static GLuint *s_vao = NULL;
static GLuint *s_vbo = NULL;
static int     s_n   = 0;

/* Dirty tracking — skip upload when no new samples */
static int *s_last_head  = NULL;
static int *s_last_count = NULL;

static GLuint s_shader          = 0;
static GLint  s_loc_vp          = -1;
static GLint  s_loc_color       = -1;
static GLint  s_loc_body_offset = -1;

/* Per-body reference position (world units) used as VBO origin.
 * Stored at the time of each VBO upload; valid until the next upload. */
static double (*s_ref_pos)[3] = NULL;

/* Scratch: linearised trail + live position, interleaved xyz + alpha. */
static float s_scratch[(TRAIL_LEN + 1) * 4];

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
    s_ref_pos    = (double(*)[3])calloc(s_n, sizeof(*s_ref_pos));
    if (!s_vao || !s_vbo || !s_last_head || !s_last_count || !s_ref_pos) return;

    for (int i = 0; i < s_n; i++) {
        s_last_head[i]  = 0;
        s_last_count[i] = 0;

        /* Stars never draw a trail (the render loop skips is_star / NULL-trail
         * bodies), so don't spend a ~0.25 MB dynamic VBO and a driver round-trip
         * on each.  At galaxy scale that is tens of thousands of buffers — many
         * GB of VRAM and seconds of init.  A zero handle is safe: it is never
         * bound, and glDelete* ignores it. */
        if (i < g_nbodies && (g_bodies[i].is_star || !g_bodies[i].trail)) {
            s_vao[i] = 0;
            s_vbo[i] = 0;
            continue;
        }

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
}

void trails_add_body(int body_idx)
{
    if (!s_shader || body_idx < 0 || body_idx >= MAX_BODIES) return;
    if (body_idx < s_n) return;

    int new_n = body_idx + 1;
    GLuint *new_vao = (GLuint*)malloc(new_n * sizeof(GLuint));
    GLuint *new_vbo = (GLuint*)malloc(new_n * sizeof(GLuint));
    int *new_head = (int*)malloc(new_n * sizeof(int));
    int *new_count = (int*)malloc(new_n * sizeof(int));
    double (*new_ref)[3] = (double(*)[3])calloc(new_n, sizeof(*new_ref));
    if (!new_vao || !new_vbo || !new_head || !new_count || !new_ref) {
        free(new_vao); free(new_vbo); free(new_head); free(new_count); free(new_ref);
        return;
    }
    if (s_n > 0) {
        memcpy(new_vao, s_vao, s_n * sizeof(GLuint));
        memcpy(new_vbo, s_vbo, s_n * sizeof(GLuint));
        memcpy(new_head, s_last_head, s_n * sizeof(int));
        memcpy(new_count, s_last_count, s_n * sizeof(int));
        memcpy(new_ref, s_ref_pos, s_n * sizeof(*s_ref_pos));
    }
    free(s_vao); free(s_vbo); free(s_last_head); free(s_last_count); free(s_ref_pos);
    s_vao = new_vao;
    s_vbo = new_vbo;
    s_last_head = new_head;
    s_last_count = new_count;
    s_ref_pos = new_ref;

    for (int i = s_n; i < new_n; i++) {
        s_last_head[i] = 0;
        s_last_count[i] = 0;
        /* Skip GL resources for stars / trail-less bodies (see trails_gl_init). */
        if (i < g_nbodies && (g_bodies[i].is_star || !g_bodies[i].trail)) {
            s_vao[i] = 0;
            s_vbo[i] = 0;
            continue;
        }
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

    s_n = new_n;
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

void trails_render(const float vp[16])
{
    if (!s_shader) return;

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

        const int head  = b->trail_head;
        const int count = b->trail_count;

        glBindVertexArray(s_vao[i]);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo[i]);

        /* Re-upload only when new trail samples were recorded.
         * VBO stores positions relative to s_ref_pos[i] (the body's world
         * position at upload time), keeping values small for float precision.
         * Camera movement is handled cheaply via u_body_offset each frame. */
        if (head != s_last_head[i] || count != s_last_count[i]) {
            /* Capture reference point: body's current world position. */
            s_ref_pos[i][0] = b->pos[0] * RS;
            s_ref_pos[i][1] = b->pos[1] * RS;
            s_ref_pos[i][2] = b->pos[2] * RS;

            /* Linearise circular buffer: oldest → newest, ref-relative.
             * Alpha follows cumulative world length, not vertex index. */
            double total_len = b->trail_total_len;
            double cumulative_len = 0.0;
            int oldest_idx = (head - count + TRAIL_LEN) & TRAIL_MASK;
            for (int k = 0; k < count; k++) {
                int idx = (head - count + k + TRAIL_LEN) & TRAIL_MASK;
                float alpha_t;
                if (idx != oldest_idx && b->trail_seg_len) {
                    cumulative_len += b->trail_seg_len[idx];
                    if (cumulative_len > total_len) cumulative_len = total_len;
                }
                if (total_len > 0.0) alpha_t = (float)(cumulative_len / total_len);
                else alpha_t = (k == count - 1) ? 1.0f : 0.0f;
                s_scratch[k*4+0] = (float)(b->trail[idx][0] - s_ref_pos[i][0]);
                s_scratch[k*4+1] = (float)(b->trail[idx][1] - s_ref_pos[i][1]);
                s_scratch[k*4+2] = (float)(b->trail[idx][2] - s_ref_pos[i][2]);
                /* alpha_t^1.5 (= alpha_t * sqrt(alpha_t)): power curve that
                 * keeps the tail nearly invisible and snaps bright only near
                 * the current position. Linear fade looks too uniform. */
                s_scratch[k*4+3] = alpha_t * sqrtf(alpha_t);
            }
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            count * 4 * sizeof(float), s_scratch);
            s_last_head[i]  = head;
            s_last_count[i] = count;
        }

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
                            count * 4 * sizeof(float), sizeof(live), live);
            draw_count = count + 1;
        } else {
            draw_count = count;
        }

        float alpha = 0.6f * (float)b->trail_fade * trail_fade;
        glUniform4f(s_loc_color, b->col[0], b->col[1], b->col[2], alpha);
        glDrawArrays(GL_LINE_STRIP, 0, draw_count);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

void trails_gl_shutdown(void)
{
    if (s_vbo) { glDeleteBuffers(s_n, s_vbo);       free(s_vbo);      s_vbo = NULL; }
    if (s_vao) { glDeleteVertexArrays(s_n, s_vao);  free(s_vao);      s_vao = NULL; }
    if (s_last_head)  { free(s_last_head);  s_last_head  = NULL; }
    if (s_last_count) { free(s_last_count); s_last_count = NULL; }
    if (s_ref_pos)    { free(s_ref_pos);    s_ref_pos    = NULL; }
    glDeleteProgram(s_shader);
    s_shader = 0;
    s_n = 0;
}
