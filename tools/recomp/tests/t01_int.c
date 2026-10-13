/* Integer arithmetic at 8, 16, and 32 bits: add/sub/mul/div/mod, shifts, logic,
 * sign and zero extension, inc/dec, neg/not. Inputs come from volatile globals so
 * gcc does not fold the arithmetic away.
 */
#include "tcommon.h"

static volatile unsigned int u_a = 0xDEADBEEFu, u_b = 0x00012345u, u_c = 7u, u_d = 31u;
static volatile int s_a = -12345, s_b = 678, s_c = -1, s_d = 0x7FFFFFFF;
static volatile unsigned char b_a = 0xF3, b_b = 0x1C;
static volatile signed char sb_a = -93, sb_b = 37;
static volatile unsigned short w_a = 0xBEEF, w_b = 0x0123;
static volatile short sw_a = -30000, sw_b = 7;

int vt_main(void)
{
    unsigned int acc = 0;

    kvx("u_add", u_a + u_b);
    kvx("u_sub", u_a - u_b);
    kvx("u_mul", u_a * u_b);
    kvx("u_div", u_a / u_c);
    kvx("u_mod", u_a % u_c);
    kvx("u_shl", u_b << (u_d & 15u));
    kvx("u_shr", u_a >> (u_c + 3u));
    kvx("u_xor", u_a ^ u_b);
    kvx("u_and", u_a & u_b);
    kvx("u_or", u_a | u_b);
    kvx("u_not", ~u_a);
    kvx("u_neg", (unsigned int)-(int)u_b);

    kv("s_add", s_a + s_b);
    kv("s_sub", s_a - s_b);
    kv("s_mul", s_a * s_b);
    kv("s_div", s_a / s_b);
    kv("s_mod", s_a % s_b);
    kv("s_sar", s_a >> 3);
    kv("s_neg", -s_b);
    kv("s_div_neg1", s_d / s_c);
    kv("s_overflow_add", s_d + s_b);
    kv("s_mulhigh", (int)(((long long)s_a * (long long)s_d) >> 32));

    kv("b_add", (int)(unsigned char)(b_a + b_b));
    kv("b_sub", (int)(unsigned char)(b_a - b_b));
    kv("b_mul", (int)(unsigned char)(b_a * b_b));
    kv("b_div", (int)(b_a / b_b));
    kv("b_shr", (int)(b_a >> 2));
    kv("sb_mul", (int)(signed char)(sb_a * sb_b));
    kv("sb_div", (int)(sb_a / sb_b));
    kv("sb_mod", (int)(sb_a % sb_b));
    kv("sb_sar", (int)(sb_a >> 1));

    kvx("w_add", (unsigned short)(w_a + w_b));
    kvx("w_mul", (unsigned short)(w_a * w_b));
    kvx("w_div", w_a / w_b);
    kv("sw_mul", (int)(short)(sw_a * sw_b));
    kv("sw_div", (int)(short)(sw_a / sw_b));
    kv("sw_mod", (int)(short)(sw_a % sw_b));

    /* inc/dec and wraparound in each width */
    unsigned char bc = b_a;
    bc++;
    unsigned short wc = w_a;
    wc--;
    unsigned int uc = u_a;
    uc += 0x100u;
    kvx("inc8", bc);
    kvx("dec16", wc);
    kvx("add32", uc);

    /* Sign extension of the high byte of a register. */
    unsigned int reg = 0x12F3u;
    unsigned char hi = (unsigned char)(reg >> 8);
    kvx("hi8", hi);
    kv("sext8", (int)(signed char)(unsigned char)reg);

    for (unsigned int i = 0; i < 64; i++) {
        acc = acc * 33u + (u_a >> (i & 31u)) + (unsigned int)(s_a ^ (int)i);
    }
    kvx("acc", acc);
    return (int)(acc & 0x7F);
}
