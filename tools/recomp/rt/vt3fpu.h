/* vt3fpu: the guest's x87, run on the host double unit.
 *
 * The eight physical registers (vt3_cpu.fp) are host IEEE doubles and every arithmetic
 * instruction is one host instruction. That is a change of representation, not a change of
 * semantics for the values that matter here: x87 registers are 80-bit extended and a double
 * keeps 53, so bits 54..64 of each intermediate are dropped instead of being carried into the
 * next operation. Windows leaves the x87 at extended precision (cw = 0x037F) and this game's
 * operands arrive from float32 and float64 memory, so what is dropped is the tail of an
 * intermediate, never of an operand. Measured against the exact 80-bit implementation this
 * replaced, 0.019% of float32 stores differed, each by one ULP. See vt3fpu.c for the model.
 *
 * The control word's rounding mode is still honoured by the stores and the integer
 * conversions. Its precision control cannot be: there is nothing narrower than double to
 * round to. vt_fadd warns once if it ever sees a control word other than 0x037F.
 *
 * vt_x80 survives only as the in-memory form of an 80-bit value, which fld80/fst80 and the
 * register block of FNSAVE/FRSTOR convert to and from the double registers.
 *
 * The one-memory-access and one-arithmetic-op helpers are static inline here, and that is a
 * speed choice: vt3.c calls them at tens of thousands of sites and it is translated code, so a
 * definition in a .c file is a call the optimiser can never see through - out-of-line on the
 * Mac, out-of-line plus a PLT hop on Android. Three of them alone were 4.7% of Retroid CPU
 * (vt_ldf32, vt_fst32, vt_fld32). The rounded stores and the status-word forms, which need real
 * work or the x80 memory layout, stay out in vt3fpu.c.
 */
#ifndef VT3FPU_H
#define VT3FPU_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* The element type of the x87 register file: a host double, as this file explains. The name
 * is kept because the translated code declares `vt_x80 t = vt_fld32(...)` and such, at
 * ~22,000 sites, and regenerating it is not worth a rename. The 10-byte in-memory form of an
 * 80-bit value is private to vt3fpu.c and is not this. */
typedef double vt_x80;

enum { VT_FC_1, VT_FC_ZERO, VT_FC_PI, VT_FC_L2E, VT_FC_LN2, VT_FC_LG2, VT_FC_L2T };
enum { VT_FD_SIN, VT_FD_COS, VT_FD_TAN, VT_FD_F2XM1 };
enum { VT_FD2_PATAN, VT_FD2_YL2X, VT_FD2_SCALE, VT_FD2_PREM };

/* vt3rt.h declares this further down, but these inlines need it above that point. */
extern uint8_t *vt_mem;

/* ---- rounded stores and classification: out of line (vt3fpu.c) ---------------- */

void vt_fst32_rc(uint32_t a, double x, uint32_t cw);  /* RC other than round-to-nearest */
void vt_fst64_rc(uint32_t a, double x, uint32_t cw);
void vt_fbadcw(uint32_t cw);                          /* the once-only control-word warning */
double vt_frndint(double a, uint32_t cw);
int64_t vt_fint(double a, uint32_t rc, int bits);
uint32_t vt_fxam(double a);
double vt_fld80(uint32_t a);        /* an 80-bit memory value, rounded to 53 bits */
void vt_fst80(uint32_t a, double x); /* widened exactly back to 80 bits */
double vt_fdbl1(double a, uint32_t cw, int op);
double vt_fdbl2(double a, double b, uint32_t cw, int op);

/* ---- arithmetic and memory forms: inline ---- */

static inline double vt_fadd(double a, double b, uint32_t cw)
{
    if (cw != 0x37Fu) vt_fbadcw(cw);
    return a + b;
}
static inline double vt_fsub(double a, double b, uint32_t cw)
{
    if (cw != 0x37Fu) vt_fbadcw(cw);
    return a - b;
}
static inline double vt_fmul(double a, double b, uint32_t cw) { (void)cw; return a * b; }
static inline double vt_fdiv(double a, double b, uint32_t cw) { (void)cw; return a / b; }
static inline double vt_fsqrt(double a, uint32_t cw) { (void)cw; return sqrt(a); }
static inline double vt_fneg(double a) { return -a; }
static inline double vt_fabs(double a) { return fabs(a); }
static inline double vt_fild(int64_t v) { return (double)v; }

/* Compare. Returns bit0 CF (less), bit1 ZF (equal), bit2 PF (unordered). */
static inline uint32_t vt_fcmp(double a, double b)
{
    if (isnan(a) || isnan(b)) return 0x7u;
    if (a < b) return 0x1u;
    if (a == b) return 0x2u;
    return 0x0u;
}

/* Loads widen exactly. */
static inline vt_x80 vt_fld32(uint32_t a)
{
    float f;
    memcpy(&f, vt_mem + a, 4);
    return (double)f;                     /* a float32 widens to double exactly */
}
static inline vt_x80 vt_fld64(uint32_t a)
{
    double d;
    memcpy(&d, vt_mem + a, 8);
    return d;
}

/* Stores round to the width. Round-to-nearest is a plain conversion and toward-zero a
 * one-ulp fix-up; the other directed modes go out of line. */
static inline void vt_fst32(uint32_t a, double x, uint32_t cw)
{
    float f = (float)x;
    uint32_t rc = (cw >> 10) & 3u;
    if (rc == 3u) {
        /* Toward zero, which the audio library decodes in: step the nearest float one ulp back
         * toward zero when it overshot x. NaN compares false; an overflow to inf steps to FLT_MAX. */
        if (fabs((double)f) > fabs(x)) {
            uint32_t b;
            memcpy(&b, &f, 4);
            b--;
            memcpy(&f, &b, 4);
        }
    } else if (rc != 0u) {
        vt_fst32_rc(a, x, cw);
        return;
    }
    memcpy(vt_mem + a, &f, 4);
}
static inline void vt_fst64(uint32_t a, double x, uint32_t cw)
{
    if (((cw >> 10) & 3u) == 0u) memcpy(vt_mem + a, &x, 8);
    else vt_fst64_rc(a, x, cw);
}

/* FPU constants. */
static inline double vt_fconst(int which)
{
    switch (which) {
    case VT_FC_1:    return 1.0;
    case VT_FC_ZERO: return 0.0;
    case VT_FC_PI:   return 3.14159265358979323846;
    case VT_FC_L2E:  return 1.44269504088896340736;   /* log2 e */
    case VT_FC_LN2:  return 0.69314718055994530942;
    case VT_FC_LG2:  return 0.30102999566398119521;   /* log10 2 */
    case VT_FC_L2T:  return 3.32192809488736234787;   /* log2 10 */
    default: return 0.0;
    }
}

#endif /* VT3FPU_H */
