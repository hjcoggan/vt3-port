/* Flags returned by a helper through ret.
 *
 * _vt_fr_h compares eax with edx and returns with the result only in the flags, the way
 * hand-written CRT helpers do. vt_fr_b reads CF after the call (past a flag-neutral mov) and
 * vt_fr_z reads ZF. The translator emits the call as a C call, so the flags must come back
 * with it: if the caller keeps its own stale flags, the exit code differs from Wine.
 */
#include "tcommon.h"

/* cdecl: the arguments are at 4(%esp) and 8(%esp) when each function starts. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    "_vt_fr_h:\n"
    "    cmpl %edx, %eax\n"
    "    ret\n"
    ".p2align 4\n"
    "_vt_fr_b:\n"                               /* CF: a < b unsigned */
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %edx\n"
    "    call _vt_fr_h\n"
    "    movl $0, %eax\n"
    "    setb %al\n"
    "    ret\n"
    ".p2align 4\n"
    "_vt_fr_z:\n"                               /* ZF: a == b */
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %edx\n"
    "    call _vt_fr_h\n"
    "    jne 1f\n"
    "    movl $1, %eax\n"
    "    ret\n"
    "1:  xorl %eax, %eax\n"
    "    ret\n"
);

int vt_fr_b(unsigned a, unsigned b) __asm__("_vt_fr_b");
int vt_fr_z(unsigned a, unsigned b) __asm__("_vt_fr_z");

int vt_main(void)
{
    int r = vt_fr_b(1, 2) * 1000 + vt_fr_b(2, 1) * 100 + vt_fr_z(3, 3) * 10 + vt_fr_z(3, 4);
    kv("flags", r);
    return r == 1010 ? 42 : 1;
}
