/*
 * render.c — scene renderer
 *
 * Pipeline each frame:
 *   1. Starfield        — rotation-only VP, GL_POINTS (skybox)
 *   2. Body spheres     — Phong-lit billboard quads, depth-written
 *   2.5. Atmosphere glow — additive billboard pass (GL_SRC_ALPHA / GL_ONE)
 *   3. Collision particles — additive GL_POINTS (impact ejecta)
 *   3. Center dots      — GL_POINTS (for sub-pixel or occluded bodies)
 *   4. Rings + belts    — rings_render() / asteroids_render()
 *   5. Trails           — trails_render()
 *   6. Star glare       — additive billboard (GL_ONE / GL_ONE), drawn after
 *                         trails so orbit lines vanish inside the glow
 *   6.5. Build preview  — ghost dot + guide lines + distance labels
 *   7. Labels           — labels_render()
 *
 * ── Camera-relative rendering ────────────────────────────────────────────
 *
 * All vertex positions are passed to the GPU as (world_pos − cam_pos), i.e.,
 * relative to the camera.  The shader receives vp_camrel = proj × view_rot
 * (no translation column) instead of the full proj × view matrix.
 *
 * Reason: body positions are stored in double precision (metres), but GL
 * uniforms and vertex attributes are float32.  At interstellar distances
 * (4 ly ≈ 250,000 AU) the world-space offset is too large to represent in
 * float32 without losing the last 5 significant digits.  Subtracting the
 * camera position in double before casting to float keeps the relative
 * coordinates small (≤ planetary-system scale) and fully precise.
 *
 * ── Billboard sphere pattern ─────────────────────────────────────────────
 *
 * All body spheres share a single unit quad (UV 0..1, two triangles).
 * phong.vert receives the quad corner UVs and expands the billboard to the
 * correct size in clip space using the camera right/up basis vectors and the
 * projected sphere radius.  No per-body vertex buffer needed.
 *
 * ── Dot–sphere transition (continuous LOD) ──────────────────────────────
 *
 * Each body crossfades between a dot (GL_POINT) and a sphere (billboard) over
 * one shared pixel window [BODY_DOT_FADE_START_PX, BODY_DOT_FADE_END_PX]
 * (0.75..1.75 px, scaled by the density LOD factor below): the sphere's
 * opacity is smoothstep(px) and the dot's alpha is its exact complement, so
 * the handoff is a constant-energy blend with no pop.  Star glare does the
 * same against the star dot over [STAR_DOT_FULL_GLARE_PX,
 * STAR_DOT_FADE_START_GLARE_PX].  info[i].show stays a *binary* routing flag
 * (px < BODY_SPHERE_APPEAR_PX) for picking and the far dot pass.
 *
 * Windows are scaled once per frame by s_lod_scale, sampled from the
 * CosmicField at the camera (local density × clumpiness): in dense fields
 * representations resolve later, spreading the detail budget across more
 * bodies — LOD driven by camera distance AND the field (roadmap Phase A #2).
 *
 * eye_z (depth along view axis) is used for px computation instead of
 * Euclidean dcam.  dcam = eye_z / cos(θ) — rotating the camera changes θ
 * and therefore dcam, causing the show flag to flicker.  eye_z is invariant
 * under pure camera rotation, so the transition is stable.
 */
#include "render.h"
#include "body.h"
#include "camera.h"
#include "cosmic_field.h"
#include "radiance_field.h"
#include "starfield.h"
#include "nebula.h"
#include "galaxy.h"
#include "comet.h"
#include "trails.h"
#include "rings.h"
#include "asteroids.h"
#include "labels.h"
#include "build.h"
#include "inspect.h"
#include "collision.h"
#include "gl_utils.h"
#include "math3d.h"
#include "supernova.h"
#include "physics.h"   /* g_sim_time — aurora substorm clock */
#include "universe.h"  /* g_field_star_begin/end, g_universe_generation */
#include "ui_theme.h"
#include "settings.h"
#include "post.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ sphere */

/* Unit quad billboard (UV 0..1), two triangles, shared by all body spheres */
static GLuint s_sphere_shader  = 0;
static GLuint s_sphere_vao     = 0;
static GLuint s_sphere_vbo     = 0;
static GLuint s_sphere_ebo     = 0;

/* Sphere shader uniform locations */
static GLint  s_sp_vp          = -1;
static GLint  s_sp_center      = -1;   /* cam-relative body centre (= -u_oc) */
static GLint  s_sp_radius      = -1;
static GLint  s_sp_cam_right   = -1;
static GLint  s_sp_cam_up      = -1;
static GLint  s_sp_oc          = -1;   /* cam − centre, double-computed float */
static GLint  s_sp_sun_rel     = -1;   /* star − centre, for Phong lighting   */
static GLint  s_sp_sun_col     = -1;   /* primary chromaticity (white = Sol)  */
static GLint  s_sp_sun2_rel    = -1;   /* secondary light − centre            */
static GLint  s_sp_light2      = -1;   /* secondary strength vs primary, 0..1 */
static GLint  s_sp_light2_col  = -1;   /* secondary chromaticity              */
static GLint  s_sp_cam_fwd     = -1;
static GLint  s_sp_fov_tan     = -1;
static GLint  s_sp_aspect      = -1;
static GLint  s_sp_screen      = -1;
static GLint  s_sp_color       = -1;
static GLint  s_sp_emission    = -1;
static GLint  s_sp_ambient     = -1;
static GLint  s_sp_sun_world   = -1;
static GLint  s_sp_rotation      = -1;
static GLint  s_sp_cloud_rotation = -1;
static GLint  s_sp_cloud_amount  = -1;
static GLint  s_sp_ring          = -1;
static GLint  s_sp_ring_pole     = -1;
static GLint  s_sp_ecl_count     = -1;
static GLint  s_sp_ecl           = -1;
static GLint  s_sp_sun_radius    = -1;
static GLint  s_sp_time          = -1;
static GLint  s_sp_obliquity   = -1;
static GLint  s_sp_ptype       = -1;   /* procedural texture variant index */
static GLint  s_sp_star_heat   = -1;
static GLint  s_sp_starspots   = -1;
static GLint  s_sp_impact_count = -1;
static GLint  s_sp_impact_dir   = -1;
static GLint  s_sp_impact_t1    = -1;
static GLint  s_sp_impact_rad   = -1;
static GLint  s_sp_impact_heat  = -1;
static GLint  s_sp_impact_prog  = -1;
static GLint  s_sp_impact_seed  = -1;
static GLint  s_sp_impact_kind  = -1;
static GLint  s_sp_use_fullscreen = -1; /* 1 when billboard would degenerate (camera near body) */
static GLint  s_sp_stretch_dir   = -1;  /* tidal spaghettification axis + factors */
static GLint  s_sp_stretch_along = -1;
static GLint  s_sp_stretch_perp  = -1;
static GLint  s_sp_tidal_glow    = -1;
static GLint  s_sp_opacity       = -1;  /* continuous-LOD dot→sphere fade-in */

/*
 * get_planet_type — map body name to a procedural texture variant index.
 *
 * The lookup is name-based (not index-based) to survive loader order changes
 * and future body additions.  Build-mode bodies use prefix matching on their
 * generated names ("Rocky Planet", "Gas Giant", etc.).
 *
 *   0=rocky(default)  1=Earth  2=Mars  3=Venus  4=Jupiter  5=Saturn
 *   6=ice-giant       7=Io     8=Titan 9=Europa 10=proc-rocky
 *   11=proc-gas       12=proc-ice 13=Uranus 14=airless moon
 */
static int get_planet_type(const char *name)
{
    static const struct { const char *name; int ptype; } tbl[] = {
        { "Mercury", 14 },
        { "Ceres",   14 }, { "Pallas", 14 }, { "Vesta",  14 },
        { "Hygiea",  14 }, { "Interamnia",14 },
        { "Pluto",   14 }, { "Eris",   14 }, { "Makemake",14 },
        { "Haumea",  14 },
        { "Moon",    14 }, { "Phobos", 14 }, { "Deimos", 14 },
        { "Ganymede",14 }, { "Callisto",14 },
        { "Mimas",   14 }, { "Enceladus",14 }, { "Tethys",14 },
        { "Dione",   14 }, { "Rhea",   14 },
        { "Miranda", 14 }, { "Ariel",  14 }, { "Umbriel",14 },
        { "Titania", 14 }, { "Oberon", 14 }, { "Triton", 14 },
        { "Earth",   1 }, { "Mars",    2 }, { "Venus",   3 },
        { "Jupiter", 4 }, { "Saturn",  5 }, { "Uranus", 13 },
        { "Neptune", 6 }, { "Io",      7 }, { "Titan",   8 },
        { "Europa",  9 }, { NULL,      0 }
    };
    int k;
    if (!name) return 0;
    if (strncmp(name, "Rocky Planet", 12) == 0) return 10;
    if (strncmp(name, "Gas Giant",    9)  == 0) return 11;
    if (strncmp(name, "Ice Planet",   10) == 0) return 12;
    if (strncmp(name, "Dwarf Planet", 12) == 0) return 14;
    if (strncmp(name, "Moon",         4) == 0) return 14;
    for (k = 0; tbl[k].name; k++)
        if (strcmp(name, tbl[k].name) == 0) return tbl[k].ptype;
    return 0;
}

/* ------------------------------------------------------------------ atmosphere */

/* Atmosphere glow shader — additive billboard over the sphere */
static GLuint s_atm_shader   = 0;
static GLint  s_at_vp        = -1;
static GLint  s_at_center    = -1;
static GLint  s_at_radius    = -1;
static GLint  s_at_cam_right = -1;
static GLint  s_at_cam_up    = -1;
static GLint  s_at_cam_fwd   = -1;
static GLint  s_at_oc        = -1;
static GLint  s_at_planet_r  = -1;
static GLint  s_at_sun_rel   = -1;
static GLint  s_at_sun_col   = -1;
static GLint  s_at_sun2_rel  = -1;
static GLint  s_at_light2    = -1;
static GLint  s_at_light2_col = -1;
static GLint  s_at_color     = -1;
static GLint  s_at_intensity = -1;
static GLint  s_at_aspect    = -1;
static GLint  s_at_screen    = -1;
static GLint  s_at_aurora    = -1;
static GLint  s_at_aur_shape = -1;
static GLint  s_at_aur_look  = -1;
static GLint  s_at_time      = -1;

/* Atmosphere color/intensity/scale are stored per-body in g_bodies[i],
 * loaded from assets/universe.json by universe_load(). */

/* ------------------------------------------------------------------ dots */

/* Center-dot shader: color.vert / color.frag, same as starfield */
static GLuint s_dot_shader  = 0;
static GLuint s_dot_vao     = 0;
static GLuint s_dot_vbo     = 0;           /* dynamic: updated each frame */
static GLint  s_dot_vp      = -1;
static GLint  s_dot_time    = -1;
static GLint  s_dot_twinkle = -1;

/* Static field-star dots (the bulk Gaia catalog field).  Frozen scenery: the
 * VBO holds absolute positions + baked absolute magnitude, uploaded once per
 * universe load and drawn every frame by star_field.vert with GPU-side
 * camera-relative transform + sizing (see the shader).  Zero per-star CPU. */
static GLuint   s_field_shader     = 0;
static GLuint   s_field_vao        = 0;
static GLuint   s_field_vbo        = 0;
static GLint    s_field_vp         = -1, s_field_cam     = -1;
static GLint    s_field_near       = -1, s_field_horizon = -1;
static GLint    s_field_time       = -1, s_field_twinkle = -1;
static int      s_field_count      = 0;      /* stars in the VBO                 */
static int      s_field_vbo_cap    = 0;      /* stars the VBO can hold           */
static unsigned s_field_generation = 0;      /* g_universe_generation it was built for */

/* Impact ejecta particles — additive GL_POINTS from collision system */
static GLuint s_impact_particle_shader = 0;
static GLint  s_impact_particle_vp = -1;
static GLuint s_impact_particle_vao = 0;
static GLuint s_impact_particle_vbo = 0;

#define RENDER_MAX_COLLISION_PARTICLES 768

/* ------------------------------------------------------------------ star glare */

/* Star glare billboard — additive (GL_ONE, GL_ONE), drawn after trails */
static GLuint s_glare_shader = 0;
static GLint  s_gl_vp        = -1;
static GLint  s_gl_center    = -1;
static GLint  s_gl_radius    = -1;
static GLint  s_gl_right     = -1;
static GLint  s_gl_up        = -1;
static GLint  s_gl_color     = -1;
static GLint  s_gl_spike     = -1;
static GLint  s_gl_corona    = -1;
static GLint  s_gl_time      = -1;
static GLint  s_gl_seed      = -1;
static GLint  s_gl_resolve   = -1;

/* Black-hole billboard — accretion disk + shadow, alpha-blended. */
static GLuint s_bh_shader = 0;
static GLint  s_bh_vp     = -1;
static GLint  s_bh_center = -1;
static GLint  s_bh_radius = -1;
static GLint  s_bh_right  = -1;
static GLint  s_bh_up     = -1;
static GLint  s_bh_color  = -1;
static GLint  s_bh_disk_n = -1;
static GLint  s_bh_time   = -1;
static GLint  s_bh_activity = -1;
static GLint  s_bh_spin     = -1;
static GLint  s_bh_disk     = -1;
static GLint  s_bh_disk_in  = -1;
static GLint  s_bh_disk_temp = -1;
static GLint  s_bh_disk_rate = -1;
static GLint  s_bh_scene     = -1;
static GLint  s_bh_has_scene = -1;

/* AGN relativistic jets — axis-aligned billboard, additive glow. */
static GLuint s_jet_shader   = 0;
static GLint  s_jet_vp       = -1;
static GLint  s_jet_center   = -1;
static GLint  s_jet_axis     = -1;
static GLint  s_jet_len      = -1;
static GLint  s_jet_width    = -1;
static GLint  s_jet_color    = -1;
static GLint  s_jet_time     = -1;
static GLint  s_jet_activity = -1;

/* AGN dust torus — raymarched volumetric doughnut, alpha-over. */
static GLuint s_torus_shader = 0;
static GLint  s_torus_vp     = -1;
static GLint  s_torus_center = -1;
static GLint  s_torus_ext    = -1;
static GLint  s_torus_right  = -1;
static GLint  s_torus_up     = -1;
static GLint  s_torus_rs     = -1;
static GLint  s_torus_normal = -1;
static GLint  s_torus_rmaj   = -1;
static GLint  s_torus_rmin   = -1;
static GLint  s_torus_color  = -1;
static GLint  s_torus_time   = -1;
static GLint  s_torus_rate   = -1;

/* AGN beamed core — camera-facing additive glow (blazar nucleus). */
static GLuint s_agncore_shader = 0;
static GLint  s_agncore_vp     = -1;
static GLint  s_agncore_center = -1;
static GLint  s_agncore_size   = -1;
static GLint  s_agncore_right  = -1;
static GLint  s_agncore_up     = -1;
static GLint  s_agncore_color  = -1;
static GLint  s_agncore_int    = -1;

/* Supernova passes: volumetric cloud and core. */
static GLuint s_supernova_core_shader = 0;
static GLuint s_supernova_cloud_shader = 0;
static GLint  s_sn_core_vp = -1;
static GLint  s_sn_core_center = -1;
static GLint  s_sn_core_radius = -1;
static GLint  s_sn_core_right = -1;
static GLint  s_sn_core_up = -1;
static GLint  s_sn_core_fwd = -1;
static GLint  s_sn_core_oc = -1;
static GLint  s_sn_core_color = -1;
static GLint  s_sn_core_flash = -1;
static GLint  s_sn_core_core = -1;
static GLint  s_sn_core_ratio = -1;
static GLint  s_sn_core_time = -1;
static GLint  s_sn_core_seed = -1;
static GLint  s_sn_core_bill = -1;
static GLint  s_sn_core_fullscreen = -1;
static GLint  s_sn_core_fov_tan = -1;
static GLint  s_sn_core_aspect = -1;
static GLint  s_sn_core_screen = -1;
static GLint  s_sn_cloud_vp = -1;
static GLint  s_sn_cloud_center = -1;
static GLint  s_sn_cloud_radius = -1;
static GLint  s_sn_cloud_right = -1;
static GLint  s_sn_cloud_up = -1;
static GLint  s_sn_cloud_fwd = -1;
static GLint  s_sn_cloud_color = -1;
static GLint  s_sn_cloud_oc = -1;
static GLint  s_sn_cloud_inner = -1;
static GLint  s_sn_cloud_density = -1;
static GLint  s_sn_cloud_hot = -1;
static GLint  s_sn_cloud_time = -1;
static GLint  s_sn_cloud_seed = -1;
static GLint  s_sn_cloud_bill = -1;
static GLint  s_sn_cloud_fullscreen = -1;
static GLint  s_sn_cloud_fov_tan = -1;
static GLint  s_sn_cloud_aspect = -1;
static GLint  s_sn_cloud_screen = -1;

/* Half-resolution offscreen target for the expensive volumetric cloud pass.
 * The supernova cloud raymarch is heavily fragment-bound (16 steps × ~7 FBM
 * each); when the cloud fills the screen it dominates the frame. Render it at
 * half resolution (¼ the fragments) into s_vol_color, then composite it back
 * over the scene — the standard volumetrics optimisation. */
/* Two cached targets: slot 0 at half res (galaxy layer, distant supernovae),
 * slot 1 at quarter res for a supernova pass whose volume surrounds the
 * camera — a fullscreen smoke volume is soft everywhere, so the extra
 * downscale is invisible while cutting the raymarch fragments 4× further. */
static GLuint s_vol_fbo[2]   = { 0, 0 };
static GLuint s_vol_color[2] = { 0, 0 };  /* RGBA16F, premultiplied */
static int    s_vol_w[2] = { 0, 0 }, s_vol_h[2] = { 0, 0 };
static GLuint s_vol_composite_shader = 0;
static GLint  s_vol_comp_tex = -1;
static GLuint s_vol_quad_vao = 0, s_vol_quad_vbo = 0;

/* Glare billboard is STAR_GLARE_BILL_SCALE × the star's visual radius.
 * This constant is also used to compute when the dot fades as the glare grows.
 * NOTE: mirrored by `const float BILL_SCALE` in star_glare.frag — the two must
 * stay synchronized, which is why this one is NOT a live setting. */
static const float STAR_GLARE_BILL_SCALE      = 15.0f;
/* Sphere-vs-dot *routing* threshold (binary info[i].show for picking and the
 * far dot pass); the visual handoff is the crossfade window below. */
static const float BODY_SPHERE_APPEAR_PX      = 1.25f;
/* Crossfade windows + dot-dedup radii are live settings (U menu → Settings →
 * Detail transitions), persisted in settings.json. */
#define BODY_DOT_FADE_START_PX        (g_settings.lod_body_fade_start_px)
#define BODY_DOT_FADE_END_PX          (g_settings.lod_body_fade_end_px)
#define STAR_DOT_FULL_GLARE_PX        (g_settings.lod_glare_full_px)
#define STAR_DOT_FADE_START_GLARE_PX  (g_settings.lod_glare_fade_px)

/* ── continuous LOD density factor (Phase A #2) ──────────────────────────────
 * Sampled once per frame from the CosmicField at the camera.  Scales the
 * dot↔sphere and dot↔glare crossfade windows: in dense/clumped fields the
 * factor rises above 1 so per-body billboards/spheres resolve at a larger
 * projected size — fewer expensive representations contend for the screen at
 * once, and the detail budget follows the field instead of being fixed.
 * 1.0 in empty space; capped so the transition never moves absurdly far. */
static float s_lod_scale = 1.0f;

static void lod_update_density_scale(void)
{
    CosmicSample cs;
    cosmic_field_sample_camera(&cs);
    /* log-compressed density so the factor reacts over orders of magnitude;
     * clumpiness weights it up — a tight cluster contends for the same pixels
     * where uniform scatter spreads out.  The cap is a live setting
     * (g_settings.lod_density_max); 1 disables density-driven scaling. */
    double f = 1.0 + (0.5 + 0.5 * cs.clumpiness) * log10(1.0 + cs.number_density);
    if (f < 1.0) f = 1.0;
    if (f > (double)g_settings.lod_density_max) f = (double)g_settings.lod_density_max;
    s_lod_scale = (float)f;
}

/* ── body lighting via the RadianceField (Phase A #4) ────────────────────────
 * The two brightest light sources at a body, as (emitter − body) in AU floats
 * for the phong/atm u_sun_rel/u_sun2_rel uniforms.  The RadianceField answers
 * "which emitters' flux wins here" — so a binary companion, a nearer foreign
 * sun, or an accreting black hole lights the body when it genuinely outshines
 * the parent chain's root.  w2 is the secondary's flux relative to the primary
 * (0 = no meaningful secondary; shaders are bit-identical to single-sun then),
 * col1/col2 their chromaticities — physical blackbody tints relative to Sol
 * (white for a Sun-like star, so the art-directed sunlight ramp survives; an
 * M dwarf's planets are lit warm orange).  Falls back to the root-star walk
 * when the field has no emitters (starless universe). */
static void body_lights(int i, const Body *b,
                        float sr1[3], float col1[3],
                        float sr2[3], float *w2, float col2[3])
{
    RadianceContrib top[2];
    int n = radiance_field_top(b->pos, i, 2, top);

    col1[0] = col1[1] = col1[2] = 1.0f;
    if (n >= 1) {
        sr1[0] = (float)((top[0].pos[0] - b->pos[0]) * RS);
        sr1[1] = (float)((top[0].pos[1] - b->pos[1]) * RS);
        sr1[2] = (float)((top[0].pos[2] - b->pos[2]) * RS);
        col1[0] = top[0].col[0];
        col1[1] = top[0].col[1];
        col1[2] = top[0].col[2];
    } else {
        int l1 = i;
        while (g_bodies[l1].parent >= 0) l1 = g_bodies[l1].parent;
        sr1[0] = (float)((g_bodies[l1].pos[0] - b->pos[0]) * RS);
        sr1[1] = (float)((g_bodies[l1].pos[1] - b->pos[1]) * RS);
        sr1[2] = (float)((g_bodies[l1].pos[2] - b->pos[2]) * RS);
    }

    *w2 = 0.0f;
    sr2[0] = sr1[0]; sr2[1] = sr1[1]; sr2[2] = sr1[2];
    col2[0] = col2[1] = col2[2] = 1.0f;
    if (n >= 2 && top[0].irr > 0.0) {
        /* Below 2% of the primary a second terminator is invisible — skip the
         * uniform work.  The ratio fades continuously above that, so there is
         * no pop at the threshold. */
        double r = top[1].irr / top[0].irr;
        if (r >= 0.02) {
            *w2 = (float)(r > 1.0 ? 1.0 : r);
            sr2[0] = (float)((top[1].pos[0] - b->pos[0]) * RS);
            sr2[1] = (float)((top[1].pos[1] - b->pos[1]) * RS);
            sr2[2] = (float)((top[1].pos[2] - b->pos[2]) * RS);
            col2[0] = top[1].col[0];
            col2[1] = top[1].col[1];
            col2[2] = top[1].col[2];
        }
    }
}

/* ---------------------------------------------------------------- aurora storms
 * Auroras are not always on: the oval sits faint most of the time and flares
 * when the stellar wind gusts.  Activity is multi-octave value noise over the
 * SIM clock (substorm ~40 min, storm ~hours, sector structure ~day), seeded
 * per emitting star, and evaluated at the wind's ARRIVAL time at the planet
 * (t − dist/450 km/s ≈ 4 days behind at 1 AU) — so one gust sweeps outward
 * through a system, lighting Earth days before Jupiter.  Cubing the noise
 * gives the right diet: mostly quiet oval, occasional bright storms. */
static double aur_hash1(int i, unsigned seed)
{
    unsigned h = (unsigned)i * 374761393u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (double)((h ^ (h >> 16)) & 0xffffffu) / (double)0xffffffu;
}

static double aur_noise1(double x, unsigned seed)
{
    double fl = floor(x), f = x - fl;
    int    i  = (int)fl;
    f = f * f * (3.0 - 2.0 * f);
    return aur_hash1(i, seed) * (1.0 - f) + aur_hash1(i + 1, seed) * f;
}

static double aurora_storm(const RadianceContrib *src, const Body *b)
{
    double dx = b->pos[0] - src->pos[0];
    double dy = b->pos[1] - src->pos[1];
    double dz = b->pos[2] - src->pos[2];
    double tw = g_sim_time - sqrt(dx*dx + dy*dy + dz*dz) / 4.5e5;
    double ts = 3600.0 * (g_settings.aurora_storm_scale > 0.01f
                          ? g_settings.aurora_storm_scale : 0.01f);
    unsigned seed = (unsigned)(src->body + 1) * 2654435761u;
    double n = 0.55 * aur_noise1(tw / (22.0 * ts), seed ^ 0x9e3779b9u)
             + 0.30 * aur_noise1(tw / ( 5.0 * ts), seed ^ 0x85ebca6bu)
             + 0.15 * aur_noise1(tw / (0.66 * ts), seed ^ 0xc2b2ae35u);
    return g_settings.aurora_storm_base
         + g_settings.aurora_storm_amp * n * n * n;
}

/* ------------------------------------------------------------------ build preview */

/* Build-mode overlay: guide lines (3D) and distance labels (2D screen-space) */
static GLuint s_build_line_shader = 0;
static GLuint s_build_line_vao = 0;
static GLuint s_build_line_vbo = 0;
static GLint  s_build_line_vp = -1;

static GLuint s_build_ui_shader = 0;
static GLuint s_build_ui_vao = 0;
static GLuint s_build_ui_vbo = 0;
static GLint  s_build_ui_screen = -1;
static GLint  s_build_ui_color = -1;
static GLint  s_build_ui_use_tex = -1;
static GLint  s_build_ui_tex = -1;
static TTF_Font *s_build_font = NULL;

#define BUILD_UI_FONT_SIZE 16.0f

/*
 * BuildTextCache — GPU texture for one distance label string.
 *
 * The str field is compared before re-rendering; the SDL_TTF texture is
 * recreated only when the string changes, avoiding a per-frame upload.
 */
typedef struct {
    GLuint tex;
    int w, h;
    char str[96];
} BuildTextCache;

static BuildTextCache s_build_dist_text[3];   /* one per guide line (up to 3) */

/* ------------------------------------------------------------------ helpers */

static float half_fov_tan(void) {
    return tanf(FOV * 0.5f * (float)(PI / 180.0));
}

/* Convert an SDL_Surface to a GL_RGBA texture and free the surface.
 * Converts to ABGR8888 first so byte order matches GL_RGBA on all platforms. */
static GLuint build_surface_to_tex(SDL_Surface *surf, int *w, int *h) {
    SDL_Surface *c = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!c) return 0;
    *w = c->w; *h = c->h;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(c);
    return tex;
}

/* Render `str` into tc->tex via SDL_TTF.  No-op if the string didn't change. */
static void build_update_text(BuildTextCache *tc, const char *str) {
    if (!s_build_font || strcmp(tc->str, str) == 0) return;
    snprintf(tc->str, sizeof(tc->str), "%s", str);
    if (tc->tex) {
        glDeleteTextures(1, &tc->tex);
        tc->tex = 0;
    }
    SDL_Color col = {235, 245, 255, 235};
    SDL_Surface *surf = TTF_RenderText_Blended(s_build_font, str, col);
    if (surf) tc->tex = build_surface_to_tex(surf, &tc->w, &tc->h);
}

/* Upload a screen-space quad to the bound GL_ARRAY_BUFFER and draw it.
 * Vertices are (x,y,u,v) pairs; the shader reads these as loc0=xy, loc1=uv. */
static void build_draw_ui_quad(float x, float y, float w, float h) {
    float v[24] = {
        x,   y,   0,0,
        x+w, y,   1,0,
        x+w, y+h, 1,1,
        x,   y,   0,0,
        x+w, y+h, 1,1,
        x,   y+h, 0,1,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Draw a cached text texture at screen position (x, y) at a fixed pixel height. */
static void build_draw_text(BuildTextCache *tc, float x, float y, float h) {
    if (!tc || !tc->tex || !s_build_ui_shader) return;
    float w = h * (float)tc->w / (float)tc->h;  /* preserve texture aspect */
    glUniform1i(s_build_ui_use_tex, 1);
    glUniform4f(s_build_ui_color, 1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, tc->tex);
    build_draw_ui_quad(x, y, w, h);
}

/*
 * draw_ring_2d — inspect-target ring as a screen-space dashed circle.
 *
 * Centre: uses the corrected perspective divisor pz_adj = eye_z − R²/eye_z.
 *   For an off-axis sphere the projected silhouette is an ellipse whose
 *   centre is NOT proj(body_pos) — it shifts toward the screen edge.
 *   Dividing the clip-space x/y by pz_adj instead of eye_z gives the true
 *   visual centre of the sphere disc at any camera angle.
 *
 * Radius: dr × 1.3 projected at Euclidean distance D (angle-stable),
 *   with a 12 px floor so the ring stays visible for any body at any distance.
 */
static void draw_ring_2d(const float rel[3], float dr,
                         float alpha, const float vp[16])
{
    enum { N_DASHES = 8, SEGS = 5 };
    float v[N_DASHES * SEGS * 2 * 4];
    float D, r_px, cx, cy, phase, step;
    float clip_x, clip_y, clip_w, pz_adj;
    int vtx = 0;

    if (!s_build_ui_shader || !s_build_ui_vbo) return;

    D = sqrtf(rel[0]*rel[0] + rel[1]*rel[1] + rel[2]*rel[2]);
    if (D < 1e-6f) return;

    clip_x = vp[0]*rel[0] + vp[4]*rel[1] + vp[8] *rel[2] + vp[12];
    clip_y = vp[1]*rel[0] + vp[5]*rel[1] + vp[9] *rel[2] + vp[13];
    clip_w = vp[3]*rel[0] + vp[7]*rel[1] + vp[11]*rel[2] + vp[15];
    if (clip_w <= 0.0f) return;

    pz_adj = clip_w - dr * dr / clip_w;
    if (pz_adj <= 0.0f) return;

    cx = (clip_x / pz_adj + 1.0f) * 0.5f * (float)WIN_W;
    cy = (float)WIN_H - (clip_y / pz_adj + 1.0f) * 0.5f * (float)WIN_H;

    /* Screen radius using D (Euclidean), not eye_z, so it is angle-stable */
    r_px = dr * 1.3f * (WIN_H * 0.5f) / (D * half_fov_tan());
    if (r_px < 12.0f) r_px = 12.0f;

    phase = (float)SDL_GetTicks() * 0.00055f;
    step  = 2.0f * (float)PI / (float)N_DASHES;

    for (int d = 0; d < N_DASHES; d++) {
        float a0 = phase + (float)d * step;
        float a1 = a0 + step * 0.45f;
        for (int s = 0; s < SEGS; s++) {
            float aa = a0 + (a1 - a0) * (float)s       / (float)SEGS;
            float ab = a0 + (a1 - a0) * (float)(s + 1) / (float)SEGS;
            v[vtx*4+0]=cx+cosf(aa)*r_px; v[vtx*4+1]=cy+sinf(aa)*r_px;
            v[vtx*4+2]=0.0f; v[vtx*4+3]=0.0f; vtx++;
            v[vtx*4+0]=cx+cosf(ab)*r_px; v[vtx*4+1]=cy+sinf(ab)*r_px;
            v[vtx*4+2]=0.0f; v[vtx*4+3]=0.0f; vtx++;
        }
    }

    glUseProgram(s_build_ui_shader);
    glUniform2f(s_build_ui_screen, (float)WIN_W, (float)WIN_H);
    glUniform1i(s_build_ui_use_tex, 0);
    glUniform4f(s_build_ui_color, 1.0f, 1.0f, 1.0f, alpha);
    glBindVertexArray(s_build_ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_build_ui_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vtx * 4 * sizeof(float), v);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINES, 0, vtx);
    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

/* Format a distance in AU into a human-readable string, adapting units. */
static void format_dist_au(double au, char *buf, size_t n) {
    if (au < 0.001)
        snprintf(buf, n, "%.0f km", au * AU / 1000.0);
    else if (au < 1.0)
        snprintf(buf, n, "%.4f AU", au);
    else if (au < 1000.0)
        snprintf(buf, n, "%.2f AU", au);
    else
        snprintf(buf, n, "%.3f ly", au / 63241.0);
}

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double smoothstepd(double edge0, double edge1, double x);

/*
 * supernova_fullscreen_raster_local - choose whether a volumetric supernova
 * pass should rasterize via a fullscreen quad instead of a world-space
 * billboard.
 *
 * The fragment shader already computes the true camera ray from gl_FragCoord
 * and marches against u_oc, so the draw primitive is only there to generate
 * fragments. When the observer is inside or very near the volume, the 3D
 * billboard can expose its own edges as the camera pans. Switching to a
 * fullscreen quad removes those layer-specific clip boundaries entirely.
 */
static int supernova_fullscreen_raster_local(const float center[3],
                                             const float cam_fwd[3],
                                             float coverage_radius,
                                             float bill_scale)
{
    const float edge_overscan = 2.0f; /* keep in sync with supernova_billboard.vert */
    float eye_z = center[0] * cam_fwd[0]
                + center[1] * cam_fwd[1]
                + center[2] * cam_fwd[2];
    float half_extent = coverage_radius * bill_scale * edge_overscan;
    float min_eye_z = fmaxf(half_extent * 1.05f, 0.18f);
    return eye_z < min_eye_z;
}

static int supernova_far_raster_local(const float center[3],
                                      const float cam_fwd[3],
                                      float coverage_radius,
                                      float bill_scale)
{
    const float edge_overscan = 2.0f; /* keep in sync with supernova_billboard.vert */
    const float far_guard = 1850.0f;
    float eye_z = center[0] * cam_fwd[0]
                + center[1] * cam_fwd[1]
                + center[2] * cam_fwd[2];
    float half_extent = coverage_radius * bill_scale * edge_overscan;
    return eye_z + half_extent >= far_guard;
}

/* Far-field horizon fade (true-depth falloff): 1 inside the horizon, ramping to
 * 0 over the last 15% and culled beyond it.  Replaces the old pin-to-shell clamp
 * so distant stars/glare/BHs recede at their real camera distance instead of
 * being pasted onto a fixed ~1500 AU shell.  `dist` is in AU (= dcam), matching
 * g_settings.farfield_horizon_au. */
static float farfield_horizon_fade(float dist)
{
    float h = (float)g_settings.farfield_horizon_au;
    if (h <= 0.0f)     return 1.0f;
    float start = h * 0.85f;
    if (dist <= start) return 1.0f;
    if (dist >= h)     return 0.0f;
    return 1.0f - (float)smoothstepd(start, h, dist);
}

static float supernova_distance_fade_local(float dist, float radius)
{
    float fade_start = 2600.0f + radius * 2.5f;
    float fade_end = 18000.0f + radius * 18.0f;
    if (fade_end <= fade_start + 1.0f) fade_end = fade_start + 1.0f;
    if (dist <= fade_start) return 1.0f;
    if (dist >= fade_end) return 0.0f;
    return 1.0f - (float)smoothstepd(fade_start, fade_end, dist);
}

/* (Re)create a reduced-res volumetric target: slot 0 at half the window size,
 * slot 1 at a quarter. Colour only (no depth attachment): the layer is
 * composited as translucent foreground, so it isn't depth-tested against the
 * scene while reduced — a brief, contained trade-off during a supernova in
 * exchange for the fragment savings and robustness (a scaling depth-blit from
 * a multisampled default framebuffer is not portable). */
static void vol_target_ensure(int slot)
{
    int div = slot ? 4 : 2;
    int hw = WIN_W > div - 1 ? WIN_W / div : 1;
    int hh = WIN_H > div - 1 ? WIN_H / div : 1;
    if (s_vol_fbo[slot] && hw == s_vol_w[slot] && hh == s_vol_h[slot]) return;
    s_vol_w[slot] = hw;
    s_vol_h[slot] = hh;
    if (!s_vol_fbo[slot])   glGenFramebuffers(1, &s_vol_fbo[slot]);
    if (!s_vol_color[slot]) glGenTextures(1, &s_vol_color[slot]);
    glBindTexture(GL_TEXTURE_2D, s_vol_color[slot]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_vol_fbo[slot]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_vol_color[slot], 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Physical scales of a black hole, for realistic rendering sizes:
 *   rs_m    — Schwarzschild radius (metres): read from b->radius, which every
 *             creating/growing event derives from mass through
 *             laws_schwarzschild_radius() — the single root.  Rendering never
 *             re-derives it, so visuals, devour horizon and accretion always
 *             agree;
 *   a_star  — dimensionless spin, from Ω·Rs/c (surface-speed proxy), 0..0.998;
 *   isco_rs — prograde Kerr ISCO (Bardeen 1972) in Rs units: 3 at a*=0 down to
 *             ~0.5 near-maximal, i.e. the inner accretion-disk edge.
 * Everything the BH/jet/torus passes draw is expressed in Rs, so the whole
 * engine scales physically with the hole's mass. */
static void bh_scales(const Body *b, double *rs_m, double *a_star, double *isco_rs)
{
    const double C = 2.99792458e8;
    double Rs = b->radius > 0.0
              ? b->radius
              : laws_schwarzschild_radius(b->mass > 0.0 ? b->mass : 1.0);
    /* Spin magnitude comes from the evolved a* (accretion.c spins it up); fall
     * back to the raw rotation_rate only for a hole that never got seeded. */
    double a  = fabs(b->spin_a);
    if (a == 0.0 && b->rotation_rate != 0.0) a = fabs(b->rotation_rate) * Rs / C;
    if (a > 0.998) a = 0.998;
    double z1 = 1.0 + cbrt(1.0 - a * a) * (cbrt(1.0 + a) + cbrt(1.0 - a));
    double z2 = sqrt(3.0 * a * a + z1 * z1);
    double risco_M = 3.0 + z2 - sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2));
    *rs_m    = Rs;
    *a_star  = a;
    *isco_rs = risco_M * 0.5;   /* M = GM/c² = Rs/2 → convert to Rs units */
}

/* Visual render radius in AU-units (= metres × RS), accounting for collision
 * scaling.  Bodies that have absorbed mass grow their visual radius. */
static float visual_radius(int i, float dcam) {
    (void)dcam;
    return (float)(collision_visual_radius(i, g_bodies[i].radius) * RS);
}

/* Smooth step in double precision — used for visibility fade calculations. */
static double smoothstepd(double edge0, double edge1, double x) {
    double t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

/* Sphere-rendered non-star occluder indices, rebuilt each frame in
 * render_frame() and consumed by body_point_occluded_by_body().  Backing store
 * is grown by render_scratch_ensure() alongside the other per-frame scratch. */
static int *s_rs_occluders = NULL;
static int  s_rs_nocc      = 0;

/*
 * star_dot_pixel_size — point size (px) for a body's centre dot.
 *
 * Stars vary in size by apparent magnitude so the field reads like real sky:
 * bright/near/luminous stars draw as larger points, faint ones small.  The
 * luminosity proxy is (R/R☉)² (the star's temperature is already conveyed by
 * its colour); apparent magnitude is
 *     m = M☉ − 2.5·log10(L/L☉) + 5·log10(d_pc) − 5,
 * and brighter (smaller m) maps to a larger point, clamped to a sane range.
 * `dcam` is the true camera distance in AU (render units), so far stars are
 * sized by their real distance even though their draw position is clamped.
 * Non-stars get a fixed small dot.
 */
static float star_dot_apparent_mag(int idx, float dcam)
{
    const double R_SUN_M   = 6.957e8;
    const double AU_PER_PC = 206264.806;
    const double M_SUN     = 4.83;

    double L = (double)g_bodies[idx].radius / R_SUN_M;
    L = L * L;
    if (!(L > 1e-6)) L = 1e-6;
    double d_pc = (double)dcam / AU_PER_PC;
    if (!(d_pc > 1e-9)) d_pc = 1e-9;

    return (float)(M_SUN - 2.5 * log10(L) + 5.0 * log10(d_pc) - 5.0);
}

static float star_dot_pixel_size(int idx, float dcam)
{
    const float BASE_DOT_PX = 2.3f;
    if (!g_bodies[idx].is_star) return BASE_DOT_PX;

    float m    = star_dot_apparent_mag(idx, dcam);
    float size = 7.0f - 0.45f * (m + 1.0f);   /* m≈−1 → 7px ; m≈+12 → ~1px */
    if (size < 1.4f) size = 1.4f;
    if (size > 7.0f) size = 7.0f;
    return size;
}

/*
 * star_dot_hdr_gain — HDR colour gain for the naked-eye bright end.
 *
 * Below apparent magnitude 2.5 the dot's colour keeps rising past 1.0 on the
 * real (compressed-exponent) magnitude scale, so the bloom pass blazes bright
 * stars in their own colour instead of capping them at an LDR dot.  The curve
 * is identical to starfield.c display_brightness_from_mag()'s overbright term
 * so body dots and the painted-skybox stars brighten consistently.
 */
static float star_dot_hdr_gain(int idx, float dcam)
{
    if (!g_bodies[idx].is_star) return 1.0f;
    float m = star_dot_apparent_mag(idx, dcam);
    if (m >= 2.5f) return 1.0f;
    float g = powf(10.0f, 0.28f * (2.5f - m));
    return g > 6.0f ? 6.0f : g;
}

/*
 * body_point_star_glare_visibility — compute how much the dot for body_idx
 * is occluded/dimmed by other stars' glare coronae.
 *
 * For each other live star, casts a ray from the camera toward body_idx and
 * measures the perpendicular distance from that star to the ray.  If the
 * star's physical disc intersects the ray, returns 0 (fully blocked).
 * Otherwise, evaluates the glare shader's exponential falloff at the radial
 * distance and converts it to a visibility fraction via smoothstep.
 *
 * The glare formula mirrors star_glare.frag so dot transparency exactly
 * matches the corona brightness — the dot fades precisely as the glare
 * brightens, with no visible seam.
 *
 * Occluders come from the caller's near-star list (dot_stars): an occluding
 * star must sit closer than the target along the view ray with any effect
 * confined to ~15 star radii of it, so it is always itself a near star —
 * scanning all g_nbodies for it was pure waste at catalog scale (and wrongly
 * counted glare-less black holes, which the near list excludes).
 *
 * Returns 1.0 (fully visible) if no star intersects.
 */
static float body_point_star_glare_visibility(int body_idx,
                                              const int *stars, int nstars) {
    Body *b = &g_bodies[body_idx];

    double bx = b->pos[0] * RS - g_cam.pos[0];
    double by = b->pos[1] * RS - g_cam.pos[1];
    double bz = b->pos[2] * RS - g_cam.pos[2];
    double bd2 = bx*bx + by*by + bz*bz;
    if (bd2 <= 1e-18) return 0;

    /* Unit direction from camera toward the body */
    double bd = sqrt(bd2);
    double ux = bx / bd;
    double uy = by / bd;
    double uz = bz / bd;
    double visibility = 1.0;

    for (int si = 0; si < nstars; si++) {
        int i = stars[si];
        if (i == body_idx) continue;
        Body *s = &g_bodies[i];
        if (!s->alive || !s->is_star) continue;

        /* Project star position onto the ray */
        double sx = s->pos[0] * RS - g_cam.pos[0];
        double sy = s->pos[1] * RS - g_cam.pos[1];
        double sz = s->pos[2] * RS - g_cam.pos[2];
        double along = sx*ux + sy*uy + sz*uz;
        /* Skip if star is behind camera or beyond the target body */
        if (along <= 0.0 || along >= bd) continue;

        /* Perpendicular distance from star to the ray, in star-radius units */
        double sd2 = sx*sx + sy*sy + sz*sz;
        double perp2 = sd2 - along*along;
        double sr = s->radius * RS;
        double r_units = sqrt(perp2 < 0.0 ? 0.0 : perp2) / sr;
        if (r_units <= 1.0) return 0.0f;   /* physical disc blocks the ray */

        /* Glare falloff matching star_glare.frag */
        double r_safe = r_units > 0.05 ? r_units : 0.05;
        double shine = 2.1 * exp(-r_safe * 0.48);
        double outer_fade = 1.0 - smoothstepd(6.0, 15.0, r_units);
        double glare = shine * outer_fade;
        double star_vis = 1.0 - smoothstepd(0.04, 0.38, glare);
        if (star_vis < visibility) visibility = star_vis;
    }

    return (float)visibility;
}

/*
 * system_dot_fade_for_body — LOD fade for planet/moon dots at interstellar range.
 *
 * Planets and moons are rendered as dots even when their system is not the
 * camera's current system.  At interstellar distances individual planet dots
 * become misleading (wrong scale, wrong context), so they fade out as the
 * camera moves far from the body's host star:
 *   distance < SYS_DOT_FADE_START  → full opacity
 *   distance > SYS_DOT_FADE_END    → invisible
 *
 * For bodies with no valid star parent (should not occur in normal operation),
 * the body's own distance to the camera is used instead.
 */
static float system_dot_fade_for_body(int body_idx)
{
    int ref_star;
    double sdx, sdy, sdz, dist_star;
    float dot_fade;
    const double FREE_DOT_FADE_START = 400.0;
    const double FREE_DOT_FADE_END = 800.0;

    if (body_idx < 0 || body_idx >= g_nbodies) return 1.0f;
    if (g_bodies[body_idx].is_star) return 1.0f;

    ref_star = body_root_star(body_idx);
    if (ref_star < 0 || ref_star >= g_nbodies ||
        !g_bodies[ref_star].alive || !g_bodies[ref_star].is_star) {
        /* No host star: use body-to-camera distance */
        double bx = g_bodies[body_idx].pos[0] * RS - g_cam.pos[0];
        double by = g_bodies[body_idx].pos[1] * RS - g_cam.pos[1];
        double bz = g_bodies[body_idx].pos[2] * RS - g_cam.pos[2];
        double dist_body = sqrt(bx*bx + by*by + bz*bz);

        dot_fade = 1.0f - (float)smoothstepd(FREE_DOT_FADE_START,
                                             FREE_DOT_FADE_END, dist_body);
        return dot_fade;
    }

    /* Camera-to-star distance drives the fade for the whole system */
    sdx = g_cam.pos[0] - g_bodies[ref_star].pos[0] * RS;
    sdy = g_cam.pos[1] - g_bodies[ref_star].pos[1] * RS;
    sdz = g_cam.pos[2] - g_bodies[ref_star].pos[2] * RS;
    dist_star = sqrt(sdx*sdx + sdy*sdy + sdz*sdz);

    /* Smooth (Hermite) fade — same endpoints as before, but eases in/out so
     * the system's dots dim without a visible fade-rate kink (continuous LOD). */
    dot_fade = 1.0f - (float)smoothstepd(SYS_DOT_FADE_START,
                                         SYS_DOT_FADE_END, dist_star);
    return dot_fade;
}

/*
 * body_point_occluded_by_body — ray-sphere test for dot occlusion.
 *
 * Returns 1 if a nearer, visible-as-sphere (info[i].show == 0) body's disc
 * falls between the camera and body_idx along the look ray.  Stars are never
 * considered occluders (they are always shown as dots or glare).
 *
 * Only the (typically tiny) set of sphere-rendered non-star bodies can occlude,
 * so it iterates the precomputed s_rs_occluders list instead of scanning all
 * g_nbodies — at galaxy scale that turns this O(dots x N) hot path into
 * O(dots x occluders).  The per-occluder test is identical to the old all-body
 * scan; only the candidates iterated change.
 */
static int body_point_occluded_by_body(int body_idx, const BodyRenderInfo info[]) {
    double bx = g_bodies[body_idx].pos[0] * RS - g_cam.pos[0];
    double by = g_bodies[body_idx].pos[1] * RS - g_cam.pos[1];
    double bz = g_bodies[body_idx].pos[2] * RS - g_cam.pos[2];
    double bd2 = bx*bx + by*by + bz*bz;
    if (bd2 <= 1e-18) return 0;

    double bd = sqrt(bd2);
    double ux = bx / bd;
    double uy = by / bd;
    double uz = bz / bd;

    for (int oi = 0; oi < s_rs_nocc; oi++) {
        int i = s_rs_occluders[oi];
        if (i == body_idx) continue;

        double sx = g_bodies[i].pos[0] * RS - g_cam.pos[0];
        double sy = g_bodies[i].pos[1] * RS - g_cam.pos[1];
        double sz = g_bodies[i].pos[2] * RS - g_cam.pos[2];
        double along = sx*ux + sy*uy + sz*uz;
        if (along <= 0.0 || along >= bd) continue;

        double sd2 = sx*sx + sy*sy + sz*sz;
        double perp2 = sd2 - along*along;
        double r = info[i].dr;
        if (perp2 <= r*r) return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ init */
void render_init(void) {
    /* GL_DEPTH_CLAMP prevents near-plane clipping of close billboard geometry.
     * Without it, the large sphere billboard quad (which extends well beyond
     * the near plane at very close range) would be clipped and produce a hole
     * at the sphere edge.  With depth clamp, fragments with z < near are kept
     * but clamped to depth 0 rather than discarded. */
    glEnable(GL_DEPTH_CLAMP);

    /* --- Sphere billboard shader --- */
    s_sphere_shader = gl_shader_load("assets/shaders/phong.vert",
                                     "assets/shaders/phong.frag");
    if (!s_sphere_shader) {
        fprintf(stderr, "[Render] phong shader failed\n");
        return;
    }

    s_sp_vp       = glGetUniformLocation(s_sphere_shader, "u_vp");
    s_sp_center   = glGetUniformLocation(s_sphere_shader, "u_center");
    s_sp_radius   = glGetUniformLocation(s_sphere_shader, "u_radius");
    s_sp_cam_right= glGetUniformLocation(s_sphere_shader, "u_cam_right");
    s_sp_cam_up   = glGetUniformLocation(s_sphere_shader, "u_cam_up");
    s_sp_color     = glGetUniformLocation(s_sphere_shader, "u_color");
    s_sp_emission  = glGetUniformLocation(s_sphere_shader, "u_emission");
    s_sp_ambient   = glGetUniformLocation(s_sphere_shader, "u_ambient");
    s_sp_sun_world = glGetUniformLocation(s_sphere_shader, "u_sun_pos_world");
    s_sp_oc        = glGetUniformLocation(s_sphere_shader, "u_oc");
    s_sp_sun_rel   = glGetUniformLocation(s_sphere_shader, "u_sun_rel");
    s_sp_sun_col   = glGetUniformLocation(s_sphere_shader, "u_sun_col");
    s_sp_sun2_rel  = glGetUniformLocation(s_sphere_shader, "u_sun2_rel");
    s_sp_light2    = glGetUniformLocation(s_sphere_shader, "u_light2");
    s_sp_light2_col = glGetUniformLocation(s_sphere_shader, "u_light2_col");
    s_sp_cam_fwd   = glGetUniformLocation(s_sphere_shader, "u_cam_fwd");
    s_sp_fov_tan   = glGetUniformLocation(s_sphere_shader, "u_fov_tan");
    s_sp_aspect    = glGetUniformLocation(s_sphere_shader, "u_aspect");
    s_sp_screen    = glGetUniformLocation(s_sphere_shader, "u_screen");
    s_sp_rotation        = glGetUniformLocation(s_sphere_shader, "u_rotation");
    s_sp_cloud_rotation  = glGetUniformLocation(s_sphere_shader, "u_cloud_rotation");
    s_sp_cloud_amount    = glGetUniformLocation(s_sphere_shader, "u_cloud_amount");
    s_sp_ring            = glGetUniformLocation(s_sphere_shader, "u_ring");
    s_sp_ring_pole       = glGetUniformLocation(s_sphere_shader, "u_ring_pole");
    s_sp_ecl_count       = glGetUniformLocation(s_sphere_shader, "u_ecl_count");
    s_sp_ecl             = glGetUniformLocation(s_sphere_shader, "u_ecl[0]");
    s_sp_sun_radius      = glGetUniformLocation(s_sphere_shader, "u_sun_radius");
    s_sp_time            = glGetUniformLocation(s_sphere_shader, "u_time");
    s_sp_obliquity = glGetUniformLocation(s_sphere_shader, "u_obliquity");
    s_sp_ptype     = glGetUniformLocation(s_sphere_shader, "u_planet_type");
    s_sp_star_heat = glGetUniformLocation(s_sphere_shader, "u_star_heat");
    s_sp_starspots = glGetUniformLocation(s_sphere_shader, "u_starspots");
    s_sp_impact_count = glGetUniformLocation(s_sphere_shader, "u_impact_count");
    s_sp_impact_dir   = glGetUniformLocation(s_sphere_shader, "u_impact_dir[0]");
    s_sp_impact_t1    = glGetUniformLocation(s_sphere_shader, "u_impact_tangent1[0]");
    s_sp_impact_rad   = glGetUniformLocation(s_sphere_shader, "u_impact_radius[0]");
    s_sp_impact_heat  = glGetUniformLocation(s_sphere_shader, "u_impact_heat[0]");
    s_sp_impact_prog  = glGetUniformLocation(s_sphere_shader, "u_impact_progress[0]");
    s_sp_impact_seed  = glGetUniformLocation(s_sphere_shader, "u_impact_seed[0]");
    s_sp_impact_kind  = glGetUniformLocation(s_sphere_shader, "u_impact_kind[0]");
    s_sp_use_fullscreen = glGetUniformLocation(s_sphere_shader, "u_use_fullscreen");
    s_sp_stretch_dir    = glGetUniformLocation(s_sphere_shader, "u_stretch_dir");
    s_sp_stretch_along  = glGetUniformLocation(s_sphere_shader, "u_stretch_along");
    s_sp_stretch_perp   = glGetUniformLocation(s_sphere_shader, "u_stretch_perp");
    s_sp_tidal_glow     = glGetUniformLocation(s_sphere_shader, "u_tidal_glow");
    s_sp_opacity        = glGetUniformLocation(s_sphere_shader, "u_opacity");

    /* Frame-constant uniforms (fov_tan, aspect, screen do not change at runtime) */
    glUseProgram(s_sphere_shader);
    glUniform1f(s_sp_fov_tan, tanf(FOV * 0.5f * (float)(PI / 180.0)));
    glUniform1f(s_sp_aspect,  (float)WIN_W / (float)WIN_H);
    glUniform2f(s_sp_screen,  (float)WIN_W, (float)WIN_H);
    glUseProgram(0);

    /* Unit quad: corners at UV (0,0)..(1,1), two triangles */
    static const float quad_v[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };
    static const unsigned int quad_i[] = { 0,1,2, 0,2,3 };

    s_sphere_vao = gl_vao_create();
    s_sphere_vbo = gl_vbo_create(sizeof(quad_v), quad_v, GL_STATIC_DRAW);
    s_sphere_ebo = gl_ebo_create(sizeof(quad_i), quad_i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glBindVertexArray(0);

    /* --- Star glare shader --- */
    s_glare_shader = gl_shader_load("assets/shaders/star_glare.vert",
                                    "assets/shaders/star_glare.frag");
    if (!s_glare_shader)
        fprintf(stderr, "[Render] star_glare shader failed\n");
    else {
        s_gl_vp     = glGetUniformLocation(s_glare_shader, "u_vp");
        s_gl_center = glGetUniformLocation(s_glare_shader, "u_center");
        s_gl_radius = glGetUniformLocation(s_glare_shader, "u_radius");
        s_gl_right  = glGetUniformLocation(s_glare_shader, "u_cam_right");
        s_gl_up     = glGetUniformLocation(s_glare_shader, "u_cam_up");
        s_gl_color  = glGetUniformLocation(s_glare_shader, "u_color");
        s_gl_spike  = glGetUniformLocation(s_glare_shader, "u_spike");
        s_gl_corona = glGetUniformLocation(s_glare_shader, "u_corona");
        s_gl_time   = glGetUniformLocation(s_glare_shader, "u_time");
        s_gl_seed   = glGetUniformLocation(s_glare_shader, "u_seed");
        s_gl_resolve= glGetUniformLocation(s_glare_shader, "u_resolve");
    }

    /* --- Black-hole shader --- */
    s_bh_shader = gl_shader_load("assets/shaders/bh.vert", "assets/shaders/bh.frag");
    if (!s_bh_shader)
        fprintf(stderr, "[Render] black-hole shader failed\n");
    else {
        s_bh_vp     = glGetUniformLocation(s_bh_shader, "u_vp");
        s_bh_center = glGetUniformLocation(s_bh_shader, "u_center");
        s_bh_radius = glGetUniformLocation(s_bh_shader, "u_radius");
        s_bh_right  = glGetUniformLocation(s_bh_shader, "u_cam_right");
        s_bh_up     = glGetUniformLocation(s_bh_shader, "u_cam_up");
        s_bh_color  = glGetUniformLocation(s_bh_shader, "u_color");
        s_bh_disk_n = glGetUniformLocation(s_bh_shader, "u_disk_normal");
        s_bh_time   = glGetUniformLocation(s_bh_shader, "u_time");
        s_bh_activity = glGetUniformLocation(s_bh_shader, "u_activity");
        s_bh_spin     = glGetUniformLocation(s_bh_shader, "u_spin");
        s_bh_disk     = glGetUniformLocation(s_bh_shader, "u_disk");
        s_bh_disk_in  = glGetUniformLocation(s_bh_shader, "u_disk_in");
        s_bh_disk_temp = glGetUniformLocation(s_bh_shader, "u_disk_temp");
        s_bh_disk_rate = glGetUniformLocation(s_bh_shader, "u_disk_rate");
        s_bh_scene     = glGetUniformLocation(s_bh_shader, "u_scene");
        s_bh_has_scene = glGetUniformLocation(s_bh_shader, "u_has_scene");
    }

    /* --- AGN jet shader --- */
    s_jet_shader = gl_shader_load("assets/shaders/jet.vert", "assets/shaders/jet.frag");
    if (!s_jet_shader)
        fprintf(stderr, "[Render] jet shader failed\n");
    else {
        s_jet_vp       = glGetUniformLocation(s_jet_shader, "u_vp");
        s_jet_center   = glGetUniformLocation(s_jet_shader, "u_center");
        s_jet_axis     = glGetUniformLocation(s_jet_shader, "u_axis");
        s_jet_len      = glGetUniformLocation(s_jet_shader, "u_len");
        s_jet_width    = glGetUniformLocation(s_jet_shader, "u_width");
        s_jet_color    = glGetUniformLocation(s_jet_shader, "u_color");
        s_jet_time     = glGetUniformLocation(s_jet_shader, "u_time");
        s_jet_activity = glGetUniformLocation(s_jet_shader, "u_activity");
    }

    /* --- AGN dust torus shader --- */
    s_torus_shader = gl_shader_load("assets/shaders/torus.vert", "assets/shaders/torus.frag");
    if (!s_torus_shader)
        fprintf(stderr, "[Render] torus shader failed\n");
    else {
        s_torus_vp     = glGetUniformLocation(s_torus_shader, "u_vp");
        s_torus_center = glGetUniformLocation(s_torus_shader, "u_center");
        s_torus_ext    = glGetUniformLocation(s_torus_shader, "u_ext");
        s_torus_right  = glGetUniformLocation(s_torus_shader, "u_cam_right");
        s_torus_up     = glGetUniformLocation(s_torus_shader, "u_cam_up");
        s_torus_rs     = glGetUniformLocation(s_torus_shader, "u_rs");
        s_torus_normal = glGetUniformLocation(s_torus_shader, "u_normal");
        s_torus_rmaj   = glGetUniformLocation(s_torus_shader, "u_rmaj");
        s_torus_rmin   = glGetUniformLocation(s_torus_shader, "u_rmin");
        s_torus_color  = glGetUniformLocation(s_torus_shader, "u_color");
        s_torus_time   = glGetUniformLocation(s_torus_shader, "u_time");
        s_torus_rate   = glGetUniformLocation(s_torus_shader, "u_rate");
    }

    /* --- AGN beamed-core shader --- */
    s_agncore_shader = gl_shader_load("assets/shaders/agncore.vert", "assets/shaders/agncore.frag");
    if (!s_agncore_shader)
        fprintf(stderr, "[Render] AGN core shader failed\n");
    else {
        s_agncore_vp     = glGetUniformLocation(s_agncore_shader, "u_vp");
        s_agncore_center = glGetUniformLocation(s_agncore_shader, "u_center");
        s_agncore_size   = glGetUniformLocation(s_agncore_shader, "u_size");
        s_agncore_right  = glGetUniformLocation(s_agncore_shader, "u_cam_right");
        s_agncore_up     = glGetUniformLocation(s_agncore_shader, "u_cam_up");
        s_agncore_color  = glGetUniformLocation(s_agncore_shader, "u_color");
        s_agncore_int    = glGetUniformLocation(s_agncore_shader, "u_intensity");
    }

    /* --- Supernova shaders --- */
    s_supernova_core_shader = gl_shader_load("assets/shaders/supernova_billboard.vert",
                                             "assets/shaders/supernova_core.frag");
    if (!s_supernova_core_shader)
        fprintf(stderr, "[Render] supernova core shader failed\n");
    else {
        s_sn_core_vp = glGetUniformLocation(s_supernova_core_shader, "u_vp");
        s_sn_core_center = glGetUniformLocation(s_supernova_core_shader, "u_center");
        s_sn_core_radius = glGetUniformLocation(s_supernova_core_shader, "u_radius");
        s_sn_core_right = glGetUniformLocation(s_supernova_core_shader, "u_cam_right");
        s_sn_core_up = glGetUniformLocation(s_supernova_core_shader, "u_cam_up");
        s_sn_core_fwd = glGetUniformLocation(s_supernova_core_shader, "u_cam_fwd");
        s_sn_core_oc = glGetUniformLocation(s_supernova_core_shader, "u_oc");
        s_sn_core_color = glGetUniformLocation(s_supernova_core_shader, "u_color");
        s_sn_core_flash = glGetUniformLocation(s_supernova_core_shader, "u_flash_intensity");
        s_sn_core_core = glGetUniformLocation(s_supernova_core_shader, "u_core_intensity");
        s_sn_core_ratio = glGetUniformLocation(s_supernova_core_shader, "u_core_ratio");
        s_sn_core_time = glGetUniformLocation(s_supernova_core_shader, "u_time");
        s_sn_core_seed = glGetUniformLocation(s_supernova_core_shader, "u_seed");
        s_sn_core_bill = glGetUniformLocation(s_supernova_core_shader, "u_bill_scale");
        s_sn_core_fullscreen = glGetUniformLocation(s_supernova_core_shader, "u_fullscreen");
        s_sn_core_fov_tan = glGetUniformLocation(s_supernova_core_shader, "u_fov_tan");
        s_sn_core_aspect = glGetUniformLocation(s_supernova_core_shader, "u_aspect");
        s_sn_core_screen = glGetUniformLocation(s_supernova_core_shader, "u_screen");
    }

    s_supernova_cloud_shader = gl_shader_load("assets/shaders/supernova_billboard.vert",
                                              "assets/shaders/supernova_cloud.frag");
    if (!s_supernova_cloud_shader)
        fprintf(stderr, "[Render] supernova cloud shader failed\n");
    else {
        s_sn_cloud_vp = glGetUniformLocation(s_supernova_cloud_shader, "u_vp");
        s_sn_cloud_center = glGetUniformLocation(s_supernova_cloud_shader, "u_center");
        s_sn_cloud_radius = glGetUniformLocation(s_supernova_cloud_shader, "u_radius");
        s_sn_cloud_right = glGetUniformLocation(s_supernova_cloud_shader, "u_cam_right");
        s_sn_cloud_up = glGetUniformLocation(s_supernova_cloud_shader, "u_cam_up");
        s_sn_cloud_fwd = glGetUniformLocation(s_supernova_cloud_shader, "u_cam_fwd");
        s_sn_cloud_oc = glGetUniformLocation(s_supernova_cloud_shader, "u_oc");
        s_sn_cloud_color = glGetUniformLocation(s_supernova_cloud_shader, "u_color");
        s_sn_cloud_inner = glGetUniformLocation(s_supernova_cloud_shader, "u_shell_inner");
        s_sn_cloud_density = glGetUniformLocation(s_supernova_cloud_shader, "u_density");
        s_sn_cloud_hot = glGetUniformLocation(s_supernova_cloud_shader, "u_hot_shell");
        s_sn_cloud_time = glGetUniformLocation(s_supernova_cloud_shader, "u_time");
        s_sn_cloud_seed = glGetUniformLocation(s_supernova_cloud_shader, "u_seed");
        s_sn_cloud_bill = glGetUniformLocation(s_supernova_cloud_shader, "u_bill_scale");
        s_sn_cloud_fullscreen = glGetUniformLocation(s_supernova_cloud_shader, "u_fullscreen");
        s_sn_cloud_fov_tan = glGetUniformLocation(s_supernova_cloud_shader, "u_fov_tan");
        s_sn_cloud_aspect = glGetUniformLocation(s_supernova_cloud_shader, "u_aspect");
        s_sn_cloud_screen = glGetUniformLocation(s_supernova_cloud_shader, "u_screen");
    }

    /* --- Half-res volumetric composite (upscales the supernova cloud layer) --- */
    s_vol_composite_shader = gl_shader_load("assets/shaders/post_quad.vert",
                                            "assets/shaders/vol_composite.frag");
    if (!s_vol_composite_shader) {
        fprintf(stderr, "[Render] vol_composite shader failed\n");
    } else {
        static const float vquad[12] = {
            -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
            -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        };
        s_vol_comp_tex = glGetUniformLocation(s_vol_composite_shader, "u_tex");
        s_vol_quad_vao = gl_vao_create();
        s_vol_quad_vbo = gl_vbo_create(sizeof(vquad), vquad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    /* --- Atmosphere glow shader --- */
    s_atm_shader = gl_shader_load("assets/shaders/atm.vert",
                                  "assets/shaders/atm.frag");
    if (!s_atm_shader)
        fprintf(stderr, "[Render] atm shader failed\n");
    else {
        s_at_vp        = glGetUniformLocation(s_atm_shader, "u_vp");
        s_at_center    = glGetUniformLocation(s_atm_shader, "u_center");
        s_at_radius    = glGetUniformLocation(s_atm_shader, "u_radius");
        s_at_cam_right = glGetUniformLocation(s_atm_shader, "u_cam_right");
        s_at_cam_up    = glGetUniformLocation(s_atm_shader, "u_cam_up");
        s_at_cam_fwd   = glGetUniformLocation(s_atm_shader, "u_cam_fwd");
        s_at_oc        = glGetUniformLocation(s_atm_shader, "u_oc");
        s_at_planet_r  = glGetUniformLocation(s_atm_shader, "u_planet_radius");
        s_at_sun_rel   = glGetUniformLocation(s_atm_shader, "u_sun_rel");
        s_at_sun_col   = glGetUniformLocation(s_atm_shader, "u_sun_col");
        s_at_sun2_rel  = glGetUniformLocation(s_atm_shader, "u_sun2_rel");
        s_at_light2    = glGetUniformLocation(s_atm_shader, "u_light2");
        s_at_light2_col = glGetUniformLocation(s_atm_shader, "u_light2_col");
        s_at_color     = glGetUniformLocation(s_atm_shader, "u_atm_color");
        s_at_intensity = glGetUniformLocation(s_atm_shader, "u_atm_intensity");
        s_at_aspect    = glGetUniformLocation(s_atm_shader, "u_aspect");
        s_at_screen    = glGetUniformLocation(s_atm_shader, "u_screen");
        s_at_aurora    = glGetUniformLocation(s_atm_shader, "u_aurora");
        s_at_aur_shape = glGetUniformLocation(s_atm_shader, "u_aur_shape");
        s_at_aur_look  = glGetUniformLocation(s_atm_shader, "u_aur_look");
        s_at_time      = glGetUniformLocation(s_atm_shader, "u_time");
        glUseProgram(s_atm_shader);
        glUniform1f(glGetUniformLocation(s_atm_shader, "u_fov_tan"),
                    tanf(FOV * 0.5f * (float)(PI / 180.0)));
        glUniform1f(s_at_aspect, (float)WIN_W / (float)WIN_H);
        glUniform2f(s_at_screen, (float)WIN_W, (float)WIN_H);
        glUseProgram(0);
    }

    /* --- Dot shader: star_dot.vert adds a per-point size (attribute 2) so the
     * dot field can convey stellar magnitude; color.frag rounds the sprite.
     * Independent program from the background starfield (which keeps color.vert),
     * so this does not change the skybox. --- */
    s_dot_shader = gl_shader_load("assets/shaders/star_dot.vert",
                                  "assets/shaders/color.frag");
    if (!s_dot_shader) {
        fprintf(stderr, "[Render] star_dot shader failed\n");
        return;
    }
    s_dot_vp      = glGetUniformLocation(s_dot_shader, "u_vp");
    s_dot_time    = glGetUniformLocation(s_dot_shader, "u_time");
    s_dot_twinkle = glGetUniformLocation(s_dot_shader, "u_twinkle");

    /* --- Static field-star shader: star_field.vert does the camera-relative
     * transform + sizing on the GPU so the bulk Gaia field costs no per-frame
     * CPU.  Reuses color.frag (round sprite + log depth), same as the dots. --- */
    s_field_shader = gl_shader_load("assets/shaders/star_field.vert",
                                    "assets/shaders/color.frag");
    if (s_field_shader) {
        s_field_vp      = glGetUniformLocation(s_field_shader, "u_vp");
        s_field_cam     = glGetUniformLocation(s_field_shader, "u_cam");
        s_field_near    = glGetUniformLocation(s_field_shader, "u_near_dist");
        s_field_horizon = glGetUniformLocation(s_field_shader, "u_horizon");
        s_field_time    = glGetUniformLocation(s_field_shader, "u_time");
        s_field_twinkle = glGetUniformLocation(s_field_shader, "u_twinkle");
        s_field_vao = gl_vao_create();
        /* Sized on first build (field_stars_ensure); layout = (x,y,z, r,g,b,a,
         * absmag) × N, GL_STATIC_DRAW (never rebuilt except on universe reload). */
        s_field_vbo = gl_vbo_create(1 * 8 * sizeof(float), NULL, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float),
                              (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8*sizeof(float),
                              (void*)(7*sizeof(float)));
        glBindVertexArray(0);
    } else {
        fprintf(stderr, "[Render] star_field shader failed (field stars will be "
                        "drawn by the dynamic path)\n");
    }

    /* --- Impact particle shader --- */
    s_impact_particle_shader = gl_shader_load("assets/shaders/impact_particle.vert",
                                              "assets/shaders/impact_particle.frag");
    if (!s_impact_particle_shader) {
        fprintf(stderr, "[Render] impact particle shader failed\n");
        return;
    }
    s_impact_particle_vp = glGetUniformLocation(s_impact_particle_shader, "u_vp");

    /* Dynamic dot VBO: layout = (x,y,z, r,g,b,a, size) × MAX_BODIES.
     * GL_DYNAMIC_DRAW since it is rebuilt every frame from live body positions. */
    s_dot_vao = gl_vao_create();
    s_dot_vbo = gl_vbo_create(MAX_BODIES * 8 * sizeof(float),
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

    /* Impact particle VBO: layout = (x,y,z, r,g,b,a, size) per particle */
    s_impact_particle_vao = gl_vao_create();
    s_impact_particle_vbo = gl_vbo_create(RENDER_MAX_COLLISION_PARTICLES * 8 * sizeof(float),
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

    /* --- Build-mode guide lines (world-space line segments) --- */
    s_build_line_shader = gl_shader_load("assets/shaders/build_line.vert",
                                         "assets/shaders/build_line.frag");
    if (s_build_line_shader) {
        s_build_line_vp = glGetUniformLocation(s_build_line_shader, "u_vp");
        s_build_line_vao = gl_vao_create();
        /* 3 lines × 2 endpoints × 7 floats (xyz, rgba) */
        s_build_line_vbo = gl_vbo_create(6 * 7 * sizeof(float),
                                         NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7*sizeof(float),
                              (void*)(3*sizeof(float)));
        glBindVertexArray(0);
    }

    /* --- Build-mode distance label overlay (screen-space text quads) --- */
    s_build_ui_shader = gl_shader_load("assets/shaders/ui.vert",
                                       "assets/shaders/ui.frag");
    if (s_build_ui_shader) {
        s_build_ui_screen  = glGetUniformLocation(s_build_ui_shader, "u_screen");
        s_build_ui_color   = glGetUniformLocation(s_build_ui_shader, "u_color");
        s_build_ui_use_tex = glGetUniformLocation(s_build_ui_shader, "u_use_tex");
        s_build_ui_tex     = glGetUniformLocation(s_build_ui_shader, "u_tex");
        s_build_ui_vao = gl_vao_create();
        /* Shared overlay buffer: enough for one text quad or an inspect ring. */
        s_build_ui_vbo = gl_vbo_create(96 * 2 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                              (void*)(2*sizeof(float)));
        glBindVertexArray(0);
        glUseProgram(s_build_ui_shader);
        glUniform2f(s_build_ui_screen, (float)WIN_W, (float)WIN_H);
        glUniform1i(s_build_ui_tex, 0);
        glUseProgram(0);
    }
    TTF_Init();
    s_build_font = ui_theme_open_font((int)BUILD_UI_FONT_SIZE);
}

/*
 * render_build_preview — draw the build-mode ghost body and contextual overlays.
 *
 * Three overlays are drawn:
 *
 *   Ghost dot: a large (36px) GL_POINT at the preview body's world position,
 *   in the preset's color.  Drawn without depth test so it is always visible.
 *
 *   Guide lines: 3D line segments from the preview position to the 3 nearest
 *   existing bodies (build_nearest3).  Alpha decreases for more distant bodies.
 *
 *   Distance text panel: screen-space label list showing "name  distance" for
 *   each guide-line target.  Placement uses a 4-quadrant scoring system:
 *     - Try 4 candidate positions (NE, NW, SE, SW of the preview dot).
 *     - Score penalises overlap with guide lines (dot product along line
 *       direction + perpendicular proximity) and clamping to screen edges.
 *     - Choose the quadrant with the lowest score.
 *   Text cache prevents SDL_TTF texture re-renders when the string is unchanged.
 */
static void render_build_preview(const float vp_camrel[16])
{
    if (!g_build_mode) return;

    const BuildPreset *preset = build_current_preset();
    if (!preset) return;

    double preview_m[3];
    build_preview_pos_m(preview_m);

    int idx[3];
    double dist_au[3];
    build_nearest3(preview_m, idx, dist_au);

    /* Camera-relative position (double → float) for correct depth at any distance */
    float px = (float)(preview_m[0] * RS - g_cam.pos[0]);
    float py = (float)(preview_m[1] * RS - g_cam.pos[1]);
    float pz = (float)(preview_m[2] * RS - g_cam.pos[2]);

    /* Ghost dot */
    if (s_dot_shader) {
        float dot[7] = {
            px, py, pz,
            preset->col[0], preset->col[1], preset->col[2], 0.88f
        };
        glUseProgram(s_dot_shader);
        glUniformMatrix4fv(s_dot_vp, 1, GL_FALSE, vp_camrel);
        glUniform1f(s_dot_twinkle, 0.0f);   /* placement cursor: no twinkle */
        glBindVertexArray(s_dot_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_dot_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(dot), dot);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glPointSize(36.0f);
        glDrawArrays(GL_POINTS, 0, 1);
        glPointSize(1.0f);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    /* Guide lines to nearest 3 bodies */
    if (s_build_line_shader) {
        float line_data[6 * 7];
        int v = 0;
        for (int k = 0; k < 3; k++) {
            if (idx[k] < 0) continue;
            Body *b = &g_bodies[idx[k]];
            float bx = (float)(b->pos[0] * RS - g_cam.pos[0]);
            float by = (float)(b->pos[1] * RS - g_cam.pos[1]);
            float bz = (float)(b->pos[2] * RS - g_cam.pos[2]);
            float a = 0.72f;

            line_data[v*7+0] = px; line_data[v*7+1] = py; line_data[v*7+2] = pz;
            line_data[v*7+3] = 1.0f; line_data[v*7+4] = 1.0f;
            line_data[v*7+5] = 1.0f; line_data[v*7+6] = a;
            v++;
            line_data[v*7+0] = bx; line_data[v*7+1] = by; line_data[v*7+2] = bz;
            line_data[v*7+3] = 1.0f; line_data[v*7+4] = 1.0f;
            line_data[v*7+5] = 1.0f; line_data[v*7+6] = a;
            v++;
        }
        if (v > 0) {
            glUseProgram(s_build_line_shader);
            glUniformMatrix4fv(s_build_line_vp, 1, GL_FALSE, vp_camrel);
            glBindVertexArray(s_build_line_vao);
            glBindBuffer(GL_ARRAY_BUFFER, s_build_line_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, v * 7 * sizeof(float), line_data);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glLineWidth(1.5f);
            glDrawArrays(GL_LINES, 0, v);
            glLineWidth(1.0f);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glBindVertexArray(0);
        }
    }

    /* Screen-space distance labels beside the preview.
     * 4-quadrant placement: try NE, NW, SE, SW; choose lowest-scoring position.
     * Score = clamping penalty + guide-line overlap penalty. */
    if (s_build_ui_shader && s_build_font) {
        glUseProgram(s_build_ui_shader);
        glUniform2f(s_build_ui_screen, (float)WIN_W, (float)WIN_H);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(s_build_ui_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_build_ui_vbo);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float psx = 0.0f, psy = 0.0f;
        int preview_on_screen = mat4_project(vp_camrel, px, py, pz, WIN_W, WIN_H, &psx, &psy);
        float preview_y = (float)WIN_H - psy;
        int active[3] = {0, 0, 0};
        float text_w[3] = {0.0f, 0.0f, 0.0f};
        float dir_x[3] = {0.0f, 0.0f, 0.0f};
        float dir_y[3] = {0.0f, 0.0f, 0.0f};
        float max_w = 0.0f;
        int n_active = 0;

        /* Project guide-line endpoints to screen; compute screen-space directions */
        for (int k = 0; k < 3; k++) {
            if (idx[k] < 0 || !preview_on_screen) continue;
            Body *b = &g_bodies[idx[k]];
            float bx = (float)(b->pos[0] * RS - g_cam.pos[0]);
            float by = (float)(b->pos[1] * RS - g_cam.pos[1]);
            float bz = (float)(b->pos[2] * RS - g_cam.pos[2]);
            float bsx = 0.0f, bsy = 0.0f;
            int body_on_screen = mat4_project(vp_camrel, bx, by, bz, WIN_W, WIN_H, &bsx, &bsy);

            char buf[96];
            char dist_buf[32];
            format_dist_au(dist_au[k], dist_buf, sizeof(dist_buf));
            snprintf(buf, sizeof(buf), "%s  %s", b->name, dist_buf);
            build_update_text(&s_build_dist_text[k], buf);
            if (!s_build_dist_text[k].tex) continue;

            if (body_on_screen) {
                float body_y = (float)WIN_H - bsy;
                dir_x[k] = bsx - psx;
                dir_y[k] = body_y - preview_y;
            } else {
                dir_x[k] = 1.0f;
                dir_y[k] = 0.0f;
            }
            {
                float dl = sqrtf(dir_x[k]*dir_x[k] + dir_y[k]*dir_y[k]);
                if (dl < 1.0f) { dir_x[k] = 1.0f; dir_y[k] = 0.0f; dl = 1.0f; }
                dir_x[k] /= dl;
                dir_y[k] /= dl;
            }

            text_w[k] = BUILD_UI_FONT_SIZE * (float)s_build_dist_text[k].w
                      / (float)s_build_dist_text[k].h;
            if (text_w[k] > max_w) max_w = text_w[k];
            active[k] = 1;
            n_active++;
        }

        if (n_active > 0) {
            float row_h = BUILD_UI_FONT_SIZE + 4.0f;
            float list_h = row_h * (float)n_active;
            float margin_x = 42.0f;
            float margin_y = 28.0f;
            float best_score = 1e30f;
            float list_x = psx + margin_x;
            float list_y = preview_y - margin_y - list_h;
            int side = 1;

            /* Test 4 candidate positions (q=0: upper-right, 1: upper-left,
             * 2: lower-right, 3: lower-left) and pick the lowest-scoring one */
            for (int q = 0; q < 4; q++) {
                int sx = (q == 0 || q == 3) ? 1 : -1;
                int sy = (q == 0 || q == 1) ? -1 : 1;
                float cx = (sx > 0) ? psx + margin_x : psx - margin_x - max_w;
                float cy = (sy < 0) ? preview_y - margin_y - list_h
                                     : preview_y + margin_y;
                float clamped_x = clampf_local(cx, 8.0f, (float)WIN_W - max_w - 8.0f);
                float clamped_y = clampf_local(cy, 8.0f, (float)WIN_H - list_h - 8.0f);
                float center_x = clamped_x + max_w * 0.5f;
                float center_y = clamped_y + list_h * 0.5f;
                float vx = center_x - psx;
                float vy = center_y - preview_y;
                float vl = sqrtf(vx*vx + vy*vy);
                /* Clamping penalty: 8× the pixel distance we had to move */
                float score = fabsf(clamped_x - cx) * 8.0f
                            + fabsf(clamped_y - cy) * 8.0f;
                if (vl < 1.0f) vl = 1.0f;
                vx /= vl;
                vy /= vl;

                /* Guide-line overlap penalty for each active label */
                for (int k = 0; k < 3; k++) {
                    if (!active[k]) continue;
                    float dot = vx * dir_x[k] + vy * dir_y[k];
                    if (dot > 0.0f) score += dot * dot * 120.0f;
                    {
                        float raw_x = center_x - psx;
                        float raw_y = center_y - preview_y;
                        float along = raw_x * dir_x[k] + raw_y * dir_y[k];
                        float perp = fabsf(raw_x * dir_y[k] - raw_y * dir_x[k]);
                        if (along > 0.0f && perp < 48.0f)
                            score += (48.0f - perp) * 2.5f;
                    }
                }

                if (score < best_score) {
                    best_score = score;
                    list_x = clamped_x;
                    list_y = clamped_y;
                    side = sx;
                }
            }

            int row = 0;
            for (int k = 0; k < 3; k++) {
                if (!active[k]) continue;
                float x = (side > 0) ? list_x : list_x + max_w - text_w[k];
                build_draw_text(&s_build_dist_text[k], x, list_y + row_h * (float)row,
                                BUILD_UI_FONT_SIZE);
                row++;
            }
        }

        glBindVertexArray(0);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }
}

/* ------------------------------------------------------------------ frame */
/* ── per-frame render scratch ──────────────────────────────────────────────
 * These were fixed [MAX_BODIES] stack arrays, which capped the engine at 128
 * bodies and would overflow a galaxy-scale universe.  They are now heap buffers
 * grown once to g_nbodies, so render_frame() can address every body.  (The
 * expensive per-body work is still gated by screen-size LOD; the camera-driven
 * active set in a later phase will bound it further.) */
static int             s_rs_cap = 0;
static BodyRenderInfo *s_rs_info = NULL;
static float          *s_rs_body_px = NULL;
static float          *s_rs_sphere_alpha = NULL;  /* continuous-LOD blend: sphere in / dot out */
static int            *s_rs_dot_order = NULL, *s_rs_dot_stars = NULL,
                      *s_rs_dot_planets = NULL, *s_rs_dot_moons = NULL;
static float          *s_rs_dot_sx = NULL, *s_rs_dot_sy = NULL,
                      *s_rs_dot_overlap_alpha = NULL,
                      *s_rs_glare_vis = NULL;   /* per-frame glare-occlusion memo */
static int            *s_rs_dot_candidate = NULL, *s_rs_dot_vis = NULL;
static float          *s_rs_dot_data = NULL;
static float          *s_rs_aur_act = NULL;  /* smoothed aurora storm activity
                                                (-1 = no sample yet)         */
/* body_lights() output cached in the sphere pass and reused by the atmosphere
 * pass — both run over the identical resolved set, so recomputing the two-light
 * RadianceField query (O(emitters)) twice per body per frame is pure waste. */
typedef struct { float sr1[3], col1[3], sr2[3], col2[3], w2; } BodyLightCache;
static BodyLightCache *s_rs_light = NULL;
static char           *s_rs_light_valid = NULL;   /* 1 = cached this frame */
static int             s_dot_vbo_cap = 0;   /* # dots the VBO can hold */

static void render_scratch_ensure(int n)
{
    if (n <= s_rs_cap) return;
    int c = s_rs_cap ? s_rs_cap : MAX_BODIES;
    while (c < n) c *= 2;
    s_rs_info              = realloc(s_rs_info,              (size_t)c * sizeof(*s_rs_info));
    s_rs_body_px           = realloc(s_rs_body_px,           (size_t)c * sizeof(float));
    s_rs_sphere_alpha      = realloc(s_rs_sphere_alpha,      (size_t)c * sizeof(float));
    s_rs_dot_order         = realloc(s_rs_dot_order,         (size_t)c * sizeof(int));
    s_rs_dot_stars         = realloc(s_rs_dot_stars,         (size_t)c * sizeof(int));
    s_rs_dot_planets       = realloc(s_rs_dot_planets,       (size_t)c * sizeof(int));
    s_rs_dot_moons         = realloc(s_rs_dot_moons,         (size_t)c * sizeof(int));
    s_rs_dot_sx            = realloc(s_rs_dot_sx,            (size_t)c * sizeof(float));
    s_rs_dot_sy            = realloc(s_rs_dot_sy,            (size_t)c * sizeof(float));
    s_rs_dot_overlap_alpha = realloc(s_rs_dot_overlap_alpha, (size_t)c * sizeof(float));
    s_rs_glare_vis         = realloc(s_rs_glare_vis,         (size_t)c * sizeof(float));
    s_rs_dot_candidate     = realloc(s_rs_dot_candidate,     (size_t)c * sizeof(int));
    s_rs_dot_vis           = realloc(s_rs_dot_vis,           (size_t)c * sizeof(int));
    s_rs_dot_data          = realloc(s_rs_dot_data,     (size_t)c * 8 * sizeof(float));
    s_rs_aur_act           = realloc(s_rs_aur_act,          (size_t)c * sizeof(float));
    s_rs_light             = realloc(s_rs_light,            (size_t)c * sizeof(*s_rs_light));
    s_rs_light_valid       = realloc(s_rs_light_valid,      (size_t)c * sizeof(char));
    s_rs_occluders         = realloc(s_rs_occluders,        (size_t)c * sizeof(int));
    if (!s_rs_info || !s_rs_body_px || !s_rs_sphere_alpha || !s_rs_dot_order || !s_rs_dot_stars ||
        !s_rs_dot_planets || !s_rs_dot_moons || !s_rs_dot_sx || !s_rs_dot_sy ||
        !s_rs_dot_overlap_alpha || !s_rs_glare_vis ||
        !s_rs_dot_candidate || !s_rs_dot_vis ||
        !s_rs_dot_data || !s_rs_aur_act || !s_rs_light || !s_rs_light_valid ||
        !s_rs_occluders) {
        fprintf(stderr, "[render] scratch alloc failed\n");
        exit(1);
    }
    /* Unlike the per-frame scratch, aurora activity persists across frames
     * (it is low-passed); mark fresh slots "no sample yet". */
    for (int i = s_rs_cap; i < c; i++) s_rs_aur_act[i] = -1.0f;
    s_rs_cap = c;
}

/*
 * field_stars_ensure — (re)build the static field-star VBO when the universe
 * changed.  Fills it from the field-star range [g_field_star_begin,
 * g_field_star_end) with absolute position (AU), colour, and baked absolute
 * magnitude.  Runs once per universe load (guarded by g_universe_generation),
 * never per frame — the field is frozen scenery.
 */
static void field_stars_ensure(void)
{
    if (!s_field_shader) return;
    if (s_field_generation == g_universe_generation) return;   /* already current */
    s_field_generation = g_universe_generation;
    s_field_count = 0;

    int begin = g_field_star_begin, end = g_field_star_end;
    int n = (end > begin) ? end - begin : 0;
    if (n <= 0) return;

    float *buf = (float *)malloc((size_t)n * 8 * sizeof(float));
    if (!buf) return;

    const double R_SUN_M = 6.957e8, M_SUN = 4.83;   /* matches star_dot_apparent_mag */
    int w = 0;
    for (int i = begin; i < end; i++) {
        Body *b = &g_bodies[i];
        if (!b->alive || !b->is_star) continue;
        double Lr = (double)b->radius / R_SUN_M;
        double L  = Lr * Lr;
        if (!(L > 1e-6)) L = 1e-6;
        float absmag = (float)(M_SUN - 2.5 * log10(L));   /* distance-independent */
        buf[w*8+0] = (float)(b->pos[0] * RS);
        buf[w*8+1] = (float)(b->pos[1] * RS);
        buf[w*8+2] = (float)(b->pos[2] * RS);
        buf[w*8+3] = b->col[0];
        buf[w*8+4] = b->col[1];
        buf[w*8+5] = b->col[2];
        buf[w*8+6] = 1.0f;
        buf[w*8+7] = absmag;
        w++;
    }
    s_field_count = w;

    glBindVertexArray(s_field_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_field_vbo);
    if (w > s_field_vbo_cap) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)w * 8 * sizeof(float),
                     NULL, GL_STATIC_DRAW);
        s_field_vbo_cap = w;
    }
    if (w > 0)
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)w * 8 * sizeof(float), buf);
    glBindVertexArray(0);
    free(buf);
    fprintf(stdout, "[Render] field-star VBO: %d stars (GPU static)\n", w);
}

void render_frame(const float view[16], const float proj[16],
                  const float view_rot[16], float dt) {
    float aspect = (float)WIN_W / (float)WIN_H;

    /* Full VP: proj × view (with translation).  Used for rings (nearby geometry). */
    Mat4 vp;
    mat4_mul(vp, proj, view);

    /* Camera-relative VP: proj × view_rot (rotation only, no translation).
     * Used for all far-field geometry (trails, dots, spheres, labels, glare)
     * to eliminate float32 cancellation at large world-space offsets. */
    Mat4 vp_camrel;
    mat4_mul(vp_camrel, proj, view_rot);

    /* Camera basis vectors extracted from view_rot (not the full view matrix).
     * Using the full view matrix here would reintroduce float cancellation —
     * view_rot is kept as a separate pure-rotation matrix for exactly this reason. */
    Vec3 cam_right, cam_up, cam_fwd;
    mat4_get_right(view_rot, cam_right);
    mat4_get_up   (view_rot, cam_up);
    mat4_get_fwd  (view_rot, cam_fwd);

    SupernovaRenderEvent sn_events[SUPERNOVA_MAX_EVENTS];
    int sn_count = supernova_render_events(sn_events, SUPERNOVA_MAX_EVENTS, g_cam.pos);

    /* Sun world position in AU units — used as lighting reference only */
    float sun_wx = (float)(g_bodies[0].pos[0] * RS);
    float sun_wy = (float)(g_bodies[0].pos[1] * RS);
    float sun_wz = (float)(g_bodies[0].pos[2] * RS);

    /* ------------------------------------------------------------------ 1. Starfield */
    /* Stars are on the unit sphere (depth ~= far plane).  Depth test would
     * Z-fight with the cleared depth buffer, so it is disabled for the skybox.
     * The skybox is a direction-only backdrop for the stellar neighbourhood:
     * fade it out as the camera travels to galactic scale (~50 ly → ~5 kly),
     * where the Milky Way volume takes over as the unresolved-star glow. */
    float sf_fade = 1.0f;   /* also feeds the procedural-star crossfade */
    {
        double cd_au = sqrt(g_cam.pos[0]*g_cam.pos[0] +
                            g_cam.pos[1]*g_cam.pos[1] +
                            g_cam.pos[2]*g_cam.pos[2]);
        if (cd_au > 3.0e6) {
            float t = (float)((log10(cd_au) - 6.477) / 2.0);  /* 3e6→3e8 AU */
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            sf_fade = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        starfield_render(view_rot, proj, sf_fade);
        glDepthMask(GL_TRUE);
    }

    /* ------------------------------------------------------------------ 2. Spheres */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    /* Continuous LOD: refresh the density factor that scales the crossfade
     * windows this frame (see the block comment at the top of the file). */
    lod_update_density_scale();
    const float lod_body_lo  = BODY_DOT_FADE_START_PX * s_lod_scale;
    const float lod_body_hi  = BODY_DOT_FADE_END_PX   * s_lod_scale;
    const float lod_glare_lo = STAR_DOT_FULL_GLARE_PX * s_lod_scale;
    const float lod_glare_hi = STAR_DOT_FADE_START_GLARE_PX * s_lod_scale;

    glUseProgram(s_sphere_shader);
    /* vp_camrel: body centre will be passed as camera-relative (u_center = -u_oc)
     * so all vertex positions in phong.vert are free of float32 cancellation. */
    glUniformMatrix4fv(s_sp_vp,       1, GL_FALSE, vp_camrel);
    glUniform1f(s_sp_aspect, aspect);
    glUniform2f(s_sp_screen, (float)WIN_W, (float)WIN_H);
    glUniform3f(s_sp_sun_world,  sun_wx, sun_wy, sun_wz);
    glUniform3f(s_sp_cam_right,  cam_right[0], cam_right[1], cam_right[2]);
    glUniform3f(s_sp_cam_up,     cam_up[0],    cam_up[1],    cam_up[2]);
    glUniform3f(s_sp_cam_fwd,    cam_fwd[0],   cam_fwd[1],   cam_fwd[2]);
    glUniform1f(s_sp_time,       (float)SDL_GetTicks() * 0.001f);

    glBindVertexArray(s_sphere_vao);

    /* BodyRenderInfo[] is also consumed by labels_render() at the end of the frame.
     * Heap-backed scratch grown to g_nbodies (see render_scratch_ensure). */
    render_scratch_ensure(g_nbodies > 0 ? g_nbodies : 1);
    BodyRenderInfo *info = s_rs_info;
    float *body_px = s_rs_body_px;   /* projected radius in pixels; used for fade thresholds */
    memset(body_px, 0, (size_t)g_nbodies * sizeof(float));
    /* Invalidate the per-frame body_lights cache before the sphere pass fills it. */
    memset(s_rs_light_valid, 0, (size_t)g_nbodies * sizeof(char));
    s_rs_nocc = 0;   /* rebuilt below: sphere-rendered non-star occluders */

    /* Dynamic body list for this frame: every body that needs full per-frame CPU
     * treatment (info[], spheres, near dots, glare, labels, picking).  This is
     * all NON-field bodies — the ~JSON stars/planets/moons and build-mode
     * additions — PLUS the few field stars currently within NEAR_DOT_DIST, which
     * are promoted back so they resolve into spheres/labels/picks on approach.
     * The bulk field (frozen, far) is drawn only by the static VBO, so these
     * per-frame loops iterate ~thousands instead of the full ~hundreds of
     * thousands. */
    static int *s_dyn = NULL;      static int s_dyn_cap = 0;
    static int *s_nearf = NULL;    static int s_nearf_cap = 0;
    int dyn_room = g_nbodies + 1;
    if (dyn_room > s_dyn_cap) {
        int cap = s_dyn_cap ? s_dyn_cap : 1024;
        while (cap < dyn_room) cap *= 2;
        s_dyn = realloc(s_dyn, (size_t)cap * sizeof(int));
        if (!s_dyn) { fprintf(stderr, "[render] dyn alloc failed\n"); exit(1); }
        s_dyn_cap = cap;
    }
    int n_dyn = 0;
    for (int i = 0; i < g_field_star_begin && i < g_nbodies; i++) s_dyn[n_dyn++] = i;
    for (int i = g_field_star_end; i < g_nbodies; i++)            s_dyn[n_dyn++] = i;
    if (g_field_star_end > g_field_star_begin) {
        /* Field stars within NEAR_DOT_DIST → promote to the dynamic set. Reuses
         * the movement-gated near-system cache, so this is not a full scan. */
        const int NEARF_MAX = 16384;
        if (s_nearf_cap < NEARF_MAX) {
            s_nearf = realloc(s_nearf, (size_t)NEARF_MAX * sizeof(int));
            if (!s_nearf) { fprintf(stderr, "[render] nearf alloc failed\n"); exit(1); }
            s_nearf_cap = NEARF_MAX;
        }
        double cam_m[3] = { g_cam.pos[0] * AU, g_cam.pos[1] * AU, g_cam.pos[2] * AU };
        double near_r_m = (double)g_settings.near_dot_dist_ly * LY;
        int nn = physics_active_bodies(cam_m, near_r_m, s_nearf, s_nearf_cap);
        for (int j = 0; j < nn; j++) {
            int b = s_nearf[j];
            if (b >= g_field_star_begin && b < g_field_star_end)
                s_dyn[n_dyn++] = b;
        }
    }

    for (int di = 0; di < n_dyn; di++) {
        int i = s_dyn[di];
        Body *b = &g_bodies[i];
        if (!b->alive) {
            memset(&info[i], 0, sizeof(info[i]));
            info[i].show = 1;
            s_rs_sphere_alpha[i] = 0.0f;
            continue;
        }

        float wx = (float)(b->pos[0] * RS);
        float wy = (float)(b->pos[1] * RS);
        float wz = (float)(b->pos[2] * RS);

        /* Camera-relative vector in double precision.
         * Subtraction must happen in double (not float) to retain precision
         * at large world-space offsets (Proxima b is ~241,000 AU from Sol). */
        double dxd = b->pos[0] * RS - g_cam.pos[0];
        double dyd = b->pos[1] * RS - g_cam.pos[1];
        double dzd = b->pos[2] * RS - g_cam.pos[2];
        float dcam = (float)sqrt(dxd*dxd + dyd*dyd + dzd*dzd);

        float dr = visual_radius(i, dcam);

        info[i].pos[0] = wx;
        info[i].pos[1] = wy;
        info[i].pos[2] = wz;
        info[i].dr     = dr;
        info[i].dcam   = dcam;

        /* When the camera is close to the body (within 4 radii), the billboard
         * degenerates at oblique view angles — the sphere centre projects near
         * the camera plane (eye_z ≈ 0) and perspective division produces NaN/Inf.
         * Switch to a fullscreen quad in that range; the fragment shader does
         * the per-pixel ray-sphere intersection independently of vertex layout
         * and produces the correct silhouette from any view direction. */
        int use_fullscreen = (dcam < dr * 4.0f);

        /* eye_z: forward-axis depth.  Used for the dot/sphere pixel-size test.
         * Prefer eye_z over dcam because dcam = eye_z/cos(θ) oscillates with
         * camera rotation; eye_z is invariant under pure rotation. */
        float eye_z = (float)(dxd*cam_fwd[0] + dyd*cam_fwd[1] + dzd*cam_fwd[2]);
        float px;
        if (use_fullscreen) {
            /* Suppress the dot (large px) and force the sphere to render below. */
            px = 1000.0f;
        } else {
            px = (eye_z > 0.0f)
                 ? (WIN_H / 2.0f) * dr / (eye_z * half_fov_tan() + 1e-9f)
                 : 0.0f;
        }
        /* Threshold: sphere first appears when its diameter (2×px) equals the dot
         * diameter (2.5px), i.e. px = 1.25 = BODY_SPHERE_APPEAR_PX. */
        body_px[i]   = px;
        info[i].show = (!use_fullscreen && px < BODY_SPHERE_APPEAR_PX) ? 1 : 0;

        /* A body being tidally shredded must stay visible even when it would
         * normally be a sub-pixel far-field dot (which can't show the stretch):
         * force it to a sphere with a minimum on-screen size.  The shed debris
         * spreads out and glows far brighter than the intact planet, so drawing
         * it larger than its shrinking body is physically fair, not a cheat. */
        if (b->tidal_frac > 0.0f) {
            const float TIDAL_MIN_PX = 8.0f;
            if (px < TIDAL_MIN_PX && eye_z > 0.0f) {
                float dr_min = TIDAL_MIN_PX * eye_z * half_fov_tan() / (WIN_H / 2.0f);
                if (dr_min > dr) { dr = dr_min; info[i].dr = dr; }
            }
            info[i].show = 0;
        }

        /* Continuous LOD blend: the sphere fades in over the (density-scaled)
         * pixel window the dot fades out over — the dot pass uses the exact
         * complement of this value, so the handoff conserves brightness. */
        {
            float a = (float)smoothstepd(lod_body_lo, lod_body_hi, px);
            if (b->tidal_frac > 0.0f) a = 1.0f;   /* shredding: always a sphere */
            s_rs_sphere_alpha[i] = a;
        }

        if (!g_bodies[i].alive || s_rs_sphere_alpha[i] <= 0.0f) continue;
        /* Black holes are raymarched in their own pass.  Stars DO take the
         * sphere path (emissive branch: limb darkening, granulation, spots) —
         * the old "glare only" skip meant a resolved star had no photosphere
         * at all, which is why starspots never showed (the glare billboard
         * now clears its wash off the disc face via u_resolve). */
        if (b->is_black_hole) continue;

        /* This body renders as a Phong sphere, so it can occlude dots behind
         * it.  Only a fully-resolved (opaque) sphere occludes — a transitioning
         * mostly-transparent one must not swallow the dots behind it. */
        if (s_rs_sphere_alpha[i] >= 1.0f)
            s_rs_occluders[s_rs_nocc++] = i;

        /* u_oc: camera − body, computed in double then cast to float.
         * u_center = −u_oc so the phong.vert billboard is camera-relative. */
        double cam_mx = (double)g_cam.pos[0] * AU;
        double cam_my = (double)g_cam.pos[1] * AU;
        double cam_mz = (double)g_cam.pos[2] * AU;
        float oc_x = (float)((cam_mx - b->pos[0]) * RS);
        float oc_y = (float)((cam_my - b->pos[1]) * RS);
        float oc_z = (float)((cam_mz - b->pos[2]) * RS);

        /* Lighting: the RadianceField's two brightest emitters at this body —
         * the sources whose incident flux actually wins here (so a binary
         * companion or an active black hole's disk lights correctly, not just
         * the parent chain's root).  Falls back to the root-star walk if the
         * field is empty (e.g. a universe with no stars). */
        float sr1[3], col1[3], sr2[3], w2, col2[3];
        body_lights(i, b, sr1, col1, sr2, &w2, col2);
        /* Cache for the atmosphere pass (same body, same lighting). */
        {
            BodyLightCache *lc = &s_rs_light[i];
            for (int k = 0; k < 3; k++) {
                lc->sr1[k] = sr1[k]; lc->col1[k] = col1[k];
                lc->sr2[k] = sr2[k]; lc->col2[k] = col2[k];
            }
            lc->w2 = w2;
            s_rs_light_valid[i] = 1;
        }

        glUniform3f(s_sp_center,  -oc_x, -oc_y, -oc_z);
        glUniform1f(s_sp_radius,   dr);
        glUniform3f(s_sp_oc,       oc_x, oc_y, oc_z);
        glUniform3f(s_sp_sun_rel,  sr1[0], sr1[1], sr1[2]);
        glUniform3f(s_sp_sun_col,  col1[0], col1[1], col1[2]);
        glUniform3f(s_sp_sun2_rel, sr2[0], sr2[1], sr2[2]);
        glUniform1f(s_sp_light2,   w2);
        glUniform3f(s_sp_light2_col, col2[0], col2[1], col2[2]);
        glUniform3f(s_sp_color,    b->col[0], b->col[1], b->col[2]);
        glUniform1f(s_sp_emission, b->is_star ? 1.0f : 0.0f);
        glUniform1f(s_sp_ambient,  b->is_star ? 1.0f : 0.05f);
        glUniform1f(s_sp_rotation,        (float)fmod(b->rotation_angle, 2.0 * PI));
        glUniform1f(s_sp_cloud_rotation,  (float)b->cloud_rotation);
        glUniform1f(s_sp_obliquity, (float)(b->obliquity * (PI / 180.0)));
        int ptype = get_planet_type(b->name);
        glUniform1i(s_sp_ptype,     ptype);
        /* Cloud coverage is data-driven: solid worlds with an authored
         * atmosphere get a procedural deck scaled by its intensity.  Gas
         * giants / Venus / Titan already ARE cloud recipes — excluded. */
        {
            int solid = (ptype == 0 || ptype == 1 || ptype == 2 ||
                         ptype == 7 || ptype == 9 || ptype == 10);
            float cloud_amt = solid ? b->atm_intensity : 0.0f;
            if (cloud_amt > 1.0f) cloud_amt = 1.0f;
            if (cloud_amt < 0.0f) cloud_amt = 0.0f;
            glUniform1f(s_sp_cloud_amount, cloud_amt);
        }
        /* Ring shadow: ringed planets get the annulus striped across the
         * globe by the sphere shader (radii/pole from the ring system). */
        {
            float rg_in, rg_out, rg_pole[3];
            if (rings_query(i, &rg_in, &rg_out, rg_pole)) {
                glUniform4f(s_sp_ring, rg_in, rg_out, 0.85f, 0.0f);
                glUniform3f(s_sp_ring_pole, rg_pole[0], rg_pole[1], rg_pole[2]);
            } else {
                glUniform4f(s_sp_ring, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
        /* Eclipse occluders: family bodies (parent, children, siblings) that
         * can block the sun.  Family-only keeps the scan O(N) int compares —
         * it covers every physically plausible eclipse (a foreign system's
         * body subtends nothing at interstellar range). */
        {
            float ecl[6 * 4];
            int   necl = 0;
            for (int j = 0; j < g_nbodies && necl < 6; j++) {
                /* Skip the frozen field-star range wholesale — like every other
                 * hot per-body loop (trails/labels/rings/inspect).  A field star
                 * is never a plausible eclipse occluder, and scanning ~10^5-10^6
                 * of them per sphere is the dominant cost of this pass. */
                if (j >= g_field_star_begin && j < g_field_star_end) {
                    j = g_field_star_end - 1;
                    continue;
                }
                if (j == i || !g_bodies[j].alive) continue;
                if (g_bodies[j].is_star) continue;
                if (j != b->parent && g_bodies[j].parent != i &&
                    (b->parent < 0 || g_bodies[j].parent != b->parent))
                    continue;
                float orad = visual_radius(j, 0.0f);
                if (orad <= 0.0f) continue;
                ecl[necl*4+0] = (float)((g_bodies[j].pos[0] - b->pos[0]) * RS);
                ecl[necl*4+1] = (float)((g_bodies[j].pos[1] - b->pos[1]) * RS);
                ecl[necl*4+2] = (float)((g_bodies[j].pos[2] - b->pos[2]) * RS);
                ecl[necl*4+3] = orad;
                necl++;
            }
            glUniform1i(s_sp_ecl_count, necl);
            if (necl > 0) {
                glUniform4fv(s_sp_ecl, necl, ecl);
                /* Penumbra size: the primary emitter's disc radius. */
                RadianceContrib etop[1];
                float sun_r = 0.0f;
                if (radiance_field_top(b->pos, i, 1, etop) >= 1 &&
                    etop[0].body >= 0)
                    sun_r = visual_radius(etop[0].body, 0.0f);
                glUniform1f(s_sp_sun_radius, sun_r);
            }
        }
        glUniform1f(s_sp_star_heat, collision_body_star_heat(i));
        glUniform1f(s_sp_starspots, b->is_star ? (float)g_settings.starspots : 0.0f);

        /* Upload per-body collision impact spots (craters/ejecta) for the shader */
        {
            CollisionSpot spots[COLLISION_MAX_SPOTS];
            float dirs[COLLISION_MAX_SPOTS * 3] = {0};
            float tangents[COLLISION_MAX_SPOTS * 3] = {0};
            float radii[COLLISION_MAX_SPOTS] = {0};
            float heats[COLLISION_MAX_SPOTS] = {0};
            float progress[COLLISION_MAX_SPOTS] = {0};
            float seeds[COLLISION_MAX_SPOTS] = {0};
            int kinds[COLLISION_MAX_SPOTS] = {0};
            int nspots = collision_spots_for_body(i, spots);
            for (int k = 0; k < nspots; k++) {
                dirs[k*3+0] = spots[k].dir[0];
                dirs[k*3+1] = spots[k].dir[1];
                dirs[k*3+2] = spots[k].dir[2];
                tangents[k*3+0] = spots[k].tangent1[0];
                tangents[k*3+1] = spots[k].tangent1[1];
                tangents[k*3+2] = spots[k].tangent1[2];
                radii[k] = spots[k].angular_radius;
                heats[k] = spots[k].heat;
                progress[k] = spots[k].progress;
                seeds[k] = spots[k].seed;
                kinds[k] = spots[k].kind;
            }
            glUniform1i(s_sp_impact_count, nspots);
            glUniform3fv(s_sp_impact_dir, nspots, dirs);
            glUniform3fv(s_sp_impact_t1, nspots, tangents);
            glUniform1fv(s_sp_impact_rad, nspots, radii);
            glUniform1fv(s_sp_impact_heat, nspots, heats);
            glUniform1fv(s_sp_impact_prog, nspots, progress);
            glUniform1fv(s_sp_impact_seed, nspots, seeds);
            glUniform1iv(s_sp_impact_kind, nspots, kinds);
        }

        glUniform1i(s_sp_use_fullscreen, use_fullscreen);

        /* Tidal disruption: stretch the body into a strand pointing at the hole
         * (elongate along the radial line, squash across it) and add a hot glow. */
        if (b->tidal_frac > 0.0f && b->tidal_hole >= 0 &&
            b->tidal_hole < g_nbodies && g_bodies[b->tidal_hole].alive) {
            Body *hole = &g_bodies[b->tidal_hole];
            float sx = (float)((hole->pos[0] - b->pos[0]) * RS);
            float sy = (float)((hole->pos[1] - b->pos[1]) * RS);
            float sz = (float)((hole->pos[2] - b->pos[2]) * RS);
            float len = sqrtf(sx*sx + sy*sy + sz*sz);
            if (len > 1e-9f) { sx/=len; sy/=len; sz/=len; }
            else             { sx=0.0f; sy=0.0f; sz=1.0f; }
            float tf = b->tidal_frac;
            glUniform3f(s_sp_stretch_dir,   sx, sy, sz);
            glUniform1f(s_sp_stretch_along, 1.0f + 3.0f * tf);   /* up to 4× long */
            glUniform1f(s_sp_stretch_perp,  1.0f - 0.55f * tf);  /* thins to ~0.45× */
            glUniform1f(s_sp_tidal_glow,    tf);
        } else {
            glUniform3f(s_sp_stretch_dir,   0.0f, 0.0f, 1.0f);
            glUniform1f(s_sp_stretch_along, 1.0f);
            glUniform1f(s_sp_stretch_perp,  1.0f);
            glUniform1f(s_sp_tidal_glow,    0.0f);
        }

        /* Continuous LOD: a transitioning sphere alpha-blends over its own
         * fading dot and must not write depth (a near-transparent disc would
         * depth-kill the dot and other points behind it).  Fully-resolved
         * spheres take the normal opaque, depth-writing path.  The transition
         * set is tiny (bodies within a ~1px window), so the state churn is
         * negligible. */
        {
            float op = s_rs_sphere_alpha[i];
            glUniform1f(s_sp_opacity, op);
            if (op < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
            }
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            if (op < 1.0f) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
        }
    }
    glBindVertexArray(0);

    inspect_pick_center(vp_camrel, info);

    /* ------------------------------------------------------------------ 2.5. Atmosphere glow
     * Separate additive pass: GL_SRC_ALPHA / GL_ONE, depth-tested but no depth write.
     * The glow billboard radius is planet_radius × atm_scale.
     * Collision heat glow from the collision system is blended additively with the
     * base atmosphere color using a weighted average (intensity as weight). */
    if (s_atm_shader) {
        glUseProgram(s_atm_shader);
        glUniform1f(s_at_aspect, aspect);
        glUniform2f(s_at_screen, (float)WIN_W, (float)WIN_H);
        glUniformMatrix4fv(s_at_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_at_cam_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_at_cam_up,    cam_up[0],    cam_up[1],    cam_up[2]);
        glUniform3f(s_at_cam_fwd,   cam_fwd[0],   cam_fwd[1],   cam_fwd[2]);
        float at_now = (float)SDL_GetTicks() * 0.001f;
        glUniform1f(s_at_time, at_now);
        /* Real-clock frame dt for the aurora-activity low-pass below. */
        static float s_at_prev = -1.0f;
        float at_rdt = (s_at_prev >= 0.0f && at_now > s_at_prev)
                       ? at_now - s_at_prev : 0.033f;
        s_at_prev = at_now;
        /* Aurora look tunables (live g_settings, shared by every body). */
        glUniform4f(s_at_aur_shape,
                    sinf(g_settings.aurora_oval_lat * (float)(PI / 180.0)),
                    g_settings.aurora_oval_width,
                    0.020f * g_settings.aurora_storm_expand,
                    0.010f * g_settings.aurora_storm_expand);
        glUniform3f(s_at_aur_look, g_settings.aurora_gain,
                    g_settings.aurora_red, g_settings.aurora_violet);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);   /* additive: glows accumulate */
        glDepthMask(GL_FALSE);               /* read depth for behind-planet cull */
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(s_sphere_vao);

        for (int di = 0; di < n_dyn; di++) {
            int i = s_dyn[di];
            if (!g_bodies[i].alive) continue;
            /* Continuous LOD: the glow fades in with the sphere (same blend)
             * instead of snapping on the instant the body resolves. */
            float lod_a = s_rs_sphere_alpha[i];
            if (lod_a <= 0.0f) continue;   /* still a dot — no sphere, no glow */
            float intensity = g_bodies[i].atm_intensity;
            float scale     = g_bodies[i].atm_scale;
            float glow_color[3];
            float glow_intensity = 0.0f;
            float glow_scale = 1.0f;
            float final_color[3];
            float final_intensity;
            float final_scale;
            collision_body_heat_glow(i, glow_color, &glow_intensity, &glow_scale);
            final_intensity = (intensity + glow_intensity) * lod_a;
            final_scale = glow_scale > scale ? glow_scale : scale;
            if (final_intensity <= 0.0f) continue;
            {
                /* Weighted average of base atm color and heat glow color */
                float base_w = intensity;
                float glow_w = glow_intensity;
                float sum_w = base_w + glow_w;
                if (sum_w <= 1e-6f) sum_w = 1.0f;
                final_color[0] = (g_bodies[i].atm_color[0] * base_w + glow_color[0] * glow_w) / sum_w;
                final_color[1] = (g_bodies[i].atm_color[1] * base_w + glow_color[1] * glow_w) / sum_w;
                final_color[2] = (g_bodies[i].atm_color[2] * base_w + glow_color[2] * glow_w) / sum_w;
            }

            Body *b = &g_bodies[i];
            double cam_mx = (double)g_cam.pos[0] * AU;
            double cam_my = (double)g_cam.pos[1] * AU;
            double cam_mz = (double)g_cam.pos[2] * AU;
            float oc_x = (float)((cam_mx - b->pos[0]) * RS);
            float oc_y = (float)((cam_my - b->pos[1]) * RS);
            float oc_z = (float)((cam_mz - b->pos[2]) * RS);
            /* Same two-light RadianceField lighting as the sphere pass, so
             * the atmosphere's day side agrees with the surface's — reuse the
             * value the sphere pass already computed for this body this frame
             * (a resolved-body atmosphere always has a matching sphere pass;
             * fall back to a fresh query only if the sphere pass skipped it,
             * e.g. a black hole with a heat glow). */
            float sr1[3], col1[3], sr2[3], w2, col2[3];
            if (s_rs_light_valid[i]) {
                const BodyLightCache *lc = &s_rs_light[i];
                for (int k = 0; k < 3; k++) {
                    sr1[k] = lc->sr1[k]; col1[k] = lc->col1[k];
                    sr2[k] = lc->sr2[k]; col2[k] = lc->col2[k];
                }
                w2 = lc->w2;
            } else {
                body_lights(i, b, sr1, col1, sr2, &w2, col2);
            }

            float planet_r = info[i].dr;
            float atm_r    = planet_r * final_scale;

            /* Aurora strength: a PHYSICAL proxy, no per-body art tags.
             * Dynamo ∝ rotation rate × mass (fast spin + big metallic core
             * ⇒ field; Venus' 243-day spin or a tidally-locked moon ⇒ ~0),
             * driven by the stellar wind ∝ √(incident flux from the dominant
             * RadianceField emitter).  Earth-normalised so Earth ≈ 1; gas
             * giants clamp (Jupiter's real aurora is brighter but a screen
             * full of neon is not the goal).  Promoted exoplanets inherit
             * whatever their rotation/mass/flux imply — same philosophy as
             * cloud decks and comet activity. */
            float aur = 0.0f;
            if (!b->is_star && intensity > 0.0f && b->rotation_rate != 0.0
                && g_settings.aurora_gain > 0.0f) {
                RadianceContrib atop[1];
                if (radiance_field_top(b->pos, i, 1, atop) >= 1
                    && atop[0].irr > 0.0) {
                    double dynamo = fabs(b->rotation_rate) / 7.292e-5
                                  * (b->mass / 5.972e24);
                    if (dynamo > 1.6) dynamo = 1.6;
                    double wind = sqrt(atop[0].irr / 1361.0);
                    if (wind > 1.3) wind = 1.3;
                    /* Storm gusting runs on the sim clock; frames run on the
                     * real clock.  At high sim rates the substorm octaves
                     * cycle many times per frame — the low-pass makes the
                     * oval breathe through them instead of strobing. */
                    float tau = g_settings.aurora_smooth_s;
                    float target = (float)(dynamo * wind
                                           * aurora_storm(&atop[0], b));
                    float *sm = &s_rs_aur_act[i];
                    if (*sm < 0.0f || tau <= 0.0f) *sm = target;
                    else *sm += (target - *sm) * (at_rdt / (at_rdt + tau));
                    aur = *sm * lod_a;
                    if (aur < 0.02f) aur = 0.0f;
                }
            }
            {
                /* Spin axis in GL world space: ecliptic north = GL +Y,
                 * obliquity tilts about GL X (same frame as the sphere and
                 * accretion-disk passes). */
                double ob = b->obliquity * (PI / 180.0);
                glUniform4f(s_at_aurora, 0.0f, (float)cos(ob), (float)sin(ob),
                            aur);
            }

            glUniform3f(s_at_center,   -oc_x, -oc_y, -oc_z);
            glUniform1f(s_at_radius,    atm_r);
            glUniform1f(s_at_planet_r,  planet_r);
            glUniform3f(s_at_oc,        oc_x, oc_y, oc_z);
            glUniform3f(s_at_sun_rel,   sr1[0], sr1[1], sr1[2]);
            glUniform3f(s_at_sun_col,   col1[0], col1[1], col1[2]);
            glUniform3f(s_at_sun2_rel,  sr2[0], sr2[1], sr2[2]);
            glUniform1f(s_at_light2,    w2);
            glUniform3f(s_at_light2_col, col2[0], col2[1], col2[2]);
            glUniform3f(s_at_color,     final_color[0], final_color[1], final_color[2]);
            glUniform1f(s_at_intensity, final_intensity);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* ------------------------------------------------------------------ 2.65. Galaxies (volumetric) */
    /* Real-position volumetric galaxies (Layer 4.2). Drawn before the nebulae:
     * both blend "over" without depth writes, and galaxies are the more
     * distant translucents, so back-to-front order keeps overlaps correct.
     *
     * The Milky Way volume makes this raymarch fullscreen in every in-galaxy
     * scene, so like the supernova cloud it renders into the half-res target
     * (¼ the fragments) and composites back. Unlike the cloud it stays
     * depth-correct: the shader clips its march to the opaque scene's depth
     * texture (post_scene_depth_tex). Falls back to the direct full-res
     * depth-tested draw when post/bloom is off (no sampleable depth). */
    {
        GLuint gal_depth   = (GLuint)post_scene_depth_tex();
        int    use_halfres = (s_vol_composite_shader != 0 && s_vol_quad_vao != 0
                              && gal_depth != 0);
        GLint  prev_fbo = 0;
        int    gal_w = WIN_W, gal_h = WIN_H;

        if (use_halfres) {
            /* Scene target is known (post's HDR FBO, or 0) — no glGet
             * round-trip; glClearBufferfv clears without touching the global
             * clear colour, so no save/restore either. */
            static const GLfloat clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            prev_fbo = (GLint)post_scene_fbo();
            vol_target_ensure(0);
            glBindFramebuffer(GL_FRAMEBUFFER, s_vol_fbo[0]);
            glViewport(0, 0, s_vol_w[0], s_vol_h[0]);
            glClearBufferfv(GL_COLOR, 0, clear0);
            gal_w = s_vol_w[0];
            gal_h = s_vol_h[0];
        }

        galaxy_render(vp_camrel, cam_right, cam_up, cam_fwd, g_cam.pos,
                      tanf(FOV * 0.5f * (float)(PI / 180.0)), aspect,
                      gal_w, gal_h, (float)SDL_GetTicks() * 0.001f,
                      use_halfres ? gal_depth : 0);

        if (use_halfres) {
            /* Upscale + composite the half-res galaxy layer over the scene. */
            glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
            glViewport(0, 0, WIN_W, WIN_H);
            glUseProgram(s_vol_composite_shader);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_vol_color[0]);
            glUniform1i(s_vol_comp_tex, 0);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisable(GL_DEPTH_TEST);
            glBindVertexArray(s_vol_quad_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
        }
    }

    /* Procedural resolved stars (§0.1 galaxy → stars): additive sparkle
     * following the same density model as the glow above, crossfaded in as
     * the painted neighbourhood skybox fades out. Always full-res (cheap
     * points, correct depth test against opaque geometry). */
    galaxy_render_stars(vp_camrel, g_cam.pos, 1.0f - sf_fade,
                        (float)SDL_GetTicks() * 0.001f);

    /* ------------------------------------------------------------------ 2.7. Nebulae (volumetric) */
    /* Real-position volumetric clouds; depth-tested against opaque geometry so
     * planets/stars occlude or embed correctly. Drawn camera-relative. */
    nebula_render(vp_camrel, cam_right, cam_up, cam_fwd, g_cam.pos,
                  tanf(FOV * 0.5f * (float)(PI / 180.0)), aspect,
                  WIN_W, WIN_H);

    /* ------------------------------------------------------------------ 2.75. Supernova cloud */
    /* The cloud raymarch is fragment-bound and fills the screen when the camera
     * is near the blast. Render it into a half-res target (¼ the fragments) and
     * composite it back over the scene; fall back to a direct full-res draw if
     * the composite shader is unavailable. */
    /* Quarter-res slot when any blast volume surrounds the camera (the
     * fullscreen-raster state): the whole screen marches the volume, and a
     * screen-filling smoke layer has no edge detail to lose — this is exactly
     * the state that otherwise dominates the frame. Both supernova passes use
     * the same slot so they upsample consistently. */
    int sn_vol_slot = 0;
    for (int i = 0; i < sn_count; i++) {
        const SupernovaRenderEvent *e = &sn_events[i];
        float cover = e->cloud_radius > e->flash_radius ? e->cloud_radius
                                                        : e->flash_radius;
        if (supernova_fullscreen_raster_local(e->pos, cam_fwd, cover, 1.34f)) {
            sn_vol_slot = 1;
            break;
        }
    }

    if (sn_count > 0 && s_supernova_cloud_shader) {
        int   use_halfres = (s_vol_composite_shader != 0 && s_vol_quad_vao != 0);
        GLint prev_fbo = 0;
        float screen_w = (float)WIN_W, screen_h = (float)WIN_H;

        if (use_halfres) {
            /* Same no-glGet redirect as the galaxy half-res block above. */
            static const GLfloat clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            prev_fbo = (GLint)post_scene_fbo();
            vol_target_ensure(sn_vol_slot);
            glBindFramebuffer(GL_FRAMEBUFFER, s_vol_fbo[sn_vol_slot]);
            glViewport(0, 0, s_vol_w[sn_vol_slot], s_vol_h[sn_vol_slot]);
            glClearBufferfv(GL_COLOR, 0, clear0);
            screen_w = (float)s_vol_w[sn_vol_slot];
            screen_h = (float)s_vol_h[sn_vol_slot];
        }

        glUseProgram(s_supernova_cloud_shader);
        glUniformMatrix4fv(s_sn_cloud_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_sn_cloud_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_sn_cloud_up, cam_up[0], cam_up[1], cam_up[2]);
        glUniform3f(s_sn_cloud_fwd, cam_fwd[0], cam_fwd[1], cam_fwd[2]);
        glUniform1f(s_sn_cloud_fov_tan, tanf(FOV * 0.5f * (float)(PI / 180.0)));
        glUniform1f(s_sn_cloud_aspect, aspect);
        glUniform2f(s_sn_cloud_screen, screen_w, screen_h);

        glEnable(GL_BLEND);
        /* Premultiplied "over": matches the cloud shader's premultiplied output
         * and is identical on-screen to the old SRC_ALPHA blend for a single
         * layer, while letting the layer be composited from the half-res target. */
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        /* Half-res layer carries no scene depth, so it isn't depth-tested; the
         * full-res fallback keeps testing so planets occlude the cloud. */
        if (use_halfres) glDisable(GL_DEPTH_TEST);
        else             glEnable(GL_DEPTH_TEST);
        glBindVertexArray(s_sphere_vao);

        for (int i = 0; i < sn_count; i++) {
            const SupernovaRenderEvent *e = &sn_events[i];
            float cloud_bill_scale = 1.34f;
            float dist;
            float dist_fade;
            float cloud_density;
            int fullscreen_raster;
            if (e->cloud_intensity <= 0.00005f || e->cloud_radius <= 0.0f) continue;

            dist = sqrtf(e->pos[0]*e->pos[0] + e->pos[1]*e->pos[1] + e->pos[2]*e->pos[2]);
            dist_fade = supernova_distance_fade_local(dist, e->cloud_radius);
            cloud_density = e->cloud_intensity * dist_fade;
            if (cloud_density <= 0.00005f) continue;

            fullscreen_raster = supernova_fullscreen_raster_local(e->pos, cam_fwd,
                                                                  e->cloud_radius,
                                                                  cloud_bill_scale)
                             || supernova_far_raster_local(e->pos, cam_fwd,
                                                           e->cloud_radius,
                                                           cloud_bill_scale);
            glUniform1f(s_sn_cloud_fullscreen, fullscreen_raster ? 1.0f : 0.0f);
            glUniform3f(s_sn_cloud_center, e->pos[0], e->pos[1], e->pos[2]);
            glUniform1f(s_sn_cloud_radius, e->cloud_radius);
            glUniform3f(s_sn_cloud_oc, -e->pos[0], -e->pos[1], -e->pos[2]);
            glUniform3f(s_sn_cloud_color, e->color[0], e->color[1], e->color[2]);
            glUniform1f(s_sn_cloud_inner,
                        e->cloud_radius > 1e-6f ? e->cloud_inner_radius / e->cloud_radius : 0.72f);
            glUniform1f(s_sn_cloud_density, cloud_density);
            glUniform1f(s_sn_cloud_hot, e->hot_shell_intensity * dist_fade);
            glUniform1f(s_sn_cloud_time, e->time_days);
            glUniform1f(s_sn_cloud_seed, e->seed);
            glUniform1f(s_sn_cloud_bill, cloud_bill_scale);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);

        if (use_halfres) {
            /* Upscale + composite the half-res cloud back over the scene. */
            glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
            glViewport(0, 0, WIN_W, WIN_H);
            glUseProgram(s_vol_composite_shader);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_vol_color[sn_vol_slot]);
            glUniform1i(s_vol_comp_tex, 0);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBindVertexArray(s_vol_quad_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }

    /* The core/flash raymarch is as fragment-bound as the cloud (18 steps ×
     * 5 FBM) and used to draw fullscreen at FULL resolution — when the camera
     * sits inside the expanding flash sphere it alone dominated the frame
     * (~4× the cloud's cost). Render it into the same half-res target and
     * composite it back additively: its blend (SRC_ALPHA, ONE) is linear, so
     * summing layers into a black texture and adding the texture to the scene
     * is exactly equivalent, minus the depth test the cloud pass already
     * trades away at half res. */
    if (sn_count > 0 && s_supernova_core_shader) {
        int   use_halfres = (s_vol_composite_shader != 0 && s_vol_quad_vao != 0);
        GLint prev_fbo = 0;
        float screen_w = (float)WIN_W, screen_h = (float)WIN_H;

        if (use_halfres) {
            /* Same no-glGet redirect as the galaxy half-res block above. */
            static const GLfloat clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            prev_fbo = (GLint)post_scene_fbo();
            vol_target_ensure(sn_vol_slot);
            glBindFramebuffer(GL_FRAMEBUFFER, s_vol_fbo[sn_vol_slot]);
            glViewport(0, 0, s_vol_w[sn_vol_slot], s_vol_h[sn_vol_slot]);
            glClearBufferfv(GL_COLOR, 0, clear0);
            screen_w = (float)s_vol_w[sn_vol_slot];
            screen_h = (float)s_vol_h[sn_vol_slot];
        }

        glUseProgram(s_supernova_core_shader);
        glUniformMatrix4fv(s_sn_core_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_sn_core_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_sn_core_up, cam_up[0], cam_up[1], cam_up[2]);
        glUniform3f(s_sn_core_fwd, cam_fwd[0], cam_fwd[1], cam_fwd[2]);
        glUniform1f(s_sn_core_fov_tan, tanf(FOV * 0.5f * (float)(PI / 180.0)));
        glUniform1f(s_sn_core_aspect, aspect);
        glUniform2f(s_sn_core_screen, screen_w, screen_h);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        /* Half-res layer carries no scene depth (see cloud pass note); the
         * full-res fallback keeps testing so planets occlude the fireball. */
        if (use_halfres) glDisable(GL_DEPTH_TEST);
        else             glEnable(GL_DEPTH_TEST);
        glBindVertexArray(s_sphere_vao);

        for (int i = 0; i < sn_count; i++) {
            const SupernovaRenderEvent *e = &sn_events[i];
            float dist = sqrtf(e->pos[0]*e->pos[0] + e->pos[1]*e->pos[1] + e->pos[2]*e->pos[2]);
            float dist_fade;
            float core_bill_scale;
            int fullscreen_raster;
            float coverage_radius;
            float flash_intensity;
            float core_intensity;

            if (e->flash_intensity <= 0.00005f && e->core_intensity <= 0.00005f)
                continue;

            dist_fade = supernova_distance_fade_local(dist,
                        e->cloud_radius > e->flash_radius ? e->cloud_radius : e->flash_radius);
            flash_intensity = e->flash_intensity * dist_fade;
            core_intensity = e->core_intensity * dist_fade;
            if (flash_intensity <= 0.00005f && core_intensity <= 0.00005f)
                continue;

            if (dist > e->flash_radius * 1.01f) {
                float denom = sqrtf(fmaxf(dist * dist - e->flash_radius * e->flash_radius, 1e-6f));
                core_bill_scale = dist / denom;
            } else {
                core_bill_scale = 8.0f;
            }
            core_bill_scale = clampf_local(core_bill_scale * 1.10f, 1.18f, 8.0f);
            coverage_radius = e->cloud_radius > e->flash_radius ? e->cloud_radius : e->flash_radius;
            fullscreen_raster = supernova_fullscreen_raster_local(e->pos, cam_fwd,
                                                                  coverage_radius,
                                                                  core_bill_scale)
                             || supernova_far_raster_local(e->pos, cam_fwd,
                                                           coverage_radius,
                                                           core_bill_scale);
            glUniform1f(s_sn_core_fullscreen, fullscreen_raster ? 1.0f : 0.0f);
            glUniform3f(s_sn_core_center, e->pos[0], e->pos[1], e->pos[2]);
            glUniform1f(s_sn_core_radius, e->flash_radius);
            glUniform3f(s_sn_core_oc, -e->pos[0], -e->pos[1], -e->pos[2]);
            glUniform3f(s_sn_core_color, e->color[0], e->color[1], e->color[2]);
            glUniform1f(s_sn_core_flash, flash_intensity);
            glUniform1f(s_sn_core_core, core_intensity);
            glUniform1f(s_sn_core_ratio,
                        e->flash_radius > 1e-6f ? e->core_radius / e->flash_radius : 0.32f);
            glUniform1f(s_sn_core_time, e->time_days);
            glUniform1f(s_sn_core_seed, e->seed);
            glUniform1f(s_sn_core_bill, core_bill_scale);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);

        if (use_halfres) {
            /* Upscale + add the half-res glow layer over the scene (the target
             * already holds accumColor·accumAlpha summed, so plain addition
             * reproduces the direct SRC_ALPHA/ONE draw). */
            glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
            glViewport(0, 0, WIN_W, WIN_H);
            glUseProgram(s_vol_composite_shader);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_vol_color[sn_vol_slot]);
            glUniform1i(s_vol_comp_tex, 0);
            glBlendFunc(GL_ONE, GL_ONE);
            glBindVertexArray(s_vol_quad_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }

    /* ------------------------------------------------------------------ 3. Collision particles */
    {
        CollisionParticle particles[RENDER_MAX_COLLISION_PARTICLES];
        float particle_data[RENDER_MAX_COLLISION_PARTICLES * 8];
        int particle_count = collision_particles(particles, RENDER_MAX_COLLISION_PARTICLES,
                                                 g_cam.pos);

        for (int i = 0; i < particle_count; i++) {
            particle_data[i*8+0] = particles[i].pos[0];
            particle_data[i*8+1] = particles[i].pos[1];
            particle_data[i*8+2] = particles[i].pos[2];
            particle_data[i*8+3] = particles[i].color[0];
            particle_data[i*8+4] = particles[i].color[1];
            particle_data[i*8+5] = particles[i].color[2];
            particle_data[i*8+6] = particles[i].color[3];
            particle_data[i*8+7] = particles[i].size;
        }

        if (particle_count > 0) {
            glUseProgram(s_impact_particle_shader);
            glUniformMatrix4fv(s_impact_particle_vp, 1, GL_FALSE, vp_camrel);
            glBindVertexArray(s_impact_particle_vao);
            glBindBuffer(GL_ARRAY_BUFFER, s_impact_particle_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            particle_count * 8 * sizeof(float), particle_data);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);   /* additive ejecta */
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_PROGRAM_POINT_SIZE);
            glDrawArrays(GL_POINTS, 0, particle_count);
            glDisable(GL_PROGRAM_POINT_SIZE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glBindVertexArray(0);
        }
    }

    /* ------------------------------------------------------------------ 3. Center dots
     *
     * Priority order for overlap resolution: stars first, then planets (sorted
     * by dcam), then moons (sorted by dcam).  Stars must beat planets so that
     * at interstellar distances (e.g. Proxima b nearly coincides with Proxima
     * Centauri on screen from Sol) the star dot reliably wins the screen slot.
     *
     * Overlap removal is greedy: iterate in priority order; a dot is visible
     * only if its screen position is ≥ DOT_EXCL_PX away from all previously
     * confirmed visible dots.  Between DOT_HIDE_PX and DOT_EXCL_PX the alpha
     * is smoothstep-faded so the handoff is gradual rather than binary.
     *
     * IMPORTANT: positions are computed as camera-relative doubles cast to
     * float, used with vp_camrel (no translation).  Using the full float VP
     * with float world positions loses 4-5 digits at 4+ ly → visible jitter. */
#define DOT_EXCL_PX (g_settings.dot_excl_px)  /* fully separated above this screen distance */
#define DOT_HIDE_PX (g_settings.dot_hide_px)  /* fully hidden when centers are this close */

    /* Near/far split (Phase 3 — galaxy-scale far field).
     * Only bodies within NEAR_DOT_DIST of the camera get the full per-dot
     * treatment that follows: priority sort, greedy overlap dedup, glare-corona
     * occlusion and the dot<->sphere transition fade.  That work is
     * O(near^2) + O(near x stars), which only the system the camera is actually
     * in benefits from.  Everything beyond NEAR_DOT_DIST — the 16k-body bulk —
     * is drawn afterwards in one cheap O(N) far pass, so a galaxy no longer pays
     * O(N^2) per frame just to place star points that are a static backdrop.
     * NEAR_DOT_DIST is a few light-years (world units are AU, RS = 1/AU) so it
     * comfortably spans the active system and its nearest neighbours. */
    const float NEAR_DOT_DIST = (float)((double)g_settings.near_dot_dist_ly * LY * RS);

    int *dot_order = s_rs_dot_order;
    int dot_ns = 0, dot_np = 0, dot_nm = 0;
    int *dot_stars = s_rs_dot_stars, *dot_planets = s_rs_dot_planets, *dot_moons = s_rs_dot_moons;
    for (int di = 0; di < n_dyn; di++) {
        int i = s_dyn[di];
        if (!g_bodies[i].alive) continue;
        if (g_bodies[i].is_black_hole) continue;       /* drawn by the BH pass */
        if (info[i].dcam >= NEAR_DOT_DIST) continue;   /* far → cheap pass below */
        if      (g_bodies[i].is_star)       dot_stars  [dot_ns++] = i;
        else if (g_bodies[i].parent < 0 ||
                 g_bodies[g_bodies[i].parent].is_star) dot_planets[dot_np++] = i;
        else                                           dot_moons  [dot_nm++] = i;
    }
    /* Insertion sort within each tier by dcam (ascending = nearest first) */
    for (int i = 1; i < dot_ns; i++) {
        int t = dot_stars[i], k = i;
        while (k > 0 && info[dot_stars[k-1]].dcam > info[t].dcam)
            { dot_stars[k] = dot_stars[k-1]; k--; }
        dot_stars[k] = t;
    }
    for (int i = 1; i < dot_np; i++) {
        int t = dot_planets[i], k = i;
        while (k > 0 && info[dot_planets[k-1]].dcam > info[t].dcam)
            { dot_planets[k] = dot_planets[k-1]; k--; }
        dot_planets[k] = t;
    }
    for (int i = 1; i < dot_nm; i++) {
        int t = dot_moons[i], k = i;
        while (k > 0 && info[dot_moons[k-1]].dcam > info[t].dcam)
            { dot_moons[k] = dot_moons[k-1]; k--; }
        dot_moons[k] = t;
    }
    /* Concatenate tiers into priority order */
    for (int i = 0; i < dot_ns; i++) dot_order[i]                  = dot_stars[i];
    for (int i = 0; i < dot_np; i++) dot_order[dot_ns + i]          = dot_planets[i];
    for (int i = 0; i < dot_nm; i++) dot_order[dot_ns + dot_np + i] = dot_moons[i];
    int dot_total = dot_ns + dot_np + dot_nm;

    /* Greedy overlap pass — project each candidate dot and test against confirmed dots */
    float *dot_sx = s_rs_dot_sx, *dot_sy = s_rs_dot_sy;
    float *dot_overlap_alpha = s_rs_dot_overlap_alpha;
    int   *dot_candidate = s_rs_dot_candidate;
    int   *dot_vis = s_rs_dot_vis;
    memset(dot_overlap_alpha, 0, (size_t)g_nbodies * sizeof(float));
    memset(dot_candidate, 0, (size_t)g_nbodies * sizeof(int));
    memset(dot_vis, 0, (size_t)g_nbodies * sizeof(int));

    {
        double cx2 = g_cam.pos[0];
        double cy2 = g_cam.pos[1];
        double cz2 = g_cam.pos[2];

        for (int oi = 0; oi < dot_total; oi++) {
            int i = dot_order[oi];
            if (!g_bodies[i].alive) continue;
            /* Beyond the far-field horizon: culled (true-depth falloff) */
            if (info[i].dcam > g_settings.farfield_horizon_au) continue;
            /* Pre-filter: fully occluded by a star's glare corona.  Memoized
             * (s_rs_glare_vis) — the upload loop below reuses the value. */
            s_rs_glare_vis[i] = body_point_star_glare_visibility(i, dot_stars,
                                                                 dot_ns);
            if (s_rs_glare_vis[i] <= 0.02f) continue;
            /* Pre-filter: star has grown large enough that its glare disc replaces its dot */
            if (g_bodies[i].is_star &&
                body_px[i] * STAR_GLARE_BILL_SCALE >= lod_glare_hi) continue;
            /* Pre-filter: non-star body is large enough to be rendered as a sphere */
            if (!g_bodies[i].is_star && body_px[i] >= lod_body_hi)
                continue;
            if (body_point_occluded_by_body(i, info)) continue;
            Body *bi = &g_bodies[i];

            float rx = (float)(bi->pos[0] * RS - cx2);
            float ry = (float)(bi->pos[1] * RS - cy2);
            float rz = (float)(bi->pos[2] * RS - cz2);

            float sx, sy;
            if (!mat4_project(vp_camrel, rx, ry, rz, WIN_W, WIN_H, &sx, &sy)) continue;
            dot_candidate[i] = 1;

            /* Measure nearest confirmed-visible dot on screen */
            float overlap_alpha = 1.0f;
            float nearest2 = DOT_EXCL_PX * DOT_EXCL_PX;
            for (int oj = 0; oj < oi; oj++) {
                int j = dot_order[oj];
                if (!dot_vis[j]) continue;
                float dx = sx - dot_sx[j], dy = sy - dot_sy[j];
                float d2 = dx*dx + dy*dy;
                if (d2 < nearest2) nearest2 = d2;
            }

            if (nearest2 < DOT_EXCL_PX * DOT_EXCL_PX) {
                float d = sqrtf(nearest2);
                overlap_alpha = (float)smoothstepd(DOT_HIDE_PX, DOT_EXCL_PX, d);
            }

            dot_overlap_alpha[i] = overlap_alpha;
            /* A dot claims its slot only if fully opaque (nearest competitor is far) */
            if (overlap_alpha >= 0.999f) {
                dot_sx[i]  = sx;
                dot_sy[i]  = sy;
                dot_vis[i] = 1;
            }
        }
    }

    /* Build the GPU upload buffer for surviving dots.
     * Dots render at their true camera-relative position and fade out toward the
     * far-field horizon (no pin-to-shell); beyond it they are culled.
     * Alpha = system_dot_fade × overlap_fade × glare_visibility × horizon_fade × sphere-approach fade. */
    float *dot_data = s_rs_dot_data;
    int   dot_count = 0;
    {
        double cx = g_cam.pos[0];
        double cy = g_cam.pos[1];
        double cz = g_cam.pos[2];

        for (int oi = 0; oi < dot_total; oi++) {
            int i = dot_order[oi];
            if (!g_bodies[i].alive) continue;
            if (!dot_candidate[i] || dot_overlap_alpha[i] <= 0.001f) continue;
            Body *b = &g_bodies[i];

            float f = b->is_star ? 1.0f : system_dot_fade_for_body(i);
            f *= dot_overlap_alpha[i];
            f *= s_rs_glare_vis[i];   /* memoized in the overlap pre-filter */
            f *= farfield_horizon_fade(info[i].dcam);
            if (f <= 0.0f) continue;

            /* Star: dot fades out as the glare billboard fades in — the exact
             * complement of the glare pass's smoothstep over the same
             * (density-scaled) window, so the handoff conserves brightness.
             * The HDR gain rolls off to 1 through the same window: the glare
             * billboard is not overbright-scaled, so a gained dot handing
             * off at full blaze would pulse. */
            float gain = 1.0f;
            if (b->is_star) {
                float glare_px = body_px[i] * STAR_GLARE_BILL_SCALE;
                float comp = 1.0f - (float)smoothstepd(lod_glare_lo, lod_glare_hi,
                                                       glare_px);
                f *= comp;
                gain = 1.0f + (star_dot_hdr_gain(i, info[i].dcam) - 1.0f) * comp;
            }
            if (f <= 0.0f) continue;

            /* Non-star: dot alpha is the exact complement of the sphere's
             * fade-in opacity (continuous LOD crossfade). */
            if (!b->is_star)
                f *= 1.0f - s_rs_sphere_alpha[i];
            if (f <= 0.0f) continue;

            float bx = (float)(b->pos[0] * RS - cx);
            float by = (float)(b->pos[1] * RS - cy);
            float bz = (float)(b->pos[2] * RS - cz);
            dot_data[dot_count*8+0] = bx;
            dot_data[dot_count*8+1] = by;
            dot_data[dot_count*8+2] = bz;
            dot_data[dot_count*8+3] = b->col[0] * gain;
            dot_data[dot_count*8+4] = b->col[1] * gain;
            dot_data[dot_count*8+5] = b->col[2] * gain;
            dot_data[dot_count*8+6] = f;
            dot_data[dot_count*8+7] = star_dot_pixel_size(i, info[i].dcam);
            dot_count++;
        }
    }

    /* ---- Far pass: cheap bulk dots for everything beyond NEAR_DOT_DIST.
     * No sort, no overlap dedup, no glare-occlusion scan — just the
     * camera-relative projection (done in double to keep light-year precision)
     * with the same distance fade and far-plane clamp the near path uses.  This
     * is the O(N) replacement for the old O(N^2) per-frame dot work, and it is
     * what makes a 16k-body universe render in real time.  Far planets/moons
     * fade out through system_dot_fade_for_body() and are skipped once
     * invisible, so only star points remain in deep space (Space-Engine style). */
    {
        double cx = g_cam.pos[0];
        double cy = g_cam.pos[1];
        double cz = g_cam.pos[2];

        for (int di = 0; di < n_dyn; di++) {
            int i = s_dyn[di];
            Body *b = &g_bodies[i];
            if (!b->alive || !info[i].show) continue;     /* spheres drawn elsewhere */
            if (b->is_black_hole) continue;               /* drawn by the BH pass */
            if (info[i].dcam < NEAR_DOT_DIST) continue;    /* near → handled above */
            if (info[i].dcam > g_settings.farfield_horizon_au) continue; /* past horizon → culled */

            float f = b->is_star ? 1.0f : system_dot_fade_for_body(i);
            f *= farfield_horizon_fade(info[i].dcam);
            if (f <= 0.0f) continue;

            /* No glare handoff out here — the gain applies in full. */
            float gain = star_dot_hdr_gain(i, info[i].dcam);

            float bx = (float)(b->pos[0] * RS - cx);
            float by = (float)(b->pos[1] * RS - cy);
            float bz = (float)(b->pos[2] * RS - cz);
            dot_data[dot_count*8+0] = bx;
            dot_data[dot_count*8+1] = by;
            dot_data[dot_count*8+2] = bz;
            dot_data[dot_count*8+3] = b->col[0] * gain;
            dot_data[dot_count*8+4] = b->col[1] * gain;
            dot_data[dot_count*8+5] = b->col[2] * gain;
            dot_data[dot_count*8+6] = f;
            dot_data[dot_count*8+7] = star_dot_pixel_size(i, info[i].dcam);
            dot_count++;
        }
    }

    if (dot_count > 0) {
        glUseProgram(s_dot_shader);
        glUniformMatrix4fv(s_dot_vp, 1, GL_FALSE, vp_camrel);
        glUniform1f(s_dot_time,    (float)SDL_GetTicks() * 0.001f);
        glUniform1f(s_dot_twinkle, (float)g_settings.star_twinkle);
        glBindVertexArray(s_dot_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_dot_vbo);
        /* Grow the GPU buffer past its initial MAX_BODIES sizing if needed. */
        if (dot_count > s_dot_vbo_cap) {
            int c = s_dot_vbo_cap ? s_dot_vbo_cap : MAX_BODIES;
            while (c < dot_count) c *= 2;
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)c * 8 * sizeof(float),
                         NULL, GL_DYNAMIC_DRAW);
            s_dot_vbo_cap = c;
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        dot_count * 8 * sizeof(float), dot_data);
        /* Depth-test the dots so a background star is occluded by a foreground
         * planet/star sphere (color.frag writes the same log-depth as phong.frag,
         * so the comparison is exact).  Depth writes stay OFF: dots must not
         * occlude each other or the trails/rings/glare drawn afterwards. */
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        /* star_dot.vert sets gl_PointSize per dot (magnitude-driven). */
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDrawArrays(GL_POINTS, 0, dot_count);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(0);
    }

    /* ---- Static field-star dots (bulk Gaia field): one draw call, GPU-side
     * camera-relative transform + sizing (star_field.vert), zero per-star CPU.
     * Same blend/depth state as the dynamic dots, and the shader culls stars
     * nearer than NEAR_DOT_DIST — the exact threshold the dynamic near path uses
     * to take over — so the handoff is seamless with no double-draw. */
    field_stars_ensure();
    if (s_field_shader && s_field_count > 0) {
        float near_dist = (float)((double)g_settings.near_dot_dist_ly * LY * RS);
        glUseProgram(s_field_shader);
        glUniformMatrix4fv(s_field_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_field_cam, (float)g_cam.pos[0], (float)g_cam.pos[1],
                    (float)g_cam.pos[2]);
        glUniform1f(s_field_near,    near_dist);
        glUniform1f(s_field_horizon, (float)g_settings.farfield_horizon_au);
        glUniform1f(s_field_time,    (float)SDL_GetTicks() * 0.001f);
        glUniform1f(s_field_twinkle, (float)g_settings.star_twinkle);
        glBindVertexArray(s_field_vao);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDrawArrays(GL_POINTS, 0, s_field_count);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glBindVertexArray(0);
    }

    /* ------------------------------------------------------------------ 4. Rings + Asteroid belts */
    rings_render(vp_camrel);
    asteroids_render(vp_camrel);

    /* ------------------------------------------------------------------ 5. Trails */
    trails_render(vp_camrel);

    /* ------------------------------------------------------------------ 6. Star glare
     * Drawn after trails so stellar corona covers orbit lines inside the glow disc.
     * Additive blend (GL_ONE / GL_ONE) accumulates glow from multiple stars.
     * Depth-tested (GL_LEQUAL) so glare is eclipsed by foreground opaque bodies.
     * Glare is drawn at the star's true camera-relative position and faded out
     * toward the far-field horizon (no pin-to-shell); beyond it it is culled. */
    if (s_glare_shader) {
        glUseProgram(s_glare_shader);
        glUniformMatrix4fv(s_gl_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_gl_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_gl_up,    cam_up[0],    cam_up[1],    cam_up[2]);
        glUniform1f(s_gl_spike,  (float)g_settings.lens_spikes);
        glUniform1f(s_gl_corona, (float)g_settings.star_corona);
        glUniform1f(s_gl_time,   (float)SDL_GetTicks() * 0.001f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);   /* purely additive */
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(s_sphere_vao);

        for (int di = 0; di < n_dyn; di++) {
            int i = s_dyn[di];
            if (!g_bodies[i].alive) continue;
            if (!g_bodies[i].is_star) continue;
            if (g_bodies[i].is_black_hole) continue;   /* no corona; BH pass below */
            /* Phase 3: skip the glare billboard for stars whose corona is
             * sub-pixel — at galaxy scale that is the overwhelming majority, and
             * each one is a full draw call + 4 uniform updates.  Below
             * STAR_DOT_FULL_GLARE_PX the dot point already represents the star
             * (the dot/glare handoff in the dot pass uses the same threshold),
             * so dropping the billboard here is visually consistent and turns a
             * per-star draw-call storm into just the few nearby stars. */
            /* Continuous LOD: the billboard fades in over the (density-scaled)
             * window the star dot fades out over, instead of popping in at
             * full brightness.  Below the window the dot alone represents the
             * star and the draw call is skipped entirely (perf-critical at
             * galaxy scale). */
            float glare_fade = (float)smoothstepd(lod_glare_lo, lod_glare_hi,
                                                  body_px[i] * STAR_GLARE_BILL_SCALE);
            if (glare_fade <= 0.0f) continue;

            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);

            /* Skip stars behind the camera */
            if (rx*cam_fwd[0] + ry*cam_fwd[1] + rz*cam_fwd[2] < 0.0f) continue;

            float dist   = sqrtf(rx*rx + ry*ry + rz*rz);
            float radius = (float)(g_bodies[i].radius * RS);
            /* True-depth falloff: cull past the horizon, fade the additive glow
             * (via colour) over the approach so it doesn't pop out.  The LOD
             * crossfade factor composes the same way. */
            float hf = farfield_horizon_fade(dist);
            if (hf <= 0.0f) continue;
            hf *= glare_fade;

            glUniform3f(s_gl_center, rx, ry, rz);
            glUniform1f(s_gl_radius, radius);
            glUniform3f(s_gl_color,
                        g_bodies[i].col[0] * hf, g_bodies[i].col[1] * hf, g_bodies[i].col[2] * hf);
            glUniform1f(s_gl_seed, (float)(i % 1024) * 0.1013f);
            /* Resolved-disc fade: once the star's disc is genuinely large on
             * screen, clear the glow off the disc face so the photosphere
             * shows (starspots/granulation).  0 below 30 px keeps the
             * dot↔glare handoff regime bit-identical. */
            glUniform1f(s_gl_resolve,
                        (float)smoothstepd(30.0, 120.0, body_px[i]));
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* ------------------------------------------------------------------ 6.35. Comets
     * Coma + ion/dust tails, additive over the scene like the star glare.
     * comet.c owns the pass; activity comes from the RadianceField, so this
     * costs nothing in comet-less universes (one flag test per body). */
    comet_render(vp_camrel, cam_right, cam_up, cam_fwd, g_cam.pos,
                 (float)SDL_GetTicks() * 0.001f);

    /* ------------------------------------------------------------------ 6.4. Black holes
     * Shadow + accretion disk billboards, alpha-blended (the shadow is opaque
     * so it occludes the background; the disk/ring write HDR-bright colour that
     * bloom turns into a glow).  Depth-tested so a nearer body occludes the BH;
     * drawn at true camera-relative depth and culled past the far-field horizon.
     *
     * The shadow/jet/torus/AGN-core passes below each used to scan all g_nbodies
     * and re-run bh_scales() (pow/log10) for the same holes.  Collect the (few)
     * black holes once here with their scales cached, and let every pass iterate
     * this list instead.  Holes past the far-field horizon are culled here (so
     * bh_scales never runs for them — positions are fixed within the frame, so
     * this is exactly the cull each pass used to apply). */
    static int    *s_bh_list  = NULL;
    static double *s_bh_rs     = NULL, *s_bh_astar = NULL, *s_bh_isco = NULL;
    static int     s_bh_cap    = 0;
    int n_bh = 0;
    for (int di = 0; di < n_dyn; di++) {
        int i = s_dyn[di];
        if (!g_bodies[i].alive || !g_bodies[i].is_black_hole) continue;
        {
            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
            if (rx*rx + ry*ry + rz*rz > g_settings.farfield_horizon_au *
                                        g_settings.farfield_horizon_au)
                continue;   /* past horizon → invisible in every pass below */
        }
        if (n_bh >= s_bh_cap) {
            s_bh_cap   = s_bh_cap ? s_bh_cap * 2 : 16;
            s_bh_list  = realloc(s_bh_list,  (size_t)s_bh_cap * sizeof(int));
            s_bh_rs    = realloc(s_bh_rs,    (size_t)s_bh_cap * sizeof(double));
            s_bh_astar = realloc(s_bh_astar, (size_t)s_bh_cap * sizeof(double));
            s_bh_isco  = realloc(s_bh_isco,  (size_t)s_bh_cap * sizeof(double));
            if (!s_bh_list || !s_bh_rs || !s_bh_astar || !s_bh_isco) {
                fprintf(stderr, "[render] bh list alloc failed\n"); exit(1);
            }
        }
        s_bh_list[n_bh] = i;
        bh_scales(&g_bodies[i], &s_bh_rs[n_bh], &s_bh_astar[n_bh], &s_bh_isco[n_bh]);
        n_bh++;
    }

    if (s_bh_shader && n_bh > 0) {
        glUseProgram(s_bh_shader);
        glUniformMatrix4fv(s_bh_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_bh_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_bh_up,    cam_up[0],    cam_up[1],    cam_up[2]);
        glUniform1f(s_bh_time,  (float)SDL_GetTicks() * 0.001f);
        glUniform1i(s_bh_scene, 0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);        /* bh.frag writes true per-fragment depth */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(s_sphere_vao);

        for (int bi = 0; bi < n_bh; bi++) {
            int i = s_bh_list[bi];

            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
            if (rx*cam_fwd[0] + ry*cam_fwd[1] + rz*cam_fwd[2] < 0.0f) continue;

            /* Snapshot the scene rendered so far (galaxies, nebulae, dots —
             * and any hole already drawn this pass) so the raymarch bends the
             * *real* background around the hole; per-hole grabs let one hole
             * lense another's image instead of erasing it where the enlarged
             * lens quads overlap.  0 when post is off → the shader falls back
             * to procedural stars. */
            GLuint scene_grab = (GLuint)post_grab_scene();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_grab);
            glUniform1i(s_bh_has_scene, scene_grab ? 1 : 0);

            double rs_m = s_bh_rs[bi], isco_rs = s_bh_isco[bi];
            float radius = (float)(rs_m * RS);   /* horizon radius from mass */

            glUniform3f(s_bh_center, rx, ry, rz);
            glUniform1f(s_bh_radius, radius);
            glUniform1f(s_bh_disk_in, (float)isco_rs);
            glUniform3f(s_bh_color,
                        g_bodies[i].col[0], g_bodies[i].col[1], g_bodies[i].col[2]);
            /* Disk spin axis (world space). Ecliptic north maps to GL +Y; a
             * non-zero obliquity tilts the disk about the GL X axis. */
            {
                double ob = g_bodies[i].obliquity * (PI / 180.0);
                glUniform3f(s_bh_disk_n, 0.0f, (float)cos(ob), (float)sin(ob));
            }
            glUniform1f(s_bh_activity, g_bodies[i].agn_activity);
            {
                double sp = g_bodies[i].spin_a != 0.0 ? g_bodies[i].spin_a
                                                       : g_bodies[i].rotation_rate;
                glUniform1f(s_bh_spin, sp < 0.0 ? -1.0f : 1.0f);
            }
            glUniform1f(s_bh_disk, g_bodies[i].accretion_disk);
            /* Disk hotness. When the hole is actually accreting, use the real
             * Shakura-Sunyaev peak effective temperature from Ṁ:
             *   T_peak ≈ 0.488·(3·G·M·Ṁ / (8π·σ·r_in³))^¼,  r_in = ISCO,
             * mapped log-linearly to the shader's 0..1 blue↔red hotness. So a
             * fading quasar's disk visibly reddens as Ṁ drops, and a stellar-mass
             * hole (hotter per T ∝ (Ṁ/M²)^¼) runs blue-white. Holes with no
             * accretion data fall back to the old mass+activity proxy. */
            {
                double msun = 1.989e30;
                double hot;
                if (g_bodies[i].mdot > 0.0) {
                    const double SIGMA = 5.670374e-8;   /* Stefan-Boltzmann */
                    double r_in = isco_rs * rs_m;       /* ISCO radius, metres */
                    double T4 = 3.0 * 6.674e-11 * g_bodies[i].mass * g_bodies[i].mdot
                                / (8.0 * PI * SIGMA * r_in * r_in * r_in);
                    double Tpeak = 0.488 * pow(T4, 0.25);
                    /* log10(T): ~5.4 (cool AGN) → red, ~7.3 (stellar-mass) → blue. */
                    hot = (log10(Tpeak) - 5.37) / (7.30 - 5.37);
                } else {
                    double act = g_bodies[i].agn_activity > 0.05 ? g_bodies[i].agn_activity : 0.05;
                    hot = 0.90 - 0.10 * log10(g_bodies[i].mass / msun) + 0.15 * log10(act);
                }
                hot = hot < 0.0 ? 0.0 : (hot > 1.0 ? 1.0 : hot);
                glUniform1f(s_bh_disk_temp, (float)hot);
                /* Visual Keplerian swirl rate: ω ∝ 1/M physically; log-compressed
                 * so a supermassive disk turns slowly and a stellar one fast, both
                 * still visibly animated. */
                double rate = 4.0 * pow(2.0e33 / g_bodies[i].mass, 0.12);
                rate = rate < 1.4 ? 1.4 : (rate > 9.0 ? 9.0 : rate);
                glUniform1f(s_bh_disk_rate, (float)rate);
            }
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* ---------------------------------- 6.4b. AGN relativistic jets (additive) */
    if (s_jet_shader) {
        glUseProgram(s_jet_shader);
        glUniformMatrix4fv(s_jet_vp, 1, GL_FALSE, vp_camrel);
        glUniform1f(s_jet_time, (float)SDL_GetTicks() * 0.001f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);          /* additive glow */
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(s_sphere_vao);

        for (int bi = 0; bi < n_bh; bi++) {
            int i = s_bh_list[bi];
            if (g_bodies[i].agn_activity <= 0.0f) continue;

            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
            float dist   = sqrtf(rx * rx + ry * ry + rz * rz);
            double rs_m = s_bh_rs[bi], a_star = s_bh_astar[bi];
            float radius = (float)(rs_m * RS);

            /* Blandford–Znajek: jets are powered by spin, so length and strength
             * scale with a* — a non-spinning hole barely jets even when accreting. */
            float spin  = (float)a_star;
            float power = g_bodies[i].agn_activity * (0.15f + 0.85f * spin);
            if (power <= 0.0f) continue;

            /* Jets fire along the spin axis (= disk normal). */
            double ob = g_bodies[i].obliquity * (PI / 180.0);
            float ax = 0.0f, ay = (float)cos(ob), az = (float)sin(ob);
            /* Skip when nearly pole-on: the axis-aligned ribbon degenerates to a
             * quad edge there, and the beamed core carries the look instead. */
            float jalign = fabsf((rx * ax + ry * ay + rz * az) / (dist > 1e-6f ? dist : 1.0f));
            if (jalign > 0.94f) continue;
            glUniform3f(s_jet_center, rx, ry, rz);
            glUniform3f(s_jet_axis, ax, ay, az);
            glUniform1f(s_jet_len,   radius * (12.0f + 46.0f * spin));
            glUniform1f(s_jet_width, radius * 4.5f);
            glUniform3f(s_jet_color, 0.55f, 0.72f, 1.0f);
            glUniform1f(s_jet_activity, power);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* -------------------------------------- 6.4c. AGN dust torus (alpha-over) */
    if (s_torus_shader) {
        const float RMAJ = 14.0f, RMIN = 6.0f;   /* in Rs units */
        glUseProgram(s_torus_shader);
        glUniformMatrix4fv(s_torus_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_torus_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_torus_up,    cam_up[0],    cam_up[1],    cam_up[2]);
        glUniform1f(s_torus_rmaj,  RMAJ);
        glUniform1f(s_torus_rmin,  RMIN);
        glUniform1f(s_torus_time,  (float)SDL_GetTicks() * 0.001f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);        /* torus.frag writes true per-fragment depth */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(s_sphere_vao);

        for (int bi = 0; bi < n_bh; bi++) {
            int i = s_bh_list[bi];
            if (g_bodies[i].dust_torus <= 0.0f) continue;

            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
            double rs_m = s_bh_rs[bi];
            float radius = (float)(rs_m * RS);
            double ob = g_bodies[i].obliquity * (PI / 180.0);

            /* Dust sublimation radius grows with luminosity (~accretion): a more
             * active nucleus pushes the torus outward and puffs it up. */
            float lum   = g_bodies[i].agn_activity;
            float rmaj  = RMAJ * (0.75f + 0.45f * lum);
            float rmin  = RMIN * (0.80f + 0.35f * lum);

            /* Azimuthal rotation: the dust doughnut orbits the spin axis at its
             * (large-radius, so slow) Keplerian rate — same swirl-rate model and
             * spin sense as the disk, evaluated at the torus major radius. */
            double t_rate = 4.0 * pow(2.0e33 / (g_bodies[i].mass > 0.0 ? g_bodies[i].mass : 1.0), 0.12);
            t_rate = t_rate < 1.4 ? 1.4 : (t_rate > 9.0 ? 9.0 : t_rate);
            double sp_t = g_bodies[i].spin_a != 0.0 ? g_bodies[i].spin_a
                                                     : g_bodies[i].rotation_rate;
            float spin_sign = sp_t < 0.0 ? -1.0f : 1.0f;
            float t_omega   = spin_sign * (float)t_rate * powf(rmaj, -1.5f);

            glUniform1f(s_torus_rate,   t_omega);
            glUniform3f(s_torus_center, rx, ry, rz);
            glUniform1f(s_torus_ext,    radius * ((rmaj + rmin) * 1.25f + 2.0f));
            glUniform1f(s_torus_rs,     radius);
            glUniform1f(s_torus_rmaj,   rmaj);
            glUniform1f(s_torus_rmin,   rmin);
            glUniform3f(s_torus_normal, 0.0f, (float)cos(ob), (float)sin(ob));
            glUniform3f(s_torus_color,  0.52f, 0.36f, 0.24f);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* -------------------------------- 6.4d. AGN beamed core (blazar, additive) */
    if (s_agncore_shader) {
        glUseProgram(s_agncore_shader);
        glUniformMatrix4fv(s_agncore_vp, 1, GL_FALSE, vp_camrel);
        glUniform3f(s_agncore_right, cam_right[0], cam_right[1], cam_right[2]);
        glUniform3f(s_agncore_up,    cam_up[0],    cam_up[1],    cam_up[2]);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(s_sphere_vao);

        for (int bi = 0; bi < n_bh; bi++) {
            int i = s_bh_list[bi];
            if (g_bodies[i].agn_activity <= 0.0f) continue;

            float rx = (float)(g_bodies[i].pos[0] * RS - g_cam.pos[0]);
            float ry = (float)(g_bodies[i].pos[1] * RS - g_cam.pos[1]);
            float rz = (float)(g_bodies[i].pos[2] * RS - g_cam.pos[2]);
            float dist = sqrtf(rx * rx + ry * ry + rz * rz);
            double rs_m = s_bh_rs[bi], a_star = s_bh_astar[bi];
            float radius = (float)(rs_m * RS);

            /* Beamed core lights up when the jet points at the camera (pole-on),
             * scaled by activity and spin (jet power). */
            double ob = g_bodies[i].obliquity * (PI / 180.0);
            float ax = 0.0f, ay = (float)cos(ob), az = (float)sin(ob);
            float align = fabsf((rx * ax + ry * ay + rz * az) / (dist > 1e-6f ? dist : 1.0f));
            float pole  = (float)smoothstepd(0.35, 0.92, align);
            float inten = g_bodies[i].agn_activity * (float)a_star * pole * pole * 4.0f;
            if (inten <= 0.001f) continue;

            glUniform3f(s_agncore_center, rx, ry, rz);
            glUniform1f(s_agncore_size, radius * 6.0f);
            glUniform3f(s_agncore_color, 0.72f, 0.83f, 1.0f);
            glUniform1f(s_agncore_int, inten);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* ------------------------------------------------------------------ 6.5. Build preview */
    render_build_preview(vp_camrel);

    /* ------------------------------------------------------------------ 6.6. Inspect target ring */
    {
        float rel[3], dr, aa;
        if (inspect_ring_params(info, rel, &dr, &aa))
            draw_ring_2d(rel, dr, aa, vp_camrel);
    }

    /* ------------------------------------------------------------------ 6.7. Lens flare feed
     *
     * Project the dominant emitter at the camera into NDC for the post-pass
     * flare overlay.  Always pushed (intensity 0 disables the pass), so a
     * stale flare can never persist a frame after the sun leaves the sky.
     * Intensity ramps with incident flux, saturating at Earth-like 1361 W/m² —
     * a sun in the outer system still flares gently, an interstellar one not
     * at all. */
    {
        float flare_col[3] = { 1.0f, 1.0f, 1.0f };
        float fl_i = 0.0f, fl_x = 0.0f, fl_y = 0.0f, fl_d = 1.0f;
        if (g_settings.lens_flare > 0.0f && post_enabled()) {
            double cam_m[3] = { g_cam.pos[0] * AU,
                                g_cam.pos[1] * AU,
                                g_cam.pos[2] * AU };
            RadianceContrib fl_top[1];
            if (radiance_field_top(cam_m, -1, 1, fl_top) >= 1 &&
                fl_top[0].irr > 0.0) {
                /* Camera-relative in double, cast late (the standard recipe). */
                double rx = fl_top[0].pos[0] * RS - g_cam.pos[0];
                double ry = fl_top[0].pos[1] * RS - g_cam.pos[1];
                double rz = fl_top[0].pos[2] * RS - g_cam.pos[2];
                float  fx = (float)rx, fy = (float)ry, fz = (float)rz;
                /* Behind-camera guard: without it the projection mirrors and
                 * the ghost chain sweeps the screen with the sun at our back. */
                if (cam_fwd[0]*fx + cam_fwd[1]*fy + cam_fwd[2]*fz > 0.0f) {
                    float cx = vp_camrel[0]*fx + vp_camrel[4]*fy + vp_camrel[8] *fz + vp_camrel[12];
                    float cy = vp_camrel[1]*fx + vp_camrel[5]*fy + vp_camrel[9] *fz + vp_camrel[13];
                    float cw = vp_camrel[3]*fx + vp_camrel[7]*fy + vp_camrel[11]*fz + vp_camrel[15];
                    if (cw > 1e-6f) {
                        fl_x = cx / cw;
                        fl_y = cy / cw;
                        double dist_au = sqrt(rx*rx + ry*ry + rz*rz);
                        fl_d = (float)(log2(dist_au + 1.0) /
                                       log2((double)RENDER_DEPTH_FAR + 1.0));
                        double t = (log10(fl_top[0].irr / 1361.0) + 4.0) / 4.0;
                        if (t < 0.0) t = 0.0;
                        if (t > 1.0) t = 1.0;
                        t = t * t * (3.0 - 2.0 * t);
                        fl_i = g_settings.lens_flare * (float)t;
                        flare_col[0] = fl_top[0].col[0];
                        flare_col[1] = fl_top[0].col[1];
                        flare_col[2] = fl_top[0].col[2];
                    }
                }
            }
        }
        post_set_lens_flare(fl_x, fl_y, fl_d, fl_i, flare_col);
    }

    /* ------------------------------------------------------------------ 7. Labels */
    labels_render(view_rot, proj, vp_camrel, info, dt);

    /* ------------------------------------------------------------------ 7.5. Screen flash
     *
     * Disabled on purpose: the camera-space glare/wash was reading as too
     * artificial, so we keep the volumetric supernova itself but omit the
     * fullscreen exposure pass entirely.
     */
}

/* ------------------------------------------------------------------ shutdown */
void render_shutdown(void) {
    for (int i = 0; i < 3; i++) {
        if (s_build_dist_text[i].tex) glDeleteTextures(1, &s_build_dist_text[i].tex);
        s_build_dist_text[i].tex = 0;
        s_build_dist_text[i].str[0] = '\0';
    }
    if (s_build_font) TTF_CloseFont(s_build_font);
    TTF_Quit();
    glDeleteProgram(s_sphere_shader);
    glDeleteProgram(s_atm_shader);
    glDeleteProgram(s_dot_shader);
    glDeleteProgram(s_impact_particle_shader);
    glDeleteProgram(s_glare_shader);
    glDeleteProgram(s_supernova_core_shader);
    glDeleteProgram(s_supernova_cloud_shader);
    glDeleteProgram(s_build_line_shader);
    glDeleteProgram(s_build_ui_shader);
    glDeleteBuffers(1, &s_sphere_vbo);
    glDeleteBuffers(1, &s_sphere_ebo);
    glDeleteVertexArrays(1, &s_sphere_vao);
    glDeleteBuffers(1, &s_dot_vbo);
    glDeleteVertexArrays(1, &s_dot_vao);
    glDeleteBuffers(1, &s_impact_particle_vbo);
    glDeleteVertexArrays(1, &s_impact_particle_vao);
    glDeleteBuffers(1, &s_build_line_vbo);
    glDeleteVertexArrays(1, &s_build_line_vao);
    glDeleteBuffers(1, &s_build_ui_vbo);
    glDeleteVertexArrays(1, &s_build_ui_vao);
    s_sphere_shader = s_dot_shader = 0;
    s_impact_particle_shader = 0;
    s_supernova_core_shader = s_supernova_cloud_shader = 0;
    s_build_line_shader = s_build_ui_shader = 0;
    s_build_font = NULL;
}
