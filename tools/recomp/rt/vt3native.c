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
#include <time.h>

extern size_t vt3_guest_size_;   /* runtime/src/core/guest.c */
/* Translated functions in vt3.c (the translator exports them for this file). */
extern void fn_00421e90_tr(vt3_cpu *c);
extern void fn_006506a2(vt3_cpu *c);   /* D3DXVec3Normalize */
extern void fn_006506a8(vt3_cpu *c);   /* D3DXPlaneFromPoints */

/* vt3native.c is the one place the native body lives. */
static int native_body(vt3_cpu *c);
static void native_init(void);

/* On until the first call reads VT3_NATIVE: Android loads vt3.env after constructors run. */
int vt_native_on = 1;

enum { NATIVE_OFF, NATIVE_ON, NATIVE_CHECK };
static int native_mode = -1;
static int checks_left;
static int checks_done, checks_matched;
static int in_check;
static double ns_native, ns_translated;   /* check mode: time in each body */

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

/* Snapshot of everything the function can write: the guest heap (from VT3_GUEST_HEAP_BASE up
 * to 1 GiB, so about 2 GiB of host memory while checking), the image's writable sections, and
 * 64 KiB either side of the entry esp. The lower stack padding is restored before the
 * translated run, but is not compared: the native body need not reproduce the dead frame.
 *
 * Input and result are copied separately. Checking runs the native body, saves its result,
 * restores the input, runs the translated body and compares the two results. */
typedef struct {
    uint32_t base, len;
    uint32_t compare;                /* skip dead bytes at the start of the stack region */
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
    regions[nregions].compare = 0;
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

    /* The guest heap: the shim allocates from 0x20000000 up (cloth buffers sit past 0x33000000). */
    uint64_t top = vt3_guest_size_ < 0x40000000u ? vt3_guest_size_ : 0x40000000u;
    add_region(0x20000000u, (uint32_t)(top - 0x20000000u));
    add_writable_sections();
    uint32_t lo = esp > STACK_PAD ? esp - STACK_PAD : 0;
    uint32_t hi = esp + STACK_PAD < esp ? 0xffffffffu : esp + STACK_PAD;
    add_region(lo, hi - lo);
    regions[nregions - 1].compare = esp - lo;

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
    const uint8_t *now = vt_mem + r->base + r->compare;
    const uint8_t *out = r->out + r->compare;
    uint32_t len = r->len - r->compare;
    if (memcmp(now, out, len) == 0) return 1;
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint32_t a, b;
        memcpy(&a, now + i, 4);
        memcpy(&b, out + i, 4);
        if (a != b) {
            *addr = r->base + r->compare + i;
            *want = b;
            *got = a;
            return 0;
        }
    }
    *addr = r->base + r->compare;
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

    double t0 = now_ns();
    int handled = native_body(c);
    ns_native += now_ns() - t0;
    vt3_cpu cpu_nat = *c;
    snapshot(out);

    *c = cpu_in;
    restore(in);

    t0 = now_ns();
    fn_00421e90_tr(c);                     /* translated result, left in the guest */
    ns_translated += now_ns() - t0;
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
        fprintf(stderr, "vt3 native: per call: native %.1f us, translated %.1f us\n",
                ns_native / 1e3 / checks_done, ns_translated / 1e3 / checks_done);
    }
    return 1;
}

int vt_native_00421e90(vt3_cpu *c)
{
    if (native_mode < 0) native_init();
    if (native_mode == NATIVE_CHECK) return check_call(c);
    if (native_mode == NATIVE_ON) return native_body(c);
    return 0;
}

static void native_init(void)
{
    const char *s = getenv("VT3_NATIVE");
    native_mode = NATIVE_OFF;
    vt_native_on = 0;
    if (!s || !*s || strcmp(s, "0") == 0) return;
    int check = strcmp(s, "check") == 0;
    const char *n = getenv("VT3_NATIVE_CHECKS");
    checks_left = n && *n ? atoi(n) : 50;
    native_mode = check ? NATIVE_CHECK : NATIVE_ON;
    vt_native_on = 1;
}

/* ---------------------------------------------------------------------------------------------
 * Native body
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    float x, y, z, w;
} cloth_v4;

typedef struct {
    float f[16];
} cloth_mat;

typedef struct {
    vt3_cpu *c;
    uint32_t frame;                  /* esp after the translated prologue */
    uint32_t obj;
    uint32_t source, dest;
    uint32_t weights;
} cloth_ctx;

static inline void cloth_put32(uint32_t a, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    wr32(a, u);
}

static inline cloth_v4 cloth_read4(uint32_t a)
{
    cloth_v4 v = { rdf32g(a), rdf32g(a + 4), rdf32g(a + 8), rdf32g(a + 12) };
    return v;
}

static inline void cloth_write4(uint32_t a, cloth_v4 v)
{
    cloth_put32(a, v.x);
    cloth_put32(a + 4, v.y);
    cloth_put32(a + 8, v.z);
    cloth_put32(a + 12, v.w);
}

static inline void cloth_write3(uint32_t a, cloth_v4 v)
{
    cloth_put32(a, v.x);
    cloth_put32(a + 4, v.y);
    cloth_put32(a + 8, v.z);
}

static inline void cloth_load_matrix(cloth_mat *m, uint32_t a)
{
    memcpy(m->f, vt_mem + a, sizeof(m->f));
}

/* The two addition orders used by the scalar SSE matrix chains. Keep each rounding point
 * explicit: these are not interchangeable dot-product expressions. */
static inline float cloth_dot_yzx(const cloth_mat *m, int row, cloth_v4 v)
{
    float r = v.y * m->f[row + 1];
    float t = v.z * m->f[row + 2];
    r = r + t;
    t = v.x * m->f[row];
    r = r + t;
    return r;
}

static inline float cloth_dot_xyz(const cloth_mat *m, int row, cloth_v4 v)
{
    float r = v.x * m->f[row];
    float t = v.y * m->f[row + 1];
    r = r + t;
    t = v.z * m->f[row + 2];
    r = r + t;
    return r;
}

static inline float cloth_point_yzx(const cloth_mat *m, int row, cloth_v4 v)
{
    float r = cloth_dot_yzx(m, row, v);
    r = r + m->f[row + 3];
    return r;
}

static inline float cloth_point_xyz(const cloth_mat *m, int row, cloth_v4 v)
{
    float r = cloth_dot_xyz(m, row, v);
    r = r + m->f[row + 3];
    return r;
}

static inline float cloth_blend(float v0, float v1, float v2, float v3,
                                const float w[4])
{
    float r = v1 * w[1];
    float t = v0 * w[0];
    r = r + t;
    t = v2 * w[2];
    r = r + t;
    t = v3 * w[3];
    r = r + t;
    return r;
}

static inline cloth_v4 cloth_blend4(const cloth_v4 v[4], const float w[4])
{
    cloth_v4 r;
    r.x = cloth_blend(v[0].x, v[1].x, v[2].x, v[3].x, w);
    r.y = cloth_blend(v[0].y, v[1].y, v[2].y, v[3].y, w);
    r.z = cloth_blend(v[0].z, v[1].z, v[2].z, v[3].z, w);
    r.w = 0.0f;
    return r;
}

static void cloth_load_skin(const cloth_ctx *s, cloth_mat m[4], float w[4], cloth_v4 *v)
{
    uint32_t a = s->source + rd8(s->obj + 7);
    for (int k = 0; k < 4; k++) w[k] = rdf32g(a + 4u * (uint32_t)k);

    a = s->source + rd8(s->obj + 8);
    for (int k = 0; k < 4; k++) {
        uint32_t ma = s->obj + 0x28u + (rd8(a + (uint32_t)k) << 6);
        cloth_load_matrix(&m[k], ma);
    }
    *v = cloth_read4(s->source + rd8(s->obj + 4));
}

static inline int cloth_greater(float a, float b)
{
    return (vt_fcmp_d((double)a, (double)b) & 3u) == 0;
}

static inline uint32_t cloth_byte(float v, float bias, float scale)
{
    v = v + bias;
    v = v * scale;
    return vt_cvt_i32((double)v, 1) & 0xffu;
}

static inline uint32_t cloth_pack3(cloth_v4 v, float bias, float scale)
{
    uint32_t r = cloth_byte(v.z, bias, scale);
    r = (r << 8) + cloth_byte(v.y, bias, scale);
    r = (r << 8) + cloth_byte(v.x, bias, scale);
    return r;
}

static inline uint32_t cloth_pack4(cloth_v4 v, float fourth, float bias, float scale)
{
    uint32_t r = cloth_byte(fourth, bias, scale);
    r = (r << 8) + cloth_byte(v.z, bias, scale);
    r = (r << 8) + cloth_byte(v.y, bias, scale);
    r = (r << 8) + cloth_byte(v.x, bias, scale);
    return r;
}

/* Make the same guest calls as the translated body, including its return addresses and
 * callee-entry esp. The vector lives at its original offset in the caller's guest frame. */
static void cloth_normalize(cloth_ctx *s, uint32_t off, uint32_t ret, cloth_v4 *v)
{
    uint32_t a = s->frame + off;
    cloth_put32(a, v->x);
    cloth_put32(a + 4, v->y);
    cloth_put32(a + 8, v->z);
    wr32(s->frame - 8, a);
    wr32(s->frame - 4, a);
    wr32(s->frame - 12, ret);
    s->c->esp = s->frame - 12;
    fn_006506a2(s->c);
    v->x = rdf32g(a);
    v->y = rdf32g(a + 4);
    v->z = rdf32g(a + 8);
}

static void cloth_plane(cloth_ctx *s, cloth_v4 p1, cloth_v4 p2, cloth_v4 p3,
                        cloth_v4 *out)
{
    const uint32_t o = s->frame + 0x264;
    const uint32_t a1 = s->frame + 0x6c;
    const uint32_t a2 = s->frame + 0x230;
    const uint32_t a3 = s->frame + 0xe0;
    cloth_write4(a1, p1);
    cloth_write4(a2, p2);
    cloth_write4(a3, p3);
    wr32(s->frame - 16, o);
    wr32(s->frame - 12, a1);
    wr32(s->frame - 8, a2);
    wr32(s->frame - 4, a3);
    wr32(s->frame - 20, 0x423fd8u);
    s->c->esp = s->frame - 20;
    fn_006506a8(s->c);
    out->x = rdf32g(o);
    out->y = rdf32g(o + 4);
    out->z = rdf32g(o + 8);
    out->w = 0.0f;
}

static float cloth_x87_float(vt3_cpu *c, uint32_t scratch, double v)
{
    if (((c->fcw >> 10) & 3u) == 0) return (float)v;
    vt_fst32(scratch, v, c->fcw);
    return rdf32g(scratch);
}

static void cloth_active_vertex(cloth_ctx *s, uint32_t index, uint32_t pos_addr,
                                uint32_t history_addr)
{
    cloth_mat m[4];
    cloth_v4 v[4];
    float w[4];
    cloth_load_skin(s, m, w, &v[0]);

    cloth_v4 source = v[0];
    for (int k = 0; k < 4; k++) {
        v[k].x = cloth_point_yzx(&m[k], 0, source);
        v[k].y = cloth_point_yzx(&m[k], 4, source);
        v[k].z = cloth_point_yzx(&m[k], 8, source);
    }
    cloth_v4 target = cloth_blend4(v, w);

    cloth_v4 normal_source = cloth_read4(s->source + rd8(s->obj + 6));
    v[0].x = cloth_dot_xyz(&m[0], 0, normal_source);
    v[0].y = cloth_dot_yzx(&m[0], 4, normal_source);
    v[0].z = cloth_dot_yzx(&m[0], 8, normal_source);
    for (int k = 1; k < 4; k++) {
        v[k].x = cloth_dot_yzx(&m[k], 0, normal_source);
        v[k].y = cloth_dot_yzx(&m[k], 4, normal_source);
        v[k].z = cloth_dot_yzx(&m[k], 8, normal_source);
    }
    cloth_v4 normal = cloth_blend4(v, w);
    cloth_normalize(s, 0x274, 0x422816u, &normal);

    float one = rdf32g(0x6cb84c);
    float pack_scale = rdf32g(0x6d91d4);
    uint32_t packed = cloth_pack4(normal, normal_source.w, one, pack_scale);
    float inertia = rdf32g(0x6d91e0);
    wr32(s->dest + rd8(s->obj + 0x1a), packed);

    cloth_v4 old = cloth_read4(pos_addr);
    cloth_v4 history = cloth_read4(history_addr);
    float base_x = old.x * inertia;
    base_x = base_x + old.x;
    float base_y = old.y * inertia;
    base_y = base_y + old.y;
    float base_z = old.z * inertia;
    base_z = base_z + old.z;
    float negative_inertia = rdf32g(0x6d91dc);
    float hx = history.x * negative_inertia;
    base_x = hx + base_x;
    float hy = history.y * negative_inertia;
    base_y = hy + base_y;
    base_y = base_y + rdf32g(0x95886c);
    float hz = history.z * negative_inertia;
    base_z = hz + base_z;

    cloth_write4(history_addr, old);
    float weight_scale = rdf32g(0x857b38);
    float weight = rdf32g(s->weights + index * 4u);
    if (cloth_greater(one, weight_scale)) weight = weight * weight_scale;

    cloth_v4 result;
    result.x = base_x - target.x;
    result.x = result.x * weight;
    result.x = result.x + target.x;
    result.y = base_y - target.y;
    result.y = result.y * weight;
    result.y = result.y + target.y;
    result.z = base_z - target.z;
    result.z = result.z * weight;
    result.z = result.z + target.z;
    result.w = one;
    cloth_write4(pos_addr, result);
}

static void cloth_tail_vertex(cloth_ctx *s, uint32_t pos_addr)
{
    cloth_mat m[4];
    cloth_v4 v[4];
    float w[4];
    cloth_load_skin(s, m, w, &v[0]);
    cloth_v4 source = v[0];

    v[0].x = cloth_point_yzx(&m[0], 0, source);
    v[0].y = cloth_point_xyz(&m[0], 4, source);
    v[0].z = cloth_point_xyz(&m[0], 8, source);
    v[1].x = cloth_point_yzx(&m[1], 0, source);
    v[1].y = cloth_point_xyz(&m[1], 4, source);
    v[1].z = cloth_point_yzx(&m[1], 8, source);
    v[2].x = cloth_point_yzx(&m[2], 0, source);
    v[2].y = cloth_point_yzx(&m[2], 4, source);
    v[2].z = cloth_point_yzx(&m[2], 8, source);
    v[3].x = cloth_point_yzx(&m[3], 0, source);
    v[3].y = cloth_point_yzx(&m[3], 4, source);
    v[3].z = cloth_point_xyz(&m[3], 8, source);
    cloth_v4 result = cloth_blend4(v, w);

    /* These four fourth-component temporaries are not initialized by the translated tail
     * path. It really does read the stale guest-frame slots, and the resulting W is stored.
     * Outside check mode the slots hold whatever an earlier call left, so W can drift from the
     * translated run; W never feeds X/Y/Z (springs, collisions and planes use it only for W,
     * and the vertex buffer gets X/Y/Z), so the output is unchanged. */
    float w0 = rdf32g(s->frame + 0x328);
    float w1 = rdf32g(s->frame + 0x2d8);
    float w2 = rdf32g(s->frame + 0x2c8);
    float w3 = rdf32g(s->frame + 0x23c);
    result.w = cloth_blend(w0, w1, w2, w3, w);
    cloth_write3(s->dest, result);
    cloth_write4(pos_addr, result);

    cloth_v4 normal_source = cloth_read4(s->source + rd8(s->obj + 5));
    v[0].x = cloth_dot_yzx(&m[0], 0, normal_source);
    v[0].y = cloth_dot_yzx(&m[0], 4, normal_source);
    v[0].z = cloth_dot_yzx(&m[0], 8, normal_source);
    v[1].x = cloth_dot_yzx(&m[1], 0, normal_source);
    v[1].y = cloth_dot_yzx(&m[1], 4, normal_source);
    v[1].z = cloth_dot_xyz(&m[1], 8, normal_source);
    v[2].x = cloth_dot_yzx(&m[2], 0, normal_source);
    v[2].y = cloth_dot_xyz(&m[2], 4, normal_source);
    v[2].z = cloth_dot_xyz(&m[2], 8, normal_source);
    v[3].x = cloth_dot_yzx(&m[3], 0, normal_source);
    v[3].y = cloth_dot_xyz(&m[3], 4, normal_source);
    v[3].z = cloth_dot_xyz(&m[3], 8, normal_source);
    cloth_v4 normal = cloth_blend4(v, w);
    cloth_normalize(s, 0x30, 0x42360du, &normal);
    float one = rdf32g(0x6cb84c);
    float pack_scale = rdf32g(0x6d91d4);
    wr32(s->dest + rd8(s->obj + 0x19), cloth_pack3(normal, one, pack_scale));

    normal_source = cloth_read4(s->source + rd8(s->obj + 6));
    for (int k = 0; k < 4; k++) {
        v[k].x = cloth_dot_xyz(&m[k], 0, normal_source);
        v[k].y = cloth_dot_xyz(&m[k], 4, normal_source);
        v[k].z = cloth_dot_xyz(&m[k], 8, normal_source);
    }
    normal = cloth_blend4(v, w);
    cloth_normalize(s, 0x80, 0x423abeu, &normal);
    one = rdf32g(0x6cb84c);
    pack_scale = rdf32g(0x6d91d4);
    wr32(s->dest + rd8(s->obj + 0x1a),
         cloth_pack4(normal, normal_source.w, one, pack_scale));
}

static void cloth_spring(vt3_cpu *c, uint32_t frame, uint32_t pos, uint32_t record)
{
    uint32_t a0 = pos + ((uint32_t)rd16(record - 4) << 4);
    uint32_t a1 = pos + ((uint32_t)rd16(record - 2) << 4);
    cloth_v4 p0 = cloth_read4(a0);
    cloth_v4 p1 = cloth_read4(a1);
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float dz = p1.z - p0.z;
    float dw = p1.w - p0.w;

    double zz = vt_fmul((double)dz, (double)dz, c->fcw);
    double yy = vt_fmul((double)dy, (double)dy, c->fcw);
    double length2 = vt_fadd(zz, yy, c->fcw);
    double xx = vt_fmul((double)dx, (double)dx, c->fcw);
    length2 = vt_fadd(length2, xx, c->fcw);
    float length = cloth_x87_float(c, frame + 0x40, vt_fsqrt(length2, c->fcw));
    float q = length - rdf32g(record);
    float denom = length + rdf32g(0x6d91d8);
    q = q / denom;
    float half = rdf32g(0x6cbac8);
    dx = dx * q;
    dy = dy * q;
    dz = dz * q;
    dx = dx * half;
    dy = dy * half;
    dz = dz * half;

    cloth_v4 r0, r1;
    r0.x = dx + p0.x;
    r0.y = p0.y + dy;
    r0.z = p0.z + dz;
    r0.w = dw + p0.w;
    r1.x = p1.x - dx;
    r1.y = p1.y - dy;
    r1.z = p1.z - dz;
    r1.w = p1.w - dw;
    cloth_write4(a0, r0);
    cloth_write4(a1, r1);
}

static void cloth_collide(vt3_cpu *c, uint32_t frame, uint32_t pos, uint32_t collider,
                          float radius, float radius2)
{
    cloth_v4 p = cloth_read4(pos);
    float dx = rdf32g(collider) - p.x;
    float dy = rdf32g(collider + 4) - p.y;
    float dz = rdf32g(collider + 8) - p.z;
    float dw = rdf32g(collider + 12) - p.w;
    float d2 = dx * dx;
    float t = dz * dz;
    d2 = d2 + t;
    t = dy * dy;
    d2 = d2 + t;
    if (!cloth_greater(0.0f, d2 - radius2)) return;

    /* fld/fsqrt/fstp: the input and the rounded result are both binary32. */
    double wide = (double)d2;
    float distance = cloth_x87_float(c, frame + 0x24, vt_fsqrt(wide, c->fcw));
    float q = distance - radius;
    q = q / distance;
    dx = dx * q;
    dy = dy * q;
    dz = dz * q;
    dw = dw * q;
    p.x = p.x + dx;
    p.y = dy + p.y;
    p.z = dz + p.z;
    p.w = dw + p.w;
    cloth_write4(pos, p);
}

static int native_body(vt3_cpu *c)
{
    const uint32_t entry_esp = c->esp;
    const uint32_t saved_ebx = c->ebx, saved_ebp = c->ebp;
    const uint32_t saved_esi = c->esi, saved_edi = c->edi;
    cloth_ctx s;
    s.c = c;
    s.frame = entry_esp - 0x36cu;
    s.obj = c->ecx;

    uint32_t mesh = rd32(s.obj + 0x107c);
    uint32_t face_count = rd32(mesh + 0x24);
    uint32_t triangles = rd32(mesh + 0x28);
    uint32_t adjacency = rd32(mesh + 0x2c);
    uint32_t positions = rd32(mesh + 0x18);
    uint32_t history = rd32(mesh + 0x14);
    uint32_t face_normals = rd32(mesh + 0x1c);
    s.weights = rd32(mesh + 4);
    s.source = rd32(s.obj + 0xc);
    s.dest = rd32(s.obj + 0x1c);

    if (rd8(s.obj + 7) == 0xffu || rd8(s.obj + 8) == 0xffu) goto done;

    uint32_t vertex_count = rd32(s.obj + 0x14);
    uint32_t index = 0;
    if (cloth_greater(rdf32g(s.weights), 0.0f)) {
        uint32_t history_delta = history - positions;
        uint32_t pos = positions;
        for (;;) {
            if (index >= vertex_count) break;
            cloth_active_vertex(&s, index, pos, pos + history_delta);
            s.dest += rd32(s.obj + 0x24);
            s.source += rd32(s.obj + 0x10);
            pos += 0x10;
            index++;
            if (!cloth_greater(rdf32g(s.weights + index * 4u), 0.0f)) break;
        }
    }
    uint32_t active_count = index;

    if (index < vertex_count) {
        uint32_t pos = positions + (index << 4);
        do {
            cloth_tail_vertex(&s, pos);
            s.source += rd32(s.obj + 0x10);
            s.dest += rd32(s.obj + 0x24);
            pos += 0x10;
            index++;
        } while (index < vertex_count);
    }

    /* Two spring/collider passes. All arithmetic here remains scalar binary32 except for
     * the explicitly modelled x87 distance calculations. */
    mesh = rd32(s.obj + 0x107c);
    uint32_t springs = rd32(mesh + 0xc);
    uint32_t spring_count = rd32(mesh + 8);
    uint32_t colliders = rd32(mesh + 0x38);
    uint32_t collider_count = rd32(mesh + 0x30);
    for (int pass = 0; pass < 2; pass++) {
        if (spring_count) {
            uint32_t record = springs + 4;
            for (uint32_t n = spring_count; n; n--, record += 8)
                cloth_spring(c, s.frame, positions, record);
        }
        if (collider_count) {
            uint32_t collider = colliders;
            for (uint32_t n = collider_count; n; n--, collider += 0x10) {
                float radius = rdf32g(collider + 0xc);
                float radius2 = radius * radius;
                for (uint32_t j = 0; j < active_count; j++) {
                    if (cloth_greater(rdf32g(s.weights + j * 4u), 0.0f))
                        cloth_collide(c, s.frame, positions + j * 0x10u,
                                      collider, radius, radius2);
                }
            }
        }
    }

    if (face_count) {
        uint32_t indices = triangles + 4;
        uint32_t out = face_normals;
        for (uint32_t n = face_count; n; n--, indices += 6, out += 0x10) {
            cloth_v4 p1 = cloth_read4(positions + ((uint32_t)rd16(indices - 4) << 4));
            cloth_v4 p2 = cloth_read4(positions + ((uint32_t)rd16(indices - 2) << 4));
            cloth_v4 p3 = cloth_read4(positions + ((uint32_t)rd16(indices) << 4));
            cloth_v4 plane;
            cloth_plane(&s, p1, p2, p3, &plane);
            cloth_write4(out, plane);
        }
    }

    if (active_count) {
        s.dest = rd32(s.obj + 0x1c);
        uint32_t pos = positions;
        uint32_t adjacent = adjacency;
        for (uint32_t n = active_count; n; n--, pos += 0x10, adjacent += 0x28) {
            cloth_v4 p = cloth_read4(pos);
            cloth_write3(s.dest, p);
            cloth_v4 sum = { 0.0f, 0.0f, 0.0f, 0.0f };
            uint32_t a = adjacent;
            for (uint32_t k = 0; k < 10; k++, a += 4) {
                uint32_t face = rd32(a);
                if (face == 0xffffffffu) break;
                uint32_t normal = face_normals + (face << 4);
                sum.x = sum.x + rdf32g(normal);
                sum.y = sum.y + rdf32g(normal + 4);
                sum.z = sum.z + rdf32g(normal + 8);
            }
            cloth_normalize(&s, 0x48, 0x42413fu, &sum);
            float one = rdf32g(0x6cb84c);
            float scale = rdf32g(0x6d91d4);
            wr32(s.dest + rd8(s.obj + 0x19), cloth_pack3(sum, one, scale));
            s.dest += rd32(s.obj + 0x24);
        }
    }

done:
    c->esp = entry_esp + 4;
    c->ebx = saved_ebx;
    c->ebp = saved_ebp;
    c->esi = saved_esi;
    c->edi = saved_edi;
    return 1;
}
