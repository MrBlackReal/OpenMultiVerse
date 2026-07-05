/*
 * starsys.c — procedural star → real system promotion. See starsys.h.
 *
 * The first half of this file is a float-exact CPU port of the hash/noise/
 * density pipeline in assets/shaders/galaxy_stars.vert — the two MUST stay
 * in sync (same constants, same order of operations) or the promoted body
 * appears next to, instead of in place of, the point sprite it replaces.
 */
#include "starsys.h"
#include "body.h"
#include "universe.h"
#include "physics.h"
#include "galaxy.h"
#include "laws.h"
#include "lifecycle.h"   /* SOLAR_MASS_KG */
#include "spectral.h"
#include "trails.h"
#include "labels.h"
#include "collision.h"
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define AU_PER_LY      63241.077
#define SS_CELL_LY     2.0        /* finest cascade cell — sync GS_CELL_LY[0] */
#define SS_PER_CELL    5          /* sync GS_PER_CELL                         */
#define SS_ACCEPT      2.4f       /* sync the shader's density accept scale   */
#define SS_ENTER_FRAC  1.35       /* sync GS_ENTER_FRAC (galaxy.c)            */
#define SS_PROMOTE_AU  (1.1 * AU_PER_LY)   /* promote inside this radius      */
#define SS_DEMOTE_AU   (2.6 * AU_PER_LY)   /* demote beyond it (hysteresis)   */
#define SS_MAX         8          /* concurrently promoted systems            */
#define SS_MAX_BODIES  8          /* star + up to 7 planets                   */
#define SS_TICK_SEC    0.15       /* promotion scan throttle                  */
/* No promotion while cruising: a promoted star instantly becomes an
 * adaptive-warp anchor, so promoting mid-flyby slams the effective warp
 * speed ~1000x and the star demotes again moments later (churn). Anything
 * at or below deliberate approach speed (the warp slider tops out at
 * ~1 ly/s) still promotes; galaxy-crossing cruise (~125 ly/s floor) never
 * does. */
#define SS_MAX_SCAN_SPEED_AU_S  (1.5 * AU_PER_LY)

typedef struct {
    int    active;
    int    gal;                  /* galaxy index                          */
    long   cx, cy, cz;           /* lattice cell (galaxy frame)           */
    int    sub;                  /* candidate index within the cell       */
    int    body[SS_MAX_BODIES];  /* g_bodies indices (body[0] = the star) */
    int    nbody;
    double pos_au[3];            /* star world position, AU               */
    char   name[32];             /* star name (slot-reuse guard)          */
} Promoted;

static Promoted s_prom[SS_MAX];
static double   s_since_scan = 1e9;
static int      s_enabled    = 1;

/* ── shader port: hashes / noise / density (keep bit-for-bit in step) ────── */

static float fract1(float x) { return x - floorf(x); }

static float hash13f(float x, float y, float z)
{
    float px = fract1(x * 0.1031f);
    float py = fract1(y * 0.1031f);
    float pz = fract1(z * 0.1031f);
    float d  = px * (py + 31.32f) + py * (pz + 31.32f) + pz * (px + 31.32f);
    px += d; py += d; pz += d;
    return fract1((px + py) * pz);
}

static void hash33f(float x, float y, float z, float out[3])
{
    float px = fract1(x * 0.1031f);
    float py = fract1(y * 0.1030f);
    float pz = fract1(z * 0.0973f);
    float d  = px * (py + 33.33f) + py * (px + 33.33f) + pz * (pz + 33.33f);
    px += d; py += d; pz += d;
    out[0] = fract1((px + py) * pz);
    out[1] = fract1((px + px) * py);
    out[2] = fract1((py + px) * px);
}

static float vnoisef(float x, float y, float z)
{
    float ix = floorf(x), iy = floorf(y), iz = floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float c000 = hash13f(ix,        iy,        iz);
    float c100 = hash13f(ix + 1.0f, iy,        iz);
    float c010 = hash13f(ix,        iy + 1.0f, iz);
    float c110 = hash13f(ix + 1.0f, iy + 1.0f, iz);
    float c001 = hash13f(ix,        iy,        iz + 1.0f);
    float c101 = hash13f(ix + 1.0f, iy,        iz + 1.0f);
    float c011 = hash13f(ix,        iy + 1.0f, iz + 1.0f);
    float c111 = hash13f(ix + 1.0f, iy + 1.0f, iz + 1.0f);
    float x00 = c000 + (c100 - c000) * fx;
    float x10 = c010 + (c110 - c010) * fx;
    float x01 = c001 + (c101 - c001) * fx;
    float x11 = c011 + (c111 - c011) * fx;
    float y0  = x00 + (x10 - x00) * fy;
    float y1  = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}

static float fbm3f(float x, float y, float z)
{
    float v = vnoisef(x, y, z) * 0.5f;
    x = x * 2.03f + 3.7f; y = y * 2.03f + 1.9f; z = z * 2.03f + 2.6f;
    v += vnoisef(x, y, z) * 0.25f;
    x = x * 2.03f + 1.9f; y = y * 2.03f + 4.2f; z = z * 2.03f + 2.1f;
    v += vnoisef(x, y, z) * 0.125f;
    return v / 0.875f;
}

static float fbm2f(float x, float y, float z)
{
    float v = vnoisef(x, y, z) * 0.6f;
    x = x * 2.11f + 4.1f; y = y * 2.11f + 2.3f; z = z * 2.11f + 3.4f;
    v += vnoisef(x, y, z) * 0.3f;
    return v / 0.9f;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float norm3(float v[3])
{
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return l;
}

/* Port of galaxy_stars.vert star_density(): emission density at unit-sphere
 * position p for galaxy `gal` (no colour outputs — acceptance only). */
static float density_cpu(int gal, const float p[3], float rr, float time_s)
{
    int   type = galaxy_type(gal);
    float seed = galaxy_seed(gal);
    float sv0 = seed * 7.0f, sv1 = seed * 3.0f, sv2 = -seed * 5.0f;

    if (type == 1)                                   /* ELLIPTICAL */
        return expf(-powf(rr / 0.42f, 0.62f) * 3.2f) * 1.5f;

    float axis[3];
    galaxy_axis(gal, axis);
    float h  = p[0]*axis[0] + p[1]*axis[1] + p[2]*axis[2];
    float pr[3] = { p[0] - axis[0]*h, p[1] - axis[1]*h, p[2] - axis[2]*h };
    float r  = sqrtf(pr[0]*pr[0] + pr[1]*pr[1] + pr[2]*pr[2]);

    float ref[3] = { 0.31f, 1.0f, 0.71f };
    float t1[3], t2[3];
    cross3(axis, ref, t1); norm3(t1);
    cross3(axis, t1, t2);

    if (type == 2) {                                 /* IRREGULAR */
        /* Keep in step with galaxy_stars.vert: lump-warped envelope,
         * off-centre stellar bar, patchy clumps + HII complexes. */
        float x1 = pr[0]*t1[0] + pr[1]*t1[1] + pr[2]*t1[2];
        float x2 = pr[0]*t2[0] + pr[1]*t2[1] + pr[2]*t2[2];
        float lump = fbm2f(p[0]*2.1f + sv0*1.7f, p[1]*2.1f + sv1*1.7f,
                           p[2]*2.1f + sv2*1.7f);
        float env  = expf(-powf(r / (0.42f + 0.30f * lump), 2.2f)
                          - powf(h / 0.30f, 2.0f));
        float bar  = 1.5f * expf(-powf((x1 - 0.07f) / 0.34f, 2.0f)
                                 - powf( x2          / 0.115f, 2.0f)
                                 - powf( h           / 0.13f,  2.0f));
        float n = fbm3f(p[0]*3.2f + sv0, p[1]*3.2f + sv1, p[2]*3.2f + sv2);
        float k = (n - 0.48f) / (0.85f - 0.48f);
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        float hn  = fbm2f(p[0]*4.6f - sv0, p[1]*4.6f - sv1, p[2]*4.6f - sv2);
        float hii = (hn - 0.68f) / (0.86f - 0.68f);
        if (hii < 0.0f) hii = 0.0f;
        if (hii > 1.0f) hii = 1.0f;
        hii = hii * hii * (3.0f - 2.0f * hii);
        return env * (0.05f + 1.9f * k * k + 2.6f * hii) + bar;
    }

    /* SPIRAL */
    float phi = atan2f(pr[0]*t2[0] + pr[1]*t2[1] + pr[2]*t2[2],
                       pr[0]*t1[0] + pr[1]*t1[1] + pr[2]*t1[2]);

    float rot  = time_s * 0.010f / (r > 0.10f ? r : 0.10f);
    float ph   = phi + rot;
    float wind = logf(r > 0.035f ? r : 0.035f) * 3.6f;
    float armw = ph * 2.0f - wind;
    float arm  = powf(0.5f + 0.5f * cosf(armw), 2.6f);

    float rimf = (1.0f - rr) / (1.0f - 0.85f);       /* smoothstep(1,.85,rr) */
    if (rimf < 0.0f) rimf = 0.0f;
    if (rimf > 1.0f) rimf = 1.0f;
    rimf = rimf * rimf * (3.0f - 2.0f * rimf);
    float disc  = expf(-r / 0.30f)
                * expf(-fabsf(h) / (0.035f + 0.09f * r * r)) * rimf;
    float bulge = 2.4f * expf(-powf(rr / 0.14f, 2.0f));

    float cr = cosf(rot), sr = sinf(rot);
    float axp[3];
    cross3(axis, pr, axp);
    float prot[3] = { pr[0]*cr + axp[0]*sr + axis[0]*h,
                      pr[1]*cr + axp[1]*sr + axis[1]*h,
                      pr[2]*cr + axp[2]*sr + axis[2]*h };
    float n  = fbm3f(prot[0]*4.6f + sv0, prot[1]*4.6f + sv1, prot[2]*4.6f + sv2);
    float kn = (n - 0.55f) / (0.88f - 0.55f);
    if (kn < 0.0f) kn = 0.0f;
    if (kn > 1.0f) kn = 1.0f;
    kn = kn * kn * (3.0f - 2.0f * kn);
    kn *= arm;

    /* Star-cloud mottling — same factor as galaxy_stars.vert/galaxy.frag. */
    float cloud = 0.60f + 0.80f * fbm2f(prot[0]*3.1f + sv0*1.3f,
                                        prot[1]*3.1f + sv1*1.3f,
                                        prot[2]*3.1f + sv2*1.3f);

    return disc * cloud * (0.38f + 2.8f * arm + 3.8f * kn) + bulge;
}

/* ── deterministic system generator ──────────────────────────────────────── */

static unsigned s_rng;

static unsigned rng_u32(void)
{
    s_rng += 0x9e3779b9u;                       /* splitmix32 */
    unsigned z = s_rng;
    z ^= z >> 16; z *= 0x21f0aaadu;
    z ^= z >> 15; z *= 0x735a2d97u;
    z ^= z >> 15;
    return z;
}

static double rng01(void) { return rng_u32() / 4294967296.0; }

static void rng_seed_cell(int gal, long cx, long cy, long cz, int sub)
{
    s_rng = (unsigned)(cx * 2654435761u)
          ^ (unsigned)(cy * 2246822519u)
          ^ (unsigned)(cz * 3266489917u)
          ^ (unsigned)(sub * 668265263u)
          ^ (unsigned)(gal * 374761393u);
}

/* Skybox↔procedural crossfade gain — keep in sync with render.c sf_fade:
 * promotion only where the procedural stars are actually the visible sky. */
static float crossfade_gain(const double cam_au[3])
{
    double cd = sqrt(cam_au[0]*cam_au[0] + cam_au[1]*cam_au[1]
                   + cam_au[2]*cam_au[2]);
    if (cd <= 3.0e6) return 0.0f;
    float t = (float)((log10(cd) - 6.477) / 2.0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void demote(Promoted *p)
{
    /* Slot-reuse guard: only kill bodies that are still ours (a promoted
     * planet may have been absorbed in a collision and its slot recycled). */
    for (int i = 0; i < p->nbody; i++) {
        int idx = p->body[i];
        if (idx < 0 || idx >= g_nbodies || !g_bodies[idx].alive) continue;
        if (i == 0 && strncmp(g_bodies[idx].name, p->name, sizeof(p->name)))
            continue;
        if (i > 0 && strncmp(g_bodies[idx].name, p->name, strlen(p->name)))
            continue;
        g_bodies[idx].alive = 0;
    }
    physics_mark_timestep_dirty();   /* bodies removed — rebuild timestep model */
    fprintf(stdout, "[StarSys] demoted '%s' (%d bodies)\n", p->name, p->nbody);
    p->active = 0;
}

static void promote(int gal, long cx, long cy, long cz, int sub,
                    const double star_au[3])
{
    Promoted *p = NULL;
    for (int i = 0; i < SS_MAX; i++)
        if (!s_prom[i].active) { p = &s_prom[i]; break; }
    if (!p) return;

    memset(p, 0, sizeof(*p));
    p->gal = gal; p->cx = cx; p->cy = cy; p->cz = cz; p->sub = sub;
    p->pos_au[0] = star_au[0];
    p->pos_au[1] = star_au[1];
    p->pos_au[2] = star_au[2];
    snprintf(p->name, sizeof(p->name), "OMV %ld.%ld.%ld.%d", cx, cy, cz, sub);

    /* Star mass from the same luminosity hash the shader brightens it with
     * (L ≈ M^3.5 main-sequence), so a brilliant sprite becomes a big star. */
    float cfx = (float)cx, cfy = (float)cy, cfz = (float)cz;
    float gseed = galaxy_seed(gal);
    float hlum = hash13f(cfx*3.1f + (float)sub*17.3f - gseed,
                         cfy*3.1f + (float)sub*17.3f - gseed,
                         cfz*3.1f + (float)sub*17.3f - gseed);
    double lum  = 0.04 + 260.0 * pow((double)hlum, 7.0);
    double msun = pow(lum, 1.0 / 3.5);
    if (msun < 0.08) msun = 0.08;
    if (msun > 40.0) msun = 40.0;

    rng_seed_cell(gal, cx, cy, cz, sub);

    BodyCreateSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name    = p->name;
    spec.mass    = msun * SOLAR_MASS_KG;
    spec.radius  = pow(msun, 0.8) * 6.957e8;
    spec.pos[0]  = star_au[0] * AU;
    spec.pos[1]  = star_au[1] * AU;
    spec.pos[2]  = star_au[2] * AU;
    spec.is_star = 1;
    spec.parent  = -1;
    spec.rotation_rate = 2.9e-6 * (0.5 + 1.5 * rng01());
    spec.col[0] = spec.col[1] = spec.col[2] = 1.0f;

    int star = universe_add_body(&spec);
    if (star < 0) { p->active = 0; return; }
    p->body[p->nbody++] = star;

    /* Physical colour from the spectral pipeline (mass → T_eff → blackbody),
     * the same source of truth the RadianceField uses. */
    {
        double t = spectral_t_eff(&g_bodies[star]);
        if (t > 0.0) spectral_blackbody_rgb(t, g_bodies[star].col);
    }
    trails_add_body(star);
    trails_reset_body(star);
    labels_add_body(star);
    collision_on_body_added(star);

    /* Deterministic planets: circular orbits near the galactic disc plane,
     * rocky inside the snow line, giants outside. */
    int nplan = (int)(rng01() * (double)(SS_MAX_BODIES - 2));   /* 0..6 */
    double snow_au = 2.7 * sqrt(lum);

    float axis[3]; galaxy_axis(gal, axis);
    float nrm[3] = { axis[0] + 0.35f * (float)(rng01() - 0.5),
                     axis[1] + 0.35f * (float)(rng01() - 0.5),
                     axis[2] + 0.35f * (float)(rng01() - 0.5) };
    norm3(nrm);
    float ref[3] = { 0.31f, 1.0f, 0.71f };
    float e1[3], e2[3];
    cross3(nrm, ref, e1); norm3(e1);
    cross3(nrm, e1, e2);

    for (int i = 0; i < nplan; i++) {
        double a_au  = 0.4 * pow(1.9, i) * (0.75 + 0.5 * rng01());
        int    rocky = a_au < snow_au;
        double re    = rocky ? 0.4 + 1.4 * rng01() : 4.0 + 7.0 * rng01();
        double r_m   = re * 6.371e6;
        double rho   = rocky ? 5500.0 : 1300.0;
        double m_kg  = (4.0 / 3.0) * PI * r_m * r_m * r_m * rho;
        double a_m   = a_au * AU;
        double v     = sqrt(g_laws.G * spec.mass / a_m);
        double phi   = rng01() * 2.0 * PI;
        double cp = cos(phi), sp = sin(phi);

        char pname[32];
        snprintf(pname, sizeof(pname), "%s %c", p->name, 'b' + i);

        BodyCreateSpec ps;
        memset(&ps, 0, sizeof(ps));
        ps.name   = pname;
        ps.mass   = m_kg;
        ps.radius = r_m;
        ps.parent = star;
        for (int k = 0; k < 3; k++) {
            ps.pos[k] = spec.pos[k] + (e1[k]*cp + e2[k]*sp) * a_m;
            ps.vel[k] = (-e1[k]*sp + e2[k]*cp) * v;
        }
        ps.obliquity     = rng01() * 30.0;
        ps.rotation_rate = 7.0e-5 * (0.3 + rng01());
        if (rocky) {
            double t = rng01();
            ps.col[0] = (float)(0.55 + 0.30 * t);
            ps.col[1] = (float)(0.45 + 0.20 * t);
            ps.col[2] = (float)(0.40 + 0.15 * (1.0 - t));
            /* temperate-zone worlds get a thin blue atmosphere */
            if (a_au > 0.7 * sqrt(lum) && a_au < 1.6 * sqrt(lum)) {
                ps.col[0] = 0.35f; ps.col[1] = 0.50f; ps.col[2] = 0.75f;
                ps.atm_color[0] = 0.45f;
                ps.atm_color[1] = 0.65f;
                ps.atm_color[2] = 1.00f;
                ps.atm_intensity = 0.6f;
                ps.atm_scale     = 1.0f;
            }
        } else {
            double t = rng01();
            ps.col[0] = (float)(0.70 + 0.25 * t);
            ps.col[1] = (float)(0.62 + 0.18 * t);
            ps.col[2] = (float)(0.50 + 0.30 * (1.0 - t));
        }

        int idx = universe_add_body(&ps);
        if (idx < 0) break;
        p->body[p->nbody++] = idx;
        trails_add_body(idx);
        trails_reset_body(idx);
        labels_add_body(idx);
        collision_on_body_added(idx);
    }

    p->active = 1;
    fprintf(stdout,
            "[StarSys] promoted '%s' (%.2f Msun, %d planets) in %s "
            "at %.12g,%.12g,%.12g AU\n",
            p->name, msun, p->nbody - 1, galaxy_name(gal),
            star_au[0], star_au[1], star_au[2]);
}

/* ── public API ──────────────────────────────────────────────────────────── */

void starsys_reset(void)
{
    memset(s_prom, 0, sizeof(s_prom));
    s_since_scan = 1e9;
}

void starsys_set_enabled(int enabled) { s_enabled = enabled ? 1 : 0; }
int  starsys_enabled(void)            { return s_enabled; }

int starsys_suppressed(int gal, int out[][4], int max)
{
    int n = 0;
    for (int i = 0; i < SS_MAX && n < max; i++) {
        if (!s_prom[i].active || s_prom[i].gal != gal) continue;
        out[n][0] = (int)s_prom[i].cx;
        out[n][1] = (int)s_prom[i].cy;
        out[n][2] = (int)s_prom[i].cz;
        out[n][3] = s_prom[i].sub;
        n++;
    }
    return n;
}

void starsys_tick(const double cam_au[3], float time_s)
{
    float gain = crossfade_gain(cam_au);

    /* Demotions run every tick (cheap): out of range, back in the catalogue
     * zone where procedural stars aren't the visible sky, or feature off. */
    for (int i = 0; i < SS_MAX; i++) {
        if (!s_prom[i].active) continue;
        double dx = s_prom[i].pos_au[0] - cam_au[0];
        double dy = s_prom[i].pos_au[1] - cam_au[1];
        double dz = s_prom[i].pos_au[2] - cam_au[2];
        double d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > SS_DEMOTE_AU * SS_DEMOTE_AU || gain < 0.98f || !s_enabled)
            demote(&s_prom[i]);
    }
    if (!s_enabled) return;

    /* Promotion scan, throttled: enumerate finest-cascade candidates in the
     * 3×3×3 cell neighbourhood around the camera, exactly like the shader. */
    {
        static float s_last_time = -1.0f;
        float dt = (s_last_time < 0.0f) ? (float)SS_TICK_SEC
                                        : time_s - s_last_time;
        if (dt < 0.0f) dt = (float)SS_TICK_SEC;
        s_last_time = time_s;
        s_since_scan += dt;
    }
    if (s_since_scan < SS_TICK_SEC) return;

    /* Camera speed over the scan interval — skip the scan while cruising
     * (see SS_MAX_SCAN_SPEED_AU_S). Stop, or slow to approach speed, and
     * the nearby stars materialise within one scan tick. */
    {
        static double s_last_cam[3];
        static int    s_have_last = 0;
        double vx = cam_au[0] - s_last_cam[0];
        double vy = cam_au[1] - s_last_cam[1];
        double vz = cam_au[2] - s_last_cam[2];
        double v  = sqrt(vx*vx + vy*vy + vz*vz) / s_since_scan;
        int had_last = s_have_last;
        s_last_cam[0] = cam_au[0];
        s_last_cam[1] = cam_au[1];
        s_last_cam[2] = cam_au[2];
        s_have_last = 1;
        s_since_scan = 0.0;
        if (!had_last || v > SS_MAX_SCAN_SPEED_AU_S) return;
    }

    if (gain < 0.99f) return;

    const double cell = SS_CELL_LY * AU_PER_LY;

    for (int g = 0; g < galaxy_count(); g++) {
        double gp[3], gr = galaxy_radius_au(g);
        if (gr <= 0.0) continue;
        galaxy_position(g, gp);
        double relx = cam_au[0] - gp[0];
        double rely = cam_au[1] - gp[1];
        double relz = cam_au[2] - gp[2];
        double gd2 = relx*relx + rely*rely + relz*relz;
        if (gd2 > (SS_ENTER_FRAC * gr) * (SS_ENTER_FRAC * gr)) continue;

        long bx = (long)floor(relx / cell);
        long by = (long)floor(rely / cell);
        long bz = (long)floor(relz / cell);
        float gseed = galaxy_seed(g);

        for (long dz = -1; dz <= 1; dz++)
        for (long dy = -1; dy <= 1; dy++)
        for (long dx = -1; dx <= 1; dx++) {
            long cx = bx + dx, cy = by + dy, cz = bz + dz;
            float cfx = (float)cx, cfy = (float)cy, cfz = (float)cz;

            for (int sub = 0; sub < SS_PER_CELL; sub++) {
                /* Same candidate position hash as the shader. */
                float h3[3];
                hash33f(cfx + (float)sub*13.17f + gseed*0.173f,
                        cfy + (float)sub*7.71f  + gseed*0.317f,
                        cfz + (float)sub*3.39f  + gseed*0.531f, h3);
                double sx = gp[0] + ((double)cx + (double)h3[0]) * cell;
                double sy = gp[1] + ((double)cy + (double)h3[1]) * cell;
                double sz = gp[2] + ((double)cz + (double)h3[2]) * cell;

                double ddx = sx - cam_au[0];
                double ddy = sy - cam_au[1];
                double ddz = sz - cam_au[2];
                if (ddx*ddx + ddy*ddy + ddz*ddz >
                    SS_PROMOTE_AU * SS_PROMOTE_AU) continue;

                /* Same existence test as the shader. */
                float p[3] = { (float)((sx - gp[0]) / gr),
                               (float)((sy - gp[1]) / gr),
                               (float)((sz - gp[2]) / gr) };
                float rr = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
                if (rr > 1.0f) continue;
                float hsel = hash13f(cfx*1.7f + (float)sub*41.7f + gseed,
                                     cfy*1.7f + (float)sub*41.7f + gseed,
                                     cfz*1.7f + (float)sub*41.7f + gseed);
                float acc = density_cpu(g, p, rr, time_s) * SS_ACCEPT;
                if (acc > 1.0f) acc = 1.0f;
                if (hsel > acc) continue;

                /* Already promoted? */
                int have = 0, free_slots = 0;
                for (int i = 0; i < SS_MAX; i++) {
                    if (!s_prom[i].active) { free_slots++; continue; }
                    if (s_prom[i].gal == g && s_prom[i].cx == cx &&
                        s_prom[i].cy == cy && s_prom[i].cz == cz &&
                        s_prom[i].sub == sub) { have = 1; break; }
                }
                if (have || !free_slots) continue;

                double star_au[3] = { sx, sy, sz };
                promote(g, cx, cy, cz, sub, star_au);
            }
        }
    }
}
