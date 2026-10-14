/* Dispatch tails of the kind the CRT memcpy uses.
 *
 * Two shapes, each with a table size given by a TABLE note (the harness turns these notes into a
 * table-size file for the translator; see run_tests.py):
 *
 *   vt_tail_a: the index is x & 3, read at table + 4 * index. Index 0 is a junk word (not code),
 *              so the translator must mark it unreachable rather than reject the table. The test
 *              only calls with x = 1..3.
 *   vt_tail_b: the index is x - 4, that is -4..-1, so the table lies below its base label. The
 *              translator must read the entries from table + 4 * index with a negative index.
 *
 * Exit code: 42 when sum(vt_tail_a(1..3)) = 66 and sum(vt_tail_b(0..3)) = 34, otherwise 1.
 */
#include "tcommon.h"

/* TABLE _vt_tail_a_jmp 4 */
/* TABLE _vt_tail_b_jmp 4 -4 */

/* cdecl: the argument is at 4(%esp) when each function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _vt_tail_a\n"
    "_vt_tail_a:\n"
    "    movl 4(%esp), %eax\n"
    "    andl $3, %eax\n"
    ".globl _vt_tail_a_jmp\n"
    "_vt_tail_a_jmp:\n"
    "    jmp *_vt_tail_a_tbl(,%eax,4)\n"
    "_vt_tail_a_c1:\n    movl $11, %eax\n    ret\n"
    "_vt_tail_a_c2:\n    movl $22, %eax\n    ret\n"
    "_vt_tail_a_c3:\n    movl $33, %eax\n    ret\n"
    ".p2align 2\n"
    "_vt_tail_a_tbl:\n"
    "    .long 0x12345678\n"                    /* index 0: junk, never reached by the test */
    "    .long _vt_tail_a_c1, _vt_tail_a_c2, _vt_tail_a_c3\n"

    ".p2align 4\n"
    ".globl _vt_tail_b\n"
    "_vt_tail_b:\n"
    "    movl 4(%esp), %ecx\n"
    "    subl $4, %ecx\n"
    ".globl _vt_tail_b_jmp\n"
    "_vt_tail_b_jmp:\n"
    "    jmp *_vt_tail_b_base(,%ecx,4)\n"
    "_vt_tail_b_c0:\n    movl $7, %eax\n    ret\n"
    "_vt_tail_b_c1:\n    movl $8, %eax\n    ret\n"
    "_vt_tail_b_c2:\n    movl $9, %eax\n    ret\n"
    "_vt_tail_b_c3:\n    movl $10, %eax\n    ret\n"
    ".p2align 2\n"
    "_vt_tail_b_tbl:\n"
    "    .long _vt_tail_b_c0, _vt_tail_b_c1, _vt_tail_b_c2, _vt_tail_b_c3\n"
    "_vt_tail_b_base:\n"                        /* index -4 is at base - 16 = _vt_tail_b_tbl */
);

int vt_tail_a(int x) __asm__("_vt_tail_a");
int vt_tail_b(int x) __asm__("_vt_tail_b");

int vt_main(void)
{
    int a = vt_tail_a(1) + vt_tail_a(2) + vt_tail_a(3);
    int b = vt_tail_b(0) + vt_tail_b(1) + vt_tail_b(2) + vt_tail_b(3);
    return (a == 66 && b == 34) ? 42 : 1;
}
