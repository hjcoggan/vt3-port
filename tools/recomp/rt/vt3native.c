/* vt3native: hand-written replacements for hot translated functions.
 *
 * The translator emits a dispatcher for every address in recomp.py NATIVE_OVERRIDES. The
 * dispatcher calls vt_native_<addr>(c) first; returning 1 means the native body handled the
 * call and performed the guest `ret` itself (it must advance esp past the return address and
 * leave the callee-saved registers as the translated body would), returning 0 means the
 * dispatcher runs the translated body.
 *
 * VT3_NATIVE=1 uses the native body. VT3_NATIVE=check runs both: for the first
 * VT3_NATIVE_CHECKS calls (default 50) it runs the native body, saves the guest state, restores
 * the input, runs the translated body and compares. Execution keeps the translated result
 * while checking, so a mismatch can never change the game. After the checks, native is used
 * only if every call matched. The log goes to stderr, as the rest of the runtime's does.
 */
#include "vt3rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The translated bodies the dispatcher and check mode call. Weak so a test translation, which
 * has no native overrides, still links. */
extern void fn_00421e90_tr(vt3_cpu *c) __attribute__((weak));
extern void fn_006506a2(vt3_cpu *c);   /* D3DXVec3Normalize */
extern void fn_006506a8(vt3_cpu *c);   /* D3DXPlaneFromPoints */

/* vt3native.c is the one place the native body lives. */
static int native_body(vt3_cpu *c);

int vt_native_on = 0;

enum { NATIVE_OFF, NATIVE_ON, NATIVE_CHECK };
static int native_mode = NATIVE_OFF;
static int checks_left;
static int checks_done, checks_matched;
static int native_ready;             /* the body works; set by the port below */
static int in_check;

/* Snapshot of everything the function can write: the guest process heap (256 MiB from
 * VT3_GUEST_HEAP_BASE), the image's writable sections, and 64 KiB either side of the entry
 * esp. The 64 KiB covers the function's 0x35c-byte frame and the calls it makes.
 *
 * Input and result are copied separately. Checking runs the native body, saves its result,
 * restores the input, runs the translated body and compares the two results. */
typedef struct {
    uint32_t base, len;
    uint8_t *in;                     /* input snapshot */
    uint8_t *out;                    /* native result */
} vt_region;

enum { MAX_REGIONS = 12, STACK_PAD = 0x10000 };
static vt_region regions[MAX_REGIONS];
static int nregions;
static int regions_built;
static int regions_failed;

static uint16_t rd16g(uint32_t a) { uint16_t v; memcpy(&v, vt_mem + a, 2); return v; }
static uint32_t rd32g(uint32_t a) { uint32_t v; memcpy(&v, vt_mem + a, 4); return v; }
static float rdf32g(uint32_t a) { float v; memcpy(&v, vt_mem + a, 4); return v; }

static int add_region(uint32_t base, uint32_t len)
{
    if (!len || nregions == MAX_REGIONS) return -1;
    /* Fold the stack padding into the neighbouring image/heap ranges if they touch. */
    regions[nregions].base = base;
    regions[nregions].len = len;
    regions[nregions].in = NULL;
    regions[nregions].out = NULL;
    nregions++;
    return 0;
}

/* IMAGE_SCN_MEM_WRITE in the section table of the PE headers, which are part of image.bin. */
static void add_writable_sections(void)
{
    const uint32_t base = vt3_mod.image_base;
    if (rd16g(base) != 0x5a4d) {                       /* "MZ" */
        add_region(base, vt3_mod.image_size);
        return;
    }
    uint32_t pe = base + rd32g(base + 0x3cu);
    unsigned nsec = rd16g(pe + 6);
    unsigned opt = rd16g(pe + 0x14);
    uint32_t sh = pe + 0x18u + opt;
    int added = 0;
    for (unsigned i = 0; i < nsec && i < 32; i++, sh += 0x28) {
        uint32_t vsize = rd32g(sh + 8);
        uint32_t vaddr = rd32g(sh + 12);
        uint32_t rawsize = rd32g(sh + 16);
        uint32_t chars = rd32g(sh + 36);
        if (!(chars & 0x80000000u)) continue;          /* not writable */
        uint32_t len = vsize ? vsize : rawsize;
        if (!len) continue;
        if (vaddr + len > vt3_mod.image_size) len = vt3_mod.image_size - vaddr;
        if (add_region(base + vaddr, len) == 0) added = 1;
    }
    if (!added) add_region(base, vt3_mod.image_size);
}

static void build_regions(uint32_t esp)
{
    if (regions_built) return;
    regions_built = 1;

    add_region(0x20000000u, 256u << 20);              /* guest process heap */
    add_writable_sections();
    uint32_t lo = esp > STACK_PAD ? esp - STACK_PAD : 0;
    uint32_t hi = esp + STACK_PAD < esp ? 0xffffffffu : esp + STACK_PAD;
    add_region(lo, hi - lo);

    size_t total = 0;
    for (int i = 0; i < nregions; i++) {
        regions[i].in = malloc(regions[i].len);
        regions[i].out = malloc(regions[i].len);
        if (!regions[i].in || !regions[i].out) {
            regions_failed = 1;
            return;
        }
        total += 2u * (size_t)regions[i].len;
    }
    fprintf(stderr, "vt3 native: check mode: %d regions, %.1f MiB of snapshots, %d calls\n",
            nregions, (double)total / (1024.0 * 1024.0), checks_left);
}

static void snapshot(uint8_t *const *dst)
{
    for (int i = 0; i < nregions; i++)
        memcpy(dst[i], vt_mem + regions[i].base, regions[i].len);
}

static void restore(uint8_t *const *src)
{
    for (int i = 0; i < nregions; i++)
        memcpy(vt_mem + regions[i].base, src[i], regions[i].len);
}

/* Only the state the function's ABI promises to preserve, plus esp (the function does its own
 * ret). eax/ecx/edx and the SSE registers are caller-saved and dead at the return. */
static int cpu_same(const vt3_cpu *a, const vt3_cpu *b)
{
    return a->esp == b->esp && a->ebx == b->ebx && a->esi == b->esi &&
           a->edi == b->edi && a->ebp == b->ebp;
}

static int region_same(const vt_region *r, uint32_t *addr, uint32_t *want, uint32_t *got)
{
    const uint8_t *now = vt_mem + r->base;
    if (memcmp(now, r->out, r->len) == 0) return 1;
    for (uint32_t i = 0; i + 4 <= r->len; i += 4) {
        uint32_t a, b;
        memcpy(&a, now + i, 4);
        memcpy(&b, r->out + i, 4);
        if (a != b) {
            *addr = r->base + i;
            *want = b;
            *got = a;
            return 0;
        }
    }
    *addr = r->base;
    *want = *got = 0;
    return 0;
}

/* Run both bodies from the same input and compare. The translated result is left in place:
 * while checking, the game keeps running the semantics it ran before. Returns 1, because the
 * dispatcher must not run the translated body a second time. */
static int check_call(vt3_cpu *c)
{
    if (in_check) return native_body(c);
    if (regions_failed) { native_mode = NATIVE_OFF; return 0; }
    build_regions(c->esp);
    if (regions_failed) {
        fprintf(stderr, "vt3 native: check disabled: out of memory for snapshots\n");
        native_mode = NATIVE_OFF;
        return 0;
    }

    uint8_t *in[MAX_REGIONS], *out[MAX_REGIONS];
    for (int i = 0; i < nregions; i++) {
        in[i] = regions[i].in;
        out[i] = regions[i].out;
    }

    in_check = 1;
    vt3_cpu cpu_in = *c;
    snapshot(in);

    int handled = native_body(c);
    vt3_cpu cpu_nat = *c;
    snapshot(out);

    *c = cpu_in;
    restore(in);

    fn_00421e90_tr(c);                     /* translated result, left in the guest */
    in_check = 0;

    checks_done++;
    if (checks_left > 0) checks_left--;

    int ok = handled && cpu_same(c, &cpu_nat);
    uint32_t addr = 0, want = 0, got = 0;
    if (ok) {
        for (int i = 0; i < nregions; i++) {
            if (!region_same(&regions[i], &addr, &want, &got)) { ok = 0; break; }
        }
    }
    if (!ok) {
        if (!handled) {
            fprintf(stderr, "vt3 native: check %d: native body declined the call\n", checks_done);
        } else if (!cpu_same(c, &cpu_nat)) {
            fprintf(stderr, "vt3 native: check %d: cpu mismatch: esp %08x/%08x ebx %08x/%08x "
                    "esi %08x/%08x edi %08x/%08x ebp %08x/%08x (translated/native)\n",
                    checks_done, c->esp, cpu_nat.esp, c->ebx, cpu_nat.ebx,
                    c->esi, cpu_nat.esi, c->edi, cpu_nat.edi, c->ebp, cpu_nat.ebp);
        } else {
            float fw, fg;
            memcpy(&fw, &want, 4);
            memcpy(&fg, &got, 4);
            fprintf(stderr, "vt3 native: check %d: first difference at guest %08x: "
                    "translated %08x (%.9g) native %08x (%.9g)\n",
                    checks_done, addr, want, (double)fw, got, (double)fg);
        }
        fprintf(stderr, "vt3 native: check %d/%d failed: keeping the translated body\n",
                checks_done, checks_done + checks_left);
        native_mode = NATIVE_OFF;
        vt_native_on = 0;
        return 0;                          /* not reached: translated already ran */
    }

    checks_matched++;
    if (checks_left == 0) {
        native_mode = NATIVE_ON;
        fprintf(stderr, "vt3 native: %d/%d calls matched; using the native body\n",
                checks_matched, checks_done);
    }
    return 1;
}

int vt_native_00421e90(vt3_cpu *c)
{
    if (native_mode == NATIVE_CHECK) return check_call(c);
    if (native_mode == NATIVE_ON) return native_body(c);
    return 0;
}

/* A build with no native body (the translator self-tests) links the default below. */
__attribute__((weak)) int vt_native_00421e90_off(vt3_cpu *c)
{
    (void)c;
    return 0;
}

__attribute__((constructor)) static void native_init(void)
{
    const char *s = getenv("VT3_NATIVE");
    if (!s || !*s || strcmp(s, "0") == 0) return;
    int check = strcmp(s, "check") == 0;
    const char *n = getenv("VT3_NATIVE_CHECKS");
    checks_left = n && *n ? atoi(n) : 50;
    native_mode = check ? NATIVE_CHECK : NATIVE_ON;
    vt_native_on = 1;
    if (check && (!fn_00421e90_tr || !native_ready)) {
        fprintf(stderr, "vt3 native: %s unavailable; staying on the translated body\n",
                !fn_00421e90_tr ? "translated body" : "native body");
        native_mode = NATIVE_OFF;
        vt_native_on = 0;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Native body
 * ------------------------------------------------------------------------------------------- */

static int native_body(vt3_cpu *c)
{
    (void)c;
    return 0;
}
