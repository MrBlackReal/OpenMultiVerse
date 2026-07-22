/*
 * galaxy.h — real-catalogue galaxies as world-space volumetric structures
 * (roadmap Layer 4.2, first iteration).
 *
 * Same architecture as nebula.{c,h}: catalogue-driven placement (J2000
 * RA/Dec + distance + apparent size → true 3D position and physical radius),
 * one volumetric raymarch representation used from backdrop to fly-through
 * (billboard carrier, fullscreen when inside, angular-size-preserving far
 * clamp), premultiplied "over" blending at log depth.
 *
 * What is galaxy-specific is the density model (galaxy.frag): an exponential
 * stellar disc + warm bulge, two logarithmic spiral arms with star-forming
 * knots, absorbing dust lanes (alpha without emission — edge-on discs get the
 * classic dark stripe for free), differential rotational shear on u_time,
 * plus elliptical (smooth de-Vaucouleurs-ish glow) and irregular (clumpy FBM)
 * types. Each galaxy's disc axis is real-ish: tilted off the Earth sightline
 * by its catalogued inclination so the iconic Earth views (edge-on Sombrero,
 * tilted Andromeda) read correctly.
 */
#pragma once

void galaxy_init(void);
/* scene_depth_tex: GL name of the opaque scene's depth texture when the
 * caller renders into a depth-less half-res target (the raymarch then clips
 * itself to scene depth); 0 for the direct full-res path (normal depth test).
 * screen_w/h must match the *current* render target, not the window. */
void galaxy_render(const float vp_camrel[16],
                   const float cam_right[3], const float cam_up[3],
                   const float cam_fwd[3], const double cam_pos[3],
                   float fov_tan, float aspect, int screen_w, int screen_h,
                   float time_s, unsigned int scene_depth_tex);
void galaxy_shutdown(void);

/* Procedural resolved stars (the §0.1 galaxy → stars scale step): when the
 * camera is inside or entering a galaxy volume, cascaded lattices of point
 * stars are scattered on the GPU following the same density model as the
 * volume glow, so flying toward an arm resolves it into individual stars.
 * `gain` is the global fade — render.c passes the complement of the skybox
 * fade so the painted neighbourhood sky crossfades into procedural stars. */
void galaxy_render_stars(const float vp_camrel[16], const double cam_pos[3],
                         float gain, float time_s);

/* Master toggle (shares the Visuals menu pattern with nebulae). */
void galaxy_set_enabled(int enabled);
int  galaxy_enabled(void);

/* Visuals-menu parameters, nebula_get/set_params pattern (runtime-only,
 * not persisted): master toggle, volume density/brightness, raymarch step
 * budget, and the resolved-stars pass toggle. */
void galaxy_get_params(int *enabled, float *density, int *steps,
                       int *stars_enabled);
void galaxy_set_params(int enabled, float density, int steps,
                       int stars_enabled);

/* Enumeration for the Navigate tab / fields. */
int          galaxy_count(void);
const char  *galaxy_name(int i);
/* World position of galaxy i in AU (zeroed if out of range). */
void         galaxy_position(int i, double out[3]);
/* Bounding radius of galaxy i in AU (0 if out of range). */
double       galaxy_radius_au(int i);
/* Display colour of galaxy i (white if out of range). */
void         galaxy_color(int i, float out[3]);

/* Procedural-model parameters of galaxy i, for CPU-side re-evaluation of the
 * galaxy_stars density/hash pipeline (starsys.c star→system promotion must
 * find exactly the stars the shader draws). */
int          galaxy_type(int i);           /* 0 spiral, 1 elliptical, 2 irregular */
float        galaxy_seed(int i);
void         galaxy_axis(int i, float out[3]);

/* AGN host query: 1 if galaxy i hosts a central supermassive black hole, with
 * its mass (kg), Eddington ratio (0 = quiescent), and dust-torus strength
 * written to the out params (any may be NULL); 0 otherwise. */
int          galaxy_agn(int i, double *mass_kg, float *activity, float *torus);

/* Compose the finished BH/AGN engine into the galaxy renderer: for every AGN
 * host galaxy, spawn a black-hole Body at its photometric centre so the
 * existing render_frame AGN passes draw its accretion disk + jets in situ.
 * Call once after galaxy_init() and after each universe (re)load, before the
 * acceleration structures / warm-up. A no-op when g_settings.galaxy_agn is 0. */
void         galaxy_spawn_agn(void);
