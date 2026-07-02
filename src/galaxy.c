/*
 * galaxy.c — real-catalogue galaxies as world-space volumetric structures.
 * See galaxy.h. Mirrors nebula.c (same carrier shader, clamp shell, and
 * culling); the galaxy-specific density model lives in galaxy.frag.
 *
 * Real (data-based): position, distance, physical size, morphological type,
 * inclination (so the Earth view matches the iconic appearance). Artistic:
 * brightness (like the nebulae — real surface brightness is far below
 * perception) and the procedural arm/dust detail.
 *
 * Data: SIMBAD/NED J2000 positions, distances, major-axis sizes, inclinations.
 */
#include "galaxy.h"
#include "gl_utils.h"
#include "common.h"
#include <stdio.h>
#include <math.h>

#define AU_PER_LY        63241.077
#define GALAXY_MAX_DIST  1400.0    /* AU clamp shell: just inside the nebula
                                    * shell (1500) so a galaxy behind a nebula
                                    * keeps drawing behind it */
#define GALAXY_BILL      1.30f     /* billboard overscan (matches nebula.vert) */

static float s_density    = 0.85f;
static int   s_base_steps = 18;

/* Morphology — must match galaxy.frag u_type. */
enum {
    GAL_SPIRAL     = 0,
    GAL_ELLIPTICAL = 1,
    GAL_IRREGULAR  = 2,
};

typedef struct {
    const char *name;
    double ra_deg;        /* J2000 right ascension, degrees            */
    double dec_deg;       /* J2000 declination, degrees                */
    double dist_ly;       /* distance, light-years                     */
    double size_arcmin;   /* apparent major-axis diameter, arcmin      */
    double radius_ly;     /* explicit physical radius (>0 overrides    */
                          /* the angular-size derivation — used for    */
                          /* the Milky Way, whose apparent size from   */
                          /* inside is meaningless)                    */
    double incl_deg;      /* inclination: 0 = face-on from Earth,      */
                          /* 90 = edge-on from Earth (ignored when a   */
                          /* real pole is given)                       */
    double pole_ra_deg;   /* explicit disc-axis (north pole) J2000     */
    double pole_dec_deg;  /* coords; pole_ra < -900 = none, derive the */
                          /* axis from incl_deg instead                */
    float  brightness;    /* per-galaxy density scale (the Milky Way   */
                          /* is seen from inside — keep the band a     */
                          /* subtle veil, not fog)                     */
    float  col[3];        /* overall stellar-population tint           */
    int    type;          /* GAL_* morphology                          */
} GalaxyDef;

static const GalaxyDef GALAXIES[] = {
    /* The home galaxy: centred 26 kly toward Sgr A*, disc axis = the real
     * galactic north pole, so Sol sits in the midplane at r ≈ 0.5 — from
     * inside it renders as the Milky Way band (brightest toward Sagittarius),
     * and zooming out it coalesces into a spiral seen from outside: the
     * §0.1 scale-continuity "leave your own galaxy" experience. */
    { "Milky Way",             266.405, -28.936,  2.60e4,     0.0, 5.0e4, 0.0, 192.859,  27.128, 0.14f, {0.90f,0.88f,0.84f}, GAL_SPIRAL     },
    /* name                    RA(deg)  Dec(deg)  dist(ly)   size'  r_ly  incl  pole_ra  pole_dec bright colour              type */
    { "LMC",                    80.89,  -69.76,   1.63e5,   645.0,  0.0, 35.0, -999.0,   0.0,    1.0f, {0.80f,0.82f,0.95f}, GAL_IRREGULAR  },
    { "SMC",                    13.19,  -72.83,   2.00e5,   320.0,  0.0, 50.0, -999.0,   0.0,    1.0f, {0.78f,0.80f,0.94f}, GAL_IRREGULAR  },
    { "Andromeda (M31)",        10.68,   41.27,   2.54e6,   190.0,  0.0, 77.0, -999.0,   0.0,    1.0f, {0.94f,0.88f,0.78f}, GAL_SPIRAL     },
    { "Triangulum (M33)",       23.46,   30.66,   2.73e6,    71.0,  0.0, 54.0, -999.0,   0.0,    1.0f, {0.82f,0.86f,0.96f}, GAL_SPIRAL     },
    { "Bode's (M81)",          148.89,   69.07,   1.18e7,    27.0,  0.0, 59.0, -999.0,   0.0,    1.0f, {0.92f,0.87f,0.78f}, GAL_SPIRAL     },
    { "Sculptor (NGC 253)",     11.89,  -25.29,   1.14e7,    27.0,  0.0, 78.0, -999.0,   0.0,    1.0f, {0.90f,0.82f,0.70f}, GAL_SPIRAL     },
    { "Centaurus A",           201.37,  -43.02,   1.20e7,    26.0,  0.0, 40.0, -999.0,   0.0,    1.0f, {0.88f,0.82f,0.74f}, GAL_ELLIPTICAL },
    { "Whirlpool (M51)",       202.47,   47.20,   2.30e7,    11.0,  0.0, 22.0, -999.0,   0.0,    1.0f, {0.80f,0.86f,0.98f}, GAL_SPIRAL     },
    { "Sombrero (M104)",       190.00,  -11.62,   2.93e7,     9.0,  0.0, 84.0, -999.0,   0.0,    1.0f, {0.93f,0.88f,0.78f}, GAL_SPIRAL     },
    { "Virgo A (M87)",         187.71,   12.39,   5.30e7,     8.0,  0.0,  0.0, -999.0,   0.0,    1.0f, {0.92f,0.88f,0.80f}, GAL_ELLIPTICAL },
};
#define GALAXY_COUNT ((int)(sizeof(GALAXIES) / sizeof(GALAXIES[0])))

typedef struct {
    double pos[3];    /* world position, AU           */
    double radius;    /* physical bounding radius, AU */
    float  col[3];
    float  axis[3];   /* disc spin axis (unit)        */
    float  seed;
    float  brightness;
    int    type;
} GalaxyInst;

/* Procedural star cascades (galaxy_render_stars): cubic lattices around the
 * camera, cell sizes in ly; each cascade covers GS_GRID_DIM/2 cells of
 * Chebyshev radius and leaves its interior to the next-finer one. Candidate
 * count per cascade = GS_GRID_DIM^3 * GS_PER_CELL (must match the shader). */
#define GS_GRID_DIM  20
#define GS_PER_CELL  5                       /* keep in sync with the .vert */
#define GS_CASCADES  6
static const double GS_CELL_LY[GS_CASCADES] = { 2, 8, 32, 128, 512, 2048 };
#define GS_ENTER_FRAC 1.35   /* start resolving stars inside this × radius */

static GalaxyInst s_gal[GALAXY_COUNT];
static GLuint s_shader = 0, s_vao = 0, s_vbo = 0, s_ebo = 0;
static GLuint s_star_shader = 0, s_star_vao = 0;
static GLint  s_su_vp, s_su_cell_base, s_su_origin_rel, s_su_cell_size;
static GLint  s_su_grid_dim, s_su_inner, s_su_outer, s_su_cam_in_gal;
static GLint  s_su_radius, s_su_axis, s_su_seed, s_su_type, s_su_time;
static GLint  s_su_gain, s_su_lum;
static GLint  s_u_vp, s_u_center, s_u_radius, s_u_right, s_u_up, s_u_fwd;
static GLint  s_u_oc, s_u_color, s_u_density, s_u_seed, s_u_bill, s_u_fullscreen;
static GLint  s_u_fov_tan, s_u_aspect, s_u_screen, s_u_steps, s_u_type;
static GLint  s_u_axis, s_u_time;
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

void galaxy_init(void)
{
    /* nebula.vert is a generic raymarch carrier (billboard / fullscreen with
     * the same uniform set) — reuse it; only the fragment stage is new. */
    s_shader = gl_shader_load("assets/shaders/nebula.vert",
                              "assets/shaders/galaxy.frag");
    if (!s_shader) {
        fprintf(stdout, "[Galaxy] shader load failed; galaxies disabled\n");
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
    s_u_type       = glGetUniformLocation(s_shader, "u_type");
    s_u_axis       = glGetUniformLocation(s_shader, "u_axis");
    s_u_time       = glGetUniformLocation(s_shader, "u_time");

    const double arcmin = (M_PI / 180.0) / 60.0;
    for (int i = 0; i < GALAXY_COUNT; i++) {
        double dir[3];
        equatorial_to_gl(GALAXIES[i].ra_deg, GALAXIES[i].dec_deg, dir);
        double dist_au = GALAXIES[i].dist_ly * AU_PER_LY;
        double ang_r   = GALAXIES[i].size_arcmin * 0.5 * arcmin;
        s_gal[i].pos[0] = dir[0] * dist_au;
        s_gal[i].pos[1] = dir[1] * dist_au;
        s_gal[i].pos[2] = dir[2] * dist_au;
        s_gal[i].radius = GALAXIES[i].radius_ly > 0.0
                        ? GALAXIES[i].radius_ly * AU_PER_LY
                        : dist_au * ang_r;
        s_gal[i].col[0] = GALAXIES[i].col[0];
        s_gal[i].col[1] = GALAXIES[i].col[1];
        s_gal[i].col[2] = GALAXIES[i].col[2];
        s_gal[i].seed   = 11.71f + (float)i * 17.313f;
        s_gal[i].brightness = GALAXIES[i].brightness;
        s_gal[i].type   = GALAXIES[i].type;

        /* Disc axis: an explicit catalogued pole (Milky Way) wins. */
        if (GALAXIES[i].pole_ra_deg > -900.0) {
            double pole[3];
            equatorial_to_gl(GALAXIES[i].pole_ra_deg, GALAXIES[i].pole_dec_deg,
                             pole);
            s_gal[i].axis[0] = (float)pole[0];
            s_gal[i].axis[1] = (float)pole[1];
            s_gal[i].axis[2] = (float)pole[2];
        } else
        /* Otherwise from the catalogued inclination: tilt the sightline
         * direction toward a stable perpendicular. axis = dir → face-on from
         * Earth; axis ⊥ dir → edge-on. The perpendicular is rotated about the
         * sightline by a per-galaxy angle so position angles vary. */
        {
            double up[3] = { 0.21, 0.94, 0.27 };
            double d = dir[0]*up[0] + dir[1]*up[1] + dir[2]*up[2];
            double perp[3] = { up[0] - dir[0]*d, up[1] - dir[1]*d,
                               up[2] - dir[2]*d };
            double pl = sqrt(perp[0]*perp[0] + perp[1]*perp[1] + perp[2]*perp[2]);
            if (pl < 1e-9) { perp[0] = 1.0; perp[1] = perp[2] = 0.0; pl = 1.0; }
            perp[0] /= pl; perp[1] /= pl; perp[2] /= pl;
            /* position angle: rotate perp about dir */
            double pa = (double)s_gal[i].seed;
            double cp = cos(pa), sp = sin(pa);
            double crx = dir[1]*perp[2] - dir[2]*perp[1];
            double cry = dir[2]*perp[0] - dir[0]*perp[2];
            double crz = dir[0]*perp[1] - dir[1]*perp[0];
            double px = perp[0]*cp + crx*sp;
            double py = perp[1]*cp + cry*sp;
            double pz = perp[2]*cp + crz*sp;
            double th = GALAXIES[i].incl_deg * M_PI / 180.0;
            s_gal[i].axis[0] = (float)(dir[0]*cos(th) + px*sin(th));
            s_gal[i].axis[1] = (float)(dir[1]*cos(th) + py*sin(th));
            s_gal[i].axis[2] = (float)(dir[2]*cos(th) + pz*sin(th));
        }
    }

    /* Star cascades: attribute-less point draw (positions from gl_VertexID),
     * so only an empty VAO is needed. */
    s_star_shader = gl_shader_load("assets/shaders/galaxy_stars.vert",
                                   "assets/shaders/galaxy_stars.frag");
    if (s_star_shader) {
        s_su_vp         = glGetUniformLocation(s_star_shader, "u_vp");
        s_su_cell_base  = glGetUniformLocation(s_star_shader, "u_cell_base");
        s_su_origin_rel = glGetUniformLocation(s_star_shader, "u_origin_rel");
        s_su_cell_size  = glGetUniformLocation(s_star_shader, "u_cell_size");
        s_su_grid_dim   = glGetUniformLocation(s_star_shader, "u_grid_dim");
        s_su_inner      = glGetUniformLocation(s_star_shader, "u_inner_half");
        s_su_outer      = glGetUniformLocation(s_star_shader, "u_outer_half");
        s_su_cam_in_gal = glGetUniformLocation(s_star_shader, "u_cam_in_gal");
        s_su_radius     = glGetUniformLocation(s_star_shader, "u_radius_gal");
        s_su_axis       = glGetUniformLocation(s_star_shader, "u_axis");
        s_su_seed       = glGetUniformLocation(s_star_shader, "u_seed");
        s_su_type       = glGetUniformLocation(s_star_shader, "u_type");
        s_su_time       = glGetUniformLocation(s_star_shader, "u_time");
        s_su_gain       = glGetUniformLocation(s_star_shader, "u_gain");
        s_su_lum        = glGetUniformLocation(s_star_shader, "u_lum_scale");
        s_star_vao      = gl_vao_create();
        glBindVertexArray(0);
    } else {
        fprintf(stdout, "[Galaxy] star shader load failed; "
                        "resolved stars disabled\n");
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

    fprintf(stdout, "[Galaxy] placed %d catalogue galaxies\n", GALAXY_COUNT);
}

/* Same near/inside + far-plane tests as the nebula pass. */
static int needs_fullscreen(const float center[3], const float cam_fwd[3],
                            float radius)
{
    const float overscan = 2.0f;
    const float far_guard = 1850.0f;
    float eye_z = center[0]*cam_fwd[0] + center[1]*cam_fwd[1] + center[2]*cam_fwd[2];
    float half  = radius * GALAXY_BILL * overscan;
    if (eye_z < fmaxf(half * 1.05f, 0.18f)) return 1;
    if (eye_z + half >= far_guard)          return 1;
    return 0;
}

void galaxy_render(const float vp_camrel[16],
                   const float cam_right[3], const float cam_up[3],
                   const float cam_fwd[3], const double cam_pos[3],
                   float fov_tan, float aspect, int screen_w, int screen_h,
                   float time_s)
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
    glUniform1f (s_u_bill,   GALAXY_BILL);
    glUniform1f (s_u_density, s_density);
    glUniform1f (s_u_time,    time_s);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(s_vao);

    float half_h = (float)screen_h * 0.5f;
    for (int i = 0; i < GALAXY_COUNT; i++) {
        double rx = s_gal[i].pos[0] - cam_pos[0];
        double ry = s_gal[i].pos[1] - cam_pos[1];
        double rz = s_gal[i].pos[2] - cam_pos[2];
        double dist = sqrt(rx*rx + ry*ry + rz*rz);
        double radius = s_gal[i].radius;
        if (dist > GALAXY_MAX_DIST && dist > 1e-9) {
            double s = GALAXY_MAX_DIST / dist;
            rx *= s; ry *= s; rz *= s; radius *= s;
        }
        float center[3] = { (float)rx, (float)ry, (float)rz };
        float radf = (float)radius;

        int fullscreen = needs_fullscreen(center, cam_fwd, radf);

        int steps = s_base_steps;
        if (!fullscreen) {
            float eye_z = center[0]*cam_fwd[0] + center[1]*cam_fwd[1]
                        + center[2]*cam_fwd[2];
            if (eye_z <= 0.0f) continue;                 /* behind camera */
            float proj_px = radf / eye_z / fov_tan * half_h;
            if (proj_px < 0.7f) continue;                /* sub-pixel: skip */
            float f = proj_px / (half_h * 0.5f);
            if (f > 1.0f) f = 1.0f;
            steps = (int)(s_base_steps * f);
            if (steps < 8) steps = 8;
        }

        /* A reduced brightness (Milky Way) is the *inside* veil level; seen
         * from outside the same galaxy is a distant object like any other,
         * so blend back to full as the camera leaves the volume. */
        float bright = s_gal[i].brightness;
        if (bright < 1.0f && s_gal[i].radius > 0.0) {
            float k = (float)(dist / s_gal[i].radius - 0.9) / 0.8f;
            if (k < 0.0f) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            bright += (1.0f - bright) * k * k * (3.0f - 2.0f * k);
        }

        glUniform1f (s_u_fullscreen, fullscreen ? 1.0f : 0.0f);
        glUniform1f (s_u_density, s_density * bright);
        glUniform1i (s_u_steps, steps);
        glUniform1i (s_u_type,  s_gal[i].type);
        glUniform3fv(s_u_axis, 1, s_gal[i].axis);
        glUniform3fv(s_u_center, 1, center);
        glUniform1f (s_u_radius, radf);
        glUniform3f (s_u_oc, -center[0], -center[1], -center[2]);
        glUniform3fv(s_u_color, 1, s_gal[i].col);
        glUniform1f (s_u_seed, s_gal[i].seed);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void galaxy_render_stars(const float vp_camrel[16], const double cam_pos[3],
                         float gain, float time_s)
{
    if (!s_enabled || !s_star_shader || !s_star_vao || gain <= 0.002f) return;

    glUseProgram(s_star_shader);
    glUniformMatrix4fv(s_su_vp, 1, GL_FALSE, vp_camrel);
    glUniform1i(s_su_grid_dim, GS_GRID_DIM);
    glUniform1f(s_su_time, time_s);
    glUniform1f(s_su_gain, gain);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);        /* additive light over the glow  */
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);            /* planets still occlude a star  */
    glBindVertexArray(s_star_vao);

    const int n_points = GS_GRID_DIM * GS_GRID_DIM * GS_GRID_DIM * GS_PER_CELL;

    for (int i = 0; i < GALAXY_COUNT; i++) {
        /* Camera in this galaxy's frame (double: centres are up to 3e12 AU
         * out; all camera-relative floats below stay small). */
        double gx = cam_pos[0] - s_gal[i].pos[0];
        double gy = cam_pos[1] - s_gal[i].pos[1];
        double gz = cam_pos[2] - s_gal[i].pos[2];
        double dist = sqrt(gx*gx + gy*gy + gz*gz);
        if (dist > GS_ENTER_FRAC * s_gal[i].radius) continue;

        glUniform3f(s_su_cam_in_gal, (float)(gx / s_gal[i].radius),
                                     (float)(gy / s_gal[i].radius),
                                     (float)(gz / s_gal[i].radius));
        glUniform1f (s_su_radius, (float)s_gal[i].radius);
        glUniform3fv(s_su_axis, 1, s_gal[i].axis);
        glUniform1f (s_su_seed, s_gal[i].seed);
        glUniform1i (s_su_type, s_gal[i].type);

        double inner = 0.0;
        for (int k = 0; k < GS_CASCADES; k++) {
            double cell  = GS_CELL_LY[k] * AU_PER_LY;
            double outer = cell * 0.5 * (double)GS_GRID_DIM;

            /* Lattice cell of the grid corner, absolute in galaxy frame:
             * anchors the hashes so stars are stable world objects. */
            long bx = (long)floor(gx / cell) - GS_GRID_DIM / 2;
            long by = (long)floor(gy / cell) - GS_GRID_DIM / 2;
            long bz = (long)floor(gz / cell) - GS_GRID_DIM / 2;

            glUniform3i(s_su_cell_base, (int)bx, (int)by, (int)bz);
            glUniform3f(s_su_origin_rel, (float)((double)bx * cell - gx),
                                         (float)((double)by * cell - gy),
                                         (float)((double)bz * cell - gz));
            glUniform1f(s_su_cell_size, (float)cell);
            glUniform1f(s_su_inner, (float)inner);
            glUniform1f(s_su_outer, (float)outer);
            {
                double rel = GS_CELL_LY[k] / GS_CELL_LY[0];
                glUniform1f(s_su_lum, (float)(rel * rel));
            }
            glDrawArrays(GL_POINTS, 0, n_points);

            inner = outer;
        }
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_PROGRAM_POINT_SIZE);
}

void galaxy_shutdown(void)
{
    glDeleteBuffers(1, &s_vbo);
    glDeleteBuffers(1, &s_ebo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_shader);
    glDeleteVertexArrays(1, &s_star_vao);
    glDeleteProgram(s_star_shader);
    s_vao = s_vbo = s_ebo = s_shader = 0;
    s_star_vao = s_star_shader = 0;
}

void galaxy_set_enabled(int enabled) { s_enabled = enabled ? 1 : 0; }
int  galaxy_enabled(void)            { return s_enabled; }

int         galaxy_count(void)       { return GALAXY_COUNT; }
const char *galaxy_name(int i)
{
    return (i >= 0 && i < GALAXY_COUNT) ? GALAXIES[i].name : "";
}

void galaxy_position(int i, double out[3])
{
    if (i < 0 || i >= GALAXY_COUNT) { out[0] = out[1] = out[2] = 0.0; return; }
    out[0] = s_gal[i].pos[0];
    out[1] = s_gal[i].pos[1];
    out[2] = s_gal[i].pos[2];
}

double galaxy_radius_au(int i)
{
    return (i >= 0 && i < GALAXY_COUNT) ? s_gal[i].radius : 0.0;
}

void galaxy_color(int i, float out[3])
{
    if (i < 0 || i >= GALAXY_COUNT) { out[0] = out[1] = out[2] = 1.0f; return; }
    out[0] = s_gal[i].col[0];
    out[1] = s_gal[i].col[1];
    out[2] = s_gal[i].col[2];
}
