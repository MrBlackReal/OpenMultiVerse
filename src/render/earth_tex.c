/*
 * earth_tex.c — real satellite imagery for Earth (see earth_tex.h).
 */
#include "earth_tex.h"
#include "gl_utils.h"
#include "settings.h"
#include "stb_image.h"

#include <GL/glew.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EARTH_DAY_UNIT   6
#define EARTH_NIGHT_UNIT 7

static GLuint s_day, s_night;

/* Halve an RGB image in place (2x2 box filter) until its width fits max_px.
 * Equirectangular maps are 2:1, so both axes halve together. */
static unsigned char *downsample_to(unsigned char *px, int *w, int *h, int max_px)
{
    while (max_px > 0 && *w > max_px && *w >= 2 && *h >= 2) {
        int nw = *w / 2, nh = *h / 2;
        unsigned char *out = (unsigned char *)malloc((size_t)nw * nh * 3);
        if (!out) break;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 3; c++) {
                    const unsigned char *r0 = px + ((size_t)(2*y)     * *w + 2*x) * 3 + c;
                    const unsigned char *r1 = px + ((size_t)(2*y + 1) * *w + 2*x) * 3 + c;
                    out[((size_t)y * nw + x) * 3 + c] =
                        (unsigned char)((r0[0] + r0[3] + r1[0] + r1[3] + 2) / 4);
                }
        stbi_image_free(px);     /* first pass frees stb's buffer ... */
        px = out;                /* ... later ones free ours (plain malloc) */
        *w = nw; *h = nh;
    }
    return px;
}

static GLuint load_one(const char *path, int max_px, const char *what)
{
    if (!path || !path[0]) return 0;
    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 3);
    if (!px) {
        fprintf(stderr, "[Earth] cannot load %s texture '%s' (%s) — using the "
                        "procedural surface\n", what, path, stbi_failure_reason());
        return 0;
    }
    int w0 = w;
    px = downsample_to(px, &w, &h, max_px);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* sRGB storage: sampling returns linear light, mipmaps average correctly. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);        /* longitude wraps */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); /* poles do not   */
    if (GLEW_EXT_texture_filter_anisotropic) {
        GLfloat aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
        /* Grazing views of the limb stretch texels hard; anisotropy keeps the
         * coastlines there sharp instead of smearing. */
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso > 8.0f ? 8.0f : aniso);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (w != w0) free(px); else stbi_image_free(px);

    fprintf(stdout, "[Earth] %s texture %dx%d%s <- %s\n", what, w, h,
            w != w0 ? " (downsampled)" : "", path);
    return tex;
}

void earth_tex_init(void)
{
    earth_tex_shutdown();
    int cap = g_settings.earth_texture_max_px;
    s_day   = load_one(g_settings.earth_day_texture,   cap, "day");
    s_night = s_day ? load_one(g_settings.earth_night_texture, cap, "night") : 0;
}

void earth_tex_shutdown(void)
{
    if (s_day)   glDeleteTextures(1, &s_day);
    if (s_night) glDeleteTextures(1, &s_night);
    s_day = s_night = 0;
}

int earth_tex_ready(void) { return s_day != 0; }

void earth_tex_bind(unsigned int prog, int on)
{
    GLint loc = glGetUniformLocation(prog, "u_earth_tex");
    int use = on && s_day;
    glUniform1i(loc, use ? (s_night ? 2 : 1) : 0);    /* 2 = day + night */
    if (!use) return;
    glActiveTexture(GL_TEXTURE0 + EARTH_DAY_UNIT);
    glBindTexture(GL_TEXTURE_2D, s_day);
    glUniform1i(glGetUniformLocation(prog, "u_earth_day"), EARTH_DAY_UNIT);
    glActiveTexture(GL_TEXTURE0 + EARTH_NIGHT_UNIT);
    glBindTexture(GL_TEXTURE_2D, s_night ? s_night : s_day);
    glUniform1i(glGetUniformLocation(prog, "u_earth_night"), EARTH_NIGHT_UNIT);
    glActiveTexture(GL_TEXTURE0);
}
