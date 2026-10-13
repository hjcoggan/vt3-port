/* Indirect calls: a table of cdecl function pointers, a comparator callback used by
 * an insertion sort, a stdcall function reached through a pointer (the callee pops
 * its own arguments), and a vtable-style struct of callbacks.
 */
#include "tcommon.h"

typedef int (*binop_t)(int, int);
typedef int (__attribute__((stdcall)) *stdfn_t)(int, int, int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int maxi(int a, int b) { return a > b ? a : b; }

static int __attribute__((stdcall)) s_mix(int a, int b, int c)
{
    return a * 100 + b * 10 + c;
}

static int cmp_asc(int a, int b) { return a - b; }
static int cmp_desc(int a, int b) { return b - a; }

static void isort(int *v, int n, int (*cmp)(int, int))
{
    for (int i = 1; i < n; i++) {
        int x = v[i];
        int j = i - 1;
        while (j >= 0 && cmp(v[j], x) > 0) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

typedef struct {
    int (*area)(const void *self);
    int (*scale)(void *self, int k);
    int w, h;
} shape_t;

static int rect_area(const void *self)
{
    const shape_t *s = self;
    return s->w * s->h;
}

static int rect_scale(void *self, int k)
{
    shape_t *s = self;
    s->w *= k;
    s->h *= k;
    return s->w + s->h;
}

int vt_main(void)
{
    binop_t ops[4] = {add, sub, mul, maxi};
    int acc = 1;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 4; i++) {
            acc = ops[i](acc, round + i + 2) & 0xFFFF;
        }
    }
    kv("ops_acc", acc);

    int v[10] = {5, -3, 12, 0, 7, 7, -9, 4, 1, 100};
    isort(v, 10, cmp_asc);
    for (int i = 0; i < 10; i++) kv("asc", v[i]);
    isort(v, 10, cmp_desc);
    kv("desc_first", v[0]);
    kv("desc_last", v[9]);

    stdfn_t fp = s_mix;
    int total = 0;
    for (int i = 0; i < 50; i++) total += fp(i, i + 1, i + 2);
    kv("stdcall_total", total);

    shape_t sh = {rect_area, rect_scale, 3, 4};
    kv("area", sh.area(&sh));
    kv("scaled", sh.scale(&sh, 3));
    kv("area2", sh.area(&sh));
    return total & 0x7F;
}
