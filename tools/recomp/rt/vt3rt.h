/* vt3rt: runtime for recompiled x86-32 code. Included by generated C.
 *
 * Guest memory is one 4 GiB anonymous reservation at vt_mem. A guest address
 * is an offset into it, so a 32-bit pointer works on a 64-bit host. The
 * guest image sits at its ImageBase (0x400000 for these PE32 exes).
 */
#ifndef VT3RT_H
#define VT3RT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union vt_xmm {
    uint8_t b[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} vt_xmm;

/* Register file passed to every guest function. The GPRs live in C locals
 * inside a function. They are spilled here around calls and returns. */
typedef struct vt3_cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t fs_base;     /* linear base of FS (the TEB) */
    uint32_t ftop;        /* x87 top of stack, 0..7 */
    uint32_t fcw;         /* x87 control word */
    uint32_t fsw;         /* x87 status word, TOP field is kept in ftop */
    double fp[8];        /* x87 physical registers; st(i) = fp[(ftop+i)&7] */
    vt_xmm xmm[8];
} vt3_cpu;

typedef void (*vt3_fnptr)(vt3_cpu *c);

typedef struct vt3_fn {
    uint32_t addr;
    vt3_fnptr fn;
    const char *name;
} vt3_fn;

typedef struct vt3_import {
    uint32_t slot;        /* IAT slot address; the loader writes a token here */
    const char *dll;
    const char *name;
} vt3_import;

typedef struct vt3_module {
    const char *name;
    uint32_t image_base, image_size, entry;
    const char *image_file;
    const vt3_fn *fns;
    size_t nfns;
    const vt3_import *imps;
    size_t nimps;
} vt3_module;

extern const vt3_module vt3_mod;
extern uint8_t *vt_mem;

/* Load image_path into guest memory and run the entry. Returns process status. */
int vt3_run(const vt3_module *m, const char *image_path);

/* Guest memory access. Addresses are guest (32-bit). Little-endian host. */
static inline uint32_t rd8(uint32_t a) { return vt_mem[a]; }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, vt_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vt_mem + a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vt_mem + a, 8); return v; }
static inline void wr8(uint32_t a, uint32_t v) { vt_mem[a] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(vt_mem + a, &x, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(vt_mem + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(vt_mem + a, &v, 8); }

/* Sign-extend the low `bits` of v to 32 bits. */
static inline uint32_t vt_sx(uint32_t v, unsigned bits)
{
    return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits));
}

/* PF is 1 when the low byte has an even number of set bits. */
static inline uint32_t vt_parity(uint32_t v) { return !__builtin_parity(v & 0xFFu); }

uint64_t vt_rdtsc(void);

/* Indirect transfer: look up a guest address, then call it. */
void vt_icall(vt3_cpu *c, uint32_t target);

/* Abort with a message. The generated code uses these for CPU faults and for
 * control flow that could not be recovered statically. */
void vt_die_at(uint32_t addr, uint32_t detail) __attribute__((noreturn));
void vt_unsupported_at(uint32_t addr, const char *text) __attribute__((noreturn));

/* x87 support. Double precision internally. */
double vt_ldf32(uint32_t a);
double vt_ldf64(uint32_t a);
double vt_ldf80(uint32_t a);
void vt_stf32(uint32_t a, double v);
void vt_stf64(uint32_t a, double v);
void vt_stf80(uint32_t a, double v);
/* Round by rc (0 nearest, 1 down, 2 up, 3 truncate), then convert. */
double vt_round(double v, uint32_t rc);
int64_t vt_fint(double v, uint32_t rc, int bits);     /* FIST/FISTP/FISTTP */
uint32_t vt_cvt_i32(double v, int trunc);            /* CVT(T)SS/SD2SI */
uint32_t vt_fcmp(double a, double b);                /* bit0 CF, bit1 ZF, bit2 PF */

/* 128-bit memory access for SSE moves. */
vt_xmm vt_ldx(uint32_t a);
void vt_stx(uint32_t a, vt_xmm v);

/* ---- Macros used by generated code -------------------------------------- */

/* Load the eight GPRs from the cpu struct. Declares them. */
#define VT_LOAD() \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx; \
    uint32_t esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi
/* Declare the flags and the direction flag. Eager flags, dead code removed by cc. */
#define VT_FLAGS() \
    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, af = 0, df = 0

#define VT_SPILL() do { \
    c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; \
    c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)
#define VT_RELOAD() do { \
    eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; \
    esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)

#define FST(i) (c->fp[(c->ftop + (uint32_t)(i)) & 7u])
#define FPUSH(v) do { double vt_t_ = (v); c->ftop = (c->ftop - 1u) & 7u; c->fp[c->ftop] = vt_t_; } while (0)
#define FPOP() do { c->ftop = (c->ftop + 1u) & 7u; } while (0)

#endif /* VT3RT_H */
