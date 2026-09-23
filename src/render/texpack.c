/*
 * texpack.c — BC7 colour maps with a DDS cache (see texpack.h).
 */
#include "texpack.h"
#include "bc7.h"
#include "stb_image.h"

#include <GL/glew.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TP_MAX_LEVELS 16
#define TP_TAG        0x31564D4Fu   /* "OMV1" in dwReserved1[0]          */
#define TP_VERSION    1u            /* bump when the encoder output changes */
#define DXGI_BC7_SRGB 99u

#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif

typedef struct {
    int      w, h, levels;
    uint8_t *data;                    /* every level, top first            */
    size_t   off[TP_MAX_LEVELS], size[TP_MAX_LEVELS];
} Pack;

static size_t level_bytes(int w, int h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }

static void pack_layout(Pack *p)
{
    size_t at = 0;
    for (int l = 0; l < p->levels; l++) {
        int w = p->w >> l, h = p->h >> l;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        p->off[l] = at;  p->size[l] = level_bytes(w, h);
        at += p->size[l];
    }
}

static int count_levels(int w, int h)
{
    int n = 1;
    while ((w > 1 || h > 1) && n < TP_MAX_LEVELS) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

/* ── DDS (DX10 header) ─────────────────────────────────────────────────── */
static void put32(uint8_t *b, int i, uint32_t v) { memcpy(b + 4 * i, &v, 4); }
static uint32_t get32(const uint8_t *b, int i) { uint32_t v; memcpy(&v, b + 4 * i, 4); return v; }

static int dds_write(const char *path, const Pack *p)
{
    uint8_t hdr[4 + 124 + 20] = { 0 };
    memcpy(hdr, "DDS ", 4);
    uint8_t *h = hdr + 4;
    put32(h, 0, 124);
    put32(h, 1, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);  /* caps|h|w|pf|mips|linsize */
    put32(h, 2, (uint32_t)p->h);
    put32(h, 3, (uint32_t)p->w);
    put32(h, 4, (uint32_t)p->size[0]);
    put32(h, 6, (uint32_t)p->levels);
    put32(h, 7, TP_TAG);                    /* dwReserved1[0..1]: our stamp */
    put32(h, 8, TP_VERSION);
    put32(h, 18, 32);                       /* pixel format: size          */
    put32(h, 19, 0x4);                      /* DDPF_FOURCC                 */
    memcpy(h + 4 * 20, "DX10", 4);
    put32(h, 26, 0x1000 | 0x400000 | 0x8);  /* texture|mipmap|complex      */
    uint8_t *x = hdr + 4 + 124;
    put32(x, 0, DXGI_BC7_SRGB);
    put32(x, 1, 3);                         /* TEXTURE2D                   */
    put32(x, 3, 1);                         /* array size                  */

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t total = p->off[p->levels - 1] + p->size[p->levels - 1];
    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(p->data, 1, total, f) == total;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

static int dds_read(const char *path, Pack *p)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[4 + 124 + 20];
    int ok = fread(hdr, sizeof hdr, 1, f) == 1 && !memcmp(hdr, "DDS ", 4);
    const uint8_t *h = hdr + 4, *x = hdr + 4 + 124;
    ok = ok && get32(h, 0) == 124 && !memcmp(h + 4 * 20, "DX10", 4) &&
         get32(x, 0) == DXGI_BC7_SRGB && get32(h, 7) == TP_TAG && get32(h, 8) == TP_VERSION;
    if (ok) {
        p->h = (int)get32(h, 2);  p->w = (int)get32(h, 3);  p->levels = (int)get32(h, 6);
        ok = p->w > 0 && p->h > 0 && p->levels >= 1 && p->levels <= TP_MAX_LEVELS &&
             p->levels == count_levels(p->w, p->h);
    }
    if (ok) {
        pack_layout(p);
        size_t total = p->off[p->levels - 1] + p->size[p->levels - 1];
        p->data = malloc(total);
        ok = p->data && fread(p->data, 1, total, f) == total;
        if (!ok) { free(p->data); p->data = NULL; }
    }
    fclose(f);
    return ok;
}

/* ── bake ──────────────────────────────────────────────────────────────── */
static float   s_lin[256];
static uint8_t s_srgb[4096];          /* linear [0,1] in 4096 steps -> sRGB byte */

static void tables(void)
{
    static int done = 0;
    if (done) return;
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        s_lin[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    for (int i = 0; i < 4096; i++) {
        double l = i / 4095.0;
        double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        s_srgb[i] = (uint8_t)(c * 255.0 + 0.5);
    }
    done = 1;
}

/* Next mip: 2x2 box in linear light (clamped at odd edges). */
static uint8_t *half_rgba(const uint8_t *src, int w, int h, int *nw, int *nh)
{
    *nw = w > 1 ? w / 2 : 1;  *nh = h > 1 ? h / 2 : 1;
    uint8_t *dst = malloc((size_t)*nw * *nh * 4);
    if (!dst) return NULL;
    int ow = *nw, oh = *nh;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int x0 = 2 * x, y0 = 2 * y;
            int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            const uint8_t *a = src + ((size_t)y0 * w + x0) * 4, *b = src + ((size_t)y0 * w + x1) * 4;
            const uint8_t *c = src + ((size_t)y1 * w + x0) * 4, *d = src + ((size_t)y1 * w + x1) * 4;
            uint8_t *o = dst + ((size_t)y * ow + x) * 4;
            for (int ch = 0; ch < 3; ch++) {
                float l = 0.25f * (s_lin[a[ch]] + s_lin[b[ch]] + s_lin[c[ch]] + s_lin[d[ch]]);
                o[ch] = s_srgb[(int)(l * 4095.0f + 0.5f)];
            }
            o[3] = 255;
        }
    return dst;
}

static int bake(const char *src, Pack *p)
{
    int w, h, n;
    uint8_t *px = stbi_load(src, &w, &h, &n, 4);
    if (!px) return 0;
    tables();
    p->w = w;  p->h = h;  p->levels = count_levels(w, h);
    pack_layout(p);
    p->data = malloc(p->off[p->levels - 1] + p->size[p->levels - 1]);
    if (!p->data) { stbi_image_free(px); return 0; }

    uint8_t *lvl = px;
    int lw = w, lh = h;
    for (int l = 0; l < p->levels; l++) {
        bc7_encode_image(lvl, lw, lh, p->data + p->off[l]);
        if (l + 1 < p->levels) {
            int nw, nh;
            uint8_t *next = half_rgba(lvl, lw, lh, &nw, &nh);
            if (lvl == px) stbi_image_free(px); else free(lvl);
            if (!next) { free(p->data); p->data = NULL; return 0; }
            lvl = next;  lw = nw;  lh = nh;
        }
    }
    if (lvl == px) stbi_image_free(px); else free(lvl);
    return 1;
}

static int newer_or_same(const char *a, const char *b)   /* mtime(a) >= mtime(b) */
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    return sa.st_mtime >= sb.st_mtime;
}

unsigned int texpack_load_srgb(const char *path, int max_px, const char *what)
{
    if (!path || !path[0]) return 0;
    if (!GLEW_ARB_texture_compression_bptc && !GLEW_VERSION_4_2) return 0;

    char cache[1100];
    snprintf(cache, sizeof cache, "%s.bc7.dds", path);
    Pack p;
    memset(&p, 0, sizeof p);
    int cached = newer_or_same(cache, path) && dds_read(cache, &p);
    if (!cached) {
        fprintf(stdout, "[texpack] baking %s texture '%s' to BC7 (once; cached as %s)\n",
                what, path, cache);
        fflush(stdout);
        if (!bake(path, &p)) return 0;
        if (!dds_write(cache, &p))
            fprintf(stderr, "[texpack] could not write cache '%s' (baking again next run)\n", cache);
    }

    GLint gl_max = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max);
    if (gl_max > 0 && (max_px <= 0 || max_px > gl_max)) max_px = gl_max;
    int base = 0;
    while (base + 1 < p.levels && max_px > 0 && (p.w >> base) > max_px) base++;

    while (glGetError() != GL_NO_ERROR) {}    /* judge only our own upload */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    size_t vram = 0;
    for (int l = base; l < p.levels; l++) {
        int w = p.w >> l, h = p.h >> l;
        glCompressedTexImage2D(GL_TEXTURE_2D, l - base, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,
                               w < 1 ? 1 : w, h < 1 ? 1 : h, 0,
                               (GLsizei)p.size[l], p.data + p.off[l]);
        vram += p.size[l];
    }
    free(p.data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, p.levels - 1 - base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);        /* longitude wraps */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); /* poles do not   */
    if (GLEW_EXT_texture_filter_anisotropic) {
        GLfloat aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso > 8.0f ? 8.0f : aniso);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        fprintf(stderr, "[texpack] BC7 upload of '%s' failed\n", path);
        return 0;
    }
    fprintf(stdout, "[texpack] %s texture %dx%d BC7, %.0f MB VRAM%s\n", what,
            p.w >> base, p.h >> base, vram / 1048576.0, cached ? "" : " (baked)");
    return tex;
}
