/* Win32 imports through the IAT and through a thunk. The dllimport calls go through
 * `call [IAT]`. The non-dllimport lstrlenA declaration goes through a `jmp [IAT]`
 * thunk that the linker generates.
 */
#include "tcommon.h"

int __stdcall lstrlenA(const char *s);

int vt_main(void)
{
    DWORD t0 = GetTickCount();
    DWORD t1 = GetTickCount();
    kv("tick_nonzero", t0 != 0);
    kv("tick_monotonic", t1 >= t0);

    void *heap = GetProcessHeap();
    kv("heap_nonnull", heap != 0);

    unsigned char *buf = HeapAlloc(heap, 8u, 256);
    kv("alloc_nonnull", buf != 0);
    unsigned int sum = 0;
    if (buf) {
        for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 3);
        for (int i = 0; i < 256; i++) sum += buf[i];
        kv("zeroed_then_filled", sum != 0);
    }
    kv("heapfree", HeapFree(heap, 0, buf) != 0);  /* BOOL upper bits are not defined */

    kv("len_direct", lstrlenA("virtua tennis"));
    kv("len_thunk", lstrlenA("recompiler"));

    /* Many calls to the same import: checks the stack balance across imports. */
    unsigned int acc = 0;
    for (int i = 0; i < 300; i++) {
        acc += (unsigned int)lstrlenA("abcdefg") + (unsigned int)i;
    }
    kv("loop_acc", (int)acc);
    return (int)(acc & 0x7F);
}
