/*
 * OpenGL 3.3 core backend. Entry points are loaded through
 * vt3_platform_gl_proc (SDL_GL_GetProcAddress), so this file does not link
 * against the OpenGL framework. The GL types and enums come from the
 * platform's GL 3.2 header.
 *
 * Frame flow:
 *   begin_frame  binds the logical-size FBO (the game draws here)
 *   draw/clear   go into the FBO
 *   present      blits the FBO into the letterboxed window rect; the caller swaps
 */
#define GL_SILENCE_DEPRECATION 1
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_renderer.h"

#include <OpenGL/gl3.h>
#include <stdint.h>
#include <string.h>

/* One X-macro list drives the typedefs, the pointers, and the loader. */
#define VT3_GL_FUNCS(X)                                                                   \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                           \
    X(void, glClear, (GLbitfield))                                                        \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                 \
    X(void, glEnable, (GLenum))                                                           \
    X(void, glDisable, (GLenum))                                                          \
    X(void, glBlendFunc, (GLenum, GLenum))                                                \
    X(void, glPixelStorei, (GLenum, GLint))                                               \
    X(const GLubyte *, glGetString, (GLenum))                                             \
    X(void, glGenTextures, (GLsizei, GLuint *))                                           \
    X(void, glDeleteTextures, (GLsizei, const GLuint *))                                  \
    X(void, glBindTexture, (GLenum, GLuint))                                              \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                     \
    X(void, glTexParameteriv, (GLenum, GLenum, const GLint *))                            \
    X(void, glGenBuffers, (GLsizei, GLuint *))                                            \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *))                                   \
    X(void, glBindBuffer, (GLenum, GLuint))                                               \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))                     \
    X(void, glGenVertexArrays, (GLsizei, GLuint *))                                       \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint *))                              \
    X(void, glBindVertexArray, (GLuint))                                                  \
    X(void, glEnableVertexAttribArray, (GLuint))                                          \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(GLuint, glCreateShader, (GLenum))                                                   \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))      \
    X(void, glCompileShader, (GLuint))                                                    \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                     \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                   \
    X(void, glDeleteShader, (GLuint))                                                     \
    X(GLuint, glCreateProgram, (void))                                                    \
    X(void, glAttachShader, (GLuint, GLuint))                                             \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                       \
    X(void, glLinkProgram, (GLuint))                                                      \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                    \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                  \
    X(void, glDeleteProgram, (GLuint))                                                    \
    X(void, glUseProgram, (GLuint))                                                       \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                              \
    X(void, glUniform1i, (GLint, GLint))                                                  \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                       \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                       \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                              \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                          \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))              \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                         \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                       \
    X(void, glActiveTexture, (GLenum))                                                    \
    X(GLenum, glGetError, (void))

#define VT3_DECL_TYPE(ret, name, params) typedef ret (*vt3_pfn_##name) params;
#define VT3_DECL_VAR(ret, name, params) static vt3_pfn_##name p_##name;
VT3_GL_FUNCS(VT3_DECL_TYPE)
VT3_GL_FUNCS(VT3_DECL_VAR)

#define VT3_LOAD(ret, name, params)                                                   \
    p_##name = (vt3_pfn_##name)vt3_platform_gl_proc(#name);                           \
    if (!p_##name) { VT3_ERROR("GL 3.3 entry point missing: %s", #name); missing++; }

#define VT3_MAX_TEX 256
#define VT3_VERTEX_BYTES ((GLsizeiptr)sizeof(vt3_tlvertex))

typedef struct gl_tex {
    GLuint id;
    int w, h;
    vt3_texfmt fmt;
    int used;
} gl_tex;

static struct {
    int logical_w, logical_h;
    GLuint fbo, fbo_color;
    GLuint white;                 /* 1x1 white, bound for untextured draws */
    GLuint program, vao, vbo;
    GLint u_logical_size, u_tex, u_use_tex;
    gl_tex tex[VT3_MAX_TEX];
} g;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec4 aPosRhw;\n"
    "layout(location = 1) in vec4 aColor;\n"        /* bytes arrive as B,G,R,A */
    "layout(location = 2) in vec2 aUV;\n"
    "uniform vec2 uLogicalSize;\n"
    "out vec4 vColor;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    float invw = 1.0 / aPosRhw.w;\n"
    "    vec2 ndc = vec2(aPosRhw.x / uLogicalSize.x * 2.0 - 1.0,\n"
    "                    1.0 - aPosRhw.y / uLogicalSize.y * 2.0);\n"
    "    float zndc = aPosRhw.z * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc * invw, zndc * invw, invw);\n"
    "    vColor = aColor.bgra;\n"
    "    vUV = aUV;\n"
    "}\n";

static const char *k_fs =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTex;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "    vec4 c = vColor;\n"
    "    if (uUseTex != 0) c *= texture(uTex, vUV);\n"
    "    oColor = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        VT3_ERROR("shader compile failed: %s", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static int gl_load_and_check(void)
{
    int missing = 0;
    VT3_GL_FUNCS(VT3_LOAD)
    return missing;
}

static int gl_init(int logical_w, int logical_h)
{
    if (gl_load_and_check() != 0) return -1;

    const char *ver = (const char *)p_glGetString(GL_VERSION);
    const char *rend = (const char *)p_glGetString(GL_RENDERER);
    VT3_INFO("GL: %s | %s", ver ? ver : "?", rend ? rend : "?");

    g.logical_w = logical_w;
    g.logical_h = logical_h;

    GLuint vs = compile(GL_VERTEX_SHADER, k_vs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) return -1;
    g.program = p_glCreateProgram();
    p_glAttachShader(g.program, vs);
    p_glAttachShader(g.program, fs);
    p_glBindAttribLocation(g.program, 0, "aPosRhw");
    p_glBindAttribLocation(g.program, 1, "aColor");
    p_glBindAttribLocation(g.program, 2, "aUV");
    p_glLinkProgram(g.program);
    GLint linked = 0;
    p_glGetProgramiv(g.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        p_glGetProgramInfoLog(g.program, sizeof log, NULL, log);
        VT3_ERROR("program link failed: %s", log);
        return -1;
    }
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    g.u_logical_size = p_glGetUniformLocation(g.program, "uLogicalSize");
    g.u_tex = p_glGetUniformLocation(g.program, "uTex");
    g.u_use_tex = p_glGetUniformLocation(g.program, "uUseTex");

    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    p_glGenBuffers(1, &g.vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, VT3_VERTEX_BYTES, (const void *)16);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)20);

    /* Offscreen target at logical resolution. */
    p_glGenTextures(1, &g.fbo_color);
    p_glBindTexture(GL_TEXTURE_2D, g.fbo_color);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, logical_w, logical_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* The sampler is always bound, so untextured draws use a white texel. */
    const uint32_t white_px = 0xFFFFFFFFu;
    p_glGenTextures(1, &g.white);
    p_glBindTexture(GL_TEXTURE_2D, g.white);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white_px);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    p_glGenFramebuffers(1, &g.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.fbo_color, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        VT3_ERROR("logical framebuffer is incomplete");
        return -1;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* TODO: map D3DRS_* blend states */
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    return 0;
}

static void gl_shutdown(void)
{
    for (int i = 0; i < VT3_MAX_TEX; i++) {
        if (g.tex[i].used) {
            p_glDeleteTextures(1, &g.tex[i].id);
            g.tex[i].used = 0;
        }
    }
    if (g.fbo) p_glDeleteFramebuffers(1, &g.fbo);
    if (g.fbo_color) p_glDeleteTextures(1, &g.fbo_color);
    if (g.white) p_glDeleteTextures(1, &g.white);
    if (g.vbo) p_glDeleteBuffers(1, &g.vbo);
    if (g.vao) p_glDeleteVertexArrays(1, &g.vao);
    if (g.program) p_glDeleteProgram(g.program);
    memset(&g, 0, sizeof g);
}

/* Handle = slot + 1, so 0 stays "no texture". */
static int gl_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    int slot = -1;
    for (int i = 0; i < VT3_MAX_TEX; i++) {
        if (!g.tex[i].used) { slot = i; break; }
    }
    if (slot < 0 || w <= 0 || h <= 0) return -1;

    GLuint id;
    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_2D, id);
    GLenum internal = (fmt == VT3_TEXFMT_A8) ? GL_R8 : GL_RGBA8;
    p_glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0,
                   (fmt == VT3_TEXFMT_A8) ? GL_RED : GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (fmt == VT3_TEXFMT_X8R8G8B8) {
        const GLint swz[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ONE };
        p_glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swz);
    } else if (fmt == VT3_TEXFMT_A8) {
        /* White with alpha from the red channel. */
        const GLint swz[4] = { GL_ONE, GL_ONE, GL_ONE, GL_RED };
        p_glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swz);
    }

    g.tex[slot].id = id;
    g.tex[slot].w = w;
    g.tex[slot].h = h;
    g.tex[slot].fmt = fmt;
    g.tex[slot].used = 1;
    *out = (vt3_tex)(slot + 1);
    return 0;
}

static void gl_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used) return;
    gl_tex *tx = &g.tex[t - 1];
    if (w != tx->w || h != tx->h) return;   /* TODO: sub-rect uploads (UpdateTexture) */
    p_glBindTexture(GL_TEXTURE_2D, tx->id);
    if (tx->fmt == VT3_TEXFMT_A8) {
        p_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, pixels);
    } else {
        p_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void gl_tex_destroy(vt3_tex t)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used) return;
    p_glDeleteTextures(1, &g.tex[t - 1].id);
    g.tex[t - 1].used = 0;
}

static void gl_bind_logical_fbo(void)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glViewport(0, 0, g.logical_w, g.logical_h);
}

static void gl_begin_frame(void)
{
    gl_bind_logical_fbo();
}

static void gl_clear(uint32_t argb)
{
    gl_bind_logical_fbo();
    p_glClearColor(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                   (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

static void gl_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex)
{
    gl_bind_logical_fbo();
    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * VT3_VERTEX_BYTES, v, GL_STREAM_DRAW);

    p_glUseProgram(g.program);
    p_glUniform2f(g.u_logical_size, (GLfloat)g.logical_w, (GLfloat)g.logical_h);
    int use_tex = (tex != 0 && tex <= VT3_MAX_TEX && g.tex[tex - 1].used);
    p_glUniform1i(g.u_use_tex, use_tex);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, use_tex ? g.tex[tex - 1].id : g.white);
    p_glUniform1i(g.u_tex, 0);

    p_glDrawArrays(prim == VT3_PRIM_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES, 0, (GLsizei)count);
}

static void gl_present(const vt3_rect *dst, int nearest, int drawable_w, int drawable_h)
{
    /* Clear the whole drawable to black first, so the bars are black. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, drawable_w, drawable_h);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);

    /* dst is top-left origin; GL blit wants bottom-left. */
    GLint x0 = dst->x, x1 = dst->x + dst->w;
    GLint y0 = drawable_h - (dst->y + dst->h), y1 = drawable_h - dst->y;

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBlitFramebuffer(0, 0, g.logical_w, g.logical_h, x0, y0, x1, y1,
                        GL_COLOR_BUFFER_BIT, nearest ? GL_NEAREST : GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

const vt3_gfx_ops vt3_gfx_gl33 = {
    .name = "OpenGL 3.3 core",
    .init = gl_init,
    .shutdown = gl_shutdown,
    .tex_create = gl_tex_create,
    .tex_upload = gl_tex_upload,
    .tex_destroy = gl_tex_destroy,
    .begin_frame = gl_begin_frame,
    .clear = gl_clear,
    .draw = gl_draw,
    .present = gl_present,
};
