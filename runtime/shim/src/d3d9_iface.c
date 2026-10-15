/*
 * IDirect3D9 methods. Each one is an SDK slot (d3d9_com.h). The stdcall adapters
 * generated from the prototypes call these functions with guest-width arguments.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"

#include <stdlib.h>
#include <string.h>

/* Layouts from the SDK (see tests/sdk_layout_check.c). */
#define DM_SIZE 16          /* D3DDISPLAYMODE */
#define ID_SIZE 1100        /* D3DADAPTER_IDENTIFIER9 */
#define CAPS_SIZE 304       /* D3DCAPS9 */
#define DM_WIDTH 0
#define DM_HEIGHT 4
#define DM_REFRESH 8
#define DM_FORMAT 12
#define ID_DRIVER 0
#define ID_DESCRIPTION 512
#define ID_DEVICENAME 1024
#define CAPS_DEVICETYPE 0
#define CAPS_ADAPTERORDINAL 4
#define CAPS_TEXOPCAPS 144
#define CAPS_MAXTEXW 88
#define CAPS_MAXTEXH 92
#define CAPS_MAXVOLEXT 96
#define CAPS_MAXPRIMCOUNT 180
#define CAPS_MAXVERTEXIDX 184
#define CAPS_MAXSIMTEX 152
#define CAPS_MAXSTREAMS 188
#define CAPS_VSVER 196
#define CAPS_MAXVSCONST 200
#define CAPS_PSVER 204
#define PP_WORDS 14         /* D3DPRESENT_PARAMETERS is 56 bytes */

/* The one adapter and the one display mode this shim reports. */
#define ADAPTER_WIDTH 640
#define ADAPTER_HEIGHT 480
#define ADAPTER_HZ 60

static int format_supported(uint32_t f)
{
    switch (f) {
    case D3DFMT_CHK_X8R8G8B8: case D3DFMT_CHK_A8R8G8B8: case D3DFMT_CHK_R5G6B5:
    case D3DFMT_CHK_A1R5G5B5: case D3DFMT_CHK_A8: case D3DFMT_CHK_D24S8:
    case D3DFMT_CHK_INDEX16: case D3DFMT_CHK_G16R16F: case D3DFMT_CHK_A16B16G16R16F:
    case D3DFMT_CHK_DXT1: case D3DFMT_CHK_DXT3: case D3DFMT_CHK_DXT5:
        return 1;
    default:
        return 0;
    }
}

void d3d9_fill_caps(GPTR caps)
{
    uint8_t *p = d3d9_gmem(caps, CAPS_SIZE);
    if (!p) return;
    memset(p, 0, CAPS_SIZE);
    d3d9_wr(caps, CAPS_DEVICETYPE, D3DDEVTYPE_CHK_HAL);
    d3d9_wr(caps, CAPS_ADAPTERORDINAL, 0);
    /* Values the native shim can honour today. The renderer should confirm them. */
    d3d9_wr(caps, CAPS_MAXTEXW, 4096);
    d3d9_wr(caps, CAPS_MAXTEXH, 4096);
    d3d9_wr(caps, CAPS_MAXVOLEXT, 256);
    d3d9_wr(caps, CAPS_MAXSIMTEX, 16);
    d3d9_wr(caps, CAPS_MAXSTREAMS, 16);
    d3d9_wr(caps, CAPS_MAXVSCONST, 256);
    d3d9_wr(caps, CAPS_VSVER, 0xFFFE0200u);   /* vs_2_0: the only shader model the game uses */
    d3d9_wr(caps, CAPS_PSVER, 0xFFFF0200u);   /* ps_2_0 */
    d3d9_wr(caps, CAPS_MAXVERTEXIDX, 0x00FFFFFFu);
    d3d9_wr(caps, CAPS_MAXPRIMCOUNT, 0x00FFFFFFu);
    (void)CAPS_TEXOPCAPS;
}

/* ---- adapters and modes ----------------------------------------------------- */

HRESULT WINAPI IDirect3D9_RegisterSoftwareDevice(GPTR This, GPTR pInitializeFunction)
{
    (void)This;
    (void)pInitializeFunction;
    return S_OK;   /* no software device: accepted and ignored */
}

UINT WINAPI IDirect3D9_GetAdapterCount(GPTR This)
{
    (void)This;
    return 1;
}

HRESULT WINAPI IDirect3D9_GetAdapterIdentifier(GPTR This, UINT Adapter, DWORD Flags, GPTR pIdentifier)
{
    (void)This;
    (void)Flags;
    if (Adapter != 0) return D3DERR_CHK_INVALIDCALL;
    uint8_t *p = d3d9_gmem(pIdentifier, ID_SIZE);
    if (!p) return D3DERR_CHK_INVALIDCALL;
    memset(p, 0, ID_SIZE);
    strncpy((char *)p + ID_DRIVER, "vt3 native", 511);
    strncpy((char *)p + ID_DESCRIPTION, "vt3 native runtime (no GPU driver)", 511);
    strncpy((char *)p + ID_DEVICENAME, "\\\\.\\DISPLAY1", 31);
    return S_OK;
}

UINT WINAPI IDirect3D9_GetAdapterModeCount(GPTR This, UINT Adapter, UINT Format)
{
    (void)This;
    (void)Format;
    return Adapter == 0 ? 1 : 0;
}

HRESULT WINAPI IDirect3D9_EnumAdapterModes(GPTR This, UINT Adapter, UINT Format, UINT Mode, GPTR pMode)
{
    (void)This;
    if (Adapter != 0 || Mode != 0) return D3DERR_CHK_INVALIDCALL;
    uint8_t *p = d3d9_gmem(pMode, DM_SIZE);
    if (!p) return D3DERR_CHK_INVALIDCALL;
    memset(p, 0, DM_SIZE);
    d3d9_wr(pMode, DM_WIDTH, ADAPTER_WIDTH);
    d3d9_wr(pMode, DM_HEIGHT, ADAPTER_HEIGHT);
    d3d9_wr(pMode, DM_REFRESH, ADAPTER_HZ);
    d3d9_wr(pMode, DM_FORMAT, Format);
    return S_OK;
}

HRESULT WINAPI IDirect3D9_GetAdapterDisplayMode(GPTR This, UINT Adapter, GPTR pMode)
{
    (void)This;
    if (Adapter != 0) return D3DERR_CHK_INVALIDCALL;
    uint8_t *p = d3d9_gmem(pMode, DM_SIZE);
    if (!p) return D3DERR_CHK_INVALIDCALL;
    memset(p, 0, DM_SIZE);
    d3d9_wr(pMode, DM_WIDTH, ADAPTER_WIDTH);
    d3d9_wr(pMode, DM_HEIGHT, ADAPTER_HEIGHT);
    d3d9_wr(pMode, DM_REFRESH, ADAPTER_HZ);
    d3d9_wr(pMode, DM_FORMAT, D3DFMT_CHK_X8R8G8B8);
    return S_OK;
}

HRESULT WINAPI IDirect3D9_CheckDeviceType(GPTR This, UINT Adapter, UINT DevType, UINT AdapterFormat,
                                          UINT BackBufferFormat, BOOL bWindowed)
{
    (void)This;
    (void)AdapterFormat;
    (void)BackBufferFormat;
    (void)bWindowed;
    return (Adapter == 0 && DevType == D3DDEVTYPE_CHK_HAL) ? S_OK : D3DERR_CHK_NOTAVAILABLE;
}

HRESULT WINAPI IDirect3D9_CheckDeviceFormat(GPTR This, UINT Adapter, UINT DeviceType, UINT AdapterFormat,
                                            DWORD Usage, UINT RType, UINT CheckFormat)
{
    (void)This;
    (void)AdapterFormat;
    (void)Usage;
    (void)RType;
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_CHK_HAL) return D3DERR_CHK_NOTAVAILABLE;
    return format_supported(CheckFormat) ? S_OK : D3DERR_CHK_NOTAVAILABLE;
}

HRESULT WINAPI IDirect3D9_CheckDeviceMultiSampleType(GPTR This, UINT Adapter, UINT DeviceType,
                                                     UINT SurfaceFormat, BOOL Windowed,
                                                     UINT MultiSampleType, GPTR pQualityLevels)
{
    (void)This;
    (void)SurfaceFormat;
    (void)Windowed;
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_CHK_HAL) return D3DERR_CHK_NOTAVAILABLE;
    if (MultiSampleType != 0) return D3DERR_CHK_NOTAVAILABLE;   /* no multisampling yet */
    if (pQualityLevels) vt3s_store_u32(pQualityLevels, 1);
    return S_OK;
}

HRESULT WINAPI IDirect3D9_CheckDepthStencilMatch(GPTR This, UINT Adapter, UINT DeviceType,
                                                 UINT AdapterFormat, UINT RenderTargetFormat,
                                                 UINT DepthStencilFormat)
{
    (void)This;
    (void)AdapterFormat;
    (void)RenderTargetFormat;
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_CHK_HAL) return D3DERR_CHK_NOTAVAILABLE;
    return DepthStencilFormat == D3DFMT_CHK_D24S8 ? S_OK : D3DERR_CHK_NOTAVAILABLE;
}

HRESULT WINAPI IDirect3D9_CheckDeviceFormatConversion(GPTR This, UINT Adapter, UINT DeviceType,
                                                      UINT SourceFormat, UINT TargetFormat)
{
    (void)This;
    (void)SourceFormat;
    (void)TargetFormat;
    return (Adapter == 0 && DeviceType == D3DDEVTYPE_CHK_HAL) ? S_OK : D3DERR_CHK_NOTAVAILABLE;
}

HRESULT WINAPI IDirect3D9_GetDeviceCaps(GPTR This, UINT Adapter, UINT DeviceType, GPTR pCaps)
{
    (void)This;
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_CHK_HAL) return D3DERR_CHK_INVALIDCALL;
    if (!d3d9_gmem(pCaps, CAPS_SIZE)) return D3DERR_CHK_INVALIDCALL;
    d3d9_fill_caps(pCaps);
    return S_OK;
}

GPTR WINAPI IDirect3D9_GetAdapterMonitor(GPTR This, UINT Adapter)
{
    (void)This;
    (void)Adapter;
    return 0;   /* no monitor handle: the window system is not wired yet */
}

/* ---- devices ---------------------------------------------------------------- */

/* D3D9's documented initial render states, for the ones the shim uses. */
static void device_defaults(d3d9_device *d)
{
    d->rs[7] = 1;      /* D3DRS_ZENABLE */
    d->rs[14] = 1;     /* D3DRS_ZWRITEENABLE */
    d->rs[15] = 0;     /* D3DRS_ALPHATESTENABLE */
    d->rs[19] = 2;     /* D3DRS_SRCBLEND = ONE */
    d->rs[20] = 1;     /* D3DRS_DESTBLEND = ZERO */
    d->rs[22] = 3;     /* D3DRS_CULLMODE = CCW */
    d->rs[23] = 4;     /* D3DRS_ZFUNC = LESSEQUAL */
    d->rs[24] = 0;     /* D3DRS_ALPHAREF */
    d->rs[25] = 8;     /* D3DRS_ALPHAFUNC = ALWAYS */
    d->rs[27] = 0;     /* D3DRS_ALPHABLENDENABLE */
    d->rs[168] = 0xF;  /* D3DRS_COLORWRITEENABLE */
    d->rs[171] = 1;    /* D3DRS_BLENDOP = ADD */
    for (int s = 0; s < 8; s++) {
        d->tss[s][1] = s == 0 ? 4u : 1u;     /* COLOROP: MODULATE, then DISABLE */
        d->tss[s][2] = 2u;                   /* COLORARG1: TEXTURE */
        d->tss[s][3] = 0u;                   /* COLORARG2: DIFFUSE */
        d->tss[s][4] = s == 0 ? 4u : 1u;     /* ALPHAOP */
        d->tss[s][5] = 2u;                   /* ALPHAARG1 */
        d->tss[s][6] = 0u;                   /* ALPHAARG2 */
    }
    for (int s = 0; s < 16; s++) {
        d->samp[s][1] = 1;   /* ADDRESSU: WRAP */
        d->samp[s][2] = 1;   /* ADDRESSV: WRAP */
        d->samp[s][5] = 1;   /* MAGFILTER: POINT */
        d->samp[s][6] = 1;   /* MINFILTER: POINT */
        d->samp[s][7] = 0;   /* MIPFILTER: NONE */
    }
}

HRESULT WINAPI IDirect3D9_CreateDevice(GPTR This, UINT Adapter, UINT DeviceType, GPTR hFocusWindow,
                                       DWORD BehaviorFlags, GPTR pPresentationParameters,
                                       GPTR ppReturnedDeviceInterface)
{
    if (!d3d9_host_of(This, D3D9_KIND_D3D9)) return D3DERR_CHK_INVALIDCALL;
    if (Adapter != 0 || DeviceType != D3DDEVTYPE_CHK_HAL) return D3DERR_CHK_INVALIDCALL;
    if (!ppReturnedDeviceInterface || !d3d9_gmem(pPresentationParameters, 56)) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(ppReturnedDeviceInterface, 0);

    d3d9_device *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY_D3D9;
    d->parent = This;
    d->adapter = Adapter;
    d->devtype = DeviceType;
    d->hwnd = hFocusWindow;
    d->behavior = BehaviorFlags;
    for (int i = 0; i < 14; i++) d->pp[i] = d3d9_rd(pPresentationParameters, (uint32_t)(4 * i));
    device_defaults(d);

    GPTR dev = d3d9_obj_new(D3D9_KIND_DEVICE, d, This);
    if (!dev) {
        free(d);
        return E_OUTOFMEMORY_D3D9;
    }
    uint32_t w = d->pp[0] ? d->pp[0] : 640, h = d->pp[1] ? d->pp[1] : 480;
    uint32_t bbfmt = d->pp[2] ? d->pp[2] : D3DFMT_CHK_X8R8G8B8;
    d->gpu = d3d9_gpu_init(w, h) == 0;

    /* The back buffer and the auto depth-stencil are objects the game can fetch, so they exist from the start. */
    d->backbuffer = d3d9_create_surface_obj(dev, w, h, bbfmt, 0x1u, 0, 0, 1);
    if (!d->backbuffer) d->backbuffer = d3d9_new_stub(D3D9_KIND_SURFACE, dev);
    d->rt[0] = d->backbuffer;
    if (d->pp[9]) {          /* EnableAutoDepthStencil */
        d->depth = d3d9_create_surface_obj(dev, w, h, d->pp[10], 0x2u, 0, 0, 0);
        if (!d->depth) d->depth = d3d9_new_stub(D3D9_KIND_SURFACE, dev);
    }
    vt3s_store_u32(ppReturnedDeviceInterface, dev);
    VT3_INFO("CreateDevice: IDirect3DDevice9 at 0x%08x (%ux%u back buffer, %s)", dev, (unsigned)w,
             (unsigned)h, d->gpu ? "renderer" : "headless");
    return S_OK;
}
