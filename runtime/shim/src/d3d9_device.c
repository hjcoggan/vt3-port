/*
 * IDirect3DDevice9 methods. Each one is an SDK slot (d3d9_com.h), reached through the
 * stdcall adapters. Every creation method writes a valid object: a real one when the
 * shim can make it, otherwise a stub whose methods return E_NOTIMPL. Drawing goes
 * through d3d9_draw.c. Clear and Present reach the renderer when one is linked.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEV(This) d3d9_device_of(This)
#define PP_SIZE 56
#define VP_SIZE 24
#define TEXTURE_BYTES (512u * 1024u * 1024u)
#define D3DCLEAR_TARGET_CHK 1u
#define D3DCLEAR_ZBUFFER_CHK 2u
#define D3DUSAGE_RENDERTARGET_CHK 0x1u
#define D3DUSAGE_DEPTHSTENCIL_CHK 0x2u

d3d9_device *d3d9_device_of(GPTR dev)
{
    d3d9_host *h = d3d9_host_of(dev, D3D9_KIND_DEVICE);
    return h ? (d3d9_device *)h->data : NULL;
}

void d3d9_device_free(d3d9_device *d)
{
    free(d);   /* host allocation, made with calloc in CreateDevice */
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

/* ---- IUnknown is generated; device info ---------------------------------- */

HRESULT WINAPI IDirect3DDevice9_GetDirect3D(GPTR This, GPTR ppD3D9)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppD3D9, d->parent);
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
    if (!DEV(This) || iSwapChain != 0 || !d3d9_gmem(pMode, 16)) return D3DERR_CHK_INVALIDCALL;
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

/* ---- swap chains and the back buffer --------------------------------------- */

UINT WINAPI IDirect3DDevice9_GetNumberOfSwapChains(GPTR This)
{
    return DEV(This) ? 1 : 0;
}

HRESULT WINAPI IDirect3DDevice9_GetSwapChain(GPTR This, UINT iSwapChain, GPTR pSwapChain)
{
    d3d9_device *d = DEV(This);
    if (!d || iSwapChain != 0) { vt3s_store_u32(pSwapChain, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR sc = d3d9_new_stub(D3D9_KIND_SWAPCHAIN, This);   /* the swap chain is the device's */
    if (!sc) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(pSwapChain, sc);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateAdditionalSwapChain(GPTR This, GPTR pPresentationParameters, GPTR pSwapChain)
{
    (void)pPresentationParameters;
    if (!DEV(This)) { vt3s_store_u32(pSwapChain, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR sc = d3d9_new_stub(D3D9_KIND_SWAPCHAIN, This);
    if (!sc) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(pSwapChain, sc);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetBackBuffer(GPTR This, UINT iSwapChain, UINT iBackBuffer, UINT Type, GPTR ppBackBuffer)
{
    (void)Type;
    d3d9_device *d = DEV(This);
    if (!d || iSwapChain != 0 || iBackBuffer != 0) { vt3s_store_u32(ppBackBuffer, 0); return D3DERR_CHK_INVALIDCALL; }
    d3d9_return_obj(ppBackBuffer, d->backbuffer);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_Present(GPTR This, GPTR src_rect, GPTR dst_rect, GPTR dst_window_override,
                                        GPTR dirty_region)
{
    (void)src_rect; (void)dst_rect; (void)dst_window_override; (void)dirty_region;
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_gpu_present();
    /* VT3_TRACE_FPS=1 logs the frames presented each second, to check the game's frame pacing. */
    static int fps_on = -1;
    static uint32_t frames;
    static struct timespec window;
    if (fps_on < 0) {
        fps_on = getenv("VT3_TRACE_FPS") != NULL;
        clock_gettime(CLOCK_MONOTONIC, &window);
    }
    if (fps_on) {
        frames++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (double)(now.tv_sec - window.tv_sec) + (double)(now.tv_nsec - window.tv_nsec) * 1e-9;
        if (dt >= 1.0) {
            VT3_INFO("present: %.1f frames per second", frames / dt);
            frames = 0;
            window = now;
        }
    }
    return S_OK;
}

/* ---- frame and clears ------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_BeginScene(GPTR This)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    if (d->scene_depth) return D3DERR_CHK_INVALIDCALL;
    d->scene_depth = 1;
    d3d9_gpu_begin();
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
    (void)rect_count; (void)rects; (void)stencil;
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    if (d->rt[0] && d->rt[0] != d->backbuffer) return S_OK;   /* render-to-texture is not implemented; see d3d9_draw */
    union { uint32_t u; float f; } zb;
    zb.u = z;
    d3d9_gpu_clear(color, (flags & D3DCLEAR_TARGET_CHK) != 0, (flags & D3DCLEAR_ZBUFFER_CHK) != 0, zb.f);
    return S_OK;
}

/* ---- render targets and viewport ------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_SetRenderTarget(GPTR This, DWORD RenderTargetIndex, GPTR pRenderTarget)
{
    d3d9_device *d = DEV(This);
    if (!d || RenderTargetIndex >= 4) return D3DERR_CHK_INVALIDCALL;
    d->rt[RenderTargetIndex] = pRenderTarget;
    if (RenderTargetIndex == 0 && pRenderTarget && pRenderTarget != d->backbuffer) {
        static int once;
        if (!once) { once = 1; VT3_WARN("SetRenderTarget: rendering still goes to the back buffer"); }
    }
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetRenderTarget(GPTR This, DWORD RenderTargetIndex, GPTR ppRenderTarget)
{
    d3d9_device *d = DEV(This);
    if (!d || RenderTargetIndex >= 4) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppRenderTarget, d->rt[RenderTargetIndex]);
    return d->rt[RenderTargetIndex] ? S_OK : D3DERR_CHK_INVALIDCALL;
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
    d3d9_return_obj(ppZStencilSurface, d->depth);
    return d->depth ? S_OK : D3DERR_CHK_INVALIDCALL;
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

/* ---- scissor ---------------------------------------------------------------- */

/* Sends the scissor state to the renderer. The test is on while D3DRS_SCISSORTESTENABLE is set.
 * Until SetScissorRect is called, the rect is the whole target (the renderer clamps it). */
static void push_scissor(const d3d9_device *d)
{
    int32_t l = 0, t = 0, r = 65535, b = 65535;
    if (d->scissor_set) {
        l = d->scissor_rc[0];
        t = d->scissor_rc[1];
        r = d->scissor_rc[2];
        b = d->scissor_rc[3];
    }
    d3d9_gpu_set_scissor(d->rs[D3DRS_CHK_SCISSORTESTENABLE] != 0, l, t, r - l, b - t);
}

HRESULT WINAPI IDirect3DDevice9_SetScissorRect(GPTR This, GPTR pRect)
{
    d3d9_device *d = DEV(This);
    const void *rc = d3d9_gmem(pRect, 16);
    if (!d || !rc) return D3DERR_CHK_INVALIDCALL;
    memcpy(d->scissor_rc, rc, sizeof d->scissor_rc);
    d->scissor_set = 1;
    push_scissor(d);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetScissorRect(GPTR This, GPTR pRect)
{
    d3d9_device *d = DEV(This);
    void *out = d3d9_gmem(pRect, 16);
    if (!d || !out) return D3DERR_CHK_INVALIDCALL;
    if (d->scissor_set) {
        memcpy(out, d->scissor_rc, sizeof d->scissor_rc);
    } else {
        d3d9_surface *bb = d->backbuffer ? d3d9_surface_of(d->backbuffer) : NULL;
        int32_t full[4] = { 0, 0, bb ? (int32_t)bb->w : 0, bb ? (int32_t)bb->h : 0 };
        memcpy(out, full, sizeof full);
    }
    return S_OK;
}

/* ---- states ----------------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_SetRenderState(GPTR This, UINT State, DWORD Value)
{
    d3d9_device *d = DEV(This);
    if (!d || State >= 256) return D3DERR_CHK_INVALIDCALL;
    d->rs[State] = Value;
    if (State == D3DRS_CHK_SCISSORTESTENABLE) push_scissor(d);
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

/* ---- textures ---------------------------------------------------------------- */

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
    if (!d || Stage >= 16) { vt3s_store_u32(ppTexture, 0); return D3DERR_CHK_INVALIDCALL; }
    d3d9_return_obj(ppTexture, d->textures[Stage]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateTexture(GPTR This, UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                              UINT Format, UINT Pool, GPTR ppTexture, GPTR pSharedHandle)
{
    (void)pSharedHandle;
    if (!DEV(This) || !ppTexture) { vt3s_store_u32(ppTexture, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR t = d3d9_create_texture_obj(This, 0, Width, Height, Levels, Format, Usage, Pool);
    if (!t) {
        VT3_WARN("CreateTexture %ux%u fmt 0x%x: no storage; returning a stub", Width, Height, Format);
        t = d3d9_new_stub(D3D9_KIND_TEXTURE, This);
        if (!t) return E_OUTOFMEMORY_D3D9;
        vt3s_store_u32(ppTexture, t);
        return S_OK;
    }
    vt3s_store_u32(ppTexture, t);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateCubeTexture(GPTR This, UINT EdgeLength, UINT Levels, DWORD Usage,
                                                  UINT Format, UINT Pool, GPTR ppCubeTexture, GPTR pSharedHandle)
{
    (void)pSharedHandle;
    if (!DEV(This) || !ppCubeTexture) { vt3s_store_u32(ppCubeTexture, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR t = d3d9_create_texture_obj(This, 1, EdgeLength, EdgeLength, Levels, Format, Usage, Pool);
    if (!t) t = d3d9_new_stub(D3D9_KIND_CUBETEXTURE, This);
    if (!t) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppCubeTexture, t);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateVolumeTexture(GPTR This, UINT Width, UINT Height, UINT Depth, UINT Levels,
                                                    DWORD Usage, UINT Format, UINT Pool, GPTR ppVolumeTexture,
                                                    GPTR pSharedHandle)
{
    (void)Width; (void)Height; (void)Depth; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppVolumeTexture, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR t = d3d9_new_stub(D3D9_KIND_VOLUMETEXTURE, This);   /* volume textures are not modelled */
    if (!t) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppVolumeTexture, t);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateRenderTarget(GPTR This, UINT Width, UINT Height, UINT Format,
                                                   UINT MultiSample, DWORD MultisampleQuality, BOOL Lockable,
                                                   GPTR ppSurface, GPTR pSharedHandle)
{
    (void)MultiSample; (void)MultisampleQuality; (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppSurface, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR s = d3d9_create_surface_obj(This, Width, Height, Format, D3DUSAGE_RENDERTARGET_CHK, 0,
                                     Lockable != 0, 0);
    if (!s) s = d3d9_new_stub(D3D9_KIND_SURFACE, This);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSurface, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateDepthStencilSurface(GPTR This, UINT Width, UINT Height, UINT Format,
                                                          UINT MultiSample, DWORD MultisampleQuality, BOOL Discard,
                                                          GPTR ppSurface, GPTR pSharedHandle)
{
    (void)MultiSample; (void)MultisampleQuality; (void)Discard; (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppSurface, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR s = d3d9_create_surface_obj(This, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL_CHK, 0, 0, 0);
    if (!s) s = d3d9_new_stub(D3D9_KIND_SURFACE, This);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSurface, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateOffscreenPlainSurface(GPTR This, UINT Width, UINT Height, UINT Format,
                                                            UINT Pool, GPTR ppSurface, GPTR pSharedHandle)
{
    (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppSurface, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR s = d3d9_create_surface_obj(This, Width, Height, Format, 0, Pool, 1, 0);
    if (!s) s = d3d9_new_stub(D3D9_KIND_SURFACE, This);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSurface, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_StretchRect(GPTR This, GPTR src_surface, GPTR src_rect, GPTR dst_surface,
                                            GPTR dst_rect, UINT filter)
{
    (void)src_surface; (void)src_rect; (void)dst_surface; (void)dst_rect; (void)filter;
    return DEV(This) ? S_OK : D3DERR_CHK_INVALIDCALL;   /* copies are not modelled yet */
}

HRESULT WINAPI IDirect3DDevice9_ValidateDevice(GPTR This, GPTR pNumPasses)
{
    if (!DEV(This)) return D3DERR_CHK_INVALIDCALL;
    if (pNumPasses) d3d9_wr(pNumPasses, 0, 1);
    return S_OK;
}

/* ---- vertex and index buffers ------------------------------------------------ */

HRESULT WINAPI IDirect3DDevice9_CreateVertexBuffer(GPTR This, UINT Length, DWORD Usage, DWORD FVF, UINT Pool,
                                                   GPTR ppVertexBuffer, GPTR pSharedHandle)
{
    (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppVertexBuffer, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR b = d3d9_create_buffer_obj(This, D3D9_KIND_VB, Length, Usage, FVF, Pool);
    if (!b) b = d3d9_new_stub(D3D9_KIND_VB, This);
    if (!b) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppVertexBuffer, b);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateIndexBuffer(GPTR This, UINT Length, DWORD Usage, UINT Format, UINT Pool,
                                                  GPTR ppIndexBuffer, GPTR pSharedHandle)
{
    (void)pSharedHandle;
    if (!DEV(This)) { vt3s_store_u32(ppIndexBuffer, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR b = d3d9_create_buffer_obj(This, D3D9_KIND_IB, Length, Usage, Format, Pool);
    if (!b) b = d3d9_new_stub(D3D9_KIND_IB, This);
    if (!b) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppIndexBuffer, b);
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
    d3d9_return_obj(ppStreamData, d->streams[StreamNumber][0]);
    vt3s_store_u32(OffsetInBytes, d->streams[StreamNumber][1]);
    vt3s_store_u32(pStride, d->streams[StreamNumber][2]);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetIndices(GPTR This, GPTR pIndexData)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->indices = pIndexData;
    d3d9_buffer *b = d3d9_buffer_of(pIndexData);
    d->indices_format = b ? b->fvf_or_fmt : 0;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetIndices(GPTR This, GPTR ppIndexData)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppIndexData, d->indices);
    return S_OK;
}

/* ---- vertex declarations and shaders -------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_CreateVertexDeclaration(GPTR This, GPTR elements, GPTR declaration)
{
    if (!DEV(This)) { vt3s_store_u32(declaration, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR v = d3d9_create_decl_obj(This, elements);
    if (!v) v = d3d9_new_stub(D3D9_KIND_VDECL, This);
    if (!v) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(declaration, v);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetVertexDeclaration(GPTR This, GPTR pDecl)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d->decl = pDecl;
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_GetVertexDeclaration(GPTR This, GPTR ppDecl)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppDecl, d->decl);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateVertexShader(GPTR This, GPTR byte_code, GPTR shader)
{
    if (!DEV(This)) { vt3s_store_u32(shader, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR s = d3d9_create_shader_obj(This, 0, byte_code);
    if (!s) s = d3d9_new_stub(D3D9_KIND_VSHADER, This);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(shader, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreatePixelShader(GPTR This, GPTR byte_code, GPTR shader)
{
    if (!DEV(This)) { vt3s_store_u32(shader, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR s = d3d9_create_shader_obj(This, 1, byte_code);
    if (!s) s = d3d9_new_stub(D3D9_KIND_PSHADER, This);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(shader, s);
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
    d3d9_return_obj(ppShader, d->vs);
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
    d3d9_return_obj(ppShader, d->ps);
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

/* Bool constants: count BOOLs (4 bytes each) from the guest. Non-zero is true. */
static void copy_guest_bools(int *dst, GPTR src, UINT count, UINT limit, UINT first)
{
    if (first >= limit) return;
    if (count > limit - first) count = limit - first;
    const uint32_t *p = d3d9_gmem(src, count * 4u);
    if (!p) return;
    for (UINT i = 0; i < count; i++) dst[first + i] = p[i] != 0;
}

HRESULT WINAPI IDirect3DDevice9_SetVertexShaderConstantB(GPTR This, UINT reg_idx, GPTR data, UINT count)
{
    d3d9_device *d = DEV(This);
    if (!d || reg_idx + count > 16) return D3DERR_CHK_INVALIDCALL;
    copy_guest_bools(d->vsb, data, count, 16, reg_idx);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_SetPixelShaderConstantB(GPTR This, UINT reg_idx, GPTR data, UINT count)
{
    d3d9_device *d = DEV(This);
    if (!d || reg_idx + count > 16) return D3DERR_CHK_INVALIDCALL;
    copy_guest_bools(d->psb, data, count, 16, reg_idx);
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

/* ---- state blocks and queries ------------------------------------------------ */

HRESULT WINAPI IDirect3DDevice9_CreateStateBlock(GPTR This, UINT Type, GPTR ppSB)
{
    (void)Type;
    if (!DEV(This)) { vt3s_store_u32(ppSB, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR sb = d3d9_new_stub(D3D9_KIND_STATEBLOCK, This);   /* Capture and Apply succeed without effect */
    if (!sb) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSB, sb);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_BeginStateBlock(GPTR This)
{
    return DEV(This) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

HRESULT WINAPI IDirect3DDevice9_EndStateBlock(GPTR This, GPTR ppSB)
{
    if (!DEV(This)) { vt3s_store_u32(ppSB, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR sb = d3d9_new_stub(D3D9_KIND_STATEBLOCK, This);
    if (!sb) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSB, sb);
    return S_OK;
}

HRESULT WINAPI IDirect3DDevice9_CreateQuery(GPTR This, UINT Type, GPTR ppQuery)
{
    (void)Type;
    if (!DEV(This)) { vt3s_store_u32(ppQuery, 0); return D3DERR_CHK_INVALIDCALL; }
    GPTR q = d3d9_new_stub(D3D9_KIND_QUERY, This);
    if (!q) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppQuery, q);
    return S_OK;
}

/* ---- draws -------------------------------------------------------------------- */

HRESULT WINAPI IDirect3DDevice9_DrawPrimitive(GPTR This, UINT PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = PrimitiveType;
    a.prim_count = PrimitiveCount;
    a.start = StartVertex;
    return d3d9_draw(d, &a);
}

HRESULT WINAPI IDirect3DDevice9_DrawIndexedPrimitive(GPTR This, UINT arg0, INT BaseVertexIndex, UINT MinVertexIndex,
                                                     UINT NumVertices, UINT startIndex, UINT primCount)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = arg0;
    a.indexed = 1;
    a.base_vertex = BaseVertexIndex;
    a.min_vertex = MinVertexIndex;
    a.num_vertices = NumVertices;
    a.start = startIndex;
    a.prim_count = primCount;
    return d3d9_draw(d, &a);
}

HRESULT WINAPI IDirect3DDevice9_DrawPrimitiveUP(GPTR This, UINT primitive_type, UINT primitive_count, GPTR data,
                                                UINT stride)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = primitive_type;
    a.prim_count = primitive_count;
    a.up_vertices = vt3_g2h(data);
    a.up_stride = stride;
    return d3d9_draw(d, &a);
}

HRESULT WINAPI IDirect3DDevice9_DrawIndexedPrimitiveUP(GPTR This, UINT primitive_type, UINT min_vertex_idx,
                                                       UINT vertex_count, UINT primitive_count, GPTR index_data,
                                                       UINT index_format, GPTR data, UINT stride)
{
    d3d9_device *d = DEV(This);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = primitive_type;
    a.indexed = 1;
    a.min_vertex = min_vertex_idx;
    a.num_vertices = vertex_count;
    a.prim_count = primitive_count;
    a.up_indices = vt3_g2h(index_data);
    a.up_index_format = index_format;
    a.up_vertices = vt3_g2h(data);
    a.up_stride = stride;
    return d3d9_draw(d, &a);
}
