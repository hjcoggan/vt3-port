/* Flags and conditional branches: every signed and unsigned comparison, setcc and
 * cmov results, and 64-bit add, sub, compare, and shift sequences, which use
 * adc, sbb, shld, and shrd.
 */
#include "tcommon.h"

static volatile int sv[6] = {-7, 0, 5, -2147483647 - 1, 2147483647, -1};
static volatile unsigned int uv[6] = {0u, 1u, 7u, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};

static int classify(int a, int b)
{
    int r = 0;
    if (a < b) r |= 1;
    if (a <= b) r |= 2;
    if (a > b) r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    return r;
}

static unsigned int classify_u(unsigned int a, unsigned int b)
{
    unsigned int r = 0;
    if (a < b) r |= 1u;
    if (a <= b) r |= 2u;
    if (a > b) r |= 4u;
    if (a >= b) r |= 8u;
    if (a == b) r |= 16u;
    if (a != b) r |= 32u;
    return r;
}

static int pick(int a, int b)
{
    int m = a > b ? a : b;
    int n = a < b ? a : b;
    return m * 3 - n;
}

static unsigned int bits_of(int a, int b)
{
    unsigned int r = 0;
    r |= (unsigned int)(a < b) << 0;
    r |= (unsigned int)(a == b) << 1;
    r |= (unsigned int)((unsigned int)a > (unsigned int)b) << 2;
    r |= (unsigned int)(a < 0) << 3;
    return r;
}

int vt_main(void)
{
    unsigned int total = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            int r = classify(sv[i], sv[j]);
            unsigned int ru = classify_u(uv[i], uv[j]);
            total = total * 31u + (unsigned int)r + ru;
            if (i == j) kv("same", r);
        }
    }
    kvx("total", total);
    kv("pick", pick(sv[0], sv[2]));
    kv("pick2", pick(sv[3], sv[4]));
    kvx("bits", bits_of(sv[0], sv[1]));
    kvx("bits2", bits_of(sv[3], sv[4]));

    /* 64-bit arithmetic: add/adc, sub/sbb, and the 64-bit compare. */
    volatile unsigned long long a = 0x0123456789ABCDEFull, b = 0xFEDCBA9876543210ull;
    unsigned long long s = a + b;
    unsigned long long d = a - b;
    kvx("s_hi", (unsigned int)(s >> 32));
    kvx("s_lo", (unsigned int)s);
    kvx("d_hi", (unsigned int)(d >> 32));
    kvx("d_lo", (unsigned int)d);
    kv("cmp64", a < b);
    kv("cmp64s", (long long)a < (long long)b);

    /* 64-bit shifts with variable counts (shld, shrd) */
    for (int k = 0; k < 4; k++) {
        unsigned int c = (unsigned int)(k * 13 + 1);
        unsigned long long x = a << c;
        unsigned long long y = b >> c;
        kvx("shl_hi", (unsigned int)(x >> 32));
        kvx("shr_lo", (unsigned int)y);
    }

    /* Carry chain through a 64-bit loop. */
    unsigned long long acc = 0;
    for (int i = 0; i < 16; i++) {
        acc += (unsigned long long)uv[i & 5] * 0x100000001ull;
        acc ^= acc >> 7;
    }
    kvx("acc_hi", (unsigned int)(acc >> 32));
    kvx("acc_lo", (unsigned int)acc);
    return (int)(total & 0x7F);
}
