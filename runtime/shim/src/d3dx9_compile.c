/*
 * HLSL to SM2 compiler for the subset the game's debug shaders use, and the ID3DXBuffer
 * object that carries the result to the guest. Host-only compile; no guest memory is
 * touched until the buffer is created.
 *
 * Subset: struct declarations with semantics; one global `matrix`; one entry point with
 * one struct parameter; in a vertex shader, `Out.Member[.mask] = expr;` and a local of the
 * output struct; in a pixel shader, `return expr;`. Expressions: one of `a + b`, `a - b`,
 * `a * b`, `mul(vector, matrix)`, or a single operand. Operands are members of the input
 * struct (with swizzles), matrix rows, and float literals. Anything else is refused with
 * the line number.
 *
 * Register rules follow D3DX: vertex inputs take v# in declaration order (every member);
 * pixel inputs take v# in declaration order, skipping POSITION; vertex outputs are oPos,
 * oD# and oT# by semantic; the pixel result is oC0; a matrix occupies four c# registers
 * (column-major, so mul(v, M) is four dp4 against c#, c#+1, ...); float literals become
 * def constants after the globals. Only the inputs a body reads get dcl declarations.
 */
#include "d3dx9_compile.h"
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAMELEN 48
#define MAX_TOK 4096
#define MAX_CODE 2048
#define MAX_STRUCT 8
#define MAX_MEMBER 12
#define MAX_GLOBAL 8
#define MAX_FUNC 16
#define MAX_LIT 32
#define MAX_CREG 256

enum { TK_IDENT, TK_NUM, TK_PUNCT, TK_EOF };
enum { OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MUL = 5, OP_DP4 = 9, OP_DCL = 31, OP_DEF = 81 };
enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_RASTOUT = 4, RT_ATTROUT = 5,
       RT_TEXCRDOUT = 6, RT_COLOROUT = 8 };
enum { USE_POSITION = 0, USE_TEXCOORD = 5, USE_COLOR = 10 };   /* D3DDECLUSAGE values */

typedef struct { int kind; char s[NAMELEN]; float f; int line; } tok;
typedef struct { char name[NAMELEN]; int width; int usage, uindex; } member;
typedef struct { char name[NAMELEN]; int nm; member m[MAX_MEMBER]; } sdef;
typedef struct { char name[NAMELEN]; int creg; } gvar;
typedef struct {
    char name[NAMELEN], ret[NAMELEN], ptype[NAMELEN], pname[NAMELEN];
    int np, has_ret_sem, ret_usage, body;
} fdef;
typedef struct { int type, idx, width, matrix; uint8_t swz[4]; } opnd;
/* kind: 0 a single operand, 1 a binary + - *, 2 mul(vector, matrix). */
typedef struct { int kind; char op; opnd a, b; } expr;

typedef struct {
    tok t[MAX_TOK];
    int nt, p;
    char err[160];
    int err_line;
    sdef st[MAX_STRUCT]; int nst;
    gvar gv[MAX_GLOBAL]; int ngv;
    fdef fn[MAX_FUNC]; int nfn;
    int next_creg;
    /* the entry point being compiled */
    int vs, in_st, ret_st, has_local, next_v, over;
    char local[NAMELEN], pname[NAMELEN];
    int in_reg[MAX_MEMBER], in_used[MAX_MEMBER];
    int out_type[MAX_MEMBER], out_idx[MAX_MEMBER];
    uint32_t code[MAX_CODE]; int ncode;
    uint32_t lit_bits[MAX_LIT]; int lit_creg[MAX_LIT]; int nlit;
} cc;

static const uint8_t k_identity[4] = { 0, 1, 2, 3 };

/* ---- errors and tokens ------------------------------------------------------ */

static int err(cc *c, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->err_line = line;
    return -1;
}

static void setname(char *d, const char *s)
{
    size_t n = strlen(s);
    if (n >= NAMELEN) n = NAMELEN - 1;
    memcpy(d, s, n);
    d[n] = 0;
}

static int lex(cc *c, const char *src, size_t len)
{
    size_t i = 0;
    int line = 1;
    c->nt = 0;
    for (;;) {
        for (;;) {
            if (i >= len) break;
            unsigned char ch = (unsigned char)src[i];
            if (ch == '\n') {
                line++;
                i++;
            } else if (isspace(ch)) {
                i++;
            } else if (ch == '/' && i + 1 < len && src[i + 1] == '/') {
                while (i < len && src[i] != '\n') i++;
            } else if (ch == '/' && i + 1 < len && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                    if (src[i] == '\n') line++;
                    i++;
                }
                i += 2;
                if (i > len) i = len;
            } else {
                break;
            }
        }
        if (c->nt >= MAX_TOK - 1) return err(c, line, "source is too long");
        tok *t = &c->t[c->nt++];
        t->line = line;
        t->f = 0.0f;
        t->s[0] = 0;
        if (i >= len) {
            t->kind = TK_EOF;
            return 0;
        }
        unsigned char ch = (unsigned char)src[i];
        if (isalpha(ch) || ch == '_') {
            size_t n = 0;
            while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
                if (n < NAMELEN - 1) t->s[n++] = src[i];
                i++;
            }
            t->s[n] = 0;
            t->kind = TK_IDENT;
        } else if (isdigit(ch) || (ch == '.' && i + 1 < len && isdigit((unsigned char)src[i + 1]))) {
            size_t n = 0;
            while (i < len && (isdigit((unsigned char)src[i]) || src[i] == '.')) {
                if (n < NAMELEN - 2) t->s[n++] = src[i];
                i++;
            }
            if (i < len && (src[i] == 'f' || src[i] == 'F')) i++;
            t->s[n] = 0;
            t->kind = TK_NUM;
            t->f = strtof(t->s, NULL);
        } else {
            t->s[0] = src[i++];
            t->s[1] = 0;
            t->kind = TK_PUNCT;
        }
    }
}

static const tok *cur(const cc *c) { return &c->t[c->p]; }

static int is_id(const cc *c, const char *s)
{
    return cur(c)->kind == TK_IDENT && !strcmp(cur(c)->s, s);
}

static int is_p(const cc *c, char ch)
{
    return cur(c)->kind == TK_PUNCT && cur(c)->s[0] == ch;
}

static int expect_p(cc *c, char ch)
{
    if (!is_p(c, ch)) return err(c, cur(c)->line, "expected '%c' before '%s'", ch, cur(c)->s);
    c->p++;
    return 0;
}

static int expect_id(cc *c, char *dst)
{
    if (cur(c)->kind != TK_IDENT) return err(c, cur(c)->line, "expected a name before '%s'", cur(c)->s);
    setname(dst, cur(c)->s);
    c->p++;
    return 0;
}

/* ---- declarations ----------------------------------------------------------- */

/* >0: scalar or vector width (matrix sets *mat and returns 4). -2: a declared struct,
 * index in *sidx. 0: unknown. */
static int type_width(const cc *c, const char *name, int *mat, int *sidx)
{
    *mat = 0;
    *sidx = -1;
    if (!strcmp(name, "float")) return 1;
    if (!strcmp(name, "float2")) return 2;
    if (!strcmp(name, "float3")) return 3;
    if (!strcmp(name, "float4")) return 4;
    if (!strcmp(name, "matrix") || !strcmp(name, "float4x4")) {
        *mat = 1;
        return 4;
    }
    for (int i = 0; i < c->nst; i++)
        if (!strcmp(c->st[i].name, name)) {
            *sidx = i;
            return -2;
        }
    return 0;
}

/* D3DDECLUSAGE for a semantic such as POSITION, POSITION1, COLOR0 or TEXCOORD2. -1 if unknown. */
static int sem_usage(const char *s, int *uidx)
{
    static const char *const names[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                         "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                                         "COLOR", "FOG", "DEPTH", "SAMPLE" };
    static const int codes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    size_t n = 0;
    while (s[n] && (isalpha((unsigned char)s[n]) || s[n] == '_')) n++;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (strlen(names[k]) == n && !strncmp(s, names[k], n)) {
            *uidx = (s[n] >= '0' && s[n] <= '9') ? atoi(s + n) : 0;
            return codes[k];
        }
    return -1;
}

static int find_member(const sdef *s, const char *name)
{
    for (int i = 0; i < s->nm; i++)
        if (!strcmp(s->m[i].name, name)) return i;
    return -1;
}

static int parse_struct(cc *c)
{
    c->p++;   /* struct */
    if (c->nst >= MAX_STRUCT) return err(c, cur(c)->line, "too many structs");
    sdef *s = &c->st[c->nst];
    s->nm = 0;
    if (expect_id(c, s->name) < 0 || expect_p(c, '{') < 0) return -1;
    while (!is_p(c, '}')) {
        if (cur(c)->kind == TK_EOF) return err(c, cur(c)->line, "unterminated struct");
        char tname[NAMELEN];
        int mat, sidx;
        if (expect_id(c, tname) < 0) return -1;
        int w = type_width(c, tname, &mat, &sidx);
        if (w <= 0 || mat) return err(c, cur(c)->line, "unsupported member type '%s'", tname);
        if (s->nm >= MAX_MEMBER) return err(c, cur(c)->line, "too many members");
        member *m = &s->m[s->nm];
        m->width = w;
        m->usage = -1;
        m->uindex = 0;
        if (expect_id(c, m->name) < 0) return -1;
        if (is_p(c, ':')) {
            char sem[NAMELEN];
            c->p++;
            if (expect_id(c, sem) < 0) return -1;
            m->usage = sem_usage(sem, &m->uindex);
            if (m->usage < 0) return err(c, cur(c)->line, "unsupported semantic '%s'", sem);
        }
        if (expect_p(c, ';') < 0) return -1;
        s->nm++;
    }
    c->p++;   /* } */
    if (expect_p(c, ';') < 0) return -1;
    c->nst++;
    return 0;
}

static int parse_func(cc *c, const char *ret, const char *name)
{
    if (c->nfn >= MAX_FUNC) return err(c, cur(c)->line, "too many functions");
    fdef *f = &c->fn[c->nfn];
    memset(f, 0, sizeof *f);
    setname(f->name, name);
    setname(f->ret, ret);
    f->ret_usage = -1;
    c->p++;   /* ( */
    if (!is_p(c, ')')) {
        char sem[NAMELEN];
        if (expect_id(c, f->ptype) < 0 || expect_id(c, f->pname) < 0) return -1;
        if (is_p(c, ':')) {   /* a semantic on a struct parameter is not needed */
            c->p++;
            if (expect_id(c, sem) < 0) return -1;
        }
        f->np = 1;
        if (is_p(c, ',')) return err(c, cur(c)->line, "functions with more than one parameter are not supported");
    }
    if (expect_p(c, ')') < 0) return -1;
    if (is_p(c, ':')) {
        char sem[NAMELEN];
        int uidx = 0;
        c->p++;
        if (expect_id(c, sem) < 0) return -1;
        f->ret_usage = sem_usage(sem, &uidx);
        f->has_ret_sem = 1;
    }
    if (expect_p(c, '{') < 0) return -1;
    f->body = c->p;
    int depth = 1;
    while (depth > 0) {
        if (cur(c)->kind == TK_EOF) return err(c, cur(c)->line, "unterminated function '%s'", name);
        if (is_p(c, '{')) depth++;
        else if (is_p(c, '}')) depth--;
        c->p++;
    }
    c->nfn++;
    return 0;
}

static int parse_top(cc *c)
{
    while (cur(c)->kind != TK_EOF) {
        if (is_id(c, "struct")) {
            if (parse_struct(c) < 0) return -1;
            continue;
        }
        char tname[NAMELEN], name[NAMELEN];
        int mat, sidx;
        if (expect_id(c, tname) < 0 || expect_id(c, name) < 0) return -1;
        if (is_p(c, '(')) {
            if (parse_func(c, tname, name) < 0) return -1;
            continue;
        }
        if (type_width(c, tname, &mat, &sidx) != 4 || !mat)
            return err(c, cur(c)->line, "unsupported global '%s'", name);
        if (expect_p(c, ';') < 0) return -1;
        if (c->ngv >= MAX_GLOBAL || c->next_creg + 4 > MAX_CREG)
            return err(c, cur(c)->line, "too many matrices");
        setname(c->gv[c->ngv].name, name);
        c->gv[c->ngv].creg = c->next_creg;
        c->next_creg += 4;
        c->ngv++;
    }
    return 0;
}

/* ---- operands and expressions ------------------------------------------------ */

static int swz_letter(char ch)
{
    switch (ch) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    default: return -1;
    }
}

/* A swizzle sets the operand's width and its per-component source pattern. A scalar
 * swizzle is replicated across all four components. */
static int parse_swizzle(cc *c, opnd *o)
{
    const char *s = cur(c)->s;
    size_t n = strlen(s);
    if (cur(c)->kind != TK_IDENT || n < 1 || n > 4) return err(c, cur(c)->line, "bad swizzle '%s'", s);
    int v[4];
    for (size_t j = 0; j < n; j++) {
        v[j] = swz_letter(s[j]);
        if (v[j] < 0) return err(c, cur(c)->line, "bad swizzle '%s'", s);
    }
    for (int j = 0; j < 4; j++) o->swz[j] = (uint8_t)(j < (int)n ? v[j] : v[n - 1]);
    o->width = (int)n;
    c->p++;
    return 0;
}

/* A write mask from letters such as xy. The mask is a set of components, not an order. */
static int parse_mask(cc *c, uint32_t *mask)
{
    const char *s = cur(c)->s;
    size_t n = strlen(s);
    if (cur(c)->kind != TK_IDENT || n < 1 || n > 4) return err(c, cur(c)->line, "bad write mask '%s'", s);
    uint32_t m = 0;
    for (size_t j = 0; j < n; j++) {
        int v = swz_letter(s[j]);
        if (v < 0) return err(c, cur(c)->line, "bad write mask '%s'", s);
        m |= 1u << v;
    }
    *mask = m;
    c->p++;
    return 0;
}

/* A constant register holding v (all four components), allocated on first use. */
static int lit_const(cc *c, float v, int line, int *creg)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    for (int i = 0; i < c->nlit; i++)
        if (c->lit_bits[i] == bits) {
            *creg = c->lit_creg[i];
            return 0;
        }
    if (c->nlit >= MAX_LIT || c->next_creg >= MAX_CREG) return err(c, line, "too many constants");
    c->lit_bits[c->nlit] = bits;
    c->lit_creg[c->nlit] = c->next_creg++;
    *creg = c->lit_creg[c->nlit];
    c->nlit++;
    return 0;
}

static int parse_operand(cc *c, opnd *o)
{
    memset(o, 0, sizeof *o);
    for (int j = 0; j < 4; j++) o->swz[j] = (uint8_t)j;
    if (cur(c)->kind == TK_NUM) {
        int creg;
        if (lit_const(c, cur(c)->f, cur(c)->line, &creg) < 0) return -1;
        o->type = RT_CONST;
        o->idx = creg;
        o->width = 1;
        o->swz[0] = o->swz[1] = o->swz[2] = o->swz[3] = 0;
        c->p++;
        return 0;
    }
    char name[NAMELEN];
    if (expect_id(c, name) < 0) return -1;
    if (!is_p(c, '.')) {   /* a global matrix */
        for (int i = 0; i < c->ngv; i++)
            if (!strcmp(c->gv[i].name, name)) {
                o->type = RT_CONST;
                o->idx = c->gv[i].creg;
                o->width = 4;
                o->matrix = 1;
                return 0;
            }
        return err(c, cur(c)->line, "unknown name '%s'", name);
    }
    if (strcmp(name, c->pname)) return err(c, cur(c)->line, "unknown name '%s'", name);
    c->p++;   /* . */
    char mem[NAMELEN];
    if (expect_id(c, mem) < 0) return -1;
    const sdef *s = &c->st[c->in_st];
    int m = find_member(s, mem);
    if (m < 0) return err(c, cur(c)->line, "no member '%s'", mem);
    if (c->in_reg[m] < 0)
        return err(c, cur(c)->line, "'%s.%s' is not an input of this shader stage", name, mem);
    c->in_used[m] = 1;
    o->type = RT_INPUT;
    o->idx = c->in_reg[m];
    o->width = s->m[m].width;
    if (is_p(c, '.')) {
        c->p++;
        if (parse_swizzle(c, o) < 0) return -1;
    }
    return 0;
}

static int parse_expr(cc *c, expr *e)
{
    memset(e, 0, sizeof *e);
    if (is_id(c, "mul")) {
        c->p++;
        if (expect_p(c, '(') < 0 || parse_operand(c, &e->a) < 0 || expect_p(c, ',') < 0 ||
            parse_operand(c, &e->b) < 0 || expect_p(c, ')') < 0)
            return -1;
        e->kind = 2;
        return 0;
    }
    if (parse_operand(c, &e->a) < 0) return -1;
    if (is_p(c, '+') || is_p(c, '-') || is_p(c, '*')) {
        e->op = cur(c)->s[0];
        c->p++;
        if (parse_operand(c, &e->b) < 0) return -1;
        e->kind = 1;
    }
    return 0;
}

/* ---- code ------------------------------------------------------------------- */

/* Register token: bit 31 set, type split over bits 28-30 and 11-12, index in bits 0-10. */
static uint32_t reg_tok(int type, uint32_t idx)
{
    return 0x80000000u | ((uint32_t)(type & 7) << 28) | ((uint32_t)((type >> 3) & 3) << 11) | (idx & 0x7FFu);
}

static uint32_t dst_tok(int type, uint32_t idx, uint32_t mask)
{
    return reg_tok(type, idx) | (mask << 16);
}

static uint32_t src_tok(int type, uint32_t idx, const uint8_t swz[4])
{
    uint32_t s = swz[0] | ((uint32_t)swz[1] << 2) | ((uint32_t)swz[2] << 4) | ((uint32_t)swz[3] << 6);
    return reg_tok(type, idx) | (s << 16);
}

static void emit(cc *c, uint32_t t)
{
    if (c->ncode >= MAX_CODE) {
        c->over = 1;
        return;
    }
    c->code[c->ncode++] = t;
}

static void emit_insn(cc *c, uint32_t op, uint32_t nops)
{
    emit(c, op | (nops << 24));
}

/* A masked write takes the source components in order: the k-th written component reads
 * the k-th source component. Scalars are replicated, so they pass through unchanged. */
static void remap(const uint8_t in[4], uint32_t mask, uint8_t out[4])
{
    int k = 0;
    for (int j = 0; j < 4; j++) {
        if (mask & (1u << j)) out[j] = in[k++];
        else out[j] = in[j];
    }
}

static int emit_expr(cc *c, const expr *e, int dtype, int didx, uint32_t mask, int line)
{
    uint32_t dst = dst_tok(dtype, (uint32_t)didx, mask);
    if (e->kind == 0) {
        uint8_t s[4];
        if (e->a.matrix) return err(c, line, "a matrix cannot be assigned to a vector");
        remap(e->a.swz, mask, s);
        emit_insn(c, OP_MOV, 2);
        emit(c, dst);
        emit(c, src_tok(e->a.type, (uint32_t)e->a.idx, s));
    } else if (e->kind == 1) {
        uint8_t sa[4], sb[4];
        if (e->a.matrix || e->b.matrix) return err(c, line, "matrix arithmetic is not supported");
        remap(e->a.swz, mask, sa);
        remap(e->b.swz, mask, sb);
        emit_insn(c, e->op == '+' ? OP_ADD : e->op == '-' ? OP_SUB : OP_MUL, 3);
        emit(c, dst);
        emit(c, src_tok(e->a.type, (uint32_t)e->a.idx, sa));
        emit(c, src_tok(e->b.type, (uint32_t)e->b.idx, sb));
    } else {
        /* mul(vector, matrix): one dp4 per written component, against column j. */
        if (!e->b.matrix || e->a.matrix || e->a.width != 4)
            return err(c, line, "mul needs a float4 and a float4x4");
        for (int j = 0; j < 4; j++) {
            if (!(mask & (1u << j))) continue;
            emit_insn(c, OP_DP4, 3);
            emit(c, dst_tok(dtype, (uint32_t)didx, 1u << j));
            emit(c, src_tok(e->a.type, (uint32_t)e->a.idx, e->a.swz));
            emit(c, src_tok(RT_CONST, (uint32_t)(e->b.idx + j), k_identity));
        }
    }
    if (c->over) return err(c, line, "shader is too large");
    return 0;
}

static int stmt(cc *c)
{
    int line = cur(c)->line;
    if (is_id(c, "return")) {
        expr e;
        c->p++;
        if (c->vs) {   /* the output struct is already written; return is a no-op */
            if (cur(c)->kind != TK_IDENT || !c->has_local || strcmp(cur(c)->s, c->local))
                return err(c, line, "a vertex shader must return its output struct");
            c->p++;
            return expect_p(c, ';');
        }
        if (parse_expr(c, &e) < 0 || expect_p(c, ';') < 0) return -1;
        return emit_expr(c, &e, RT_COLOROUT, 0, 0xFu, line);
    }
    if (cur(c)->kind != TK_IDENT) return err(c, line, "unexpected '%s'", cur(c)->s);
    if (c->p + 1 < c->nt && c->t[c->p + 1].kind == TK_IDENT) {   /* local: TYPE NAME ; */
        char tname[NAMELEN], lname[NAMELEN];
        int mat, sidx;
        if (expect_id(c, tname) < 0 || expect_id(c, lname) < 0) return -1;
        type_width(c, tname, &mat, &sidx);
        if (!c->vs || sidx < 0 || sidx != c->ret_st || c->has_local)
            return err(c, line, "only the output struct may be declared, once");
        setname(c->local, lname);
        c->has_local = 1;
        return expect_p(c, ';');
    }
    /* assignment: NAME . MEMBER [. MASK] = expr ; */
    if (!c->vs) return err(c, line, "a pixel shader may only return a value");
    char name[NAMELEN], mem[NAMELEN];
    if (expect_id(c, name) < 0 || expect_p(c, '.') < 0 || expect_id(c, mem) < 0) return -1;
    if (!c->has_local || strcmp(name, c->local)) return err(c, line, "'%s' is not writable", name);
    const sdef *o = &c->st[c->ret_st];
    int m = find_member(o, mem);
    if (m < 0) return err(c, line, "no output member '%s'", mem);
    uint32_t mask = (1u << o->m[m].width) - 1u;
    if (is_p(c, '.')) {
        c->p++;
        if (parse_mask(c, &mask) < 0) return -1;
    }
    expr e;
    if (expect_p(c, '=') < 0 || parse_expr(c, &e) < 0 || expect_p(c, ';') < 0) return -1;
    return emit_expr(c, &e, c->out_type[m], c->out_idx[m], mask, line);
}

static int compile_entry(cc *c, const char *entry, const char *profile)
{
    int fi = -1;
    for (int i = 0; i < c->nfn; i++)
        if (!strcmp(c->fn[i].name, entry)) fi = i;
    if (fi < 0) return err(c, 0, "entry point '%s' is not defined in the source", entry);
    if (!strcmp(profile, "vs_2_0")) c->vs = 1;
    else if (!strcmp(profile, "ps_2_0")) c->vs = 0;
    else return err(c, 0, "profile '%s' is not supported (vs_2_0 and ps_2_0 only)", profile);
    const fdef *f = &c->fn[fi];
    if (f->np != 1) return err(c, 0, "'%s' must take one input struct", entry);

    int mat, isidx;
    if (type_width(c, f->ptype, &mat, &isidx) != -2) return err(c, 0, "the input of '%s' must be a struct", entry);
    c->in_st = isidx;
    setname(c->pname, f->pname);
    const sdef *in = &c->st[isidx];
    c->next_v = 0;
    for (int m = 0; m < in->nm; m++) {
        c->in_reg[m] = -1;
        if (c->vs || in->m[m].usage != USE_POSITION) c->in_reg[m] = c->next_v++;
    }

    if (c->vs) {
        int rs;
        if (type_width(c, f->ret, &mat, &rs) != -2) return err(c, 0, "a vertex shader must return a struct");
        c->ret_st = rs;
        const sdef *o = &c->st[rs];
        for (int m = 0; m < o->nm; m++) {
            const member *mm = &o->m[m];
            switch (mm->usage) {
            case USE_POSITION: c->out_type[m] = RT_RASTOUT; c->out_idx[m] = 0; break;
            case USE_COLOR:    c->out_type[m] = RT_ATTROUT; c->out_idx[m] = mm->uindex; break;
            case USE_TEXCOORD: c->out_type[m] = RT_TEXCRDOUT; c->out_idx[m] = mm->uindex; break;
            default: return err(c, 0, "output '%s' has a semantic that is not supported", mm->name);
            }
        }
    } else {
        if (strcmp(f->ret, "float4") || !f->has_ret_sem || f->ret_usage != USE_COLOR)
            return err(c, 0, "a pixel shader must return float4 : COLOR");
        c->ret_st = -1;
    }

    c->p = f->body;
    while (!is_p(c, '}')) {
        if (cur(c)->kind == TK_EOF) return err(c, cur(c)->line, "missing '}'");
        if (stmt(c) < 0) return -1;
    }
    return 0;
}

static int assemble(cc *c, uint32_t **out, uint32_t *nout)
{
    const sdef *in = &c->st[c->in_st];
    uint32_t ndcl = 0;
    for (int m = 0; m < in->nm; m++) {
        if (!c->in_used[m]) continue;
        if (in->m[m].usage < 0) return err(c, 0, "input '%s' is read but has no semantic", in->m[m].name);
        ndcl++;
    }
    /* a dcl is 3 tokens, a def is 6 (opcode, destination, four values), an instruction is its own length */
    uint32_t total = 1 + 3 * ndcl + 6 * (uint32_t)c->nlit + (uint32_t)c->ncode + 1;
    uint32_t *t = calloc(total, sizeof *t);
    if (!t) return err(c, 0, "out of memory");
    uint32_t n = 0;
    t[n++] = c->vs ? 0xFFFE0200u : 0xFFFF0200u;
    for (int m = 0; m < in->nm; m++) {
        if (!c->in_used[m]) continue;
        const member *mm = &in->m[m];
        t[n++] = (2u << 24) | OP_DCL;
        t[n++] = 0x80000000u | (uint32_t)mm->usage | ((uint32_t)mm->uindex << 16);
        t[n++] = dst_tok(RT_INPUT, (uint32_t)c->in_reg[m], 0xFu);
    }
    for (int i = 0; i < c->nlit; i++) {
        t[n++] = (5u << 24) | OP_DEF;
        t[n++] = dst_tok(RT_CONST, (uint32_t)c->lit_creg[i], 0xFu);
        for (int k = 0; k < 4; k++) t[n++] = c->lit_bits[i];
    }
    memcpy(t + n, c->code, (size_t)c->ncode * sizeof *t);
    n += (uint32_t)c->ncode;
    t[n++] = 0x0000FFFFu;
    *out = t;
    *nout = n;
    return 0;
}

int d3dx_compile_hlsl(const char *src, size_t len, const char *entry, const char *profile,
                      uint32_t **out, uint32_t *nout, char *msg, size_t msgcap)
{
    cc *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(msg, msgcap, "out of memory");
        return -1;
    }
    int r = lex(c, src, len);
    if (r == 0) r = parse_top(c);
    if (r == 0) r = compile_entry(c, entry, profile);
    if (r == 0) r = assemble(c, out, nout);
    if (r < 0) {
        if (c->err_line > 0) snprintf(msg, msgcap, "line %d: %s", c->err_line, c->err);
        else snprintf(msg, msgcap, "%s", c->err);
    }
    free(c);
    return r;
}

/* ---- ID3DXBuffer ------------------------------------------------------------ */

typedef struct d3dx_buf {
    GPTR storage;   /* guest heap copy of the bytes */
    uint32_t size;
} d3dx_buf;

static d3dx_buf *buf_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_D3DXBUFFER);
    return h ? h->data : NULL;
}

uint32_t d3dx_buffer_new(const void *bytes, uint32_t nbytes)
{
    d3dx_buf *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nbytes ? nbytes : 1);
    if (!b->storage) {
        free(b);
        return 0;
    }
    void *dst = d3d9_gmem(b->storage, nbytes);
    if (dst && bytes) memcpy(dst, bytes, nbytes);
    b->size = nbytes;
    GPTR obj = d3d9_obj_new(D3D9_KIND_D3DXBUFFER, b, 0);
    if (!obj) {
        HeapFree(GetProcessHeap(), 0, b->storage);
        free(b);
    }
    return obj;
}

GPTR WINAPI ID3DXBuffer_GetBufferPointer(GPTR This)
{
    d3dx_buf *b = buf_of(This);
    return b ? b->storage : 0;
}

DWORD WINAPI ID3DXBuffer_GetBufferSize(GPTR This)
{
    d3dx_buf *b = buf_of(This);
    return b ? b->size : 0;
}

void d3dx_obj_free(uint32_t kind, void *data)
{
    if (kind == D3D9_KIND_D3DXBUFFER && data) {
        d3dx_buf *b = data;
        if (b->storage) HeapFree(GetProcessHeap(), 0, b->storage);
    }
    free(data);
}
