/* Constant index: a jump table whose index is set by mov on every path into the dispatch.
 *
 * vt_constidx sets the index to 4 on one path and to 2 on the other, then jumps through a
 * five-entry table. Only entries 2 and 4 are reached. Nothing in the dispatch compares the index,
 * so the translator must take the bound from the constants on the paths (largest + 1 = 5).
 *
 * Exit code: 42 when vt_constidx(0) returns 22 and vt_constidx(1) returns 44, otherwise 1.
 */
#include "tcommon.h"

/* cdecl: the argument is at 4(%esp) when the function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _vt_constidx\n"
    "_vt_constidx:\n"
    "    movl 4(%esp), %ecx\n"
    "    testl %ecx, %ecx\n"
    "    jz 1f\n"
    "    movl $4, %eax\n"
    "    jmp 2f\n"
    "1:\n"
    "    movl $2, %eax\n"
    "2:\n"
    "    jmp *_vt_constidx_table(,%eax,4)\n"
    "_vt_constidx_c0:\n    movl $0, %eax\n    ret\n"
    "_vt_constidx_c1:\n    movl $1, %eax\n    ret\n"
    "_vt_constidx_c2:\n    movl $22, %eax\n    ret\n"
    "_vt_constidx_c3:\n    movl $3, %eax\n    ret\n"
    "_vt_constidx_c4:\n    movl $44, %eax\n    ret\n"
    ".p2align 2\n"
    "_vt_constidx_table:\n"
    "    .long _vt_constidx_c0, _vt_constidx_c1, _vt_constidx_c2\n"
    "    .long _vt_constidx_c3, _vt_constidx_c4\n"
);

int vt_constidx(int x) __asm__("_vt_constidx");

int vt_main(void)
{
    int a = vt_constidx(0);   /* index 2 */
    int b = vt_constidx(1);   /* index 4 */
    return (a == 22 && b == 44) ? 42 : 1;
}
