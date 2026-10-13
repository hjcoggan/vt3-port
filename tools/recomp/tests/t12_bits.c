/* Bit operations and wide arithmetic: bsf/bsr, bswap, rol/ror, bt/bts/btr/btc,
 * 32x32 to 64 multiply (mul), signed multiply-high, 64-bit division through the
 * libgcc helper, and popcount.
 */
#include "tcommon.h"

unsigned long long __udivdi3(unsigned long long, unsigned long long);
long long __divdi3(long long, long long);

static volatile unsigned int ua = 0x80010F0Au, ub = 37u;
static volatile int sa = -700000, sb = 33;

static unsigned int rol(unsigned int x, unsigned int k)
{
    k &= 31u;
    return (x << k) | (x >> ((32u - k) & 31u));
}

static unsigned int ror(unsigned int x, unsigned int k)
{
    k &= 31u;
    return (x >> k) | (x << ((32u - k) & 31u));
}

int vt_main(void)
{
    kv("ctz", __builtin_ctz(ua));
    kv("clz", __builtin_clz(ua));
    kvx("bswap", __builtin_bswap32(ua));
    kvx("rol5", rol(ua, 5u));
    kvx("ror9", ror(ua, 9u));
    kv("popcnt", __builtin_popcount(ua));

    unsigned int mask = 0;
    for (unsigned int i = 0; i < 32; i += 3) {
        mask |= 1u << i;
    }
    kvx("mask", mask);
    unsigned int m2 = mask;
    for (unsigned int i = 0; i < 32; i += 5) {
        m2 &= ~(1u << i);
    }
    kvx("m2", m2);
    int hits = 0;
    for (unsigned int i = 0; i < 32; i++) {
        if (ua & (1u << i)) hits++;
    }
    kv("bt_hits", hits);

    /* 32x32 -> 64 unsigned product (mul) and signed product (imul) */
    unsigned long long p = (unsigned long long)ua * ub;
    kvx("mul_hi", (unsigned int)(p >> 32));
    kvx("mul_lo", (unsigned int)p);
    long long q = (long long)sa * (long long)sb;
    kvx("imul_hi", (unsigned int)((unsigned long long)q >> 32));
    kvx("imul_lo", (unsigned int)q);

    /* 64-bit division through libgcc */
    volatile unsigned long long big = 0xFEDCBA9876543210ull;
    volatile long long sbig = -987654321987654LL;
    kvx("udiv_hi", (unsigned int)(big / 1000003ull >> 32));
    kvx("udiv_lo", (unsigned int)(big / 1000003ull));
    kvx("udiv2", (unsigned int)(big % 65537ull));
    kvx("sdiv_lo", (unsigned int)(sbig / 12345));
    kvx("smod_lo", (unsigned int)(sbig % 12345));

    /* Shift by a variable amount on a value wider than 32 bits */
    volatile unsigned long long w = 0x0123456789ABCDEFull;
    for (int k = 0; k < 8; k++) {
        unsigned long long v = w >> (k * 7);
        unsigned long long v2 = w << (k * 5);
        kvx("w_shr", (unsigned int)v ^ (unsigned int)(v >> 32));
        kvx("w_shl", (unsigned int)v2 ^ (unsigned int)(v2 >> 32));
    }

    /* Bit-field-style masks and merges */
    unsigned int packed = 0;
    for (unsigned int i = 0; i < 8; i++) {
        packed = (packed & ~(0xFu << (i * 4))) | ((i * 5u & 0xFu) << (i * 4));
    }
    kvx("packed", packed);
    return (int)(packed & 0x7F);
}
