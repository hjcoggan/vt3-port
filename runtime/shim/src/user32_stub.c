/* user32: no window system in the runtime yet. MessageBoxA writes the text to
 * stderr and returns IDOK, so startup error boxes are visible without blocking. */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "win32/user32.h"

#include <stdio.h>
#include <string.h>

#define VT3_IDOK 1

int WINAPI MessageBoxA(HANDLE hwnd, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)hwnd; (void)type;
    size_t tcap = text ? vt3_guest_size() - text : 0;
    size_t ccap = caption ? vt3_guest_size() - caption : 0;
    const char *t = text ? vt3_g2h(text) : "";
    const char *c = caption ? vt3_g2h(caption) : "";
    fprintf(stderr, "MessageBoxA: [%.*s] %.*s\n",
            (int)strnlen(c, ccap), c, (int)strnlen(t, tcap), t);
    vt3s_set_last_error(ERROR_SUCCESS);
    return VT3_IDOK;
}
