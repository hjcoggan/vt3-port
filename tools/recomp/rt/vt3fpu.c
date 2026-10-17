/* vt3fpu: x87 extended precision as a soft float. See vt3fpu.h for the model.
 *
 * Each operation computes an exact or sticky-exact intermediate in 64+64 bits, and
 * pack_round rounds it once to the target precision. The rounding rules are those of the
 * x87: nearest-even, toward -inf, toward +inf, toward zero. Denormal results are shifted
 * with truncation, which is the one known approximation outside double-precision range.
 */
#include "vt3fpu.h"
#include "vt3rt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

#define CLS_ZERO 0
#define CLS_FIN 1
#define CLS_INF 2
#define CLS_NAN 3

#define EXP_MAX 16383
#define EXP_MIN (-16382)

static const vt_x80 VX_NAN_INDEF = {0xC000000000000000ull, 0xFFFF};

static double vt_to_double_rc(vt_x80 a, uint32_t rc);

/* Decode to sign, unbiased exponent E, and significand m with bit 63 set (value m * 2^(E-63)). */
static int dec(vt_x80 x, int *neg, int32_t *E, uint64_t *m)
{
    *neg = x.se >> 15;
    uint32_t ex = x.se & 0x7FFFu;
    if (ex == 0x7FFFu) return (x.m << 1) == 0 ? CLS_INF : CLS_NAN;
    if (ex == 0) {
        if (x.m == 0) return CLS_ZERO;
        int sh = __builtin_clzll(x.m);
        *m = x.m << sh;
        *E = EXP_MIN - sh;
        return CLS_FIN;
    }
    *E = (int32_t)ex - EXP_MAX;
    *m = x.m;
    return CLS_FIN;
}

static vt_x80 mk_zero(int neg)
{
    vt_x80 z = {0, (uint16_t)(neg ? 0x8000u : 0u)};
    return z;
}

static vt_x80 mk_inf(int neg)
{
    vt_x80 x = {0x8000000000000000ull, (uint16_t)(0x7FFFu | (neg ? 0x8000u : 0u))};
    return x;
}

/* ---- host double fast path ----------------------------------------------------
 *
 * Windows leaves the x87 at extended precision (fcw = 0x037F, PC = 64 bits), so every
 * arithmetic instruction in the guest runs one of the soft 80-bit routines below. They are
 * exact, but they are also the most expensive thing the port does: profiled on a Retroid
 * Pocket they were ~38% of all CPU time, with vt_fsqrt a 64-step restoring square root and
 * addsub a 128-bit alignment with a leading-zero scan.
 *
 * Almost every operand the guest computes comes from a float32 or float64 sitting in memory,
 * and both are exactly representable as a host double. When the control word asks for
 * extended precision and round to nearest - this game's constant setting - doing the op in
 * the host FPU and keeping the result as a double drops only bits 54..64 of each
 * intermediate. A later store to float32 (24 bits) or float64 (53) is then unchanged unless
 * those dropped bits straddle a rounding boundary, which is about a 2^-29 chance per store.
 *
 * The fast path is taken only when both operands and the result are normal doubles, so
 * overflow past double range, underflow, denormals, infinities and NaNs all still fall
 * through to the exact routines. VT3_FPU_EXACT=1 turns the fast path off (read once) so the
 * two implementations can be diffed against each other. */

static int fpu_fast = -1;

static int fpu_use_fast(void)
{
    if (fpu_fast < 0) {
        const char *e = getenv("VT3_FPU_EXACT");
        fpu_fast = (e && *e && *e != '0') ? 0 : 1;
    }
    return fpu_fast;
}

/* True when the control word is the one this game runs in: extended precision, nearest. */
static int fast_cw(uint32_t cw)
{
    uint32_t pc = (cw >> 8) & 3u;
    return fpu_use_fast() && (pc == 3u || pc == 1u) && ((cw >> 10) & 3u) == 0u;
}

/* x80 biased exponents that land on a normal double: 2^-1022 .. 2^1023. */
#define DEXP_LO (16383u - 1022u)
#define DEXP_SPAN (1022u + 1023u + 1u)

/* A normal x80 whose value is inside the double range, so x80_to_d is exact before rounding.
 * Zero rides along with them. 0.0 is the commonest single operand in game math - matrix rows,
 * vector components nothing has written yet - and shipping it to the soft path meant a full
 * decode and pack_round of the other operand for every multiply by an unused slot. Measured at
 * 4.7% of CPU on the Retroid doing exactly that. */
static int dnormal_x80(vt_x80 a)
{
    uint32_t e = a.se & 0x7FFFu;
    return e - DEXP_LO < DEXP_SPAN || (e == 0u && a.m == 0u);
}

/* A normal double: not denormal, infinite or NaN. An exact zero counts as normal too, for the
 * same reason its x80 twin does. */
static int dnormal_d(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    uint32_t e = ((uint32_t)(b >> 52)) & 0x7FFu;
    return e - 1u < 0x7FEu || (e == 0u && (b << 12) == 0u);
}

/* Normal x80 -> double, rounded to nearest even. The caller guarantees a normal x80 or zero. */
static double x80_to_d(vt_x80 a)
{
    uint64_t m = a.m;
    int neg = a.se >> 15;
    if (!m) return neg ? -0.0 : 0.0;          /* zero; its exponent field is not meaningful */
    int32_t e = (int32_t)(a.se & 0x7FFFu) - EXP_MAX;  /* value = m * 2^(e-63), m in [2^63,2^64) */
    uint64_t round = m & 0x7FFu;
    m >>= 11;
    if (round > 0x400u || (round == 0x400u && (m & 1u))) {
        m++;
        if (m >> 53) { m >>= 1; e++; }                /* carried past the 53-bit significand */
    }
    uint64_t b = ((uint64_t)(uint32_t)(e + 1023) << 52) | (m & 0x0000FFFFFFFFFFFFFull)
               | ((uint64_t)(neg != 0) << 63);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* Double -> x80. Exact: a double's 53-bit significand always fits in 64. */
static vt_x80 d_to_x80(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    uint16_t neg = (uint16_t)((b >> 63) << 15);      /* the x87 sign is bit 15 of se */
    uint32_t ex = (uint32_t)((b >> 52) & 0x7FFu);
    uint64_t frac = b & 0x0000FFFFFFFFFFFFFull;
    vt_x80 r;
    if (ex == 0x7FFu) {                               /* infinity or NaN */
        r.m = frac ? 0xC000000000000000ull : 0x8000000000000000ull;
        r.se = (uint16_t)(0x7FFFu | neg);
        return r;
    }
    if (ex == 0u) {                                   /* zero or double denormal */
        if (frac == 0) { r.m = 0; r.se = neg; return r; }
        int lz = __builtin_clzll(frac);
        r.m = frac << lz;                             /* bit 63 set */
        r.se = (uint16_t)(neg | (uint16_t)(EXP_MAX - 1074 - lz + 63));
        return r;
    }
    r.m = (frac | ((uint64_t)1 << 52)) << 11;
    r.se = (uint16_t)(neg | (uint16_t)(ex - 1023u + 16383u));
    return r;
}

static int prec_bits(uint32_t cw)
{
    switch ((cw >> 8) & 3u) {
    case 0: return 24;
    case 2: return 53;
    default: return 64;
    }
}

/* Round a value to prec bits by mode rc. The value is (mant + extra/2^64 + sticky) * 2^(E-63),
 * with mant's top bit set. sticky means nonzero bits lie below extra. */
static vt_x80 pack_round(int neg, int32_t E, uint64_t mant, uint64_t extra, int sticky,
                         int prec, uint32_t rc)
{
    if (mant == 0) return mk_zero(neg);
    int drop = 64 - prec;
    /* W = mant:extra. Kept bits are the top prec bits, guard is the next, the rest is sticky. */
    u128 W = ((u128)mant << 64) | extra;
    uint64_t kept = (uint64_t)(W >> (64 + drop));
    int guard = (int)((W >> (63 + drop)) & 1u);
    int below = (W & ((((u128)1) << (63 + drop)) - 1)) != 0 || sticky;
    int up = 0;
    switch (rc & 3u) {
    case 0: up = guard && (below || (kept & 1u)); break;
    case 1: up = neg && (guard || below); break;
    case 2: up = !neg && (guard || below); break;
    default: up = 0; break;
    }
    if (up) {
        kept++;
        /* Carried out of the significand. At 64 bits the increment wraps to 0, and 1 << 64 is not a valid
         * shift, so the carry was never seen and the result lost its leading bit: an invalid number that
         * the guest then read as NaN. */
        if (prec == 64 ? kept == 0 : kept == ((uint64_t)1 << prec)) {
            kept = prec == 64 ? ((uint64_t)1 << 63) : kept >> 1;
            E++;
        }
    }
    uint64_t m = kept << drop;
    if (E > EXP_MAX) return mk_inf(neg);
    if (E < EXP_MIN) {
        /* Denormal range: shift the significand right, truncating. */
        int sh = EXP_MIN - E;
        m = sh >= 64 ? 0 : (m >> sh);
        if (m == 0) return mk_zero(neg);
        vt_x80 d = {m, (uint16_t)(neg ? 0x8000u : 0u)};
        return d;
    }
    vt_x80 r = {m, (uint16_t)((neg ? 0x8000u : 0u) | (uint32_t)(E + EXP_MAX))};
    return r;
}

static vt_x80 result_nan(vt_x80 a, vt_x80 b, int ca, int cb)
{
    /* The first NaN operand wins. Two NaNs or a NaN and an infinity give the default NaN. */
    if (ca == CLS_NAN) { vt_x80 r = a; r.m |= 0xC000000000000000ull; return r; }
    if (cb == CLS_NAN) { vt_x80 r = b; r.m |= 0xC000000000000000ull; return r; }
    return VX_NAN_INDEF;
}

static vt_x80 addsub(vt_x80 a, vt_x80 b, int subtract, uint32_t cw)
{
    if (fast_cw(cw) && dnormal_x80(a) && dnormal_x80(b)) {
        double x = x80_to_d(a), y = x80_to_d(b);
        double r = subtract ? (x - y) : (x + y);
        if (dnormal_d(r)) return d_to_x80(r);
    }
    int na, nb, ca, cb;
    int32_t Ea = 0, Eb = 0;
    uint64_t ma = 0, mb = 0;
    ca = dec(a, &na, &Ea, &ma);
    cb = dec(b, &nb, &Eb, &mb);
    int sb = nb ^ subtract;
    if (ca == CLS_NAN || cb == CLS_NAN) return result_nan(a, b, ca, cb);
    if (ca == CLS_INF && cb == CLS_INF) {
        if (na != sb) return VX_NAN_INDEF;
        return mk_inf(na);
    }
    if (ca == CLS_INF) return mk_inf(na);
    if (cb == CLS_INF) return mk_inf(sb);
    int prec = prec_bits(cw);
    uint32_t rc = (cw >> 10) & 3u;
    if (ca == CLS_ZERO && cb == CLS_ZERO) return mk_zero(na && sb);
    if (ca == CLS_ZERO) return pack_round(sb, Eb, mb, 0, 0, prec, rc);
    if (cb == CLS_ZERO) return pack_round(na, Ea, ma, 0, 0, prec, rc);

    /* Work with the larger exponent in front. */
    if (Eb > Ea) {
        int32_t t = Ea; Ea = Eb; Eb = t;
        uint64_t u = ma; ma = mb; mb = u;
        int v = na; na = sb; sb = v;
    }
    int32_t d = Ea - Eb;
    u128 A = ((u128)ma) << 63;
    u128 B = ((u128)mb) << 63;
    int sticky = 0;
    if (d >= 127) {
        sticky = 1;
        B = 0;
    } else if (d > 0) {
        u128 lost = B & ((((u128)1) << d) - 1);
        sticky = lost != 0;
        B >>= d;
    }
    if (sticky) B |= 1;   /* keeps the discarded bits as a sticky LSB for the subtraction */

    u128 S;
    int sign;
    if (na == sb) {
        S = A + B;
        sign = na;
    } else if (A >= B) {
        S = A - B;
        sign = na;
    } else {
        S = B - A;
        sign = sb;
    }
    if (S == 0) return mk_zero(rc == 1);
    int p = 127;
    while (!((S >> p) & 1u)) p--;
    /* Shift so the top bit is bit 127. The top 64 bits are the significand. */
    u128 T = S << (127 - p);
    uint64_t mant = (uint64_t)(T >> 64);
    uint64_t extra = (uint64_t)T;
    int32_t E = Ea - 126 + p;
    return pack_round(sign, E, mant, extra, sticky, prec, rc);
}

vt_x80 vt_fadd(vt_x80 a, vt_x80 b, uint32_t cw) { return addsub(a, b, 0, cw); }
vt_x80 vt_fsub(vt_x80 a, vt_x80 b, uint32_t cw) { return addsub(a, b, 1, cw); }

vt_x80 vt_fmul(vt_x80 a, vt_x80 b, uint32_t cw)
{
    if (fast_cw(cw) && dnormal_x80(a) && dnormal_x80(b)) {
        double r = x80_to_d(a) * x80_to_d(b);
        if (dnormal_d(r)) return d_to_x80(r);
    }
    int na, nb, ca, cb;
    int32_t Ea = 0, Eb = 0;
    uint64_t ma = 0, mb = 0;
    ca = dec(a, &na, &Ea, &ma);
    cb = dec(b, &nb, &Eb, &mb);
    int neg = na ^ nb;
    if (ca == CLS_NAN || cb == CLS_NAN) return result_nan(a, b, ca, cb);
    if ((ca == CLS_INF && cb == CLS_ZERO) || (ca == CLS_ZERO && cb == CLS_INF)) return VX_NAN_INDEF;
    if (ca == CLS_INF || cb == CLS_INF) return mk_inf(neg);
    if (ca == CLS_ZERO || cb == CLS_ZERO) return mk_zero(neg);
    u128 prod = (u128)ma * mb;
    uint64_t mant, extra;
    int32_t E;
    if ((prod >> 127) & 1u) {
        mant = (uint64_t)(prod >> 64);
        extra = (uint64_t)prod;
        E = Ea + Eb + 1;
    } else {
        u128 T = prod << 1;
        mant = (uint64_t)(T >> 64);
        extra = (uint64_t)T;
        E = Ea + Eb;
    }
    return pack_round(neg, E, mant, extra, 0, prec_bits(cw), (cw >> 10) & 3u);
}

/* a / b, rounded once from a 64+2-bit quotient with a sticky remainder bit. */
static vt_x80 divide(vt_x80 a, vt_x80 b, uint32_t cw)
{
    if (fast_cw(cw) && dnormal_x80(a) && dnormal_x80(b)) {
        double r = x80_to_d(a) / x80_to_d(b);      /* +-inf or NaN on a zero/NaN operand: falls through */
        if (dnormal_d(r)) return d_to_x80(r);
    }
    int na, nb, ca, cb;
    int32_t Ea = 0, Eb = 0;
    uint64_t ma = 0, mb = 0;
    ca = dec(a, &na, &Ea, &ma);
    cb = dec(b, &nb, &Eb, &mb);
    int neg = na ^ nb;
    if (ca == CLS_NAN || cb == CLS_NAN) return result_nan(a, b, ca, cb);
    if ((ca == CLS_ZERO && cb == CLS_ZERO) || (ca == CLS_INF && cb == CLS_INF)) return VX_NAN_INDEF;
    if (cb == CLS_ZERO) return mk_inf(neg);      /* divide by zero: infinity (no exception) */
    if (ca == CLS_INF || cb == CLS_INF) return ca == CLS_INF ? mk_inf(neg) : mk_zero(neg);
    if (ca == CLS_ZERO) return mk_zero(neg);
    /* Two-step division. q1 = ma*2^64/mb has 64 or 65 bits. The next two quotient bits come
     * from the remainder, and they are the guard and round bits. Q' = q1*4 + g. */
    u128 N = ((u128)ma) << 64;
    u128 q1 = N / mb;
    u128 r1 = N % mb;
    u128 t = r1 << 2;                               /* r1 < mb < 2^64, so this fits */
    u128 g = t / mb;                                /* 0..3 */
    u128 r2 = t % mb;
    int sticky = r2 != 0;
    u128 Qp = (q1 << 2) | g;                        /* in (2^65, 2^67) */
    uint64_t mant, extra;
    int32_t E;
    if (Qp >> 66) {
        mant = (uint64_t)(Qp >> 3);
        extra = (uint64_t)(Qp & 7) << 61;
        E = Ea - Eb;
    } else {
        mant = (uint64_t)(Qp >> 2);
        extra = (uint64_t)(Qp & 3) << 62;
        E = Ea - Eb - 1;
    }
    return pack_round(neg, E, mant, extra, sticky, prec_bits(cw), (cw >> 10) & 3u);
}

vt_x80 vt_fdiv(vt_x80 a, vt_x80 b, uint32_t cw) { return divide(a, b, cw); }

vt_x80 vt_fsqrt(vt_x80 a, uint32_t cw)
{
    if (fast_cw(cw) && dnormal_x80(a)) {
        double r = sqrt(x80_to_d(a));               /* NaN on a negative operand: falls through */
        if (dnormal_d(r)) return d_to_x80(r);
    }
    int neg;
    int32_t E = 0;
    uint64_t m = 0;
    int c = dec(a, &neg, &E, &m);
    if (c == CLS_NAN) return result_nan(a, a, c, c);
    if (c == CLS_ZERO) return a;                     /* sqrt(+-0) = +-0 */
    if (c == CLS_INF) return neg ? VX_NAN_INDEF : a;
    if (neg) return VX_NAN_INDEF;
    /* value = m * 2^(E-63). Choose a shift s in {63, 64} so that E-63-s is even. */
    int s = (((E - 63) - 63) & 1) == 0 ? 63 : 64;   /* (E-63-s) must be even */
    int32_t k2 = E - 63 - s;                        /* even */
    u128 N = ((u128)m) << s;                        /* in [2^126, 2^128) */
    u128 x = N, res = 0, bit = ((u128)1) << 126;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    uint64_t R = (uint64_t)res;                     /* in [2^63, 2^64) */
    u128 rem = N - (u128)R * R;                     /* remainder of the root */
    int guard = rem > R;                            /* the fraction is at least one half */
    uint64_t extra = guard ? ((uint64_t)1 << 63) : 0;
    int sticky = rem != 0;
    int32_t Eout = 63 + k2 / 2;
    return pack_round(0, Eout, R, extra, sticky, prec_bits(cw), (cw >> 10) & 3u);
}

/* Round to an integer by RC. The rounded magnitude is returned in *out with its sign. */
static int round_int(vt_x80 a, uint32_t rc, uint64_t *out, int *neg, int *bad)
{
    int32_t E = 0;
    uint64_t m = 0;
    int c = dec(a, neg, &E, &m);
    *bad = 0;
    *out = 0;
    if (c == CLS_NAN || c == CLS_INF) { *bad = 1; return 0; }
    if (c == CLS_ZERO) return 0;
    if (E >= 63) {
        if (E > 63) { *bad = 1; return 0; }
        *out = m;                                   /* exact */
        return 0;
    }
    int frac = 63 - E;                              /* bits below the units place, in 1..64+ */
    uint64_t kept = 0;
    int guard = 0, sticky = 0;
    if (frac >= 64) {
        kept = 0;
        guard = (frac == 64) ? (int)(m >> 63) : 0;
        sticky = (frac == 64) ? ((m & 0x7FFFFFFFFFFFFFFFull) != 0) : 1;
    } else {
        kept = m >> frac;
        guard = (int)((m >> (frac - 1)) & 1u);
        sticky = (m & ((((uint64_t)1) << (frac - 1)) - 1)) != 0;
    }
    int up = 0;
    switch (rc & 3u) {
    case 0: up = guard && (sticky || (kept & 1u)); break;
    case 1: up = *neg && (guard || sticky); break;
    case 2: up = !*neg && (guard || sticky); break;
    default: up = 0; break;
    }
    *out = kept + (up ? 1 : 0);
    return 0;
}

int64_t vt_fint(vt_x80 a, uint32_t rc, int bits)
{
    uint64_t mag;
    int neg, bad;
    round_int(a, rc, &mag, &neg, &bad);
    int64_t indef = bits == 16 ? INT16_MIN : bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
    if (bad) return indef;
    if (neg) {
        uint64_t lim = (uint64_t)1 << (bits - 1);
        if (mag > lim) return indef;
        return (int64_t)(0 - mag);
    }
    uint64_t lim = ((uint64_t)1 << (bits - 1)) - 1;
    if (bits == 64) lim = 0x7FFFFFFFFFFFFFFFull;
    if (mag > lim) return indef;
    return (int64_t)mag;
}

vt_x80 vt_frndint(vt_x80 a, uint32_t cw)
{
    uint64_t mag;
    int neg, bad;
    int32_t E = 0;
    uint64_t m = 0;
    int c = dec(a, &neg, &E, &m);
    if (c == CLS_NAN) return result_nan(a, a, c, c);
    if (c != CLS_FIN) return a;                     /* zero and infinity are already integers */
    if (E >= 63) return a;
    round_int(a, (cw >> 10) & 3u, &mag, &neg, &bad);
    if (mag == 0) return mk_zero(neg);
    int lz = __builtin_clzll(mag);
    vt_x80 r = {mag << lz, (uint16_t)((neg ? 0x8000u : 0u) | (uint32_t)(EXP_MAX + 63 - lz))};
    return r;
}

vt_x80 vt_fild(int64_t v)
{
    if (v == 0) return mk_zero(0);
    int neg = v < 0;
    uint64_t mag = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    int lz = __builtin_clzll(mag);
    vt_x80 r = {mag << lz, (uint16_t)((neg ? 0x8000u : 0u) | (uint32_t)(EXP_MAX + 63 - lz))};
    return r;
}

vt_x80 vt_fneg(vt_x80 a) { a.se ^= 0x8000u; return a; }
vt_x80 vt_fabs(vt_x80 a) { a.se &= 0x7FFFu; return a; }

/* Compare: returns the x87 status bits for less (CF), equal (ZF), unordered (PF). */
uint32_t vt_fcmp(vt_x80 a, vt_x80 b)
{
    int na, nb;
    int32_t Ea = 0, Eb = 0;
    uint64_t ma = 0, mb = 0;
    int ca = dec(a, &na, &Ea, &ma), cb = dec(b, &nb, &Eb, &mb);
    if (ca == CLS_NAN || cb == CLS_NAN) return 0x7u;
    /* Map each value to a signed key: zero is 0, and the magnitude orders by (E, m). */
    int ra, rb;
    if (ca == CLS_ZERO && cb == CLS_ZERO) return 0x2u;
    if (ca == CLS_INF || cb == CLS_INF) {
        /* a < b with an infinity: -inf is below every finite value and +inf is above it. */
        if (ca == CLS_INF && cb == CLS_INF) return na == nb ? 0x2u : (na ? 0x1u : 0x0u);
        if (ca == CLS_INF) return na ? 0x1u : 0x0u;   /* a is -inf: less, a is +inf: not */
        return nb ? 0x0u : 0x1u;                       /* b is -inf: not less, b is +inf: less */
    }
    if (ca == CLS_ZERO) ra = 0, rb = nb ? -1 : 1;
    else if (cb == CLS_ZERO) ra = na ? -1 : 1, rb = 0;
    else {
        if (na != nb) return na ? 0x1u : 0x0u;      /* negative is less than positive */
        int mag;
        if (Ea != Eb) mag = Ea < Eb ? -1 : 1;
        else if (ma != mb) mag = ma < mb ? -1 : 1;
        else mag = 0;
        if (mag == 0) return 0x2u;
        int less = na ? (mag > 0) : (mag < 0);      /* negatives reverse the magnitude order */
        return less ? 0x1u : 0x0u;
    }
    if (ra == rb) return 0x2u;
    return ra < rb ? 0x1u : 0x0u;
}

uint32_t vt_fxam(vt_x80 a)
{
    uint32_t sign = (a.se >> 15) & 1u;
    uint32_t ex = a.se & 0x7FFFu;
    uint32_t c = 0;
    if (ex == 0x7FFFu) c = (a.m << 1) == 0 ? 0x500u : 0x100u;          /* inf : nan */
    else if (ex == 0) c = a.m == 0 ? 0x4000u : 0x4400u;                 /* zero : denormal */
    else c = 0x400u;                                                    /* normal */
    return c | (sign << 9);
}

vt_x80 vt_fld32(uint32_t a)
{
    uint32_t bits;
    memcpy(&bits, vt_mem + a, 4);
    float f;
    memcpy(&f, &bits, 4);
    if (fpu_use_fast()) return d_to_x80((double)f);   /* a float32 widens to double exactly */
    return vt_from_double((double)f);
}

vt_x80 vt_fld64(uint32_t a)
{
    double d;
    memcpy(&d, vt_mem + a, 8);
    if (fpu_use_fast()) return d_to_x80(d);
    return vt_from_double(d);
}

vt_x80 vt_fld80(uint32_t a)
{
    vt_x80 x;
    memcpy(&x.m, vt_mem + a, 8);
    memcpy(&x.se, vt_mem + a + 8, 2);
    return x;
}

void vt_fst32(uint32_t a, vt_x80 x, uint32_t cw)
{
    uint32_t bits;
    if (fpu_use_fast() && ((cw >> 10) & 3u) == 0u && dnormal_x80(x)) {
        /* Rounding a normal value to nearest, double then float, is the single x87 rounding. */
        float f = (float)x80_to_d(x);
        memcpy(&bits, &f, 4);
        memcpy(vt_mem + a, &bits, 4);
        return;
    }
    (void)cw;
    int neg;
    int32_t E = 0;
    uint64_t m = 0;
    int c = dec(x, &neg, &E, &m);
    float f;
    if (c == CLS_NAN) f = NAN;
    else if (c == CLS_INF) f = neg ? -INFINITY : INFINITY;
    else if (c == CLS_ZERO) f = neg ? -0.0f : 0.0f;
    else {
        /* Round to 24 bits by RC, then build the float exactly. */
        vt_x80 r = pack_round(neg, E, m, 0, 0, 24, (cw >> 10) & 3u);
        int32_t E2 = 0;
        uint64_t m2 = 0;
        int n2;
        dec(r, &n2, &E2, &m2);
        f = ldexpf((float)(m2 >> 40), E2 - 23);
        if (neg) f = -f;
    }
    memcpy(&bits, &f, 4);
    memcpy(vt_mem + a, &bits, 4);
}

void vt_fst64(uint32_t a, vt_x80 x, uint32_t cw)
{
    double d = vt_to_double_rc(x, (cw >> 10) & 3u);
    memcpy(vt_mem + a, &d, 8);
}

void vt_fst80(uint32_t a, vt_x80 x)
{
    memcpy(vt_mem + a, &x.m, 8);
    memcpy(vt_mem + a + 8, &x.se, 2);
}

vt_x80 vt_fconst(int which)
{
    switch (which) {
    case VT_FC_1: { vt_x80 x = {0x8000000000000000ull, 0x3FFFu}; return x; }
    case VT_FC_ZERO: return mk_zero(0);
    case VT_FC_PI: { vt_x80 x = {0xC90FDAA22168C235ull, 0x4000u}; return x; }
    case VT_FC_L2E: { vt_x80 x = {0xB8AA3B295C17F0BCull, 0x3FFFu}; return x; }
    case VT_FC_LN2: { vt_x80 x = {0xB17217F7D1CF79ACull, 0x3FFEu}; return x; }
    case VT_FC_LG2: { vt_x80 x = {0x9A209A84FBCFF799ull, 0x3FFDu}; return x; }
    case VT_FC_L2T: { vt_x80 x = {0xD49A784BCD1B8AFEull, 0x4000u}; return x; }
    default: return mk_zero(0);
    }
}

vt_x80 vt_from_double(double d)
{
    if (fpu_use_fast()) return d_to_x80(d);
    if (isnan(d)) { vt_x80 x = {0xC000000000000000ull, 0x7FFFu | (signbit(d) ? 0x8000u : 0u)}; return x; }
    if (isinf(d)) return mk_inf(signbit(d) != 0);
    if (d == 0.0) return mk_zero(signbit(d) != 0);
    int neg = d < 0;
    int e;
    double f = frexp(fabs(d), &e);                  /* f in [0.5, 1) */
    uint64_t m = (uint64_t)ldexp(f, 64);            /* exact: f has at most 53 bits */
    vt_x80 x = {m, (uint16_t)((neg ? 0x8000u : 0u) | (uint32_t)(e - 1 + EXP_MAX))};
    return x;
}

/* Convert to double, rounding by RC. The result may be an infinity or a denormal. */
static double vt_to_double_rc(vt_x80 a, uint32_t rc)
{
    if (rc == 0u && fpu_use_fast() && dnormal_x80(a)) return x80_to_d(a);
    int neg;
    int32_t E = 0;
    uint64_t m = 0;
    int c = dec(a, &neg, &E, &m);
    if (c == CLS_NAN) return NAN;
    if (c == CLS_INF) return neg ? -INFINITY : INFINITY;
    if (c == CLS_ZERO) return neg ? -0.0 : 0.0;
    vt_x80 r = pack_round(neg, E, m, 0, 0, 53, rc);
    int32_t E2 = 0;
    uint64_t m2 = 0;
    int n2;
    dec(r, &n2, &E2, &m2);
    double d = ldexp((double)(m2 >> 11), E2 - 52);
    return neg ? -d : d;
}

double vt_to_double(vt_x80 a) { return vt_to_double_rc(a, 0); }

/* Transcendental functions. Double precision, rounded to the control word's PC. */
vt_x80 vt_fdbl1(vt_x80 a, uint32_t cw, int op)
{
    double x = vt_to_double(a), r;
    switch (op) {
    case VT_FD_SIN: r = sin(x); break;
    case VT_FD_COS: r = cos(x); break;
    case VT_FD_TAN: r = tan(x); break;
    default: r = exp2(x) - 1.0; break;               /* F2XM1 */
    }
    return vt_from_double(r);
}

vt_x80 vt_fdbl2(vt_x80 a, vt_x80 b, uint32_t cw, int op)
{
    double x = vt_to_double(a), y = vt_to_double(b), r;
    switch (op) {
    case VT_FD2_PATAN: r = atan2(y, x); break;
    case VT_FD2_YL2X: r = y * log2(x); break;
    case VT_FD2_SCALE: r = x * exp2(trunc(y)); break;
    default: r = fmod(x, y); break;                  /* FPREM, approximate */
    }
    return vt_from_double(r);
}
