/*
 * bc7.c — BC7 mode-6 encoder (see bc7.h).
 */
#include "bc7.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

typedef struct { int q[2][3], p[2]; int idx[16]; long err; } Enc6;

/* Nearest of the 16 levels to t in [0, 64] (weights are not uniform). */
static int nearest_level(float t)
{
    int best = 0;
    float bd = 1e30f;
    int guess = (int)(t * (15.0f / 64.0f) + 0.5f);
    for (int k = guess - 1; k <= guess + 1; k++) {
        if (k < 0 || k > 15) continue;
        float d = fabsf(t - (float)W4[k]);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

/* Endpoint value v in [0,255] -> 7-bit code for p-bit p. */
static int quant7(float v, int p)
{
    int q = (int)floorf((v - (float)p) * 0.5f + 0.5f);
    return q < 0 ? 0 : (q > 127 ? 127 : q);
}

/* Indices and squared error for quantised endpoints e (0..255 per channel).
 * Each texel takes the best of the levels around its projection. */
static long assign(const float c[16][3], const int e0[3], const int e1[3], int idx[16])
{
    int pal[16][3];
    for (int k = 0; k < 16; k++)
        for (int ch = 0; ch < 3; ch++)
            pal[k][ch] = ((64 - W4[k]) * e0[ch] + W4[k] * e1[ch] + 32) >> 6;
    float d[3] = { (float)(e1[0] - e0[0]), (float)(e1[1] - e0[1]), (float)(e1[2] - e0[2]) };
    float dd = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
    long err = 0;
    for (int i = 0; i < 16; i++) {
        int g = 0;
        if (dd > 0.0f) {
            float t = ((c[i][0] - e0[0]) * d[0] + (c[i][1] - e0[1]) * d[1] +
                       (c[i][2] - e0[2]) * d[2]) / dd * 64.0f;
            g = nearest_level(t < 0.0f ? 0.0f : (t > 64.0f ? 64.0f : t));
        }
        long be = -1; int bk = g;
        for (int k = g - 1; k <= g + 1; k++) {
            if (k < 0 || k > 15) continue;
            long e = 0;
            for (int ch = 0; ch < 3; ch++) {
                long x = (long)(c[i][ch] + 0.5f) - pal[k][ch];
                e += x * x;
            }
            if (be < 0 || e < be) { be = e; bk = k; }
        }
        idx[i] = bk;
        err += be;
    }
    return err;
}

/* Best quantisation of float endpoints a, b over the four p-bit pairs. */
static void try_endpoints(const float c[16][3], const float a[3], const float b[3], Enc6 *best)
{
    for (int p0 = 0; p0 < 2; p0++)
    for (int p1 = 0; p1 < 2; p1++) {
        Enc6 e;
        int v0[3], v1[3];
        e.p[0] = p0;  e.p[1] = p1;
        for (int ch = 0; ch < 3; ch++) {
            e.q[0][ch] = quant7(a[ch], p0);  v0[ch] = (e.q[0][ch] << 1) | p0;
            e.q[1][ch] = quant7(b[ch], p1);  v1[ch] = (e.q[1][ch] << 1) | p1;
        }
        e.err = assign(c, v0, v1, e.idx);
        if (best->err < 0 || e.err < best->err) *best = e;
    }
}

static void put_bits(uint8_t out[16], int *pos, unsigned v, int n)
{
    for (int k = 0; k < n; k++, (*pos)++)
        if (v & (1u << k)) out[*pos >> 3] |= (uint8_t)(1u << (*pos & 7));
}

static unsigned get_bits(const uint8_t in[16], int *pos, int n)
{
    unsigned v = 0;
    for (int k = 0; k < n; k++, (*pos)++)
        if (in[*pos >> 3] & (1u << (*pos & 7))) v |= 1u << k;
    return v;
}

void bc7_encode_block(const uint8_t px[64], uint8_t out[16])
{
    float c[16][3], m[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; i++)
        for (int ch = 0; ch < 3; ch++) { c[i][ch] = px[i * 4 + ch];  m[ch] += c[i][ch]; }
    for (int ch = 0; ch < 3; ch++) m[ch] /= 16.0f;

    /* Principal axis by power iteration on the covariance. */
    float cov[6] = { 0 };
    for (int i = 0; i < 16; i++) {
        float r = c[i][0] - m[0], g = c[i][1] - m[1], b = c[i][2] - m[2];
        cov[0] += r*r; cov[1] += r*g; cov[2] += r*b; cov[3] += g*g; cov[4] += g*b; cov[5] += b*b;
    }
    float ax[3] = { 1.0f, 1.0f, 1.0f };
    for (int it = 0; it < 8; it++) {
        float nx = cov[0]*ax[0] + cov[1]*ax[1] + cov[2]*ax[2];
        float ny = cov[1]*ax[0] + cov[3]*ax[1] + cov[4]*ax[2];
        float nz = cov[2]*ax[0] + cov[4]*ax[1] + cov[5]*ax[2];
        float n = sqrtf(nx*nx + ny*ny + nz*nz);
        if (n < 1e-12f) break;
        ax[0] = nx / n; ax[1] = ny / n; ax[2] = nz / n;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = (c[i][0] - m[0]) * ax[0] + (c[i][1] - m[1]) * ax[1] + (c[i][2] - m[2]) * ax[2];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float a[3], b[3];
    for (int ch = 0; ch < 3; ch++) { a[ch] = m[ch] + ax[ch] * tmin;  b[ch] = m[ch] + ax[ch] * tmax; }

    Enc6 best; best.err = -1;
    try_endpoints(c, a, b, &best);

    /* Least-squares endpoints for the chosen indices, twice. */
    for (int it = 0; it < 2 && best.err > 0; it++) {
        float aa = 0, ab = 0, bb = 0, ac[3] = { 0 }, bc[3] = { 0 };
        for (int i = 0; i < 16; i++) {
            float wb = W4[best.idx[i]] / 64.0f, wa = 1.0f - wb;
            aa += wa * wa; ab += wa * wb; bb += wb * wb;
            for (int ch = 0; ch < 3; ch++) { ac[ch] += wa * c[i][ch]; bc[ch] += wb * c[i][ch]; }
        }
        float det = aa * bb - ab * ab;
        if (fabsf(det) < 1e-6f) break;
        for (int ch = 0; ch < 3; ch++) {
            a[ch] = (bb * ac[ch] - ab * bc[ch]) / det;
            b[ch] = (aa * bc[ch] - ab * ac[ch]) / det;
            a[ch] = a[ch] < 0 ? 0 : (a[ch] > 255 ? 255 : a[ch]);
            b[ch] = b[ch] < 0 ? 0 : (b[ch] > 255 ? 255 : b[ch]);
        }
        long before = best.err;
        try_endpoints(c, a, b, &best);
        if (best.err >= before) break;
    }

    /* The anchor texel's index must have its top bit clear: swap if not. */
    if (best.idx[0] & 8) {
        for (int ch = 0; ch < 3; ch++) { int t = best.q[0][ch]; best.q[0][ch] = best.q[1][ch]; best.q[1][ch] = t; }
        int t = best.p[0]; best.p[0] = best.p[1]; best.p[1] = t;
        for (int i = 0; i < 16; i++) best.idx[i] = 15 - best.idx[i];
    }

    memset(out, 0, 16);
    int pos = 0;
    put_bits(out, &pos, 1u << 6, 7);                  /* mode 6 */
    for (int ch = 0; ch < 3; ch++) {
        put_bits(out, &pos, (unsigned)best.q[0][ch], 7);
        put_bits(out, &pos, (unsigned)best.q[1][ch], 7);
    }
    put_bits(out, &pos, 127, 7);                       /* alpha: opaque */
    put_bits(out, &pos, 127, 7);
    put_bits(out, &pos, (unsigned)best.p[0], 1);
    put_bits(out, &pos, (unsigned)best.p[1], 1);
    put_bits(out, &pos, (unsigned)best.idx[0], 3);
    for (int i = 1; i < 16; i++) put_bits(out, &pos, (unsigned)best.idx[i], 4);
}

int bc7_decode_block6(const uint8_t in[16], uint8_t px[64])
{
    int pos = 0;
    if (get_bits(in, &pos, 7) != (1u << 6)) return 0;
    int q[2][4], p[2], idx[16];
    for (int ch = 0; ch < 4; ch++) { q[0][ch] = (int)get_bits(in, &pos, 7); q[1][ch] = (int)get_bits(in, &pos, 7); }
    p[0] = (int)get_bits(in, &pos, 1);  p[1] = (int)get_bits(in, &pos, 1);
    idx[0] = (int)get_bits(in, &pos, 3);
    for (int i = 1; i < 16; i++) idx[i] = (int)get_bits(in, &pos, 4);
    for (int i = 0; i < 16; i++)
        for (int ch = 0; ch < 4; ch++) {
            int e0 = (q[0][ch] << 1) | p[0], e1 = (q[1][ch] << 1) | p[1];
            px[i * 4 + ch] = (uint8_t)(((64 - W4[idx[i]]) * e0 + W4[idx[i]] * e1 + 32) >> 6);
        }
    return 1;
}

void bc7_encode_image(const uint8_t *rgba, int w, int h, uint8_t *out)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    #pragma omp parallel for schedule(dynamic, 4)
    for (int by = 0; by < bh; by++) {
        uint8_t blk[64];
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int sx = bx * 4 + x, sy = by * 4 + y;
                    if (sx >= w) sx = w - 1;
                    if (sy >= h) sy = h - 1;
                    memcpy(blk + (y * 4 + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
                }
            bc7_encode_block(blk, out + ((size_t)by * bw + bx) * 16);
        }
    }
}

/* ── self-test ───────────────────────────────────────────────────────────── */
int bc7_selftest(void)
{
    enum { N = 256 };
    uint8_t *img = malloc(N * N * 4), *enc = malloc((N / 4) * (N / 4) * 16);
    if (!img || !enc) { free(img); free(enc); return 0; }
    unsigned rng = 1u;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint8_t *p = img + (y * N + x) * 4;
            rng = rng * 1664525u + 1013904223u;
            int noise = (int)(rng >> 28) - 8;                         /* +-8 grain */
            if (y < N / 2) {                                          /* smooth ramps */
                p[0] = (uint8_t)x;  p[1] = (uint8_t)(y * 2);  p[2] = (uint8_t)(255 - x);
            } else {                                                  /* land/sea edges + grain */
                int land = ((x / 23 + y / 17) & 1);
                int r = land ? 120 : 20, g = land ? 110 : 60, b = land ? 70 : 110;
                p[0] = (uint8_t)(r + noise < 0 ? 0 : r + noise);
                p[1] = (uint8_t)(g + noise < 0 ? 0 : g + noise);
                p[2] = (uint8_t)(b + noise < 0 ? 0 : b + noise);
            }
            p[3] = 255;
        }
    bc7_encode_image(img, N, N, enc);
    double se_s = 0, se_e = 0;
    int bad_mode = 0;
    for (int by = 0; by < N / 4; by++)
        for (int bx = 0; bx < N / 4; bx++) {
            uint8_t dec[64];
            if (!bc7_decode_block6(enc + (by * (N / 4) + bx) * 16, dec)) { bad_mode++; continue; }
            for (int i = 0; i < 16; i++) {
                const uint8_t *s = img + ((by * 4 + i / 4) * N + bx * 4 + i % 4) * 4;
                for (int ch = 0; ch < 3; ch++) {
                    double d = (double)s[ch] - dec[i * 4 + ch];
                    if (by < N / 8) se_s += d * d; else se_e += d * d;
                }
                if (dec[i * 4 + 3] < 254) bad_mode++;
            }
        }
    double n_half = (double)N * N / 2 * 3;
    double psnr_s = 10 * log10(255.0 * 255.0 / fmax(se_s / n_half, 1e-9));
    double psnr_e = 10 * log10(255.0 * 255.0 / fmax(se_e / n_half, 1e-9));
    int ok = bad_mode == 0 && psnr_s > 45.0 && psnr_e > 36.0;
    fprintf(stdout, "[selftest] bc7: smooth %.1f dB, edges+grain %.1f dB, %d bad blocks: %s\n",
            psnr_s, psnr_e, bad_mode, ok ? "passed" : "FAILED");
    free(img); free(enc);
    return ok;
}
