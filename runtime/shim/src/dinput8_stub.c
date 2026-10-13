/* DirectInput 8 entry point. Not implemented: returns E_NOTIMPL. See win32/dinput.h. */
#include "shim_internal.h"
#include "win32/dinput.h"

HRESULT WINAPI DirectInput8Create(GPTR hinst, DWORD version, LPCGUID riid, GPTR out, LPUNKNOWN outer)
{
    (void)hinst;
    (void)version;
    (void)riid;
    (void)out;
    (void)outer;
    vt3s_unimpl("DirectInput8Create");
    return E_NOTIMPL;
}
