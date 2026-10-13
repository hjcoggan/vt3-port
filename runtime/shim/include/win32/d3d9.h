/*
 * Direct3D 9 interface sketch. This is the layout the shim will fill in.
 * Nothing is implemented yet: Direct3DCreate9 logs and returns NULL.
 *
 * How COM works here: a guest object is a block of guest memory whose first
 * word is a pointer to its vtable (also in guest memory). Each vtable slot
 * holds a guest address of a thunk. The thunk calls the matching host
 * function. The shim allocates and fills those thunks at init. That is the
 * recompiler's job to standardise, so the slots are plain GPTR values here.
 *
 * CHECK BEFORE USE: the method order below is reproduced from memory of the
 * DirectX 9 SDK d3d9.h. Verify each slot index against the SDK header before
 * relying on it. The order matters because the game calls through slot index.
 */
#ifndef VT3_WIN32_D3D9_H
#define VT3_WIN32_D3D9_H

#include "win32/types.h"

#define D3D_SDK_VERSION 32u

/* D3DFORMAT values the renderer supports (see vt3_renderer.h). */
#define D3DFMT_X8R8G8B8 22u
#define D3DFMT_A8R8G8B8 21u
#define D3DFMT_A8       28u
#define D3DFMT_R5G6B5   23u   /* TODO: renderer needs a conversion path */

/* D3DFVF bits used by the first draw path (XYZRHW | DIFFUSE | TEX1). */
#define D3DFVF_XYZRHW   0x004u
#define D3DFVF_DIFFUSE  0x040u
#define D3DFVF_TEX1     0x100u

/* IDirect3D9: 17 methods, in SDK order. */
typedef struct IDirect3D9Vtbl {
    GPTR QueryInterface, AddRef, Release;
    GPTR RegisterSoftwareDevice, GetAdapterCount, GetAdapterIdentifier;
    GPTR GetAdapterModeCount, EnumAdapterModes, GetAdapterDisplayMode;
    GPTR CheckDeviceType, CheckDeviceFormat, CheckDeviceMultiSampleType;
    GPTR CheckDepthStencilMatch, CheckDeviceFormatConversion, GetDeviceCaps;
    GPTR GetAdapterMonitor, CreateDevice;
} IDirect3D9Vtbl;

/* IDirect3DDevice9: 119 methods, in SDK order. */
typedef struct IDirect3DDevice9Vtbl {
    GPTR QueryInterface, AddRef, Release;                                       /* 1-3 */
    GPTR TestCooperativeLevel, GetAvailableTextureMem, EvictManagedResources;   /* 4-6 */
    GPTR GetDirect3D, GetDeviceCaps, GetDisplayMode, GetCreationParameters;     /* 7-10 */
    GPTR SetCursorProperties, SetCursorPosition, ShowCursor;                    /* 11-13 */
    GPTR CreateAdditionalSwapChain, GetSwapChain, GetNumberOfSwapChains;        /* 14-16 */
    GPTR Reset, Present, GetBackBuffer, GetRasterStatus;                        /* 17-20 */
    GPTR SetDialogBoxMode, SetGammaRamp, GetGammaRamp;                          /* 21-23 */
    GPTR CreateTexture, CreateVolumeTexture, CreateCubeTexture;                 /* 24-26 */
    GPTR CreateVertexBuffer, CreateIndexBuffer;                                 /* 27-28 */
    GPTR CreateRenderTarget, CreateDepthStencilSurface;                         /* 29-30 */
    GPTR UpdateSurface, UpdateTexture, GetRenderTargetData, GetFrontBufferData; /* 31-34 */
    GPTR StretchRect, ColorFill, CreateOffscreenPlainSurface;                   /* 35-37 */
    GPTR SetRenderTarget, GetRenderTarget, SetDepthStencilSurface;              /* 38-40 */
    GPTR GetDepthStencilSurface, BeginScene, EndScene, Clear;                   /* 41-44 */
    GPTR SetTransform, GetTransform, MultiplyTransform, SetViewport;            /* 45-48 */
    GPTR GetViewport, SetMaterial, GetMaterial, SetLight, GetLight;             /* 49-53 */
    GPTR LightEnable, GetLightEnable, SetClipPlane, GetClipPlane;               /* 54-57 */
    GPTR SetRenderState, GetRenderState, CreateStateBlock;                      /* 58-60 */
    GPTR BeginStateBlock, EndStateBlock, SetClipStatus, GetClipStatus;          /* 61-64 */
    GPTR GetTexture, SetTexture, GetTextureStageState, SetTextureStageState;    /* 65-68 */
    GPTR GetSamplerState, SetSamplerState, ValidateDevice;                      /* 69-71 */
    GPTR SetPaletteEntries, GetPaletteEntries;                                  /* 72-73 */
    GPTR SetCurrentTexturePalette, GetCurrentTexturePalette;                    /* 74-75 */
    GPTR SetScissorRect, GetScissorRect;                                        /* 76-77 */
    GPTR SetSoftwareVertexProcessing, GetSoftwareVertexProcessing;              /* 78-79 */
    GPTR SetNPatchMode, GetNPatchMode;                                          /* 80-81 */
    GPTR DrawPrimitive, DrawIndexedPrimitive;                                   /* 82-83 */
    GPTR DrawPrimitiveUP, DrawIndexedPrimitiveUP, ProcessVertices;              /* 84-86 */
    GPTR CreateVertexDeclaration, SetVertexDeclaration, GetVertexDeclaration;   /* 87-89 */
    GPTR SetFVF, GetFVF;                                                        /* 90-91 */
    GPTR CreateVertexShader, SetVertexShader, GetVertexShader;                  /* 92-94 */
    GPTR SetVertexShaderConstantF, GetVertexShaderConstantF;                    /* 95-96 */
    GPTR SetVertexShaderConstantI, GetVertexShaderConstantI;                    /* 97-98 */
    GPTR SetVertexShaderConstantB, GetVertexShaderConstantB;                    /* 99-100 */
    GPTR SetStreamSource, GetStreamSource, SetStreamSourceFreq;                 /* 101-103 */
    GPTR GetStreamSourceFreq, SetIndices, GetIndices;                           /* 104-106 */
    GPTR CreatePixelShader, SetPixelShader, GetPixelShader;                     /* 107-109 */
    GPTR SetPixelShaderConstantF, GetPixelShaderConstantF;                      /* 110-111 */
    GPTR SetPixelShaderConstantI, GetPixelShaderConstantI;                      /* 112-113 */
    GPTR SetPixelShaderConstantB, GetPixelShaderConstantB;                      /* 114-115 */
    GPTR DrawRectPatch, DrawTriPatch, DeletePatch, CreateQuery;                 /* 116-119 */
} IDirect3DDevice9Vtbl;

GPTR WINAPI Direct3DCreate9(UINT sdk_version);   /* returns IDirect3D9*, or 0 */

#endif /* VT3_WIN32_D3D9_H */
