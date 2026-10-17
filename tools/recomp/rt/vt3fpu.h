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
 */
#ifndef VT3FPU_H
#define VT3FPU_H

#include <stdint.h>

/* The element type of the x87 register file: a host double, as vt3fpu.h explains. The name
 * is kept because the translated code declares `vt_x80 t = vt_fld32(...)` and such, at
 * ~22,000 sites, and regenerating it is not worth a rename. The 10-byte in-memory form of an
 * 80-bit value is private to vt3fpu.c and is not this. */
typedef double vt_x80;

/* Compare. Returns bit0 CF (less), bit1 ZF (equal), bit2 PF (unordered). */
uint32_t vt_fcmp(double a, double b);

/* Arithmetic. cw is the x87 control word; the result is rounded by RC as below. */
double vt_fadd(double a, double b, uint32_t cw);
double vt_fsub(double a, double b, uint32_t cw);
double vt_fmul(double a, double b, uint32_t cw);
double vt_fdiv(double a, double b, uint32_t cw);
double vt_fsqrt(double a, uint32_t cw);
double vt_frndint(double a, uint32_t cw);
double vt_fneg(double a);
double vt_fabs(double a);

/* Integer conversions. vt_fint returns the rounded integer, or the indefinite value
 * (the minimum integer of the width) when it does not fit. */
double vt_fild(int64_t v);
int64_t vt_fint(double a, uint32_t rc, int bits);

/* Status word C3 C2 C0 for FXAM (C1 is the sign). */
uint32_t vt_fxam(double a);

/* Memory forms. Loads widen exactly. Stores round to the width (RC only, as on x87). */
double vt_fld32(uint32_t a);
double vt_fld64(uint32_t a);
double vt_fld80(uint32_t a);        /* an 80-bit memory value, rounded to 53 bits */
void vt_fst32(uint32_t a, double x, uint32_t cw);
void vt_fst64(uint32_t a, double x, uint32_t cw);
void vt_fst80(uint32_t a, double x); /* widened exactly back to 80 bits */

/* FPU constants. */
double vt_fconst(int which);        /* see VT_FC_* below */

/* Transcendental functions, through the host libm. */
double vt_fdbl1(double a, uint32_t cw, int op);
double vt_fdbl2(double a, double b, uint32_t cw, int op);

enum { VT_FC_1, VT_FC_ZERO, VT_FC_PI, VT_FC_L2E, VT_FC_LN2, VT_FC_LG2, VT_FC_L2T };
enum { VT_FD_SIN, VT_FD_COS, VT_FD_TAN, VT_FD_F2XM1 };
enum { VT_FD2_PATAN, VT_FD2_YL2X, VT_FD2_SCALE, VT_FD2_PREM };

#endif /* VT3FPU_H */
