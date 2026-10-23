/* vt3native: hand-written replacements for hot translated functions.
 *
 * The translator emits a dispatcher for every address in recomp.py NATIVE_OVERRIDES. The
 * dispatcher calls vt_native_<addr>(c) first; returning 1 means the native body handled the
 * call and performed the guest `ret` itself (it must advance esp past the return address and
 * leave the callee-saved registers as the translated body would), returning 0 means the
 * dispatcher runs the translated body.
 *
 * The weak defaults keep a build with no override (the translator self-tests) linkable: the
 * generated test programs never reference these symbols, and if they did they would fall back.
 */
#include "vt3rt.h"

#include <stdlib.h>
#include <string.h>

__attribute__((weak)) int vt_native_on = 0;

__attribute__((weak)) int vt_native_00421e90(vt3_cpu *c)
{
    (void)c;
    return 0;
}
