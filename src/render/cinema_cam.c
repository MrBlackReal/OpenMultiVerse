/*
 * cinema_cam.c — keyframed camera shots (see cinema_cam.h, CINEMATIC.md §9).
 */
#include "cinema_cam.h"
#include "cinematic.h"
#include "camera.h"
#include "body.h"
#include "laws.h"
#include "json.h"

#include <sys/stat.h>
#include <strings.h>

static CineKey s_keys[CINE_MAX_KEYS];
static int     s_nkeys;
static char    s_name[64] = "untitled";

/* Saved state for the settings a shot is allowed to drive, so live playback is
 * reversible (see cinema_shot_begin/end). */
static int    s_playing;
static float  s_save_fov, s_save_aperture, s_save_focus_au;
static int    s_save_focus_auto;
static double s_save_timescale;
static float  s_save_shutter;

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
static void key_position(const CineKey *k, double out[3])
{
    if (k->anchor[0]) {
        int i = body_find_named(k->anchor);
        if (i >= 0) {
            out[0] = g_bodies[i].pos[0] * RS + k->pos[0];
            out[1] = g_bodies[i].pos[1] * RS + k->pos[1];
            out[2] = g_bodies[i].pos[2] * RS + k->pos[2];
            return;
        }
        /* Anchor gone (absorbed, or a typo): fall through to the raw offset
         * rather than snapping the camera to the origin mid-shot. */
    }
    out[0] = k->pos[0]; out[1] = k->pos[1]; out[2] = k->pos[2];
}

static void dir_from_yaw_pitch(float yaw, float pitch, double out[3])
{
    double y = yaw   * (M_PI / 180.0);
    double p = pitch * (M_PI / 180.0);
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
        int i = body_find_named(k->look_at);
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

/* Uniform Catmull-Rom. Key times are arbitrary, but the spline runs on the
 * normalised segment parameter, so uneven spacing changes pacing (which is
 * what the author asked for by placing the keys) without breaking continuity. */
static double catmull(double p0, double p1, double p2, double p3, double u)
{
    double u2 = u * u, u3 = u2 * u;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * u +
                  (2.0*p0 - 5.0*p1 + 4.0*p2 - p3) * u2 +
                  (-p0 + 3.0*p1 - 3.0*p2 + p3) * u3);
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
    if (s_playing) return;
    s_save_fov        = g_settings.fov;
    s_save_aperture   = g_settings.cine_aperture;
    s_save_focus_au   = g_settings.cine_focus_au;
    s_save_focus_auto = g_settings.cine_focus_auto;
    s_save_timescale  = g_laws.time_scale;
    s_save_shutter    = g_settings.cine_shutter;
    s_playing = 1;
}

void cinema_shot_end(void)
{
    if (!s_playing) return;
    g_settings.fov             = s_save_fov;
    g_settings.cine_aperture   = s_save_aperture;
    g_settings.cine_focus_au   = s_save_focus_au;
    g_settings.cine_focus_auto = s_save_focus_auto;
    g_laws.time_scale          = s_save_timescale;
    g_settings.cine_shutter    = s_save_shutter;
    cinematic_set_focus_target(NULL);
    s_playing = 0;
}

static double s_time;

void cinema_shot_play(double from_t)
{
    if (!cinema_shot_active()) {
        fprintf(stderr, "[CineCam] need at least 2 keys to play\n");
        return;
    }
    cinema_shot_begin();
    s_time = from_t;
}

void cinema_shot_stop(void)
{
    cinema_shot_end();
    s_time = 0.0;
}

int    cinema_shot_playing(void) { return s_playing && cinema_shot_active(); }
double cinema_shot_time(void)    { return s_time; }
void   cinema_shot_set_time(double t) { s_time = t; }

void cinema_shot_advance(double dt)
{
    if (!cinema_shot_playing()) return;
    s_time += dt;
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
    if (kb->cut) {
        kb = ka;
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
    for (int c = 0; c < 3; c++)
        pos[c] = catmull(p0[c], p1[c], p2[c], p3[c], u);

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
    g_cam.yaw   = (float)(atan2(dir[2], dir[0]) * (180.0 / M_PI));
    g_cam.pitch = (float)(asin(clampd(dir[1], -1.0, 1.0)) * (180.0 / M_PI));

    /* ---- scalars --------------------------------------------------------- */
    if (ka->fov > 0.0f && kb->fov > 0.0f)
        g_settings.fov = (float)(ka->fov + (kb->fov - ka->fov) * u);
    else if (ka->fov > 0.0f)
        g_settings.fov = ka->fov;

    /* Aperture interpolates GEOMETRICALLY, because f-numbers are a log scale
     * (f/2.8 to f/11 is two stops, and the midpoint a photographer expects is
     * f/5.6, not f/6.9).
     *
     * Zero is special: it means "depth of field off", not "an infinitely wide
     * aperture". Interpolating into it would ramp through f/2, f/1, f/0.1 —
     * blurring harder and harder right up to the moment it switches off, which
     * is the exact opposite of what the author asked for. So a segment with a
     * zero at either end holds the near key's value and switches at the key. */
    if (ka->aperture > 0.0f && kb->aperture > 0.0f)
        g_settings.cine_aperture = (float)lerp_geom(ka->aperture, kb->aperture, u);
    else if (ka->aperture >= 0.0f)
        g_settings.cine_aperture = ka->aperture;

    /* Shutter is keyframable for the same reason fov is: a hyper-fast
     * pull-back sweeps stars across the whole frame in one frame interval, and
     * a 180-degree shutter then smears them into a wall of streaks that N
     * accumulation samples render as N discrete ghosts ("double vision") — no
     * practical sample count fixes it. Closing the shutter for the fast leg
     * shortens the streak instead, which is exactly what a camera operator
     * would do. */
    if (ka->shutter >= 0.0f && kb->shutter >= 0.0f)
        g_settings.cine_shutter = (float)(ka->shutter + (kb->shutter - ka->shutter) * u);
    else if (ka->shutter >= 0.0f)
        g_settings.cine_shutter = ka->shutter;

    if (ka->timescale >= 0.0 && kb->timescale >= 0.0)
        g_laws.time_scale = lerp_geom(ka->timescale, kb->timescale, u);
    else if (ka->timescale >= 0.0)
        g_laws.time_scale = ka->timescale;

    /* Focus: a named target wins and is handed to the cinematic renderer,
     * which re-resolves it per frame (so it racks focus as the body moves).
     * Otherwise interpolate the explicit distance geometrically. */
    if (ka->focus_name[0]) {
        cinematic_set_focus_target(ka->focus_name);
    } else if (ka->focus_au > 0.0f) {
        cinematic_set_focus_target(NULL);
        g_settings.cine_focus_auto = 0;
        g_settings.cine_focus_au   = kb->focus_au > 0.0f
            ? (float)lerp_geom(ka->focus_au, kb->focus_au, u)
            : ka->focus_au;
    }
}

void cinema_shot_goto_key(int i)
{
    if (i < 0 || i >= s_nkeys) return;
    double pos[3], dir[3];
    key_position(&s_keys[i], pos);
    key_direction(&s_keys[i], pos, dir);
    g_cam.pos[0] = pos[0]; g_cam.pos[1] = pos[1]; g_cam.pos[2] = pos[2];
    g_cam.yaw   = (float)(atan2(dir[2], dir[0]) * (180.0 / M_PI));
    g_cam.pitch = (float)(asin(clampd(dir[1], -1.0, 1.0)) * (180.0 / M_PI));
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
    k->pos[0]   = g_cam.pos[0];
    k->pos[1]   = g_cam.pos[1];
    k->pos[2]   = g_cam.pos[2];
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
        fprintf(f, ", \"ease\": \"%s\" }%s\n", ease_name(k->ease),
                i == s_nkeys - 1 ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stdout, "[CineCam] saved %d keys -> %s\n", s_nkeys, path);
    return 1;
}
