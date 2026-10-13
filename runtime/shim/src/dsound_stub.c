/* DirectSound entry point. Not implemented: reports no driver. See win32/dsound.h. */
#include "shim_internal.h"
#include "win32/dsound.h"

HRESULT WINAPI DirectSoundCreate(LPCGUID device, GPTR out_ds, LPUNKNOWN outer)
{
    (void)device;
    (void)out_ds;
    (void)outer;
    vt3s_unimpl("DirectSoundCreate");
    return DSERR_NODRIVER;
}
