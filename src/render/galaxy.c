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
#include "render.h"
#include "cinematic.h"
#include "starsys.h"     /* suppressed cells: promoted stars are real bodies */
#include "gl_utils.h"
#include "common.h"
#include "body.h"        /* g_bodies: post-set BH fields on the spawned nucleus */
#include "universe.h"    /* universe_add_body / BodyCreateSpec                  */
#include "laws.h"        /* laws_schwarzschild_radius                           */
#include "accretion.h"   /* accretion_init_body (seeds spin + gas reservoir)    */
#include "settings.h"    /* g_settings.galaxy_agn gate                          */
#include "stellar_lf.h"  /* measured LF + catalog detection limit (derive_lf.py) */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define AU_PER_LY        63241.077
#define MSUN             1.989e30    /* kg per solar mass (AGN host BH masses) */
#define GALAXY_MAX_DIST  1400.0    /* AU clamp shell: just inside the nebula
                                    * shell (1500) so a galaxy behind a nebula
                                    * keeps drawing behind it */
#define GALAXY_BILL      1.30f     /* billboard overscan (matches nebula.vert) */

static float s_density       = 0.85f;
static int   s_base_steps    = 18;
static int   s_stars_enabled = 1;   /* resolved-stars pass (galaxy_render_stars) */

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
    double bh_mass_msun;  /* central SMBH mass (solar masses); 0 = no  */
                          /* nucleus. Drives Rs, so it sets the whole  */
                          /* AGN render scale via bh_scales().         */
    float  agn_activity;  /* Eddington ratio: 0 quiescent (bare shadow */
                          /* + faint disk), >0 active (jets + torus).  */
    float  agn_torus;     /* dust-torus strength (active hosts only)   */
} GalaxyDef;

static const GalaxyDef GALAXIES[] = {
    /* The home galaxy: centred 26 kly toward Sgr A*, disc axis = the real
     * galactic north pole, so Sol sits in the midplane at r ≈ 0.5 — from
     * inside it renders as the Milky Way band (brightest toward Sagittarius),
     * and zooming out it coalesces into a spiral seen from outside: the
     * §0.1 scale-continuity "leave your own galaxy" experience. */
    /* AGN nuclei (bh_mass in M☉, Eddington ratio, dust-torus): Sgr A* + M31 are
     * quiescent (bare shadow + faint disk); Centaurus A + M87 are the real active
     * hosts (jets). Rows with mass 0 have no nucleus. Masses: SIMBAD/literature. */
    { "Milky Way",             266.405, -28.936,  2.60e4,     0.0, 5.0e4, 0.0, 192.859,  27.128, 0.20f, {0.90f,0.88f,0.84f}, GAL_SPIRAL    , 4.15e6, 0.0f, 0.0f },
    /* name                    RA(deg)  Dec(deg)  dist(ly)   size'  r_ly  incl  pole_ra  pole_dec bright colour              type            bh_M☉  act  torus */
    { "LMC",                    80.89,  -69.76,   1.63e5,   645.0,  0.0, 35.0, -999.0,   0.0,    1.0f, {0.80f,0.82f,0.95f}, GAL_IRREGULAR , 0.0,    0.0f, 0.0f },
    { "SMC",                    13.19,  -72.83,   2.00e5,   320.0,  0.0, 50.0, -999.0,   0.0,    1.0f, {0.78f,0.80f,0.94f}, GAL_IRREGULAR , 0.0,    0.0f, 0.0f },
    { "Andromeda (M31)",        10.68,   41.27,   2.54e6,   190.0,  0.0, 77.0, -999.0,   0.0,    1.0f, {0.94f,0.88f,0.78f}, GAL_SPIRAL    , 1.4e8,  0.0f, 0.0f },
    { "Triangulum (M33)",       23.46,   30.66,   2.73e6,    71.0,  0.0, 54.0, -999.0,   0.0,    1.0f, {0.82f,0.86f,0.96f}, GAL_SPIRAL    , 0.0,    0.0f, 0.0f },
    { "Bode's (M81)",          148.89,   69.07,   1.18e7,    27.0,  0.0, 59.0, -999.0,   0.0,    1.0f, {0.92f,0.87f,0.78f}, GAL_SPIRAL    , 0.0,    0.0f, 0.0f },
    { "Sculptor (NGC 253)",     11.89,  -25.29,   1.14e7,    27.0,  0.0, 78.0, -999.0,   0.0,    1.0f, {0.90f,0.82f,0.70f}, GAL_SPIRAL    , 0.0,    0.0f, 0.0f },
    { "Centaurus A",           201.37,  -43.02,   1.20e7,    26.0,  0.0, 40.0, -999.0,   0.0,    1.0f, {0.88f,0.82f,0.74f}, GAL_ELLIPTICAL, 5.5e7,  0.7f, 1.0f },
    { "Whirlpool (M51)",       202.47,   47.20,   2.30e7,    11.0,  0.0, 22.0, -999.0,   0.0,    1.0f, {0.80f,0.86f,0.98f}, GAL_SPIRAL    , 0.0,    0.0f, 0.0f },
    { "Sombrero (M104)",       190.00,  -11.62,   2.93e7,     9.0,  0.0, 84.0, -999.0,   0.0,    1.0f, {0.93f,0.88f,0.78f}, GAL_SPIRAL    , 0.0,    0.0f, 0.0f },
    { "Virgo A (M87)",         187.71,   12.39,   5.30e7,     8.0,  0.0,  0.0, -999.0,   0.0,    1.0f, {0.92f,0.88f,0.80f}, GAL_ELLIPTICAL, 6.5e9,  1.0f, 1.0f },
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

/* Live galaxy set: the catalogue rows first, then procedurally generated
 * galaxies rebuilt around the camera. galaxy_count() reports the whole set, so
 * every consumer -- crucially galaxy_render_stars(), which already takes its
 * position/radius/axis/seed/type per galaxy as uniforms -- picks up procedural
 * galaxies with no further work. */
static GalaxyInst *s_gal = NULL;
static int         s_gal_n = 0, s_gal_cap = 0;
static double      s_gal_built_cam[3] = { 1e300, 0, 0 };

/* ── distant-galaxy impostors ─────────────────────────────────────────────
 *
 * A galaxy stops being worth raymarching long before it stops being worth
 * seeing. A 50 kly disc subtends 0.9 px at 50 Mly and 0.02 px at 2 Gly, so the
 * volumetric pass correctly skips almost every procedural galaxy -- which left
 * the deep sky empty even though thousands were being generated.
 *
 * Below the volumetric threshold a galaxy becomes a point sprite instead,
 * reusing star_dot.vert + cluster.frag: exactly the soft additive glow that
 * already stands in for unresolved star clumps. Flux goes as the solid angle
 * (surface brightness is roughly scale-free across galaxies), so apparent
 * brightness ~ angular_radius^2 -- which falls off as 1/d^2 for free. */
static GLuint s_imp_shader = 0, s_imp_vao = 0, s_imp_vbo = 0;
static GLint  s_imp_vp = -1, s_imp_time = -1, s_imp_twinkle = -1;
static float *s_imp_buf = NULL;
static int    s_imp_n = 0, s_imp_cap = 0;

/* ── procedural galaxy lattice ────────────────────────────────────────────
 *
 * Beyond the catalogue the universe was empty: eleven hand-listed rows and
 * then nothing, so flying past M87 left you in a void. Galaxies are now
 * generated on a cubic lattice around the camera, the same construction the
 * star cascade uses one scale down.
 *
 * The lattice is deliberately NOT the cosmic_field cell hash: that packs 21
 * bits per axis at 1 ly per cell, so it tops out at +-1 Mly -- Andromeda at
 * 2.54 Mly is already outside it. At GAL_CELL_MLY per cell the same bit
 * budget reaches far past the observable universe. */
/* A CASCADE of lattices, not one. Within 160 Mly there are ~10^4 galaxies;
 * within 5 Gly there are ~3x10^8. Enumerating the far ones is impossible and
 * pointless -- but generating none of them is why the deep sky read as empty,
 * because that is where almost every galaxy you can see actually is.
 *
 * Each level covers a shell the next-finer level does not, with cells ~5x
 * larger, and emits only the LARGEST galaxies of that volume (its cell holds
 * far more than the few candidates it can produce). Same construction
 * galaxy_stars.vert uses for stars one scale down. */
#define GAL_CASCADES    5
#define GAL_SPAN_CELLS  11       /* cells each way per level                 */
#define GAL_PER_CELL    2        /* candidates per cell                      */
#define GAL_PROC_MAX    8192     /* total kept, largest angular size         */
static const double GAL_CELL_MLY[GAL_CASCADES] = { 12.0, 55.0, 260.0, 1200.0, 5500.0 };
#define GAL_REBUILD_MLY 4.0      /* camera travel that forces a rebuild      */
#define GAL_IMPOSTOR_PX  1.5f    /* below this projected radius, draw a point */
/* Brightness: the SAME magnitude law the stars use, not a separate invented
 * one. A galaxy gets an absolute magnitude from its size, an apparent
 * magnitude from its distance, and then star_field.vert's size curve and the
 * two-tier alpha. Visibility then falls out of the physics at every vantage --
 * a 2 Gly galaxy is correctly invisible from Earth AND correctly bright when
 * you are beside it -- with no distance-dependent exposure hack.
 *
 * M = GAL_M_REF - 5*log10(r / GAL_R_REF): luminosity goes as r^2, and a
 * 50 kly disc is about M_V -20.9 (the Milky Way). */
#define GAL_M_REF   (-20.9f)
#define GAL_R_REF   (5.0e4f)     /* light-years */
#define GAL_IMPOSTOR_MINA 0.01f     /* identical floor to galaxy_stars.vert */
#define GAL_IMPOSTOR_MINPX 4.0f     /* extended source: never a bare point   */

static double gh_hash(long x, long y, long z, int k)
{
    uint64_t h = (uint64_t)(x * 73856093L) ^ (uint64_t)(y * 19349663L)
               ^ (uint64_t)(z * 83492791L) ^ (uint64_t)(k * 2654435761u);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (double)(h >> 11) / 9007199254740992.0;      /* [0,1) */
}

/* Smooth value noise over the lattice, for large-scale structure. Real
 * galaxies sit in filaments and walls around voids rather than uniformly, and
 * a uniform scatter reads as obviously synthetic when you fly through it. */
static double gh_vnoise(double x, double y, double z)
{
    long ix = (long)floor(x), iy = (long)floor(y), iz = (long)floor(z);
    double fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx*fx*(3.0-2.0*fx); fy = fy*fy*(3.0-2.0*fy); fz = fz*fz*(3.0-2.0*fz);
    double c[8];
    for (int b = 0; b < 8; b++)
        c[b] = gh_hash(ix + (b&1), iy + ((b>>1)&1), iz + ((b>>2)&1), 7);
    double x00 = c[0] + (c[1]-c[0])*fx, x10 = c[2] + (c[3]-c[2])*fx;
    double x01 = c[4] + (c[5]-c[4])*fx, x11 = c[6] + (c[7]-c[6])*fx;
    double y0 = x00 + (x10-x00)*fy, y1 = x01 + (x11-x01)*fy;
    return y0 + (y1-y0)*fz;
}

/* Cosmic-web occupancy at a lattice point, 0 in voids .. 1 in filaments. */
static double gh_structure(double cx, double cy, double cz)
{
    double n = gh_vnoise(cx * 0.16, cy * 0.16, cz * 0.16) * 0.6
             + gh_vnoise(cx * 0.41, cy * 0.41, cz * 0.41) * 0.3
             + gh_vnoise(cx * 0.93, cy * 0.93, cz * 0.93) * 0.1;
    /* Sharpen into walls/filaments with wide voids between. */
    double t = (n - 0.42) / 0.30;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

static GLuint s_shader = 0, s_vao = 0, s_vbo = 0, s_ebo = 0;
static GLuint s_star_shader = 0, s_star_vao = 0;
static GLint  s_su_vp, s_su_cell_base, s_su_origin_rel, s_su_cell_size;
static GLint  s_su_grid_dim, s_su_inner, s_su_outer, s_su_cam_in_gal;
static GLint  s_su_radius, s_su_axis, s_su_seed, s_su_type, s_su_time;
static GLint  s_su_gain, s_su_suppress, s_su_n_suppress;
static GLint  s_su_lf_mag, s_su_mag_limit, s_su_cam_abs, s_su_q_max;

/* Inverse CDF of the measured luminosity function: s_lf_mag[i] is the
 * absolute magnitude at quantile i/(LF_TABLE-1), bright end first. Built once
 * so the shader samples the real stellar population with one lerp. */
#define LF_TABLE 32
/* The table is indexed by LOG10 quantile, not quantile. A coarse cascade cuts
 * at q_max ~ 1e-7 (its cell holds millions of stars but emits five), so a
 * table linear in q collapses every coarse star onto the brightest bin -- a
 * sky of M = -10 hypergiants. Log spacing keeps resolution across the whole
 * bright tail. */
#define LF_LOGQ_MIN (-9.0)
static float s_lf_mag[LF_TABLE];

static void build_lf_table(void)
{
    double cum[STELLAR_LF_BINS + 1];
    cum[0] = 0.0;
    for (int i = 0; i < STELLAR_LF_BINS; i++)
        cum[i + 1] = cum[i] + (double)STELLAR_LF[i];
    double total = cum[STELLAR_LF_BINS];
    if (total <= 0.0) {                       /* degenerate table: flat fallback */
        for (int i = 0; i < LF_TABLE; i++) s_lf_mag[i] = 5.0f;
        return;
    }
    for (int k = 0; k < LF_TABLE; k++) {
        double lq = LF_LOGQ_MIN
                  + (double)k / (double)(LF_TABLE - 1) * (-LF_LOGQ_MIN);
        double q = pow(10.0, lq) * total;
        int b = 0;
        while (b < STELLAR_LF_BINS - 1 && cum[b + 1] < q) b++;
        double span = cum[b + 1] - cum[b];
        double f = span > 0.0 ? (q - cum[b]) / span : 0.0;
        s_lf_mag[k] = (float)(STELLAR_LF_M_MIN + b + f);
    }
}
static GLint  s_u_vp, s_u_center, s_u_radius, s_u_right, s_u_up, s_u_fwd;
static GLint  s_u_oc, s_u_color, s_u_density, s_u_seed, s_u_bill, s_u_fullscreen;
static GLint  s_u_fov_tan, s_u_aspect, s_u_screen, s_u_steps, s_u_type;
static GLint  s_u_axis, s_u_time, s_u_scene_depth, s_u_use_scene_depth;
static int    s_enabled = 1;

/* J2000 equatorial RA/Dec -> ecliptic GL unit vector (matches starfield.c). */
static void equatorial_to_gl(double ra_deg, double dec_deg, double out[3])
{
    const double deg = PI / 180.0;
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
    s_u_scene_depth     = glGetUniformLocation(s_shader, "u_scene_depth");
    s_u_use_scene_depth = glGetUniformLocation(s_shader, "u_use_scene_depth");

    s_imp_shader = gl_shader_load("assets/shaders/star_dot.vert",
                                  "assets/shaders/cluster.frag");
    if (s_imp_shader) {
        s_imp_vp      = glGetUniformLocation(s_imp_shader, "u_vp");
        s_imp_time    = glGetUniformLocation(s_imp_shader, "u_time");
        s_imp_twinkle = glGetUniformLocation(s_imp_shader, "u_twinkle");
        s_imp_cap = GAL_PROC_MAX;
        s_imp_buf = (float *)malloc((size_t)s_imp_cap * 8 * sizeof(float));
        s_imp_vao = gl_vao_create();
        s_imp_vbo = gl_vbo_create((GLsizeiptr)s_imp_cap * 8 * sizeof(float),
                                  NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float),
                              (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float),
                              (void*)(7*sizeof(float)));
        glBindVertexArray(0);
    }

    const double arcmin = (PI / 180.0) / 60.0;
    s_gal_cap = GALAXY_COUNT + GAL_PROC_MAX;
    s_gal = (GalaxyInst *)calloc((size_t)s_gal_cap, sizeof(GalaxyInst));
    if (!s_gal) { fprintf(stderr, "[Galaxy] alloc failed\n"); return; }
    s_gal_n = GALAXY_COUNT;
    for (int i = 0; i < GALAXY_COUNT; i++) {   /* catalogue rows only */
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
            double th = GALAXIES[i].incl_deg * PI / 180.0;
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
        s_su_lf_mag     = glGetUniformLocation(s_star_shader, "u_lf_mag");
        s_su_mag_limit  = glGetUniformLocation(s_star_shader, "u_mag_limit");
        s_su_cam_abs    = glGetUniformLocation(s_star_shader, "u_cam_abs");
        s_su_q_max      = glGetUniformLocation(s_star_shader, "u_q_max");
        build_lf_table();
        s_su_suppress   = glGetUniformLocation(s_star_shader, "u_suppress");
        s_su_n_suppress = glGetUniformLocation(s_star_shader, "u_n_suppress");
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
                   float time_s, unsigned int scene_depth_tex)
{
    if (!s_enabled || !s_shader || !s_vao) return;

    glUseProgram(s_shader);
    render_star_veil_uniforms(s_shader);
    if (scene_depth_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
        glUniform1i(s_u_scene_depth, 0);
    }
    glUniform1f(s_u_use_scene_depth, scene_depth_tex ? 1.0f : 0.0f);
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
    s_imp_n = 0;
    for (int i = 0; i < s_gal_n; i++) {
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

        /* See nebula.c: film-out buys extra march steps (docs/CINEMATIC.md §8.4). */
        int base_steps = (int)(s_base_steps * cinematic_quality_scale());
        int steps = base_steps;
        if (!fullscreen) {
            float eye_z = center[0]*cam_fwd[0] + center[1]*cam_fwd[1]
                        + center[2]*cam_fwd[2];
            if (eye_z <= 0.0f) continue;                 /* behind camera */
            float proj_px = radf / eye_z / fov_tan * half_h;
            if (proj_px < GAL_IMPOSTOR_PX) {
                /* Too small to raymarch: emit a point sprite instead of
                 * dropping it, or the deep sky reads as empty. */
                if (s_imp_buf && s_imp_n < s_imp_cap) {
                    float r_ly  = (float)(s_gal[i].radius / AU_PER_LY);
                    float absmag = GAL_M_REF - 5.0f * log10f(r_ly / GAL_R_REF);
                    float d_pc   = (float)(dist / (AU_PER_LY * 3.261563));
                    if (d_pc < 1e-6f) d_pc = 1e-6f;
                    float m = absmag + 5.0f * log10f(d_pc) - 5.0f;
                    /* star_field.vert's curves, verbatim. */
                    float a = powf(10.0f, -0.4f * (m - 9.0f));
                    if (a > 1.0f) a = 1.0f;
                    if (a > GAL_IMPOSTOR_MINA) {
                        /* A galaxy is an EXTENDED source, not a point. Giving
                         * it the stars' 1.4 px floor is what made the deep
                         * field read as a thin starfield: the objects were
                         * there and correctly bright, they just looked like
                         * stars. A resolved fuzzy patch is both truer and what
                         * makes the sky legible as galaxies. */
                        float sz = 7.0f - 0.45f * (m + 1.0f);
                        if (sz < GAL_IMPOSTOR_MINPX) sz = GAL_IMPOSTOR_MINPX;
                        if (proj_px * 2.4f > sz) sz = proj_px * 2.4f;
                        if (sz > 40.0f) sz = 40.0f;
                        float *o = &s_imp_buf[s_imp_n * 8];
                        o[0]=center[0]; o[1]=center[1]; o[2]=center[2];
                        o[3]=s_gal[i].col[0]; o[4]=s_gal[i].col[1];
                        o[5]=s_gal[i].col[2]; o[6]=a; o[7]=sz;
                        s_imp_n++;
                    }
                }
                continue;
            }
            float f = proj_px / (half_h * 0.5f);
            if (f > 1.0f) f = 1.0f;
            steps = (int)(base_steps * f);
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

    { static int f=0; if(f++==3) fprintf(stderr,"[GN] impostors=%d set=%d\n", s_imp_n, s_gal_n); }
    /* Everything the loop above was too small to march is left in s_imp_buf
     * for galaxy_render_impostors(), which the caller draws separately. */
}

void galaxy_render_stars(const float vp_camrel[16], const double cam_pos[3],
                         float gain, float time_s)
{
    if (!s_enabled || !s_stars_enabled || !s_star_shader || !s_star_vao ||
        gain <= 0.002f) return;

    glUseProgram(s_star_shader);
    glUniformMatrix4fv(s_su_vp, 1, GL_FALSE, vp_camrel);
    glUniform1i(s_su_grid_dim, GS_GRID_DIM);
    glUniform1f(s_su_time, time_s);
    glUniform1f(s_su_gain, gain);
    glUniform1fv(s_su_lf_mag, LF_TABLE, s_lf_mag);
    glUniform1f(s_su_mag_limit, CATALOG_MAG_LIMIT);
    /* The selection function is evaluated from the Sun (the survey's vantage),
     * so the shader needs the camera's absolute position to recover each
     * candidate's heliocentric distance. Float is ample: a ~100 AU rounding
     * error at galactic-centre range shifts a distance modulus by ~1e-7 mag. */
    glUniform3f(s_su_cam_abs, (float)cam_pos[0], (float)cam_pos[1],
                              (float)cam_pos[2]);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);        /* additive light over the glow  */
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);            /* planets still occlude a star  */
    glBindVertexArray(s_star_vao);

    const int n_points = GS_GRID_DIM * GS_GRID_DIM * GS_GRID_DIM * GS_PER_CELL;

    for (int i = 0; i < s_gal_n; i++) {
        /* Camera in this galaxy's frame (double: centres are up to 3e12 AU
         * out; all camera-relative floats below stay small). */
        double gx = cam_pos[0] - s_gal[i].pos[0];
        double gy = cam_pos[1] - s_gal[i].pos[1];
        double gz = cam_pos[2] - s_gal[i].pos[2];
        double dist = sqrt(gx*gx + gy*gy + gz*gz);
        if (dist > GS_ENTER_FRAC * s_gal[i].radius) continue;

        /* Hide sprites of stars currently promoted to real bodies. */
        {
            int sup[8][4];
            int nsup = starsys_suppressed(i, sup, 8);
            glUniform1i(s_su_n_suppress, nsup);
            if (nsup > 0)
                glUniform4iv(s_su_suppress, nsup, &sup[0][0]);
        }

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
                /* Quantile cut for this cascade. A cell of edge c holds about
                 * STELLAR_DENSITY_LY3 * c^3 stars but can emit only
                 * GS_PER_CELL candidates, so draw from the brightest
                 * q_max fraction of the luminosity function. The emitted
                 * stars then sit at true space density instead of being
                 * luminosity-boosted stand-ins. */
                double c3 = GS_CELL_LY[k] * GS_CELL_LY[k] * GS_CELL_LY[k];
                double in_cell = (double)STELLAR_DENSITY_LY3 * c3;
                double q_max = in_cell > 0.0
                             ? (double)GS_PER_CELL / in_cell : 1.0;
                if (q_max > 1.0) q_max = 1.0;
                glUniform1f(s_su_q_max, (float)q_max);
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

/* Draw the galaxies the volumetric pass was too small to march, as additive
 * point sprites (see galaxy.h). s_imp_buf was filled by the last
 * galaxy_render(), with sizes in that call's target pixels. */
void galaxy_render_impostors(const float vp_camrel[16], float time_s,
                             float px_scale)
{
    if (!s_enabled || !s_imp_shader || !s_imp_vao || s_imp_n <= 0) return;

    /* Rescale sizes in place: galaxy_render() rebuilds the buffer each call,
     * and each build is drawn once. */
    if (px_scale != 1.0f)
        for (int i = 0; i < s_imp_n; i++) s_imp_buf[i * 8 + 7] *= px_scale;

    glBindVertexArray(s_imp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_imp_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)s_imp_n * 8 * sizeof(float), s_imp_buf);

    glUseProgram(s_imp_shader);
    glUniformMatrix4fv(s_imp_vp, 1, GL_FALSE, vp_camrel);
    glUniform1f(s_imp_time, time_s);
    glUniform1f(s_imp_twinkle, 0.0f);      /* galaxies do not scintillate */

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);           /* additive, like the glow it replaces */
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDrawArrays(GL_POINTS, 0, s_imp_n);
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

void galaxy_get_params(int *enabled, float *density, int *steps,
                       int *stars_enabled)
{
    if (enabled)       *enabled       = s_enabled;
    if (density)       *density       = s_density;
    if (steps)         *steps         = s_base_steps;
    if (stars_enabled) *stars_enabled = s_stars_enabled;
}

void galaxy_set_params(int enabled, float density, int steps,
                       int stars_enabled)
{
    s_enabled = enabled ? 1 : 0;
    s_density = density < 0.0f ? 0.0f : density;
    s_base_steps = steps < 4 ? 4 : (steps > 64 ? 64 : steps);
    s_stars_enabled = stars_enabled ? 1 : 0;
}

/* Regenerate the procedural galaxies around `cam`. Cheap and idempotent: it
 * returns immediately unless the camera has moved GAL_REBUILD_MLY since the
 * last build, so this can be called every frame.
 *
 * Candidates are scored by ANGULAR size and only the largest GAL_PROC_MAX are
 * kept. That is the LOD: each galaxy is a raymarched volume, so instantiating
 * every one within the search radius would be thousands of them. Keeping the
 * biggest on screen is both the cheapest and the most faithful cut -- the ones
 * dropped are the ones you could not resolve anyway. A proper cascade (coarser
 * lattices contributing only their brightest members, as galaxy_stars.vert
 * already does for stars) is the next step and is what lets the search radius
 * grow without the count growing with it. */
void galaxy_proc_update(const double cam[3])
{
    if (!s_gal || s_gal_cap <= GALAXY_COUNT) return;
    double mv = 0.0;
    for (int k = 0; k < 3; k++) {
        double d = cam[k] - s_gal_built_cam[k];
        mv += d * d;
    }
    const double reb_au = GAL_REBUILD_MLY * 1e6 * AU_PER_LY;
    if (mv < reb_au * reb_au) return;
    for (int k = 0; k < 3; k++) s_gal_built_cam[k] = cam[k];

    /* Insertion sort into a fixed top-N by angular radius. */
    /* static: GAL_PROC_MAX entries is large, too much for the stack */
    static struct { double ang, pos[3], radius; int type; float seed, z; } best[GAL_PROC_MAX];
    int nb = 0;

    /* Budget PER LEVEL, not globally. A single top-N by angular size is always
     * won by the nearest shell -- coarse levels produce genuinely smaller
     * angular sizes, so they lose every comparison and the deep sky stays
     * empty however many are generated. Each shell keeps its own share. */
    const int lvl_budget = GAL_PROC_MAX / GAL_CASCADES;

    double inner_au = 0.0;
    for (int lv = 0; lv < GAL_CASCADES; lv++) {
        const int lv_base = nb;
        const int lv_cap  = lv_base + lvl_budget;
        const double cell_au  = GAL_CELL_MLY[lv] * 1e6 * AU_PER_LY;
        const double outer_au = cell_au * GAL_SPAN_CELLS;
        const int    span     = GAL_SPAN_CELLS;
        long bx = (long)floor(cam[0] / cell_au);
        long by = (long)floor(cam[1] / cell_au);
        long bz = (long)floor(cam[2] / cell_au);

        /* Coarse levels stand in for volumes holding orders of magnitude more
         * galaxies than they can emit, so they draw only from the big end of
         * the size distribution -- the ones that would actually be visible. */
        double t_lo = (double)lv / (double)GAL_CASCADES;

        for (long dz = -span; dz <= span; dz++)
        for (long dy = -span; dy <= span; dy++)
        for (long dx = -span; dx <= span; dx++) {
            long cx = bx + dx, cy = by + dy, cz = bz + dz;
            double occ = gh_structure((double)cx * (GAL_CELL_MLY[lv] / 12.0),
                                      (double)cy * (GAL_CELL_MLY[lv] / 12.0),
                                      (double)cz * (GAL_CELL_MLY[lv] / 12.0));
            if (occ <= 0.001) continue;                  /* void */
            for (int k = 0; k < GAL_PER_CELL; k++) {
                int kk = k * 31 + lv * 7919;
                if (gh_hash(cx, cy, cz, kk + 1) > occ) continue;
                double px = ((double)cx + gh_hash(cx, cy, cz, kk+2)) * cell_au;
                double py = ((double)cy + gh_hash(cx, cy, cz, kk+3)) * cell_au;
                double pz = ((double)cz + gh_hash(cx, cy, cz, kk+4)) * cell_au;
                double rx = px - cam[0], ry = py - cam[1], rz = pz - cam[2];
                double d2 = rx*rx + ry*ry + rz*rz;
                double dist = sqrt(d2);
                if (dist < inner_au || dist > outer_au) continue;  /* this shell only */
                if (dist < 1.0) dist = 1.0;

                double t = t_lo + (1.0 - t_lo) * gh_hash(cx, cy, cz, kk+5);
                double r_ly = 3.0e3 * pow(8.0e4 / 3.0e3, t);
                double r_au = r_ly * AU_PER_LY;
                double ang  = r_au / dist;               /* angular radius, rad */

                if (nb == lv_cap && ang <= best[nb-1].ang) continue;
                int at = (nb < lv_cap) ? nb : lv_cap - 1;
                while (at > lv_base && best[at-1].ang < ang) { best[at] = best[at-1]; at--; }
                best[at].ang = ang;
                best[at].pos[0]=px; best[at].pos[1]=py; best[at].pos[2]=pz;
                best[at].radius = r_au;
                double ht = gh_hash(cx, cy, cz, kk+6);
                best[at].type = ht < 0.60 ? GAL_SPIRAL
                              : (ht < 0.85 ? GAL_ELLIPTICAL : GAL_IRREGULAR);
                best[at].seed = (float)(gh_hash(cx, cy, cz, kk+7) * 977.0);
                /* Cosmological redshift: z ~ d / (c/H0), Hubble distance
                 * ~14.4 Gly. This is what makes the deep field warm -- distant
                 * galaxies really are redder, and it is the colour gradient a
                 * flat tint cannot produce. */
                best[at].z = (float)(dist / (AU_PER_LY * 14.4e9));
                if (nb < lv_cap) nb++;
            }
        }
        inner_au = outer_au;
    }

    s_gal_n = GALAXY_COUNT;
    for (int i = 0; i < nb; i++) {
        GalaxyInst *g = &s_gal[s_gal_n++];
        for (int k = 0; k < 3; k++) g->pos[k] = best[i].pos[k];
        g->radius = best[i].radius;
        g->type   = best[i].type;
        g->seed   = best[i].seed;
        g->brightness = 1.0f;
        /* Population tint by morphology: ellipticals old and red, spirals
         * mixed, irregulars blue with young star formation. */
        if (best[i].type == GAL_ELLIPTICAL) {
            g->col[0]=1.00f; g->col[1]=0.80f; g->col[2]=0.60f;   /* old, red   */
        } else if (best[i].type == GAL_IRREGULAR) {
            g->col[0]=0.70f; g->col[1]=0.82f; g->col[2]=1.00f;   /* young, blue*/
        } else {
            g->col[0]=0.92f; g->col[1]=0.90f; g->col[2]=0.88f;   /* mixed      */
        }
        /* Redden with redshift: boost red, suppress blue. */
        float zz = best[i].z; if (zz > 2.0f) zz = 2.0f;
        float warm = zz / (1.0f + zz);
        g->col[0] = g->col[0] * (1.0f - warm) + 1.00f * warm;
        g->col[1] = g->col[1] * (1.0f - warm) + 0.62f * warm;
        g->col[2] = g->col[2] * (1.0f - warm) + 0.34f * warm;
        /* Orientation: a unit axis from the same seed, so it is stable. */
        double u = best[i].seed * 0.01731, v = best[i].seed * 0.00977;
        double sz = cos(u), sr = sqrt(1.0 - sz*sz);
        g->axis[0] = (float)(sr * cos(v * 6.2831853));
        g->axis[1] = (float)(sr * sin(v * 6.2831853));
        g->axis[2] = (float)sz;
    }
}

int         galaxy_count(void)       { return s_gal_n; }
const char *galaxy_name(int i)
{
    return (i >= 0 && i < GALAXY_COUNT) ? GALAXIES[i].name : "";
}

void galaxy_position(int i, double out[3])
{
    if (i < 0 || i >= s_gal_n) { out[0] = out[1] = out[2] = 0.0; return; }
    out[0] = s_gal[i].pos[0];
    out[1] = s_gal[i].pos[1];
    out[2] = s_gal[i].pos[2];
}

double galaxy_radius_au(int i)
{
    return (i >= 0 && i < s_gal_n) ? s_gal[i].radius : 0.0;
}

void galaxy_color(int i, float out[3])
{
    if (i < 0 || i >= s_gal_n) { out[0] = out[1] = out[2] = 1.0f; return; }
    out[0] = s_gal[i].col[0];
    out[1] = s_gal[i].col[1];
    out[2] = s_gal[i].col[2];
}

int galaxy_type(int i)
{
    return (i >= 0 && i < s_gal_n) ? s_gal[i].type : 0;
}

float galaxy_seed(int i)
{
    return (i >= 0 && i < s_gal_n) ? s_gal[i].seed : 0.0f;
}

void galaxy_axis(int i, float out[3])
{
    if (i < 0 || i >= s_gal_n) { out[0] = 0; out[1] = 1; out[2] = 0; return; }
    out[0] = s_gal[i].axis[0];
    out[1] = s_gal[i].axis[1];
    out[2] = s_gal[i].axis[2];
}

int galaxy_agn(int i, double *mass_kg, float *activity, float *torus)
{
    if (i < 0 || i >= GALAXY_COUNT || GALAXIES[i].bh_mass_msun <= 0.0) return 0;
    if (mass_kg)  *mass_kg  = GALAXIES[i].bh_mass_msun * MSUN;
    if (activity) *activity = GALAXIES[i].agn_activity;
    if (torus)    *torus    = GALAXIES[i].agn_torus;
    return 1;
}

/* Spawn a black-hole Body at each AGN host galaxy's centre. The four AGN passes
 * in render_frame (bh/jet/torus/agncore) then draw the full nucleus for free —
 * this is a pure data-level composition of the finished engine, with no changes
 * to the render passes. Bodies sit at real galaxy distances (Mly), so they are
 * culled past farfield_horizon_au until the camera flies to the nucleus. */
void galaxy_spawn_agn(void)
{
    if (!g_settings.galaxy_agn) return;

    for (int i = 0; i < s_gal_n; i++) {
        double mass_kg; float activity, torus;
        if (!galaxy_agn(i, &mass_kg, &activity, &torus)) continue;

        double pos_au[3];
        galaxy_position(i, pos_au);

        char nm[48];
        snprintf(nm, sizeof(nm), "%.24s Nucleus", galaxy_name(i));

        BodyCreateSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.name   = nm;
        spec.mass   = mass_kg;
        spec.radius = laws_schwarzschild_radius(mass_kg);
        spec.pos[0] = pos_au[0] * AU;   /* AU → metres: Body state is SI */
        spec.pos[1] = pos_au[1] * AU;
        spec.pos[2] = pos_au[2] * AU;
        spec.is_star = 1;               /* a hole is always a system root */
        spec.parent  = -1;
        spec.obliquity = 35.0;          /* tilt so both jet lobes frame nicely */
        /* Spin drives the Blandford–Znajek jets: active hosts need a real SMBH
         * horizon period (~minutes) so accretion_init_body clamps a* near the
         * Thorne limit. Quiescent nuclei can turn slowly (no jet regardless). */
        spec.rotation_rate = (activity > 0.0f)
                           ? (2.0 * PI) / (0.003 * DAY)
                           : (2.0 * PI) / DAY;
        spec.col[0] = 1.00f; spec.col[1] = 0.86f; spec.col[2] = 0.72f;

        int idx = universe_add_body(&spec);
        if (idx < 0) continue;

        Body *b = &g_bodies[idx];
        b->is_black_hole  = 1;
        body_bh_mut(b)->agn_activity   = activity;
        body_bh_mut(b)->accretion_disk = 1.0f;       /* even quiescent holes get a faint disk */
        body_bh_mut(b)->dust_torus     = torus;
        b->radius         = laws_schwarzschild_radius(b->mass);
        accretion_init_body(b);         /* seed spin_a + gas reservoir */

        /* Active hosts: stretch the (physically tiny) jet into a galaxy-scale kpc
         * beam so it reads against the host from a distance, and aim it along the
         * galaxy's disc axis. The disk/torus stay Rs-sized (a compact bright
         * nucleus). Target: one galaxy radius per lobe, independent of
         * BH mass; a scale of 1 (quiescent hosts) leaves the jet physical. */
        if (activity > 0.0f) {
            double rs_au    = b->radius / AU;               /* Rs in AU           */
            double spin     = fabs(body_bh(b)->spin_a);
            double base_len = rs_au * (12.0 + 46.0 * spin); /* physical jet (AU)  */
            double target   = 1.0 * galaxy_radius_au(i);    /* per-lobe reach     */
            if (base_len > 0.0)
                body_bh_mut(b)->agn_visual_scale = (float)(target / base_len);
            galaxy_axis(i, body_bh_mut(b)->agn_axis);                /* disc axis = jet axis */
        }
    }
}
