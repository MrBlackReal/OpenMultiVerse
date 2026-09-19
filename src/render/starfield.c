/*
 * starfield.c - catalog-backed background star skybox
 *
 * The runtime path loads a compact Yale Bright Star Catalog subset from
 * assets/bright_star_catalog.csv. Coordinates are J2000.0 equatorial
 * RA/Dec and are rotated into the simulation's ecliptic GL frame:
 *
 *   GL X = ecliptic X (vernal equinox)
 *   GL Y = ecliptic Z (north ecliptic pole)
 *   GL Z = ecliptic Y
 *
 * If the catalog asset is missing, a deterministic procedural fallback is
 * generated so the renderer still starts in development builds.
 */
#include "starfield.h"
#include "gl_utils.h"
#include "math3d.h"
#include "settings.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define STAR_CATALOG_PATH "assets/bright_star_catalog.csv"
#define STAR_MAG_BRIGHT   1.5f
#define STAR_MAG_MID      4.5f

typedef struct {
    float pos[3];
    float col[3];
    float mag;
} StarVertex;

/* ---------------------------------------------------------------- private */

static GLuint s_shader       = 0;
static GLuint s_vao          = 0;
static GLuint s_vbo          = 0;
static GLint  s_loc_vp       = -1;
static GLint  s_loc_fade     = -1;
static int    s_count        = 0;
static int    s_faint_count  = 0;
static int    s_mid_count    = 0;
static int    s_bright_count = 0;
static int    s_bg_count     = 0;   /* faint background star-dust layer */

static float randf(void) { return (float)rand() / (float)RAND_MAX; }

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static int star_cmp_faint_to_bright(const void *a, const void *b)
{
    const StarVertex *sa = (const StarVertex *)a;
    const StarVertex *sb = (const StarVertex *)b;
    if (sa->mag < sb->mag) return 1;
    if (sa->mag > sb->mag) return -1;
    return 0;
}

static void temperature_to_rgb(float kelvin, float *r, float *g, float *b)
{
    float t = clampf(kelvin, 1000.0f, 40000.0f) / 100.0f;

    if (t <= 66.0f) {
        *r = 1.0f;
        *g = clampf(0.39008158f * logf(t) - 0.63184144f, 0.0f, 1.0f);
        if (t <= 19.0f)
            *b = 0.0f;
        else
            *b = clampf(0.54320679f * logf(t - 10.0f) - 1.19625409f, 0.0f, 1.0f);
    } else {
        *r = clampf(1.29293619f * powf(t - 60.0f, -0.13320476f), 0.0f, 1.0f);
        *g = clampf(1.12989086f * powf(t - 60.0f, -0.07551485f), 0.0f, 1.0f);
        *b = 1.0f;
    }
}

static float display_brightness_from_mag(float mag)
{
    float t = (6.70f - mag) / (6.70f - (-1.50f));
    t = clampf(t, 0.0f, 1.0f);
    float b = 0.18f + 0.82f * powf(t, 0.65f);
    /* HDR overbright for the naked-eye bright end (m < 2.5): brightness keeps
     * rising past 1.0 on the real magnitude scale (compressed exponent), so
     * the bloom pass gives Sirius/Canopus/Vega a soft blaze in their own
     * colour instead of capping them at an LDR dot.  The scene target is
     * RGBA16F and the composite is ACES, so the overshoot tonemaps cleanly. */
    if (mag < 2.5f) {
        float g = powf(10.0f, 0.28f * (2.5f - mag));
        b *= (g > 6.0f) ? 6.0f : g;
    }
    return b;
}

static void equatorial_to_gl(double ra_deg, double dec_deg, float out[3])
{
    const double deg = PI / 180.0;
    const double eps = 23.4392911 * deg; /* J2000 mean obliquity */

    double ra  = ra_deg  * deg;
    double dec = dec_deg * deg;
    double ce  = cos(eps);
    double se  = sin(eps);

    double x_eq = cos(dec) * cos(ra);
    double y_eq = cos(dec) * sin(ra);
    double z_eq = sin(dec);

    double x_ecl = x_eq;
    double y_ecl = y_eq * ce + z_eq * se;
    double z_ecl = -y_eq * se + z_eq * ce;

    out[0] = (float)x_ecl;
    out[1] = (float)z_ecl;
    out[2] = (float)y_ecl;
}

static int load_catalog(StarVertex **out)
{
    FILE *f = fopen(STAR_CATALOG_PATH, "rb");
    if (!f) return 0;

    int cap = 8192;
    int n = 0;
    StarVertex *stars = (StarVertex *)malloc((size_t)cap * sizeof(StarVertex));
    if (!stars) {
        fclose(f);
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        double ra_deg, dec_deg, mag, temp_k;
        float bright;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (strncmp(line, "ra_deg", 6) == 0)
            continue;
        if (sscanf(line, "%lf,%lf,%lf,%lf", &ra_deg, &dec_deg, &mag, &temp_k) != 4)
            continue;

        if (n >= cap) {
            int new_cap = cap * 2;
            StarVertex *grown = (StarVertex *)realloc(stars, (size_t)new_cap * sizeof(StarVertex));
            if (!grown) break;
            stars = grown;
            cap = new_cap;
        }

        equatorial_to_gl(ra_deg, dec_deg, stars[n].pos);
        temperature_to_rgb((float)temp_k, &stars[n].col[0], &stars[n].col[1], &stars[n].col[2]);
        bright = display_brightness_from_mag((float)mag);
        stars[n].col[0] *= bright;
        stars[n].col[1] *= bright;
        stars[n].col[2] *= bright;
        stars[n].mag = (float)mag;
        n++;
    }

    fclose(f);

    if (n <= 0) {
        free(stars);
        return 0;
    }

    *out = stars;
    return n;
}

static void procedural_color_t(float t, float *r, float *g, float *b)
{
    if      (t < 0.03f) { *r=0.70f; *g=0.77f; *b=1.00f; }
    else if (t < 0.13f) { *r=0.90f; *g=0.92f; *b=1.00f; }
    else if (t < 0.30f) { *r=1.00f; *g=0.98f; *b=0.85f; }
    else if (t < 0.55f) { *r=1.00f; *g=0.95f; *b=0.70f; }
    else if (t < 0.78f) { *r=1.00f; *g=0.80f; *b=0.50f; }
    else                { *r=1.00f; *g=0.55f; *b=0.35f; }
}

static void procedural_color(float *r, float *g, float *b)
{
    procedural_color_t(randf(), r, g, b);
}

static int build_procedural(StarVertex **out)
{
    int n = NUM_STARS;
    StarVertex *stars = (StarVertex *)malloc((size_t)n * sizeof(StarVertex));
    if (!stars) return 0;

    srand(42);
    for (int i = 0; i < n; i++) {
        float theta = acosf(1.0f - 2.0f * randf());
        float phi   = 2.0f * (float)PI * randf();
        float bright;

        stars[i].pos[0] = sinf(theta) * cosf(phi);
        stars[i].pos[1] = cosf(theta);
        stars[i].pos[2] = sinf(theta) * sinf(phi);

        procedural_color(&stars[i].col[0], &stars[i].col[1], &stars[i].col[2]);
        stars[i].mag = -1.0f + 7.7f * randf();
        bright = display_brightness_from_mag(stars[i].mag);
        stars[i].col[0] *= bright;
        stars[i].col[1] *= bright;
        stars[i].col[2] *= bright;
    }

    *out = stars;
    return n;
}

/* Local RNG for the background layer: fixed-seed xorshift32, so the layer is
 * deterministic and generating it never perturbs libc rand() (the procedural
 * fallback and anything else seeded off it stay reproducible). */
static unsigned int s_bg_rng;
static float bg_randf(void)
{
    unsigned int x = s_bg_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_bg_rng = x;
    return (float)(x >> 8) / 16777216.0f;   /* [0,1) from the top 24 bits */
}

/* Faint background star dust: thousands of sub-catalog stars (m ≈ 5–8.5)
 * filling the sky between the BSC5 stars, 70% concentrated toward the
 * galactic plane so the dust visibly thickens along the Milky Way band.
 * Direction-only skybox geometry, like the catalog stars. */
static int build_background(StarVertex **out, int n)
{
    if (n <= 0) return 0;
    StarVertex *stars = (StarVertex *)malloc((size_t)n * sizeof(StarVertex));
    if (!stars) return 0;

    /* Galactic frame: north pole (J2000 RA 192.859°, Dec 27.128°) in GL,
     * plus any two orthonormal in-plane axes. */
    float pole[3], e1[3], e2[3];
    equatorial_to_gl(192.859, 27.128, pole);
    /* e1 = normalize(any-vector × pole), e2 = pole × e1 */
    e1[0] = -pole[2]; e1[1] = 0.0f; e1[2] = pole[0];
    {
        float l = sqrtf(e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]);
        e1[0] /= l; e1[1] /= l; e1[2] /= l;
    }
    e2[0] = pole[1]*e1[2] - pole[2]*e1[1];
    e2[1] = pole[2]*e1[0] - pole[0]*e1[2];
    e2[2] = pole[0]*e1[1] - pole[1]*e1[0];

    s_bg_rng = 0x9E3779B9u;
    const float lat_s = sinf(11.0f * (float)PI / 180.0f);  /* band half-width */

    for (int i = 0; i < n; i++) {
        float dir[3];
        if (bg_randf() < 0.30f) {
            /* isotropic sprinkle */
            float ct  = 1.0f - 2.0f * bg_randf();
            float st  = sqrtf(1.0f - ct * ct);
            float phi = 2.0f * (float)PI * bg_randf();
            dir[0] = st * cosf(phi);
            dir[1] = ct;
            dir[2] = st * sinf(phi);
        } else {
            /* galactic-plane weighted: exponential drop-off in sin(latitude) */
            float u     = bg_randf();
            float sin_b = lat_s * logf(1.0f / (1.0f - u + 1e-7f));
            if (bg_randf() < 0.5f) sin_b = -sin_b;
            sin_b = clampf(sin_b, -0.999f, 0.999f);
            float cos_b = sqrtf(1.0f - sin_b * sin_b);
            float a     = 2.0f * (float)PI * bg_randf();
            for (int k = 0; k < 3; k++)
                dir[k] = cos_b * (cosf(a) * e1[k] + sinf(a) * e2[k])
                       + sin_b * pole[k];
        }
        stars[i].pos[0] = dir[0];
        stars[i].pos[1] = dir[1];
        stars[i].pos[2] = dir[2];

        /* Faint-end magnitudes, weighted toward the faint side.  Brightness
         * bypasses display_brightness_from_mag(): its 0.18 floor is tuned for
         * naked-eye catalog stars and would make this layer read as noise. */
        float mag    = 5.0f + 3.5f * powf(bg_randf(), 0.4f);
        float bright = 0.2f * powf(10.0f, -0.25f * (mag - 5.0f));

        /* Desaturate toward white: saturated single-pixel colours sparkle. */
        float r, g, b;
        procedural_color_t(bg_randf(), &r, &g, &b);
        r += (1.0f - r) * 0.4f;
        g += (1.0f - g) * 0.4f;
        b += (1.0f - b) * 0.4f;
        stars[i].col[0] = r * bright;
        stars[i].col[1] = g * bright;
        stars[i].col[2] = b * bright;
        stars[i].mag    = mag;
    }

    *out = stars;
    return n;
}

static float *pack_vertices(StarVertex *stars, int n)
{
    float *verts = (float *)malloc((size_t)n * 6 * sizeof(float));
    if (!verts) return NULL;

    qsort(stars, (size_t)n, sizeof(StarVertex), star_cmp_faint_to_bright);

    s_faint_count = s_mid_count = s_bright_count = 0;
    for (int i = 0; i < n; i++) {
        verts[i*6+0] = stars[i].pos[0];
        verts[i*6+1] = stars[i].pos[1];
        verts[i*6+2] = stars[i].pos[2];
        verts[i*6+3] = stars[i].col[0];
        verts[i*6+4] = stars[i].col[1];
        verts[i*6+5] = stars[i].col[2];

        if (stars[i].mag > STAR_MAG_MID)
            s_faint_count++;
        else if (stars[i].mag > STAR_MAG_BRIGHT)
            s_mid_count++;
        else
            s_bright_count++;
    }

    return verts;
}

/* ---------------------------------------------------------------- public */

void starfield_init(void) {
    StarVertex *stars = NULL;
    float *verts;

    /* Re-callable: release any prior GL resources so a settings-driven
     * regenerate (new star count) doesn't leak the old buffers/shader. */
    if (s_vao || s_vbo || s_shader) starfield_shutdown();

    s_shader = gl_shader_load("assets/shaders/color.vert",
                              "assets/shaders/starfield.frag");
    if (!s_shader) return;

    s_loc_vp   = glGetUniformLocation(s_shader, "u_vp");
    s_loc_fade = glGetUniformLocation(s_shader, "u_fade");

    s_count = load_catalog(&stars);
    if (s_count > 0) {
        fprintf(stdout, "[Starfield] loaded %d BSC5 catalog stars\n", s_count);
    } else {
        s_count = build_procedural(&stars);
        fprintf(stdout, "[Starfield] catalog missing; generated %d fallback stars\n", s_count);
    }
    if (s_count <= 0 || !stars) return;

    verts = pack_vertices(stars, s_count);
    free(stars);
    if (!verts) return;

    /* Faint background layer: appended after the three catalog buckets as a
     * fourth block in the same VBO (the catalog sort above never touches it). */
    StarVertex *bg = NULL;
    s_bg_count = build_background(&bg, g_settings.bg_star_count);
    if (s_bg_count > 0) {
        float *grown = (float *)realloc(verts,
                (size_t)(s_count + s_bg_count) * 6 * sizeof(float));
        if (grown) {
            verts = grown;
            for (int i = 0; i < s_bg_count; i++) {
                float *v = verts + (size_t)(s_count + i) * 6;
                v[0] = bg[i].pos[0]; v[1] = bg[i].pos[1]; v[2] = bg[i].pos[2];
                v[3] = bg[i].col[0]; v[4] = bg[i].col[1]; v[5] = bg[i].col[2];
            }
            fprintf(stdout, "[Starfield] + %d background dust stars\n", s_bg_count);
        } else {
            s_bg_count = 0;
        }
    }
    free(bg);

    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create((size_t)(s_count + s_bg_count) * 6 * sizeof(float),
                          verts, GL_STATIC_DRAW);
    free(verts);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float),
                          (void*)(3*sizeof(float)));

    glBindVertexArray(0);
}

void starfield_render(const float view_rot[16], const float proj[16],
                      float fade) {
    if (!s_shader || !s_vao || s_count <= 0 || fade <= 0.001f) return;

    Mat4 vp;
    mat4_mul(vp, proj, view_rot);

    glUseProgram(s_shader);
    glUniformMatrix4fv(s_loc_vp, 1, GL_FALSE, vp);
    glUniform1f(s_loc_fade, fade);

    glBindVertexArray(s_vao);

    if (s_faint_count > 0) {
        glPointSize(1.0f);
        glDrawArrays(GL_POINTS, 0, s_faint_count);
    }
    if (s_mid_count > 0) {
        glPointSize(2.0f);
        glDrawArrays(GL_POINTS, s_faint_count, s_mid_count);
    }
    if (s_bright_count > 0) {
        glPointSize(3.0f);
        glDrawArrays(GL_POINTS, s_faint_count + s_mid_count, s_bright_count);
    }
    if (s_bg_count > 0) {
        glPointSize(1.0f);
        glDrawArrays(GL_POINTS, s_count, s_bg_count);
    }

    glBindVertexArray(0);
    glPointSize(1.0f);
}

void starfield_shutdown(void) {
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    s_vao = s_vbo = s_shader = 0;
    s_count = s_faint_count = s_mid_count = s_bright_count = 0;
    s_bg_count = 0;
}
