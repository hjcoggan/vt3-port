/* vt3fpu: the guest's x87, run on the host double unit. See vt3fpu.h for the model.
 *
 * Every operation here is the host instruction or the libm call that does the same job. The
 * x87 used to be emulated bit-exactly in software, and on a Retroid Pocket that emulation was
 * ~38% of all CPU time: a 64-step restoring square root, a 128-bit alignment with a
 * leading-zero scan for every add. Trading 64-bit intermediates for 53-bit ones is what buys
 * that back, and the trade is measured rather than assumed - a differential harness ran the
 * two implementations against each other over the game's own data, and 0.019% of the float32
 * stores differed, each by one ULP, with no change in classification.
 *
 * What is *not* kept:
 *   - the precision control word. A double register file has one precision, 53 bits. The
 *     guest's PC is 64 (extended), so the difference only shows in intermediate tails.
 *   - exact FILD. An int64 wider than 2^53 rounds on load. Game counters and indices are
 *     far below that.
 *   - VT3_FPU_EXACT. There is no exact implementation left to fall back to.
 *
 * What *is* kept: RC on every store and every integer conversion, the status-word encodings
 * of FCOM/FUCOM/FXAM including unordered, the indefinite values on overflow, and the 80-bit
 * memory forms - which round to 53 on load and are widened exactly on store.
 */
#include "vt3fpu.h"
#include "vt3rt.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EXP_MAX 16383
#define EXP_MIN (-16382)

/* ---- the 80-bit memory form ---------------------------------------------------
 *
 * mm.m is a 64-bit significand with the integer bit explicit, mm.se carries the sign and a
 * 16383-biased exponent, and the value is m * 2^(E-63) with E unbiased. Nothing in the
 * register file is stored this way; only fld80, fst80 and FNSAVE's register block see it. */
typedef struct x80mem {
    uint64_t m;   /* significand, bit 63 is the explicit integer bit */
    uint16_t se;  /* bit 15 sign, bits 14:0 biased exponent (bias 16383) */
} x80mem;

/* Rounded to nearest even, so an 80-bit value that needs more than 53 bits lands on the
 * nearest double rather than the one below it. Handles zero, denormal, infinity and NaN. */
static double x80_to_d(x80mem a)
{
    int neg = a.se >> 15;
    uint32_t ex = a.se & 0x7FFFu;
    uint64_t m = a.m;
    int32_t E;

    if (ex == 0x7FFFu) {                       /* infinity (integer bit set) or NaN */
        double d = ((m << 1) == 0) ? (double)INFINITY : (double)NAN;
        return neg ? -d : d;
    }
    if (ex == 0u) {
        if (m == 0) return neg ? -0.0 : 0.0;
        int sh = __builtin_clzll(m);
        m <<= sh;                              /* normalise; the exponent absorbs the shift */
        E = EXP_MIN - sh;
    } else {
        E = (int32_t)ex - EXP_MAX;
    }

    uint64_t round = m & 0x7FFu;
    uint64_t sig = m >> 11;                    /* 53 significant bits */
    if (round > 0x400u || (round == 0x400u && (sig & 1u))) {
        sig++;
        if (sig >> 53) { sig >>= 1; E++; }     /* carried past the significand */
    }
    return ldexp((double)sig, E - 52);
}

/* Exact: a double's 53-bit significand always fits in 64. Denormal doubles are widened to a
 * normal x80, infinities and NaNs keep their encoding. */
static x80mem d_to_x80(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    uint16_t neg = (uint16_t)((b >> 63) << 15);      /* the x87 sign is bit 15 of se */
    uint32_t ex = (uint32_t)((b >> 52) & 0x7FFu);
    uint64_t frac = b & 0x0000FFFFFFFFFFFFFull;
    x80mem r;
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

/* ---- rounding ---------------------------------------------------------------- */

static const int vt_fe_rc[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};

/* Round by RC, as frndint does. floor/ceil/trunc and nearbyint cover the four x87 modes;
 * nearbyint runs on the host's own mode, which is round-to-nearest-even unless these
 * functions have just changed it. */
static double round_by_rc(double x, uint32_t rc)
{
    switch (rc & 3u) {
    case 0: return nearbyint(x);
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}

/* Round to the precision of a float32 under RC. The float store is the one conversion where
 * the bits the double register file dropped could still matter, so the directed modes are
 * done properly instead of being left to the host's nearest rounding. The mode is changed
 * only when RC asks for it, which is never in this game. */
static double round_to32(double x, uint32_t rc)
{
    if (rc == 0u || !isfinite(x)) return x;
    int old = fegetround();
    fesetround(vt_fe_rc[rc & 3u]);
    volatile double r = (double)(float)x;           /* volatile forces the rounding to happen */
    fesetround(old);
    return r;
}

/* The game holds the control word at 0x037F: extended precision, round to nearest. RC is
 * honoured throughout, but PC cannot be - say once if that ever stops being true. The one
 * value seen in a full match run is 0x0C3F, single precision toward zero, from a library
 * control87 path: there is no constant 0xC3F in the translated code and only four FPU
 * env save/restore sites, so it is not game math, and double is at least as accurate as
 * single either way. */
static void check_cw(uint32_t cw)
{
    static int said = 0;
    if (cw != 0x37Fu && !said) {
        said = 1;
        fprintf(stderr, "vt3fpu: control word %04X is not 0x037F; the double register file "
                        "ignores its precision control\n", cw);
    }
}

/* ---- arithmetic -------------------------------------------------------------- */

double vt_fadd(double a, double b, uint32_t cw) { check_cw(cw); return a + b; }
double vt_fsub(double a, double b, uint32_t cw) { check_cw(cw); return a - b; }
double vt_fmul(double a, double b, uint32_t cw) { (void)cw; return a * b; }
double vt_fdiv(double a, double b, uint32_t cw) { (void)cw; return a / b; }
double vt_fsqrt(double a, uint32_t cw) { (void)cw; return sqrt(a); }
double vt_fneg(double a) { return -a; }
double vt_fabs(double a) { return fabs(a); }

double vt_frndint(double a, uint32_t cw)
{
    if (!isfinite(a)) return a;                    /* zero and infinity are already integers */
    return round_by_rc(a, (cw >> 10) & 3u);
}

double vt_fild(int64_t v) { return (double)v; }

int64_t vt_fint(double a, uint32_t rc, int bits)
{
    int64_t indef = bits == 16 ? INT16_MIN : bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
    if (isnan(a)) return indef;
    double r = round_by_rc(a, rc);
    /* Range-check before the conversion: converting an out-of-range double is undefined. */
    double lo, hi;
    switch (bits) {
    case 16: lo = -32768.0;          hi = 32768.0;            break;
    case 32: lo = -2147483648.0;     hi = 2147483648.0;       break;
    default: lo = -9223372036854775808.0; hi = 9223372036854775808.0; break;
    }
    if (!(r >= lo && r < hi)) return indef;
    return (int64_t)r;
}

/* ---- classification and comparison ------------------------------------------- */

/* Compare: the x87 status bits for less (CF), equal (ZF), unordered (PF). */
uint32_t vt_fcmp(double a, double b)
{
    if (isnan(a) || isnan(b)) return 0x7u;
    if (a < b) return 0x1u;
    if (a == b) return 0x2u;
    return 0x0u;
}

uint32_t vt_fxam(double a)
{
    uint32_t sign = (uint32_t)(signbit(a) != 0);
    uint32_t c;
    if (isnan(a)) c = 0x100u;                                       /* C0 */
    else if (isinf(a)) c = 0x500u;                                  /* C2 C0 */
    else if (a == 0.0) c = 0x4000u;                                 /* C3 */
    else {
        uint64_t b;
        memcpy(&b, &a, 8);
        c = (((b >> 52) & 0x7FFu) == 0u) ? 0x4400u : 0x400u;        /* denormal C3 C2 : normal C2 */
    }
    return c | (sign << 9);
}

/* ---- memory forms ------------------------------------------------------------ */

double vt_fld32(uint32_t a)
{
    float f;
    memcpy(&f, vt_mem + a, 4);
    return (double)f;                       /* a float32 widens to double exactly */
}

double vt_fld64(uint32_t a)
{
    double d;
    memcpy(&d, vt_mem + a, 8);
    return d;
}

double vt_fld80(uint32_t a)
{
    x80mem x;
    memcpy(&x.m, vt_mem + a, 8);
    memcpy(&x.se, vt_mem + a + 8, 2);
    return x80_to_d(x);
}

void vt_fst32(uint32_t a, double x, uint32_t cw)
{
    float f = (float)round_to32(x, (cw >> 10) & 3u);
    memcpy(vt_mem + a, &f, 4);
}

void vt_fst64(uint32_t a, double x, uint32_t cw)
{
    double d = round_by_rc(x, (cw >> 10) & 3u);
    memcpy(vt_mem + a, &d, 8);
}

void vt_fst80(uint32_t a, double x)
{
    x80mem r = d_to_x80(x);
    memcpy(vt_mem + a, &r.m, 8);
    memcpy(vt_mem + a + 8, &r.se, 2);
}

/* ---- constants --------------------------------------------------------------- */

double vt_fconst(int which)
{
    switch (which) {
    case VT_FC_1:   return 1.0;
    case VT_FC_ZERO: return 0.0;
    case VT_FC_PI:  return 3.14159265358979323846;
    case VT_FC_L2E: return 1.44269504088896340736;   /* log2 e */
    case VT_FC_LN2: return 0.69314718055994530942;
    case VT_FC_LG2: return 0.30102999566398119521;   /* log10 2 */
    case VT_FC_L2T: return 3.32192809488736234787;   /* log2 10 */
    default: return 0.0;
    }
}

/* ---- transcendental functions ------------------------------------------------ */

double vt_fdbl1(double a, uint32_t cw, int op)
{
    (void)cw;
    switch (op) {
    case VT_FD_SIN: return sin(a);
    case VT_FD_COS: return cos(a);
    case VT_FD_TAN: return tan(a);
    default: return exp2(a) - 1.0;               /* F2XM1 */
    }
}

double vt_fdbl2(double a, double b, uint32_t cw, int op)
{
    (void)cw;
    switch (op) {
    case VT_FD2_PATAN: return atan2(b, a);
    case VT_FD2_YL2X: return b * log2(a);
    case VT_FD2_SCALE: return a * exp2(trunc(b));
    default: return fmod(a, b);                  /* FPREM, approximate */
    }
}
