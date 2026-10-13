/* vt3simd: MMX and SSE2 integer lane operations for recompiled code.
 *
 * One helper per operation class, with the lane width and operand width as
 * arguments. nbytes is 8 for MMX (vt_xmm with only u64[0] used) and 16 for SSE2.
 * Lane arithmetic is exact. Shift counts and saturation follow the Intel SDM.
 */
#include "vt3rt.h"

enum { PK_ADD, PK_SUB, PK_ADDS, PK_ADDUS, PK_SUBS, PK_SUBUS, PK_MULLO, PK_MULHI,
       PK_MULHIU, PK_AVG, PK_MAX, PK_MAXU, PK_MIN, PK_MINU, PK_EQ, PK_GT,
       PK_AND, PK_ANDN, PK_OR, PK_XOR };

static uint64_t lane_get(const vt_xmm *v, int bits, int i)
{
    switch (bits) {
    case 8: return v->b[i];
    case 16: return v->u16[i];
    case 32: return v->u32[i];
    default: return v->u64[i];
    }
}

static void lane_put(vt_xmm *v, int bits, int i, uint64_t x)
{
    switch (bits) {
    case 8: v->b[i] = (uint8_t)x; break;
    case 16: v->u16[i] = (uint16_t)x; break;
    case 32: v->u32[i] = (uint32_t)x; break;
    default: v->u64[i] = x; break;
    }
}

static int64_t sext(uint64_t x, int bits)
{
    if (bits == 64) return (int64_t)x;
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (int64_t)((x ^ m) - m);
}

static int64_t sat_s(int64_t x, int bits)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    return x > hi ? hi : x < lo ? lo : x;
}

static int64_t sat_u(int64_t x, int bits)
{
    int64_t hi = ((int64_t)1 << bits) - 1;
    return x > hi ? hi : x < 0 ? 0 : x;
}

void vt_pbinop(int op, int bits, vt_xmm *d, const vt_xmm *s, int nbytes)
{
    if (op == PK_AND || op == PK_ANDN || op == PK_OR || op == PK_XOR) {
        for (int k = 0; k < nbytes / 8; k++) {
            uint64_t a = d->u64[k], b = s->u64[k];
            d->u64[k] = op == PK_AND ? (a & b) : op == PK_ANDN ? (~a & b) :
                        op == PK_OR ? (a | b) : (a ^ b);
        }
        return;
    }
    int n = nbytes * 8 / bits;
    uint64_t mask = bits == 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
    for (int i = 0; i < n; i++) {
        uint64_t ua = lane_get(d, bits, i), ub = lane_get(s, bits, i);
        int64_t sa = sext(ua, bits), sb = sext(ub, bits);
        uint64_t r = 0;
        switch (op) {
        case PK_ADD: r = ua + ub; break;
        case PK_SUB: r = ua - ub; break;
        case PK_ADDS: r = (uint64_t)sat_s(sa + sb, bits); break;
        case PK_ADDUS: r = (uint64_t)sat_u((int64_t)ua + (int64_t)ub, bits); break;
        case PK_SUBS: r = (uint64_t)sat_s(sa - sb, bits); break;
        case PK_SUBUS: r = (uint64_t)sat_u((int64_t)ua - (int64_t)ub, bits); break;
        case PK_MULLO: r = (uint64_t)(sa * sb); break;
        case PK_MULHI: r = (uint64_t)((sa * sb) >> bits); break;
        case PK_MULHIU: r = (ua * ub) >> bits; break;
        case PK_AVG: r = (ua + ub + 1) >> 1; break;
        case PK_MAX: r = sa > sb ? ua : ub; break;
        case PK_MAXU: r = ua > ub ? ua : ub; break;
        case PK_MIN: r = sa < sb ? ua : ub; break;
        case PK_MINU: r = ua < ub ? ua : ub; break;
        case PK_EQ: r = ua == ub ? mask : 0; break;
        case PK_GT: r = sa > sb ? mask : 0; break;
        default: break;
        }
        lane_put(d, bits, i, r & mask);
    }
}

/* Shifts by a count. kind: 0 left logical, 1 right logical, 2 right arithmetic. */
void vt_pshift(int kind, int bits, vt_xmm *d, uint64_t count, int nbytes)
{
    int n = nbytes * 8 / bits;
    uint64_t mask = bits == 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
    for (int i = 0; i < n; i++) {
        uint64_t u = lane_get(d, bits, i);
        uint64_t r;
        if (kind == 2) {
            uint64_t c = count > (uint64_t)(bits - 1) ? (uint64_t)(bits - 1) : count;
            r = (uint64_t)(sext(u, bits) >> c);
        } else if (count >= (uint64_t)bits) {
            r = 0;
        } else if (kind == 0) {
            r = u << count;
        } else {
            r = u >> count;
        }
        lane_put(d, bits, i, r & mask);
    }
}

/* Interleave the low (high=0) or high (high=1) halves of d and s. */
void vt_punpck(int bits, int high, vt_xmm *d, const vt_xmm *s, int nbytes)
{
    int n = nbytes * 8 / bits, half = n / 2, base = high ? half : 0;
    vt_xmm a = *d, b = *s, r;
    memset(&r, 0, sizeof r);
    for (int j = 0; j < half; j++) {
        lane_put(&r, bits, 2 * j, lane_get(&a, bits, base + j));
        lane_put(&r, bits, 2 * j + 1, lane_get(&b, bits, base + j));
    }
    *d = r;
}

/* kind: 0 signed 32 to 16, 1 signed 16 to 8, 2 unsigned 16 to 8. Low half from d. */
void vt_ppack(int kind, vt_xmm *d, const vt_xmm *s, int nbytes)
{
    int inbits = kind == 0 ? 32 : 16, outbits = kind == 0 ? 16 : 8;
    int n = nbytes * 8 / inbits;
    vt_xmm a = *d, b = *s, r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        int64_t x = sext(lane_get(&a, inbits, i), inbits);
        int64_t y = sext(lane_get(&b, inbits, i), inbits);
        int64_t xv = kind == 2 ? sat_u(x, outbits) : sat_s(x, outbits);
        int64_t yv = kind == 2 ? sat_u(y, outbits) : sat_s(y, outbits);
        lane_put(&r, outbits, i, (uint64_t)xv);
        lane_put(&r, outbits, n + i, (uint64_t)yv);
    }
    *d = r;
}

void vt_pshufw(vt_xmm *d, const vt_xmm *s, int imm)
{
    vt_xmm r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 4; i++) r.u16[i] = s->u16[(imm >> (2 * i)) & 3];
    *d = r;
}

/* PSHUFLW (high=0) or PSHUFHW (high=1): shuffle one half of four words. */
void vt_pshufhalf(vt_xmm *d, const vt_xmm *s, int imm, int high)
{
    vt_xmm r = *s;
    int base = high ? 4 : 0;
    for (int i = 0; i < 4; i++) r.u16[base + i] = s->u16[base + ((imm >> (2 * i)) & 3)];
    *d = r;
}

uint32_t vt_pextrw(const vt_xmm *s, int idx, int nbytes)
{
    int n = nbytes / 2;
    return s->u16[idx & (n - 1)];
}

void vt_pinsrw(vt_xmm *d, uint32_t v, int idx, int nbytes)
{
    int n = nbytes / 2;
    d->u16[idx & (n - 1)] = (uint16_t)v;
}

uint32_t vt_pmovmskb(const vt_xmm *s, int nbytes)
{
    uint32_t m = 0;
    for (int i = 0; i < nbytes; i++) m |= (uint32_t)((s->b[i] >> 7) & 1u) << i;
    return m;
}

/* SSE compare predicates. pred: 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT, 6 NLE, 7 ORD.
 * dbl selects double lanes. scalar compares only lane 0 and keeps the rest of d. */
static int cmp_pred(int pred, double a, double b)
{
    int unord = isnan(a) || isnan(b);
    switch (pred) {
    case 0: return !unord && a == b;
    case 1: return !unord && a < b;
    case 2: return !unord && a <= b;
    case 3: return unord;
    case 4: return unord || a != b;
    case 5: return unord || !(a < b);
    case 6: return unord || !(a <= b);
    default: return !unord;
    }
}

void vt_cmpf(int pred, int dbl, int scalar, vt_xmm *d, const vt_xmm *s)
{
    int n = dbl ? 2 : 4;
    int lanes = scalar ? 1 : n;
    for (int i = 0; i < lanes; i++) {
        if (dbl) {
            double a = d->f64[i], b = s->f64[i];
            d->u64[i] = cmp_pred(pred, a, b) ? ~(uint64_t)0 : 0;
        } else {
            float a = d->f32[i], b = s->f32[i];
            d->u32[i] = cmp_pred(pred, a, b) ? 0xFFFFFFFFu : 0;
        }
    }
}

uint32_t vt_cvt_packed_i32(double v, int trunc)
{
    return vt_cvt_i32(v, trunc);
}

/* CPUID reports the features this runtime provides: the x87 FPU only. The game then
 * takes its non-SIMD paths. */
void vt_cpuid(vt3_cpu *c)
{
    uint32_t leaf = c->eax;
    if (leaf == 0) {
        c->eax = 1;
        c->ebx = 0x756E6547u; /* "Genu" */
        c->edx = 0x49656E69u; /* "ineI" */
        c->ecx = 0x6C65746Eu; /* "ntel" */
    } else if (leaf == 1) {
        c->eax = 0x00000612u;
        c->ebx = 0;
        c->ecx = 0;
        c->edx = 0x00000001u; /* FPU only */
    } else {
        c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

/* FNSTENV / FLDENV, 28 bytes. FNSAVE / FRSTOR, 108 bytes: the environment followed by
 * the eight registers in ST(0)..ST(7) order, each as an 80-bit value. */
void vt_fnstenv(vt3_cpu *c, uint32_t a)
{
    wr16(a + 0, c->fcw);
    wr16(a + 4, (c->fsw & 0xC7FFu) | ((c->ftop & 7u) << 11));
    wr16(a + 8, 0xFFFFu); /* tag word: all empty */
    for (uint32_t k = 12; k < 28; k += 4) wr32(a + k, 0);
}

void vt_fldenv(vt3_cpu *c, uint32_t a)
{
    c->fcw = rd16(a + 0);
    uint32_t sw = rd16(a + 4);
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7u;
}

void vt_fnsave(vt3_cpu *c, uint32_t a)
{
    vt_fnstenv(c, a);
    for (int i = 0; i < 8; i++) vt_fst80(a + 28 + 10 * (uint32_t)i, FST(i));
    /* FNSAVE leaves the FPU initialised. */
    c->fcw = 0x37Fu;
    c->fsw = 0;
    c->ftop = 0;
}

void vt_frstor(vt3_cpu *c, uint32_t a)
{
    vt_fldenv(c, a);
    for (int i = 0; i < 8; i++) FST(i) = vt_fld80(a + 28 + 10 * (uint32_t)i);
}

/* PMADDWD: multiply signed words, add adjacent pairs into signed dwords. */
void vt_pmaddwd(vt_xmm *d, const vt_xmm *s, int nbytes)
{
    vt_xmm a = *d, b = *s, r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < nbytes / 4; i++) {
        int32_t p0 = (int32_t)(int16_t)a.u16[2 * i] * (int32_t)(int16_t)b.u16[2 * i];
        int32_t p1 = (int32_t)(int16_t)a.u16[2 * i + 1] * (int32_t)(int16_t)b.u16[2 * i + 1];
        r.u32[i] = (uint32_t)(p0 + p1);
    }
    *d = r;
}
