/*
 * starfield.c — procedural background star skybox
 *
 * Stars are placed on a unit sphere and rendered with the translation
 * component stripped from the view matrix, making them appear infinitely
 * distant regardless of camera position.
 *
 * Uniform sphere sampling:
 *   Naive uniform (theta, phi) sampling produces a polar clustering artefact
 *   (too many stars near the poles).  Instead we use the area-preserving
 *   transform:  theta = acos(1 - 2*u)  where u is uniform in [0,1].
 *   This gives a uniform distribution of points on the sphere surface.
 *
 * Brightness split:
 *   The VBO stores NUM_STARS points, sorted so that the first 3/4 are "dim"
 *   (1px) and the last 1/4 are "bright" (2px).  The split is done at
 *   generation time by building the bright subset into the tail of the
 *   same buffer — no index buffer needed, just two glDrawArrays calls.
 *
 * Colour distribution:
 *   Rough stellar spectral type frequencies (O/B/A/F/G/K/M) drive the colour
 *   table.  G-type (Sun-like yellow) is the plurality at ~25%, M-type red
 *   dwarfs the majority at ~22%.  The distribution is intentionally biased
 *   toward the visually interesting O/B/A end to avoid an all-red skybox.
 */
#include "starfield.h"
#include "gl_utils.h"
#include "math3d.h"
#include <stdlib.h>
#include <math.h>

/* ---------------------------------------------------------------- private */

static GLuint s_shader   = 0;
static GLuint s_vao      = 0;
static GLuint s_vbo      = 0;
static GLint  s_loc_vp   = -1;
static int    s_count    = 0;

static float randf(void) { return (float)rand() / (float)RAND_MAX; }

/* Assign a stellar colour by randomly sampling the spectral type distribution.
 * Thresholds are cumulative frequencies: O/B (3%), A (13%), F (30%),
 * G (55%), K (78%), M (100%).  All colours are normalised to [0,1] linear. */
static void star_color(float *r, float *g, float *b) {
    float t = randf();
    if      (t < 0.03f) { *r=0.70f; *g=0.77f; *b=1.00f; } /* O/B blue-white */
    else if (t < 0.13f) { *r=0.90f; *g=0.92f; *b=1.00f; } /* A white        */
    else if (t < 0.30f) { *r=1.00f; *g=0.98f; *b=0.85f; } /* F yellow-white */
    else if (t < 0.55f) { *r=1.00f; *g=0.95f; *b=0.70f; } /* G yellow (Sun) */
    else if (t < 0.78f) { *r=1.00f; *g=0.80f; *b=0.50f; } /* K orange       */
    else                { *r=1.00f; *g=0.55f; *b=0.35f; } /* M red          */
}

/* ---------------------------------------------------------------- public */

/* Build the star VBO and compile the shader.
 * srand(42) is used for a deterministic skybox — same stars every run. */
void starfield_init(void) {
    s_shader = gl_shader_load("assets/shaders/color.vert",
                              "assets/shaders/color.frag");
    if (!s_shader) return;

    s_loc_vp = glGetUniformLocation(s_shader, "u_vp");

    /* Vertex layout: xyz position (unit sphere) + rgb colour — 6 floats/star */
    srand(42);
    s_count = NUM_STARS;
    float *verts = (float *)malloc(s_count * 6 * sizeof(float));

    for (int i = 0; i < s_count; i++) {
        /* Area-preserving uniform sphere distribution.
         * theta = acos(1 - 2u) ensures equal point density per unit area. */
        float theta = acosf(1.0f - 2.0f * randf());
        float phi   = 2.0f * (float)PI * randf();
        float sx    = sinf(theta) * cosf(phi);
        float sy    = cosf(theta);
        float sz    = sinf(theta) * sinf(phi);

        float r, g, b;
        star_color(&r, &g, &b);
        /* Scale brightness individually — creates realistic magnitude variation */
        float bright = 0.4f + 0.6f * randf();
        r *= bright; g *= bright; b *= bright;

        verts[i*6+0]=sx; verts[i*6+1]=sy; verts[i*6+2]=sz;
        verts[i*6+3]=r;  verts[i*6+4]=g;  verts[i*6+5]=b;
    }

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(s_count * 6 * sizeof(float), verts, GL_STATIC_DRAW);
    free(verts);

    /* loc 0: vec3 position on unit sphere */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    /* loc 1: vec3 colour */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float),
                          (void*)(3*sizeof(float)));

    glBindVertexArray(0);
}

/* Render the skybox using a rotation-only VP (no translation).
 * view_rot is the standard view matrix with its translation column zeroed,
 * so the stars move with the camera's orientation but not its position.
 * Two draw calls render dim (1px) and bright (2px) stars separately. */
void starfield_render(const float view_rot[16], const float proj[16]) {
    if (!s_shader) return;

    Mat4 vp;
    mat4_mul(vp, proj, view_rot);

    glUseProgram(s_shader);
    glUniformMatrix4fv(s_loc_vp, 1, GL_FALSE, vp);

    glBindVertexArray(s_vao);

    glPointSize(1.0f);
    glDrawArrays(GL_POINTS, 0, s_count * 3 / 4);        /* dim stars (75%) */

    glPointSize(2.0f);
    glDrawArrays(GL_POINTS, s_count * 3 / 4, s_count / 4); /* bright stars (25%) */

    glBindVertexArray(0);
    glPointSize(1.0f);
}

/* Release all GPU resources. */
void starfield_shutdown(void) {
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    s_vao = s_vbo = s_shader = 0;
}
