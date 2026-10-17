/*
 * SM2 bytecode -> GLSL 330. See include/vt3/vt3_sm2.h for the output conventions.
 *
 * Token layout (D3D9 shader token stream):
 *   version:     0xFFFE0200 (vs_2_0) / 0xFFFF0200 (ps_2_0); low byte minor, next major
 *   instruction: opcode bits 0-15, opcode control bits 16-23, length bits 24-27
 *   dst:         reg num 0-10, write mask 16-19, modifier 20-23, shift 24-27,
 *                reg type = ((t >> 28) & 7) | ((t >> 8) & 0x18)
 *   src:         reg num 0-10, swizzle 16-23, modifier 24-27, relative flag bit 13
 *                (the next token is then the address register)
 *   dcl:         tok1 = usage (0-3) | usage index << 16, or texture type << 27 for samplers
 *   comment:     opcode 0xFFFE, length in bits 16-30 (skipped)
 *   end:         0x0000FFFF
 */
#include "vt3/vt3_sm2.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D3DSHADER_PARAM_REGISTER_TYPE (from the mingw-w64 d3d9types.h) */
enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR_OR_TEX = 3, RT_RASTOUT = 4,
    RT_ATTROUT = 5, RT_TEXCRDOUT = 6, RT_CONSTINT = 7, RT_COLOROUT = 8,
    RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONSTBOOL = 14, RT_LOOP = 15,
    RT_MISC = 17, RT_LABEL = 18, RT_PREDICATE = 19
};

/* D3DSIO_* (from the mingw-w64 d3d9types.h) */
enum {
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5,
    OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11,
    OP_SLT = 12, OP_SGE = 13, OP_LRP = 18, OP_FRC = 19, OP_LOOP = 27, OP_ENDLOOP = 29,
    OP_DCL = 31, OP_POW = 32, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37,
    OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48, OP_IF = 40, OP_ELSE = 42, OP_ENDIF = 43,
    OP_TEXKILL = 65, OP_TEX = 66, OP_DEF = 81, OP_CMP = 88,
    OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

/* D3DSPSM_* source modifiers (bits 24-27) */
enum {
    SM_NONE = 0, SM_NEG = 1, SM_BIAS = 2, SM_BIASNEG = 3, SM_SIGN = 4, SM_SIGNNEG = 5,
    SM_COMP = 6, SM_X2 = 7, SM_X2NEG = 8, SM_DZ = 9, SM_DW = 10, SM_ABS = 11,
    SM_ABSNEG = 12, SM_NOT = 13
};

#define TOK_REL_FLAG   0x00002000u
#define TOK_WMASK_SHIFT 16
#define TOK_DSTMOD_SHIFT 20
#define TOK_DSTSHIFT_SHIFT 24

/* ---- output buffer ---------------------------------------------------- */

typedef struct sb {
    char *p;
    size_t n, cap;
    int oom;
} sb;

static void sb_add(sb *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (k < 0) { va_end(ap2); b->oom = 1; return; }
    size_t need = b->n + (size_t)k + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < need) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) { va_end(ap2); b->oom = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)k;
}

/* ---- translator state -------------------------------------------------- */

typedef struct ctx {
    int vulkan;                     /* VT3_SM2_FLAG_VULKAN: GLSL 450 for SPIR-V */
    const uint32_t *t;
    size_t nt;
    vt3_sm2_shader *out;
    sb body;
    int failed;
    uint32_t temps_used;            /* bit per r# */
    int16_t in_loc[VT3_SM2_MAX_ATTR];   /* VS: location per v# from dcl, -1 if none */
    uint32_t in_used;               /* VS: v# read */
    uint32_t tc_used;               /* PS: t# or VS: oT# written (bit per index) */
    uint32_t d_used;                /* bit per vo_D# used */
    int loop_seq;
    int depth_stack[32];            /* 1 = loop, 2 = if */
    int depth;
    int loop_ids[32];
    int uses_pos;                   /* VS: oPos written */
    uint32_t oc_used;               /* PS: bit per oC# written (0-3) */
} ctx;

static void fail(ctx *c, const char *fmt, ...)
{
    if (c->failed) return;
    c->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->out->error, sizeof c->out->error, fmt, ap);
    va_end(ap);
}

static int reg_type(uint32_t tok)
{
    return (int)(((tok >> 28) & 7u) | ((tok >> 8) & 0x18u));
}

static const char *swz_chars = "xyzw";

static void swizzle_str(uint32_t sw, char *out)
{
    for (int i = 0; i < 4; i++) out[i] = swz_chars[(sw >> (2 * i)) & 3u];
    out[4] = '\0';
}

static void mask_str(uint32_t wm, char *out)
{
    int k = 0;
    for (int i = 0; i < 4; i++) if (wm & (1u << i)) out[k++] = swz_chars[i];
    out[k] = '\0';
}

/* Base name of a register (no swizzle). Marks what the shader uses. */
/* A GLSL float literal for v. Non-finite values have no literal, so they become 0. */
static void glsl_float(float v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%.9g", (double)v);
    if (strpbrk(buf, "ni")) snprintf(buf, cap, "0.0");
    else if (!strpbrk(buf, ".e")) strncat(buf, ".0", cap - strlen(buf) - 1);
}

static void reg_base(ctx *c, int type, uint32_t idx, int has_rel, uint32_t rel_tok,
                     char *out, size_t cap)
{
    char rel[16] = "";
    if (has_rel) {
        int rt = reg_type(rel_tok);
        char comp = swz_chars[rel_tok >> 16 & 3u];
        if (rt == RT_ADDR_OR_TEX) snprintf(rel, sizeof rel, "a0.%c", comp);
        else if (rt == RT_LOOP) snprintf(rel, sizeof rel, "aL.%c", comp);
        else { fail(c, "relative addressing through register type %d", rt); return; }
    }
    int vs = c->out->kind == VT3_SM_VS;
    switch (type) {
    case RT_TEMP:
        if (idx >= VT3_SM2_MAX_TEMP) { fail(c, "temp r%u out of range", idx); return; }
        c->temps_used |= 1u << idx;
        snprintf(out, cap, "r%u", idx);
        return;
    case RT_INPUT:
        if (vs) {
            if (idx >= VT3_SM2_MAX_ATTR) { fail(c, "input v%u out of range", idx); return; }
            c->in_used |= 1u << idx;
            snprintf(out, cap, "vi%u", idx);
        } else {
            if (idx > 1) { fail(c, "pixel input v%u not supported", idx); return; }
            c->d_used |= 1u << idx;
            snprintf(out, cap, "vo_D%u", idx);
        }
        return;
    case RT_CONST:
        if (idx >= VT3_SM2_MAX_CONST) { fail(c, "constant c%u out of range", idx); return; }
        if (!has_rel && (int)idx + 1 > c->out->const_max) c->out->const_max = (int)idx + 1;
        if (has_rel) {
            snprintf(out, cap, "%sc[%u + %s]", vs ? "vs" : "ps", idx, rel);
            if (c->out->const_max < VT3_SM2_MAX_CONST) c->out->const_max = VT3_SM2_MAX_CONST;
        } else if (c->out->cdef_set[idx]) {
            /* A def constant belongs to the shader: its value is part of the code, and it
             * does not come from the device's constant store. */
            char f[4][40];
            for (int i = 0; i < 4; i++) glsl_float(c->out->cdef[idx][i], f[i], sizeof f[i]);
            snprintf(out, cap, "vec4(%s, %s, %s, %s)", f[0], f[1], f[2], f[3]);
        } else {
            snprintf(out, cap, "%sc[%u]", vs ? "vs" : "ps", idx);
        }
        return;
    case RT_ADDR_OR_TEX:
        if (vs) {
            fail(c, "address register used as a source");
            return;
        }
        if (idx >= VT3_SM2_MAX_TEXCOORD) { fail(c, "texture coordinate t%u out of range", idx); return; }
        c->tc_used |= 1u << idx;
        snprintf(out, cap, "vo_T%u", idx);
        return;
    case RT_CONSTINT:
        if (idx >= VT3_SM2_MAX_INT) { fail(c, "int constant i%u out of range", idx); return; }
        snprintf(out, cap, "%si[%u]", vs ? "vs" : "ps", idx);
        return;
    default:
        fail(c, "register type %d not supported as a value", type);
        return;
    }
}

/* Source operand as a vec4 expression, with swizzle and modifier applied. */
static void src_expr(ctx *c, uint32_t tok, int has_rel, uint32_t rel_tok, char *out, size_t cap)
{
    char base[320], sw[8], e[1024];
    reg_base(c, reg_type(tok), tok & 0x7FFu, has_rel, rel_tok, base, sizeof base);
    if (c->failed) { out[0] = '\0'; return; }
    swizzle_str((tok >> 16) & 0xFFu, sw);
    if (strcmp(sw, "xyzw") == 0)
        snprintf(e, sizeof e, "%s", base);
    else
        snprintf(e, sizeof e, "%s.%s", base, sw);

    char m[1280];
    switch ((tok >> 24) & 0xFu) {
    case SM_NONE:    snprintf(m, sizeof m, "%s", e); break;
    case SM_NEG:     snprintf(m, sizeof m, "(-%s)", e); break;
    case SM_BIAS:    snprintf(m, sizeof m, "(%s - 0.5)", e); break;
    case SM_BIASNEG: snprintf(m, sizeof m, "(-(%s - 0.5))", e); break;
    case SM_SIGN:    snprintf(m, sizeof m, "(2.0 * %s - 1.0)", e); break;
    case SM_SIGNNEG: snprintf(m, sizeof m, "(-(2.0 * %s - 1.0))", e); break;
    case SM_COMP:    snprintf(m, sizeof m, "(1.0 - %s)", e); break;
    case SM_X2:      snprintf(m, sizeof m, "(2.0 * %s)", e); break;
    case SM_X2NEG:   snprintf(m, sizeof m, "(-2.0 * %s)", e); break;
    case SM_DZ:      snprintf(m, sizeof m, "(%s / %s.z)", e, base); break;
    case SM_DW:      snprintf(m, sizeof m, "(%s / %s.w)", e, base); break;
    case SM_ABS:     snprintf(m, sizeof m, "abs(%s)", e); break;
    case SM_ABSNEG:  snprintf(m, sizeof m, "(-abs(%s))", e); break;
    default:
        fail(c, "source modifier %u not supported", (tok >> 24) & 0xFu);
        m[0] = '\0';
    }
    snprintf(out, cap, "%s", m);
}

/* Destination: writes `value` (vec4 expression) to the masked components. */
static void dst_write(ctx *c, uint32_t tok, const char *value)
{
    int type = reg_type(tok);
    uint32_t idx = tok & 0x7FFu;
    uint32_t wm = (tok >> TOK_WMASK_SHIFT) & 0xFu;
    if (wm == 0) return;
    char name[64], mask[8], v[4800];
    int vs = c->out->kind == VT3_SM_VS;

    switch (type) {
    case RT_TEMP:
        if (idx >= VT3_SM2_MAX_TEMP) { fail(c, "temp r%u out of range", idx); return; }
        c->temps_used |= 1u << idx;
        snprintf(name, sizeof name, "r%u", idx);
        break;
    case RT_RASTOUT:
        if (!vs) { fail(c, "rasterizer output in a pixel shader"); return; }
        if (idx == 0) { snprintf(name, sizeof name, "vs_pos"); c->uses_pos = 1; }
        else { return; }   /* oFog and oPts are not used by the GL path */
        break;
    case RT_ATTROUT:
        if (!vs) { fail(c, "colour output used as an attribute"); return; }
        if (idx > 1) { fail(c, "vertex colour output oD%u not supported", idx); return; }
        c->d_used |= 1u << idx;
        snprintf(name, sizeof name, "vo_D%u", idx);
        break;
    case RT_TEXCRDOUT:
        if (!vs) { fail(c, "texcoord output in a pixel shader"); return; }
        if (idx >= VT3_SM2_MAX_TEXCOORD) { fail(c, "texcoord output oT%u out of range", idx); return; }
        c->tc_used |= 1u << idx;
        snprintf(name, sizeof name, "vo_T%u", idx);
        break;
    case RT_COLOROUT:
        if (vs) { fail(c, "colour output in a vertex shader"); return; }
        if (idx >= 4) { fail(c, "colour output oC%u out of range", idx); return; }
        c->oc_used |= 1u << idx;
        snprintf(name, sizeof name, "oC%u", idx);
        break;
    default:
        fail(c, "destination register type %d not supported", type);
        return;
    }
    if (c->failed) return;

    /* Result modifiers. _sat clamps; _pp is ignored; shift scales by 2^n. */
    snprintf(v, sizeof v, "%s", value);
    uint32_t dmod = (tok >> TOK_DSTMOD_SHIFT) & 0xFu;
    if (dmod & 1u) snprintf(v, sizeof v, "clamp(%s, 0.0, 1.0)", value);
    int shift = (int)((tok >> TOK_DSTSHIFT_SHIFT) & 0xFu);
    if (shift >= 8) shift -= 16;
    if (shift != 0) {
        double f = 1.0;
        for (int i = 0; i < (shift > 0 ? shift : -shift); i++) f = shift > 0 ? f * 2.0 : f / 2.0;
        char tmp[4800];
        snprintf(tmp, sizeof tmp, "(%s * %g)", v, f);
        snprintf(v, sizeof v, "%s", tmp);
    }

    if (wm == 0xFu) {
        sb_add(&c->body, "  %s = %s;\n", name, v);
    } else {
        mask_str(wm, mask);
        sb_add(&c->body, "  %s.%s = (%s).%s;\n", name, mask, v, mask);
    }
}

/* Operand reader: advances *p past the operand (and its relative token). */
typedef struct opnd {
    uint32_t tok;
    int has_rel;
    uint32_t rel;
} opnd;

static opnd read_opnd(ctx *c, size_t *p, size_t end)
{
    opnd o = {0, 0, 0};
    if (*p >= end) { fail(c, "operand runs past the instruction"); return o; }
    o.tok = c->t[(*p)++];
    if (o.tok & TOK_REL_FLAG) {
        if (*p >= end) { fail(c, "relative operand missing its address token"); return o; }
        o.has_rel = 1;
        o.rel = c->t[(*p)++];
    }
    return o;
}

/* Builds the GLSL for one arithmetic instruction. Returns after emitting. */
static void do_arith(ctx *c, uint32_t op, opnd dst, opnd s[3], int nsrc)
{
    char e[3][1400];
    for (int i = 0; i < nsrc; i++) {
        src_expr(c, s[i].tok, s[i].has_rel, s[i].rel, e[i], sizeof e[i]);
        if (c->failed) return;
    }
    char value[4800];
    switch (op) {
    case OP_MOV:  snprintf(value, sizeof value, "%s", e[0]); break;
    case OP_ADD:  snprintf(value, sizeof value, "(%s + %s)", e[0], e[1]); break;
    case OP_SUB:  snprintf(value, sizeof value, "(%s - %s)", e[0], e[1]); break;
    case OP_MUL:  snprintf(value, sizeof value, "(%s * %s)", e[0], e[1]); break;
    case OP_MAD:  snprintf(value, sizeof value, "(%s * %s + %s)", e[0], e[1], e[2]); break;
    case OP_RCP:  snprintf(value, sizeof value, "vec4(1.0 / (%s).x)", e[0]); break;
    case OP_RSQ:  snprintf(value, sizeof value, "vec4(inversesqrt(abs((%s).x)))", e[0]); break;
    case OP_DP3:  snprintf(value, sizeof value, "vec4(dot((%s).xyz, (%s).xyz))", e[0], e[1]); break;
    case OP_DP4:  snprintf(value, sizeof value, "vec4(dot(%s, %s))", e[0], e[1]); break;
    case OP_MIN:  snprintf(value, sizeof value, "min(%s, %s)", e[0], e[1]); break;
    case OP_MAX:  snprintf(value, sizeof value, "max(%s, %s)", e[0], e[1]); break;
    case OP_SLT:  snprintf(value, sizeof value, "mix(vec4(0.0), vec4(1.0), lessThan(%s, %s))", e[0], e[1]); break;
    case OP_SGE:  snprintf(value, sizeof value, "mix(vec4(0.0), vec4(1.0), greaterThanEqual(%s, %s))", e[0], e[1]); break;
    case OP_FRC:  snprintf(value, sizeof value, "fract(%s)", e[0]); break;
    case OP_LRP:  snprintf(value, sizeof value, "mix(%s, %s, %s)", e[2], e[1], e[0]); break;
    case OP_ABS:  snprintf(value, sizeof value, "abs(%s)", e[0]); break;
    case OP_NRM:  snprintf(value, sizeof value, "vt3_nrm(%s)", e[0]); break;
    case OP_POW:  snprintf(value, sizeof value, "pow(max(abs(%s), vec4(1e-30)), %s)", e[0], e[1]); break;   /* pow(0, y) is undefined in GLSL */
    case OP_CMP:  snprintf(value, sizeof value, "mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))", e[2], e[1], e[0]); break;
    case OP_SINCOS: snprintf(value, sizeof value, "vec4(cos((%s).x), sin((%s).x), 0.0, 0.0)", e[0], e[0]); break;
    default:
        fail(c, "opcode %u not handled by do_arith", op);
        return;
    }
    dst_write(c, dst.tok, value);
    if (!c->failed) c->out->ninsn++;
}

/* Number of source operands for each arithmetic opcode. 0 = not arithmetic. */
static int arith_nsrc(uint32_t op)
{
    switch (op) {
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_FRC: case OP_ABS: case OP_NRM: case OP_SINCOS:
        return 1;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX:
    case OP_SLT: case OP_SGE: case OP_POW:
        return 2;
    case OP_MAD: case OP_LRP: case OP_CMP:
        return 3;
    default:
        return 0;
    }
}

static void do_texld(ctx *c, uint32_t op_tok, opnd dst, opnd coord, uint32_t samp_tok)
{
    if (reg_type(samp_tok) != RT_SAMPLER || (samp_tok & 0x7FFu) >= VT3_SM2_MAX_SAMPLER) {
        fail(c, "texld sampler operand is not s0-s15");
        return;
    }
    unsigned s = samp_tok & 0x7FFu;
    char e[1400], value[4800], uv[1500];
    src_expr(c, coord.tok, coord.has_rel, coord.rel, e, sizeof e);
    if (c->failed) return;
    int project = (op_tok >> 16) & 1u;
    int cube = c->out->sampler_type[s] == 3;
    if (project)
        snprintf(uv, sizeof uv, "((%s) / (%s).w)", e, e);
    else
        snprintf(uv, sizeof uv, "(%s)", e);
    snprintf(value, sizeof value, "texture(s%u, %s%s)", s, uv, cube ? ".xyz" : ".xy");
    if (c->out->sampler_type[s] == 0) c->out->sampler_type[s] = cube ? 3 : 2;
    c->out->sampler_mask |= (uint16_t)(1u << s);
    dst_write(c, dst.tok, value);
    if (!c->failed) c->out->ninsn++;
}

static int bool_name(ctx *c, uint32_t tok, char *out, size_t cap)
{
    if (reg_type(tok) != RT_CONSTBOOL || (tok & 0x7FFu) >= VT3_SM2_MAX_BOOL) {
        fail(c, "if: operand is not a bool register");
        return 0;
    }
    unsigned n = tok & 0x7FFu;
    c->out->bool_used |= (uint8_t)(1u << n);
    if (c->vulkan) snprintf(out, cap, "vt3_%sb(%u)", c->out->kind == VT3_SM_VS ? "vs" : "ps", n);
    else snprintf(out, cap, "%sb[%u]", c->out->kind == VT3_SM_VS ? "vs" : "ps", n);
    return 1;
}

static void do_dcl(ctx *c, uint32_t usage_tok, uint32_t dst_tok)
{
    int type = reg_type(dst_tok);
    uint32_t idx = dst_tok & 0x7FFu;
    if (type == RT_SAMPLER) {
        if (idx >= VT3_SM2_MAX_SAMPLER) { fail(c, "sampler s%u out of range", idx); return; }
        uint32_t tt = (usage_tok >> 27) & 0xFu;   /* D3DSTT_*: 2 = 2D, 3 = cube */
        c->out->sampler_type[idx] = (uint8_t)(tt == 3 ? 3 : 2);
        return;
    }
    if (c->out->kind == VT3_SM_VS && type == RT_INPUT) {
        uint32_t usage = usage_tok & 0xFu;
        uint32_t uidx = (usage_tok >> 16) & 0xFu;
        int loc = vt3_sm2_attr_location(usage, uidx);
        if (loc < 0) { fail(c, "vertex input usage %u[%u] has no location", usage, uidx); return; }
        if (idx >= VT3_SM2_MAX_ATTR) { fail(c, "input v%u out of range", idx); return; }
        c->in_loc[idx] = (int16_t)loc;
        return;
    }
    /* Pixel-shader texcoord and colour declarations need no action here. */
}

static void do_def(ctx *c, uint32_t op, uint32_t dst_tok, const uint32_t *vals)
{
    int type = reg_type(dst_tok);
    uint32_t idx = dst_tok & 0x7FFu;
    if (op == OP_DEF) {
        if (type != RT_CONST || idx >= VT3_SM2_MAX_CONST) { fail(c, "def: bad constant register"); return; }
        for (int i = 0; i < 4; i++) {
            union { uint32_t u; float f; } cv;
            cv.u = vals[i];
            c->out->cdef[idx][i] = cv.f;
        }
        if (!c->out->cdef_set[idx]) c->out->cdef_idx[c->out->cdef_n++] = (uint16_t)idx;
        c->out->cdef_set[idx] = 1;
        if ((int)idx + 1 > c->out->const_max) c->out->const_max = (int)idx + 1;
    } else {
        if (type != RT_CONSTINT || idx >= VT3_SM2_MAX_INT) { fail(c, "defi: bad int constant register"); return; }
        for (int i = 0; i < 4; i++) c->out->idef[idx][i] = (int32_t)vals[i];
        c->out->idef_set[idx] = 1;
    }
}

static void open_block(ctx *c, int kind)
{
    if (c->depth >= 32) { fail(c, "control flow nested too deep"); return; }
    c->depth_stack[c->depth] = kind;
    if (kind == 1) c->loop_ids[c->depth] = c->loop_seq++;
    c->depth++;
}

static void close_block(ctx *c, int expect, const char *what)
{
    if (c->depth == 0 || c->depth_stack[c->depth - 1] != expect) {
        fail(c, "%s without a matching open block", what);
        return;
    }
    c->depth--;
}

/* ---- main walk -------------------------------------------------------- */

/* Def constants hold for the whole shader, so they are collected before the walk. */
static void prescan_defs(ctx *c)
{
    size_t pos = 1;
    while (pos < c->nt) {
        uint32_t tok = c->t[pos];
        uint32_t op = tok & 0xFFFFu;
        if (op == OP_END) break;
        if (op == OP_COMMENT) {
            pos += 1 + ((tok >> 16) & 0x7FFFu);
            continue;
        }
        size_t len = (tok >> 24) & 0xFu;
        if (op == OP_DEF && len == 5 && pos + 5 < c->nt) do_def(c, OP_DEF, c->t[pos + 1], &c->t[pos + 2]);
        pos += 1 + len;
    }
}

static int translate_walk(ctx *c)
{
    size_t pos = 1;   /* token 0 is the version */
    while (pos < c->nt) {
        uint32_t tok = c->t[pos];
        uint32_t op = tok & 0xFFFFu;
        if (op == OP_END) break;
        if (op == OP_COMMENT) {
            uint32_t size = (tok >> 16) & 0x7FFFu;
            pos += 1 + size;
            continue;
        }
        size_t len = (tok >> 24) & 0xFu;
        size_t p = pos + 1;
        size_t end = pos + 1 + len;
        if (end > c->nt) { fail(c, "instruction at token %zu runs past the end", pos); return -1; }

        switch (op) {
        case OP_NOP:
            break;
        case OP_DCL: {
            uint32_t usage = c->t[p];
            uint32_t dst = c->t[p + 1];
            do_dcl(c, usage, dst);
            break;
        }
        case OP_DEF:
        case OP_DEFI: {
            if (len != 5) { fail(c, "def: expected 5 tokens"); return -1; }
            do_def(c, op, c->t[p], &c->t[p + 1]);
            break;
        }
        case OP_DEFB:
            fail(c, "defb not supported");
            return -1;
        case OP_MOVA: {
            opnd dst = read_opnd(c, &p, end);
            opnd s0 = read_opnd(c, &p, end);
            if (c->failed) return -1;
            char e[1400];
            src_expr(c, s0.tok, s0.has_rel, s0.rel, e, sizeof e);
            if (c->failed) return -1;
            uint32_t wm = (dst.tok >> 16) & 0xFu;
            if (reg_type(dst.tok) != RT_ADDR_OR_TEX || c->out->kind != VT3_SM_VS) {
                fail(c, "mova: destination must be a0 in a vertex shader");
                return -1;
            }
            for (int i = 0; i < 4; i++) {
                if (!(wm & (1u << i))) continue;
                sb_add(&c->body, "  a0.%c = int(round((%s).%c));\n", swz_chars[i], e, swz_chars[i]);
            }
            c->out->ninsn++;
            break;
        }
        case OP_TEX: {
            opnd dst = read_opnd(c, &p, end);
            opnd coord = read_opnd(c, &p, end);
            opnd samp = read_opnd(c, &p, end);
            if (c->failed) return -1;
            do_texld(c, tok, dst, coord, samp.tok);
            if (c->failed) return -1;
            break;
        }
        case OP_TEXKILL: {
            opnd r = read_opnd(c, &p, end);
            if (c->failed) return -1;
            char base[320];
            reg_base(c, reg_type(r.tok), r.tok & 0x7FFu, 0, 0, base, sizeof base);
            if (c->failed) return -1;
            sb_add(&c->body, "  if (any(lessThan((%s).xyz, vec3(0.0)))) discard;\n", base);
            c->out->ninsn++;
            break;
        }
        case OP_LOOP: {
            (void)read_opnd(c, &p, end);            /* aL register: implicit */
            opnd ireg = read_opnd(c, &p, end);
            if (c->failed) return -1;
            if (reg_type(ireg.tok) != RT_CONSTINT || (ireg.tok & 0x7FFu) >= VT3_SM2_MAX_INT) {
                fail(c, "loop: counter must be an int constant i#");
                return -1;
            }
            open_block(c, 1);
            if (c->failed) return -1;
            int id = c->loop_ids[c->depth - 1];
            unsigned n = ireg.tok & 0x7FFu;
            if (c->out->idef_set[n]) {
                /* The shader's own defi: in D3D it overrides the device's integer constant for this
                 * shader, so the loop bounds are literals (count, start, step). The uniform is never
                 * set from the defi, and the game's skinning loops rely on it. */
                const int32_t *iv = c->out->idef[n];
                sb_add(&c->body, "  for (int _lk%d = 0; _lk%d < %d; _lk%d++) {\n", id, id, (int)iv[0], id);
                sb_add(&c->body, "  aL.x = %d + _lk%d * %d;\n", (int)iv[1], id, (int)iv[2]);
            } else {
                const char *pref = c->out->kind == VT3_SM_VS ? "vs" : "ps";
                sb_add(&c->body, "  for (int _lk%d = 0; _lk%d < %si[%u].x; _lk%d++) {\n", id, id, pref, n, id);
                sb_add(&c->body, "  aL.x = %si[%u].y + _lk%d * %si[%u].z;\n", pref, n, id, pref, n);
            }
            c->out->ninsn++;
            break;
        }
        case OP_ENDLOOP:
            close_block(c, 1, "endloop");
            if (c->failed) return -1;
            sb_add(&c->body, "  }\n");
            break;
        case OP_IF: {
            opnd b = read_opnd(c, &p, end);
            if (c->failed) return -1;
            char bn[64];
            if (!bool_name(c, b.tok, bn, sizeof bn)) return -1;
            open_block(c, 2);
            if (c->failed) return -1;
            sb_add(&c->body, "  if (%s) {\n", bn);
            c->out->ninsn++;
            break;
        }
        case OP_ELSE:
            if (c->depth == 0 || c->depth_stack[c->depth - 1] != 2) { fail(c, "else without if"); return -1; }
            sb_add(&c->body, "  } else {\n");
            break;
        case OP_ENDIF:
            close_block(c, 2, "endif");
            if (c->failed) return -1;
            sb_add(&c->body, "  }\n");
            break;
        default: {
            int nsrc = arith_nsrc(op);
            if (nsrc == 0) {
                fail(c, "opcode %u (token %zu) not supported", op, pos);
                return -1;
            }
            opnd dst = read_opnd(c, &p, end);
            opnd s[3];
            for (int i = 0; i < nsrc; i++) s[i] = read_opnd(c, &p, end);
            if (c->failed) return -1;
            do_arith(c, op, dst, s, nsrc);
            if (c->failed) return -1;
            break;
        }
        }
        if (c->failed) return -1;
        pos = end;
    }
    if (c->depth != 0) { fail(c, "unclosed loop or if"); return -1; }
    return 0;
}

/* ---- public ----------------------------------------------------------- */

int vt3_sm2_attr_location(uint32_t usage, uint32_t usage_index)
{
    switch (usage) {
    case 0:  /* POSITION */    return usage_index == 0 ? 0 : -1;
    case 1:  /* BLENDWEIGHT */ return usage_index == 0 ? 1 : -1;
    case 2:  /* BLENDINDICES */return usage_index == 0 ? 2 : -1;
    case 3:  /* NORMAL */      return usage_index == 0 ? 3 : -1;
    case 4:  /* PSIZE */       return usage_index == 0 ? 4 : -1;
    case 5:  /* TEXCOORD */    return usage_index < 8 ? (int)(5 + usage_index) : -1;
    case 6:  /* TANGENT */     return usage_index == 0 ? 13 : -1;
    case 7:  /* BINORMAL */    return usage_index == 0 ? 14 : -1;
    case 10: /* COLOR */       return usage_index == 0 ? 15 : -1;
    default: return -1;
    }
}

void vt3_sm2_free(vt3_sm2_shader *out)
{
    if (!out) return;
    free(out->glsl);
    out->glsl = NULL;
}

static void emit_header(ctx *c, sb *h)
{
    int vs = c->out->kind == VT3_SM_VS;
    int vk = c->vulkan;
    sb_add(h, vk ? "#version 450\n" : "#version 330 core\n");
    sb_add(h, "// generated by vt3 sm2_glsl: %s %d.%d%s\n", vs ? "vs" : "ps", c->out->major, c->out->minor,
           vk ? " (vulkan)" : "");
    if (vs) {
        for (int i = 0; i < VT3_SM2_MAX_ATTR; i++) {
            if (!(c->in_used & (1u << i))) continue;
            int loc = c->in_loc[i] >= 0 ? c->in_loc[i] : i;
            c->out->attr_mask |= 1u << loc;
            sb_add(h, "layout(location = %d) in vec4 vi%d;\n", loc, i);
        }
        if (vk) {
            sb_add(h, "layout(location = 0) out vec4 vo_D0;\nlayout(location = 1) out vec4 vo_D1;\n");
            for (int i = 0; i < VT3_SM2_MAX_TEXCOORD; i++) sb_add(h, "layout(location = %d) out vec4 vo_T%d;\n", 2 + i, i);
            sb_add(h, "layout(std140, set = 0, binding = 0) uniform VsConsts {\n"
                      "  vec4 vsc[256];\n  ivec4 vsi[16];\n  ivec4 vsb4[4];\n};\n"
                      "bool vt3_vsb(int n) { return vsb4[n >> 2][n & 3] != 0; }\n");
        } else {
            sb_add(h, "out vec4 vo_D0;\nout vec4 vo_D1;\n");
            for (int i = 0; i < VT3_SM2_MAX_TEXCOORD; i++) sb_add(h, "out vec4 vo_T%d;\n", i);
            sb_add(h, "uniform vec4 vsc[256];\nuniform ivec4 vsi[16];\nuniform bool vsb[16];\n"
                      "uniform float vt3_flip;\n");   /* -1 when drawing into a render texture (gl33.c) */
        }
    } else {
        if (vk) {
            sb_add(h, "layout(location = 0) in vec4 vo_D0;\nlayout(location = 1) in vec4 vo_D1;\n");
            for (int i = 0; i < VT3_SM2_MAX_TEXCOORD; i++) sb_add(h, "layout(location = %d) in vec4 vo_T%d;\n", 2 + i, i);
            sb_add(h, "layout(std140, set = 0, binding = 1) uniform PsConsts {\n"
                      "  vec4 psc[256];\n  ivec4 psi[16];\n  ivec4 psb4[4];\n"
                      "  int vt3_at_func;\n  float vt3_at_ref;\n};\n"
                      "bool vt3_psb(int n) { return psb4[n >> 2][n & 3] != 0; }\n");
        } else {
            sb_add(h, "in vec4 vo_D0;\nin vec4 vo_D1;\n");
            for (int i = 0; i < VT3_SM2_MAX_TEXCOORD; i++) sb_add(h, "in vec4 vo_T%d;\n", i);
            sb_add(h, "uniform vec4 psc[256];\nuniform ivec4 psi[16];\nuniform bool psb[16];\n");
            sb_add(h, "uniform int vt3_at_func;\nuniform float vt3_at_ref;\n");
        }
        /* Alpha test (D3DRS_ALPHAFUNC and D3DRS_ALPHAREF), applied at the end of main. 0 = off. */
        sb_add(h, "bool vt3_alpha_pass(float a) {\n"
                  "  float v = floor(a * 255.0 + 0.5);\n"
                  "  if (vt3_at_func == 1) return false;\n"
                  "  if (vt3_at_func == 2) return v < vt3_at_ref;\n"
                  "  if (vt3_at_func == 3) return v == vt3_at_ref;\n"
                  "  if (vt3_at_func == 4) return v <= vt3_at_ref;\n"
                  "  if (vt3_at_func == 5) return v > vt3_at_ref;\n"
                  "  if (vt3_at_func == 6) return v != vt3_at_ref;\n"
                  "  if (vt3_at_func == 7) return v >= vt3_at_ref;\n"
                  "  return true;\n}\n");
        {
            uint32_t oc = c->oc_used ? c->oc_used : 1u;
            for (int i = 0; i < 4; i++)
                if (oc & (1u << i)) sb_add(h, vk ? "layout(location = %d) out vec4 oC%d;\n" : "out vec4 oC%d;\n", vk ? i : 0, i);
        }
    }
    for (int i = 0; i < VT3_SM2_MAX_SAMPLER; i++) {
        if (!(c->out->sampler_mask & (1u << i))) continue;
        const char *type = c->out->sampler_type[i] == 3 ? "samplerCube" : "sampler2D";
        if (vk) sb_add(h, "layout(set = 1, binding = %d) uniform %s s%d;\n", i, type, i);
        else sb_add(h, "uniform %s s%d;\n", type, i);
    }
    sb_add(h, "ivec4 a0;\nivec4 aL;\n");
    if (vs) sb_add(h, "vec4 vs_pos;\n");
    for (int i = 0; i < VT3_SM2_MAX_TEMP; i++) {
        if (c->temps_used & (1u << i)) sb_add(h, "vec4 r%d;\n", i);
    }
    /* D3D nrm normalises xyz only; w is left undefined. Zero length gives zero, not NaN. */
    sb_add(h, "vec4 vt3_nrm(vec4 v) {\n"
              "  float l2 = dot(v.xyz, v.xyz);\n"
              "  return vec4(l2 > 1e-30 ? v.xyz * inversesqrt(l2) : vec3(0.0), 0.0);\n"
              "}\n");
    sb_add(h, "\nvoid main() {\n");
    sb_add(h, "  a0 = ivec4(0);\n  aL = ivec4(0);\n");
    /* A D3D shader may read a temporary before it writes it, such as a skinning accumulator. Apple's GL
     * zeroes globals; SPIR-V on Metal and Adreno leaves them undefined, so zero them here. */
    for (int i = 0; i < VT3_SM2_MAX_TEMP; i++) {
        if (c->temps_used & (1u << i)) sb_add(h, "  r%d = vec4(0.0);\n", i);
    }
    if (vs) {
        /* Every vertex output is written, so the GL linker keeps it even when the shader does not write it. */
        sb_add(h, "  vs_pos = vec4(0.0, 0.0, 0.0, 1.0);\n  vo_D0 = vec4(0.0);\n  vo_D1 = vec4(0.0);\n");
        for (int i = 0; i < VT3_SM2_MAX_TEXCOORD; i++) sb_add(h, "  vo_T%d = vec4(0.0);\n", i);
    }
    else {
        uint32_t oc = c->oc_used ? c->oc_used : 1u;
        for (int i = 0; i < 4; i++)
            if (oc & (1u << i)) sb_add(h, "  oC%d = vec4(0.0);\n", i);
    }
    for (int i = 0; i < VT3_SM2_MAX_TEMP; i++) {
        if (c->temps_used & (1u << i)) sb_add(h, "  r%d = vec4(0.0);\n", i);
    }
}

int vt3_sm2_translate(const uint32_t *tokens, size_t ntokens, vt3_sm2_shader *out)
{
    return vt3_sm2_translate_ex(tokens, ntokens, out, 0);
}

int vt3_sm2_translate_ex(const uint32_t *tokens, size_t ntokens, vt3_sm2_shader *out, unsigned flags)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!tokens || ntokens < 2) {
        snprintf(out->error, sizeof out->error, "shader too short");
        return -1;
    }
    uint32_t v = tokens[0];
    uint32_t hi = v >> 16;
    if (hi == 0xFFFEu) out->kind = VT3_SM_VS;
    else if (hi == 0xFFFFu) out->kind = VT3_SM_PS;
    else {
        snprintf(out->error, sizeof out->error, "bad version token 0x%08x", v);
        return -1;
    }
    out->major = (int)((v >> 8) & 0xFFu);
    out->minor = (int)(v & 0xFFu);
    if (out->major != 2) {
        snprintf(out->error, sizeof out->error, "only SM2 is supported (got %d.%d)", out->major, out->minor);
        return -1;
    }

    ctx c;
    memset(&c, 0, sizeof c);
    c.t = tokens;
    c.nt = ntokens;
    c.out = out;
    c.vulkan = (flags & VT3_SM2_FLAG_VULKAN) != 0;
    for (int i = 0; i < VT3_SM2_MAX_ATTR; i++) c.in_loc[i] = -1;

    prescan_defs(&c);
    if (translate_walk(&c) != 0 || c.failed) {
        free(c.body.p);
        if (!out->error[0]) snprintf(out->error, sizeof out->error, "translation failed");
        return -1;
    }

    sb h;
    memset(&h, 0, sizeof h);
    emit_header(&c, &h);
    sb_add(&h, "%s", c.body.p ? c.body.p : "");
    if (out->kind == VT3_SM_VS) {
        if (c.vulkan) sb_add(&h, "  gl_Position = vec4(vs_pos.x, -vs_pos.y, vs_pos.z, vs_pos.w);\n");
        else sb_add(&h, "  gl_Position = vec4(vs_pos.x, vs_pos.y * vt3_flip, 2.0 * vs_pos.z - vs_pos.w, vs_pos.w);\n");
    }
    if (out->kind != VT3_SM_VS) sb_add(&h, "  if (vt3_at_func != 0 && !vt3_alpha_pass(oC0.a)) discard;\n");
    sb_add(&h, "}\n");
    free(c.body.p);
    if (h.oom) {
        free(h.p);
        snprintf(out->error, sizeof out->error, "out of memory");
        return -1;
    }
    out->glsl = h.p;
    return 0;
}
