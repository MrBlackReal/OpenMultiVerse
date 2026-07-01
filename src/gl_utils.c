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
    fread(buf, 1, sz, f);
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
    char prelude[128];
    int plen = snprintf(prelude, sizeof(prelude),
                        "#define DEPTH_FAR %.8e\n", (double)RENDER_DEPTH_FAR);
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
