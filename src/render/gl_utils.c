/*
 * gl_utils.c — thin wrappers around common OpenGL object creation
 *
 * All functions return 0 / NULL on failure and print a diagnostic to stderr.
 * Ownership: the caller is responsible for deleting returned objects
 * (glDeleteProgram, glDeleteVertexArrays, glDeleteBuffers).
 *
 * VAO/VBO binding convention:
 *   gl_vao_create() leaves the new VAO bound.
 *   gl_vbo_create() / gl_ebo_create() leave the new buffer bound to its
 *   target. The caller sets up vertex attribute pointers, then unbinds
 *   the VAO with glBindVertexArray(0).
 */
#include "gl_utils.h"
#include "star_veil.h"
#include "dust_field.h"

/* ---------------------------------------------------------------- private */

/* Read an entire file into a heap-allocated NUL-terminated string.
 * Uses binary mode so line endings are preserved as-is for GLSL. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[GL] cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    /* A short read means a truncated shader, which would fail to compile with a
     * confusing message — report it here instead. (fread is warn_unused_result
     * under _FORTIFY_SOURCE, so the return value must be consumed regardless.) */
    size_t got = fread(buf, 1, (size_t)sz, f);
    if (got != (size_t)sz) {
        fprintf(stderr, "[GL] short read on '%s' (%zu of %ld bytes)\n", path, got, sz);
        free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Splice a shared prelude of #defines right after the "#version ..." line so every
 * shader draws its constants from one place. Currently exposes DEPTH_FAR — the single
 * source of truth for the logarithmic-depth range (see RENDER_DEPTH_FAR in common.h),
 * so all depth-writing passes normalise gl_FragDepth identically and sort together.
 *
 * Returns a newly malloc'd NUL-terminated string the caller must free. On any failure
 * it returns a plain copy of src (never NULL unless src is NULL). GLSL requires
 * #version to be the first token, so the prelude is inserted after that line; if no
 * #version line is present it is prepended. */
static char *inject_prelude(const char *src) {
    if (!src) return NULL;
    /* Also the star veil (star_veil.h): the same glare test as the C side,
     * so every background layer drowns in a nearby star's glare identically.
     * Shaders that never call veil_vis() compile the uniforms away. */
    char prelude[4096];
    int plen = snprintf(prelude, sizeof(prelude),
        "#define DEPTH_FAR %.8e\n"
        "#define STAR_FADE_MAG0 %.6f\n"
        "#define STAR_FADE_FLOOR %.6f\n"
        "#define VEIL_DIFFUSE %.6f\n"
        "#define VEIL_PSF_GLSL %.6f\n"
        "#define VEIL_FLOOR_GLSL %.6f\n"
        "#define VEIL_CORE_DEG_GLSL %.6f\n"
        "uniform vec3  u_veil_dir;\n"
        "uniform float u_veil_e;       /* E_psf   */\n"
        "uniform float u_veil_f;       /* E_floor */\n"
        "float veil_vis(vec3 dir, float lum) {\n"
        "    if (u_veil_e <= 0.0 && u_veil_f <= 0.0) return 1.0;\n"
        "    float c  = clamp(dot(normalize(dir), u_veil_dir), -1.0, 1.0);\n"
        "    float th = max(degrees(acos(c)), %.6f);\n"
        "    float glare = u_veil_e * %.6f / (th * th) + u_veil_f * %.6f;\n"
        "    return smoothstep(%.6f, %.6f, log2(max(lum, 1e-6) / glare));\n"
        "}\n"
        /* Interstellar dust (dust_field.h): A_V along a segment, from the 3D
         * texture render_dust_uniforms() binds. Positions are in the calling
         * program's frame; u_dust_origin is the cube centre (the Sun) in that
         * frame. One sample per two voxels (6..32), each read one mip level
         * finer than its step, so a long ray averages instead of aliasing.
         * Against a half-voxel reference on 1 kpc rays through the real cube:
         * 2.5%% median, 0.07 mag worst; a fixed 12 at the step's own level was
         * 23%% and 0.47 mag. */
        "uniform sampler3D u_dust;\n"
        "uniform float u_dust_on;\n"
        "uniform float u_dust_half;    /* cube half-size, AU            */\n"
        "uniform float u_dust_vmax;    /* density scale, ZGR23 per pc   */\n"
        "uniform float u_dust_dim;\n"
        "uniform vec3  u_dust_origin;\n"
        "#define DUST_AG_PER_AV %.6f\n"
        "#define DUST_K vec3(%.6f, %.6f, %.6f)\n"
        "float dust_av(vec3 a, vec3 b) {\n"
        "    if (u_dust_on <= 0.0) return 0.0;\n"
        "    vec3 pa = a - u_dust_origin, d = b - a;\n"
        "    vec3 ds = mix(vec3(1e-6), d, step(1e-6, abs(d)));\n"
        "    vec3 ta = (-u_dust_half - pa) / ds, tb = (u_dust_half - pa) / ds;\n"
        "    vec3 lo = min(ta, tb), hi = max(ta, tb);\n"
        "    float t0 = max(max(lo.x, lo.y), max(lo.z, 0.0));\n"
        "    float t1 = min(min(hi.x, hi.y), min(hi.z, 1.0));\n"
        "    if (t0 >= t1) return 0.0;\n"
        "    float len = length(d) * (t1 - t0);\n"
        "    float vox = 2.0 * u_dust_half / u_dust_dim;\n"
        "    int   n   = int(clamp(ceil(len / (2.0 * vox)), 6.0, 32.0));\n"
        "    float stp = len / float(n);\n"
        "    float lod = max(0.0, log2(stp / vox) - 1.0);\n"
        "    float s = 0.0;\n"
        "    for (int i = 0; i < n; i++) {\n"
        "        vec3 p = pa + d * mix(t0, t1, (float(i) + 0.5) / float(n));\n"
        "        s += textureLod(u_dust, p / (2.0 * u_dust_half) + 0.5, lod).r;\n"
        "    }\n"
        "    return %.6f * s * u_dust_vmax * stp / 206264.806;\n"
        "}\n"
        /* Display-channel colour change for a column A_V, luminance-neutral:
         * the G-band dimming belongs in the magnitude, this is only the tint. */
        "vec3 dust_redden(float av) {\n"
        "    vec3 k = DUST_K;\n"
        "    return pow(vec3(10.0), -0.4 * av * (k - (k.r + k.g + k.b) / 3.0));\n"
        "}\n",
        (double)RENDER_DEPTH_FAR, STAR_FADE_MAG0, STAR_FADE_FLOOR, VEIL_DIFFUSE, VEIL_PSF, VEIL_FLOOR, VEIL_CORE_DEG, VEIL_CORE_DEG, VEIL_PSF, VEIL_FLOOR,
        VEIL_LO, VEIL_HI,
        DUST_AG_PER_AV, DUST_KR, DUST_KG, DUST_KB, DUST_AV_PER_ZGR);
    if (plen < 0 || plen >= (int)sizeof(prelude)) plen = 0;  /* fall back to plain copy */

    size_t slen = strlen(src);
    char *out = (char *)malloc(slen + (size_t)plen + 1);
    if (!out) return NULL;

    /* Find the end of the #version line (insertion point) */
    const char *ins = src;
    if (plen > 0) {
        const char *v = strstr(src, "#version");
        if (v) {
            const char *nl = strchr(v, '\n');
            ins = nl ? nl + 1 : src + slen;   /* after the newline, or EOF */
        } else {
            ins = src;                        /* no #version — prepend */
        }
    }

    size_t head = (size_t)(ins - src);
    memcpy(out, src, head);
    if (plen > 0) memcpy(out + head, prelude, (size_t)plen);
    memcpy(out + head + (size_t)plen, ins, slen - head + 1);  /* +1 copies the NUL */
    return out;
}

/* Compile a single shader stage and return its handle, or 0 on failure.
 * The info log (up to 1 KB) is printed to stderr on compile error.
 * path is used only for the error message — it is not re-read here. */
static GLuint compile_shader(GLenum type, const char *src, const char *path) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] shader compile error (%s):\n%s\n", path, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* ---------------------------------------------------------------- public */

/* Load, compile, and link a vertex+fragment shader pair from disk.
 * Shader objects are deleted after linking — only the program handle survives.
 * Returns the linked program, or 0 on any failure. */
GLuint gl_shader_load(const char *vert_path, const char *frag_path) {
    char *vraw = read_file(vert_path);
    char *fraw = read_file(frag_path);
    if (!vraw || !fraw) { free(vraw); free(fraw); return 0; }

    /* Splice the shared prelude (DEPTH_FAR, …) after each stage's #version line. */
    char *vsrc = inject_prelude(vraw);
    char *fsrc = inject_prelude(fraw);
    free(vraw); free(fraw);
    if (!vsrc || !fsrc) { free(vsrc); free(fsrc); return 0; }

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vsrc, vert_path);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc, frag_path);
    free(vsrc); free(fsrc);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    /* Shader objects are no longer needed once the program is linked */
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] program link error (%s / %s):\n%s\n",
                vert_path, frag_path, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* Create and bind a VAO.  The caller must set up vertex attribute pointers
 * before calling glBindVertexArray(0). */
GLuint gl_vao_create(void) {
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    return vao;
}

/* Create and bind a VBO, optionally uploading initial data.
 * data may be NULL for a zero-initialised or to-be-filled buffer.
 * usage is typically GL_STATIC_DRAW or GL_DYNAMIC_DRAW. */
GLuint gl_vbo_create(size_t bytes, const void *data, GLenum usage) {
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, usage);
    return vbo;
}

/* Create and bind an EBO (index buffer) with static data.
 * Must be called while a VAO is bound so the binding is captured. */
GLuint gl_ebo_create(size_t bytes, const unsigned int *data) {
    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STATIC_DRAW);
    return ebo;
}
