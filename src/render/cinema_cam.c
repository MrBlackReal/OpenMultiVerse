/*
 * cinema_cam.c — keyframed camera shots (see cinema_cam.h, docs/CINEMATIC.md §9).
 */
#include "cinema_cam.h"
#include "frame.h"
#include "cinematic.h"
#include "camera.h"
#include "body.h"
#include "laws.h"
#include "json.h"
#include "lifecycle.h"
#include "nebula.h"
#include "galaxy.h"

#include <sys/stat.h>
#include <strings.h>

static CineKey s_keys[CINE_MAX_KEYS];
static int     s_nkeys;
static char    s_name[64] = "untitled";

/* Saved state for the settings a shot is allowed to drive, so live playback is
 * reversible (see cinema_shot_begin/end). */
static int    s_playing;
static int    s_snap;        /* a pre-shot snapshot is held (play or preview) */
static int    s_previewing;  /* editor scrub: posed from the shot, clock stopped */
static double s_time;        /* shot playback clock, seconds */
static double s_event_t = -1.0;  /* "detonate" keys up to here have fired */
static float  s_save_fov, s_save_aperture, s_save_focus_au;
static int    s_save_focus_auto;
static double s_save_timescale;
static float  s_save_shutter;
static char   s_save_focus_name[CINE_NAME_LEN];

/* ------------------------------------------------------------------ helpers */

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static CineEase ease_from_name(const char *s)
{
    if (!s) return CINE_EASE_INOUT;
    if (!strcasecmp(s, "linear")) return CINE_EASE_LINEAR;
    if (!strcasecmp(s, "in"))     return CINE_EASE_IN;
    if (!strcasecmp(s, "out"))    return CINE_EASE_OUT;
    if (!strcasecmp(s, "hold"))   return CINE_EASE_HOLD;
    return CINE_EASE_INOUT;
}

static const char *ease_name(CineEase e)
{
    switch (e) {
    case CINE_EASE_LINEAR: return "linear";
    case CINE_EASE_IN:     return "in";
    case CINE_EASE_OUT:    return "out";
    case CINE_EASE_HOLD:   return "hold";
    default:               return "inout";
    }
}

static double apply_ease(CineEase e, double u)
{
    switch (e) {
    case CINE_EASE_LINEAR: return u;
    case CINE_EASE_IN:     return u * u;
    case CINE_EASE_OUT:    return u * (2.0 - u);
    case CINE_EASE_HOLD:   return 0.0;
    default:               return u * u * (3.0 - 2.0 * u);   /* smoothstep */
    }
}

/* Geometric interpolation. Timescale and focus distance span many decades in a
 * single shot (real-time near a planet to a million years per second for a
 * pull-back); a linear ramp between 1 and 3e7 sits above 1.5e7 for half the
 * segment, which reads as an instant jump followed by nothing. */
static double lerp_geom(double a, double b, double u)
{
    if (a > 0.0 && b > 0.0) return a * pow(b / a, u);
    return a + (b - a) * u;
}

/* World position of a key in AU, resolving an anchor body every call. */
/* A shot's body by name. A star that died mid-shot (a "detonate" key) is
 * retired and replaced by "<name> Remnant" at the same spot, so fall back to
 * that: a shot framed on a star keeps framing what it became. */
static int shot_body(const char *name)
{
    int i = body_find_named(name);
    if (i < 0 && name[0]) {
        char rem[64];
        snprintf(rem, sizeof rem, "%.22s Remnant", name);   /* supernova.c's name */
        i = body_find_named(rem);
    }
    return i;
}

static void key_position(const CineKey *k, double out[3])
{
    if (k->anchor[0]) {
        int i = shot_body(k->anchor);
        if (i >= 0) {
            out[0] = g_bodies[i].pos[0] * RS + k->pos[0];
            out[1] = g_bodies[i].pos[1] * RS + k->pos[1];
            out[2] = g_bodies[i].pos[2] * RS + k->pos[2];
            return;
        }
        /* Anchor gone (absorbed, or a typo): fall through to the raw offset
         * rather than snapping the camera to the origin mid-shot. */
    }
    /* An absolute key is a Sun-frame position (frame.h), so a shot means the
     * same place whatever the floating origin is doing. */
    for (int q = 0; q < 3; q++) out[q] = k->pos[q] - g_frame_origin_au[q];
}

static void dir_from_yaw_pitch(float yaw, float pitch, double out[3])
{
    double y = yaw   * (PI / 180.0);
    double p = pitch * (PI / 180.0);
    out[0] = cos(p) * cos(y);
    out[1] = sin(p);
    out[2] = cos(p) * sin(y);
}

static void normalize3(double v[3])
{
    double L = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (L > 0.0) { v[0] /= L; v[1] /= L; v[2] /= L; }
    else         { v[0] = 0.0; v[1] = 0.0; v[2] = -1.0; }
}

/* The look direction key `k` wants, evaluated FROM `from`. A look_at key aims
 * at the body wherever it currently is, so both ends of a segment that watch
 * the same subject agree exactly and the subject stays pinned. */
static void key_direction(const CineKey *k, const double from[3], double out[3])
{
    if (k->look_at[0]) {
        int i = shot_body(k->look_at);
        if (i >= 0) {
            out[0] = g_bodies[i].pos[0] * RS - from[0];
            out[1] = g_bodies[i].pos[1] * RS - from[1];
            out[2] = g_bodies[i].pos[2] * RS - from[2];
            normalize3(out);
            return;
        }
    }
    dir_from_yaw_pitch(k->yaw, k->pitch, out);
}

/* Shortest-arc interpolation between two unit directions. Falls back to a
 * normalised lerp when they are nearly parallel (sin(theta) -> 0), which is the
 * common case: most segments barely turn. */
static void slerp3(const double a[3], const double b[3], double u, double out[3])
{
    double d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    d = clampd(d, -1.0, 1.0);
    double th = acos(d);
    if (th < 1e-6) {
        for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * u;
        normalize3(out);
        return;
    }
    double s = sin(th);
    double wa = sin((1.0 - u) * th) / s;
    double wb = sin(u * th) / s;
    for (int i = 0; i < 3; i++) out[i] = wa * a[i] + wb * b[i];
    normalize3(out);
}

/* Centripetal Catmull-Rom (alpha = 1/2, Barry-Goldman form) through p1..p2.
 *
 * The uniform variant took each key's tangent from its neighbours with no
 * regard for how far apart they are, so a key between a 0.01 AU segment and a
 * 1e9 AU one got a tangent a hundred times too long: the camera flew past the
 * key, turned round and came back — a visible U-turn at every change of scale,
 * which is exactly where scale-crossing shots spend their time. Centripetal
 * knots (spacing = sqrt of segment length) provably never cusp or loop within
 * a segment, whatever the ratio, and still pass through every key. Timing is
 * unchanged: u is still the normalised segment time; only the path's shape
 * uses the knots. Missing end neighbours are reflected (p0 = 2p1 - p2), so a
 * shot starts and ends moving along its first and last segments. */
static double knot_step(const double a[3], const double b[3])
{
    double dx = b[0]-a[0], dy = b[1]-a[1], dz = b[2]-a[2];
    return sqrt(sqrt(dx*dx + dy*dy + dz*dz));
}

static void catmull_centripetal(const double P0[3], const double P1[3],
                                const double P2[3], const double P3[3],
                                double u, double out[3])
{
    double p0[3], p3[3];
    double d12 = knot_step(P1, P2);
    if (d12 <= 1e-12) {                        /* a hold: nothing to move */
        out[0] = P1[0]; out[1] = P1[1]; out[2] = P1[2];
        return;
    }
    for (int c = 0; c < 3; c++) { p0[c] = P0[c]; p3[c] = P3[c]; }
    if (knot_step(P0, P1) <= 1e-12)
        for (int c = 0; c < 3; c++) p0[c] = 2.0 * P1[c] - P2[c];
    if (knot_step(P2, P3) <= 1e-12)
        for (int c = 0; c < 3; c++) p3[c] = 2.0 * P2[c] - P1[c];

    double t0 = 0.0;
    double t1 = t0 + knot_step(p0, P1);
    double t2 = t1 + d12;
    double t3 = t2 + knot_step(P2, p3);
    double t  = t1 + (t2 - t1) * u;
    for (int c = 0; c < 3; c++) {
        double a1 = (t1-t)/(t1-t0)*p0[c] + (t-t0)/(t1-t0)*P1[c];
        double a2 = (t2-t)/(t2-t1)*P1[c] + (t-t1)/(t2-t1)*P2[c];
        double a3 = (t3-t)/(t3-t2)*P2[c] + (t-t2)/(t3-t2)*p3[c];
        double b1 = (t2-t)/(t2-t0)*a1 + (t-t0)/(t2-t0)*a2;
        double b2 = (t3-t)/(t3-t1)*a2 + (t-t1)/(t3-t1)*a3;
        out[c]    = (t2-t)/(t2-t1)*b1 + (t-t1)/(t2-t1)*b2;
    }
}

/* ------------------------------------------------------- optional fields
 *
 * fov, aperture, focus, timescale and shutter are optional on a key. A key
 * that leaves one out inherits it from the nearest earlier key of the SAME
 * continuous move; a cut starts a new move, so the search stops there. With
 * nothing to inherit, the field falls back to its value from before the shot
 * began.
 *
 * This used to be "leave whatever is currently set", which made a frame
 * depend on the evaluation history instead of on t alone. In the procedural
 * tour that meant an AGN leg (which sets no aperture) inherited the previous
 * star leg's f/8 focus lock on a body 1e12 AU away: the lens disc came out
 * billions of AU wide, and every jittered sub-frame put the camera nowhere
 * near a subject it was framing from 100 AU. The legs rendered black at
 * --samples > 1 and perfectly at --samples 1, where there is no lens jitter. */
enum { F_FOV, F_APERTURE, F_FOCUS, F_TIMESCALE, F_SHUTTER };

static int key_has(const CineKey *k, int f)
{
    switch (f) {
    case F_FOV:       return k->fov > 0.0f;
    case F_APERTURE:  return k->aperture >= 0.0f;
    case F_FOCUS:     return k->focus_name[0] || k->focus_au > 0.0f;
    case F_TIMESCALE: return k->timescale >= 0.0;
    default:          return k->shutter >= 0.0f;
    }
}

/* The key whose value of field f is in force at key i, or NULL if nothing in
 * i's move sets it. The cut key itself still counts: it opens the move. */
static const CineKey *key_owner(int i, int f)
{
    for (int j = i; j >= 0; j--) {
        if (key_has(&s_keys[j], f)) return &s_keys[j];
        if (s_keys[j].cut) break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ queries */

int    cinema_shot_active(void)    { return s_nkeys >= 2; }
int    cinema_shot_key_count(void) { return s_nkeys; }
CineKey *cinema_shot_keys(void)    { return s_keys; }
const char *cinema_shot_name(void) { return s_name; }

double cinema_shot_duration(void)
{
    return s_nkeys > 0 ? s_keys[s_nkeys - 1].t : 0.0;
}

void cinema_shot_sort(void)
{
    for (int i = 1; i < s_nkeys; i++) {          /* insertion sort: n <= 64 */
        CineKey k = s_keys[i];
        int j = i - 1;
        while (j >= 0 && s_keys[j].t > k.t) { s_keys[j + 1] = s_keys[j]; j--; }
        s_keys[j + 1] = k;
    }
}

/* ------------------------------------------------------------------ playback */

void cinema_shot_begin(void)
{
    if (s_snap) { s_playing = 1; s_previewing = 0; return; }   /* from a preview */
    s_save_fov        = g_settings.fov;
    s_save_aperture   = g_settings.cine_aperture;
    s_save_focus_au   = g_settings.cine_focus_au;
    s_save_focus_auto = g_settings.cine_focus_auto;
    s_save_timescale  = g_laws.time_scale;
    s_save_shutter    = g_settings.cine_shutter;
    snprintf(s_save_focus_name, sizeof s_save_focus_name, "%s",
             cinematic_focus_target());
    s_snap    = 1;
    s_playing = 1;
}

void cinema_shot_end(void)
{
    if (!s_snap) return;
    g_settings.fov             = s_save_fov;
    g_settings.cine_aperture   = s_save_aperture;
    g_settings.cine_focus_au   = s_save_focus_au;
    g_settings.cine_focus_auto = s_save_focus_auto;
    g_laws.time_scale          = s_save_timescale;
    g_settings.cine_shutter    = s_save_shutter;
    cinematic_set_focus_target(s_save_focus_name);
    s_playing    = 0;
    s_previewing = 0;
    s_snap       = 0;
}

/* Editor scrub. Takes the same snapshot as playback (once), so fields the
 * shot leaves unset fall back to the pre-shot values exactly as they do when
 * playing — previously a bare scrub left whatever the last evaluated frame
 * had set, so scrubbing backwards could show settings from later in the
 * shot. The clock does not run; cinema_shot_preview_end() restores. */
void cinema_shot_preview(double t)
{
    if (!cinema_shot_active()) return;
    if (!s_snap) cinema_shot_begin();
    s_playing    = 0;
    s_previewing = 1;
    s_time = t;
    cinema_shot_eval(t);
}

void cinema_shot_preview_end(void)
{
    if (s_previewing) cinema_shot_end();
}

int cinema_shot_previewing(void) { return s_previewing; }


/* Fire the "detonate" keys whose time lies in (s_event_t, t]. Driven from the
 * shot clock (play / advance / set_time) and never from eval, so scrubbing a
 * preview cannot kill stars, and film-out fires on the same frame every run. */
static void fire_events(double t)
{
    for (int i = 0; i < s_nkeys; i++) {
        const CineKey *k = &s_keys[i];
        if (!k->detonate[0] || k->t <= s_event_t || k->t > t) continue;
        int star = body_find_named(k->detonate);
        if (star >= 0 && lifecycle_trigger_death(star) > 0)
            fprintf(stdout, "[CineCam] t=%.2fs detonated %s\n", k->t, k->detonate);
        else
            fprintf(stderr, "[CineCam] t=%.2fs cannot detonate '%s' (not an "
                            "evolvable star)\n", k->t, k->detonate);
    }
    s_event_t = t;
}

void cinema_shot_play(double from_t)
{
    if (!cinema_shot_active()) {
        fprintf(stderr, "[CineCam] need at least 2 keys to play\n");
        return;
    }
    cinema_shot_begin();
    s_time = from_t;
    s_event_t = from_t - 1e-9;   /* a key exactly at from_t still fires */
    fire_events(from_t);
}

void cinema_shot_stop(void)
{
    cinema_shot_end();
    s_time = 0.0;
}

int    cinema_shot_playing(void) { return s_playing && cinema_shot_active(); }
double cinema_shot_time(void)    { return s_time; }
void   cinema_shot_set_time(double t)
{
    s_time = t;
    if (cinema_shot_playing()) fire_events(t);
}

void cinema_shot_advance(double dt)
{
    if (!cinema_shot_playing()) return;
    s_time += dt;
    fire_events(s_time);
    if (s_time >= cinema_shot_duration()) {
        s_time = cinema_shot_duration();
        cinema_shot_stop();
    }
}

void cinema_shot_eval(double t)
{
    if (s_nkeys < 2) return;

    t = clampd(t, s_keys[0].t, s_keys[s_nkeys - 1].t);

    /* Segment containing t. */
    int i = 0;
    while (i < s_nkeys - 2 && t >= s_keys[i + 1].t) i++;
    const CineKey *ka = &s_keys[i];
    const CineKey *kb = &s_keys[i + 1];

    /* A cut into kb means this segment is not a move at all: hold ka's pose
     * until kb's time, then the next segment starts cleanly at kb. Generated
     * tours place the cut key at the same time as the previous key's end, so
     * the hold has zero duration and the cut reads as an instant change. */
    int ib = i + 1;
    if (kb->cut) {
        kb = ka;
        ib = i;
    }

    double span = kb->t - ka->t;
    double u    = span > 1e-9 ? (t - ka->t) / span : 0.0;
    u = apply_ease(ka->ease, clampd(u, 0.0, 1.0));

    /* ---- position: Catmull-Rom over the four surrounding keys ------------
     * Control points stop at a cut: reaching across one would let a subject
     * megaparsecs away bend the tangent of a move that is AU-sized. */
    int i0 = (i > 0 && !s_keys[i].cut) ? i - 1 : i;
    int i3 = i + 1;
    if (i + 2 < s_nkeys && !s_keys[i + 1].cut && !s_keys[i + 2].cut) i3 = i + 2;
    if (i3 >= s_nkeys) i3 = s_nkeys - 1;
    double p0[3], p1[3], p2[3], p3[3], pos[3];
    key_position(&s_keys[i0], p0);
    key_position(ka,          p1);
    key_position(kb,          p2);
    key_position(&s_keys[i3], p3);
    catmull_centripetal(p0, p1, p2, p3, u, pos);

    g_cam.pos[0] = pos[0];
    g_cam.pos[1] = pos[1];
    g_cam.pos[2] = pos[2];

    /* ---- direction ------------------------------------------------------- */
    double da[3], db[3], dir[3];
    key_direction(ka, pos, da);
    key_direction(kb, pos, db);
    slerp3(da, db, u, dir);

    /* Write back as yaw/pitch so cam_get_dir(), the HUD and the free-look
     * camera all agree with where the shot actually points. */
    g_cam.yaw   = (float)(atan2(dir[2], dir[0]) * (180.0 / PI));
    g_cam.pitch = (float)(asin(clampd(dir[1], -1.0, 1.0)) * (180.0 / PI));

    /* ---- scalars ---------------------------------------------------------
     * Each end of the segment resolves its own value (see key_owner). With no
     * key setting a field, it falls back to the pre-shot value (snapshotted by
     * both playback and editor preview); with no snapshot at all it is left
     * alone. */
    const CineKey *oa, *ob;

    oa = key_owner(i, F_FOV); ob = key_owner(ib, F_FOV);
    if (oa && ob)     g_settings.fov = (float)(oa->fov + (ob->fov - oa->fov) * u);
    else if (s_snap) g_settings.fov = s_save_fov;

    /* Aperture interpolates GEOMETRICALLY, because f-numbers are a log scale
     * (f/2.8 to f/11 is two stops, and the midpoint a photographer expects is
     * f/5.6, not f/6.9).
     *
     * Zero is special: it means "depth of field off", not "an infinitely wide
     * aperture". Interpolating into it would ramp through f/2, f/1, f/0.1 —
     * blurring harder and harder right up to the moment it switches off, which
     * is the exact opposite of what the author asked for. So a segment with a
     * zero at either end holds the near key's value and switches at the key. */
    oa = key_owner(i, F_APERTURE); ob = key_owner(ib, F_APERTURE);
    if (oa && ob && oa->aperture > 0.0f && ob->aperture > 0.0f)
        g_settings.cine_aperture = (float)lerp_geom(oa->aperture, ob->aperture, u);
    else if (oa)
        g_settings.cine_aperture = oa->aperture;
    else if (s_snap)
        g_settings.cine_aperture = s_save_aperture;

    /* Shutter is keyframable for the same reason fov is: a hyper-fast
     * pull-back sweeps stars across the whole frame in one frame interval, and
     * a 180-degree shutter then smears them into a wall of streaks that N
     * accumulation samples render as N discrete ghosts ("double vision") — no
     * practical sample count fixes it. Closing the shutter for the fast leg
     * shortens the streak instead, which is exactly what a camera operator
     * would do. */
    oa = key_owner(i, F_SHUTTER); ob = key_owner(ib, F_SHUTTER);
    if (oa && ob)     g_settings.cine_shutter = (float)(oa->shutter + (ob->shutter - oa->shutter) * u);
    else if (s_snap) g_settings.cine_shutter = s_save_shutter;

    oa = key_owner(i, F_TIMESCALE); ob = key_owner(ib, F_TIMESCALE);
    if (oa && ob)     g_laws.time_scale = lerp_geom(oa->timescale, ob->timescale, u);
    else if (s_snap) g_laws.time_scale = s_save_timescale;

    /* Focus: a named target wins and is handed to the cinematic renderer,
     * which re-resolves it per frame (so it racks focus as the body moves).
     * Otherwise interpolate the explicit distance geometrically. */
    oa = key_owner(i, F_FOCUS); ob = key_owner(ib, F_FOCUS);
    if (oa && oa->focus_name[0]) {
        cinematic_set_focus_target(oa->focus_name);
    } else if (oa) {
        cinematic_set_focus_target(NULL);
        g_settings.cine_focus_auto = 0;
        g_settings.cine_focus_au   = (ob && !ob->focus_name[0])
            ? (float)lerp_geom(oa->focus_au, ob->focus_au, u)
            : oa->focus_au;
    } else if (s_snap) {
        cinematic_set_focus_target(s_save_focus_name);
        g_settings.cine_focus_auto = s_save_focus_auto;
        g_settings.cine_focus_au   = s_save_focus_au;
    }
}

void cinema_shot_goto_key(int i)
{
    if (i < 0 || i >= s_nkeys) return;
    double pos[3], dir[3];
    key_position(&s_keys[i], pos);
    key_direction(&s_keys[i], pos, dir);
    g_cam.pos[0] = pos[0]; g_cam.pos[1] = pos[1]; g_cam.pos[2] = pos[2];
    g_cam.yaw   = (float)(atan2(dir[2], dir[0]) * (180.0 / PI));
    g_cam.pitch = (float)(asin(clampd(dir[1], -1.0, 1.0)) * (180.0 / PI));
}

/* ------------------------------------------------------------------ subject */

void cinema_body_kind(int i, char *out, size_t n)
{
    const Body *b = &g_bodies[i];
    if (b->is_black_hole) {
        snprintf(out, n, "%s", b->mass > 1.0e5 * SOLAR_MASS_KG
                 ? "Supermassive black hole" : "Black hole");
    } else if (b->is_star) {
        snprintf(out, n, "%s", b->star_phase == STAR_MAIN_SEQUENCE
                 ? "Star" : lifecycle_phase_name(b->star_phase));
    } else if (b->is_comet) {
        snprintf(out, n, "Comet");
    } else if (b->parent >= 0 && !g_bodies[b->parent].is_star &&
               !g_bodies[b->parent].is_black_hole) {
        snprintf(out, n, "Moon");
    } else {
        snprintf(out, n, "Planet");
    }
}

int cinema_shot_anchor_body(double t)
{
    if (s_nkeys < 1) return -1;
    t = clampd(t, s_keys[0].t, s_keys[s_nkeys - 1].t);
    int i = 0;
    while (i < s_nkeys - 1 && t >= s_keys[i + 1].t) i++;
    return s_keys[i].anchor[0] ? body_find_named(s_keys[i].anchor) : -1;
}

int cinema_shot_cut_between(double t0, double t1)
{
    for (int i = 0; i < s_nkeys; i++)
        if (s_keys[i].cut && s_keys[i].t > t0 && s_keys[i].t <= t1) return 1;
    return 0;
}

int cinema_shot_subject(double t, CineSubject *out)
{
    if (s_nkeys < 1) return 0;
    t = clampd(t, s_keys[0].t, s_keys[s_nkeys - 1].t);
    int i = 0;
    while (i < s_nkeys - 1 && t >= s_keys[i + 1].t) i++;
    const CineKey *k = &s_keys[i];
    const char *name = k->subject[0] ? k->subject
                     : (k->look_at[0] ? k->look_at : k->anchor);
    if (!name[0]) return 0;
    int b = body_find_named(name);
    if (b < 0) {
        /* Not a body: static scenery can be a subject too, by the name its
         * catalogue uses ("Lagoon (M8)", "Milky Way"). */
        memset(out, 0, sizeof *out);
        out->body = -1;
        for (int n = 0; n < nebula_count(); n++)
            if (!strcasecmp(nebula_name(n), name)) {
                snprintf(out->name, sizeof out->name, "%s", nebula_name(n));
                snprintf(out->kind, sizeof out->kind, "Nebula");
                nebula_position(n, out->pos_au);
                return 1;
            }
        for (int g = 0; g < galaxy_count(); g++)
            if (!strcasecmp(galaxy_name(g), name)) {
                snprintf(out->name, sizeof out->name, "%s", galaxy_name(g));
                snprintf(out->kind, sizeof out->kind, "Galaxy");
                galaxy_position(g, out->pos_au);
                return 1;
            }
        return 0;                      /* absorbed or a typo: nothing to name */
    }
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", g_bodies[b].name);
    cinema_body_kind(b, out->kind, sizeof out->kind);
    out->body = b;
    return 1;
}

/* ----------------------------------------------------------------- authoring */

int cinema_shot_set(const CineKey *keys, int n, const char *name)
{
    if (!keys || n < 1 || n > CINE_MAX_KEYS) return 0;
    memcpy(s_keys, keys, (size_t)n * sizeof(CineKey));
    s_nkeys = n;
    snprintf(s_name, sizeof s_name, "%s", name && name[0] ? name : "generated");
    cinema_shot_sort();
    return 1;
}

int cinema_shot_get(CineKey *out)
{
    if (!out) return 0;
    memcpy(out, s_keys, (size_t)s_nkeys * sizeof(CineKey));
    return s_nkeys;
}

void cinema_shot_clear(void)
{
    s_nkeys = 0;
    snprintf(s_name, sizeof s_name, "untitled");
}

int cinema_shot_remove_last(void)
{
    if (s_nkeys <= 0) return 0;
    s_nkeys--;
    return 1;
}

int cinema_shot_add_key_here(double dt_after)
{
    if (s_nkeys >= CINE_MAX_KEYS) {
        fprintf(stderr, "[CineCam] shot is full (%d keys)\n", CINE_MAX_KEYS);
        return 0;
    }
    CineKey *k = &s_keys[s_nkeys];
    memset(k, 0, sizeof *k);
    k->t        = s_nkeys == 0 ? 0.0 : s_keys[s_nkeys - 1].t + dt_after;
    frame_cam_sun(k->pos);              /* absolute keys are Sun frame */
    k->yaw      = g_cam.yaw;
    k->pitch    = g_cam.pitch;
    k->fov      = g_settings.fov;
    k->aperture = -1.0f;        /* inherit unless the author sets it */
    k->focus_au = 0.0f;
    k->timescale = -1.0;
    k->shutter   = -1.0f;        /* inherit unless the author sets it */
    k->ease     = CINE_EASE_INOUT;
    s_nkeys++;
    fprintf(stdout, "[CineCam] key %d at t=%.2fs  pos=(%.6g, %.6g, %.6g) "
                    "yaw=%.2f pitch=%.2f\n",
            s_nkeys - 1, k->t, k->pos[0], k->pos[1], k->pos[2],
            (double)k->yaw, (double)k->pitch);
    return 1;
}

/* ------------------------------------------------------------ load and save */

int cinema_shot_load(const char *path)
{
    JsonNode *root = json_parse_file(path);
    if (!root) {
        fprintf(stderr, "[CineCam] cannot parse shot '%s'\n", path);
        return 0;
    }
    JsonNode *keys = json_get(root, "keys");
    if (!keys || keys->type != JSON_ARRAY) {
        fprintf(stderr, "[CineCam] shot '%s' has no \"keys\" array\n", path);
        json_free(root);
        return 0;
    }

    /* Parse into a scratch buffer: a malformed file must not destroy the shot
     * already loaded (which may be one you have been authoring by hand). */
    CineKey tmp[CINE_MAX_KEYS];
    int n = 0;
    for (JsonNode *c = keys->first_child; c && n < CINE_MAX_KEYS; c = c->next) {
        CineKey *k = &tmp[n];
        memset(k, 0, sizeof *k);
        k->t         = json_num(json_get(c, "t"), n == 0 ? 0.0 : tmp[n - 1].t + 5.0);
        k->fov       = (float)json_num(json_get(c, "fov"), 0.0);
        k->aperture  = (float)json_num(json_get(c, "aperture"), -1.0);
        k->yaw       = (float)json_num(json_get(c, "yaw"), 0.0);
        k->pitch     = (float)json_num(json_get(c, "pitch"), 0.0);
        k->timescale = json_num(json_get(c, "timescale"), -1.0);
        k->shutter   = (float)json_num(json_get(c, "shutter"), -1.0);
        k->cut       = json_bool(json_get(c, "cut"), 0);
        k->ease      = ease_from_name(json_str(json_get(c, "ease"), "inout"));

        snprintf(k->anchor,  sizeof k->anchor,  "%s", json_str(json_get(c, "anchor"),  ""));
        snprintf(k->look_at, sizeof k->look_at, "%s", json_str(json_get(c, "look_at"), ""));
        snprintf(k->subject, sizeof k->subject, "%s", json_str(json_get(c, "subject"), ""));
        snprintf(k->detonate, sizeof k->detonate, "%s", json_str(json_get(c, "detonate"), ""));

        /* An anchored key carries an offset; a free key carries a position. */
        JsonNode *p = json_get(c, k->anchor[0] ? "offset" : "pos");
        if (!p) p = json_get(c, "pos");
        for (int a = 0; a < 3; a++)
            k->pos[a] = json_num(json_idx(p, a), 0.0);

        /* "focus" is a body name or a number, so it is typed at parse time. */
        JsonNode *f = json_get(c, "focus");
        if (f && f->type == JSON_STRING)
            snprintf(k->focus_name, sizeof k->focus_name, "%s", json_str(f, ""));
        else if (f && f->type == JSON_NUMBER)
            k->focus_au = (float)f->number;
        n++;
    }
    if (n < 2) {
        fprintf(stderr, "[CineCam] shot '%s' needs at least 2 keys (found %d)\n",
                path, n);
        json_free(root);
        return 0;
    }

    memcpy(s_keys, tmp, (size_t)n * sizeof(CineKey));
    s_nkeys = n;
    snprintf(s_name, sizeof s_name, "%s", json_str(json_get(root, "name"), "untitled"));
    cinema_shot_sort();
    json_free(root);

    fprintf(stdout, "[CineCam] loaded '%s' — %d keys, %.2fs\n",
            s_name, s_nkeys, cinema_shot_duration());
    return 1;
}

int cinema_shot_save(const char *path)
{
    if (s_nkeys < 1) {
        fprintf(stderr, "[CineCam] nothing to save (no keys)\n");
        return 0;
    }
    /* Create the parent directory if it is missing, so saving a first shot
     * from the menu does not require a mkdir in the shell first. */
    {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir(dir, 0755); }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[CineCam] cannot write '%s'\n", path);
        return 0;
    }
    fprintf(f, "{\n  \"name\": \"%s\",\n  \"keys\": [\n", s_name);
    for (int i = 0; i < s_nkeys; i++) {
        const CineKey *k = &s_keys[i];
        fprintf(f, "    { \"t\": %.4f", k->t);
        if (k->anchor[0]) {
            fprintf(f, ", \"anchor\": \"%s\", \"offset\": [%.10g, %.10g, %.10g]",
                    k->anchor, k->pos[0], k->pos[1], k->pos[2]);
        } else {
            fprintf(f, ", \"pos\": [%.10g, %.10g, %.10g]",
                    k->pos[0], k->pos[1], k->pos[2]);
        }
        if (k->look_at[0]) fprintf(f, ", \"look_at\": \"%s\"", k->look_at);
        else               fprintf(f, ", \"yaw\": %.4f, \"pitch\": %.4f",
                                   (double)k->yaw, (double)k->pitch);
        if (k->fov > 0.0f)       fprintf(f, ", \"fov\": %.3f", (double)k->fov);
        if (k->aperture >= 0.0f) fprintf(f, ", \"aperture\": %.3f", (double)k->aperture);
        if (k->focus_name[0])    fprintf(f, ", \"focus\": \"%s\"", k->focus_name);
        else if (k->focus_au > 0.0f) fprintf(f, ", \"focus\": %.6g", (double)k->focus_au);
        if (k->timescale >= 0.0) fprintf(f, ", \"timescale\": %.6g", k->timescale);
        if (k->shutter   >= 0.0f) fprintf(f, ", \"shutter\": %.3f", (double)k->shutter);
        if (k->cut)               fprintf(f, ", \"cut\": true");
        if (k->subject[0])        fprintf(f, ", \"subject\": \"%s\"", k->subject);
        if (k->detonate[0])       fprintf(f, ", \"detonate\": \"%s\"", k->detonate);
        fprintf(f, ", \"ease\": \"%s\" }%s\n", ease_name(k->ease),
                i == s_nkeys - 1 ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stdout, "[CineCam] saved %d keys -> %s\n", s_nkeys, path);
    return 1;
}
