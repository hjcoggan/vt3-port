/* vt3rt: loader, indirect-call dispatch, and x87 helpers for recompiled code. */
#include "vt3rt.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Shim interface (vt3shim.c). */
void vt_shim_init(void);
vt3_fnptr vt_shim_lookup(const char *name);
void vt_shim_fini(void);

uint8_t *vt_mem = NULL;

#define VT_MEM_SIZE ((uint64_t)1 << 32)
#define VT_STACK_TOP 0x10000000u
#define VT_TEB 0x7FFD0000u
#define VT_TOKEN_BASE 0xFFFF0000u   /* import tokens, never memory */
#define VT_SENTINEL_RET 0xFFFFFFF0u /* return address for the entry call */

typedef struct {
    uint32_t addr;
    vt3_fnptr fn;
    const char *name;
} icent;

static icent *itab = NULL;
static size_t nitab = 0;
static int trace = 0;

uint64_t vt_rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void vt_die_at(uint32_t addr, uint32_t detail)
{
    fflush(stdout);
    fprintf(stderr, "vt3: fault at guest %08x (detail %08x)\n", addr, detail);
    exit(99);
}

void vt_unsupported_at(uint32_t addr, const char *text)
{
    fflush(stdout);
    fprintf(stderr, "vt3: unsupported instruction at %08x: %s\n", addr, text);
    exit(98);
}

static int icent_cmp(const void *a, const void *b)
{
    const icent *x = a, *y = b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

void vt_icall(vt3_cpu *c, uint32_t target)
{
    size_t lo = 0, hi = nitab;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (itab[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= nitab || itab[lo].addr != target) {
        fflush(stdout);
        fprintf(stderr, "vt3: indirect transfer to unknown guest address %08x\n", target);
        exit(97);
    }
    icent *e = &itab[lo];
    if (!e->fn) {
        fflush(stdout);
        fprintf(stderr, "vt3: unimplemented import %s\n", e->name);
        exit(96);
    }
    if (trace) fprintf(stderr, "vt3: call %s\n", e->name);
    e->fn(c);
}

/* ---- x87 ----------------------------------------------------------------- */

double vt_ldf32(uint32_t a)
{
    float f;
    memcpy(&f, vt_mem + a, 4);
    return f;
}

double vt_ldf64(uint32_t a)
{
    double d;
    memcpy(&d, vt_mem + a, 8);
    return d;
}

double vt_ldf80(uint32_t a)
{
    uint64_t m;
    uint16_t se;
    memcpy(&m, vt_mem + a, 8);
    memcpy(&se, vt_mem + a + 8, 2);
    int sign = se >> 15;
    int e = se & 0x7FFF;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7FFF) v = ((m << 1) == 0) ? INFINITY : NAN;
    else v = ldexp((double)m, e - 16383 - 63);
    return sign ? -v : v;
}

void vt_stf32(uint32_t a, double v)
{
    float f = (float)v;
    memcpy(vt_mem + a, &f, 4);
}

void vt_stf64(uint32_t a, double v)
{
    memcpy(vt_mem + a, &v, 8);
}

void vt_stf80(uint32_t a, double v)
{
    uint64_t m = 0;
    uint16_t se;
    int sign = signbit(v) ? 1 : 0;
    double av = fabs(v);
    if (av == 0.0) {
        se = 0;
    } else if (isinf(av)) {
        m = 0x8000000000000000ull;
        se = 0x7FFF;
    } else if (isnan(av)) {
        m = 0xC000000000000000ull;
        se = 0x7FFF;
    } else {
        int ex;
        double f = frexp(av, &ex);          /* f in [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);          /* explicit integer bit set */
        se = (uint16_t)(ex - 1 + 16383);
    }
    se |= (uint16_t)(sign << 15);
    memcpy(vt_mem + a, &m, 8);
    memcpy(vt_mem + a + 8, &se, 2);
}

double vt_round(double v, uint32_t rc)
{
    switch (rc & 3u) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

int64_t vt_fint(double v, uint32_t rc, int bits)
{
    double r = vt_round(v, rc);
    double lim = (double)((uint64_t)1 << (bits - 1));
    if (!(r >= -lim && r < lim)) {
        return bits == 16 ? (int64_t)INT16_MIN : bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
    }
    return (int64_t)r;
}

uint32_t vt_cvt_i32(double v, int tr)
{
    double r = tr ? trunc(v) : nearbyint(v);
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

uint32_t vt_fcmp(double a, double b)
{
    if (isnan(a) || isnan(b)) return 0x7u;
    if (a < b) return 0x1u;
    if (a == b) return 0x2u;
    return 0x0u;
}

vt_xmm vt_ldx(uint32_t a)
{
    vt_xmm v;
    memcpy(v.b, vt_mem + a, 16);
    return v;
}

void vt_stx(uint32_t a, vt_xmm v)
{
    memcpy(vt_mem + a, v.b, 16);
}

/* ---- loader -------------------------------------------------------------- */

static void put32(uint32_t a, uint32_t v)
{
    memcpy(vt_mem + a, &v, 4);
}

int vt3_run(const vt3_module *m, const char *image_path)
{
    trace = getenv("VT3_TRACE") != NULL;

    vt_mem = mmap(NULL, (size_t)VT_MEM_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (vt_mem == MAP_FAILED) {
        fprintf(stderr, "vt3: cannot reserve 4 GiB of guest memory: %s\n", strerror(errno));
        return 1;
    }

    FILE *f = fopen(image_path, "rb");
    if (!f) {
        fprintf(stderr, "vt3: cannot open %s\n", image_path);
        return 1;
    }
    size_t got = fread(vt_mem + m->image_base, 1, m->image_size, f);
    fclose(f);
    if (got != m->image_size) {
        fprintf(stderr, "vt3: %s is %zu bytes, expected %u\n", image_path, got, m->image_size);
        return 1;
    }

    /* TEB: the few fields the CRT and SEH read through FS. */
    put32(VT_TEB + 0x00, 0xFFFFFFFFu);          /* SEH chain end */
    put32(VT_TEB + 0x04, VT_STACK_TOP);         /* stack base */
    put32(VT_TEB + 0x08, VT_STACK_TOP - (1u << 20)); /* stack limit */
    put32(VT_TEB + 0x18, VT_TEB);               /* self */
    put32(VT_TEB + 0x30, VT_TEB + 0x1000);      /* PEB */

    vt_shim_init();

    /* Imports: each IAT slot gets a unique token. Calls through the slot land in
     * the icall table, which maps the token to a shim. */
    size_t nimp = m->nimps;
    icent *tab = calloc(m->nfns + nimp, sizeof *tab);
    if (!tab) return 1;
    size_t n = 0;
    for (size_t i = 0; i < m->nfns; i++) {
        tab[n].addr = m->fns[i].addr;
        tab[n].fn = m->fns[i].fn;
        tab[n].name = m->fns[i].name;
        n++;
    }
    for (size_t i = 0; i < nimp; i++) {
        const vt3_import *im = &m->imps[i];
        uint32_t tok = VT_TOKEN_BASE + 16u * (uint32_t)i;
        put32(im->slot, tok);
        tab[n].addr = tok;
        tab[n].fn = vt_shim_lookup(im->name);
        tab[n].name = im->name;
        n++;
    }
    qsort(tab, n, sizeof *tab, icent_cmp);
    itab = tab;
    nitab = n;

    vt3_cpu c = {0};
    c.esp = VT_STACK_TOP - 64;
    c.esp -= 4;
    put32(c.esp, VT_SENTINEL_RET);
    c.fs_base = VT_TEB;
    c.fcw = 0x37Fu;

    vt_icall(&c, m->entry);
    /* The entry is expected to call ExitProcess. Returning means it did not. */
    fflush(stdout);
    vt_shim_fini();
    return 0;
}
