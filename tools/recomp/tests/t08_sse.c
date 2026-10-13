/* Scalar single and double precision in SSE. Built with -mfpmath=sse -msse2. Results
 * are printed as exact bit patterns, which is valid because IEEE single and double
 * results match between Wine and the native host.
 */
#include "tcommon.h"

static volatile float fa = 1.5f, fb = -0.25f, fc = 3.0f;
static volatile double da = 2.75, db = 1.0e-3, dc = -8.5;

static void fbits(const char *k, float f)
{
    union { float f; unsigned int u; } x;
    x.f = f;
    kvx(k, x.u);
}

static void dbits(const char *k, double d)
{
    union { double d; unsigned int u[2]; } x;
    x.d = d;
    kvx(k, x.u[1]);
    kvx(k, x.u[0]);
}

static float fsum(float *v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += v[i] * 0.5f;
    return s;
}

int vt_main(void)
{
    fbits("f_add", fa + fb);
    fbits("f_sub", fa - fb);
    fbits("f_mul", fa * fc);
    fbits("f_div", fa / fc);
    fbits("f_neg", -fb);
    fbits("f_sqrt", __builtin_sqrtf(fc));
    fbits("f_min", fa < fb ? fa : fb);
    fbits("f_max", fa > fb ? fa : fb);

    dbits("d_add", da + db);
    dbits("d_mul", da * dc);
    dbits("d_div", dc / da);
    dbits("d_sqrt", __builtin_sqrt(da));

    /* conversions */
    volatile int iv = -123456;
    volatile unsigned int uv = 3000000000u;
    fbits("i2f", (float)iv);
    dbits("i2d", (double)iv);
    dbits("u2d", (double)uv);
    kv("f2i_trunc", (int)(fa * 10.0f));
    kv("d2i_trunc", (int)(dc * 3.0));
    kv("f2i_neg", (int)(fb * 4.0f));
    dbits("f2d", (double)fa);
    fbits("d2f", (float)da);

    /* compares and branches on floats */
    int cnt = 0;
    for (int i = 0; i < 20; i++) {
        float x = (float)i * 0.37f - 3.0f;
        if (x < fa) cnt += 1;
        if (x > fb) cnt += 2;
        if (x == 0.0f) cnt += 4;
    }
    kv("fcmp_count", cnt);

    float arr[8];
    for (int i = 0; i < 8; i++) arr[i] = (float)(i * i) - 5.5f;
    fbits("fsum", fsum(arr, 8));

    /* a short accumulation that depends on rounding order */
    float acc = 0.0f;
    for (int i = 1; i < 200; i++) acc += 1.0f / (float)i;
    fbits("harmonic", acc);
    double dacc = 0.0;
    for (int i = 1; i < 200; i++) dacc += 1.0 / (double)i;
    dbits("harmonic_d", dacc);
    return (int)(acc) & 0x7F;
}
