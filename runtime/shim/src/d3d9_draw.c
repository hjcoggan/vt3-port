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
static int layout_from_decl(const d3d9_decl *dc, vt3_vlayout *lay, uint32_t stride, int *swap_colour)
{
    memset(lay, 0, sizeof *lay);
    *swap_colour = 0;
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
            a->type = VT3_VT_UBYTE_NORM;
            *swap_colour = 1;                  /* D3DCOLOR is BGRA in memory */
            /* Swapping only works per attribute, so mark the element by its offset. */
            lay->attr[k - 1].type = VT3_VT_UBYTE_NORM;
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
    }
    lay->count = k;
    return 0;
}

/* Copies vertex bytes [v0, v1) of one stream into dst, swapping every D3DCOLOR element. */
static void copy_vertices(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t stride,
                          const d3d9_decl *dc)
{
    memcpy(dst, src, (size_t)n * stride);
    for (uint32_t i = 0; i < dc->n; i++) {
        const uint8_t *e = dc->el[i];
        if ((e[0] | (e[1] << 8)) != 0 || e[4] != DT_D3DCOLOR) continue;
        uint32_t off = e[2] | (e[3] << 8);
        for (uint32_t v = 0; v < n; v++) {
            uint8_t *p = dst + (size_t)v * stride + off;
            uint8_t t = p[0];
            p[0] = p[2];
            p[2] = t;
        }
    }
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
    int free_slot = -1;
    for (int i = 0; i < PROG_CACHE; i++) {
        if (!g_progs[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (g_progs[i].vs == vs_obj && g_progs[i].ps == ps_obj) return g_progs[i].prog;
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
    return p;
}

/* ---- the draw ---------------------------------------------------------------- */

static void fill_rstate(const d3d9_device *d, vt3_rstate *s)
{
    memset(s, 0, sizeof *s);
    s->alpha_blend = d->rs[27] != 0;
    s->src_blend = d->rs[19] ? (int)d->rs[19] : 2;
    s->dst_blend = d->rs[20] ? (int)d->rs[20] : 1;
    s->depth_test = d->rs[7] != 0;
    s->depth_write = d->rs[14] != 0;
    s->depth_func = d->rs[23] ? (int)d->rs[23] : 4;
    s->cull = d->rs[22] ? (int)d->rs[22] : 3;
    s->blend_op = d->rs[171] ? (int)d->rs[171] : 1;   /* D3DRS_BLENDOP, default ADD */
    s->alpha_test = d->rs[15] != 0;                  /* D3DRS_ALPHATESTENABLE */
    s->alpha_func = d->rs[25] ? (int)d->rs[25] : 8; /* D3DRS_ALPHAFUNC, default ALWAYS */
    s->alpha_ref = (int)(d->rs[24] & 0xFFu);         /* D3DRS_ALPHAREF */
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
    if (count >= limit || a->prim_count < (uint32_t)min_prims) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now_s = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    if (first_s < 0.0) first_s = now_s;
    if (now_s - first_s < after_s) return;
    count++;
    VT3_INFO("draw %d: prim %u count %u verts %u vs 0x%x ps 0x%x", count, a->prim, a->prim_count, nverts,
             d->vs, d->ps);
    VT3_INFO("  psc c0 %g %g %g %g | c1 %g %g %g %g | c2 %g %g %g %g | c3 %g %g %g %g",
             d->psc[0][0], d->psc[0][1], d->psc[0][2], d->psc[0][3],
             d->psc[1][0], d->psc[1][1], d->psc[1][2], d->psc[1][3],
             d->psc[2][0], d->psc[2][1], d->psc[2][2], d->psc[2][3],
             d->psc[3][0], d->psc[3][1], d->psc[3][2], d->psc[3][3]);
    VT3_INFO("  vsc c0 %g %g %g %g | c4 %g %g %g %g | c15 %g %g %g %g",
             d->vsc[0][0], d->vsc[0][1], d->vsc[0][2], d->vsc[0][3],
             d->vsc[4][0], d->vsc[4][1], d->vsc[4][2], d->vsc[4][3],
             d->vsc[15][0], d->vsc[15][1], d->vsc[15][2], d->vsc[15][3]);
    for (int s = 0; s < VT3_MAX_SAMPLERS; s++) {
        d3d9_texture *t = d3d9_texture_of(d->textures[s]);
        if (!t) continue;
        VT3_INFO("  sampler %d: texture %ux%u fmt 0x%x faces %u gl %u", s, t->w, t->h, t->fmt, t->faces,
                 (unsigned)t->gl);
    }
}

HRESULT d3d9_draw(d3d9_device *d, const d3d9_draw_args *a)
{
    if (!d3d9_gpu_ok()) return S_OK;   /* headless: nothing to draw into */
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
        if (!vb || !vb->data) return D3DERR_CHK_INVALIDCALL;
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

    /* ---- vertex range and index list ---- */
    uint32_t first_v = 0, count_v = 0;
    uint16_t *idx16 = NULL;
    uint32_t nidx = nverts;
    if (a->indexed) {
        const uint8_t *ibase = NULL;
        uint32_t fmt = 0;
        if (a->up_indices) {
            ibase = a->up_indices;
            fmt = a->up_index_format;
        } else {
            d3d9_buffer *ib = d3d9_buffer_of(d->indices);
            if (!ib || !ib->data) return D3DERR_CHK_INVALIDCALL;
            fmt = ib->fvf_or_fmt;
            ibase = vt3_g2h(ib->data);
        }
        uint32_t isz = fmt == D3DFMT_CHK_INDEX32 ? 4u : 2u;
        const uint8_t *ip = ibase + (size_t)a->start * isz;
        uint32_t lo = a->min_vertex, hi = a->min_vertex + a->num_vertices;
        if (hi <= lo) return S_OK;
        idx16 = malloc((size_t)nidx * sizeof *idx16);
        if (!idx16) return E_OUTOFMEMORY_D3D9;
        for (uint32_t i = 0; i < nidx; i++) {
            uint32_t v = isz == 4 ? (uint32_t)(ip[4 * i] | (ip[4 * i + 1] << 8) | (ip[4 * i + 2] << 16) | ((uint32_t)ip[4 * i + 3] << 24))
                                  : (uint32_t)(ip[2 * i] | (ip[2 * i + 1] << 8));
            if (v < lo || v >= hi) {
                if (warned_once(&g_warn_idx) == 0) VT3_WARN("draw: index %u outside [%u,%u), clamped", v, lo, hi);
                v = lo;
            }
            idx16[i] = (uint16_t)(v - lo);
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
        uint16_t *fan = malloc((size_t)(tris ? tris : 1) * 3 * sizeof *fan);
        if (!fan) { free(idx16); return E_OUTOFMEMORY_D3D9; }
        for (uint32_t t = 0; t < tris; t++) {
            uint32_t k[3] = { 0, t + 1, t + 2 };
            for (int j = 0; j < 3; j++)
                fan[3 * t + (uint32_t)j] = a->indexed ? idx16[k[j]] : (uint16_t)k[j];
        }
        free(idx16);
        idx16 = fan;
        nidx = tris * 3;
        use_idx = 1;
        prim_list = PRIM_TRIANGLELIST;
    }

    /* ---- fixed function: the TL path ---- */
    if (d->vs == 0 && d->fvf == FVF_TL) {
        if (stride != 28) { free(idx16); return S_OK; }
        uint32_t n = use_idx ? nidx : nverts;
        const uint8_t *src = vbase + (size_t)first_v * stride;
        vt3_tlvertex *tmp = malloc((size_t)n * sizeof *tmp);
        if (!tmp) { free(idx16); return E_OUTOFMEMORY_D3D9; }
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
        free(tmp);
        free(idx16);
        return S_OK;
    }

    /* ---- shader path ---- */
    if (d->vs == 0) {
        if (!warned_once(&g_warn_ff_ps)) VT3_WARN("draw: fixed-function vertex format 0x%x not supported", d->fvf);
        free(idx16);
        return S_OK;
    }
    d3d9_host *vh = d3d9_host_of(d->vs, D3D9_KIND_VSHADER);
    d3d9_host *ph = d->ps ? d3d9_host_of(d->ps, D3D9_KIND_PSHADER) : NULL;
    d3d9_shader *vsh = vh ? vh->data : NULL;
    d3d9_shader *psh = ph ? ph->data : NULL;
    if (!vsh || !vsh->translated || (d->ps && (!psh || !psh->translated))) {
        if (!warned_once(&g_warn_no_prog))
            VT3_WARN("draw: shader not translated (skipped): vs 0x%x %s, ps 0x%x %s", d->vs,
                     vsh ? (vsh->translated ? "translated" : "untranslated") : "missing", d->ps,
                     d->ps ? (psh ? (psh->translated ? "translated" : "untranslated") : "missing") : "none");
        free(idx16);
        return S_OK;
    }
    vt3_prog prog = program_for(d->vs, d->ps, vsh, psh);
    if (!prog) { free(idx16); return S_OK; }

    vt3_vlayout lay;
    int swap = 0;
    if (layout_from_decl(dc, &lay, stride, &swap) != 0) {
        if (!warned_once(&g_warn_layout))
            VT3_WARN("draw: vertex element type %u (usage %u) not supported (skipped)", g_bad_type, g_bad_usage);
        free(idx16);
        return S_OK;
    }

    /* Vertex bytes for the referenced range. Indexed: [first, first + count); plain: the vertices. */
    uint32_t v0 = first_v;
    uint32_t nv = a->indexed ? count_v : nverts;
    size_t bytes = (size_t)nv * stride;
    uint8_t *vcopy = malloc(bytes ? bytes : 1);
    if (!vcopy) { free(idx16); return E_OUTOFMEMORY_D3D9; }
    copy_vertices(vcopy, vbase + (size_t)v0 * stride, nv, stride, dc);
    (void)swap;

    /* Plain draws reference vertices 0..nverts-1 of the copy. */
    static float vsc_copy[256 * 4];
    static float psc_copy[256 * 4];
    memcpy(vsc_copy, d->vsc, sizeof vsc_copy);
    memset(psc_copy, 0, sizeof psc_copy);
    memcpy(psc_copy, d->psc, sizeof d->psc);

    vt3_draw dd;
    memset(&dd, 0, sizeof dd);
    dd.prog = prog;
    dd.layout = &lay;
    dd.vertices = vcopy;
    dd.vertex_bytes = (uint32_t)bytes;
    dd.indices = use_idx ? idx16 : NULL;
    dd.count = use_idx ? nidx : nverts;
    dd.prim = prim_list == PRIM_TRIANGLESTRIP ? VT3_PRIM_TRIANGLESTRIP : VT3_PRIM_TRIANGLELIST;
    dd.vsc = vsc_copy;
    dd.psc = psc_copy;
    memcpy(dd.vsb, d->vsb, sizeof dd.vsb);
    memcpy(dd.psb, d->psb, sizeof dd.psb);
    fill_textures(d, dd.samplers);
    fill_rstate(d, &dd.state);
    trace_draw(d, a, nverts);
    vt3_gfx_draw_program(&dd);

    free(vcopy);
    free(idx16);
    return S_OK;
}
