/* A function whose entry is not the lowest address in its body.
 *
 * vt_entry_under_test is the entry. Its body also contains vt_entry_low, which sits at a
 * lower address and is reached only by the jump in the entry. A translator must start the
 * generated C at the entry, not at the lowest address it reached. If it starts at the lowest
 * address, every call returns 7 instead of 42, and the exit code differs from Wine.
 */
#include "tcommon.h"

/* cdecl. The argument is at 4(%esp) when the function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_vt_entry_low:\n"
    "    movl $7, %eax\n"
    "    ret\n"
    ".p2align 4\n"
    ".globl _vt_entry_under_test\n"
    "_vt_entry_under_test:\n"
    "    movl 4(%esp), %eax\n"
    "    testl %eax, %eax\n"
    "    jz _vt_entry_low\n"
    "    movl $42, %eax\n"
    "    ret\n"
);

int vt_entry_under_test(int x) __asm__("_vt_entry_under_test");

int vt_main(void)
{
    int taken = vt_entry_under_test(0);     /* jz taken: the low block returns 7 */
    int fallen = vt_entry_under_test(1);    /* not taken: the entry returns 42 */
    return (taken == 7 && fallen == 42) ? 42 : 1;
}
