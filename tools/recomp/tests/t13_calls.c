/* Calling conventions: stdcall with seven arguments (callee pops), cdecl with nine
 * arguments (caller pops), 64-bit arguments (two stack slots), a large struct
 * returned through a hidden pointer, a variadic cdecl function, and many calls to
 * check that the stack stays balanced.
 */
#include "tcommon.h"

#include <stdarg.h>

typedef struct {
    int v[7];
    long long big;
} wide_t;

static int __attribute__((stdcall)) sum7(int a, int b, int c, int d, int e, int f, int g)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

static int __attribute__((cdecl)) sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i)
{
    return a - b + c - d + e - f + g - h + i * 1000;
}

static long long mix64(int a, long long b, int c, unsigned long long d)
{
    return (long long)a * 3 + b - (long long)c + (long long)(d >> 1);
}

static wide_t make_wide(int seed)
{
    wide_t w;
    for (int i = 0; i < 7; i++) w.v[i] = seed * (i + 1);
    w.big = (long long)seed * 1000000007LL;
    return w;
}

static int vsum(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return s;
}

static int __attribute__((stdcall)) add_stdcall(int a, int b)
{
    return a + b;
}

int vt_main(void)
{
    kv("sum7", sum7(1, 2, 3, 4, 5, 6, 7));
    kv("sum9", sum9(1, 2, 3, 4, 5, 6, 7, 8, 9));

    long long m = mix64(5, 1234567890123LL, -9, 0x1FFFFFFFFFFull);
    kv("mix64_hi", (int)(m >> 32));
    kvx("mix64_lo", (unsigned int)m);

    wide_t w = make_wide(9);
    kv("wide_v3", w.v[3]);
    kv("wide_big_hi", (int)(w.big >> 32));
    kvx("wide_big_lo", (unsigned int)w.big);

    kv("vsum4", vsum(4, 10, 20, 30, 40));
    kv("vsum1", vsum(1, -5));

    int acc = 0;
    for (int i = 0; i < 200; i++) {
        acc += add_stdcall(i, sum7(i, 1, 0, 0, 0, 0, 0)) % 997;
    }
    kv("balance_acc", acc);
    kv("chain", sum9(acc, 1, 2, 3, 4, 5, 6, 7, 8));
    return acc & 0x7F;
}
