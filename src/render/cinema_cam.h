/*
 * cinema_cam.h — keyframed camera shots for the cinematic renderer
 * (docs/CINEMATIC.md §9). Phase 3 of that plan.
 *
 * A *shot* is a list of keyframes, each pinning some subset of the camera state
 * at a time in seconds. Evaluating the shot at time t writes g_cam.pos/yaw/pitch
 * and, where the keys specify them, the FOV, aperture, focus target and
 * simulation timescale. The camera stops being free-look and becomes directed.
 *
 * Shots are JSON files under assets/shots/, parsed with the project's own
 * core/json.c (which already tolerates // comments and trailing commas), so a
 * shot is hand-editable. They can equally be authored by flying the normal app
 * and dropping keyframes with K — see cinema_shot_add_key_here().
 *
 * ---- how a key interpolates -------------------------------------------------
 *   position   Catmull-Rom spline through the resolved key positions, so the
 *              path is C1 continuous and passes through every key exactly.
 *              Endpoints are handled by duplicating the terminal keys.
 *   direction  Each key contributes a look direction evaluated FROM THE CURRENT
 *              interpolated position, then the two are slerped. When both keys
 *              look at the same body this makes the subject exactly centred for
 *              the whole segment, which is the property you actually want.
 *   scalars    fov and aperture lerp; timescale and focus distance interpolate
 *              GEOMETRICALLY, because they routinely span many decades (1 to
 *              3e7 seconds-per-second) and a linear ramp would spend almost the
 *              whole segment at the large end.
 *
 * ---- anchors ----------------------------------------------------------------
 * A key with an `anchor` body resolves its position as (body position + offset)
 * EVERY FRAME, so a shot anchored to a moving planet follows it instead of
 * drifting off the mark. Names are resolved per frame via body_find_named()
 * rather than cached as indices: a body can be absorbed mid-shot and dead
 * slots get reused.
 *
 * All positions are AU in double precision, matching Camera.pos — the camera
 * has to stay double all the way to render.c's camera-relative subtraction or
 * the path jitters at interstellar range.
 */
#pragma once
#include "common.h"

/* A hand-authored shot rarely needs more than a dozen keys; a generated tour
 * (cinema_tour.c) emits ~3 per subject and can run for minutes. */
#define CINE_MAX_KEYS   2048
#define CINE_NAME_LEN   32

typedef enum {
    CINE_EASE_LINEAR = 0,
    CINE_EASE_IN,        /* accelerate out of the key      */
    CINE_EASE_OUT,       /* decelerate into the next key   */
    CINE_EASE_INOUT,     /* smoothstep — the usual choice  */
    CINE_EASE_HOLD       /* freeze until the next key      */
} CineEase;

/* Unset optional fields (fov, aperture, focus, timescale, shutter) inherit
 * from the nearest earlier key in the same move — a cut starts a new one — and
 * otherwise fall back to their pre-shot values. A frame is a function of t
 * alone, never of which frames were evaluated before it. */
typedef struct {
    double   t;                        /* seconds into the shot             */
    double   pos[3];                   /* AU; an offset when anchor is set  */
    char     anchor[CINE_NAME_LEN];    /* "" = pos is absolute              */
    char     look_at[CINE_NAME_LEN];   /* "" = use yaw/pitch                */
    char     subject[CINE_NAME_LEN];   /* title-card subject: a body, nebula
                                        * or galaxy name; "" = look_at, else
                                        * anchor (see cinema_shot_subject) */
    float    yaw, pitch;               /* degrees                           */
    float    fov;                      /* <=0 = unset (inherits, see below)  */
    float    aperture;                 /* <0  = unset; 0 = depth of field off*/
    char     focus_name[CINE_NAME_LEN];/* "" = use focus_au                 */
    float    focus_au;                 /* <=0 = unset (with no focus_name)  */
    double   timescale;                /* <0  = unset                       */
    float    shutter;                  /* <0  = unset                       */
    char     detonate[CINE_NAME_LEN];/* star to kill (supernova or planetary
                                        * nebula) as playback crosses t; ""
                                        * = none. Fires once per play. */
    CineEase ease;
    int      cut;    /* 1 = hard cut INTO this key: the camera jumps here
                      * instead of flying from the previous key, and the
                      * Catmull-Rom control points never reach across the
                      * boundary.
                      *
                      * Without this a generated tour tries to *fly* between
                      * subjects that can be megaparsecs apart. The spline takes
                      * a key's tangent from its neighbours, so a leg 8.5e7 AU
                      * out followed by one at 7.6e11 AU produces a tangent
                      * enormous compared with the local segment and the camera
                      * overshoots into empty space — black frames mid-transit.
                      * Films cut between subjects anyway; flying the gap would
                      * be dead screen time even if it were well behaved. */
} CineKey;

/* ---- loading / saving ---------------------------------------------------- */

/* Parse a shot JSON into the active shot. Returns 0 on failure (and leaves any
 * previously loaded shot untouched, so a typo in a new file does not destroy
 * what you were working on). */
int  cinema_shot_load(const char *path);

/* Write the active shot to `path`, creating the directory if needed. */
int  cinema_shot_save(const char *path);

/* ---- playback ------------------------------------------------------------ */

/* 1 when a shot is loaded and has enough keys to evaluate (>= 2). */
int  cinema_shot_active(void);

/* Total shot length in seconds — the last key's time. 0 when inactive. */
double cinema_shot_duration(void);

const char *cinema_shot_name(void);

/* Evaluate at `t` seconds and apply to the camera and the look parameters.
 * Clamped to [0, duration]. Safe to call several times per frame: the
 * cinematic renderer calls it once per accumulation sub-frame at slightly
 * different times, which is what makes CAMERA motion blur work rather than
 * only object motion blur. */
void cinema_shot_eval(double t);

/* ---- playback clock -------------------------------------------------------
 * The shot's own clock lives here rather than in main.c so the ImGui editor can
 * scrub and preview it without reaching into the main loop's locals. In
 * film-out main.c sets the time directly from the frame number (keeping the
 * render deterministic); live playback advances it by the wall-clock dt. */
void   cinema_shot_play(double from_t);   /* also calls cinema_shot_begin() */
void   cinema_shot_stop(void);            /* also calls cinema_shot_end()   */
int    cinema_shot_playing(void);
double cinema_shot_time(void);
void   cinema_shot_set_time(double t);
/* Advance the live clock; stops playback when it runs past the last key. */
void   cinema_shot_advance(double dt);

/* Take a snapshot of the settings a shot may overwrite (fov, aperture, focus,
 * timescale) and restore it afterwards, so playing a shot in the live app does
 * not quietly redefine the user's preferences. */
void cinema_shot_begin(void);
void cinema_shot_end(void);

/* Editor scrub: pose the camera from the shot at t without running the clock,
 * under the same pre-shot snapshot playback uses (so unset fields fall back to
 * your settings). preview_end restores them. */
void cinema_shot_preview(double t);
void cinema_shot_preview_end(void);
int  cinema_shot_previewing(void);

/* What the camera is presenting right now — the thing a title card names
 * (docs/CINEMATIC.md §11.1). Supplied by the tour (current leg or cutaway), by a
 * shot (the current key's look_at, else its anchor), or by the free camera. */
typedef struct {
    char   name[CINE_NAME_LEN];
    char   kind[32];      /* "Planet", "Red giant", "Nebula", "Supernova" ... */
    int    body;          /* g_bodies index, or -1 for static scenery/events  */
    double pos_au[3];     /* absolute AU when body < 0                        */
} CineSubject;

/* Fill a kind label for body i ("Planet", "Moon", "Black hole", ...). */
void cinema_body_kind(int i, char *out, size_t n);

/* The subject of the active shot at time t; 0 if its keys name none. */
int  cinema_shot_subject(double t, CineSubject *out);

/* The body the camera rides at shot time t (its key's anchor), or -1. */
int  cinema_shot_anchor_body(double t);

/* 1 if a hard cut lies in (t0, t1] — the camera jumped rather than moved. */
int  cinema_shot_cut_between(double t0, double t1);

/* Replace the active shot with keys built in memory — the entry point the
 * procedural tour and the director use, so a generated shot goes through
 * exactly the same interpolation as a hand-authored one (and can be written
 * out with cinema_shot_save() and then hand-tuned). Copies the keys; sorts by
 * time. Returns 0 if n is out of range. */
int  cinema_shot_set(const CineKey *keys, int n, const char *name);

/* Snapshot the active shot into `out` (capacity CINE_MAX_KEYS); returns the
 * count. Lets the director stash the tour before cutting away from it. */
int  cinema_shot_get(CineKey *out);

/* ---- authoring ----------------------------------------------------------- */

/* Append a key at the current camera pose, timed `dt_after` seconds after the
 * last key (or at 0 for the first). Returns 0 if the shot is full. */
int  cinema_shot_add_key_here(double dt_after);
int  cinema_shot_remove_last(void);
void cinema_shot_clear(void);

int      cinema_shot_key_count(void);
CineKey *cinema_shot_keys(void);      /* mutable — the ImGui editor edits in place */

/* Re-sort keys by time. Call after editing a key's `t` in the editor. */
void cinema_shot_sort(void);

/* Move the camera to key `i` so you can see what it framed. */
void cinema_shot_goto_key(int i);
