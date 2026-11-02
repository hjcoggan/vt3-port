/* Flags returned by a helper, then read at the entry of the next function called.
 *
 * The shape of VT3's CRT acos (0x654e81): call a helper that returns status in ZF, then call a
 * function whose first flag use is a je on it (call leaves the flags alone). _vt_fc_h compares
 * eax with edx; _vt_fc_g returns 1 when ZF is set at its entry. If the helper's flags are not
 * handed back, or not handed on into the second call, the exit code differs from Wine.
 */
#include "tcommon.h"

__asm__(
    ".text\n"
    ".p2align 4\n"
    "_vt_fc_h:\n"
    "    cmpl %edx, %eax\n"
    "    ret\n"
    ".p2align 4\n"
    "_vt_fc_g:\n"
    "    push %edx\n"                           /* flag-neutral, as the CRT's push edx */
    "    je 1f\n"
    "    xorl %eax, %eax\n"
    "    pop %edx\n"
    "    ret\n"
    "1:  movl $1, %eax\n"
    "    pop %edx\n"
    "    ret\n"
    ".p2align 4\n"
    "_vt_fc:\n"
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %edx\n"
    "    call _vt_fc_h\n"
    "    call _vt_fc_g\n"
    "    ret\n"
);

int vt_fc(unsigned a, unsigned b) __asm__("_vt_fc");

int vt_main(void)
{
    int r = vt_fc(3, 3) * 10 + vt_fc(3, 4);
    kv("flags", r);
    return r == 10 ? 42 : 1;
}
