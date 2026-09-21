/*
 * cinematic.h — cinematic renderer + deterministic film-out (see CINEMATIC.md).
 *
 * Two modes, selected purely by whether --output was given:
 *
 *   --cinematic                 Live: the window, free-look camera and menu all
 *                               behave exactly as before; only the *renderer* is
 *                               swapped for the accumulating one (low sample
 *                               count, still interactive).
 *   --cinematic --output PATH   Film-out: implies headless, abandons the wall
 *                               clock for a fixed timestep, renders exactly
 *                               ceil(duration x fps) frames and pipes them to an
 *                               encoder.
 *
 * ---- accumulation sampling (CINEMATIC.md §4) -------------------------------
 * Each output frame is the average of `samples` sub-frames, each rendered with
 * a jittered sub-pixel offset (and, from phase 2, a jittered lens position and
 * shutter time).  One integer controls antialiasing, depth of field and motion
 * blur together, which is what lets live and film-out share one renderer: live
 * runs samples=4, film-out runs samples=32+, and nothing else differs.
 *
 * Supersampling rather than a post-process AA is not a style choice here: the
 * scene is a field of sub-pixel high-contrast points, which TAA ghosts on and
 * FXAA erases.
 *
 * ---- per-frame call order (main.c) -----------------------------------------
 *     cinematic_frame_begin();                  // clear the accumulator
 *     for (s = 0; s < cinematic_samples(); s++) {
 *         cinematic_jitter(s, &jx, &jy);        // offset the projection
 *         cinematic_sub_begin();                // redirect post's composite
 *         post_begin(); render_frame(...); post_end();
 *         cinematic_sub_end();                  // add sub-frame to accumulator
 *     }
 *     cinematic_frame_resolve();                // average -> default framebuffer
 *     cinematic_capture_frame();                // film-out only: readback + encode
 *
 * Every entry point is a no-op when cinematic mode is off, so the normal path
 * is unchanged (and byte-identical: with samples==1 the accumulate/resolve
 * passes are skipped entirely and post composites straight to the back buffer).
 */
#pragma once
#include "common.h"

typedef struct {
    int    enabled;        /* --cinematic                                    */
    int    filming;        /* --output given: deterministic film-out          */
    char   output[512];    /* --output PATH                                   */

    int    width, height;  /* --res WxH   (render res, independent of window) */
    int    fps;            /* --fps N                                         */
    double duration;       /* --duration S                                    */
    int    samples;        /* --samples N (accumulation sub-frames)           */
    int    crf;            /* --crf N     (x264 quality; lower = better)      */

    /* The look parameters (aperture, shutter, focus, grade) deliberately live
     * in g_settings instead: live --cinematic is the tuning surface, so they
     * must persist to settings.json and be editable from the ImGui panel. */

    int    audio;          /* mux a soundtrack (--no-audio clears)            */
    char   audio_path[512];/* --audio PATH                                    */
} CinematicConfig;

extern CinematicConfig g_cine;

/* Populate g_cine with the live-mode defaults. Call before parsing argv. */
void cinematic_defaults(void);

/* Apply the --film / --draft presets. Applied before individual flags so
 * `--film --fps 30` gives 4K at 30fps. */
void cinematic_preset_film(void);
void cinematic_preset_draft(void);

/* Create GL resources. Call after render_init()/post_init(). Safe to call when
 * cinematic mode is off (it does nothing). */
void cinematic_init(void);
void cinematic_shutdown(void);

/* 1 when the cinematic renderer is driving the frame (GL resources are live). */
int  cinematic_active(void);
/* 1 when filming to --output. Implies cinematic_active(). */
int  cinematic_filming(void);

/* Effective accumulation sub-frame count for this frame: g_cine.samples, or 1
 * when cinematic mode is off or the accumulation targets are unavailable. */
int  cinematic_samples(void);

/* Change the accumulation sample count at runtime (the ImGui Look panel).
 * Clamped to [1,256]; ignored when accumulation is unavailable. */
void cinematic_set_samples(int n);

/* ---- film-out timing ------------------------------------------------------
 * Fixed frame interval (1/fps). main.c substitutes this for the wall-clock dt
 * while filming, which is what makes the output reproducible; it must also
 * bypass the loop's 0.1s dt clamp, since that clamp exists to stop a realtime
 * spiral and would silently truncate a long-timescale film. */
double cinematic_frame_dt(void);
int    cinematic_total_frames(void);

/* Sub-pixel jitter for sub-frame `s`, as small ANGULAR camera offsets in
 * radians: +ax yaws right, +ay pitches up. A Halton (2,3) low-discrepancy
 * sequence, so even samples=4 in live mode is well distributed, not clumped.
 *
 * Angular, rather than the projection-matrix skew this originally used,
 * because of how this renderer draws: the planet, atmosphere and volumetric
 * passes (phong/atm, nebula.frag, galaxy.frag, bh.frag) reconstruct their
 * camera ray PER FRAGMENT from u_fov_tan and aspect, and never consult the
 * projection matrix. Skewing the frustum therefore moves a volumetric's
 * bounding geometry but not the image it draws inside it — measured: a
 * projection skew large enough to sweep the starfield 96px moved a planet
 * 0.2px. Rotating the camera moves every path, vertex-projected and
 * ray-reconstructed alike. */
void cinematic_jitter(int s, float *ax, float *ay);

/* ---- depth of field (CINEMATIC.md §8.1/§8.2) ------------------------------
 * Focus distance in AU for this frame: the nearest body when
 * settings.cine_focus_auto is set, else the manual cine_focus_au. Returns <= 0
 * when depth of field is off or nothing is in focus range, which callers must
 * treat as "skip the lens jitter entirely". */
double cinematic_focus_distance(void);

/* Lock focus to a named body (case-insensitive), so the shot racks focus
 * automatically as that body moves. NULL or "" clears back to
 * settings.cine_focus_auto / the manual distance. A name that matches nothing
 * is reported once and then ignored. */
void cinematic_set_focus_target(const char *name);
/* The current focus-lock name, "" when none. */
const char *cinematic_focus_target(void);

/* Lens sample for sub-frame `s`: a point on the aperture disc in camera-space
 * (right, up) AU. The caller offsets the camera by du*right + dv*up IN DOUBLE
 * and compensates the projection so the focal plane stays fixed — see
 * cinematic_lens_proj_shift(). Both are zero when DOF is off. */
void cinematic_lens_sample(int s, double *du, double *dv);

/* Direction the lens-offset eye must look along to hold the focal point in
 * place: aim at the point `focus` AU straight ahead of the UNOFFSET camera.
 * Geometry at the focal plane then stays put while everything nearer or
 * further parallaxes — which is exactly the circle of confusion.
 *
 * Aiming rather than skewing the frustum, for the reason in cinematic_jitter()
 * above. It is paraxially exact and leaves a sub-pixel residual at the frame
 * edges (~1px against a 15px blur at 30 degrees FOV), which is far cheaper
 * than teaching every ray-reconstructing shader about a skewed frustum.
 * `fwd`, `right`, `up` are the unjittered camera basis; out_dir is normalised. */
void cinematic_lens_aim(double du, double dv, double focus,
                        const double fwd[3], const double right[3],
                        const double up[3], double out_dir[3]);

/* ---- motion blur (CINEMATIC.md §4.1) --------------------------------------
 * Fraction of the frame interval the shutter is open (shutter angle / 360),
 * clamped to [0,1]. 0 means no motion blur: the sim advances once per output
 * frame as it did in phase 1. */
double cinematic_shutter_fraction(void);

/* 1 when sub-frames should each advance the sim (shutter open and more than
 * one sample). Separated from the fraction so main.c can branch cheaply. */
int cinematic_motion_blur_active(void);

/* Raymarch step multiplier for the volumetric passes: settings.cine_quality
 * while filming, 1.0 otherwise. Film-out can afford detail that a live frame
 * cannot (CINEMATIC.md §8.4). */
float cinematic_quality_scale(void);

/* Accumulation lifecycle (no-ops when inactive or samples==1). */
void cinematic_frame_begin(void);
/* `sub` is the sub-frame index: it gates auto-exposure, which must adapt on
 * the first sample and then hold for the rest of the output frame. */
void cinematic_sub_begin(int sub);
void cinematic_sub_end(void);
void cinematic_frame_resolve(void);
/* The visible picture band in output pixels, y down: the whole frame, or the
 * area between the letterbox bars. Overlays position themselves inside it. */
void cinematic_picture_band(float *top, float *bottom);

/* ---- encoder --------------------------------------------------------------
 * Raw RGB frames are piped to an ffmpeg subprocess (CINEMATIC.md §7): the
 * project links SDL2/GLEW/GL only, and libavcodec would roughly double that for
 * one feature. If ffmpeg is not on PATH, frames are written as a numbered PPM
 * sequence and the command to finish the job is printed — a missing encoder
 * never loses the film. */
int  cinematic_encoder_open(void);    /* 0 on failure */
void cinematic_encoder_close(void);

/* Read back the resolved frame and push it to the encoder. Uses a two-deep PBO
 * ring so frame n's readback overlaps frame n+1's render (a 4K glReadPixels
 * stall is otherwise a large fraction of film-out wall time). */
void cinematic_capture_frame(void);

/* Progress line with an ETA, throttled to ~1 Hz. */
void cinematic_report_progress(void);
