/* Flags carried into another function's entry by a fallthrough or a jump.
 *
 * vt_flag_b is a function entry (vt_main calls it) whose first instruction reads CF. vt_flag_a
 * falls through into it right after a cmp, and vt_flag_c jumps to it. The translator turns
 * both edges into tail calls, so the flags must survive them: if the callee starts with CF=0,
 * vt_flag_a(1, 2) and vt_flag_c(1, 2) return 0 instead of 1 and the exit code differs from Wine.
 */
#include "tcommon.h"

/* cdecl: the arguments are at 4(%esp) and 8(%esp) when each function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_vt_flag_c:\n"
    "    movl 4(%esp), %eax\n"
    "    cmpl 8(%esp), %eax\n"
    "    jmp _vt_flag_b\n"
    ".p2align 4\n"
    "_vt_flag_a:\n"
    "    movl 4(%esp), %eax\n"
    "    cmpl 8(%esp), %eax\n"
    "_vt_flag_b:\n"                             /* CF from the caller's cmp: a < b unsigned */
    "    sbbl %eax, %eax\n"
    "    negl %eax\n"
    "    ret\n"
);

int vt_flag_a(unsigned a, unsigned b) __asm__("_vt_flag_a");
int vt_flag_b(void) __asm__("_vt_flag_b");
int vt_flag_c(unsigned a, unsigned b) __asm__("_vt_flag_c");

int vt_main(void)
{
    (void)vt_flag_b();                          /* makes _vt_flag_b an entry; its result is undefined */
    int r = vt_flag_a(1, 2) * 1000 + vt_flag_a(2, 1) * 100 + vt_flag_c(1, 2) * 10 + vt_flag_c(2, 1);
    kv("flags", r);
    return r == 1010 ? 42 : 1;
}
