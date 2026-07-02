/*
 * nebula.c — real-catalogue nebulae as world-space volumetric clouds.
 *
 * Placement is fully data-based: J2000 RA/Dec (rotated into the ecliptic GL
 * frame, same convention as starfield.c), real distance, and apparent angular
 * size give each nebula a true 3D position and physical radius.  They are real
 * objects in the world — navigable, with parallax — not a painted backdrop.
 *
 * Rendering mirrors the supernova volumetric pass: a screen-space raymarch
 * (nebula.frag) carried by a camera-facing billboard, or a fullscreen quad when
 * the camera is near/inside the volume.  A nebula beyond NEBULA_MAX_DIST is
 * pulled to that shell with its radius scaled by the same factor, preserving
 * angular size, so distant nebulae stay legible as backdrops.  (The star-dot and
 * black-hole passes formerly used this same shell trick but now render at true
 * depth — see farfield_horizon_fade() in render.c — nebulae keep the clamp by
 * design, as visitable volumes that must remain visible from far away.)
 * The clamp is the identity at the
 * boundary, so flying in from the far blob to the enveloping volume is
 * seamless: one raymarch representation, no LOD pop.
 *
 * Real (data-based): position, physical size, per-type colour.  Artistic: the
 * density/brightness (NEBULA_DENSITY) — real nebulae are far too faint to see
 * in colour, so like every planetarium we exaggerate luminance.
 *
 * Data sources: Messier / NGC / IC catalogue positions, distances and
 * major-axis sizes (SIMBAD / NED, J2000).
 */
#include "nebula.h"
#include "gl_utils.h"
#include "common.h"
#include "radiance_field.h"
#include <stdio.h>
#include <math.h>
#include <stddef.h>

#define AU_PER_LY        63241.077
#define NEBULA_MAX_DIST  1500.0    /* AU: clamp shell, safely inside far-plane */
#define NEBULA_BILL      1.30f     /* billboard overscan factor (matches .vert) */

/* Adjustable render params (Visuals menu). Defaults chosen for a readable but
 * not-too-heavy look; density is artistic, steps trades quality for perf. */
static float s_density   = 0.85f;
static int   s_base_steps = 16;    /* near/large nebulae; far ones scaled down */

/* Morphology archetypes — must match shape_env() in nebula.frag. */
enum {
    NEB_DIFFUSE = 0,   /* soft ball: reflection nebulae, big diffuse HII glow */
    NEB_SHELL   = 1,   /* hollow spherical shell: supernova remnants          */
    NEB_RING    = 2,   /* torus: planetary nebulae, ring-shaped HII           */
    NEB_BIPOLAR = 3,   /* two lobes: bipolar planetaries                      */
    NEB_PILLARS = 4,   /* columns rising from a base: star-forming pillars     */
    NEB_CAVITY  = 5,   /* cloud with a blown-out cavity + bright rim          */
};

typedef struct {
    const char *name;
    double ra_deg;        /* J2000 right ascension, degrees   */
    double dec_deg;       /* J2000 declination, degrees       */
    double dist_ly;       /* distance, light-years            */
    double size_arcmin;   /* apparent major-axis diameter      */
    float  col[3];        /* emission-type tint               */
    int    shape;         /* morphology archetype (NEB_*)     */
} NebulaDef;

/* Colour families: emission (HII/Halpha) warm red/pink; reflection cool blue;
 * planetary / SNR teal.  Shape picks the 3D morphology that reads as that real
 * object from any angle (planetary = ring, SNR = shell, Pillars = columns, …). */
static const NebulaDef NEBULAE[] = {
    /* name                  RA(deg)  Dec(deg)  dist(ly)  size'  colour (r,g,b)        shape */
    { "Orion (M42)",          83.82,   -5.39,    1344.0,  65.0, {0.95f,0.45f,0.55f}, NEB_CAVITY  },
    { "Carina (NGC 3372)",   161.27,  -59.87,    7500.0, 120.0, {0.92f,0.32f,0.32f}, NEB_CAVITY  },
    { "Lagoon (M8)",         270.92,  -24.38,    4100.0,  90.0, {0.92f,0.36f,0.42f}, NEB_CAVITY  },
    { "Eagle (M16)",         274.70,  -13.78,    7000.0,  35.0, {0.86f,0.36f,0.42f}, NEB_PILLARS },
    { "Trifid (M20)",        270.60,  -23.03,    5200.0,  28.0, {0.72f,0.40f,0.72f}, NEB_CAVITY  },
    { "Omega/Swan (M17)",    275.20,  -16.18,    5500.0,  46.0, {0.90f,0.40f,0.40f}, NEB_CAVITY  },
    { "North America",       314.82,   44.52,    2590.0, 120.0, {0.86f,0.32f,0.36f}, NEB_DIFFUSE },
    { "Veil (NGC 6960)",     311.43,   30.72,    2400.0, 180.0, {0.45f,0.72f,0.70f}, NEB_SHELL   },
    { "Helix (NGC 7293)",    337.41,  -20.84,     650.0,  16.0, {0.42f,0.72f,0.64f}, NEB_RING    },
    { "Dumbbell (M27)",      299.90,   22.72,    1360.0,   8.0, {0.42f,0.76f,0.60f}, NEB_BIPOLAR },
    { "Rosette (NGC 2237)",   98.44,    4.95,    5200.0,  80.0, {0.90f,0.32f,0.36f}, NEB_RING    },
    { "California (NGC 1499)",60.82,   36.42,    1000.0, 145.0, {0.86f,0.30f,0.34f}, NEB_DIFFUSE },
    { "Heart (IC 1805)",      38.18,   61.45,    7500.0, 150.0, {0.86f,0.30f,0.36f}, NEB_CAVITY  },
    { "Soul (IC 1848)",       42.75,   60.43,    7500.0, 100.0, {0.84f,0.32f,0.36f}, NEB_CAVITY  },
    { "Tarantula (NGC 2070)", 84.68,  -69.10,  160000.0,  40.0, {0.90f,0.40f,0.40f}, NEB_CAVITY  },
    { "Pleiades (M45)",       56.85,   24.12,     444.0, 110.0, {0.52f,0.62f,0.96f}, NEB_DIFFUSE },
    { "Witch Head (IC 2118)", 75.50,   -7.90,     900.0, 180.0, {0.46f,0.56f,0.92f}, NEB_DIFFUSE },
    { "Flame (NGC 2024)",     85.48,   -1.85,    1350.0,  30.0, {0.92f,0.46f,0.34f}, NEB_CAVITY  },
};
#define NEBULA_COUNT ((int)(sizeof(NEBULAE) / sizeof(NEBULAE[0])))

typedef struct {
    double pos[3];    /* world position, AU            */
    double radius;    /* physical bounding radius, AU  */
    float  col[3];
    float  seed;
    int    shape;
} NebulaInst;

static NebulaInst s_neb[NEBULA_COUNT];
static GLuint s_shader = 0, s_vao = 0, s_vbo = 0, s_ebo = 0;
static GLint  s_u_vp, s_u_center, s_u_radius, s_u_right, s_u_up, s_u_fwd;
static GLint  s_u_oc, s_u_color, s_u_density, s_u_seed, s_u_bill, s_u_fullscreen;
static GLint  s_u_fov_tan, s_u_aspect, s_u_screen, s_u_steps, s_u_shape;
static GLint  s_u_boost, s_u_boost_col;
static int    s_enabled = 1;

/* J2000 equatorial RA/Dec -> ecliptic GL unit vector (matches starfield.c). */
static void equatorial_to_gl(double ra_deg, double dec_deg, double out[3])
{
    const double deg = M_PI / 180.0;
    const double eps = 23.4392911 * deg;
    double ce = cos(eps), se = sin(eps);
    double ra = ra_deg * deg, dec = dec_deg * deg;
    double x_eq = cos(dec) * cos(ra);
    double y_eq = cos(dec) * sin(ra);
    double z_eq = sin(dec);
    out[0] = x_eq;
    out[1] = -y_eq * se + z_eq * ce;   /* GL Y = ecliptic Z (north pole) */
    out[2] =  y_eq * ce + z_eq * se;   /* GL Z = ecliptic Y              */
}

void nebula_init(void)
{
    s_shader = gl_shader_load("assets/shaders/nebula.vert",
                              "assets/shaders/nebula.frag");
    if (!s_shader) {
        fprintf(stdout, "[Nebula] shader load failed; nebulae disabled\n");
        return;
    }

    s_u_vp         = glGetUniformLocation(s_shader, "u_vp");
    s_u_center     = glGetUniformLocation(s_shader, "u_center");
    s_u_radius     = glGetUniformLocation(s_shader, "u_radius");
    s_u_right      = glGetUniformLocation(s_shader, "u_cam_right");
    s_u_up         = glGetUniformLocation(s_shader, "u_cam_up");
    s_u_fwd        = glGetUniformLocation(s_shader, "u_cam_fwd");
    s_u_oc         = glGetUniformLocation(s_shader, "u_oc");
    s_u_color      = glGetUniformLocation(s_shader, "u_color");
    s_u_density    = glGetUniformLocation(s_shader, "u_density");
    s_u_seed       = glGetUniformLocation(s_shader, "u_seed");
    s_u_bill       = glGetUniformLocation(s_shader, "u_bill_scale");
    s_u_fullscreen = glGetUniformLocation(s_shader, "u_fullscreen");
    s_u_fov_tan    = glGetUniformLocation(s_shader, "u_fov_tan");
    s_u_aspect     = glGetUniformLocation(s_shader, "u_aspect");
    s_u_screen     = glGetUniformLocation(s_shader, "u_screen");
    s_u_steps      = glGetUniformLocation(s_shader, "u_steps");
    s_u_shape      = glGetUniformLocation(s_shader, "u_shape");
    s_u_boost      = glGetUniformLocation(s_shader, "u_boost");
    s_u_boost_col  = glGetUniformLocation(s_shader, "u_boost_col");

    const double arcmin = (M_PI / 180.0) / 60.0;
    for (int i = 0; i < NEBULA_COUNT; i++) {
        double dir[3];
        equatorial_to_gl(NEBULAE[i].ra_deg, NEBULAE[i].dec_deg, dir);
        double dist_au = NEBULAE[i].dist_ly * AU_PER_LY;
        double ang_r   = NEBULAE[i].size_arcmin * 0.5 * arcmin;   /* radians */
        s_neb[i].pos[0] = dir[0] * dist_au;
        s_neb[i].pos[1] = dir[1] * dist_au;
        s_neb[i].pos[2] = dir[2] * dist_au;
        s_neb[i].radius = dist_au * ang_r;          /* physical radius, AU */
        s_neb[i].col[0] = NEBULAE[i].col[0];
        s_neb[i].col[1] = NEBULAE[i].col[1];
        s_neb[i].col[2] = NEBULAE[i].col[2];
        s_neb[i].seed   = (float)i * 13.137f;
        s_neb[i].shape  = NEBULAE[i].shape;
    }

    static const float quad[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    static const unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
    s_vao = gl_vao_create();
    s_vbo = gl_vbo_create(sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    s_ebo = gl_ebo_create(sizeof(idx), idx);
    glBindVertexArray(0);

    fprintf(stdout, "[Nebula] placed %d catalogue nebulae\n", NEBULA_COUNT);
}

/* Choose a fullscreen raster when a world-space billboard would degenerate:
 * the camera is near/inside the volume, or the billboard would punch through
 * the far-plane.  Same tests as the supernova pass. */
static int needs_fullscreen(const float center[3], const float cam_fwd[3],
                            float radius)
{
    const float overscan = 2.0f;
    const float far_guard = 1850.0f;
    float eye_z = center[0]*cam_fwd[0] + center[1]*cam_fwd[1] + center[2]*cam_fwd[2];
    float half  = radius * NEBULA_BILL * overscan;
    if (eye_z < fmaxf(half * 1.05f, 0.18f)) return 1;  /* inside / near plane */
    if (eye_z + half >= far_guard)          return 1;  /* crosses far-plane   */
    return 0;
}

void nebula_render(const float vp_camrel[16],
                   const float cam_right[3], const float cam_up[3],
                   const float cam_fwd[3], const double cam_pos[3],
                   float fov_tan, float aspect, int screen_w, int screen_h)
{
    if (!s_enabled || !s_shader || !s_vao) return;

    glUseProgram(s_shader);
    glUniformMatrix4fv(s_u_vp, 1, GL_FALSE, vp_camrel);
    glUniform3fv(s_u_right, 1, cam_right);
    glUniform3fv(s_u_up,    1, cam_up);
    glUniform3fv(s_u_fwd,   1, cam_fwd);
    glUniform1f (s_u_fov_tan, fov_tan);
    glUniform1f (s_u_aspect,  aspect);
    glUniform2f (s_u_screen, (float)screen_w, (float)screen_h);
    glUniform1f (s_u_bill,   NEBULA_BILL);
    glUniform1f (s_u_density, s_density);

    /* Premultiplied "over"; depth-tested against opaque geometry, no writes. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(s_vao);

    float half_h = (float)screen_h * 0.5f;
    for (int i = 0; i < NEBULA_COUNT; i++) {
        /* Camera-relative centre + radius, with the angular-size-preserving
         * far clamp (pull distant nebulae onto a shell, scale radius to match). */
        double rx = s_neb[i].pos[0] - cam_pos[0];
        double ry = s_neb[i].pos[1] - cam_pos[1];
        double rz = s_neb[i].pos[2] - cam_pos[2];
        double dist = sqrt(rx*rx + ry*ry + rz*rz);
        double radius = s_neb[i].radius;
        if (dist > NEBULA_MAX_DIST && dist > 1e-9) {
            double s = NEBULA_MAX_DIST / dist;
            rx *= s; ry *= s; rz *= s; radius *= s;
        }
        float center[3] = { (float)rx, (float)ry, (float)rz };
        float radf = (float)radius;

        int fullscreen = needs_fullscreen(center, cam_fwd, radf);

        /* Adaptive step count + cheap culling for the far billboard case. The
         * near/inside (fullscreen) case keeps the full step budget. */
        int steps = s_base_steps;
        if (!fullscreen) {
            float eye_z = center[0]*cam_fwd[0] + center[1]*cam_fwd[1]
                        + center[2]*cam_fwd[2];
            if (eye_z <= 0.0f) continue;                 /* behind camera */
            float proj_px = radf / eye_z / fov_tan * half_h;
            if (proj_px < 0.6f) continue;                /* sub-pixel: skip */
            /* Fewer steps the smaller it is on screen. */
            float f = proj_px / (half_h * 0.5f);
            if (f > 1.0f) f = 1.0f;
            steps = (int)(s_base_steps * f);
            if (steps < 6) steps = 6;
        }

        glUniform1f (s_u_fullscreen, fullscreen ? 1.0f : 0.0f);
        glUniform1i (s_u_steps, steps);
        glUniform1i (s_u_shape, s_neb[i].shape);
        glUniform3fv(s_u_center, 1, center);
        glUniform1f (s_u_radius, radf);
        glUniform3f (s_u_oc, -center[0], -center[1], -center[2]);
        glUniform3fv(s_u_color, 1, s_neb[i].col);
        glUniform1f (s_u_seed, s_neb[i].seed);

        /* RadianceField illumination (roadmap 4.1 "dynamic energy injection"):
         * the nebula is a light *receiver* — a strong emitter at/near it (an
         * embedded star flown in, an AGN, above all a supernova going off
         * inside it) brightens the glow and pulls the tint toward the source.
         * The baseline authored look is the floor: catalogue starlight at
         * interstellar distance is ~1e-7 W/m², far below IRR_REF, so boost
         * stays 1.0 and every existing scene is untouched. */
        {
            const double IRR_REF   = 1e-3;   /* W/m² where brightening starts */
            const float  BOOST_MAX = 3.5f;
            float boost = 1.0f, bcol[3] = { 1.0f, 1.0f, 1.0f };
            RadianceContrib top[1];
            double p_m[3] = { s_neb[i].pos[0] * AU,
                              s_neb[i].pos[1] * AU,
                              s_neb[i].pos[2] * AU };
            if (radiance_field_top(p_m, -1, 1, top) >= 1 && top[0].irr > 0.0) {
                boost = 1.0f + 1.2f * (float)log10(1.0 + top[0].irr / IRR_REF);
                if (boost > BOOST_MAX) boost = BOOST_MAX;
                float t = boost - 1.0f;
                if (t > 1.0f) t = 1.0f;
                t *= 0.6f;   /* partial tint: keep the nebula's own species colour */
                bcol[0] = 1.0f + (top[0].col[0] - 1.0f) * t;
                bcol[1] = 1.0f + (top[0].col[1] - 1.0f) * t;
                bcol[2] = 1.0f + (top[0].col[2] - 1.0f) * t;
            }
            glUniform1f (s_u_boost, boost);
            glUniform3fv(s_u_boost_col, 1, bcol);
        }

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void nebula_shutdown(void)
{
    glDeleteBuffers(1, &s_vbo);
    glDeleteBuffers(1, &s_ebo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    s_vao = s_vbo = s_ebo = s_shader = 0;
}

void nebula_set_enabled(int enabled) { s_enabled = enabled ? 1 : 0; }
int  nebula_enabled(void)            { return s_enabled; }

void nebula_get_params(int *enabled, float *density, int *steps)
{
    if (enabled) *enabled = s_enabled;
    if (density) *density = s_density;
    if (steps)   *steps   = s_base_steps;
}

void nebula_set_params(int enabled, float density, int steps)
{
    s_enabled = enabled ? 1 : 0;
    s_density = density < 0.0f ? 0.0f : density;
    s_base_steps = steps < 4 ? 4 : (steps > 48 ? 48 : steps);
}

int         nebula_count(void)       { return NEBULA_COUNT; }
const char *nebula_name(int i)
{
    return (i >= 0 && i < NEBULA_COUNT) ? NEBULAE[i].name : "";
}

void nebula_position(int i, double out[3])
{
    if (i < 0 || i >= NEBULA_COUNT) { out[0] = out[1] = out[2] = 0.0; return; }
    out[0] = s_neb[i].pos[0];
    out[1] = s_neb[i].pos[1];
    out[2] = s_neb[i].pos[2];
}

double nebula_radius_au(int i)
{
    return (i >= 0 && i < NEBULA_COUNT) ? s_neb[i].radius : 0.0;
}
