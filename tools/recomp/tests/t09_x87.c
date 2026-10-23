/* x87 floating point. Built with -mfpmath=387, and also used with long double (80-bit,
 * x87 native). The translator computes x87 in double precision, so this test prints
 * values scaled and rounded to a few decimal places. That keeps the comparison
 * meaningful, and the gap is recorded in the design note.
 */
#include "tcommon.h"

static volatile double da = 2.75, db = 1.25, dc = -8.5;
static volatile float fa = 0.5f, fb = 3.0f;
static volatile long long big = 123456789012LL;
static volatile int small = -42;

static void scaled(const char *k, double v)
{
    /* round to 1/1000 without libm */
    double s = v * 1000.0;
    long long r = (long long)(s < 0 ? s - 0.5 : s + 0.5);
    out_str(k);
    out_str("=");
    if (r < 0) {
        out_str("-");
        r = -r;
    }
    out_u32((unsigned int)(r / 1000));
    out_str(".");
    unsigned int frac = (unsigned int)(r % 1000);
    if (frac < 100) out_str("0");
    if (frac < 10) out_str("0");
    out_u32(frac);
    out_nl();
}

static double poly(double x)
{
    return ((x * 0.5 - 1.25) * x + 3.0) * x - 0.75;
}

static double newton_sqrt(double v)
{
    double x = v;
    for (int i = 0; i < 30; i++) x = 0.5 * (x + v / x);
    return x;
}

/* fpatan computes atan2(ST1, ST0) with ST0 = x and ST1 = y: y is pushed first. The test runs the
 * instruction itself, so the operand order is the one the x87 defines. A swapped order gives the
 * wrong quadrant or angle at every point below. */
static double atan2_x87(double y, double x)
{
    double r;
    __asm__ volatile(
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fpatan\n\t"
        "fstpl %0\n\t"
        : "=m"(r)
        : "m"(y), "m"(x)
        : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    return r;
}

/* fstps under round-toward-zero (RC=3), the mode the game's audio library stores in. Prints
 * the float's bits, so a store rounded the wrong way shows as an off-by-one. */
static void fst32_chop(const char *k, double x)
{
    unsigned short chop = 0x0F7F, near = 0x037F;
    float f;
    __asm__ volatile("fldcw %2\n\tfldl %1\n\tfstps %0\n\tfldcw %3"
                     : "=m"(f) : "m"(x), "m"(chop), "m"(near) : "st");
    unsigned int b;
    __builtin_memcpy(&b, &f, 4);
    out_str(k);
    out_str("=");
    out_u32(b);
    out_nl();
}

int vt_main(void)
{
    scaled("add", da + db);
    scaled("sub", da - db);
    scaled("mul", da * dc);
    scaled("div", dc / db);
    scaled("neg", -da);
    scaled("poly", poly(da));
    scaled("sqrt", __builtin_sqrt(da + db));
    scaled("newton", newton_sqrt(7.0));
    scaled("f_add", (double)fa + (double)fb);
    scaled("i2d", (double)big / 1000.0);
    scaled("small", (double)small * 2.5);
    scaled("f2i", (double)(int)(dc * 1.5));

    int cnt = 0;
    for (int i = 0; i < 40; i++) {
        double x = (double)i * 0.25 - 4.0;
        if (x < da) cnt += 1;
        if (x > db) cnt += 2;
        if (x == 0.0) cnt += 4;
    }
    kv("fcmp", cnt);

    long double ld = 1.0L;
    for (int i = 1; i <= 10; i++) ld = ld * (long double)i;
    scaled("fact10", (double)ld);
    long double ldd = (long double)da * (long double)db;
    scaled("ld_mul", (double)ldd);

    /* fpatan in all four quadrants, with the axes and one point off the diagonal. */
    scaled("atan2_q1", atan2_x87(1.0, 2.0));
    scaled("atan2_q2", atan2_x87(1.0, -2.0));
    scaled("atan2_q3", atan2_x87(-1.0, -2.0));
    scaled("atan2_q4", atan2_x87(-1.0, 2.0));
    scaled("atan2_neg_x_axis", atan2_x87(0.0, -1.0));
    scaled("atan2_34", atan2_x87(3.0, 4.0));
    fst32_chop("chop_third", 1.0 / 3.0);
    fst32_chop("chop_neg_third", -1.0 / 3.0);
    fst32_chop("chop_tenth", 0.1);
    fst32_chop("chop_exact", 1.5);
    fst32_chop("chop_huge", 1e39);
    fst32_chop("chop_neg_huge", -1e39);
    fst32_chop("chop_tiny", -1e-50);
    return cnt & 0x7F;
}
