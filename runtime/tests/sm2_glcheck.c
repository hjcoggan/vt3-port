/*
 * Driver check for the SM2 translator: compiles every .bin shader in a directory
 * (ps_2_0 / vs_2_0 by file suffix) as GLSL 330 core in a real GL 3.3 context,
 * and links sample vertex/pixel pairs. Run as `vt3rt --sm2-check DIR`.
 *
 * The directory is game-derived data. It is never inside the repo.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_sm2.h"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint compile_stage(GLenum type, const char *src, char *log, size_t logcap)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, (GLsizei)logcap, NULL, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int load_tokens(const char *path, uint32_t **tok, size_t *nt)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    *nt = (size_t)n / 4;
    *tok = malloc(*nt * sizeof(uint32_t));
    for (size_t i = 0; i < *nt; i++) {
        const unsigned char *p = buf + 4 * i;
        (*tok)[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    free(buf);
    return 0;
}

typedef struct entry {
    char name[96];
    vt3_sm2_shader sh;
    GLuint shader;
} entry;

int vt3_sm2_glcheck_dir(const char *dir)
{
    vt3_config cfg;
    vt3_config_defaults(&cfg);
    cfg.audio = 0;
    if (vt3_platform_init(&cfg) != 0) return 1;

    entry *vs = NULL, *ps = NULL;
    size_t nvs = 0, nps = 0, cap_vs = 0, cap_ps = 0;
    int translate_fail = 0, compile_fail = 0;
    char log[2048];

    DIR *d = opendir(dir);
    if (!d) {
        VT3_ERROR("sm2-check: cannot open %s", dir);
        vt3_platform_shutdown();
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        int is_vs = strstr(n, ".vs_2_0.bin") != NULL;
        int is_ps = strstr(n, ".ps_2_0.bin") != NULL;
        if (!is_vs && !is_ps) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        uint32_t *tok;
        size_t nt;
        if (load_tokens(path, &tok, &nt) != 0) continue;
        entry e;
        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", n);
        if (vt3_sm2_translate(tok, nt, &e.sh) != 0) {
            translate_fail++;
            VT3_ERROR("translate %s: %s", n, e.sh.error);
            free(tok);
            continue;
        }
        free(tok);
        const char *src = e.sh.glsl;
        GLuint s = compile_stage(is_vs ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, src, log, sizeof log);
        if (!s) {
            compile_fail++;
            if (compile_fail <= 5) VT3_ERROR("compile %s failed:\n%s\n%s", n, log, src);
            vt3_sm2_free(&e.sh);
            continue;
        }
        glDeleteShader(s);
        vt3_sm2_free(&e.sh);
        if (is_vs) {
            if (nvs == cap_vs) { cap_vs = cap_vs ? cap_vs * 2 : 64; vs = realloc(vs, cap_vs * sizeof *vs); }
            memset(&vs[nvs], 0, sizeof vs[nvs]);
            strcpy(vs[nvs].name, n);
            nvs++;
        } else {
            if (nps == cap_ps) { cap_ps = cap_ps ? cap_ps * 2 : 64; ps = realloc(ps, cap_ps * sizeof *ps); }
            memset(&ps[nps], 0, sizeof ps[nps]);
            strcpy(ps[nps].name, n);
            nps++;
        }
    }
    closedir(d);

    /* Link sample pairs: every VS against the first 40 PS, and the first 40 VS against every PS. */
    int link_fail = 0, link_ok = 0;
    for (size_t i = 0; i < nvs; i++) {
        for (size_t j = 0; j < nps; j++) {
            if (!(i < 40 || j < 40)) continue;
            char vpath[1024], ppath[1024];
            uint32_t *vt, *pt; size_t vn, pn;
            snprintf(vpath, sizeof vpath, "%s/%s", dir, vs[i].name);
            snprintf(ppath, sizeof ppath, "%s/%s", dir, ps[j].name);
            if (load_tokens(vpath, &vt, &vn) != 0 || load_tokens(ppath, &pt, &pn) != 0) continue;
            vt3_sm2_shader vsh, psh;
            int okv = vt3_sm2_translate(vt, vn, &vsh) == 0;
            int okp = vt3_sm2_translate(pt, pn, &psh) == 0;
            free(vt);
            free(pt);
            if (!okv || !okp) { vt3_sm2_free(&vsh); vt3_sm2_free(&psh); continue; }
            GLuint vsid = compile_stage(GL_VERTEX_SHADER, vsh.glsl, log, sizeof log);
            GLuint psid = compile_stage(GL_FRAGMENT_SHADER, psh.glsl, log, sizeof log);
            if (vsid && psid) {
                GLuint prog = glCreateProgram();
                glAttachShader(prog, vsid);
                glAttachShader(prog, psid);
                glLinkProgram(prog);
                GLint linked = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &linked);
                if (linked) link_ok++;
                else {
                    link_fail++;
                    if (link_fail <= 3) {
                        glGetProgramInfoLog(prog, sizeof log, NULL, log);
                        VT3_ERROR("link %s + %s: %s", vs[i].name, ps[j].name, log);
                    }
                }
                glDeleteProgram(prog);
            }
            if (vsid) glDeleteShader(vsid);
            if (psid) glDeleteShader(psid);
            vt3_sm2_free(&vsh);
            vt3_sm2_free(&psh);
        }
    }

    printf("sm2-check: vs %zu ps %zu | translate failures %d | compile failures %d | links ok %d fail %d\n",
           nvs, nps, translate_fail, compile_fail, link_ok, link_fail);
    free(vs);
    free(ps);
    vt3_platform_shutdown();
    return (translate_fail || compile_fail || link_fail) ? 1 : 0;
}

/* ---- end-to-end draw test with hand-encoded SM2 bytecode ----------------- */

#define OPTOK(op, len)   ((uint32_t)(op) | ((uint32_t)(len) << 24))
#define DSTTOK(type, num, mask) (0x80000000u | (((uint32_t)(type) & 7u) << 28) | \
                                 (((uint32_t)(type) & 0x18u) << 8) | ((uint32_t)(mask) << 16) | (uint32_t)(num))
#define SRCTOK(type, num, swz)  (0x80000000u | (((uint32_t)(type) & 7u) << 28) | \
                                 (((uint32_t)(type) & 0x18u) << 8) | ((uint32_t)(swz) << 16) | (uint32_t)(num))
#define SWZ_XYZW 0xE4u
#define MASK_ALL 0xFu

/* vs_2_0: position passes through, colour goes to oD0. */
static const uint32_t k_vs[] = {
    0xFFFE0200u,
    OPTOK(31, 2), 0x00000000u, DSTTOK(1, 0, MASK_ALL),          /* dcl_position v0 */
    OPTOK(31, 2), 0x0000000Au, DSTTOK(1, 1, MASK_ALL),          /* dcl_color v1 */
    OPTOK(1, 2), DSTTOK(4, 0, MASK_ALL), SRCTOK(1, 0, SWZ_XYZW), /* mov oPos, v0 */
    OPTOK(1, 2), DSTTOK(5, 0, MASK_ALL), SRCTOK(1, 1, SWZ_XYZW), /* mov oD0, v1 */
    0x0000FFFFu
};

/* ps_2_0: output is the constant c0 = (0.25, 0.5, 0.75, 1). */
static const uint32_t k_ps_const[] = {
    0xFFFF0200u,
    OPTOK(81, 5), DSTTOK(2, 0, MASK_ALL), 0x3E800000u, 0x3F000000u, 0x3F400000u, 0x3F800000u,  /* 0.25 0.5 0.75 1.0 */
    OPTOK(31, 2), 0x0000000Au, DSTTOK(1, 0, MASK_ALL),          /* dcl_color v0 (unused) */
    OPTOK(1, 2), DSTTOK(8, 0, MASK_ALL), SRCTOK(2, 0, SWZ_XYZW), /* mov oC0, c0 */
    0x0000FFFFu
};

/* ps_2_0: output is the interpolated colour v0. */
static const uint32_t k_ps_color[] = {
    0xFFFF0200u,
    OPTOK(31, 2), 0x0000000Au, DSTTOK(1, 0, MASK_ALL),          /* dcl_color v0 */
    OPTOK(1, 2), DSTTOK(8, 0, MASK_ALL), SRCTOK(1, 0, SWZ_XYZW), /* mov oC0, v0 */
    0x0000FFFFu
};

typedef struct tvert {
    float x, y, z, w;
    uint8_t b, g, r, a;   /* D3DCOLOR in memory order */
} tvert;

static int check_pixel(const uint8_t *img, int w, int x, int y, const char *what,
                       int er, int eg, int eb, int ea)
{
    const uint8_t *p = img + ((size_t)y * (size_t)w + (size_t)x) * 4;
    int ok = abs(p[0] - er) <= 2 && abs(p[1] - eg) <= 2 && abs(p[2] - eb) <= 2 && abs(p[3] - ea) <= 2;
    printf("  %s at (%d,%d): rgba=(%d,%d,%d,%d) expect (%d,%d,%d,%d) %s\n", what, x, y,
           p[0], p[1], p[2], p[3], er, eg, eb, ea, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static void fill_consts(float *dst, const vt3_sm2_shader *sh)
{
    memset(dst, 0, sizeof(float) * 256 * 4);
    for (int r = 0; r < VT3_SM2_MAX_CONST; r++) {
        if (!sh->cdef_set[r]) continue;
        memcpy(dst + r * 4, sh->cdef[r], sizeof(float) * 4);
    }
}

int vt3_sm2_gl_draw_test(void)
{
    vt3_config cfg;
    vt3_config_defaults(&cfg);
    cfg.audio = 0;
    if (vt3_platform_init(&cfg) != 0) return 1;
    vt3_gfx_config gc = { cfg.logical_w, cfg.logical_h, cfg.aspect_w, cfg.aspect_h, 0 };
    if (vt3_gfx_init(&vt3_gfx_gl33, &gc) != 0) { vt3_platform_shutdown(); return 1; }

    int fails = 0;
    vt3_sm2_shader vs, ps_c, ps_v;
    if (vt3_sm2_translate(k_vs, sizeof k_vs / 4, &vs) != 0) { printf("vs translate: %s\n", vs.error); fails++; }
    if (vt3_sm2_translate(k_ps_const, sizeof k_ps_const / 4, &ps_c) != 0) { printf("ps translate: %s\n", ps_c.error); fails++; }
    if (vt3_sm2_translate(k_ps_color, sizeof k_ps_color / 4, &ps_v) != 0) { printf("ps translate: %s\n", ps_v.error); fails++; }
    if (fails) return fails;

    static float psc[256 * 4];
    fill_consts(psc, &ps_c);

    /* A triangle that covers the whole logical target. */
    static const tvert verts[3] = {
        { -1.0f, -1.0f, 0.5f, 1.0f, 0, 255, 0, 255 },
        {  3.0f, -1.0f, 0.5f, 1.0f, 0, 255, 0, 255 },
        { -1.0f,  3.0f, 0.5f, 1.0f, 0, 255, 0, 255 },
    };
    static const uint16_t idx[3] = { 0, 1, 2 };
    vt3_vlayout lay;
    memset(&lay, 0, sizeof lay);
    lay.stride = (int)sizeof(tvert);
    lay.count = 2;
    lay.attr[0].location = 0;  lay.attr[0].components = 4; lay.attr[0].type = VT3_VT_FLOAT;      lay.attr[0].offset = 0;
    lay.attr[1].location = 15; lay.attr[1].components = 4; lay.attr[1].type = VT3_VT_UBYTE_NORM; lay.attr[1].offset = 16;

    char err[1024];
    vt3_prog p_const = 0, p_color = 0;
    if (vt3_gfx_prog_create(&p_const, vs.glsl, ps_c.glsl, err, sizeof err) != 0) {
        printf("program (const) failed: %s\n", err); fails++;
    }
    if (vt3_gfx_prog_create(&p_color, vs.glsl, ps_v.glsl, err, sizeof err) != 0) {
        printf("program (colour) failed: %s\n", err); fails++;
    }

    size_t npix = (size_t)cfg.logical_w * (size_t)cfg.logical_h * 4;
    uint8_t *img = malloc(npix);
    if (!img) { fails++; }
    if (img && p_const && p_color) {
        vt3_draw d;
        memset(&d, 0, sizeof d);
        d.layout = &lay;
        d.vertices = verts;
        d.vertex_bytes = sizeof verts;
        d.prim = VT3_PRIM_TRIANGLELIST;
        d.state.cull = 1;       /* D3DCULL_NONE */
        d.state.depth_func = 8; /* D3DCMP_ALWAYS, depth test off */

        /* Pass 1: non-indexed, constant colour from a def. */
        vt3_gfx_begin_frame();
        vt3_gfx_clear(0xFF000000u);
        d.prog = p_const;
        d.psc = psc;
        d.count = 3;
        vt3_gfx_draw_program(&d);
        vt3_gfx_readback(img);
        printf("pass 1 (def constant):\n");
        fails += check_pixel(img, cfg.logical_w, 320, 240, "centre", 64, 128, 191, 255);
        fails += check_pixel(img, cfg.logical_w, 2, 2, "corner", 64, 128, 191, 255);

        /* Pass 2: indexed, vertex colour through the varying. */
        vt3_gfx_begin_frame();
        vt3_gfx_clear(0xFF000000u);
        d.prog = p_color;
        d.psc = NULL;
        d.indices = idx;
        d.count = 3;
        vt3_gfx_draw_program(&d);
        vt3_gfx_readback(img);
        printf("pass 2 (vertex colour, indexed):\n");
        fails += check_pixel(img, cfg.logical_w, 320, 240, "centre", 0, 255, 0, 255);
    }

    if (p_const) vt3_gfx_prog_destroy(p_const);
    if (p_color) vt3_gfx_prog_destroy(p_color);
    free(img);
    vt3_sm2_free(&vs);
    vt3_sm2_free(&ps_c);
    vt3_sm2_free(&ps_v);
    vt3_gfx_shutdown();
    vt3_platform_shutdown();
    printf("sm2 draw test: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}

