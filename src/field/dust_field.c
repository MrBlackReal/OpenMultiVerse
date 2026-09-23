/*
 * dust_field.c — the measured 3D interstellar dust around the Sun (dust_field.h).
 */
#include "dust_field.h"
#include "common.h"

#define DUSTBIN_MAGIC   0x4F4D5644u     /* 'OMVD' */
#define DUSTBIN_VERSION 1u
#define AU_PER_PC       206264.806

static uint8_t *s_rho = NULL;   /* gamma-encoded density, x fastest, [dim^3] */
static float    s_lut[256];     /* encoded byte -> density / vmax            */
static int    s_dim = 0;
static float  s_vmax = 0.0f;    /* ZGR23 extinction per pc at rho = 1        */
static double s_half_au = 0.0;

int dust_field_loaded(void) { return s_rho != NULL; }
double dust_field_half_au(void) { return s_half_au; }
double dust_field_voxel_au(void) { return s_dim ? 2.0 * s_half_au / s_dim : 1.0; }
float  dust_field_vmax(void) { return s_vmax; }

float *dust_field_density_alloc(int *dim, float *vmax)
{
    if (dim)  *dim  = s_dim;
    if (vmax) *vmax = s_vmax;
    if (!s_rho) return NULL;
    size_t n = (size_t)s_dim * s_dim * s_dim;
    float *out = (float *)malloc(n * sizeof(float));
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) out[i] = s_lut[s_rho[i]];
    return out;
}

int dust_field_load(const char *path)
{
    free(s_rho);
    s_rho = NULL;
    s_dim = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Dust] %s not found — no interstellar extinction "
                        "(tools/fetch_dustmap.py builds it)\n", path);
        return 0;
    }
    uint32_t magic, version, dim, format;
    float half_pc, vmax, gamma;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&dim, 4, 1, f) != 1 || fread(&format, 4, 1, f) != 1 ||
        fread(&half_pc, 4, 1, f) != 1 || fread(&vmax, 4, 1, f) != 1 ||
        fread(&gamma, 4, 1, f) != 1 ||
        magic != DUSTBIN_MAGIC || version != DUSTBIN_VERSION || format != 0 ||
        dim == 0 || dim > 1024 || !(half_pc > 0.0f) || !(vmax > 0.0f) ||
        !(gamma > 0.0f)) {
        fprintf(stderr, "[Dust] %s: not a v1 DustBin\n", path);
        fclose(f);
        return 0;
    }

    size_t n = (size_t)dim * dim * dim;
    uint8_t *enc = (uint8_t *)malloc(n);
    s_rho = (uint8_t *)malloc(n);
    if (!enc || !s_rho || fread(enc, 1, n, f) != n) {
        fprintf(stderr, "[Dust] %s: truncated or out of memory\n", path);
        free(enc); free(s_rho); s_rho = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);

    /* Kept encoded (a quarter of the float size); the gamma curve is a
     * lookup. Transposed: the file is C-ordered with z fastest (numpy
     * out[x][y][z]); GL textures and the sampler below want x fastest. */
    for (int v = 0; v < 256; v++) s_lut[v] = powf(v / 255.0f, gamma);
    for (uint32_t x = 0; x < dim; x++)
        for (uint32_t y = 0; y < dim; y++)
            for (uint32_t z = 0; z < dim; z++)
                s_rho[((size_t)z * dim + y) * dim + x] =
                    enc[((size_t)x * dim + y) * dim + z];
    free(enc);

    s_dim = (int)dim;
    s_vmax = vmax;
    s_half_au = (double)half_pc * AU_PER_PC;
    fprintf(stdout, "[Dust] %s: %u^3, +/-%.0f pc (%.1f pc/voxel)\n",
            path, dim, half_pc, 2.0 * half_pc / dim);
    return 1;
}

/* Trilinear density (ZGR23 per pc) at p in AU. */
static double sample(const double p[3])
{
    double u[3];
    int    i0[3];
    double t[3];
    for (int k = 0; k < 3; k++) {
        u[k] = (p[k] / (2.0 * s_half_au) + 0.5) * s_dim - 0.5;
        if (u[k] < 0.0 || u[k] > s_dim - 1) return 0.0;
        i0[k] = (int)u[k];
        if (i0[k] >= s_dim - 1) i0[k] = s_dim - 2;
        t[k] = u[k] - i0[k];
    }
    const size_t sy = (size_t)s_dim, sz = (size_t)s_dim * s_dim;
    const uint8_t *e = s_rho + i0[2] * sz + i0[1] * sy + i0[0];
#define V(o) ((double)s_lut[e[o]])
    double c00 = V(0)       + (V(1)           - V(0))       * t[0];
    double c10 = V(sy)      + (V(sy + 1)      - V(sy))      * t[0];
    double c01 = V(sz)      + (V(sz + 1)      - V(sz))      * t[0];
    double c11 = V(sz + sy) + (V(sz + sy + 1) - V(sz + sy)) * t[0];
#undef V
    double c0 = c00 + (c10 - c00) * t[1];
    double c1 = c01 + (c11 - c01) * t[1];
    return (c0 + (c1 - c0) * t[2]) * s_vmax;
}

double dust_field_av(const double a[3], const double b[3])
{
    if (!s_rho) return 0.0;

    /* Clip the segment to the cube (slab method). */
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 3; k++) {
        double d = b[k] - a[k];
        if (fabs(d) < 1e-30) {
            if (a[k] < -s_half_au || a[k] > s_half_au) return 0.0;
            continue;
        }
        double ta = (-s_half_au - a[k]) / d, tb = (s_half_au - a[k]) / d;
        if (ta > tb) { double s = ta; ta = tb; tb = s; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 >= t1) return 0.0;
    }

    /* Midpoint rule at half a voxel per step: this is the reference the GPU
     * version (mip-filtered, fewer steps) is checked against. */
    double len_au = sqrt((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) +
                         (b[2]-a[2])*(b[2]-a[2])) * (t1 - t0);
    double vox_au = 2.0 * s_half_au / s_dim;
    int n = (int)ceil(len_au / (0.5 * vox_au));
    if (n < 1) n = 1;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double t = t0 + (t1 - t0) * (i + 0.5) / n, p[3];
        for (int k = 0; k < 3; k++) p[k] = a[k] + (b[k] - a[k]) * t;
        sum += sample(p);
    }
    return DUST_AV_PER_ZGR * sum * (len_au / n) / AU_PER_PC;
}

double dust_field_ag_from_sun(const double p[3])
{
    static const double sun[3] = { 0.0, 0.0, 0.0 };
    return DUST_AG_PER_AV * dust_field_av(sun, p);
}
