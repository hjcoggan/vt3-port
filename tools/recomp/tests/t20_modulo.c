/* Modulo dispatch: a computed jump through a table indexed by a remainder.
 *
 * vt_mod7 divides its argument by 7 and jumps through vt_mod7_table with the remainder, which is
 * 0..6. The table holds seven code addresses. The translator must bound the index from the div
 * (the remainder of a division by N is 0..N-1), or the jump traps at run time.
 *
 * Exit code: 42 when the sum over x = 0..20 is 2163 (each remainder occurs three times, and the
 * cases return 100..106), otherwise 1.
 */
#include "tcommon.h"

/* cdecl: the argument is at 4(%esp) when the function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _vt_mod7\n"
    "_vt_mod7:\n"
    "    movl 4(%esp), %eax\n"
    "    pushl %esi\n"
    "    movl $7, %esi\n"
    "    xorl %edx, %edx\n"
    "    divl %esi\n"
    "    popl %esi\n"
    "    movl _vt_mod7_table(,%edx,4), %eax\n"
    "    jmp *%eax\n"
    "_vt_mod7_c0:\n    movl $100, %eax\n    ret\n"
    "_vt_mod7_c1:\n    movl $101, %eax\n    ret\n"
    "_vt_mod7_c2:\n    movl $102, %eax\n    ret\n"
    "_vt_mod7_c3:\n    movl $103, %eax\n    ret\n"
    "_vt_mod7_c4:\n    movl $104, %eax\n    ret\n"
    "_vt_mod7_c5:\n    movl $105, %eax\n    ret\n"
    "_vt_mod7_c6:\n    movl $106, %eax\n    ret\n"
    ".p2align 2\n"
    "_vt_mod7_table:\n"
    "    .long _vt_mod7_c0, _vt_mod7_c1, _vt_mod7_c2, _vt_mod7_c3\n"
    "    .long _vt_mod7_c4, _vt_mod7_c5, _vt_mod7_c6\n"
);

int vt_mod7(int x) __asm__("_vt_mod7");

int vt_main(void)
{
    int sum = 0;
    for (int x = 0; x <= 20; x++) sum += vt_mod7(x);
    return sum == 2163 ? 42 : 1;
}
