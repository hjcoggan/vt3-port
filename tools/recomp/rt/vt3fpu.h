/* vt3fpu: x87 extended precision (80-bit) as a soft float.
 *
 * Register values are kept as x87 extended values: a 64-bit significand with an explicit
 * integer bit, and a 16-bit sign and biased exponent. Every operation rounds its result
 * to the precision and rounding mode in the control word (PC bits 9:8, RC bits 11:10).
 * Windows defaults to extended precision and round to nearest, so cw = 0x037F.
 *
 * Arithmetic is exact apart from one rounding per operation. Transcendental functions go
 * through double precision and are not bit-exact.
 */
#ifndef VT3FPU_H
#define VT3FPU_H

#include <stdint.h>

typedef struct vt_x80 {
    uint64_t m;   /* significand, bit 63 is the explicit integer bit */
    uint16_t se;  /* bit 15 sign, bits 14:0 biased exponent (bias 16383) */
} vt_x80;

/* Compare. Returns bit0 CF (less), bit1 ZF (equal), bit2 PF (unordered). */
uint32_t vt_fcmp(vt_x80 a, vt_x80 b);

/* Arithmetic. cw is the x87 control word. The result is rounded to its PC and RC. */
vt_x80 vt_fadd(vt_x80 a, vt_x80 b, uint32_t cw);
vt_x80 vt_fsub(vt_x80 a, vt_x80 b, uint32_t cw);
vt_x80 vt_fmul(vt_x80 a, vt_x80 b, uint32_t cw);
vt_x80 vt_fdiv(vt_x80 a, vt_x80 b, uint32_t cw);
vt_x80 vt_fsqrt(vt_x80 a, uint32_t cw);
vt_x80 vt_frndint(vt_x80 a, uint32_t cw);
vt_x80 vt_fneg(vt_x80 a);
vt_x80 vt_fabs(vt_x80 a);

/* Integer conversions. vt_fint returns the rounded integer, or the indefinite value
 * (the minimum integer of the width) when it does not fit. */
vt_x80 vt_fild(int64_t v);
int64_t vt_fint(vt_x80 a, uint32_t rc, int bits);

/* Status word C3 C2 C0 for FXAM (C1 is the sign). */
uint32_t vt_fxam(vt_x80 a);

/* Memory forms. Loads widen exactly. Stores round to the width (RC only, as on x87). */
vt_x80 vt_fld32(uint32_t a);
vt_x80 vt_fld64(uint32_t a);
vt_x80 vt_fld80(uint32_t a);
void vt_fst32(uint32_t a, vt_x80 x, uint32_t cw);
void vt_fst64(uint32_t a, vt_x80 x, uint32_t cw);
void vt_fst80(uint32_t a, vt_x80 x);

/* Constants and double-precision bridges. */
vt_x80 vt_fconst(int which);     /* see VT_FC_* in vt3fpu.c */
vt_x80 vt_from_double(double d);
double vt_to_double(vt_x80 a);

/* Transcendental functions, through double precision. */
vt_x80 vt_fdbl1(vt_x80 a, uint32_t cw, int op);
vt_x80 vt_fdbl2(vt_x80 a, vt_x80 b, uint32_t cw, int op);

enum { VT_FC_1, VT_FC_ZERO, VT_FC_PI, VT_FC_L2E, VT_FC_LN2, VT_FC_LG2, VT_FC_L2T };
enum { VT_FD_SIN, VT_FD_COS, VT_FD_TAN, VT_FD_F2XM1 };
enum { VT_FD2_PATAN, VT_FD2_YL2X, VT_FD2_SCALE, VT_FD2_PREM };

#endif /* VT3FPU_H */
