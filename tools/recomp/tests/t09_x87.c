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
    return cnt & 0x7F;
}
