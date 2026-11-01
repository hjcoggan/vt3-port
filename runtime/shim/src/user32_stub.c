/* user32: no window system in the runtime yet. MessageBoxA writes the text to
 * stderr and returns the box's go-ahead answer, so startup error boxes are visible
 * without blocking. */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "win32/user32.h"

#include <stdio.h>
#include <string.h>

/* The guest string at s and the bytes left in the arena after it, or "" when s is not mapped. */
static const char *box_str(LPCSTR s, size_t *cap)
{
    if (!vt3s_guest_range_ok(s, 1)) { *cap = 0; return ""; }
    *cap = vt3_guest_size() - s;
    return vt3_g2h(s);
}

int WINAPI MessageBoxA(HANDLE hwnd, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)hwnd;
    size_t tcap, ccap;
    const char *t = box_str(text, &tcap);
    const char *c = box_str(caption, &ccap);
    fprintf(stderr, "MessageBoxA: [%.*s] %.*s\n",
            (int)strnlen(c, ccap), c, (int)strnlen(t, tcap), t);
    vt3s_set_last_error(ERROR_SUCCESS);
    /* Nobody answers, so take the answer that lets the game carry on; retry/cancel cancels,
     * since retrying a box that never blocks would spin. */
    switch (type & 0xFu) {
    case 2:  return 5;   /* MB_ABORTRETRYIGNORE: IDIGNORE */
    case 3:              /* MB_YESNOCANCEL */
    case 4:  return 6;   /* MB_YESNO: IDYES */
    case 5:  return 2;   /* MB_RETRYCANCEL: IDCANCEL */
    case 6:  return 11;  /* MB_CANCELTRYCONTINUE: IDCONTINUE */
    default: return 1;   /* MB_OK, MB_OKCANCEL: IDOK */
    }
}
