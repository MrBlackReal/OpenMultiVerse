/*
 * cinema_titles.c — --cinematic-info overlay (see cinema_titles.h, §11.1).
 */
#include "cinema_titles.h"
#include "cinema_cam.h"
#include "cinema_tour.h"
#include "cinematic.h"
#include "camera.h"
#include "body.h"
#include "laws.h"
#include "physics.h"
#include "settings.h"
#include "ui.h"

#include <string.h>

/* Lower-third timing, seconds of film after the subject changes. The short
 * delay lets a cut land before text appears over it. */
#define T_DELAY  0.6
#define T_IN     0.9
#define T_HOLD   4.5
#define T_OUT    1.2

#define LY_AU    63241.077
#define J2000_JD_DAY0  10957.5   /* 2000-01-01 12:00 as days since 1970-01-01 */

static int         s_enabled;
static CineSubject s_sub;
static int         s_have;
static double      s_age;          /* film seconds since the subject changed */

/* Free camera: a subject must stay the nearest-arrived body this long before
 * it earns a title, so skimming past a moon does not flash one up. */
static char        s_cand[CINE_NAME_LEN];
static double      s_cand_age;

static double      s_prev_pos[3];
static double      s_prev_shot_t = -1.0;
static int         s_prev_valid;
static double      s_speed_c;      /* smoothed camera speed, fraction of c */
static int         s_speed_valid;

void cinema_titles_set_enabled(int on) { s_enabled = on ? 1 : 0; }
int  cinema_titles_enabled(void)       { return s_enabled; }

/* ------------------------------------------------------------ formatting */

/* 1234567 -> "1,234,567" */
static void thousands(double v, char *b, size_t n)
{
    char raw[48];
    snprintf(raw, sizeof raw, "%.0f", v);
    int len = (int)strlen(raw), neg = raw[0] == '-', o = 0;
    for (int i = 0; i < len && o < (int)n - 1; i++) {
        b[o++] = raw[i];
        int rem = len - i - 1;
        if (rem > 0 && rem % 3 == 0 && !(neg && i == 0) && o < (int)n - 1) b[o++] = ',';
    }
    b[o] = 0;
}

/* Large magnitudes in words, which read better on a title card than 1.6e7. */
static void big_words(double v, const char *unit, char *b, size_t n)
{
    char t[48];
    if      (v >= 1e12) snprintf(b, n, "%.1f trillion %s", v / 1e12, unit);
    else if (v >= 1e9)  snprintf(b, n, "%.1f billion %s",  v / 1e9,  unit);
    else if (v >= 1e6)  snprintf(b, n, "%.1f million %s",  v / 1e6,  unit);
    else { thousands(v, t, sizeof t); snprintf(b, n, "%s %s", t, unit); }
}

static void fmt_distance(double au, char *b, size_t n)
{
    char t[48];
    if (au < 0.01) {
        thousands(au * AU / 1000.0, t, sizeof t);
        snprintf(b, n, "%s km", t);
    } else if (au < 2000.0) {
        snprintf(b, n, au < 10.0 ? "%.3f AU" : "%.1f AU", au);
    } else {
        double ly = au / LY_AU;
        if (ly < 100.0) snprintf(b, n, "%.2f light-years", ly);
        else            big_words(ly, "light-years", b, n);
    }
}

/* Camera speed. A percentage of c across the everyday range, multiples of c
 * past it (the camera is a massless observer: nothing stops it), and km/s at
 * orbital speeds, where "0.0006% c" would say nothing. */
static void fmt_speed(double vc, char *b, size_t n)
{
    double c = g_laws.c_light > 0.0 ? g_laws.c_light : 2.99792458e8;
    if (vc < 1e-4) {
        double kms = vc * c / 1000.0;
        if (kms < 10.0) snprintf(b, n, "%.1f km/s", kms);
        else            snprintf(b, n, "%.0f km/s", kms);
    } else if (vc < 0.1) {
        double pct = vc * 100.0;
        snprintf(b, n, pct < 1.0 ? "%.2f%% c" : "%.1f%% c", pct);
    } else if (vc < 10.0) {
        snprintf(b, n, "%.2f c", vc);
    } else {
        big_words(vc, "c", b, n);
    }
}

/* Days since 1970-01-01 -> civil date (H. Hinnant's algorithm). */
static void civil_from_days(long long z, long long *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy  = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp  = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

/* The orbital clock as a date. The Solar System is seeded from J2000.0
 * elements and g_sim_time counts from load, so J2000.0 + g_sim_time is the
 * date its planets are actually showing. Far futures switch to years. */
static void fmt_date(char *b, size_t n)
{
    static const char *mon[12] = { "January", "February", "March", "April",
        "May", "June", "July", "August", "September", "October", "November",
        "December" };
    double days = J2000_JD_DAY0 + g_sim_time / DAY;
    double years = g_sim_time / (365.25 * DAY);
    if (years > 7000.0) {
        char t[48];
        big_words(2000.0 + years, "", t, sizeof t);
        snprintf(b, n, "Year %s", t);
        size_t L = strlen(b);
        while (L > 0 && b[L - 1] == ' ') b[--L] = 0;
        return;
    }
    long long y; int m, d;
    civil_from_days((long long)floor(days), &y, &m, &d);
    snprintf(b, n, "%d %s %lld", d, mon[m - 1], y);
}

/* ------------------------------------------------------------- subject */

static int subject_pos(const CineSubject *s, double out[3])
{
    if (s->body >= 0 && s->body < g_nbodies && g_bodies[s->body].alive) {
        out[0] = g_bodies[s->body].pos[0] * RS;
        out[1] = g_bodies[s->body].pos[1] * RS;
        out[2] = g_bodies[s->body].pos[2] * RS;
        return 1;
    }
    if (s->body >= 0) return 0;          /* was a body, now gone */
    out[0] = s->pos_au[0]; out[1] = s->pos_au[1]; out[2] = s->pos_au[2];
    return 1;
}

static int current_subject(CineSubject *out, double dt)
{
    if (cinema_tour_active())  return cinema_tour_current_subject(out);
    if (cinema_shot_playing()) return cinema_shot_subject(cinema_shot_time(), out);

    /* Free camera: "arrived" = the nearest body is close enough to be the
     * thing on screen, and has stayed so for a second. */
    int b = g_cam_prox.body;
    if (b < 0) { s_cand[0] = 0; return 0; }
    double r = g_bodies[b].radius * RS;
    if (g_bodies[b].is_star) r *= 90.0;          /* frame on the glare */
    if (g_cam_prox.body_dist_au > r * 60.0) { s_cand[0] = 0; return 0; }
    if (strcmp(s_cand, g_bodies[b].name) != 0) {
        snprintf(s_cand, sizeof s_cand, "%s", g_bodies[b].name);
        s_cand_age = 0.0;
    } else {
        s_cand_age += dt;
    }
    if (s_cand_age < 1.0) {
        if (s_have) *out = s_sub;               /* keep the old title meanwhile */
        return s_have;
    }
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", g_bodies[b].name);
    cinema_body_kind(b, out->kind, sizeof out->kind);
    out->body = b;
    return 1;
}

static float title_alpha(double age)
{
    double t = age - T_DELAY;
    if (t <= 0.0) return 0.0f;
    double a;
    if      (t < T_IN)                 a = t / T_IN;
    else if (t < T_IN + T_HOLD)        a = 1.0;
    else if (t < T_IN + T_HOLD + T_OUT) a = 1.0 - (t - T_IN - T_HOLD) / T_OUT;
    else                               a = 0.0;
    return (float)(a * a * (3.0 - 2.0 * a));
}

/* Scale bar: a round length at the subject's depth, ~10% of frame width. */
static float scale_bar(double depth_au, char *label, size_t n)
{
    double tanh_ = tan(g_settings.fov * 0.5 * (PI / 180.0));
    if (depth_au <= 0.0 || tanh_ <= 0.0) return 0.0f;
    double px_per_au = (double)WIN_H / (2.0 * depth_au * tanh_);
    double want_au = (double)WIN_W * 0.10 / px_per_au;

    const char *unit; double per;                 /* AU per unit */
    if (want_au < 0.05)             { unit = "km";          per = 1000.0 / AU; }
    else if (want_au < 5000.0)      { unit = "AU";          per = 1.0; }
    else                            { unit = "light-years"; per = LY_AU; }
    double v = want_au / per;
    double p = pow(10.0, floor(log10(v)));
    double m = v / p >= 5.0 ? 5.0 : (v / p >= 2.0 ? 2.0 : 1.0);
    double nice = m * p;

    if (nice >= 1e6) big_words(nice, unit, label, n);
    else if (nice >= 1.0) { char t[48]; thousands(nice, t, sizeof t); snprintf(label, n, "%s %s", t, unit); }
    else snprintf(label, n, "%g %s", nice, unit);
    return (float)(nice * per * px_per_au);
}

/* ---------------------------------------------------------------- frame */

void cinema_titles_frame(double dt)
{
    if (!s_enabled) return;

    CineSubject cur;
    int have = current_subject(&cur, dt);
    int changed = (have != s_have) || (have && strcmp(cur.name, s_sub.name) != 0);
    if (changed) { s_age = 0.0; }
    else         { s_age += dt; }
    s_have = have;
    if (have) s_sub = cur;

    /* ---- camera speed, per film second, RELATIVE TO THE SUBJECT. An anchored
     * camera rides a body the time-lapse moves thousands of times faster than
     * real (a star's galactic drift at 30 sim-days/s is ~1,700 c): true
     * displacement, meaningless on screen. What the viewer reads as camera
     * speed is motion against what they are looking at. No subject: absolute.
     * Hard cuts are skipped — a jump between subjects is not a velocity. */
    double ref[3] = { 0.0, 0.0, 0.0 };
    if (have) subject_pos(&s_sub, ref);
    double rel[3] = { g_cam.pos[0] - ref[0], g_cam.pos[1] - ref[1], g_cam.pos[2] - ref[2] };
    double st = cinema_shot_playing() ? cinema_shot_time() : -1.0;
    int cut = !s_prev_valid || changed ||
              (st >= 0.0 && s_prev_shot_t >= 0.0 &&
               (st < s_prev_shot_t || cinema_shot_cut_between(s_prev_shot_t, st)));
    if (!cut && dt > 0.0) {
        double dx = rel[0] - s_prev_pos[0];
        double dy = rel[1] - s_prev_pos[1];
        double dz = rel[2] - s_prev_pos[2];
        double c  = g_laws.c_light > 0.0 ? g_laws.c_light : 2.99792458e8;
        double vc = sqrt(dx*dx + dy*dy + dz*dz) * AU / dt / c;
        if (!s_speed_valid) { s_speed_c = vc; s_speed_valid = 1; }
        else {
            /* Smoothed (~0.25 s) so the readout does not flicker frame to
             * frame, but eases over accelerations of many decades quickly:
             * the filter runs in log space. */
            double k = 1.0 - exp(-dt / 0.25);
            double a = log(fmax(s_speed_c, 1e-15)), b = log(fmax(vc, 1e-15));
            s_speed_c = exp(a + (b - a) * k);
        }
    }
    s_prev_pos[0] = rel[0]; s_prev_pos[1] = rel[1]; s_prev_pos[2] = rel[2];
    s_prev_shot_t = st;
    s_prev_valid  = 1;

    float band_top, band_bot;
    cinematic_picture_band(&band_top, &band_bot);

    /* ---- lower-third */
    float a = have ? title_alpha(s_age) : 0.0f;
    if (a > 0.0f) {
        char l2[128], l3[64], dist[64], bar[64] = "";
        double p[3];
        float bar_px = 0.0f;
        l2[0] = 0;
        if (subject_pos(&s_sub, p)) {
            double dx = p[0] - g_cam.pos[0], dy = p[1] - g_cam.pos[1], dz = p[2] - g_cam.pos[2];
            double d = sqrt(dx*dx + dy*dy + dz*dz);
            fmt_distance(d, dist, sizeof dist);
            if (s_sub.kind[0]) snprintf(l2, sizeof l2, "%s  \xC2\xB7  %s away", s_sub.kind, dist);
            else               snprintf(l2, sizeof l2, "%s away", dist);
            bar_px = scale_bar(d, bar, sizeof bar);
        } else if (s_sub.kind[0]) {
            snprintf(l2, sizeof l2, "%s", s_sub.kind);
        }
        fmt_date(l3, sizeof l3);
        ui_cine_title(s_sub.name, l2[0] ? l2 : NULL, l3, a, bar_px, bar, band_top, band_bot);
    }

    /* ---- speed readout: steady while the camera is actually moving */
    if (s_speed_valid) {
        double c = g_laws.c_light > 0.0 ? g_laws.c_light : 2.99792458e8;
        double kms = s_speed_c * c / 1000.0;
        float sa = (float)fmin(1.0, fmax(0.0, (kms - 0.01) / 0.09));  /* hide at rest */
        if (sa > 0.0f) {
            char sp[64];
            fmt_speed(s_speed_c, sp, sizeof sp);
            ui_cine_speed(sp, sa, band_top, band_bot);
        }
    }
}
