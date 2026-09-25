/*
 * terrain.h — cube-sphere mesh terrain for close-up planets.
 *
 * A planet is drawn as a ray-traced sphere (phong.frag) until its relief
 * would show: once the tallest terrain moves the silhouette by about a pixel,
 * it switches to a mesh of six quadtrees on a cube-sphere. Tiles are baked on
 * the GPU into a height cache (a texture array), split and merged by their
 * projected vertex spacing, and hide the cracks between levels with skirts.
 * The mesh is rasterised into a depth + normal buffer, which the sphere's own
 * shader (phong.frag) then shades per pixel: one surface model, and the
 * handover changes geometry only.
 *
 * Heights are procedural (planet_noise.glsl relief_shape + fractal detail in
 * terrain_gen.frag); a measured elevation map would bake into the same cache.
 */
#pragma once

#include "common.h"

#define TERRAIN_TILE_N 65   /* vertices per tile edge (terrain_tile.glsl TILE_N) */

/* Compile the programs and allocate the tile cache. Returns 0 when terrain is
 * unavailable (planets then stay ray-traced spheres at every distance). */
int    terrain_init(void);

/* Relief amplitude of a planet recipe (phong.frag u_planet_type), in
 * body radii; 0 = no mesh terrain (gas giants, cloud-covered worlds, stars,
 * and Earth drawn from imagery, whose real elevation is not loaded yet). */
double terrain_relief_amp(int ptype, int earth_imagery);

/* Largest height above the sphere any tile can reach, in units of the relief
 * amplitude (for the silhouette-error test and culling bounds). */
#define TERRAIN_HMAX 1.4

typedef struct {
    int          body;          /* index in g_bodies                        */
    unsigned int name_hash;     /* guards a reused body slot                */
    int          ptype;
    double       amp;           /* terrain_relief_amp                       */
    double       radius;        /* AU                                       */
    double       center_rel[3]; /* body centre − camera, AU, world frame    */
    double       l2w[9];        /* body-local → world rotation, row-major   */
    const float *vp;            /* camera-relative view-projection          */
    float        cam_fwd[3];
    double       px_per_rad;    /* screen pixels per radian at the centre   */
} TerrainBody;

/* Once per frame, before any terrain_update. */
void terrain_frame_begin(void);

/* Decide this body's representation for the frame, from how many pixels its
 * relief spans on screen, and refresh its tiles (baking the ones it lacks,
 * within a per-frame budget). Returns 1 when it should be drawn as a mesh:
 * relief past a pixel and the whole planet already covered by baked tiles.
 * Changes the bound program, framebuffer and viewport only transiently. */
int  terrain_update(const TerrainBody *tb, double relief_px);

/* How much of the mesh's own slope its shading shows (phong.frag
 * u_terrain_slope), from the same relief measure: none at the switch from the
 * sphere, all of it a few pixels of relief closer. */
double terrain_slope_weight(double relief_px);

/* Rasterise the tiles terrain_update selected into the mesh buffer, then bind
 * it for surface_prog (phong.frag: u_terrain_depth, u_terrain_nrm), which is
 * left bound. The caller then draws the fullscreen surface pass with
 * u_terrain = 1. Returns 0 if there was nothing to draw. */
int  terrain_render(const TerrainBody *tb, GLuint surface_prog);

/* Ground height (body radii above the sphere) under a body-local direction,
 * from the surface as drawn. Returns 1 when exact; 2 when only an upper bound
 * is known (the height range of the finest tile there — a safe floor for a
 * camera); 0 when the body has no terrain around that point. Exact heights
 * are kept for the ground under the camera, a frame or two behind it. `level`
 * (may be NULL) is the detail level of the tile the answer came from: the
 * drawn surface itself shifts when that changes. */
int terrain_ground(int body, unsigned int name_hash, const double dir[3], double *h,
                   int *level);

/* Identity of a body's terrain (terrain_update's name_hash). */
unsigned int terrain_name_hash(const char *name);

/* A body's local → world rotation, row-major, from its spin angle (radians)
 * and axial tilt (degrees) — the frame its surface and terrain turn with. */
void terrain_local_to_world(double rotation_angle, double obliquity_deg, double m[9]);

void terrain_shutdown(void);
