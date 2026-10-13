/*
 * IDirect3DDevice9 methods. Each one is an SDK slot (d3d9_com.h), reached through the
 * stdcall adapters. State is recorded so later reads return what was set. Drawing,
 * clearing and presenting are not wired to the renderer yet: they succeed and do
 * nothing, and say so in the log the first time.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"

#include <stdlib.h>
#include <string.h>

#define PP_SIZE 56
#define VP_SIZE 24
#define TEXTURE_BYTES (512u * 1024u * 1024u) /* reported as available texture memory */

#define DEV(This) d3d9_dev_of(This)

static d3d9_device *d3d9_dev_of(GPTR This)
{
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_DEVICE);
    return h ? (d3d9_device *)h->data : NULL;
}

/* Vector constants are floats on the host. They are copied bitwise, not converted. */
static void copy_guest_floats(float *dst, GPTR src, uint32_t vec4s)
{
    const void *p = d3d9_gmem(src, vec4s * 16u);
    if (p) memcpy(dst, p, vec4s * 16u);
}

static void copy_host_floats(GPTR dst, const float *src, uint32_t vec4s)
{
    void *p = d3d9_gmem(dst, vec4s * 16u);
    if (p) memcpy(p, src, vec4s * 16u);
}

/* ---- IUnknown -------------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_QueryInterface(GPTR This, GPTR riid, GPTR ppvObject)
{
    if (!DEV(This)) return E_NOINTERFACE;
    return d3d9_query_interface(This, riid, ppvObject, d3d9_iid_device9);
}

DWORD WINAPI IDirect3DDevice9_AddRef(GPTR This)
{
    return d3d9_add_ref(This);
}

DWORD WINAPI IDirect3DDevice9_Release(GPTR This)
{
    return d3d9_release(This);
}

/* ---- device info ----------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_GetDirect3D(GPTR This, GPTR ppD3D9)
{
    d3d9_device *d = DEV(This);
    if (!d || !ppD3D9) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppD3D9, d->parent);
    d3d9_add_ref(d->parent);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetDeviceCaps(GPTR This, GPTR pCaps)
{
    if (!DEV(This) || !d3d9_gmem(pCaps, 304)) return D3DERR_CHK_INVALIDCALL;
    d3d9_fill_caps(pCaps);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetDisplayMode(GPTR This, UINT iSwapChain, GPTR pMode)
{
    if (!DEV(This) || iSwapChain != 0) return D3DERR_CHK_INVALIDCALL;
    uint8_t *p = d3d9_gmem(pMode, 16);
    if (!p) return D3DERR_CHK_INVALIDCALL;
    memset(p, 0, 16);
    d3d9_wr(pMode, 0, 640);
    d3d9_wr(pMode, 4, 480);
    d3d9_wr(pMode, 8, 60);
    d3d9_wr(pMode, 12, D3DFMT_CHK_X8R8G8B8);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetCreationParameters(GPTR This, GPTR pParameters)
{
    d3d9_device *d = DEV(This);
    if (!d || !d3d9_gmem(pParameters, 16)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pParameters, 0, d->adapter);
    d3d9_wr(pParameters, 4, d->devtype);
    d3d9_wr(pParameters, 8, d->hwnd);
    d3d9_wr(pParameters, 12, d->behavior);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_TestCooperativeLevel(GPTR This)
{
    return DEV(This) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

UINT WINAPI IDirect3DDevice9_GetAvailableTextureMem(GPTR This)
{
    return DEV(This) ? TEXTURE_BYTES : 0;
}

HRESULT WINAPI IDirect3DDevice9_EvictManagedResources(GPTR This)
{
    return DEV(This) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

HRESULT WINAPI IDirect3DDevice9_Reset(GPTR This, GPTR pPresentationParameters)
{
    d3d9_device *d = DEV(This);
    if (!d || !d3d9_gmem(pPresentationParameters, PP_SIZE)) return D3DERR_CHK_INVALIDCALL;
    for (int i = 0; i < 14; i++) d->pp[i] = d3d9_rd(pPresentationParameters, (uint32_t)(4 * i));
    return S_OK;
}

/* ---- frame ------------------------------------------------------------------ */

HRESULT WINAPI IDirect3DDevice9_Present(GPTR This, GPTR src_rect, GPTR dst_rect,
                                        GPTR dst_window_override, GPTR dirty_region)
{
    (void)src_rect;
    (void)dst_rect;
    (void)dst_window_override;
    (void)dirty_region;
    if (!DEV(This)) return D3DERR_CHK_INVALIDCALL;
    static int once;
    if (!once) {
        once = 1;
        VT3_WARN("Present: not wired to the renderer yet (frames are not shown)");
    }
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_BeginScene(GPTR This)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    if (d->scene_depth) return D3DERR_CHK_INVALIDCALL;
    d->scene_depth = 1;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_EndScene(GPTR This)
{
    d3d9_device *d = DEV(This);
    if (!d || !d->scene_depth) return D3DERR_CHK_INVALIDCALL;
    d->scene_depth = 0;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_Clear(GPTR This, DWORD rect_count, GPTR rects, DWORD flags,
                                      UINT color, DWORD z, DWORD stencil)
{
    (void)rect_count;
    (void)rects;
    (void)flags;
    (void)color;
    (void)z;
    (void)stencil;
    if (!DEV(This)) return D3DERR_CHK_INVALIDCALL;
    static int once;
    if (!once) {
        once = 1;
        VT3_WARN("Clear: not wired to the renderer yet");
    }
    return S_OK;
}

/* ---- render targets and viewport ------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_SetRenderTarget(GPTR This, DWORD RenderTargetIndex, GPTR pRenderTarget)
{
    d3d9_device *d = DEV(This);
    if (!d || RenderTargetIndex >= 4) return D3DERR_CHK_INVALIDCALL;
    d->rt[RenderTargetIndex] = pRenderTarget;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetRenderTarget(GPTR This, DWORD RenderTargetIndex, GPTR ppRenderTarget)
{
    d3d9_device *d = DEV(This);
    if (!d || RenderTargetIndex >= 4) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppRenderTarget, d->rt[RenderTargetIndex]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetDepthStencilSurface(GPTR This, GPTR pNewZStencil)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->depth = pNewZStencil;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetDepthStencilSurface(GPTR This, GPTR ppZStencilSurface)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppZStencilSurface, d->depth);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetViewport(GPTR This, GPTR viewport)
{
    d3d9_device *d = DEV(This);
    if (!d || !d3d9_gmem(viewport, VP_SIZE)) return D3DERR_CHK_INVALIDCALL;
    for (int i = 0; i < 6; i++) d->viewport[i] = d3d9_rd(viewport, (uint32_t)(4 * i));
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetViewport(GPTR This, GPTR pViewport)
{
    d3d9_device *d = DEV(This);
    if (!d || !d3d9_gmem(pViewport, VP_SIZE)) return D3DERR_CHK_INVALIDCALL;
    for (int i = 0; i < 6; i++) d3d9_wr(pViewport, (uint32_t)(4 * i), d->viewport[i]);
    return S_OK;
}

/* ---- states ----------------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_SetRenderState(GPTR This, UINT State, DWORD Value)
{
    d3d9_device *d = DEV(This);
    if (!d || State >= 256) return D3DERR_CHK_INVALIDCALL;
    d->rs[State] = Value;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetRenderState(GPTR This, UINT State, GPTR pValue)
{
    d3d9_device *d = DEV(This);
    if (!d || State >= 256 || !d3d9_gmem(pValue, 4)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pValue, 0, d->rs[State]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetTextureStageState(GPTR This, DWORD Stage, UINT Type, DWORD Value)
{
    d3d9_device *d = DEV(This);
    if (!d || Stage >= 8 || Type >= 33) return D3DERR_CHK_INVALIDCALL;
    d->tss[Stage][Type] = Value;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetTextureStageState(GPTR This, DWORD Stage, UINT Type, GPTR pValue)
{
    d3d9_device *d = DEV(This);
    if (!d || Stage >= 8 || Type >= 33 || !d3d9_gmem(pValue, 4)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pValue, 0, d->tss[Stage][Type]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetSamplerState(GPTR This, DWORD Sampler, UINT Type, DWORD Value)
{
    d3d9_device *d = DEV(This);
    if (!d || Sampler >= 16 || Type >= 14) return D3DERR_CHK_INVALIDCALL;
    d->samp[Sampler][Type] = Value;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetSamplerState(GPTR This, DWORD Sampler, UINT Type, GPTR pValue)
{
    d3d9_device *d = DEV(This);
    if (!d || Sampler >= 16 || Type >= 14 || !d3d9_gmem(pValue, 4)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pValue, 0, d->samp[Sampler][Type]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetFVF(GPTR This, DWORD FVF)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->fvf = FVF;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetFVF(GPTR This, GPTR pFVF)
{
    d3d9_device *d = DEV(This);
    if (!d || !d3d9_gmem(pFVF, 4)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pFVF, 0, d->fvf);
    return S_OK;
}

/* ---- textures, shaders, streams -------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_SetTexture(GPTR This, DWORD Stage, GPTR pTexture)
{
    d3d9_device *d = DEV(This);
    if (!d || Stage >= 16) return D3DERR_CHK_INVALIDCALL;
    d->textures[Stage] = pTexture;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetTexture(GPTR This, DWORD Stage, GPTR ppTexture)
{
    d3d9_device *d = DEV(This);
    if (!d || Stage >= 16) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppTexture, d->textures[Stage]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetVertexShader(GPTR This, GPTR pShader)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->vs = pShader;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetVertexShader(GPTR This, GPTR ppShader)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppShader, d->vs);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetPixelShader(GPTR This, GPTR pShader)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->ps = pShader;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetPixelShader(GPTR This, GPTR ppShader)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppShader, d->ps);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetVertexShaderConstantF(GPTR This, UINT reg_idx, GPTR data, UINT count)
{
    d3d9_device *d = DEV(This);
    if (!d || reg_idx + count > 256) return D3DERR_CHK_INVALIDCALL;
    copy_guest_floats(&d->vsc[reg_idx][0], data, count);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetVertexShaderConstantF(GPTR This, UINT StartRegister, GPTR pConstantData,
                                                         UINT Vector4fCount)
{
    d3d9_device *d = DEV(This);
    if (!d || StartRegister + Vector4fCount > 256) return D3DERR_CHK_INVALIDCALL;
    copy_host_floats(pConstantData, &d->vsc[StartRegister][0], Vector4fCount);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetPixelShaderConstantF(GPTR This, UINT reg_idx, GPTR data, UINT count)
{
    d3d9_device *d = DEV(This);
    if (!d || reg_idx + count > 224) return D3DERR_CHK_INVALIDCALL;
    copy_guest_floats(&d->psc[reg_idx][0], data, count);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetPixelShaderConstantF(GPTR This, UINT StartRegister, GPTR pConstantData,
                                                        UINT Vector4fCount)
{
    d3d9_device *d = DEV(This);
    if (!d || StartRegister + Vector4fCount > 224) return D3DERR_CHK_INVALIDCALL;
    copy_host_floats(pConstantData, &d->psc[StartRegister][0], Vector4fCount);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetStreamSource(GPTR This, UINT StreamNumber, GPTR pStreamData,
                                                UINT OffsetInBytes, UINT Stride)
{
    d3d9_device *d = DEV(This);
    if (!d || StreamNumber >= 16) return D3DERR_CHK_INVALIDCALL;
    d->streams[StreamNumber][0] = pStreamData;
    d->streams[StreamNumber][1] = OffsetInBytes;
    d->streams[StreamNumber][2] = Stride;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetStreamSource(GPTR This, UINT StreamNumber, GPTR ppStreamData,
                                                GPTR OffsetInBytes, GPTR pStride)
{
    d3d9_device *d = DEV(This);
    if (!d || StreamNumber >= 16) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppStreamData, d->streams[StreamNumber][0]);
    vt3s_store_u32(OffsetInBytes, d->streams[StreamNumber][1]);
    vt3s_store_u32(pStride, d->streams[StreamNumber][2]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetIndices(GPTR This, GPTR pIndexData)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->indices = pIndexData;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetIndices(GPTR This, GPTR ppIndexData)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppIndexData, d->indices);
    return S_OK;
}
