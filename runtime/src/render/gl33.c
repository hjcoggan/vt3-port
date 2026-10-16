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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One X-macro list drives the typedefs, the pointers, and the loader. */
#define VT3_GL_FUNCS(X)                                                                   \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                           \
    X(void, glClear, (GLbitfield))                                                        \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                 \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                  \
    X(void, glDepthRange, (GLclampd, GLclampd))                                           \
    X(void, glEnable, (GLenum))                                                           \
    X(void, glDisable, (GLenum))                                                          \
    X(void, glBlendFunc, (GLenum, GLenum))                                                \
    X(void, glBlendEquation, (GLenum))                                                    \
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
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat))                                                  \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                       \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                       \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                              \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                          \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))              \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                         \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                       \
    X(void, glActiveTexture, (GLenum))                                                    \
    X(GLenum, glGetError, (void))                                                         \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))                              \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))                      \
    X(void, glDepthFunc, (GLenum))                                                        \
    X(void, glDepthMask, (GLboolean))                                                     \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                    \
    X(void, glCullFace, (GLenum))                                                         \
    X(void, glFrontFace, (GLenum))                                                        \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))       \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *))                                      \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *))                             \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                         \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                    \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                  \
    X(void, glDisableVertexAttribArray, (GLuint))                                         \
    X(void, glClearDepth, (GLclampd))

#define VT3_DECL_TYPE(ret, name, params) typedef ret (*vt3_pfn_##name) params;
#define VT3_DECL_VAR(ret, name, params) static vt3_pfn_##name p_##name;
VT3_GL_FUNCS(VT3_DECL_TYPE)
VT3_GL_FUNCS(VT3_DECL_VAR)

#define VT3_LOAD(ret, name, params)                                                   \
    p_##name = (vt3_pfn_##name)vt3_platform_gl_proc(#name);                           \
    if (!p_##name) { VT3_ERROR("GL 3.3 entry point missing: %s", #name); missing++; }

/* GL texture slots. A slot is freed when its D3D texture is released; a game that keeps
 * several hundred textures alive (a match loads its HUD and players that way) needs more than 256. */
#define VT3_MAX_TEX 4096
#define VT3_MAX_PROGS 256
#define VT3_CONST_REGS 256

typedef struct gl_prog {
    GLuint id;
    GLint loc_vsc, loc_psc;
    GLint loc_vsb[16], loc_psb[16];
    GLint loc_samp[VT3_MAX_SAMPLERS];
    GLint loc_at_func, loc_at_ref;   /* alpha test uniforms (sm2_glsl.c) */
    GLint loc_flip;                  /* vt3_flip: -1 when drawing into a render texture */
    int used;
} gl_prog;
#define VT3_VERTEX_BYTES ((GLsizeiptr)sizeof(vt3_tlvertex))

typedef struct gl_tex {
    GLuint id;
    int w, h;
    vt3_texfmt fmt;
    int used;
    int cube;                     /* GL_TEXTURE_CUBE_MAP rather than GL_TEXTURE_2D */
    int rt;                       /* a render target: fbo and depth below are its own */
    GLuint fbo, depth;
} gl_tex;

static struct {
    int logical_w, logical_h;
    GLuint fbo, fbo_color;
    GLuint white;                 /* 1x1 white, bound for untextured draws */
    GLuint zero;                  /* 1x1 zero texel, bound where a texture cannot be used (VT3_TEX_ZERO) */
    GLuint zero_cube;             /* 1x1 black on each face, bound on a cube sampler with no cube texture */
    GLuint ebo, depth_rb;
    uint32_t attr_mask;           /* attribute locations currently enabled */
    float zero_consts[VT3_CONST_REGS * 4];
    gl_prog progs[VT3_MAX_PROGS];
    GLuint program, vao, vbo;
    GLuint vao_tl, vbo_tl;        /* the fixed-function (XYZRHW) path: its own VAO and buffer */
    GLint u_logical_size, u_tex, u_use_tex;
    gl_tex tex[VT3_MAX_TEX];
    int scissor_on;               /* set_scissor state, applied to every logical-FBO operation */
    vt3_rect scissor;             /* logical pixels, top-left origin */
    vt3_tex tgt;                  /* render target: 0 = the logical back buffer */
    GLint u_flip;                 /* uFlip in k_vs */
} g;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec4 aPosRhw;\n"
    "layout(location = 1) in vec4 aColor;\n"        /* bytes arrive as B,G,R,A */
    "layout(location = 2) in vec2 aUV;\n"
    "uniform vec2 uLogicalSize;\n"
    "uniform float uFlip;\n"                       /* -1 when drawing into a render texture */
    "out vec4 vColor;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    float invw = 1.0 / aPosRhw.w;\n"
    "    vec2 ndc = vec2(aPosRhw.x / uLogicalSize.x * 2.0 - 1.0,\n"
    "                    1.0 - aPosRhw.y / uLogicalSize.y * 2.0);\n"
    "    ndc.y *= uFlip;\n"
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
    g.scissor_on = 0;

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
    g.u_flip = p_glGetUniformLocation(g.program, "uFlip");
    g.u_tex = p_glGetUniformLocation(g.program, "uTex");
    g.u_use_tex = p_glGetUniformLocation(g.program, "uUseTex");

    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    p_glGenBuffers(1, &g.vbo);
    p_glGenBuffers(1, &g.ebo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, VT3_VERTEX_BYTES, (const void *)16);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)20);
    g.attr_mask = 0x7u;           /* attributes 0-2 are enabled on g.vao above */

    /* The fixed-function path has its own VAO with the XYZRHW layout. The program path sets
     * g.vao's attributes for each draw, so this layout must not share that VAO. */
    p_glGenVertexArrays(1, &g.vao_tl);
    p_glBindVertexArray(g.vao_tl);
    p_glGenBuffers(1, &g.vbo_tl);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo_tl);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, VT3_VERTEX_BYTES, (const void *)16);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VT3_VERTEX_BYTES, (const void *)20);
    p_glBindVertexArray(g.vao);

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

    const uint32_t zero_px = 0u;
    p_glGenTextures(1, &g.zero);
    p_glBindTexture(GL_TEXTURE_2D, g.zero);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &zero_px);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    p_glGenTextures(1, &g.zero_cube);
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, g.zero_cube);
    for (int f = 0; f < 6; f++)
        p_glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &zero_px);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    p_glGenFramebuffers(1, &g.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.fbo_color, 0);
    p_glGenRenderbuffers(1, &g.depth_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g.depth_rb);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, logical_w, logical_h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g.depth_rb);
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
    if (g.depth_rb) p_glDeleteRenderbuffers(1, &g.depth_rb);
    if (g.ebo) p_glDeleteBuffers(1, &g.ebo);
    for (int i = 0; i < VT3_MAX_PROGS; i++) {
        if (g.progs[i].used) p_glDeleteProgram(g.progs[i].id);
    }
    if (g.vbo) p_glDeleteBuffers(1, &g.vbo);
    if (g.vao) p_glDeleteVertexArrays(1, &g.vao);
    if (g.vbo_tl) p_glDeleteBuffers(1, &g.vbo_tl);
    if (g.vao_tl) p_glDeleteVertexArrays(1, &g.vao_tl);
    if (g.program) p_glDeleteProgram(g.program);
    memset(&g, 0, sizeof g);
}

/* The first free texture slot, or -1 when the table is full. */
static int gl_free_slot(void)
{
    for (int i = 0; i < VT3_MAX_TEX; i++) {
        if (!g.tex[i].used) return i;
    }
    static int warned_full;
    if (!warned_full) {
        warned_full = 1;
        VT3_WARN("GL texture slots full (%d): new textures have no handle and draw white", VT3_MAX_TEX);
    }
    return -1;
}

/* Handle = slot + 1, so 0 stays "no texture". */
static int gl_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    int slot = gl_free_slot();
    if (slot < 0) return -1;
    if (w <= 0 || h <= 0) return -1;

    GLuint id;
    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_2D, id);
    GLenum internal = (fmt == VT3_TEXFMT_A8) ? GL_R8 : (fmt == VT3_TEXFMT_RGBA16F) ? GL_RGBA16F : GL_RGBA8;
    if (fmt == VT3_TEXFMT_RGBA16F)
        p_glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    else
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
    g.tex[slot].cube = 0;
    g.tex[slot].used = 1;
    *out = (vt3_tex)(slot + 1);
    return 0;
}

static void gl_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used || g.tex[t - 1].cube) return;
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
    if (g.tex[t - 1].rt) {
        p_glDeleteFramebuffers(1, &g.tex[t - 1].fbo);
        p_glDeleteRenderbuffers(1, &g.tex[t - 1].depth);
        if (g.tgt == t) g.tgt = 0;
    }
    p_glDeleteTextures(1, &g.tex[t - 1].id);
    g.tex[t - 1].used = 0;
    g.tex[t - 1].rt = 0;
}

/* A cube map: six square faces, level 0 only, BGRA like the 2D colour textures. */
static int gl_cube_create(vt3_tex *out, int size, vt3_texfmt fmt)
{
    if (!out || size <= 0 || fmt == VT3_TEXFMT_A8) return -1;
    int slot = gl_free_slot();
    if (slot < 0) return -1;
    GLuint id;
    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, id);
    for (int f = 0; f < 6; f++)
        p_glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, size, size, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    g.tex[slot].id = id;
    g.tex[slot].w = size;
    g.tex[slot].h = size;
    g.tex[slot].fmt = fmt;
    g.tex[slot].cube = 1;
    g.tex[slot].used = 1;
    *out = (vt3_tex)(slot + 1);
    return 0;
}

static void gl_cube_upload(vt3_tex t, int face, const void *pixels, int pitch, int size)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used || !g.tex[t - 1].cube) return;
    if (face < 0 || face >= 6 || !pixels || size != g.tex[t - 1].w) return;
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, g.tex[t - 1].id);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    p_glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, size, size, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* D3D scissor (top-left origin, logical pixels) becomes a GL scissor (bottom-left origin).
 * Applied to every operation on the logical buffer; present turns it off for its own blit. */
static void gl_set_scissor(int enable, vt3_rect r)
{
    g.scissor_on = enable != 0;
    g.scissor = r;
}

/* Size and framebuffer of a target: 0 is the logical back buffer, anything else a render texture. */
static void gl_dims_of(vt3_tex t, int *w, int *h, GLuint *fbo)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].rt) {
        *w = g.logical_w;
        *h = g.logical_h;
        *fbo = g.fbo;
        return;
    }
    *w = g.tex[t - 1].w;
    *h = g.tex[t - 1].h;
    *fbo = g.tex[t - 1].fbo;
}

/* Window row of a top-first row y in target t. The back buffer's rows run bottom-up in GL; a
 * render texture keeps the D3D row order, because draws into it are flipped (vt3_flip). */
/* Whether draws into target t are flipped (vt3_flip -1), so its rows keep the D3D order. The
 * back buffer is not. VT3_SHADOW_NOFLIP=1 (debug) leaves half-float targets unflipped too. */
static int gl_target_flipped(vt3_tex t)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].rt) return 0;
    static int noflip16 = -1;
    if (noflip16 < 0) noflip16 = getenv("VT3_SHADOW_NOFLIP") != NULL;
    return !(noflip16 && g.tex[t - 1].fmt == VT3_TEXFMT_RGBA16F);
}

static GLint gl_row_of(vt3_tex t, int y, int h)
{
    return gl_target_flipped(t) ? (GLint)y : (GLint)(h - y);
}

/* GL's lower window row of the top-first rows [y0, y1) of target t: the back buffer runs
 * bottom-up, a render texture keeps the D3D row order (see gl_row_of). */
static GLint gl_rect_low(vt3_tex t, int y0, int y1, int h)
{
    return gl_target_flipped(t) ? (GLint)y0 : (GLint)(h - y1);
}

static void apply_scissor(void)
{
    if (!g.scissor_on) {
        p_glDisable(GL_SCISSOR_TEST);
        return;
    }
    int tw, th;
    GLuint fbo;
    gl_dims_of(g.tgt, &tw, &th, &fbo);
    int x0 = g.scissor.x, y0 = g.scissor.y;
    int x1 = g.scissor.x + g.scissor.w, y1 = g.scissor.y + g.scissor.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x0, gl_rect_low(g.tgt, y0, y1, th), x1 - x0, y1 - y0);
}

/* Binds the current render target (the back buffer, or a texture set by gl_rt_bind). */
static void gl_bind_logical_fbo(void)
{
    int w, h;
    GLuint fbo;
    gl_dims_of(g.tgt, &w, &h, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, w, h);
    apply_scissor();
}

static void gl_begin_frame(void)
{
    gl_bind_logical_fbo();
}

/* A render target: a texture with a framebuffer and a depth-stencil renderbuffer of the same size. */
static int gl_rt_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    if (fmt != VT3_TEXFMT_RGBA16F) fmt = VT3_TEXFMT_A8R8G8B8;
    if (gl_tex_create(out, w, h, fmt) != 0) return -1;
    gl_tex *t = &g.tex[*out - 1];
    t->rt = 1;
    p_glGenFramebuffers(1, &t->fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    p_glGenRenderbuffers(1, &t->depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        VT3_WARN("render target %dx%d incomplete (status 0x%x)", w, h, (unsigned)st);
        gl_tex_destroy(*out);
        *out = 0;
        return -1;
    }
    return 0;
}

/* Draws and clears go to t from now on. 0 or a plain texture: the back buffer. */
static void gl_rt_bind(vt3_tex t)
{
    g.tgt = (t != 0 && t <= VT3_MAX_TEX && g.tex[t - 1].used && g.tex[t - 1].rt) ? t : 0;
}

/* Copies src_r of src onto dst_r of dst, scaling as needed. 0 is the back buffer. The rows are
 * converted to each target's window rows, so a copy from the back buffer into a texture (or back)
 * comes out in the right orientation. */
static void gl_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest)
{
    int sw, sh, dw, dh;
    GLuint sf, df;
    gl_dims_of(src, &sw, &sh, &sf);
    gl_dims_of(dst, &dw, &dh, &df);
    if (!sf || !df || sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0) return;
    GLint sy0 = gl_row_of(src, sr.y, sh), sy1 = gl_row_of(src, sr.y + sr.h, sh);
    GLint dy0 = gl_row_of(dst, dr.y, dh), dy1 = gl_row_of(dst, dr.y + dr.h, dh);
    p_glDisable(GL_SCISSOR_TEST);            /* the scissor applies to draws, not to copies */
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sf);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    p_glBlitFramebuffer(sr.x, sy0, sr.x + sr.w, sy1, dr.x, dy0, dr.x + dr.w, dy1, GL_COLOR_BUFFER_BIT,
                        nearest ? GL_NEAREST : GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static void gl_clear(uint32_t argb)
{
    gl_bind_logical_fbo();
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);   /* D3D clears every channel, whatever the write mask */
    p_glClearColor(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                   (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

static void apply_state(const vt3_rstate *s);

static void gl_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex)
{
    gl_bind_logical_fbo();
    static const vt3_rstate tl_state = { 1, 5, 6, 0, 0, 8, 1, 1, 0, 0, 0, 0xF };   /* alpha blend, no depth, no cull, ADD */
    apply_state(&tl_state);
    p_glBindVertexArray(g.vao_tl);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo_tl);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * VT3_VERTEX_BYTES, v, GL_STREAM_DRAW);

    p_glUseProgram(g.program);
    int tw, th;
    GLuint tfbo;
    gl_dims_of(g.tgt, &tw, &th, &tfbo);
    p_glUniform2f(g.u_logical_size, (GLfloat)tw, (GLfloat)th);
    p_glUniform1f(g.u_flip, gl_target_flipped(g.tgt) ? -1.0f : 1.0f);
    int use_tex = (tex != 0 && tex <= VT3_MAX_TEX && g.tex[tex - 1].used);
    p_glUniform1i(g.u_use_tex, use_tex);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, use_tex ? g.tex[tex - 1].id : g.white);
    p_glUniform1i(g.u_tex, 0);

    p_glDrawArrays(prim == VT3_PRIM_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES, 0, (GLsizei)count);
}

static void gl_present(const vt3_rect *dst, int nearest, int drawable_w, int drawable_h)
{
    /* The scissor belongs to the logical buffer; it must not clip the bars or the blit. */
    p_glDisable(GL_SCISSOR_TEST);
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

/* ---- programmable path ------------------------------------------------ */

/* D3DBLENDOP_*: 1 ADD, 2 SUBTRACT, 3 REVSUBTRACT, 4 MIN, 5 MAX. Zero means the default, ADD. */
static GLenum map_blendop(int d3d)
{
    switch (d3d) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum map_blend(int d3d)
{
    switch (d3d) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    default: return GL_ONE;
    }
}

static GLenum map_cmp(int d3d)
{
    switch (d3d) {
    case 1: return GL_NEVER;
    case 2: return GL_LESS;
    case 3: return GL_EQUAL;
    case 4: return GL_LEQUAL;
    case 5: return GL_GREATER;
    case 6: return GL_NOTEQUAL;
    case 7: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static void apply_state(const vt3_rstate *s)
{
    p_glBlendEquation(map_blendop(s->blend_op));
    if (s->alpha_blend) {
        p_glEnable(GL_BLEND);
        p_glBlendFunc(map_blend(s->src_blend), map_blend(s->dst_blend));
    } else {
        p_glDisable(GL_BLEND);
    }
    if (s->depth_test) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(map_cmp(s->depth_func));
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
    p_glDepthMask(s->depth_write ? GL_TRUE : GL_FALSE);
    p_glColorMask((s->color_write & 1) != 0, (s->color_write & 2) != 0, (s->color_write & 4) != 0,
                  (s->color_write & 8) != 0);
    {   /* the D3D viewport: a sub-rectangle of the target and a depth range; none = the whole target */
        int tw, th;
        GLuint fbo;
        gl_dims_of(g.tgt, &tw, &th, &fbo);
        if (s->vp_w > 0 && s->vp_h > 0) {
            p_glViewport(s->vp_x, gl_rect_low(g.tgt, s->vp_y, s->vp_y + s->vp_h, th), s->vp_w, s->vp_h);
            p_glDepthRange((GLclampd)s->vp_min_z, (GLclampd)s->vp_max_z);
        } else {
            p_glViewport(0, 0, tw, th);
            p_glDepthRange(0.0, 1.0);
        }
    }
    int cull = s->cull;
    if (gl_target_flipped(g.tgt) && (cull == 2 || cull == 3)) cull = 5 - cull;   /* flipped target: CW and CCW swap */
    if (cull == 1) {                          /* D3DCULL_NONE */
        p_glDisable(GL_CULL_FACE);
    } else {
        /* HYPOTHESIS: D3DCULL_CW culls triangles that are clockwise in GL
         * window space, so the front face is counter-clockwise. Verify on a
         * known winding before relying on it. */
        p_glEnable(GL_CULL_FACE);
        p_glCullFace(GL_BACK);
        p_glFrontFace(cull == 2 ? GL_CCW : GL_CW);
    }
}

static gl_prog *prog_get(vt3_prog p)
{
    if (p == 0 || p > VT3_MAX_PROGS || !g.progs[p - 1].used) return NULL;
    return &g.progs[p - 1];
}

static int compile_capture(GLenum type, const char *src, GLuint *out, char *err, size_t errcap)
{
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        p_glGetShaderInfoLog(sh, (GLsizei)errcap, NULL, err);
        p_glDeleteShader(sh);
        return -1;
    }
    *out = sh;
    return 0;
}

static int gl_prog_create(vt3_prog *out, const char *vs_src, const char *ps_src, char *err, size_t errcap)
{
    int slot = -1;
    for (int i = 0; i < VT3_MAX_PROGS; i++) {
        if (!g.progs[i].used) { slot = i; break; }
    }
    if (slot < 0) { snprintf(err, errcap, "program table full"); return -1; }

    GLuint vs = 0, ps = 0;
    if (compile_capture(GL_VERTEX_SHADER, vs_src, &vs, err, errcap) != 0) return -1;
    if (compile_capture(GL_FRAGMENT_SHADER, ps_src, &ps, err, errcap) != 0) {
        p_glDeleteShader(vs);
        return -1;
    }
    GLuint id = p_glCreateProgram();
    p_glAttachShader(id, vs);
    p_glAttachShader(id, ps);
    p_glLinkProgram(id);
    p_glDeleteShader(vs);
    p_glDeleteShader(ps);
    GLint linked = 0;
    p_glGetProgramiv(id, GL_LINK_STATUS, &linked);
    if (!linked) {
        p_glGetProgramInfoLog(id, (GLsizei)errcap, NULL, err);
        p_glDeleteProgram(id);
        return -1;
    }
    gl_prog *p = &g.progs[slot];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->used = 1;
    p->loc_vsc = p_glGetUniformLocation(id, "vsc");
    p->loc_psc = p_glGetUniformLocation(id, "psc");
    for (int i = 0; i < 16; i++) {
        char name[16];
        snprintf(name, sizeof name, "vsb[%d]", i);
        p->loc_vsb[i] = p_glGetUniformLocation(id, name);
        snprintf(name, sizeof name, "psb[%d]", i);
        p->loc_psb[i] = p_glGetUniformLocation(id, name);
    }
    p->loc_at_func = p_glGetUniformLocation(id, "vt3_at_func");
    p->loc_at_ref = p_glGetUniformLocation(id, "vt3_at_ref");
    p->loc_flip = p_glGetUniformLocation(id, "vt3_flip");
    for (int n = 0; n < VT3_MAX_SAMPLERS; n++) {
        char name[8];
        snprintf(name, sizeof name, "s%d", n);
        p->loc_samp[n] = p_glGetUniformLocation(id, name);
    }
    *out = (vt3_prog)(slot + 1);
    return 0;
}

static void gl_prog_destroy(vt3_prog h)
{
    gl_prog *p = prog_get(h);
    if (!p) return;
    p_glDeleteProgram(p->id);
    p->used = 0;
}

static void gl_draw_program(const vt3_draw *d)
{
    gl_prog *p = prog_get(d->prog);
    if (!p || !d->layout) return;
    gl_bind_logical_fbo();
    apply_state(&d->state);

    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)d->vertex_bytes, d->vertices, GL_STREAM_DRAW);

    uint32_t want = 0;
    for (int i = 0; i < d->layout->count && i < VT3_MAX_VATTR; i++) {
        const vt3_vattr *a = &d->layout->attr[i];
        if (a->location < 0 || a->location >= VT3_MAX_VATTR) continue;
        want |= 1u << a->location;
        /* Integer types are converted to float by GL: as values, or normalised to 0..1 or -1..1. */
        GLenum t = GL_FLOAT;
        GLboolean norm = GL_FALSE;
        switch (a->type) {
        case VT3_VT_UBYTE_NORM: t = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
        case VT3_VT_UBYTE:      t = GL_UNSIGNED_BYTE; break;
        case VT3_VT_SHORT:      t = GL_SHORT; break;
        case VT3_VT_SHORT_NORM: t = GL_SHORT; norm = GL_TRUE; break;
        default: break;
        }
        p_glEnableVertexAttribArray((GLuint)a->location);
        p_glVertexAttribPointer((GLuint)a->location, a->components, t, norm, d->layout->stride,
                                (const void *)(intptr_t)a->offset);
    }
    for (int loc = 0; loc < VT3_MAX_VATTR; loc++) {
        if ((g.attr_mask & (1u << loc)) && !(want & (1u << loc))) p_glDisableVertexAttribArray((GLuint)loc);
    }
    g.attr_mask = want;

    p_glUseProgram(p->id);
    if (p->loc_flip >= 0) p_glUniform1f(p->loc_flip, gl_target_flipped(g.tgt) ? -1.0f : 1.0f);
    if (p->loc_at_func >= 0) p_glUniform1i(p->loc_at_func, d->state.alpha_test ? d->state.alpha_func : 0);
    if (p->loc_at_ref >= 0) p_glUniform1f(p->loc_at_ref, (GLfloat)d->state.alpha_ref);
    if (p->loc_vsc >= 0) p_glUniform4fv(p->loc_vsc, VT3_CONST_REGS, d->vsc ? d->vsc : g.zero_consts);
    if (p->loc_psc >= 0) p_glUniform4fv(p->loc_psc, VT3_CONST_REGS, d->psc ? d->psc : g.zero_consts);
    for (int i = 0; i < 16; i++) {
        if (p->loc_vsb[i] >= 0) p_glUniform1i(p->loc_vsb[i], d->vsb[i]);
        if (p->loc_psb[i] >= 0) p_glUniform1i(p->loc_psb[i], d->psb[i]);
    }
    for (int n = 0; n < VT3_MAX_SAMPLERS; n++) {
        if (p->loc_samp[n] < 0) continue;
        vt3_tex t = d->samplers[n];
        /* Both targets are bound on every unit. The program's sampler type reads the one it declares:
         * a cube texture for samplerCube, a 2D texture for sampler2D. */
        GLuint tid2 = g.zero, tidc = g.zero_cube;
        if (t == 0 || (t != VT3_TEX_ZERO && (t > VT3_MAX_TEX || !g.tex[t - 1].used))) {
            tid2 = g.white;                       /* no texture bound */
        } else if (t != VT3_TEX_ZERO) {
            if (g.tex[t - 1].cube) tidc = g.tex[t - 1].id;
            else tid2 = g.tex[t - 1].id;
        }
        p_glActiveTexture(GL_TEXTURE0 + (GLenum)n);
        p_glBindTexture(GL_TEXTURE_2D, tid2);
        p_glBindTexture(GL_TEXTURE_CUBE_MAP, tidc);
        p_glUniform1i(p->loc_samp[n], n);
    }
    p_glActiveTexture(GL_TEXTURE0);

    if (d->indices) {
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)d->count * 2, d->indices, GL_STREAM_DRAW);
        p_glDrawElements(d->prim == VT3_PRIM_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES,
                         (GLsizei)d->count, GL_UNSIGNED_SHORT, NULL);
    } else {
        p_glDrawArrays(d->prim == VT3_PRIM_TRIANGLESTRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES, 0, (GLsizei)d->count);
    }
}

static void gl_readback(uint8_t *rgba)
{
    int w = g.logical_w, h = g.logical_h;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);   /* bottom row first */
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    size_t row = (size_t)w * 4;
    uint8_t *tmp = malloc(row);
    if (!tmp) return;
    for (int y = 0; y < h / 2; y++) {
        uint8_t *a = rgba + (size_t)y * row, *b = rgba + (size_t)(h - 1 - y) * row;
        memcpy(tmp, a, row);
        memcpy(a, b, row);
        memcpy(b, tmp, row);
    }
    free(tmp);
}

static void gl_clear_depth(float depth)
{
    gl_bind_logical_fbo();
    p_glDepthMask(GL_TRUE);
    p_glClearDepth((GLclampd)depth);
    p_glClear(GL_DEPTH_BUFFER_BIT);
}

/* The backend the shim uses. The platform decides (Vulkan by default, see wants_vulkan in
 * platform_sdl.c), because the window kind has to match the backend. */
const vt3_gfx_ops *vt3_gfx_default_ops(void)
{
    return vt3_platform_is_vulkan() ? &vt3_gfx_vk : &vt3_gfx_gl33;
}

const vt3_gfx_ops vt3_gfx_gl33 = {
    .name = "OpenGL 3.3 core",
    .init = gl_init,
    .shutdown = gl_shutdown,
    .tex_create = gl_tex_create,
    .tex_upload = gl_tex_upload,
    .tex_destroy = gl_tex_destroy,
    .rt_create = gl_rt_create,
    .rt_bind = gl_rt_bind,
    .blit = gl_blit,
    .cube_create = gl_cube_create,
    .cube_upload = gl_cube_upload,
    .begin_frame = gl_begin_frame,
    .clear = gl_clear,
    .draw = gl_draw,
    .present = gl_present,
    .prog_create = gl_prog_create,
    .prog_destroy = gl_prog_destroy,
    .draw_program = gl_draw_program,
    .readback = gl_readback,
    .clear_depth = gl_clear_depth,
    .set_scissor = gl_set_scissor,
};
