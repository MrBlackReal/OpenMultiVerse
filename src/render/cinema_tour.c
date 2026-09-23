/*
 * cinema_tour.c — procedural tour + auto-director (see cinema_tour.h).
 */
#include "cinema_tour.h"
#include "cinema_cam.h"
#include "cinematic.h"
#include "body.h"
#include "camera.h"
#include "universe.h"
#include "nebula.h"
#include "galaxy.h"
#include "field_graph.h"
#include "laws.h"
#include "physics.h"

#include <strings.h>

/* ---- subject model --------------------------------------------------------
 * One entry per thing worth pointing a camera at. Bodies are anchored by NAME
 * (resolved per frame by cinema_cam, so a moving planet is followed and an
 * absorbed one degrades gracefully); nebulae and galaxies are static scenery
 * and are pinned by absolute position. */
typedef enum { SUB_BODY, SUB_STATIC } SubKind;

/* What sort of thing this is, used only to keep the route varied — three
 * black holes in a row is the correct answer by score and a monotonous film. */
typedef enum { CAT_BH, CAT_STAR, CAT_PLANET, CAT_COMET, CAT_NEBULA, CAT_GALAXY,
               CAT_N } SubCat;

typedef struct {
    SubKind kind;
    char    name[CINE_NAME_LEN];
    double  pos[3];        /* AU, absolute — SUB_STATIC only     */
    double  radius_au;     /* physical / bounding radius          */
    double  vis_au;        /* VISUAL extent, what framing uses    */
    double  axis[3];       /* preferred approach axis (0 = any)   */
    double  score;
    SubCat  cat;
    int     is_galaxy;     /* needs the face-on approach         */
} Subject;

#define MAX_SUBJECTS 64

static Subject s_subs[MAX_SUBJECTS];
static int     s_nsubs;

static int      s_tour_on = 1, s_director_on = 1;
static int      s_active;
static double   s_clock;            /* position within the tour shot       */
static double   s_duration;
static char     s_subject[64];

/* Where each leg starts on the tour clock and what it presents, so the title
 * cards (cinema_titles.c) can name the current subject at any moment. */
#define MAX_LEGS 32
static double      s_leg_t0[MAX_LEGS];
static CineSubject s_leg_sub[MAX_LEGS];
static int         s_nlegs;
static CineSubject s_cut_sub;       /* the event a cutaway is covering */

/* Tour shot stashed while a cutaway plays. */
static CineKey  s_tour_keys[CINE_MAX_KEYS];
static int      s_tour_nkeys;
static int      s_cutaway;          /* 1 while the director has the camera */
static double   s_cut_end;          /* cutaway clock at which we go back   */
static double   s_cut_clock;
static double   s_last_event_time;  /* newest event already acted on       */
static int      s_cuts_taken;
static double   s_finale_t;         /* tour clock at which the finale starts */

/* ------------------------------------------------------------------ helpers */

static void subject_to_cine(const Subject *sub, CineSubject *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", sub->name);
    out->body = -1;
    if (sub->kind == SUB_BODY) {
        out->body = body_find_named(sub->name);
        if (out->body >= 0) cinema_body_kind(out->body, out->kind, sizeof out->kind);
    } else {
        out->pos_au[0] = sub->pos[0]; out->pos_au[1] = sub->pos[1]; out->pos_au[2] = sub->pos[2];
        snprintf(out->kind, sizeof out->kind, "%s",
                 sub->is_galaxy ? "Galaxy" : (sub->cat == CAT_NEBULA ? "Nebula" : ""));
    }
}

/* Deterministic per-subject variation: a tour must render identically every
 * run (§5), so orbit phases come from the subject's name, not rand(). */
static double name_phase(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return (double)(h % 3600u) / 3600.0;         /* 0..1 */
}

/* How far back a subject must sit to fill `fill` of the frame height at `fov`
 * degrees. The whole tour frames through this, which is why a moon and a
 * galaxy both end up composed without per-case tuning. */
static double frame_distance(double radius_au, double fov_deg, double fill)
{
    double t = tan(fov_deg * 0.5 * (PI / 180.0));
    if (t < 1e-6) t = 1e-6;
    if (fill < 0.05) fill = 0.05;
    double d = radius_au / (t * fill);
    return d > 1e-9 ? d : 1e-9;
}

/* A unit vector offset from `phase`, tilted above the subject's equator so a
 * planet is lit and a disc is not seen exactly edge-on. */
static void offset_dir(double phase, double out[3])
{
    double a = phase * 2.0 * PI;
    out[0] = cos(a) * 0.86;
    out[1] = 0.38;
    out[2] = sin(a) * 0.86;
    double L = sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    out[0] /= L; out[1] /= L; out[2] /= L;
}

/* Offset direction for a subject that has a preferred axis: mostly along it,
 * tilted `tilt` off so the view is not perfectly degenerate. A galaxy wants
 * this (face-on to the disc); an AGN uses it tilted well off the jet, since
 * looking straight down a beam shows a dot. */
static void offset_dir_axis(const Subject *sub, double phase, double tilt,
                            double out[3])
{
    double ax[3] = { sub->axis[0], sub->axis[1], sub->axis[2] };
    double L = sqrt(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    if (L < 1e-9) { offset_dir(phase, out); return; }
    ax[0] /= L; ax[1] /= L; ax[2] /= L;

    /* Any vector not parallel to the axis gives a perpendicular basis. */
    double t[3] = { 0.0, 1.0, 0.0 };
    if (fabs(ax[1]) > 0.9) { t[0] = 1.0; t[1] = 0.0; }
    double u[3] = { ax[1]*t[2] - ax[2]*t[1],
                    ax[2]*t[0] - ax[0]*t[2],
                    ax[0]*t[1] - ax[1]*t[0] };
    double ul = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    double v[3] = { ax[1]*u[2] - ax[2]*u[1],
                    ax[2]*u[0] - ax[0]*u[2],
                    ax[0]*u[1] - ax[1]*u[0] };

    double a = phase * 2.0 * PI;
    double c = cos(tilt), s2 = sin(tilt);
    for (int i = 0; i < 3; i++)
        out[i] = ax[i] * c + (u[i] * cos(a) + v[i] * sin(a)) * s2;
    double ol = sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    out[0] /= ol; out[1] /= ol; out[2] /= ol;
}

/* The approach direction a subject wants. */
static void subject_dir(const Subject *sub, double phase, double out[3])
{
    int has_axis = (sub->axis[0] || sub->axis[1] || sub->axis[2]);
    if (!has_axis)          { offset_dir(phase, out); return; }
    if (sub->is_galaxy)       offset_dir_axis(sub, phase, 0.30, out); /* face-on */
    else                      offset_dir_axis(sub, phase, 1.35, out); /* off-jet */
}

static void aim_yaw_pitch(const double from[3], const double to[3],
                          float *yaw, float *pitch)
{
    double d[3] = { to[0]-from[0], to[1]-from[1], to[2]-from[2] };
    double L = sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (L < 1e-12) { *yaw = 0.0f; *pitch = 0.0f; return; }
    d[0] /= L; d[1] /= L; d[2] /= L;
    *yaw   = (float)(atan2(d[2], d[0]) * (180.0 / PI));
    double c = d[1] < -1.0 ? -1.0 : (d[1] > 1.0 ? 1.0 : d[1]);
    *pitch = (float)(asin(c) * (180.0 / PI));
}

/* ------------------------------------------------------------ subject survey */

/* Claim a subject slot, keeping the best MAX_SUBJECTS by score.
 *
 * Not simply "append until full": a large universe holds ~16k bodies, so
 * first-come would fill every slot with whatever happened to sit at low
 * indices and the galaxies and nebulae — added last, and the most cinematic
 * things present — would never get in at all. (That is exactly what happened
 * the first time: the survey reported precisely MAX_SUBJECTS bodies and the
 * tour had no finale.) When the list is full the weakest entry is evicted, so
 * the result is the true top-K regardless of insertion order. */
#define CAT_QUOTA (MAX_SUBJECTS / CAT_N)

static Subject *claim_slot(double score, SubCat cat)
{
    if (s_nsubs < MAX_SUBJECTS) return &s_subs[s_nsubs++];

    /* The list is full, so something must go. Evict from the category that is
     * most over its share, not simply the globally weakest entry.
     *
     * Plain global eviction looks correct and destroys the film: a catalogue
     * universe holds thousands of stars scoring ~102, so they evict every
     * nebula (70), planet (60) and comet (45) before the route is even chosen,
     * and the tour can then only offer stars and black holes. Scoring decides
     * which member of a category gets filmed; the quota decides that the
     * category survives at all. */
    int counts[CAT_N];
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < s_nsubs; i++) counts[s_subs[i].cat]++;

    int victim = -1;
    int over   = 0;
    for (int i = 0; i < s_nsubs; i++) {
        int c = (int)s_subs[i].cat;
        int excess = counts[c] - CAT_QUOTA;
        if (c == (int)cat && counts[c] <= CAT_QUOTA) continue;  /* under quota */
        if (excess < over) continue;
        if (excess > over || victim < 0 || s_subs[i].score < s_subs[victim].score) {
            over = excess > 0 ? excess : over;
            victim = i;
        }
    }
    if (victim < 0) return NULL;
    if (s_subs[victim].score >= score && (int)s_subs[victim].cat == (int)cat)
        return NULL;                                   /* nothing to gain */
    return &s_subs[victim];
}

static void add_body_subject(int i, double score, SubCat cat)
{
    Subject *s = claim_slot(score, cat);
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->cat  = cat;
    s->kind = SUB_BODY;
    snprintf(s->name, sizeof s->name, "%s", g_bodies[i].name);
    s->radius_au = g_bodies[i].radius * RS;
    s->score     = score;
    s->is_galaxy = 0;

    /* Visual extent, not physical. What the renderer draws is much larger than
     * the body for most categories, and framing on the physical radius put the
     * camera inside the Sun's corona with the frame blown to white.
     *
     * An ACTIVE nucleus is the extreme case: its jets are deliberately stretched
     * to galaxy scale by agn_visual_scale, so the thing worth filming is kpc
     * across while its horizon is ~1 AU. Framing on the horizon buried the
     * camera inside the host galaxy's volumetric haze, which is why those legs
     * came out as brown fog and black frames. */
    const Body *b = &g_bodies[i];
    double k;
    switch (cat) {
    case CAT_STAR:   k = 90.0;  break;   /* glare + corona       */
    case CAT_COMET:  k = 400.0; break;   /* the tail is the shot */
    case CAT_PLANET: k = 1.6;   break;   /* atmosphere + rings   */
    case CAT_BH:
        k = 25.0;                         /* bare hole: disc + shadow */
        if (body_bh(b)->agn_activity > 0.0f && body_bh(b)->agn_visual_scale > 1.0f)
            k *= (double)body_bh(b)->agn_visual_scale;   /* jets reach galaxy scale */
        break;
    default:         k = 1.0;   break;
    }
    s->vis_au = s->radius_au * k;
    if (body_bh(b)->agn_axis[0] || body_bh(b)->agn_axis[1] || body_bh(b)->agn_axis[2]) {
        /* Jets run along this axis; approaching down it just looks at the
         * beam end-on, so the caller offsets away from it. */
        s->axis[0] = body_bh(b)->agn_axis[0];
        s->axis[1] = body_bh(b)->agn_axis[1];
        s->axis[2] = body_bh(b)->agn_axis[2];
    }
    s->pos[0] = g_bodies[i].pos[0] * RS;
    s->pos[1] = g_bodies[i].pos[1] * RS;
    s->pos[2] = g_bodies[i].pos[2] * RS;
}

static void add_static_subject(const char *name, const double pos[3],
                               double radius_au, double score, int is_galaxy)
{
    Subject *s = claim_slot(score, is_galaxy ? CAT_GALAXY : CAT_NEBULA);
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->cat  = is_galaxy ? CAT_GALAXY : CAT_NEBULA;
    s->kind = SUB_STATIC;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->pos[0] = pos[0]; s->pos[1] = pos[1]; s->pos[2] = pos[2];
    s->radius_au = radius_au;
    s->vis_au    = radius_au;
    s->score     = score;
    s->is_galaxy = is_galaxy;
}

/* Score a body by how much there is to look at. The weights are a judgement
 * about spectacle, not physics: an active black hole with jets is the most
 * cinematic object in the universe, a ringed giant is next, a bare rock is
 * only interesting if nothing else is nearby. */
static double score_body(int i)
{
    const Body *b = &g_bodies[i];
    if (!b->alive) return 0.0;

    double sc = 0.0;
    if (b->is_black_hole) {
        sc = 100.0 + 120.0 * (double)body_bh(b)->agn_activity;   /* jets are the draw */
        if (body_bh(b)->accretion_disk > 0.0f) sc += 25.0;
    } else if (b->is_star) {
        sc = 30.0;
        /* A star with planets is a system you can fly through, not a dot. */
        int children = 0;
        for (int j = 0; j < g_nbodies; j++) {
            if (j >= g_field_star_begin && j < g_field_star_end) {
                j = g_field_star_end - 1; continue;
            }
            if (g_bodies[j].alive && g_bodies[j].parent == i) children++;
        }
        sc += 12.0 * (children > 6 ? 6 : children);
        if (b->star_phase == STAR_RED_GIANT)   sc += 30.0;
        if (b->star_phase == STAR_NEUTRON_STAR) sc += 45.0;
    } else if (b->is_comet) {
        sc = 45.0;                                   /* tails read beautifully */
    } else {
        sc = 14.0;                                   /* planet or moon         */
        int moons = 0;
        for (int j = 0; j < g_nbodies; j++) {
            if (j >= g_field_star_begin && j < g_field_star_end) {
                j = g_field_star_end - 1; continue;
            }
            if (g_bodies[j].alive && g_bodies[j].parent == i) moons++;
        }
        sc += 6.0 * (moons > 4 ? 4 : moons);
        /* Big planets carry rings and banding; small ones are grey dots. */
        if (b->radius > 4.0e7) sc += 22.0;
    }
    return sc;
}

static int cmp_desc(const void *a, const void *b)
{
    double d = ((const Subject *)b)->score - ((const Subject *)a)->score;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

/* Survey everything filmable. Field stars are skipped in O(1): they are frozen
 * scenery with no planets and no physics, and scanning ~260k of them here would
 * dominate the build. */
static void survey(void)
{
    s_nsubs = 0;

    /* Scenery first. It is a handful of entries and the most cinematic content
     * in any universe, so it should never be competing for the last slot. */
    for (int i = 0; i < nebula_count(); i++) {
        double p[3];
        nebula_position(i, p);
        add_static_subject(nebula_name(i), p, nebula_radius_au(i), 70.0, 0);
    }
    for (int i = 0; i < galaxy_count(); i++) {
        double p[3];
        galaxy_position(i, p);
        float ax[3];
        galaxy_axis(i, ax);
        /* Galaxies are the finale, not a stop on the way: scored high so one
         * always survives the cut, and the route puts them last. */
        add_static_subject(galaxy_name(i), p, galaxy_radius_au(i), 150.0, 1);
        /* Approach down the disc axis so the reveal is face-on. Coming in off
         * the axis shows a featureless edge-on sliver — the arms are the whole
         * point of a spiral (§13.1). */
        if (s_nsubs > 0) {
            Subject *g = &s_subs[s_nsubs - 1];
            if (g->is_galaxy) {
                g->axis[0] = ax[0]; g->axis[1] = ax[1]; g->axis[2] = ax[2];
            }
        }
    }

    for (int i = 0; i < g_nbodies; i++) {
        if (i >= g_field_star_begin && i < g_field_star_end) {
            i = g_field_star_end - 1;
            continue;
        }
        if (!g_bodies[i].alive || !g_bodies[i].name[0]) continue;
        double sc = score_body(i);
        if (sc <= 0.0) continue;
        const Body *b = &g_bodies[i];
        SubCat cat = b->is_black_hole ? CAT_BH
                   : b->is_comet      ? CAT_COMET
                   : b->is_star       ? CAT_STAR : CAT_PLANET;
        add_body_subject(i, sc, cat);
    }

    qsort(s_subs, (size_t)s_nsubs, sizeof(Subject), cmp_desc);
}

/* ------------------------------------------------------------------ moves ---
 * Each move appends keys for one leg. They share a shape: place the camera at
 * a framing distance from the subject, aim at it, and let §9.2's interpolation
 * carry the motion. Bodies are anchored by name so the shot tracks them; static
 * scenery is pinned absolutely.
 *
 * Timescale is chosen per leg from the subject's scale, applying §13.1: close
 * to a planet the clock stays near real time, a star system wants enough speed
 * that orbits visibly sweep, and past the system it drops back to 1 because
 * nothing out there animates and sim time is the dominant render cost. */

static void set_frame(CineKey *k, const Subject *sub, const double dir[3],
                      double dist, double fov, double t, CineEase ease)
{
    memset(k, 0, sizeof *k);
    k->t        = t;
    k->fov      = (float)fov;
    k->aperture = -1.0f;
    k->focus_au = 0.0f;
    k->timescale = -1.0;
    k->shutter   = -1.0f;
    k->ease      = ease;

    if (sub->kind == SUB_BODY) {
        snprintf(k->anchor,  sizeof k->anchor,  "%s", sub->name);
        snprintf(k->look_at, sizeof k->look_at, "%s", sub->name);
        k->pos[0] = dir[0] * dist;
        k->pos[1] = dir[1] * dist;
        k->pos[2] = dir[2] * dist;
    } else {
        k->pos[0] = sub->pos[0] + dir[0] * dist;
        k->pos[1] = sub->pos[1] + dir[1] * dist;
        k->pos[2] = sub->pos[2] + dir[2] * dist;
        aim_yaw_pitch(k->pos, sub->pos, &k->yaw, &k->pitch);
    }
}

/* Timescale appropriate to how big the subject's neighbourhood is.
 *
 * Keyed on the VISUAL extent — the size the camera actually frames — not the
 * physical radius. On the physical radius the star branch was unreachable: the
 * Sun is 0.0047 AU, under the 1e-2 cut, so every ordinary star got the planet
 * clock and its system sat visibly still. An AGN, whose visual extent is
 * galaxy-scale, correctly lands in the "nothing moves" branch. */
static double leg_timescale(const Subject *sub)
{
    if (sub->is_galaxy)       return 1.0;
    if (sub->vis_au > 1.0e3)  return 1.0;       /* nebula-scale: nothing moves */
    if (sub->vis_au > 1.0e-2) return 30.0;      /* a star: let orbits sweep    */
    return 6.0;                                  /* a planet or moon           */
}

/* Shutter: close it on the legs where the camera covers a lot of ground, so a
 * star crossing the frame within one frame interval becomes a short streak
 * rather than N discrete ghosts (§15). */
static float leg_shutter(const Subject *sub)
{
    return sub->is_galaxy ? 150.0f : 180.0f;
}

/* APPROACH + ORBIT: arrive from a distance, then swing around the subject.
 * This is the workhorse — it establishes the subject, then shows it has three
 * dimensions. Emits 3 keys. */
static int move_approach_orbit(CineKey *out, const Subject *sub,
                               double t0, double dur, double fov)
{
    double ph = name_phase(sub->name);
    double vr = (sub->vis_au);
    double near_d = frame_distance(vr, fov, sub->is_galaxy ? 0.55 : 0.34);
    double far_d  = near_d * 2.2;

    double d0[3], d1[3], d2[3];
    subject_dir(sub, ph,        d0);
    subject_dir(sub, ph + 0.16, d1);
    subject_dir(sub, ph + 0.34, d2);

    set_frame(&out[0], sub, d0, far_d,  fov, t0,               CINE_EASE_INOUT);
    set_frame(&out[1], sub, d1, near_d, fov, t0 + dur * 0.55,  CINE_EASE_INOUT);
    set_frame(&out[2], sub, d2, near_d * 1.25, fov, t0 + dur,  CINE_EASE_INOUT);

    double ts = leg_timescale(sub);
    float  sh = leg_shutter(sub);
    for (int i = 0; i < 3; i++) { out[i].timescale = ts; out[i].shutter = sh; }

    /* Rack focus onto the subject for the close pass, then open up. A body
     * subject can be focused by name; static scenery cannot move, so it uses
     * the framing distance directly. */
    if (sub->kind == SUB_BODY && sub->radius_au < 1.0e-2) {
        for (int i = 0; i < 3; i++) {
            snprintf(out[i].focus_name, sizeof out[i].focus_name, "%s", sub->name);
            out[i].aperture = (i == 1) ? 4.0f : 8.0f;
        }
    }
    return 3;
}

/* REVEAL: pull back from the subject until its whole neighbourhood is in
 * frame. Used to end a leg and to hand over to the next, larger subject. */
static int move_reveal(CineKey *out, const Subject *sub,
                       double t0, double dur, double fov)
{
    double ph = name_phase(sub->name);
    double d0[3], d1[3];
    subject_dir(sub, ph + 0.34, d0);
    subject_dir(sub, ph + 0.52, d1);
    double near_d = frame_distance((sub->vis_au), fov, 0.42);

    /* How far to pull back. A planet or star sits in a system worth revealing,
     * so a big multiplier is the point. A galaxy is already the widest thing in
     * frame — pulling back 28x just loses it to black, which is a poor way to
     * end a film. */
    double pull = sub->is_galaxy ? 2.2 : 28.0;

    set_frame(&out[0], sub, d0, near_d,        fov, t0,       CINE_EASE_INOUT);
    set_frame(&out[1], sub, d1, near_d * pull, fov, t0 + dur, CINE_EASE_OUT);
    double ts = leg_timescale(sub);
    for (int i = 0; i < 2; i++) { out[i].timescale = ts; out[i].shutter = 120.0f; }
    return 2;
}

/* ------------------------------------------------------------------- route */

int cinema_tour_build(double duration)
{
    if (!s_tour_on) {
        /* Director-only: no spine. Park the camera on the best subject so the
         * film has something on screen between events. */
        survey();
        if (s_nsubs == 0) return 0;
        CineKey k[3];
        int n = move_approach_orbit(k, &s_subs[0], 0.0, duration, 45.0);
        cinema_shot_set(k, n, "director");
        s_tour_nkeys = cinema_shot_get(s_tour_keys);
        s_duration = duration; s_clock = 0.0; s_active = 1;
        s_cutaway = 0; s_cuts_taken = 0;
        s_finale_t = duration;                 /* no finale to protect */
        s_last_event_time = g_sim_time;
        snprintf(s_subject, sizeof s_subject, "%s", s_subs[0].name);
        cinema_shot_play(0.0);
        fprintf(stdout, "[Tour] director-only: holding on %s, waiting for events\n",
                s_subs[0].name);
        return 1;
    }

    survey();
    if (s_nsubs == 0) {
        fprintf(stderr, "[Tour] nothing filmable in this universe\n");
        return 0;
    }

    /* Route selection, in two passes.
     *
     * Pass 1 picks the cast: the highest-scoring subjects, but at most two of
     * any one category. Pure score is monotonous — an active AGN outscores
     * everything, so a universe with several gives three black holes in a row
     * and never shows a planet.
     *
     * Pass 2 orders the cast by distance from the origin, which is what turns
     * a list of subjects into the close -> out -> across -> in journey §10.1
     * asks for. The galaxy finale is appended after, so the film always ends on
     * the largest structure present. */
    /* Budget first: the cast has to be chosen at the size the film can
     * actually show. Selecting ten and then truncating after the distance sort
     * would keep the ten NEAREST rather than the best ones — which silently
     * dropped every AGN in the first version of this. */
    int want_legs = (int)(duration / 9.0);
    if (want_legs < 1)  want_legs = 1;
    if (want_legs > 10) want_legs = 10;

    Subject route[12];
    int nr = 0;
    int used[CAT_N];
    memset(used, 0, sizeof used);
    /* Take at most ONE of each category first, and only start doubling up if
     * there are not enough distinct kinds to fill the film.
     *
     * A flat "two per category" looks fine on paper and reads badly on screen:
     * two active nuclei in a row are different objects at different distances,
     * but both render as a hazy elliptical host with a jet through it, so the
     * film appears to show the same thing twice. Variety of KIND is what the
     * eye reads, not variety of catalogue entry. */
    unsigned char taken[MAX_SUBJECTS];
    memset(taken, 0, sizeof taken);
    for (int pass = 1; pass <= 3 && nr < want_legs; pass++) {
        for (int i = 0; i < s_nsubs && nr < want_legs; i++) {
            if (s_subs[i].is_galaxy || taken[i]) continue;
            if (used[s_subs[i].cat] >= pass) continue;
            used[s_subs[i].cat]++;
            taken[i] = 1;
            route[nr++] = s_subs[i];
        }
    }

    for (int i = 1; i < nr; i++) {            /* insertion sort by |pos| */
        Subject k = route[i];
        double kd = sqrt(k.pos[0]*k.pos[0] + k.pos[1]*k.pos[1] + k.pos[2]*k.pos[2]);
        int j = i - 1;
        while (j >= 0) {
            double jd = sqrt(route[j].pos[0]*route[j].pos[0] +
                             route[j].pos[1]*route[j].pos[1] +
                             route[j].pos[2]*route[j].pos[2]);
            if (jd <= kd) break;
            route[j + 1] = route[j];
            j--;
        }
        route[j + 1] = k;
    }

    int galaxy_idx = -1;
    for (int i = 0; i < s_nsubs; i++)
        if (s_subs[i].is_galaxy) { galaxy_idx = i; break; }

    /* Every leg gets equal screen time; the galaxy finale gets a longer hold
     * because the reveal is the payoff (§13.1). */
    if (want_legs > nr) want_legs = nr;

    double finale = (galaxy_idx >= 0) ? (duration * 0.30) : 0.0;
    double body_time = duration - finale;
    double per_leg = body_time / (double)want_legs;

    CineKey keys[CINE_MAX_KEYS];
    int n = 0;
    double t = 0.0;
    s_nlegs = 0;

    for (int i = 0; i < want_legs && n + 4 < CINE_MAX_KEYS; i++) {
        int leg_start = n;
        double fov = 42.0;
        if (getenv("TOUR_DEBUG")) {
            double R = sqrt(route[i].pos[0]*route[i].pos[0] +
                            route[i].pos[1]*route[i].pos[1] +
                            route[i].pos[2]*route[i].pos[2]);
            /* Mirrors move_approach_orbit's framing (vis_au, far = 2.2x). */
            double nd = frame_distance(route[i].vis_au, fov, 0.34);
            fprintf(stderr, "[tourdbg] %-24s radius_au=%.4g vis_au=%.4g "
                    "|pos|=%.4g near=%.4g far=%.4g ts=%.3g\n", route[i].name,
                    route[i].radius_au, route[i].vis_au, R, nd, nd * 2.2,
                    leg_timescale(&route[i]));
        }
        if (s_nlegs < MAX_LEGS) {
            s_leg_t0[s_nlegs] = t;
            subject_to_cine(&route[i], &s_leg_sub[s_nlegs++]);
        }
        n += move_approach_orbit(&keys[n], &route[i], t, per_leg * 0.92, fov);
        /* Cut into every leg after the first: subjects are light years to
         * megaparsecs apart and a film cuts between them. Flying the gap
         * overshoots (see CineKey.cut) and would be dead screen time anyway. */
        if (i > 0) keys[leg_start].cut = 1;
        t += per_leg;
    }

    if (galaxy_idx >= 0 && n + 4 < CINE_MAX_KEYS) {
        /* Finale: approach the galaxy face-on and hold. */
        Subject *g = &s_subs[galaxy_idx];
        int fin_start = n;
        if (s_nlegs < MAX_LEGS) {
            s_leg_t0[s_nlegs] = t;
            subject_to_cine(g, &s_leg_sub[s_nlegs++]);
        }
        n += move_approach_orbit(&keys[n], g, t, finale * 0.70, 50.0);
        keys[fin_start].cut = 1;
        t += finale * 0.70;
        n += move_reveal(&keys[n], g, t, finale * 0.30, 50.0);
        t += finale * 0.30;
        snprintf(s_subject, sizeof s_subject, "%s", g->name);
    } else if (want_legs > 0) {
        snprintf(s_subject, sizeof s_subject, "%s", route[0].name);
    }

    if (n < 2) {
        fprintf(stderr, "[Tour] could not compose a route\n");
        return 0;
    }
    /* The last key must land exactly on `duration`: film-out renders a fixed
     * frame count, and a shot that ends early would freeze on its last key. */
    keys[n - 1].t = duration;

    if (!cinema_shot_set(keys, n, "Procedural tour")) return 0;
    s_tour_nkeys = cinema_shot_get(s_tour_keys);
    s_duration   = duration;
    s_clock      = 0.0;
    s_active     = 1;
    s_cutaway    = 0;
    s_cuts_taken = 0;
    s_finale_t   = duration - finale;
    /* Only events from here on are news. The log already holds whatever the
     * warm-up produced; without this the director cut to a stale event on the
     * very first frame. */
    s_last_event_time = g_sim_time;
    cinema_shot_play(0.0);

    fprintf(stdout, "[Tour] %d subjects surveyed, %d legs, %d keys, %.1fs%s\n",
            s_nsubs, want_legs + (galaxy_idx >= 0 ? 1 : 0), n, duration,
            s_director_on ? " (+ director)" : "");
    for (int i = 0; i < want_legs; i++)
        fprintf(stdout, "[Tour]   leg %d: %s (score %.0f)\n",
                i, route[i].name, route[i].score);
    if (galaxy_idx >= 0)
        fprintf(stdout, "[Tour]   finale: %s\n", s_subs[galaxy_idx].name);
    return 1;
}

int         cinema_tour_active(void)  { return s_active; }
const char *cinema_tour_subject(void) { return s_subject; }

int cinema_tour_current_subject(CineSubject *out)
{
    if (!s_active) return 0;
    if (s_cutaway) { *out = s_cut_sub; return 1; }
    int i = -1;
    for (int k = 0; k < s_nlegs; k++) if (s_clock >= s_leg_t0[k]) i = k;
    if (i < 0) return 0;
    *out = s_leg_sub[i];
    /* A leg's body can have been absorbed since the build; re-resolve by name
     * rather than trusting the index (g_nbodies slots are reused). */
    if (out->body >= 0) out->body = body_find_named(out->name);
    return 1;
}
void        cinema_tour_shutdown(void){ s_active = 0; s_nsubs = 0; }

void cinema_tour_set_mode(int tour_on, int director_on)
{
    s_tour_on     = tour_on;
    s_director_on = director_on;
}

/* ---------------------------------------------------------------- director --
 *
 * The simulation already keeps an event log — field_graph_events() returns
 * supernovae, mergers, tidal disruptions and phase changes newest-first, each
 * with the position it happened at. That is the signal source §10.2 wanted, so
 * the director is a consumer of existing state rather than a parallel event
 * bus wired into the physics.
 *
 * When something scores highly enough, the tour shot is stashed, a short
 * cutaway is built around the event position and played, and the tour is then
 * restored. The tour clock KEEPS RUNNING underneath the cutaway, so the tour
 * loses the covered screen time rather than being pushed later: --duration
 * renders a fixed frame count, and parking the clock instead (the first
 * version) shifted the whole remaining tour back by CUT_DURATION per cut and
 * silently truncated the finale. For the same reason no cutaway may start once
 * it would run into the finale — the galaxy reveal is the payoff and is never
 * given up for an event.
 */

#define CUT_MIN_SCORE   40.0
#define CUT_DURATION     6.0
#define CUT_MAX_TAKES    6
#define SN_FRAME_AU      40.0   /* ejecta cloud of a giant, ~20 days in  */

static double score_event(const FieldGraphEvent *e)
{
    switch (e->type) {
    case FG_EVENT_SUPERNOVA: return 100.0;   /* the most spectacular thing here */
    case FG_EVENT_TDE:       return 85.0;    /* a hole eating a star            */
    case FG_EVENT_MERGE:     return 60.0;
    case FG_EVENT_PHASE:     return 35.0;    /* below threshold: too quiet      */
    default:                 return 0.0;
    }
}

/* Build a 3-key cutaway that arrives on the event, holds, and drifts. The
 * framing radius is a guess from the event's own scale — a supernova shell is
 * large, a merger is not — because the participants may already be gone by the
 * time this runs (a supernova retires its progenitor). */
static void build_cutaway(const FieldGraphEvent *e, CineKey *out)
{
    Subject sub;
    memset(&sub, 0, sizeof sub);
    sub.kind = SUB_STATIC;
    snprintf(sub.name, sizeof sub.name, "%s", e->a_name);
    sub.pos[0] = e->pos[0] * RS;
    sub.pos[1] = e->pos[1] * RS;
    sub.pos[2] = e->pos[2] * RS;
    sub.radius_au = (e->type == FG_EVENT_SUPERNOVA) ? 3.0 : 0.6;

    /* If a participant is still alive, prefer anchoring to it: the event site
     * drifts with the system, and a static pin would slowly lose the subject. */
    int is_sn = (e->type == FG_EVENT_SUPERNOVA);
    int live = -1;
    if (!is_sn) {
        live = body_find_named(e->a_name);
        if (live < 0 && e->b_name[0]) live = body_find_named(e->b_name);
    }
    /* ...except a supernova: its blast stays pinned at the birth position in
     * world space while the remnant drifts off (supernova.h), so the event
     * site IS the static point, and anchoring to the remnant would drift the
     * camera away from the shell it came to film. */
    if (live >= 0) {
        sub.kind = SUB_BODY;
        snprintf(sub.name, sizeof sub.name, "%s", g_bodies[live].name);
        double r = g_bodies[live].radius * RS;
        if (r > sub.radius_au) sub.radius_au = r;
    }
    /* move_approach_orbit frames on vis_au. Left at the memset's zero it put
     * the camera ~1e-9 AU from the event — inside whatever was there. A live
     * star is drawn inside its glare (see add_body_subject), so frame on that. */
    sub.vis_au = sub.radius_au;
    if (live >= 0 && g_bodies[live].is_star) {
        double glare = g_bodies[live].radius * RS * 90.0;
        if (glare > sub.vis_au) sub.vis_au = glare;
    }

    /* A supernova is framed on the size its ejecta cloud reaches by the END
     * of the cutaway (see the clock below), so the shell grows into a frame
     * the camera is already outside of. Framed on the progenitor's glare
     * instead, the camera sat a few AU from the flash and inside where the
     * cloud was about to be: a white blowout, then grey fog. */
    if (is_sn && sub.vis_au < SN_FRAME_AU) sub.vis_au = SN_FRAME_AU;

    move_approach_orbit(out, &sub, 0.0, CUT_DURATION, 40.0);
    out[0].cut = 1;
    for (int i = 0; i < 3; i++) { out[i].timescale = 1.0; out[i].shutter = 180.0f; }

    /* A supernova unfolds over days to months of ORBITAL sim time: the flash
     * peaks within hours and is gone by ~0.4 day, the fireball lingers a
     * week, the cloud expands for months (supernova.c). The timescale keys
     * multiply g_sim_speed (1 day/s by default, but user-selectable), so the
     * ramp is written in absolute sim-days per second and converted: a slow
     * start so the flash reads, then fast enough that the shell visibly
     * grows. Geometric interpolation (cinema_cam.c) integrates this to ~20
     * sim-days over the cutaway's six seconds. */
    if (is_sn && g_sim_speed > 0.0) {
        const double days_per_s[3] = { 0.1, 3.0, 10.0 };
        for (int i = 0; i < 3; i++)
            out[i].timescale = days_per_s[i] * DAY / g_sim_speed;
    }
}

static void director_poll(void)
{
    if (!s_director_on || s_cutaway || s_cuts_taken >= CUT_MAX_TAKES) return;
    if (s_clock + CUT_DURATION > s_finale_t) return;   /* protect the finale */

    FieldGraphEvent ev[16];
    int n = field_graph_events(ev, 16);
    if (n <= 0) return;

    /* Newest first; take the best event we have not already acted on. */
    const FieldGraphEvent *best = NULL;
    double best_score = 0.0;
    for (int i = 0; i < n; i++) {
        if (ev[i].sim_time_s <= s_last_event_time) continue;
        double sc = score_event(&ev[i]);
        if (sc > best_score) { best_score = sc; best = &ev[i]; }
    }
    if (!best || best_score < CUT_MIN_SCORE) {
        /* Still mark quiet events as seen, so they are not re-examined every
         * frame for the rest of the film. */
        if (n > 0 && ev[0].sim_time_s > s_last_event_time)
            s_last_event_time = ev[0].sim_time_s;
        return;
    }

    CineKey cut[3];
    build_cutaway(best, cut);

    memset(&s_cut_sub, 0, sizeof s_cut_sub);
    snprintf(s_cut_sub.name, sizeof s_cut_sub.name, "%s", best->a_name);
    snprintf(s_cut_sub.kind, sizeof s_cut_sub.kind, "%s", field_graph_event_name(best->type));
    if (s_cut_sub.kind[0] >= 'a' && s_cut_sub.kind[0] <= 'z') s_cut_sub.kind[0] -= 32;
    s_cut_sub.body = -1;
    s_cut_sub.pos_au[0] = best->pos[0] * RS;
    s_cut_sub.pos_au[1] = best->pos[1] * RS;
    s_cut_sub.pos_au[2] = best->pos[2] * RS;

    s_tour_nkeys      = cinema_shot_get(s_tour_keys);   /* stash the spine */
    s_last_event_time = best->sim_time_s;
    s_cut_clock       = 0.0;
    s_cut_end         = CUT_DURATION;
    s_cutaway         = 1;
    s_cuts_taken++;

    cinema_shot_set(cut, 3, "cutaway");
    snprintf(s_subject, sizeof s_subject, "%s (%s)",
             best->a_name, field_graph_event_name(best->type));
    fprintf(stdout, "[Director] cut %d: %s %s at t=%.1fs\n",
            s_cuts_taken, field_graph_event_name(best->type),
            best->a_name, s_clock);
}

static void director_resume(void)
{
    cinema_shot_set(s_tour_keys, s_tour_nkeys, "Procedural tour");
    s_cutaway = 0;
    snprintf(s_subject, sizeof s_subject, "tour");
}

/* ------------------------------------------------------------------- tick */

void cinema_tour_tick(double dt)
{
    if (!s_active) return;

    /* The spine's clock always advances, cutaway or not (see the director
     * comment above): the film's length is fixed, so covered tour time is
     * skipped, never deferred. */
    s_clock += dt;
    if (s_clock > s_duration) s_clock = s_duration;

    if (s_cutaway) {
        s_cut_clock += dt;
        if (s_cut_clock < s_cut_end) {
            cinema_shot_set_time(s_cut_clock);
            return;
        }
        director_resume();                       /* hard cut back to the tour */
    }

    director_poll();
    if (s_cutaway) {                             /* a cut just started */
        cinema_shot_set_time(0.0);
        return;
    }
    cinema_shot_set_time(s_clock);
}
