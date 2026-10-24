/*
 * Draw calls. A D3D draw becomes one of two renderer calls:
 *   - fixed function (no vertex shader, FVF XYZRHW|DIFFUSE|TEX1): vt3_gfx_draw, the
 *     pre-transformed path the renderer already has;
 *   - shader (a translated vertex shader, with the pixel shader translated or a
 *     fixed-function stage 0 stand-in): vt3_gfx_draw_program.
 * Anything else is skipped with one log line per reason. Skipped draws still return
 * D3D_OK, so the game keeps running.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_builtin.h"
#include "vt3/vt3_log.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PRIM_TRIANGLELIST 4u
#define PRIM_TRIANGLESTRIP 5u
#define PRIM_TRIANGLEFAN 6u
#define FVF_XYZ 0x002u
#define FVF_XYZRHW 0x004u
#define FVF_NORMAL 0x010u
#define FVF_PSIZE 0x020u
#define FVF_DIFFUSE 0x040u
#define FVF_SPECULAR 0x080u
#define FVF_TEXCOUNT(f) (((f) >> 8) & 0xFu)
#define FVF_TL 0x144u                  /* XYZRHW | DIFFUSE | TEX1 */

/* D3DDECLTYPE and D3DDECLUSAGE values used here. */
#define DT_FLOAT1 0u
#define DT_FLOAT4 3u
#define DT_D3DCOLOR 4u
#define DT_UBYTE4 5u
#define DT_SHORT2 6u
#define DT_SHORT4 7u
#define DT_UBYTE4N 8u
#define DT_SHORT2N 9u
#define DT_SHORT4N 10u
#define DU_POSITION 0u
#define DU_NORMAL 3u
#define DU_PSIZE 4u
#define DU_TEXCOORD 5u
#define DU_COLOR 10u

static int warned_once(int *flag)
{
    if (*flag) return 1;
    *flag = 1;
    return 0;
}

/* Draw calls are serialized by the guest lock and both backends copy submitted bytes before
 * returning. Reuse one bounded-by-high-water scratch buffer instead of allocating once per
 * draw. Keeping it at file scope is safe because D3D9 draw submission is serialized here. */
static uint8_t *g_vertex_scratch;
static size_t g_vertex_scratch_cap;

static uint8_t *vertex_scratch_reserve(size_t need)
{
    if (need <= g_vertex_scratch_cap) return g_vertex_scratch;
    uint8_t *p = realloc(g_vertex_scratch, need);
    if (!p) return NULL;
    g_vertex_scratch = p;
    g_vertex_scratch_cap = need;
    return p;
}

/* The index list D3D hands us is converted to the 16-bit, range-relative form the renderer
 * takes, per draw. That list used to be malloc'd and freed every draw and the conversion read
 * the 32-bit indices a byte at a time; on the Retroid the loop was ~4% of in-match CPU. Reuse
 * one growing buffer (draws are serialized by the guest lock) and let the compiler load whole
 * 32-bit indices. */
static uint16_t *g_index_scratch;
static size_t g_index_scratch_n;

static uint16_t *index_scratch_reserve(size_t n)
{
    if (n <= g_index_scratch_n) return g_index_scratch;
    uint16_t *p = realloc(g_index_scratch, n * sizeof *p);
    if (!p) return NULL;
    g_index_scratch = p;
    g_index_scratch_n = n;
    return p;
}

/* A triangle fan expands to a triangle list: three indices per triangle, at most as many as
 * there are fan indices. Same lifetime as the list above. */
static uint16_t *g_fan_scratch;
static size_t g_fan_scratch_n;

static uint16_t *fan_scratch_reserve(size_t n)
{
    if (n <= g_fan_scratch_n) return g_fan_scratch;
    uint16_t *p = realloc(g_fan_scratch, n * sizeof *p);
    if (!p) return NULL;
    g_fan_scratch = p;
    g_fan_scratch_n = n;
    return p;
}

static uint32_t prim_vertex_count(uint32_t prim, uint32_t count)
{
    if (prim == PRIM_TRIANGLELIST) return 3u * count;
    if (prim == PRIM_TRIANGLESTRIP || prim == PRIM_TRIANGLEFAN) return count + 2u;
    return 0;
}

/* Builds a declaration from an FVF code, in the order the SDK lays the fields out. */
static void decl_from_fvf(d3d9_decl *dc, uint32_t fvf)
{
    memset(dc, 0, sizeof *dc);
    uint32_t n = 0, off = 0;
    #define ADD(stream_, type_, usage_, index_, size_) do { \
        dc->el[n][0] = (uint8_t)(stream_); dc->el[n][1] = 0; \
        dc->el[n][2] = (uint8_t)(off); dc->el[n][3] = (uint8_t)(off >> 8); \
        dc->el[n][4] = (uint8_t)(type_); dc->el[n][5] = 0; \
        dc->el[n][6] = (uint8_t)(usage_); dc->el[n][7] = (uint8_t)(index_); \
        n++; off += (size_); } while (0)
    if (fvf & FVF_XYZRHW) ADD(0, DT_FLOAT4, DU_POSITION, 0, 16);
    else if (fvf & FVF_XYZ) ADD(0, 2u, DU_POSITION, 0, 12);
    if (fvf & FVF_NORMAL) ADD(0, 2u, DU_NORMAL, 0, 12);
    if (fvf & FVF_PSIZE) ADD(0, DT_FLOAT1, DU_PSIZE, 0, 4);
    if (fvf & FVF_DIFFUSE) ADD(0, DT_D3DCOLOR, DU_COLOR, 0, 4);
    if (fvf & FVF_SPECULAR) ADD(0, DT_D3DCOLOR, DU_COLOR, 1, 4);
    for (uint32_t t = 0; t < FVF_TEXCOUNT(fvf) && t < 8; t++) ADD(0, 1u, DU_TEXCOORD, t, 8);
    #undef ADD
    dc->n = n;
    dc->stride[0] = off;
}

/* The element that layout_from_decl last refused, for the warning. */
static uint32_t g_bad_type, g_bad_usage;

/* Fills a renderer layout from a declaration. Returns 0, or -1 for a type the renderer lacks. */
static int layout_from_decl(const d3d9_decl *dc, vt3_vlayout *lay, uint32_t stride)
{
    memset(lay, 0, sizeof *lay);
    lay->stride = (int)stride;
    int k = 0;
    for (uint32_t i = 0; i < dc->n; i++) {
        const uint8_t *e = dc->el[i];
        uint32_t stream = e[0] | (e[1] << 8);
        if (stream != 0) continue;
        uint32_t offset = e[2] | (e[3] << 8), type = e[4], usage = e[6], index = e[7];
        int loc = vt3_sm2_attr_location(usage, index);
        if (loc < 0) continue;                 /* no shader input for it */
        if (k >= VT3_MAX_VATTR) return -1;
        vt3_vattr *a = &lay->attr[k++];
        a->location = loc;
        a->offset = (int)offset;
        if (type <= DT_FLOAT4) {
            a->components = (int)type + 1;
            a->type = VT3_VT_FLOAT;
        } else if (type == DT_D3DCOLOR) {
            a->components = 4;
            a->type = VT3_VT_UBYTE_NORM_BGRA;   /* D3DCOLOR is B,G,R,A in memory: the GPU reorders it */
        } else if (type == DT_UBYTE4N) {
            a->components = 4;
            a->type = VT3_VT_UBYTE_NORM;
        } else if (type == DT_UBYTE4) {
            a->components = 4;
            a->type = VT3_VT_UBYTE;
        } else if (type == DT_SHORT2 || type == DT_SHORT2N) {
            a->components = 2;
            a->type = type == DT_SHORT2 ? VT3_VT_SHORT : VT3_VT_SHORT_NORM;
        } else if (type == DT_SHORT4 || type == DT_SHORT4N) {
            a->components = 4;
            a->type = type == DT_SHORT4 ? VT3_VT_SHORT : VT3_VT_SHORT_NORM;
        } else {
            g_bad_type = type;
            g_bad_usage = usage;
            return -1;
        }
        /* VT3_DROP_OOB_ATTR=1 (experiment, off by default): leave out an element that lies past the end of the
         * vertex. The planar-shadow blur quad has 20-byte vertices under the HUD's declaration, which has a float4
         * at offset 20, so binding it fetches beyond the buffer. Dropping it removes the scrambled shadow streaks
         * but the shadows then vanish altogether (the blurred target stays empty), so the cause is not settled:
         * see docs/HANDOFF.md, open issue 1. */
        static int drop_oob = -1;
        if (drop_oob < 0) drop_oob = getenv("VT3_DROP_OOB_ATTR") != NULL;
        int esize = a->components * (a->type == VT3_VT_FLOAT ? 4 : (a->type == VT3_VT_SHORT || a->type == VT3_VT_SHORT_NORM) ? 2 : 1);
        if (drop_oob && (uint32_t)a->offset + (uint32_t)esize > stride) k--;
    }
    lay->count = k;
    return 0;
}

/* ---- programs ---------------------------------------------------------------- */

#define PROG_CACHE 512
typedef struct prog_entry {
    int used;
    uint32_t vs, ps;
    vt3_prog prog;                     /* 0 when linking failed (remembered, not retried) */
} prog_entry;
static prog_entry g_progs[PROG_CACHE];
static int g_warn_ff_ps, g_warn_no_prog, g_warn_layout, g_warn_idx;

static const char *k_ff_ps =
    "#version 330 core\n"
    "in vec4 vo_D0;\n"
    "in vec4 vo_T0;\n"
    "uniform sampler2D s0;\n"
    "out vec4 oC0;\n"
    "void main() { oC0 = vo_D0 * texture(s0, vo_T0.xy); }\n";

static vt3_prog program_for(uint32_t vs_obj, uint32_t ps_obj, const d3d9_shader *vs, const d3d9_shader *ps)
{
    static int last = -1;   /* most recent hit: a draw normally repeats the previous program */
    if (last >= 0 && g_progs[last].used && g_progs[last].vs == vs_obj && g_progs[last].ps == ps_obj)
        return g_progs[last].prog;
    int free_slot = -1;
    for (int i = 0; i < PROG_CACHE; i++) {
        if (!g_progs[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (g_progs[i].vs == vs_obj && g_progs[i].ps == ps_obj) { last = i; return g_progs[i].prog; }
    }
    if (free_slot < 0) return 0;
    char err[1024] = "";
    vt3_prog p = 0;
    const char *ps_src = ps ? ps->sm.glsl : (vt3_gfx_glsl_flags() ? vt3_builtin_ff_fs_vk : k_ff_ps);
    if (vt3_gfx_prog_create(&p, vs->sm.glsl, ps_src, err, sizeof err) != 0) {
        VT3_WARN("program: link failed (vs 0x%x, ps 0x%x): %s", vs_obj, ps_obj, err);
        p = 0;
    }
    g_progs[free_slot].used = 1;
    g_progs[free_slot].vs = vs_obj;
    g_progs[free_slot].ps = ps_obj;
    g_progs[free_slot].prog = p;
    last = free_slot;
    return p;
}

/* ---- the draw ---------------------------------------------------------------- */

static void fill_rstate(const d3d9_device *d, vt3_rstate *s)
{
    memset(s, 0, sizeof *s);
    s->alpha_blend = d->rs[27] != 0;
    s->src_blend = d->rs[19] ? (int)d->rs[19] : 2;
    s->dst_blend = d->rs[20] ? (int)d->rs[20] : 1;
    static int no_ztest = -1;   /* debug: VT3_NO_ZTEST=1 ignores the depth test */
    if (no_ztest < 0) no_ztest = getenv("VT3_NO_ZTEST") != NULL;
    s->depth_test = d->rs[7] != 0 && !no_ztest;
    s->depth_write = d->rs[14] != 0;
    s->depth_func = d->rs[23] ? (int)d->rs[23] : 4;
    s->cull = d->rs[22] ? (int)d->rs[22] : 3;
    s->blend_op = d->rs[171] ? (int)d->rs[171] : 1;   /* D3DRS_BLENDOP, default ADD */
    s->alpha_test = d->rs[15] != 0;                  /* D3DRS_ALPHATESTENABLE */
    s->alpha_func = d->rs[25] ? (int)d->rs[25] : 8; /* D3DRS_ALPHAFUNC, default ALWAYS */
    s->alpha_ref = (int)(d->rs[24] & 0xFFu);         /* D3DRS_ALPHAREF */
    s->color_write = (int)(d->rs[168] & 0xFu);       /* D3DRS_COLORWRITEENABLE */
    if (d->viewport[2] && d->viewport[3]) {          /* SetViewport; SetRenderTarget resets it */
        union { uint32_t u; float f; } mn, mx;
        mn.u = d->viewport[4];
        mx.u = d->viewport[5];
        s->vp_x = (int)d->viewport[0];
        s->vp_y = (int)d->viewport[1];
        s->vp_w = (int)d->viewport[2];
        s->vp_h = (int)d->viewport[3];
        s->vp_min_z = mn.f;
        s->vp_max_z = mx.f;
    }
}

/* The renderer binds 2D textures only. A cube texture on a sampler gets the zero texel, and the
 * first such case is logged with its sampler number. */
static void fill_textures(const d3d9_device *d, vt3_tex out[VT3_MAX_SAMPLERS])
{
    for (int s = 0; s < VT3_MAX_SAMPLERS; s++) {
        out[s] = 0;
        d3d9_texture *t = d3d9_texture_of(d->textures[s]);
        if (!t) continue;
        if (t->faces == 6) {                 /* a cube texture that failed to create reads as zero */
            out[s] = t->gl ? t->gl : VT3_TEX_ZERO;
            continue;
        }
        out[s] = t->gl;
    }
}

/* FNV-1a over a shader's bytecode. Handles change from run to run (allocation order follows the
 * match being loaded), so traces name a shader by this hash. It matches the same hash taken over
 * a dumped shader's .bin file. */
static uint32_t shader_hash(const d3d9_shader *s)
{
    uint32_t h = 2166136261u;
    if (!s || !s->tok) return 0;
    const uint8_t *p = (const uint8_t *)s->tok;
    for (uint32_t i = 0; i < s->ntok * 4u; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* VT3_TRACE_HASH=<hex>: the traces below keep only draws whose vertex shader has that hash. */
static int trace_hash_match(const d3d9_shader *vs)
{
    static int set = -1;
    static uint32_t want;
    if (set < 0) {
        const char *e = getenv("VT3_TRACE_HASH");
        set = e != NULL;
        want = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
    }
    return !set || (vs && shader_hash(vs) == want);
}

/* VT3_TRACE_DRAW=N logs the first N draws with at least VT3_TRACE_MIN primitives (default 0): the
 * shaders, the first constants of each bank, and each bound sampler's texture. Sprites are small,
 * so a minimum keeps the log on the 3D draws. It is for finding why a draw comes out dark. */
static void trace_draw(const d3d9_device *d, const d3d9_draw_args *a, uint32_t nverts)
{
    static int limit = -1, min_prims, count;
    static double after_s, first_s = -1.0;
    if (limit < 0) {
        const char *e = getenv("VT3_TRACE_DRAW");
        const char *m = getenv("VT3_TRACE_MIN");
        const char *t = getenv("VT3_TRACE_AFTER");
        limit = e ? atoi(e) : 0;
        min_prims = m ? atoi(m) : 0;
        after_s = t ? atof(t) : 0.0;
    }
    if (count >= limit) return;
    struct timespec ts;   /* VT3_TRACE_AFTER counts from the first draw of the run */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now_s = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    if (first_s < 0.0) first_s = now_s;
    if (now_s - first_s < after_s || a->prim_count < (uint32_t)min_prims) return;
    {
        d3d9_host *fh = d3d9_host_of(d->vs, D3D9_KIND_VSHADER);
        if (!trace_hash_match(fh ? fh->data : NULL)) return;
    }
    count++;
    d3d9_host *trace_vh = d3d9_host_of(d->vs, D3D9_KIND_VSHADER);
    int trace_skinned = trace_vh && trace_vh->data && ((d3d9_shader *)trace_vh->data)->skinned;
    VT3_INFO("draw %d: prim %u count %u verts %u vs 0x%x ps 0x%x rt 0x%x depth 0x%x skinned %d", count, a->prim,
             a->prim_count, nverts, d->vs, d->ps, d->rt[0], d->depth, trace_skinned);
    {
        d3d9_host *hp = d->ps ? d3d9_host_of(d->ps, D3D9_KIND_PSHADER) : NULL;
        VT3_INFO("  hash vs %08x ps %08x", trace_vh ? shader_hash(trace_vh->data) : 0u,
                 hp ? shader_hash(hp->data) : 0u);
    }
    static int trace_light = -1;
    if (trace_light < 0) trace_light = getenv("VT3_TRACE_LIGHT") != NULL;
    if (trace_skinned && trace_light)   /* the light matrix the scene and shadow passes read */
        VT3_INFO("  mvp rt 0x%x c0 %g %g %g %g | c1 %g %g %g %g | c2 %g %g %g %g | c3 %g %g %g %g", (unsigned)d->rt[0],
                 d->vsc[0][0], d->vsc[0][1], d->vsc[0][2], d->vsc[0][3], d->vsc[1][0], d->vsc[1][1], d->vsc[1][2], d->vsc[1][3],
                 d->vsc[2][0], d->vsc[2][1], d->vsc[2][2], d->vsc[2][3], d->vsc[3][0], d->vsc[3][1], d->vsc[3][2], d->vsc[3][3]);
    if (trace_skinned && trace_light)
        VT3_INFO("  light rt 0x%x c248 %g %g %g %g | c249 %g %g %g %g | c250 %g %g %g %g | c251 %g %g %g %g", (unsigned)d->rt[0],
                 d->vsc[248][0], d->vsc[248][1], d->vsc[248][2], d->vsc[248][3],
                 d->vsc[249][0], d->vsc[249][1], d->vsc[249][2], d->vsc[249][3],
                 d->vsc[250][0], d->vsc[250][1], d->vsc[250][2], d->vsc[250][3],
                 d->vsc[251][0], d->vsc[251][1], d->vsc[251][2], d->vsc[251][3]);
    VT3_INFO("  rs blend %u src %u dst %u atest %u func %u ref %u zenable %u zwrite %u cull %u",
             (unsigned)d->rs[27], (unsigned)d->rs[19], (unsigned)d->rs[20], (unsigned)d->rs[15],
             (unsigned)d->rs[25], (unsigned)(d->rs[24] & 0xFF), (unsigned)d->rs[7], (unsigned)d->rs[14],
             (unsigned)d->rs[22]);
    static int trace_psc = -1;
    if (trace_psc < 0) trace_psc = getenv("VT3_TRACE_PSC") != NULL;
    if (trace_skinned && trace_psc) {   /* the first pixel constants of skinned draws */
        for (int r = 0; r < 16; r += 2)
            VT3_INFO("  psc c%-2d %9.4f %9.4f %9.4f %9.4f | c%-2d %9.4f %9.4f %9.4f %9.4f", r, d->psc[r][0],
                     d->psc[r][1], d->psc[r][2], d->psc[r][3], r + 1, d->psc[r + 1][0], d->psc[r + 1][1],
                     d->psc[r + 1][2], d->psc[r + 1][3]);
    }
    static int trace_vsc = -1;
    if (trace_vsc < 0) trace_vsc = getenv("VT3_TRACE_VSC") != NULL;
    if (trace_skinned && trace_vsc) {   /* the whole vertex constant block of skinned draws */
        for (int r = 147; r < 171; r += 3)
            VT3_INFO("  vsc c%-2d %9.4f %9.4f %9.4f %9.4f | c%-2d %9.4f %9.4f %9.4f %9.4f | c%-2d %9.4f %9.4f %9.4f %9.4f",
                     r, d->vsc[r][0], d->vsc[r][1], d->vsc[r][2], d->vsc[r][3],
                     r + 1, d->vsc[r + 1][0], d->vsc[r + 1][1], d->vsc[r + 1][2], d->vsc[r + 1][3],
                     r + 2, d->vsc[r + 2][0], d->vsc[r + 2][1], d->vsc[r + 2][2], d->vsc[r + 2][3]);
    }
    VT3_INFO("  psc c0 %g %g %g %g | c1 %g %g %g %g | c2 %g %g %g %g | c3 %g %g %g %g",
             d->psc[0][0], d->psc[0][1], d->psc[0][2], d->psc[0][3],
             d->psc[1][0], d->psc[1][1], d->psc[1][2], d->psc[1][3],
             d->psc[2][0], d->psc[2][1], d->psc[2][2], d->psc[2][3],
             d->psc[3][0], d->psc[3][1], d->psc[3][2], d->psc[3][3]);
    {
        char vb[17], pb[17];
        for (int k = 0; k < 16; k++) { vb[k] = d->vsb[k] ? '1' : '0'; pb[k] = d->psb[k] ? '1' : '0'; }
        vb[16] = pb[16] = 0;
        VT3_INFO("  bools vs %s ps %s", vb, pb);
    }
    VT3_INFO("  vsc c0 %g %g %g %g | c4 %g %g %g %g | c14 %g %g %g %g | c15 %g %g %g %g",
             d->vsc[0][0], d->vsc[0][1], d->vsc[0][2], d->vsc[0][3],
             d->vsc[4][0], d->vsc[4][1], d->vsc[4][2], d->vsc[4][3],
             d->vsc[14][0], d->vsc[14][1], d->vsc[14][2], d->vsc[14][3],
             d->vsc[15][0], d->vsc[15][1], d->vsc[15][2], d->vsc[15][3]);
    for (int s = 0; s < VT3_MAX_SAMPLERS; s++) {
        d3d9_texture *t = d3d9_texture_of(d->textures[s]);
        if (!t) continue;
        VT3_INFO("  sampler %d: texture %ux%u fmt 0x%x faces %u gl %u | min %d mag %d addrU %d addrV %d addrW %d",
                 s, t->w, t->h, t->fmt, t->faces, (unsigned)t->gl,
                 (int)d->samp[s][6], (int)d->samp[s][5], (int)d->samp[s][1], (int)d->samp[s][2], (int)d->samp[s][3]);
    }
}

/* One vertex attribute as the shader sees it: missing components read (0, 0, 0, 1). */
static void attr_read(const vt3_vattr *at, const uint8_t *v, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const uint8_t *p = v + at->offset;
    for (int i = 0; i < at->components && i < 4; i++) {
        switch (at->type) {
        case VT3_VT_FLOAT: memcpy(&out[i], p + 4 * i, 4); break;
        case VT3_VT_UBYTE_NORM: out[i] = p[i] / 255.0f; break;
        case VT3_VT_UBYTE_NORM_BGRA: out[i] = p[i < 3 ? 2 - i : 3] / 255.0f; break;   /* B,G,R,A in memory */
        case VT3_VT_UBYTE: out[i] = (float)p[i]; break;
        case VT3_VT_SHORT: { int16_t s; memcpy(&s, p + 2 * i, 2); out[i] = (float)s; break; }
        case VT3_VT_SHORT_NORM: { int16_t s; memcpy(&s, p + 2 * i, 2); out[i] = s / 32767.0f; break; }
        default: break;
        }
    }
}

/* VT3_SKIN_CHECK=N: for the first N skinned draws (only those VT3_TRACE_HASH names, when set), skin
 * every vertex on the CPU the way the game's skinning shader does (up to four bones, palette rows
 * c[index + 15..17], weights from BLENDWEIGHT), project it with c0..c3, and log where the draw lands:
 * the NDC box, vertices behind the eye, weight sums, palette rows that are all zero. The raw box
 * (position through c0..c3 with no skinning) shows whether the mesh is in bind space. */
static void skin_check(const d3d9_device *d, const d3d9_shader *vsh, const d3d9_draw_args *a,
                       const vt3_vlayout *lay, const uint8_t *verts, uint32_t nv, uint32_t stride,
                       const uint16_t *idx, uint32_t nidx, int is_list)
{
    static int limit = -1, count;
    static double after_s, first_s = -1.0;
    if (limit < 0) {
        const char *e = getenv("VT3_SKIN_CHECK");
        const char *t = getenv("VT3_SKIN_AFTER");   /* seconds after the first draw, to line up with a capture */
        limit = e ? atoi(e) : 0;
        after_s = t ? atof(t) : 0.0;
    }
    if (count >= limit) return;
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    double now_s = (double)ts_now.tv_sec + ts_now.tv_nsec * 1e-9;
    if (first_s < 0.0) first_s = now_s;
    /* Skinned draws, or any draw whose vertex shader VT3_TRACE_HASH names (positions then go
     * straight through c0..c3, as a rigid mesh's shader does). */
    if (now_s - first_s < after_s || !trace_hash_match(vsh)) return;
    static int trace_hash = -1, skin_nan = -1;
    if (trace_hash < 0) trace_hash = getenv("VT3_TRACE_HASH") != NULL;
    if (skin_nan < 0) skin_nan = getenv("VT3_SKIN_NAN") != NULL;
    if (!vsh->skinned && !trace_hash) return;
    if (isnan(d->vsc[0][0]) && !skin_nan) return;   /* a NaN camera draws nothing; VT3_SKIN_NAN keeps them */
    const vt3_vattr *pos = NULL, *wt = NULL, *ix = NULL;
    for (int k = 0; k < lay->count; k++) {
        if (lay->attr[k].location == 0) pos = &lay->attr[k];
        if (lay->attr[k].location == 1) wt = &lay->attr[k];
        if (lay->attr[k].location == 2) ix = &lay->attr[k];
    }
    count++;
    d3d9_surface *ts = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
    VT3_INFO("skin check %d: vs %08x prims %u verts %u rt 0x%x %ux%u%s, attrs pos %d weight %d index %d", count,
             shader_hash(vsh), a->prim_count, nv, d->rt[0], ts ? ts->w : 0, ts ? ts->h : 0,
             ts && ts->backbuffer ? " (back buffer)" : "", pos != NULL, wt != NULL, ix != NULL);
    {
        d3d9_host *hp = d->ps ? d3d9_host_of(d->ps, D3D9_KIND_PSHADER) : NULL;
        d3d9_texture *t0 = d3d9_texture_of(d->textures[0]);
        VT3_INFO("  ps %08x | blend %u %u/%u atest %u ref %u func %u | z %u zw %u cull %u cw 0x%x | s0 %ux%u fmt 0x%x gl %u"
                 " | c14 %g %g",
                 hp ? shader_hash(hp->data) : 0u, (unsigned)d->rs[27], (unsigned)d->rs[19], (unsigned)d->rs[20],
                 (unsigned)d->rs[15], (unsigned)(d->rs[24] & 0xFF), (unsigned)d->rs[25], (unsigned)d->rs[7],
                 (unsigned)d->rs[14], (unsigned)d->rs[22], (unsigned)d->rs[168], t0 ? t0->w : 0, t0 ? t0->h : 0,
                 t0 ? t0->fmt : 0, t0 ? (unsigned)t0->gl : 0u, d->vsc[14][0], d->vsc[14][1]);
    }
    VT3_INFO("  t %.2f s | c0 %g %g %g %g | c1 %g %g %g %g | c2 %g %g %g %g | c3 %g %g %g %g", now_s - first_s,
             d->vsc[0][0], d->vsc[0][1], d->vsc[0][2], d->vsc[0][3], d->vsc[1][0], d->vsc[1][1], d->vsc[1][2],
             d->vsc[1][3], d->vsc[2][0], d->vsc[2][1], d->vsc[2][2], d->vsc[2][3], d->vsc[3][0], d->vsc[3][1],
             d->vsc[3][2], d->vsc[3][3]);
    {   /* court landmarks through c0..c3 (world units are decimetres; the net is at z = 0) */
        static const float probe[5][3] = {
            { 0, 0, -118.9f }, { 0, 0, 118.9f }, { -41.15f, 0, -118.9f }, { 41.15f, 0, -118.9f }, { 0, 0, 0 } };
        static const char *name[5] = { "baseline centre z-", "baseline centre z+", "singles corner x- z-",
                                       "singles corner x+ z-", "net centre" };
        for (int k = 0; k < 5; k++) {
            float c[4];
            for (int i = 0; i < 4; i++)
                c[i] = d->vsc[0][i] * probe[k][0] + d->vsc[1][i] * probe[k][1] + d->vsc[2][i] * probe[k][2] + d->vsc[3][i];
            VT3_INFO("  probe %-26s ndc %.3f %.3f (w %g)", name[k], c[3] != 0.0f ? c[0] / c[3] : 0.0f,
                     c[3] != 0.0f ? c[1] / c[3] : 0.0f, c[3]);
        }
    }
    for (int k = 0; k < lay->count; k++)
        VT3_INFO("  attr loc %d comps %d type %d offset %d", lay->attr[k].location, lay->attr[k].components,
                 (int)lay->attr[k].type, lay->attr[k].offset);
    if (!pos) return;
    /* The bones per vertex are the shader's skinning loop count (its defi i0): 4 or 2 in this game. */
    int bones = vsh->sm.idef_set[0] ? (int)vsh->sm.idef[0][0] : 4;
    if (bones < 1 || bones > 4) bones = 4;
    if (!vsh->skinned) bones = 0;   /* rigid: the position is used as is */
    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    float rlo[2] = { FLT_MAX, FLT_MAX }, rhi[2] = { -FLT_MAX, -FLT_MAX };
    float wmin = FLT_MAX, wmax = -FLT_MAX;
    float wlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, whi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    uint32_t behind = 0, bad_index = 0;
    int max_index = -1;
    static uint8_t zero_bone[256];
    memset(zero_bone, 0, sizeof zero_bone);
    const vt3_vattr *nrm_at = NULL;
    for (int k = 0; k < lay->count; k++)
        if (lay->attr[k].location == 3) nrm_at = &lay->attr[k];
    /* Per vertex, for the facing check below: clip x, y, w, world position, world normal. */
    float *pv = malloc((size_t)nv * 9 * sizeof *pv);
    for (uint32_t v = 0; v < nv; v++) {
        const uint8_t *vp = verts + (size_t)v * stride;
        float p[4], w[4] = { 0, 0, 0, 1 }, ind[4] = { 0, 0, 0, 1 }, s[3] = { 0, 0, 0 }, n[4] = { 0, 0, 0, 0 };
        float sn[3] = { 0, 0, 0 };
        attr_read(pos, vp, p);
        if (wt) attr_read(wt, vp, w);
        if (ix) attr_read(ix, vp, ind);
        if (nrm_at) {
            attr_read(nrm_at, vp, n);
            for (int c = 0; c < 3; c++) n[c] = n[c] * 2.0f - 1.0f;   /* the shader's mad v1, 2, -1 */
        }
        float wsum = 0.0f;
        for (int b = 0; b < bones; b++) wsum += w[b];
        if (wsum < wmin) wmin = wsum;
        if (wsum > wmax) wmax = wsum;
        for (int b = 0; b < bones; b++) {
            int i = (int)floorf(ind[b]);
            if (i > max_index) max_index = i;
            if (i < 0 || i + 17 > 255) { bad_index++; continue; }
            const float *r0 = d->vsc[i + 15], *r1 = d->vsc[i + 16], *r2 = d->vsc[i + 17];
            int zero = 1;
            for (int c = 0; c < 4; c++) if (r0[c] != 0.0f || r1[c] != 0.0f || r2[c] != 0.0f) zero = 0;
            if (zero && w[b] != 0.0f) zero_bone[i] = 1;
            s[0] += w[b] * (p[0] * r0[0] + p[1] * r0[1] + p[2] * r0[2] + p[3] * r0[3]);
            s[1] += w[b] * (p[0] * r1[0] + p[1] * r1[1] + p[2] * r1[2] + p[3] * r1[3]);
            s[2] += w[b] * (p[0] * r2[0] + p[1] * r2[1] + p[2] * r2[2] + p[3] * r2[3]);
            sn[0] += w[b] * (n[0] * r0[0] + n[1] * r0[1] + n[2] * r0[2]);
            sn[1] += w[b] * (n[0] * r1[0] + n[1] * r1[1] + n[2] * r1[2]);
            sn[2] += w[b] * (n[0] * r2[0] + n[1] * r2[1] + n[2] * r2[2]);
        }
        if (!bones)
            for (int c = 0; c < 3; c++) { s[c] = p[c]; sn[c] = n[c]; }
        for (int c = 0; c < 3; c++) {
            if (s[c] < wlo[c]) wlo[c] = s[c];
            if (s[c] > whi[c]) whi[c] = s[c];
        }
        float clip[4], raw[4];
        for (int c = 0; c < 4; c++) {
            clip[c] = d->vsc[0][c] * s[0] + d->vsc[1][c] * s[1] + d->vsc[2][c] * s[2] + d->vsc[3][c];
            raw[c] = d->vsc[0][c] * p[0] + d->vsc[1][c] * p[1] + d->vsc[2][c] * p[2] + d->vsc[3][c];
        }
        if (pv) {
            float *q = pv + (size_t)v * 9;
            q[0] = clip[0]; q[1] = clip[1]; q[2] = clip[3];
            q[3] = s[0]; q[4] = s[1]; q[5] = s[2];
            q[6] = sn[0]; q[7] = sn[1]; q[8] = sn[2];
        }
        if (v < 2)
            VT3_INFO("  v%u pos %g %g %g w %g %g %g %g idx %g %g %g %g -> skinned %g %g %g clip %g %g %g %g", v,
                     p[0], p[1], p[2], w[0], w[1], w[2], w[3], ind[0], ind[1], ind[2], ind[3], s[0], s[1], s[2],
                     clip[0], clip[1], clip[2], clip[3]);
        if (clip[3] <= 0.0f) { behind++; continue; }
        for (int c = 0; c < 3; c++) {
            float n = clip[c] / clip[3];
            if (n < lo[c]) lo[c] = n;
            if (n > hi[c]) hi[c] = n;
        }
        if (raw[3] > 0.0f)
            for (int c = 0; c < 2; c++) {
                float n = raw[c] / raw[3];
                if (n < rlo[c]) rlo[c] = n;
                if (n > rhi[c]) rhi[c] = n;
            }
    }
    int zeros = 0;
    for (int i = 0; i < 256; i++) zeros += zero_bone[i];
    VT3_INFO("  ndc x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f | behind %u | weight sum %.3f..%.3f | max index %d, "
             "out of range %u, zero bones %d | raw ndc x %.3f..%.3f y %.3f..%.3f", lo[0], hi[0], lo[1], hi[1],
             lo[2], hi[2], behind, wmin, wmax, max_index, bad_index, zeros, rlo[0], rhi[0], rlo[1], rhi[1]);
    VT3_INFO("  world x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f", wlo[0], whi[0], wlo[1], whi[1], wlo[2], whi[2]);

    /* Facing check (triangle lists): the eye is where clip x = y = w = 0. A triangle whose skinned
     * vertex normals point at the eye should be the one the cull mode keeps. Kept means: D3DCULL_CW
     * keeps triangles counter-clockwise in NDC (y up), CCW keeps clockwise ones, NONE keeps all. */
    if (pv && nrm_at && is_list) {
        const float (*m)[4] = (const float (*)[4])d->vsc;
        float a00 = m[0][0], a01 = m[1][0], a02 = m[2][0], b0 = -m[3][0];
        float a10 = m[0][1], a11 = m[1][1], a12 = m[2][1], b1 = -m[3][1];
        float a20 = m[0][3], a21 = m[1][3], a22 = m[2][3], b2 = -m[3][3];
        float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
        if (fabsf(det) > 1e-12f) {
            float eye[3] = {
                (b0 * (a11 * a22 - a12 * a21) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a21 - a11 * b2)) / det,
                (a00 * (b1 * a22 - a12 * b2) - b0 * (a10 * a22 - a12 * a20) + a02 * (a10 * b2 - b1 * a20)) / det,
                (a00 * (a11 * b2 - b1 * a21) - a01 * (a10 * b2 - b1 * a20) + b0 * (a10 * a21 - a11 * a20)) / det };
            int cull = d->rs[22] ? (int)d->rs[22] : 3;
            uint32_t tk = 0, tc = 0, ak = 0, ac = 0;
            for (uint32_t t = 0; t + 2 < nidx; t += 3) {
                uint32_t i0 = idx ? idx[t] : t, i1 = idx ? idx[t + 1] : t + 1, i2 = idx ? idx[t + 2] : t + 2;
                if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                const float *q0 = pv + (size_t)i0 * 9, *q1 = pv + (size_t)i1 * 9, *q2 = pv + (size_t)i2 * 9;
                if (q0[2] <= 0.0f || q1[2] <= 0.0f || q2[2] <= 0.0f) continue;
                float x0 = q0[0] / q0[2], y0 = q0[1] / q0[2], x1 = q1[0] / q1[2], y1 = q1[1] / q1[2];
                float x2 = q2[0] / q2[2], y2 = q2[1] / q2[2];
                float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                int kept = cull == 1 ? 1 : cull == 2 ? area > 0.0f : area < 0.0f;
                float f = 0.0f;
                for (int c = 0; c < 3; c++) {
                    float nc = q0[6 + c] + q1[6 + c] + q2[6 + c];
                    float vc = eye[c] - (q0[3 + c] + q1[3 + c] + q2[3 + c]) / 3.0f;
                    f += nc * vc;
                }
                if (f > 0.0f) { if (kept) tk++; else tc++; }
                else          { if (kept) ak++; else ac++; }
            }
            VT3_INFO("  facing (cull %d): toward eye kept %u culled %u | away kept %u culled %u", cull, tk, tc, ak, ac);
        }
    }
    free(pv);
}

int d3d9_frame_draws;
int d3d9_pixel_watch;   /* set by Present for the one frame VT3_PIXEL_AFTER picks */

/* VT3_PIXEL="x,y[;x,y...]" with VT3_PIXEL_AFTER=<s>: in one frame after that time, read the back
 * buffer after each draw that targets it and log the draws that change a watched pixel (logical
 * coordinates, top-left origin). Names the draw that put a wrong colour on screen. */
static void pixel_watch(const d3d9_device *d, const d3d9_draw_args *a, const d3d9_shader *vsh,
                        const d3d9_shader *psh)
{
    static int n = -1;
    static int px[4][2];
    static uint32_t last[4];
    static uint8_t *buf;
    static uint32_t bw, bh;
    if (n < 0) {
        n = 0;
        const char *e = getenv("VT3_PIXEL");
        while (e && *e && n < 4) {
            char *end;
            px[n][0] = (int)strtol(e, &end, 10);
            if (*end != ',') break;
            px[n][1] = (int)strtol(end + 1, &end, 10);
            last[n] = 0xFFFFFFFFu;
            n++;
            e = *end == ';' ? end + 1 : NULL;
        }
    }
    if (!n || !d3d9_pixel_watch) return;
    d3d9_surface *ts = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
    if (!ts || !ts->backbuffer) return;
    if (!buf || bw != ts->w || bh != ts->h) {
        free(buf);
        bw = ts->w;
        bh = ts->h;
        buf = malloc((size_t)bw * bh * 4);
        if (!buf) return;
    }
    vt3_gfx_readback(buf);
    for (int k = 0; k < n; k++) {
        if (px[k][0] < 0 || px[k][1] < 0 || (uint32_t)px[k][0] >= bw || (uint32_t)px[k][1] >= bh) continue;
        const uint8_t *p = buf + ((size_t)px[k][1] * bw + (size_t)px[k][0]) * 4;
        uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        if (v == last[k]) continue;
        last[k] = v;
        VT3_INFO("pixel %d,%d -> %3u %3u %3u %3u after draw %d: vs %08x ps %08x prims %u blend %u %u/%u z %u zw %u "
                 "zf %u cull %u atest %u", px[k][0], px[k][1], p[0], p[1], p[2], p[3], d3d9_frame_draws,
                 shader_hash(vsh), psh ? shader_hash(psh) : 0u, a->prim_count, (unsigned)d->rs[27],
                 (unsigned)d->rs[19], (unsigned)d->rs[20], (unsigned)d->rs[7], (unsigned)d->rs[14],
                 (unsigned)d->rs[23], (unsigned)d->rs[22], (unsigned)d->rs[15]);
    }
}

HRESULT d3d9_draw(d3d9_device *d, const d3d9_draw_args *a)
{
    if (!d3d9_gpu_ok()) return S_OK;   /* headless: nothing to draw into */
    if (d3d9_target_missing(d->rt[0])) return S_OK;   /* no renderer target: skip, do not paint the back buffer */
    {   /* debug: VT3_SKIP_TARGET=WxH drops draws into targets of that size (for example 512x512, the shadow map) */
        static int skip_w = -1, skip_h;
        if (skip_w < 0) {
            const char *e = getenv("VT3_SKIP_TARGET");
            skip_w = e ? atoi(e) : 0;
            skip_h = (e && strchr(e, 'x')) ? atoi(strchr(e, 'x') + 1) : 0;
        }
        d3d9_surface *tsurf = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
        if (skip_w && tsurf && !tsurf->backbuffer && (int)tsurf->w == skip_w && (int)tsurf->h == skip_h) return S_OK;
    }
    d3d9_gpu_rt_bind(d3d9_surface_target(d->rt[0]));   /* draws go to the device's render target */
    /* VT3_SKIP_FRAME_DRAWS=a-b (or a) drops draws a..b of each frame, to find which draws make up a
     * picture. Debug only. */
    {
        static int lo = -2, hi = -2;
        if (lo == -2) {
            const char *e = getenv("VT3_SKIP_FRAME_DRAWS");
            lo = e ? atoi(e) : -1;
            const char *dash = e ? strchr(e, '-') : NULL;
            hi = dash ? atoi(dash + 1) : lo;
        }
        int idx = d3d9_frame_draws++;
        if (lo >= 0 && idx >= lo && idx <= hi) return S_OK;
    }
    uint32_t nverts = prim_vertex_count(a->prim, a->prim_count);
    if (nverts == 0) {
        static int once;
        if (!warned_once(&once)) VT3_WARN("draw: primitive type %u not supported (skipped)", a->prim);
        return S_OK;
    }
    if (a->prim_count == 0) return S_OK;

    /* ---- where the vertices and indices come from ---- */
    const uint8_t *vbase = NULL;
    uint32_t stride = 0;
    d3d9_decl fvf_decl;
    const d3d9_decl *dc = NULL;
    int from_user = a->up_vertices != NULL;
    if (from_user) {
        stride = a->up_stride;
        vbase = a->up_vertices;
    } else {
        d3d9_buffer *vb = d3d9_buffer_of(d->streams[0][0]);
        if (!vb || !vb->data) { VT3_WARN_ONCE("draw: stream 0 unbound, draw skipped"); return D3DERR_CHK_INVALIDCALL; }
        vbase = vt3_g2h(vb->data + d->streams[0][1]);
        stride = d->streams[0][2];
        if (stride == 0 && d->decl) {
            d3d9_host *h = d3d9_host_of(d->decl, D3D9_KIND_VDECL);
            if (h) stride = ((d3d9_decl *)h->data)->stride[0];
        }
    }
    if (d->decl) {
        d3d9_host *h = d3d9_host_of(d->decl, D3D9_KIND_VDECL);
        if (h) dc = h->data;
    }
    if (!dc) {
        decl_from_fvf(&fvf_decl, d->fvf);
        dc = &fvf_decl;
    }
    if (stride == 0) stride = dc->stride[0];
    if (stride == 0) return D3DERR_CHK_INVALIDCALL;

    /* Several vertex streams (the game's cloth keeps positions and normals in a dynamic stream
     * and the UVs in another): each vertex is merged into one interleaved vertex, the streams in
     * order, and the declaration is rewritten to match, so the rest of the path sees stream 0. */
    d3d9_decl merged;
    int multi = 0;
    uint32_t sbase[16] = { 0 }, sstride[16] = { 0 };
    const uint8_t *sptr[16] = { 0 };
    if (!from_user) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < dc->n; i++) {
            uint32_t s = dc->el[i][0] | (dc->el[i][1] << 8);
            if (s < 16) used |= 1u << s;
        }
        if (used & ~1u) {
            uint32_t off = 0;
            for (uint32_t s = 0; s < 16; s++) {
                if (!(used & (1u << s))) continue;
                d3d9_buffer *sb = d3d9_buffer_of(d->streams[s][0]);
                uint32_t st = d->streams[s][2] ? d->streams[s][2] : dc->stride[s];
                if (!sb || !sb->data || st == 0) { VT3_WARN_ONCE("draw: stream %d unbound, draw skipped", s); return D3DERR_CHK_INVALIDCALL; }
                sptr[s] = vt3_g2h(sb->data + d->streams[s][1]);
                sstride[s] = st;
                sbase[s] = off;
                off += st;
            }
            merged = *dc;
            for (uint32_t i = 0; i < merged.n; i++) {
                uint8_t *e = merged.el[i];
                uint32_t s = e[0] | (e[1] << 8);
                if (s >= 16) continue;
                uint32_t o = (uint32_t)(e[2] | (e[3] << 8)) + sbase[s];
                e[0] = e[1] = 0;
                e[2] = (uint8_t)o;
                e[3] = (uint8_t)(o >> 8);
            }
            merged.stride[0] = off;
            dc = &merged;
            stride = off;
            multi = 1;
        }
    }

    /* ---- vertex range and index list ---- */
    uint32_t first_v = 0, count_v = 0;
    uint16_t *idx16 = NULL;
    uint32_t nidx = nverts;
    /* Shader draws can have the renderer write the converted indices straight into its ring. The
     * fixed-function TL path builds its own vertex list and calls vt3_gfx_draw, so it uses scratch. */
    int ring_indices = d->vs != 0 && a->prim != PRIM_TRIANGLEFAN;   /* the fan pass re-reads idx16 */
    int index_ring = 0;
    uint32_t index_ring_off = 0;
    if (a->indexed) {
        const uint8_t *ibase = NULL;
        uint32_t fmt = 0;
        if (a->up_indices) {
            ibase = a->up_indices;
            fmt = a->up_index_format;
        } else {
            d3d9_buffer *ib = d3d9_buffer_of(d->indices);
            if (!ib || !ib->data) { VT3_WARN_ONCE("draw: index buffer unbound, draw skipped"); return D3DERR_CHK_INVALIDCALL; }
            fmt = ib->fvf_or_fmt;
            ibase = vt3_g2h(ib->data);
        }
        uint32_t isz = fmt == D3DFMT_CHK_INDEX32 ? 4u : 2u;
        const uint8_t *ip = ibase + (size_t)a->start * isz;
        uint32_t lo = a->min_vertex, hi = a->min_vertex + a->num_vertices;
        if (hi <= lo) return S_OK;
        if (ring_indices)
            idx16 = vt3_gfx_reserve_indices((size_t)nidx * sizeof *idx16, &index_ring_off);
        if (idx16) {
            index_ring = 1;
        } else {
            idx16 = index_scratch_reserve(nidx ? nidx : 1u);
            if (!idx16) return E_OUTOFMEMORY_D3D9;
        }
        if (isz == 4) {
            for (uint32_t i = 0; i < nidx; i++) {
                uint32_t v;
                memcpy(&v, ip + 4 * i, 4);
                if (v < lo || v >= hi) {
                    if (warned_once(&g_warn_idx) == 0) VT3_WARN("draw: index %u outside [%u,%u), clamped", v, lo, hi);
                    v = lo;
                }
                idx16[i] = (uint16_t)(v - lo);
            }
        } else {
            for (uint32_t i = 0; i < nidx; i++) {
                uint16_t v16;
                memcpy(&v16, ip + 2 * i, 2);
                uint32_t v = v16;
                if (v < lo || v >= hi) {
                    if (warned_once(&g_warn_idx) == 0) VT3_WARN("draw: index %u outside [%u,%u), clamped", v, lo, hi);
                    v = lo;
                }
                idx16[i] = (uint16_t)(v - lo);
            }
        }
        first_v = a->base_vertex + (int32_t)lo;
        count_v = a->num_vertices;
    } else {
        first_v = a->start;
        count_v = nverts;
    }

    /* A triangle fan becomes a triangle list over the same vertices: triangle t is (0, t+1, t+2)
     * in the fan's own order. The list is an index list, so both paths below take it as indexed. */
    int use_idx = a->indexed;
    uint32_t prim_list = a->prim;
    if (a->prim == PRIM_TRIANGLEFAN) {
        uint32_t fan_n = a->indexed ? nidx : nverts;
        uint32_t tris = fan_n >= 3 ? fan_n - 2 : 0;
        uint16_t *fan = NULL;
        if (d->vs != 0)
            fan = vt3_gfx_reserve_indices((size_t)(tris ? tris : 1) * 3u * sizeof *fan, &index_ring_off);
        if (fan) {
            index_ring = 1;
        } else {
            fan = fan_scratch_reserve((size_t)(tris ? tris : 1) * 3u);
            index_ring = 0;
        }
        if (!fan) { return E_OUTOFMEMORY_D3D9; }
        for (uint32_t t = 0; t < tris; t++) {
            uint32_t k[3] = { 0, t + 1, t + 2 };
            for (int j = 0; j < 3; j++)
                fan[3 * t + (uint32_t)j] = a->indexed ? idx16[k[j]] : (uint16_t)k[j];
        }
        idx16 = fan;
        nidx = tris * 3;
        use_idx = 1;
        prim_list = PRIM_TRIANGLELIST;
    }

    /* ---- fixed function: the TL path ---- */
    if (d->vs == 0 && d->fvf == FVF_TL) {
        if (stride != 28) { return S_OK; }
        uint32_t n = use_idx ? nidx : nverts;
        const uint8_t *src = vbase + (size_t)first_v * stride;
        vt3_tlvertex *tmp = (vt3_tlvertex *)vertex_scratch_reserve((size_t)n * sizeof *tmp);
        if (!tmp) { return E_OUTOFMEMORY_D3D9; }
        if (use_idx) {
            /* idx16 is local to the range that starts at first_v. */
            for (uint32_t i = 0; i < n; i++)
                memcpy(&tmp[i], vbase + (size_t)(first_v + idx16[i]) * stride, 28);
        } else {
            memcpy(tmp, src, (size_t)n * 28);
        }
        /* The movie quad never writes rhw. A vertex whose rhw is not a normal, positive, finite
         * float would divide by zero in the vertex shader, so it gets 1.0. Position comes from x
         * and y, so valid vertices are unchanged. */
        uint32_t fixed = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!(tmp[i].rhw >= FLT_MIN && tmp[i].rhw <= FLT_MAX)) {
                tmp[i].rhw = 1.0f;
                fixed++;
            }
        }
        if (fixed) VT3_DEBUG("TL draw: %u of %u vertices had an invalid rhw, set to 1.0", fixed, n);
        vt3_tex t0 = 0;
        d3d9_texture *tx = d3d9_texture_of(d->textures[0]);
        if (tx) t0 = tx->gl;
        vt3_gfx_draw(prim_list == PRIM_TRIANGLESTRIP ? VT3_PRIM_TRIANGLESTRIP : VT3_PRIM_TRIANGLELIST,
                     tmp, n, t0);
        return S_OK;
    }

    /* ---- shader path ---- */
    if (d->vs == 0) {
        if (!warned_once(&g_warn_ff_ps)) VT3_WARN("draw: fixed-function vertex format 0x%x not supported", d->fvf);
        return S_OK;
    }
    d3d9_host *vh = d3d9_host_of(d->vs, D3D9_KIND_VSHADER);
    d3d9_host *ph = d->ps ? d3d9_host_of(d->ps, D3D9_KIND_PSHADER) : NULL;
    d3d9_shader *vsh = vh ? vh->data : NULL;
    d3d9_shader *psh = ph ? ph->data : NULL;
    {   /* VT3_ONLY_SKINNED=1 keeps only the draws whose vertex shader is skinned (debug: isolate the players) */
        static int only = -1;
        if (only < 0) only = getenv("VT3_ONLY_SKINNED") != NULL;
        if (only && vsh && !vsh->skinned) return S_OK;
    }
    if (!vsh || !vsh->translated || (d->ps && (!psh || !psh->translated))) {
        if (!warned_once(&g_warn_no_prog))
            VT3_WARN("draw: shader not translated (skipped): vs 0x%x %s, ps 0x%x %s", d->vs,
                     vsh ? (vsh->translated ? "translated" : "untranslated") : "missing", d->ps,
                     d->ps ? (psh ? (psh->translated ? "translated" : "untranslated") : "missing") : "none");
        return S_OK;
    }
    {   /* VT3_SKIP_MESH=<vs hash>:<prims>[,...]: drop those draws (debug: see what one mesh covers) */
        static int n_skip = -1;
        static uint32_t skip_hash[8], skip_prims[8];
        if (n_skip < 0) {
            n_skip = 0;
            const char *e = getenv("VT3_SKIP_MESH");
            while (e && *e && n_skip < 8) {
                char *end;
                skip_hash[n_skip] = (uint32_t)strtoul(e, &end, 16);
                if (*end != ':') break;
                skip_prims[n_skip++] = (uint32_t)strtoul(end + 1, &end, 10);
                e = *end == ',' ? end + 1 : NULL;
            }
        }
        for (int k = 0; k < n_skip; k++)
            if (a->prim_count == skip_prims[k] && shader_hash(vsh) == skip_hash[k]) { return S_OK; }
    }
    vt3_prog prog = program_for(d->vs, d->ps, vsh, psh);
    if (!prog) { return S_OK; }

    vt3_vlayout lay;
    if (layout_from_decl(dc, &lay, stride) != 0) {
        if (!warned_once(&g_warn_layout))
            VT3_WARN("draw: vertex element type %u (usage %u) not supported (skipped)", g_bad_type, g_bad_usage);
        return S_OK;
    }
    /* The game's picture and blur quads (FVF 0x102, POSITION float3 + TEXCOORD float2) draw after
     * meshes that set a vertex declaration. D3D9 keeps the FVF and the declaration mutually
     * exclusive, so d3d9_device.c clears one when the other is set; otherwise the quad's layout
     * would come from the previous mesh's declaration and its UV would sample the wrong bytes.
     * The FVF-derived layout above is the quad's real one. */

    /* Whether the vertex bytes need a shadow copy: multiple streams must be gathered into one,
     * or the Adreno UBYTE4 blend-indices must be expanded to float4. D3DCOLOR no longer forces
     * one: its vertices carry VT3_VT_UBYTE_NORM_BGRA and the GPU does the reorder. Wherever we do
     * have to copy, the scratch copy goes into the frame's ring when the backend has one, so the
     * renderer does not copy the same bytes a second time. Anything else is handed to the
     * renderer straight from the locked buffer and it copies into its ring within the same call. */
    int idx_k = -1;
    for (int k = 0; k < lay.count; k++)
        if (lay.attr[k].location == 2 && lay.attr[k].type == VT3_VT_UBYTE && lay.attr[k].components == 4)
            idx_k = k;
    int blend_indices = idx_k >= 0;
    int needs_copy = multi;

    /* Vertex bytes for the referenced range. Indexed: [first, first + count); plain: the vertices. */
    uint32_t v0 = first_v;
    uint32_t nv = a->indexed ? count_v : nverts;
    size_t bytes = (size_t)nv * stride;
    size_t expanded_bytes = blend_indices ? (size_t)nv * (stride + 12u) : 0u;
    size_t scratch_need = (needs_copy ? bytes : 0u) + expanded_bytes;
    uint8_t *vcopy = NULL;
    uint8_t *vring = NULL;
    uint32_t vring_off = 0;
    int vertex_ring = 0;
    if (needs_copy) {
        /* The expansion below needs room after the source, so a blended draw keeps its source in
         * scratch and only the expanded copy goes straight into the ring. */
        if (!blend_indices) vring = vt3_gfx_reserve_vertices(bytes, &vring_off);
        vcopy = vring ? vring : vertex_scratch_reserve(scratch_need ? scratch_need : 1);
        if (!vcopy) { return E_OUTOFMEMORY_D3D9; }
        for (uint32_t v = 0; v < nv; v++)
            for (uint32_t s = 0; s < 16; s++)
                if (sptr[s])
                    memcpy(vcopy + (size_t)v * stride + sbase[s], sptr[s] + (size_t)(v0 + v) * sstride[s], sstride[s]);
        vertex_ring = vring != NULL;
    } else {
        vcopy = (uint8_t *)vbase + (size_t)v0 * stride;   /* renderer copies into its ring within the call */
    }
    skin_check(d, vsh, a, &lay, vcopy, nv, stride, use_idx ? idx16 : NULL, use_idx ? nidx : nverts,
               prim_list != PRIM_TRIANGLESTRIP);
    {   /* VT3_DUMP_SKIN=1: layout and the first vertices of skinned draws, as the renderer receives them */
        static int dump = -1, dumped;
        if (dump < 0) dump = getenv("VT3_DUMP_SKIN") != NULL;
        if (dump && vsh->skinned && dumped < 3) {
            dumped++;
            VT3_INFO("skin draw: stride %d, %d attributes, first vertex %u, %u vertices", stride, lay.count,
                     (unsigned)v0, (unsigned)nv);
            for (int k = 0; k < lay.count; k++)
                VT3_INFO("  attr loc %d comps %d type %d offset %d", lay.attr[k].location, lay.attr[k].components,
                         (int)lay.attr[k].type, lay.attr[k].offset);
            for (uint32_t vi = 0; vi < 3 && vi < nv; vi++) {
                const uint32_t *w = (const uint32_t *)(vcopy + (size_t)vi * stride);
                char line[256]; int pos = 0;
                for (int k = 0; k < stride / 4 && pos < 230; k++)
                    pos += snprintf(line + pos, sizeof line - pos, "%08x ", w[k]);
                VT3_INFO("  v%u: %s", (unsigned)vi, line);
            }
        }
    }
    /* The device's own constant arrays go straight to the renderer, which copies them into the
     * frame's uniform ring within the same call, so they need no shadow copy here. Only a draw
     * that has to patch something - a shader's `def` constants, or a debug hook below - pays for
     * one first. With 210 draws a frame the unconditional copies were 1.7 MB per frame. */
    static float vsc_copy[256 * 4];
    static float psc_copy[256 * 4];
    const float *vsc = &d->vsc[0][0], *psc = &d->psc[0][0];
    {   /* debug: VT3_SHADOW_MVP=1 makes shadow-map draws (512x512 targets) use the scene's light matrix c248..c251 */
        static int shadow_mvp = -1;
        if (shadow_mvp < 0) shadow_mvp = getenv("VT3_SHADOW_MVP") != NULL;
        d3d9_surface *ts = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
        if (shadow_mvp && ts && !ts->backbuffer && ts->w == 512 && ts->h == 512) {
            memcpy(vsc_copy, vsc, sizeof vsc_copy);
            memcpy(vsc_copy, &d->vsc[248][0], 4 * 4 * sizeof(float)), memcpy(&vsc_copy[0], &d->vsc[248][0], 16 * sizeof(float));
            vsc = vsc_copy;
        }
    }
    /* A shader's def constants are its own values. A direct read is translated to the literal, but a
     * relative read (c[aL + n], as the skinning loop does) reads the uniform, which never holds them. Copy
     * them in so both read the same value. cdef_idx lists the few registers that have one; most shaders
     * have none, and that is the common case at the top. */
    if (vsh && vsh->sm.cdef_n) {
        memcpy(vsc_copy, vsc, sizeof vsc_copy);
        for (int i = 0; i < vsh->sm.cdef_n; i++) {
            int r = vsh->sm.cdef_idx[i];
            memcpy(&vsc_copy[4 * r], vsh->sm.cdef[r], 4 * sizeof(float));
        }
        vsc = vsc_copy;
    }
    if (psh && psh->sm.cdef_n) {
        memcpy(psc_copy, psc, sizeof psc_copy);
        for (int i = 0; i < psh->sm.cdef_n; i++) {
            int r = psh->sm.cdef_idx[i];
            memcpy(&psc_copy[4 * r], psh->sm.cdef[r], 4 * sizeof(float));
        }
        psc = psc_copy;
    }

    {   /* VT3_NO_VSM=1 (debug): skinned draws outside the shadow pass get a huge variance floor
         * (c10.x), so the variance shadow factor of the player shaders reads 1 (fully lit). */
        static int no_vsm = -1;
        if (no_vsm < 0) no_vsm = getenv("VT3_NO_VSM") != NULL;
        d3d9_surface *ns = no_vsm && d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
        if (no_vsm && vsh->skinned && !(ns && ns->w == 512 && ns->h == 512)) {
            if (psc != psc_copy) { memcpy(psc_copy, psc, sizeof psc_copy); psc = psc_copy; }
            psc_copy[10 * 4 + 0] = 1.0e6f;
        }
    }

    /* Adreno (740) reads a UBYTE4 blend-indices element (R8G8B8A8_USCALED) as zero, which collapses
     * the skinning shader's bone index to 0 and leaves every skinned mesh in bind/T-pose. The bytes
     * are present and correct (the CPU skinning verifier reads them fine), so expand the 4 index
     * bytes into a float4 in this per-draw copy and lengthen the stride by 12; the shader then gets
     * its bone indices on every driver. */
    vt3_vlayout rlay = lay;
    uint32_t rstride = (uint32_t)lay.stride;
    const uint8_t *draw_vertices = vcopy;
    if (idx_k >= 0) {
        int ioff = lay.attr[idx_k].offset;
        rstride = (uint32_t)lay.stride + 12u;
        /* Convert straight into the ring when it has room; otherwise the merged source stays in
         * the first scratch region while the expanded draw data occupies the tail. */
        uint32_t expanded_off = 0;
        uint8_t *expanded = vt3_gfx_reserve_vertices(expanded_bytes, &expanded_off);
        if (expanded) {
            vertex_ring = 1;
            vring_off = expanded_off;
        } else {
            uint8_t *scratch = vertex_scratch_reserve(scratch_need ? scratch_need : 1);
            size_t expanded_offset = (vcopy == g_vertex_scratch) ? bytes : 0u;
            expanded = scratch ? scratch + expanded_offset : NULL;
            if (!expanded || expanded_bytes > g_vertex_scratch_cap - expanded_offset) {
                return E_OUTOFMEMORY_D3D9;
            }
            vertex_ring = 0;
        }
        {
            for (uint32_t v = 0; v < nv; v++) {
                const uint8_t *src = vcopy + (size_t)v * (uint32_t)lay.stride;
                uint8_t *dst = expanded + (size_t)v * rstride;
                memcpy(dst, src, (size_t)ioff);
                uint8_t ix[4];
                memcpy(ix, src + ioff, sizeof ix);
                float f[4] = { (float)ix[0], (float)ix[1], (float)ix[2], (float)ix[3] };
                memcpy(dst + ioff, f, sizeof f);
                memcpy(dst + ioff + 16, src + ioff + 4,
                       (size_t)lay.stride - (size_t)ioff - 4u);
            }
            draw_vertices = expanded;
            rlay.attr[idx_k].type = VT3_VT_FLOAT;
            for (int k = 0; k < rlay.count; k++)
                if (rlay.attr[k].offset > ioff) rlay.attr[k].offset += 12;
            rlay.stride = (int)rstride;
        }
    }

    vt3_draw dd;
    memset(&dd, 0, sizeof dd);
    dd.prog = prog;
    dd.layout = (idx_k >= 0 && rlay.stride != lay.stride) ? &rlay : &lay;
    dd.vertices = draw_vertices;
    dd.vertex_bytes = (uint32_t)nv * (uint32_t)dd.layout->stride;
    if (vertex_ring) {
        dd.vertex_ring = 1;
        dd.vertex_offset = vring_off;
    }
    dd.indices = use_idx ? idx16 : NULL;
    if (use_idx && index_ring) {
        dd.index_ring = 1;
        dd.index_offset = index_ring_off;
    }
    dd.count = use_idx ? nidx : nverts;
    dd.prim = prim_list == PRIM_TRIANGLESTRIP ? VT3_PRIM_TRIANGLESTRIP : VT3_PRIM_TRIANGLELIST;
    dd.vsc = vsc;
    dd.psc = psc;
    dd.vs_const_max = vsh->sm.const_max;      /* ≤ 256 = vsc[256][4] */
    dd.ps_const_max = psh ? psh->sm.const_max : 0;
    if (dd.ps_const_max > 224u) dd.ps_const_max = 224u;   /* psc[224][4]; device never writes beyond */
    memcpy(dd.vsb, d->vsb, sizeof dd.vsb);
    memcpy(dd.psb, d->psb, sizeof dd.psb);
    fill_textures(d, dd.samplers);
    for (int s = 0; s < VT3_MAX_SAMPLERS; s++) {
        dd.samp_min[s] = (uint8_t)d->samp[s][6];   /* D3DSAMP_MINFILTER */
        dd.samp_mag[s] = (uint8_t)d->samp[s][5];   /* D3DSAMP_MAGFILTER */
        dd.samp_addr_u[s] = (uint8_t)d->samp[s][1];
        dd.samp_addr_v[s] = (uint8_t)d->samp[s][2];
        dd.samp_addr_w[s] = (uint8_t)d->samp[s][3];
    }
    fill_rstate(d, &dd.state);
    static int skin_nocull = -1, skin_flipcull = -1;
    if (skin_nocull < 0) skin_nocull = getenv("VT3_SKIN_NOCULL") != NULL;
    if (skin_flipcull < 0) skin_flipcull = getenv("VT3_SKIN_FLIPCULL") != NULL;
    if (vsh->skinned && skin_nocull) dd.state.cull = 1;   /* debug: skinned meshes drawn two-sided */
    if (vsh->skinned && skin_flipcull && (dd.state.cull == 2 || dd.state.cull == 3))
        dd.state.cull = 5 - dd.state.cull;                              /* debug: skinned meshes, culling reversed */
    {   /* VT3_TRACE_QUAD=1 (debug): the layout and raw floats of the first few small draws, such as full-screen quads */
        static int quad_on = -1, quad_shown;
        if (quad_on < 0) quad_on = getenv("VT3_TRACE_QUAD") != NULL;
        d3d9_surface *qs = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
        if (quad_on && nverts <= 6 && quad_shown < 10 && qs && !qs->backbuffer) {
            quad_shown++;
            VT3_INFO("quad %d: %u verts, stride %d, %d attrs, prim %d, target %ux%u fvf 0x%x%s", quad_shown, (unsigned)nverts, lay.stride,
                     lay.count, (int)dd.prim, qs ? (unsigned)qs->w : 0u, qs ? (unsigned)qs->h : 0u, (unsigned)d->fvf,
                     qs && qs->backbuffer ? " (back buffer)" : "");
            for (int i = 0; i < lay.count; i++)
                VT3_INFO("  attr loc %d comps %d type %d offset %d", lay.attr[i].location, lay.attr[i].components, (int)lay.attr[i].type,
                         lay.attr[i].offset);
            int nf = lay.stride / 4 < 8 ? lay.stride / 4 : 8;
            for (uint32_t v = 0; v < nverts; v++) {
                char line[160];
                int n = 0;
                const float *f = (const float *)((const uint8_t *)vcopy + (size_t)v * (uint32_t)lay.stride);
                for (int k = 0; k < nf; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %g", f[k]);
                VT3_INFO("  v%u:%s", v, line);
            }
            VT3_INFO("  state: blend %d src %d dst %d op %d | z test %d write %d func %d | cull %d | colour write 0x%x | alpha test %d func %d ref %d",
                     dd.state.alpha_blend, dd.state.src_blend, dd.state.dst_blend, dd.state.blend_op, dd.state.depth_test,
                     dd.state.depth_write, dd.state.depth_func, dd.state.cull, dd.state.color_write, dd.state.alpha_test,
                     dd.state.alpha_func, dd.state.alpha_ref);
            for (int k = 0; k < 4; k++)
                VT3_INFO("  vsc c%d: %g %g %g %g   psc c%d: %g %g %g %g", k, vsc[k * 4], vsc[k * 4 + 1], vsc[k * 4 + 2],
                         vsc[k * 4 + 3], k, psc[k * 4], psc[k * 4 + 1], psc[k * 4 + 2], psc[k * 4 + 3]);
        }
    }
    {   /* VT3_TRACE_QUAD_BB=1 (debug): small back-buffer draws, deduplicated by shader and layout
         * because a screen repeats its picture quad every frame. Names the changeover / result
         * background quads and their vertex declaration. */
        static int bb_on = -1;
        static double bb_start;
        static struct { uint32_t vs, ps; int stride, nattr; uint32_t fvf; } seen[128];
        static int shown;
        if (bb_on < 0) {
            const char *e = getenv("VT3_TRACE_QUAD_BB");
            bb_on = e != NULL;
            bb_start = e ? atof(e) : 0.0;   /* seconds to wait before tracing: catch a later screen */
        }
        d3d9_surface *qs = d->rt[0] ? d3d9_surface_of(d->rt[0]) : NULL;
        if (bb_on && nverts <= 6 && qs && qs->backbuffer) {
            static double bb_first = -1.0;
            struct timespec bts;
            clock_gettime(CLOCK_MONOTONIC, &bts);
            double bnow = (double)bts.tv_sec + bts.tv_nsec * 1e-9;
            if (bb_first < 0.0) bb_first = bnow;
            if (bnow - bb_first >= bb_start) {
                uint32_t vh = shader_hash(vsh), ph = psh ? shader_hash(psh) : 0u;
                int dup = 0;
                for (int i = 0; i < shown; i++)
                    if (seen[i].vs == vh && seen[i].ps == ph && seen[i].stride == lay.stride &&
                        seen[i].nattr == lay.count && seen[i].fvf == d->fvf) dup = 1;
                if (!dup && shown < 128) {
                    seen[shown].vs = vh; seen[shown].ps = ph; seen[shown].stride = lay.stride;
                    seen[shown].nattr = lay.count; seen[shown].fvf = d->fvf;
                    shown++;
                    VT3_INFO("bbquad %d: %.1fs, %u verts, stride %d, %d attrs, prim %d, vs %08x ps %08x fvf 0x%x",
                             shown, bnow - bb_first, (unsigned)nverts, lay.stride, lay.count, (int)dd.prim, vh, ph, (unsigned)d->fvf);
                    for (int i = 0; i < lay.count; i++)
                        VT3_INFO("  attr loc %d comps %d type %d offset %d", lay.attr[i].location, lay.attr[i].components,
                                 (int)lay.attr[i].type, lay.attr[i].offset);
                    int nf = lay.stride / 4 < 8 ? lay.stride / 4 : 8;
                    for (uint32_t v = 0; v < nverts; v++) {
                        char line[160];
                        int n = 0;
                        const float *f = (const float *)((const uint8_t *)vcopy + (size_t)v * (uint32_t)lay.stride);
                        for (int k = 0; k < nf; k++) n += snprintf(line + n, sizeof line - (size_t)n, " %g", f[k]);
                        VT3_INFO("  v%u:%s", v, line);
                    }
                }
            }
        }
    }
    trace_draw(d, a, nverts);
    {   /* VT3_TRACE_UV=1 (debug): for draws the hash filter names, each float attribute's raw
         * vertex span and the span after the c14 UV-scale the net shader multiplies by
         * (vo_T0 = vi1 * c14). Separates "wrong UVs in the buffer" from "wrong constant scale". */
        static int uv_on = -1, uv_shown;
        if (uv_on < 0) uv_on = getenv("VT3_TRACE_UV") != NULL;
        if (uv_on && uv_shown < 8 && trace_hash_match(vsh)) {
            uv_shown++;
            VT3_INFO("uv %d: vs %08x ps %08x, %u verts, c14 %g %g %g %g", uv_shown,
                     vsh ? shader_hash(vsh) : 0u, psh ? shader_hash(psh) : 0u, (unsigned)nverts,
                     vsc[14 * 4], vsc[14 * 4 + 1], vsc[14 * 4 + 2], vsc[14 * 4 + 3]);
            for (int k = 0; k < lay.count; k++) {
                if (lay.attr[k].type != VT3_VT_FLOAT) continue;
                float mn[4], mx[4];
                for (int c = 0; c < 4; c++) { mn[c] = 1e30f; mx[c] = -1e30f; }
                for (uint32_t v = 0; v < nv; v++) {
                    float o[4];
                    attr_read(&lay.attr[k], vcopy + (size_t)v * (uint32_t)lay.stride, o);
                    for (int c = 0; c < lay.attr[k].components && c < 4; c++) {
                        if (o[c] < mn[c]) mn[c] = o[c];
                        if (o[c] > mx[c]) mx[c] = o[c];
                    }
                }
                VT3_INFO("  attr loc %d comps %d off %d raw x %g..%g y %g..%g | *c14 x %g..%g y %g..%g",
                         lay.attr[k].location, lay.attr[k].components, lay.attr[k].offset,
                         mn[0], mx[0], mn[1], mx[1],
                         mn[0] * vsc[14 * 4], mx[0] * vsc[14 * 4],
                         mn[1] * vsc[14 * 4 + 1], mx[1] * vsc[14 * 4 + 1]);
            }
        }
    }
    vt3_gfx_draw_program(&dd);
    pixel_watch(d, a, vsh, psh);
    return S_OK;
}
