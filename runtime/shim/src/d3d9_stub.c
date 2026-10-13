/* Direct3D 9 entry point. Not implemented: logs and returns NULL. See win32/d3d9.h. */
#include "shim_internal.h"
#include "win32/d3d9.h"

GPTR WINAPI Direct3DCreate9(UINT sdk_version)
{
    (void)sdk_version;
    vt3s_unimpl("Direct3DCreate9");
    return 0;
}
