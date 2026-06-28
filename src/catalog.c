/*
 * catalog.c — convert real astronomical catalogs into universe JSON.
 *
 * See catalog.h. No SDL/OpenGL dependencies: this builds both into the
 * simulator and into the standalone `catalogtool` CLI.
 */
#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ------------------------------------------------------------------ units */
#define CAT_PI        3.14159265358979323846
#define PC_TO_LY      3.2615638        /* parsec -> light-year                */
#define MSUN_KG       1.98892e30
#define RSUN_KM       695700.0
#define MEARTH_KG     5.972e24
#define REARTH_KM     6371.0
#define J2000_EPS     23.4392911       /* mean obliquity of the ecliptic, deg */

/* ------------------------------------------------------------------ I/O */

/* Read one line (without trailing CR/LF) into a freshly malloc'd buffer.
 * Returns NULL at EOF. Caller frees. Grows as needed, so long catalog rows
 * are safe. */
static char *read_line(FILE *f)
{
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    int c;
    if (!buf) return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    if (len > 0 && buf[len - 1] == '\r') buf[len - 1] = '\0';
    return buf;
}

/* In-place trim of leading/trailing ASCII whitespace; returns the start. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Split a CSV line into fields (in place). Handles "quoted, fields" and ""
 * escapes. Trims unquoted fields. Returns field count (capped at maxf). */
static int split_csv(char *line, char **fields, int maxf)
{
    int n = 0;
    char *p = line;
    while (n < maxf) {
        if (*p == '"') {
            char *out = ++p;
            fields[n++] = out;
            while (*p) {
                if (*p == '"' && p[1] == '"') { *out++ = '"'; p += 2; }
                else if (*p == '"')           { p++; break; }
                else                           { *out++ = *p++; }
            }
            *out = '\0';
            if (*p == ',') { p++; continue; }
            break;
        } else {
            char *start = p;
            while (*p && *p != ',') p++;
            int more = (*p == ',');
            *p = '\0';
            fields[n++] = trim(start);
            if (more) { p++; continue; }
            break;
        }
    }
    return n;
}

/* Lowercase a copy in place. */
static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* Find the column index whose (lowercased) header equals `name`, or -1. */
static int col_index(char **hdr, int nh, const char *name)
{
    for (int i = 0; i < nh; i++)
        if (hdr[i] && strcmp(hdr[i], name) == 0) return i;
    return -1;
}

/* Field value by column index, or "" if out of range. */
static const char *field(char **f, int nf, int idx)
{
    if (idx < 0 || idx >= nf) return "";
    return f[idx];
}

/* Numeric field, or NAN if absent/empty (so callers can test isnan()). */
static double field_num(char **f, int nf, int idx)
{
    const char *s = field(f, nf, idx);
    if (!s || !s[0]) return NAN;
    char *end;
    double v = strtod(s, &end);
    if (end == s) return NAN;
    return v;
}

/* ------------------------------------------------------------------ astro */

/* Equatorial J2000 (ra,dec in deg) + distance (ly) -> simulation GL/ecliptic
 * light-year coordinates, matching starfield.c's equatorial_to_gl(). */
static void eq_to_ecliptic_ly(double ra_deg, double dec_deg, double dist_ly,
                              double out[3])
{
    const double d = CAT_PI / 180.0;
    const double eps = J2000_EPS * d;
    double ra = ra_deg * d, dec = dec_deg * d;
    double ce = cos(eps), se = sin(eps);
    double xe = cos(dec) * cos(ra), ye = cos(dec) * sin(ra), ze = sin(dec);
    double x_ecl = xe;
    double y_ecl = ye * ce + ze * se;
    double z_ecl = -ye * se + ze * ce;
    out[0] = x_ecl * dist_ly;  /* GL X = ecliptic X */
    out[1] = z_ecl * dist_ly;  /* GL Y = ecliptic Z */
    out[2] = y_ecl * dist_ly;  /* GL Z = ecliptic Y */
}

/* Rough blackbody star colour from effective temperature (K). */
static void teff_color(double t, double rgb[3])
{
    if (isnan(t) || t <= 0.0) t = 5772.0;            /* default: Sun-like */
    if      (t < 3500.0) { rgb[0]=1.00; rgb[1]=0.55; rgb[2]=0.35; }
    else if (t < 5000.0) { rgb[0]=1.00; rgb[1]=0.75; rgb[2]=0.50; }
    else if (t < 6000.0) { rgb[0]=1.00; rgb[1]=0.95; rgb[2]=0.82; }
    else if (t < 7500.0) { rgb[0]=0.95; rgb[1]=0.97; rgb[2]=1.00; }
    else                 { rgb[0]=0.80; rgb[1]=0.88; rgb[2]=1.00; }
}

/* Print a JSON-escaped string (quotes + backslashes). */
static void put_json_str(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', o);
        fputc(*s, o);
    }
    fputc('"', o);
}

/* Default Newtonian "laws" block — every generated universe gets one so it is
 * tunable in the multiverse menu like any other. */
static void write_default_laws(FILE *o)
{
    fprintf(o,
        "  \"laws\": {\n"
        "    \"G\": 6.674e-11,\n"
        "    \"softening\": 1e5,\n"
        "    \"time_scale\": 1.0,\n"
        "    \"force_exp\": 2.0\n"
        "  },\n\n");
}

/* ------------------------------------------------------------------ exoplanets */

typedef struct {
    char   host[96];
    char   pname[96];
    double dist, ra, dec, smass, srad, teff;   /* star (NAN = missing)    */
    double a, per, ecc, inc, lper, pmass, prad; /* planet (NAN = missing) */
} ExoRow;

/* Semi-major axis (AU) from orbital period: a^3 = M_star[Msun] * P[yr]^2. */
static double a_from_period(double per_days, double smass_msun)
{
    if (isnan(per_days) || per_days <= 0.0) return NAN;
    double m = (isnan(smass_msun) || smass_msun <= 0.0) ? 1.0 : smass_msun;
    double pyr = per_days / 365.25;
    return cbrt(m * pyr * pyr);
}

static int convert_exoplanets(const char *in_path, const char *out_path, int max_systems)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "[catalog] cannot open '%s'\n", in_path); return -1; }

    /* Find the header line (skip NASA Archive '#' comment banner + blanks). */
    char *header = NULL;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        header = ln;
        break;
    }
    if (!header) { fprintf(stderr, "[catalog] no header row in '%s'\n", in_path); fclose(in); return -1; }

    char *hdr[256];
    /* Work on a copy so split_csv can mutate it; keep it alive for the run. */
    int nh = split_csv(header, hdr, 256);
    for (int i = 0; i < nh; i++) { hdr[i] = trim(hdr[i]); lower(hdr[i]); }

    int ci_host = col_index(hdr, nh, "hostname");
    int ci_pl   = col_index(hdr, nh, "pl_name");
    int ci_dist = col_index(hdr, nh, "sy_dist");
    int ci_ra   = col_index(hdr, nh, "ra");
    int ci_dec  = col_index(hdr, nh, "dec");
    int ci_sm   = col_index(hdr, nh, "st_mass");
    int ci_sr   = col_index(hdr, nh, "st_rad");
    int ci_te   = col_index(hdr, nh, "st_teff");
    int ci_a    = col_index(hdr, nh, "pl_orbsmax");
    int ci_per  = col_index(hdr, nh, "pl_orbper");
    int ci_ecc  = col_index(hdr, nh, "pl_orbeccen");
    int ci_inc  = col_index(hdr, nh, "pl_orbincl");
    int ci_lper = col_index(hdr, nh, "pl_orblper");
    int ci_pm   = col_index(hdr, nh, "pl_bmasse");
    int ci_pr   = col_index(hdr, nh, "pl_rade");

    if (ci_host < 0 || ci_pl < 0) {
        fprintf(stderr, "[catalog] '%s' lacks hostname/pl_name columns "
                        "(is this a NASA Exoplanet Archive CSV?)\n", in_path);
        free(header); fclose(in); return -1;
    }

    /* Slurp all planet rows. */
    ExoRow *rows = NULL; int nrows = 0, cap = 0;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        char *f[256];
        int nf = split_csv(ln, f, 256);

        if (nrows == cap) {
            cap = cap ? cap * 2 : 64;
            ExoRow *nr = (ExoRow *)realloc(rows, (size_t)cap * sizeof(ExoRow));
            if (!nr) { free(ln); free(rows); free(header); fclose(in); return -1; }
            rows = nr;
        }
        ExoRow *r = &rows[nrows++];
        memset(r, 0, sizeof(*r));
        snprintf(r->host,  sizeof(r->host),  "%s", field(f, nf, ci_host));
        snprintf(r->pname, sizeof(r->pname), "%s", field(f, nf, ci_pl));
        r->dist  = field_num(f, nf, ci_dist);
        r->ra    = field_num(f, nf, ci_ra);
        r->dec   = field_num(f, nf, ci_dec);
        r->smass = field_num(f, nf, ci_sm);
        r->srad  = field_num(f, nf, ci_sr);
        r->teff  = field_num(f, nf, ci_te);
        r->a     = field_num(f, nf, ci_a);
        r->per   = field_num(f, nf, ci_per);
        r->ecc   = field_num(f, nf, ci_ecc);
        r->inc   = field_num(f, nf, ci_inc);
        r->lper  = field_num(f, nf, ci_lper);
        r->pmass = field_num(f, nf, ci_pm);
        r->prad  = field_num(f, nf, ci_pr);
        free(ln);
    }
    fclose(in);

    /* Count distinct hosts (first occurrence wins) for the single-system case. */
    int n_hosts = 0;
    for (int i = 0; i < nrows; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(rows[j].host, rows[i].host) == 0) { seen = 1; break; }
        if (!seen) n_hosts++;
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) { fprintf(stderr, "[catalog] cannot write '%s'\n", out_path); free(rows); free(header); return -1; }

    fprintf(o, "{\n  // Generated by catalogtool from NASA Exoplanet Archive data: %s\n",
            in_path);
    write_default_laws(o);
    fprintf(o, "  \"bodies\": [\n");

    int bodies = 0, systems = 0, first = 1;
    for (int i = 0; i < nrows; i++) {
        /* Process each host once, at its first appearance. */
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(rows[j].host, rows[i].host) == 0) { seen = 1; break; }
        if (seen) continue;
        if (max_systems > 0 && systems >= max_systems) break;
        systems++;

        ExoRow *s = &rows[i];
        double pos[3] = { 0, 0, 0 };
        if (n_hosts > 1 && !isnan(s->ra) && !isnan(s->dec) && !isnan(s->dist))
            eq_to_ecliptic_ly(s->ra, s->dec, s->dist * PC_TO_LY, pos);

        double smass_kg = (isnan(s->smass) ? 1.0 : s->smass) * MSUN_KG;
        double srad_km  = !isnan(s->srad) ? s->srad * RSUN_KM
                        : (!isnan(s->smass) ? pow(s->smass, 0.8) * RSUN_KM : RSUN_KM);
        double rgb[3]; teff_color(s->teff, rgb);

        if (!first) fprintf(o, ",\n");
        first = 0;
        fprintf(o, "    { \"name\": ");
        put_json_str(o, s->host[0] ? s->host : "Star");
        fprintf(o, ", \"type\": \"star\",\n");
        fprintf(o, "      \"pos_ly\": [%.6f, %.6f, %.6f],\n", pos[0], pos[1], pos[2]);
        fprintf(o, "      \"mass\": %.6e, \"radius_km\": %.3f,\n", smass_kg, srad_km);
        fprintf(o, "      \"color\": [%.3f, %.3f, %.3f],\n", rgb[0], rgb[1], rgb[2]);
        fprintf(o, "      \"obliquity_deg\": 0.0, \"rotation_period_days\": 25.0 }");
        bodies++;

        /* Planets of this host, in encounter order. */
        int p_idx = 0;
        for (int k = i; k < nrows; k++) {
            if (strcmp(rows[k].host, s->host) != 0) continue;
            ExoRow *p = &rows[k];

            double a = !isnan(p->a) ? p->a : a_from_period(p->per, s->smass);
            if (isnan(a) || a <= 0.0) continue;           /* unplaceable */
            double e = isnan(p->ecc) ? 0.0 : p->ecc;
            if (e < 0.0)  e = 0.0;
            if (e > 0.95) e = 0.95;
            double inc = isnan(p->inc) ? 0.0 : p->inc;
            double wbar = isnan(p->lper) ? 0.0 : p->lper;
            double L = fmod(p_idx * 137.50776405, 360.0); /* golden-angle phase */

            double pmass_kg = !isnan(p->pmass) ? p->pmass * MEARTH_KG
                            : (!isnan(p->prad) ? pow(p->prad, 3.0) * MEARTH_KG : MEARTH_KG);
            double prad_km  = !isnan(p->prad) ? p->prad * REARTH_KM
                            : (!isnan(p->pmass) ? cbrt(p->pmass) * REARTH_KM : REARTH_KM);

            fprintf(o, ",\n    { \"name\": ");
            put_json_str(o, p->pname[0] ? p->pname : "planet");
            fprintf(o, ", \"type\": \"planet\", \"parent\": ");
            put_json_str(o, s->host);
            fprintf(o, ",\n      \"mass\": %.6e, \"radius_km\": %.3f,\n", pmass_kg, prad_km);
            fprintf(o, "      \"color\": [0.70, 0.74, 0.80],\n");
            fprintf(o, "      \"keplerian\": { \"a\": %.6f, \"e\": %.4f, \"i\": %.4f, "
                       "\"Omega\": 0.0, \"omega_tilde\": %.4f, \"L\": %.4f } }",
                    a, e, inc, wbar, L);
            bodies++;
            p_idx++;
        }
    }

    fprintf(o, "\n  ]\n}\n");
    fclose(o);
    free(rows);
    free(header);

    fprintf(stdout, "[catalog] exoplanets: wrote %d bodies (%d system%s) -> %s\n",
            bodies, systems, systems == 1 ? "" : "s", out_path);
    return bodies;
}

/* ------------------------------------------------------------------ stubs */

/* Sun GM in km^3/s^2 (= G * 1.989e30 kg), and km per AU. */
#define MU_SUN_KM3S2  1.32712440018e11
#define AU_KM         1.495978707e8

static double norm360(double deg)
{
    deg = fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

/*
 * rv_to_elements — classical orbital elements from a heliocentric state vector.
 *
 * Inputs r (km) and v (km/s) must be in the ecliptic-of-J2000 frame (Horizons'
 * "Ecliptic of J2000.0" output), matching the Keplerian convention the loader
 * feeds to keplerian_to_state(). Outputs: a (AU); e; i, Omega, omega_tilde
 * (longitude of perihelion), L (mean longitude) in degrees. Returns 0 on
 * success, -1 for a degenerate/unbound orbit.
 */
static int rv_to_elements(const double r[3], const double v[3],
                          double *a_au, double *e_out, double *i_deg,
                          double *Omega_deg, double *wtilde_deg, double *L_deg)
{
    const double mu = MU_SUN_KM3S2;
    double rmag = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    double vmag2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (rmag <= 0.0) return -1;

    double h[3] = { r[1]*v[2] - r[2]*v[1],
                    r[2]*v[0] - r[0]*v[2],
                    r[0]*v[1] - r[1]*v[0] };
    double hmag = sqrt(h[0]*h[0] + h[1]*h[1] + h[2]*h[2]);

    double n[3] = { -h[1], h[0], 0.0 };          /* k x h */
    double nmag = sqrt(n[0]*n[0] + n[1]*n[1]);

    double rdotv = r[0]*v[0] + r[1]*v[1] + r[2]*v[2];
    double ev[3];
    for (int k = 0; k < 3; k++)
        ev[k] = ((vmag2 - mu/rmag) * r[k] - rdotv * v[k]) / mu;
    double e = sqrt(ev[0]*ev[0] + ev[1]*ev[1] + ev[2]*ev[2]);

    double energy = 0.5 * vmag2 - mu / rmag;
    if (energy >= 0.0 || fabs(energy) < 1e-30) return -1;   /* unbound */
    double a = -mu / (2.0 * energy);

    double inc = (hmag > 0.0) ? acos(h[2] / hmag) : 0.0;

    double Omega = 0.0;
    if (nmag > 1e-12) {
        Omega = acos(n[0] / nmag);
        if (n[1] < 0.0) Omega = 2.0 * CAT_PI - Omega;
    }

    double omega = 0.0;
    if (nmag > 1e-12 && e > 1e-9) {
        double c = (n[0]*ev[0] + n[1]*ev[1] + n[2]*ev[2]) / (nmag * e);
        if (c >  1.0) c =  1.0;
        if (c < -1.0) c = -1.0;
        omega = acos(c);
        if (ev[2] < 0.0) omega = 2.0 * CAT_PI - omega;
    }

    double nu = 0.0;
    if (e > 1e-9) {
        double c = (ev[0]*r[0] + ev[1]*r[1] + ev[2]*r[2]) / (e * rmag);
        if (c >  1.0) c =  1.0;
        if (c < -1.0) c = -1.0;
        nu = acos(c);
        if (rdotv < 0.0) nu = 2.0 * CAT_PI - nu;
    } else {
        /* circular: measure argument of latitude from the node instead */
        if (nmag > 1e-12) {
            double c = (n[0]*r[0] + n[1]*r[1] + n[2]*r[2]) / (nmag * rmag);
            if (c >  1.0) c =  1.0;
            if (c < -1.0) c = -1.0;
            nu = acos(c);
            if (r[2] < 0.0) nu = 2.0 * CAT_PI - nu;
        }
    }

    /* eccentric & mean anomaly */
    double E = atan2(sqrt(1.0 - e*e) * sin(nu), e + cos(nu));
    double M = E - e * sin(E);

    double deg = 180.0 / CAT_PI;
    *a_au      = a / AU_KM;
    *e_out     = e;
    *i_deg     = norm360(inc   * deg);
    *Omega_deg = norm360(Omega * deg);
    *wtilde_deg= norm360((Omega + omega) * deg);
    *L_deg     = norm360((Omega + omega + M) * deg);
    return 0;
}

/*
 * Horizons importer. Input CSV columns (heliocentric, Ecliptic of J2000.0):
 *   name, mass_kg, radius_km, x_km, y_km, z_km, vx_kms, vy_kms, vz_kms
 * Emits a Sun at the origin plus each body as a planet with Keplerian elements
 * derived from its state vector.
 */
static int convert_horizons(const char *in_path, const char *out_path, int max_items)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "[catalog] cannot open '%s'\n", in_path); return -1; }

    char *header = NULL;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        header = ln; break;
    }
    if (!header) { fprintf(stderr, "[catalog] no header in '%s'\n", in_path); fclose(in); return -1; }

    char *hdr[64];
    int nh = split_csv(header, hdr, 64);
    for (int i = 0; i < nh; i++) { hdr[i] = trim(hdr[i]); lower(hdr[i]); }

    int ci_name = col_index(hdr, nh, "name");
    int ci_m    = col_index(hdr, nh, "mass_kg");
    int ci_r    = col_index(hdr, nh, "radius_km");
    int ci_x    = col_index(hdr, nh, "x_km");
    int ci_y    = col_index(hdr, nh, "y_km");
    int ci_z    = col_index(hdr, nh, "z_km");
    int ci_vx   = col_index(hdr, nh, "vx_kms");
    int ci_vy   = col_index(hdr, nh, "vy_kms");
    int ci_vz   = col_index(hdr, nh, "vz_kms");
    if (ci_x < 0 || ci_vx < 0) {
        fprintf(stderr, "[catalog] '%s' lacks x_km/vx_kms columns\n", in_path);
        free(header); fclose(in); return -1;
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) { fprintf(stderr, "[catalog] cannot write '%s'\n", out_path); free(header); fclose(in); return -1; }

    fprintf(o, "{\n  // Generated by catalogtool from JPL Horizons vectors: %s\n", in_path);
    write_default_laws(o);
    fprintf(o, "  \"bodies\": [\n");
    fprintf(o, "    { \"name\": \"Sun\", \"type\": \"star\",\n"
               "      \"pos_ly\": [0.0, 0.0, 0.0],\n"
               "      \"mass\": 1.989e30, \"radius_km\": 696000.0,\n"
               "      \"color\": [1.0, 0.92, 0.23],\n"
               "      \"obliquity_deg\": 7.25, \"rotation_period_days\": 25.38 }");

    int bodies = 1, count = 0;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        if (max_items > 0 && count >= max_items) { free(ln); break; }

        char *f[64];
        int nf = split_csv(ln, f, 64);
        double r[3] = { field_num(f,nf,ci_x), field_num(f,nf,ci_y), field_num(f,nf,ci_z) };
        double v[3] = { field_num(f,nf,ci_vx), field_num(f,nf,ci_vy), field_num(f,nf,ci_vz) };
        if (isnan(r[0])||isnan(r[1])||isnan(r[2])||isnan(v[0])||isnan(v[1])||isnan(v[2])) { free(ln); continue; }

        double a,e,i,Om,wt,L;
        if (rv_to_elements(r, v, &a,&e,&i,&Om,&wt,&L) != 0) { free(ln); continue; }

        double mass = field_num(f,nf,ci_m);   if (isnan(mass)) mass = MEARTH_KG;
        double rad  = field_num(f,nf,ci_r);   if (isnan(rad))  rad  = REARTH_KM;
        const char *nm = field(f,nf,ci_name); if (!nm[0]) nm = "body";

        fprintf(o, ",\n    { \"name\": ");
        put_json_str(o, nm);
        fprintf(o, ", \"type\": \"planet\", \"parent\": \"Sun\",\n");
        fprintf(o, "      \"mass\": %.6e, \"radius_km\": %.3f,\n", mass, rad);
        fprintf(o, "      \"color\": [0.72, 0.74, 0.78],\n");
        fprintf(o, "      \"keplerian\": { \"a\": %.6f, \"e\": %.5f, \"i\": %.5f, "
                   "\"Omega\": %.5f, \"omega_tilde\": %.5f, \"L\": %.5f } }",
                a, e, i, Om, wt, L);
        bodies++; count++;
        free(ln);
    }

    fprintf(o, "\n  ]\n}\n");
    fclose(o); fclose(in); free(header);
    fprintf(stdout, "[catalog] horizons: wrote %d bodies (%d from vectors) -> %s\n",
            bodies, count, out_path);
    return bodies;
}

/* Very rough main-sequence mass (solar masses) from effective temperature. */
static double mass_from_teff(double t)
{
    if (isnan(t) || t <= 0.0) return 0.9;
    if (t >= 25000.0) return 12.0;
    if (t >= 10000.0) return 2.2;
    if (t >=  7500.0) return 1.6;
    if (t >=  6000.0) return 1.15;
    if (t >=  5200.0) return 0.9;
    if (t >=  4000.0) return 0.6;
    if (t >=  3000.0) return 0.3;
    return 0.15;
}

/* Equatorial space velocity (radial + proper-motion components) rotated into
 * the simulation's GL/ecliptic frame, in km/s. */
static void eq_vel_to_gl(double ra_deg, double dec_deg,
                         double v_r, double v_alpha, double v_delta,
                         double out[3])
{
    const double d = CAT_PI / 180.0;
    double ra = ra_deg * d, dec = dec_deg * d;
    double cr = cos(ra), sr = sin(ra), cd = cos(dec), sd = sin(dec);
    /* equatorial cartesian basis at (ra,dec) */
    double r_hat[3]   = {  cd*cr,  cd*sr,  sd };
    double a_hat[3]   = { -sr,     cr,     0  };          /* +RA (east)   */
    double d_hat[3]   = { -sd*cr, -sd*sr,  cd };          /* +Dec (north) */
    double ve[3];
    for (int k = 0; k < 3; k++)
        ve[k] = v_r*r_hat[k] + v_alpha*a_hat[k] + v_delta*d_hat[k];

    /* equatorial -> ecliptic -> GL (same convention as eq_to_ecliptic_ly) */
    const double eps = J2000_EPS * d;
    double ce = cos(eps), se = sin(eps);
    double x_ecl = ve[0];
    double y_ecl = ve[1]*ce + ve[2]*se;
    double z_ecl = -ve[1]*se + ve[2]*ce;
    out[0] = x_ecl;
    out[1] = z_ecl;
    out[2] = y_ecl;
}

/*
 * Gaia / Hipparcos importer. Expected columns (any subset; names are flexible):
 *   name|source_id, ra, dec, parallax (mas) [or dist_pc],
 *   pmra, pmdec (mas/yr), radial_velocity (km/s), teff [or st_teff]
 * Emits one positioned star per row (pos_ly + proper-motion velocity_km_s).
 */
static int convert_gaia(const char *in_path, const char *out_path, int max_items)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "[catalog] cannot open '%s'\n", in_path); return -1; }

    char *header = NULL;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        header = ln; break;
    }
    if (!header) { fprintf(stderr, "[catalog] no header in '%s'\n", in_path); fclose(in); return -1; }

    char *hdr[128];
    int nh = split_csv(header, hdr, 128);
    for (int i = 0; i < nh; i++) { hdr[i] = trim(hdr[i]); lower(hdr[i]); }

    int ci_name = col_index(hdr, nh, "name");
    if (ci_name < 0) ci_name = col_index(hdr, nh, "source_id");
    int ci_ra   = col_index(hdr, nh, "ra");
    int ci_dec  = col_index(hdr, nh, "dec");
    int ci_plx  = col_index(hdr, nh, "parallax");
    int ci_dpc  = col_index(hdr, nh, "dist_pc");
    int ci_pmra = col_index(hdr, nh, "pmra");
    int ci_pmde = col_index(hdr, nh, "pmdec");
    int ci_rv   = col_index(hdr, nh, "radial_velocity");
    int ci_te   = col_index(hdr, nh, "teff");
    if (ci_te < 0) ci_te = col_index(hdr, nh, "st_teff");
    if (ci_ra < 0 || ci_dec < 0 || (ci_plx < 0 && ci_dpc < 0)) {
        fprintf(stderr, "[catalog] '%s' needs ra, dec and parallax/dist_pc\n", in_path);
        free(header); fclose(in); return -1;
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) { fprintf(stderr, "[catalog] cannot write '%s'\n", out_path); free(header); fclose(in); return -1; }
    fprintf(o, "{\n  // Generated by catalogtool from a Gaia/Hipparcos star catalog: %s\n", in_path);
    write_default_laws(o);
    fprintf(o, "  \"bodies\": [\n");

    int count = 0, first = 1;
    for (;;) {
        char *ln = read_line(in);
        if (!ln) break;
        char *t = trim(ln);
        if (t[0] == '\0' || t[0] == '#') { free(ln); continue; }
        if (max_items > 0 && count >= max_items) { free(ln); break; }

        char *f[128];
        int nf = split_csv(ln, f, 128);
        double ra = field_num(f,nf,ci_ra), dec = field_num(f,nf,ci_dec);
        double dist_pc = field_num(f,nf,ci_dpc);
        if (isnan(dist_pc)) {
            double plx = field_num(f,nf,ci_plx);
            if (isnan(plx) || plx <= 0.0) { free(ln); continue; }
            dist_pc = 1000.0 / plx;   /* mas -> pc */
        }
        if (isnan(ra) || isnan(dec) || dist_pc <= 0.0) { free(ln); continue; }

        double pos[3];
        eq_to_ecliptic_ly(ra, dec, dist_pc * PC_TO_LY, pos);

        double teff = field_num(f,nf,ci_te);
        double smass = mass_from_teff(teff);
        double rgb[3]; teff_color(teff, rgb);

        /* proper motion (mas/yr) -> transverse velocity (km/s): 4.74047 km/s
         * per (arcsec/yr * pc). */
        double pmra = field_num(f,nf,ci_pmra), pmde = field_num(f,nf,ci_pmde);
        double rv   = field_num(f,nf,ci_rv);
        double v_alpha = isnan(pmra) ? 0.0 : 4.74047 * (pmra/1000.0) * dist_pc;
        double v_delta = isnan(pmde) ? 0.0 : 4.74047 * (pmde/1000.0) * dist_pc;
        double v_r     = isnan(rv)   ? 0.0 : rv;
        double vel[3]; eq_vel_to_gl(ra, dec, v_r, v_alpha, v_delta, vel);

        char nm[96];
        snprintf(nm, sizeof(nm), "%s", field(f,nf,ci_name));
        if (!nm[0]) snprintf(nm, sizeof(nm), "Star_%d", count + 1);

        if (!first) fprintf(o, ",\n");
        first = 0;
        fprintf(o, "    { \"name\": ");
        put_json_str(o, nm);
        fprintf(o, ", \"type\": \"star\",\n");
        fprintf(o, "      \"pos_ly\": [%.6f, %.6f, %.6f],\n", pos[0], pos[1], pos[2]);
        fprintf(o, "      \"velocity_km_s\": [%.5f, %.5f, %.5f],\n", vel[0], vel[1], vel[2]);
        fprintf(o, "      \"mass\": %.6e, \"radius_km\": %.3f,\n",
                smass * MSUN_KG, pow(smass, 0.8) * RSUN_KM);
        fprintf(o, "      \"color\": [%.3f, %.3f, %.3f],\n", rgb[0], rgb[1], rgb[2]);
        fprintf(o, "      \"obliquity_deg\": 0.0, \"rotation_period_days\": 25.0 }");
        count++;
        free(ln);
    }

    fprintf(o, "\n  ]\n}\n");
    fclose(o); fclose(in); free(header);
    fprintf(stdout, "[catalog] gaia: wrote %d stars -> %s\n", count, out_path);
    return count;
}

/* ------------------------------------------------------------------ API */

int catalog_type_from_name(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "exoplanets")) return CATALOG_EXOPLANETS;
    if (!strcmp(name, "horizons"))   return CATALOG_HORIZONS;
    if (!strcmp(name, "gaia"))       return CATALOG_GAIA;
    return -1;
}

int catalog_convert(CatalogType type, const char *in_path,
                    const char *out_path, int max_items)
{
    switch (type) {
        case CATALOG_EXOPLANETS: return convert_exoplanets(in_path, out_path, max_items);
        case CATALOG_HORIZONS:   return convert_horizons(in_path, out_path, max_items);
        case CATALOG_GAIA:       return convert_gaia(in_path, out_path, max_items);
    }
    fprintf(stderr, "[catalog] unknown catalog type\n");
    return -1;
}
