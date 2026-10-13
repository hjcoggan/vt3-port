/*
 * D3DX 9 entry points that are not math. Shader compilation and file-based texture
 * loading are not available: they log once and fail. D3DXCreateTexture makes a real
 * texture through the device's own path. A failed texture creation still writes a
 * valid stub object, so the caller never gets NULL.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"
#include "win32/d3dx9.h"

#include <string.h>

#define E_FAIL_D3DX 0x80004005u

static void note_unavailable(const char *what)
{
    static const char *seen[8];
    static int n;
    for (int i = 0; i < n; i++)
        if (strcmp(seen[i], what) == 0) return;
    if (n < 8) seen[n++] = what;
    VT3_WARN("%s: not available in the native runtime (returns E_FAIL)", what);
}

HRESULT WINAPI D3DXCompileShader(GPTR src_data, UINT data_len, GPTR defines, GPTR include, GPTR function_name,
                                 GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs, GPTR ppConstantTable)
{
    (void)src_data; (void)data_len; (void)defines; (void)include; (void)function_name;
    (void)profile; (void)flags;
    note_unavailable("D3DXCompileShader");
    vt3s_store_u32(ppShader, 0);
    vt3s_store_u32(ppErrorMsgs, 0);
    vt3s_store_u32(ppConstantTable, 0);
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCompileShaderFromFileA(GPTR filename, GPTR defines, GPTR include, GPTR function_name,
                                          GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs,
                                          GPTR ppConstantTable)
{
    (void)filename; (void)defines; (void)include; (void)function_name; (void)profile; (void)flags;
    note_unavailable("D3DXCompileShaderFromFileA");
    vt3s_store_u32(ppShader, 0);
    vt3s_store_u32(ppErrorMsgs, 0);
    vt3s_store_u32(ppConstantTable, 0);
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXGetShaderConstantTable(GPTR byte_code, GPTR constant_table)
{
    (void)byte_code;
    note_unavailable("D3DXGetShaderConstantTable");
    vt3s_store_u32(constant_table, 0);
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCreateTexture(GPTR device, UINT width, UINT height, UINT miplevels, DWORD usage, UINT format,
                                 UINT pool, GPTR texture)
{
    if (!d3d9_device_of(device) || !texture) {
        vt3s_store_u32(texture, 0);
        return D3DERR_CHK_INVALIDCALL;
    }
    GPTR t = d3d9_create_texture_obj(device, 0, width, height, miplevels, format, usage, pool);
    if (!t) t = d3d9_new_stub(D3D9_KIND_TEXTURE, device);
    if (!t) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(texture, t);
    return S_OK;
}

HRESULT WINAPI D3DXCreateTextureFromFileInMemoryEx(GPTR device, GPTR srcdata, UINT srcdatasize, UINT width,
                                                   UINT height, UINT miplevels, DWORD usage, UINT format,
                                                   UINT pool, DWORD filter, DWORD mipfilter, DWORD colorkey,
                                                   GPTR srcinfo, GPTR palette, GPTR texture)
{
    (void)srcdata; (void)srcdatasize; (void)width; (void)height; (void)miplevels; (void)usage;
    (void)format; (void)pool; (void)filter; (void)mipfilter; (void)colorkey; (void)srcinfo; (void)palette;
    note_unavailable("D3DXCreateTextureFromFileInMemoryEx");
    GPTR t = d3d9_new_stub(D3D9_KIND_TEXTURE, device);
    vt3s_store_u32(texture, t);
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCreateCubeTextureFromFileInMemory(GPTR device, GPTR srcdata, UINT srcdatasize, GPTR texture)
{
    (void)srcdata; (void)srcdatasize;
    note_unavailable("D3DXCreateCubeTextureFromFileInMemory");
    GPTR t = d3d9_new_stub(D3D9_KIND_CUBETEXTURE, device);
    vt3s_store_u32(texture, t);
    return E_FAIL_D3DX;
}
