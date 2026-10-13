/*
 * Direct3D 9 interface layouts and constants for the shim.
 * Nothing is implemented yet: Direct3DCreate9 logs and returns NULL.
 *
 * Layouts checked against the mingw-w64 14.0.0 headers: tests/sdk_layout_check.c
 * asserts every slot offset and constant, and tests/layout_check.c checks these
 * declarations at host build time. Slot order is the SDK's.
 *
 * COM here: a guest object's first word is a pointer to its vtable (in guest
 * memory). Each vtable slot holds the guest address of a thunk, and the thunk
 * calls the matching host function. The shim fills those thunks in at init.
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

/* IDirect3D9: 17 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirect3D9Vtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR RegisterSoftwareDevice;      /* 3 */
    GPTR GetAdapterCount;             /* 4 */
    GPTR GetAdapterIdentifier;        /* 5 */
    GPTR GetAdapterModeCount;         /* 6 */
    GPTR EnumAdapterModes;            /* 7 */
    GPTR GetAdapterDisplayMode;       /* 8 */
    GPTR CheckDeviceType;             /* 9 */
    GPTR CheckDeviceFormat;           /* 10 */
    GPTR CheckDeviceMultiSampleType;  /* 11 */
    GPTR CheckDepthStencilMatch;      /* 12 */
    GPTR CheckDeviceFormatConversion; /* 13 */
    GPTR GetDeviceCaps;               /* 14 */
    GPTR GetAdapterMonitor;           /* 15 */
    GPTR CreateDevice;                /* 16 */
} IDirect3D9Vtbl;

/* IDirect3DDevice9: 119 slots, in the SDK's order. Slot N is at byte offset 4*N. */
typedef struct IDirect3DDevice9Vtbl {
    GPTR QueryInterface;              /* 0 */
    GPTR AddRef;                      /* 1 */
    GPTR Release;                     /* 2 */
    GPTR TestCooperativeLevel;        /* 3 */
    GPTR GetAvailableTextureMem;      /* 4 */
    GPTR EvictManagedResources;       /* 5 */
    GPTR GetDirect3D;                 /* 6 */
    GPTR GetDeviceCaps;               /* 7 */
    GPTR GetDisplayMode;              /* 8 */
    GPTR GetCreationParameters;       /* 9 */
    GPTR SetCursorProperties;         /* 10 */
    GPTR SetCursorPosition;           /* 11 */
    GPTR ShowCursor;                  /* 12 */
    GPTR CreateAdditionalSwapChain;   /* 13 */
    GPTR GetSwapChain;                /* 14 */
    GPTR GetNumberOfSwapChains;       /* 15 */
    GPTR Reset;                       /* 16 */
    GPTR Present;                     /* 17 */
    GPTR GetBackBuffer;               /* 18 */
    GPTR GetRasterStatus;             /* 19 */
    GPTR SetDialogBoxMode;            /* 20 */
    GPTR SetGammaRamp;                /* 21 */
    GPTR GetGammaRamp;                /* 22 */
    GPTR CreateTexture;               /* 23 */
    GPTR CreateVolumeTexture;         /* 24 */
    GPTR CreateCubeTexture;           /* 25 */
    GPTR CreateVertexBuffer;          /* 26 */
    GPTR CreateIndexBuffer;           /* 27 */
    GPTR CreateRenderTarget;          /* 28 */
    GPTR CreateDepthStencilSurface;   /* 29 */
    GPTR UpdateSurface;               /* 30 */
    GPTR UpdateTexture;               /* 31 */
    GPTR GetRenderTargetData;         /* 32 */
    GPTR GetFrontBufferData;          /* 33 */
    GPTR StretchRect;                 /* 34 */
    GPTR ColorFill;                   /* 35 */
    GPTR CreateOffscreenPlainSurface; /* 36 */
    GPTR SetRenderTarget;             /* 37 */
    GPTR GetRenderTarget;             /* 38 */
    GPTR SetDepthStencilSurface;      /* 39 */
    GPTR GetDepthStencilSurface;      /* 40 */
    GPTR BeginScene;                  /* 41 */
    GPTR EndScene;                    /* 42 */
    GPTR Clear;                       /* 43 */
    GPTR SetTransform;                /* 44 */
    GPTR GetTransform;                /* 45 */
    GPTR MultiplyTransform;           /* 46 */
    GPTR SetViewport;                 /* 47 */
    GPTR GetViewport;                 /* 48 */
    GPTR SetMaterial;                 /* 49 */
    GPTR GetMaterial;                 /* 50 */
    GPTR SetLight;                    /* 51 */
    GPTR GetLight;                    /* 52 */
    GPTR LightEnable;                 /* 53 */
    GPTR GetLightEnable;              /* 54 */
    GPTR SetClipPlane;                /* 55 */
    GPTR GetClipPlane;                /* 56 */
    GPTR SetRenderState;              /* 57 */
    GPTR GetRenderState;              /* 58 */
    GPTR CreateStateBlock;            /* 59 */
    GPTR BeginStateBlock;             /* 60 */
    GPTR EndStateBlock;               /* 61 */
    GPTR SetClipStatus;               /* 62 */
    GPTR GetClipStatus;               /* 63 */
    GPTR GetTexture;                  /* 64 */
    GPTR SetTexture;                  /* 65 */
    GPTR GetTextureStageState;        /* 66 */
    GPTR SetTextureStageState;        /* 67 */
    GPTR GetSamplerState;             /* 68 */
    GPTR SetSamplerState;             /* 69 */
    GPTR ValidateDevice;              /* 70 */
    GPTR SetPaletteEntries;           /* 71 */
    GPTR GetPaletteEntries;           /* 72 */
    GPTR SetCurrentTexturePalette;    /* 73 */
    GPTR GetCurrentTexturePalette;    /* 74 */
    GPTR SetScissorRect;              /* 75 */
    GPTR GetScissorRect;              /* 76 */
    GPTR SetSoftwareVertexProcessing; /* 77 */
    GPTR GetSoftwareVertexProcessing; /* 78 */
    GPTR SetNPatchMode;               /* 79 */
    GPTR GetNPatchMode;               /* 80 */
    GPTR DrawPrimitive;               /* 81 */
    GPTR DrawIndexedPrimitive;        /* 82 */
    GPTR DrawPrimitiveUP;             /* 83 */
    GPTR DrawIndexedPrimitiveUP;      /* 84 */
    GPTR ProcessVertices;             /* 85 */
    GPTR CreateVertexDeclaration;     /* 86 */
    GPTR SetVertexDeclaration;        /* 87 */
    GPTR GetVertexDeclaration;        /* 88 */
    GPTR SetFVF;                      /* 89 */
    GPTR GetFVF;                      /* 90 */
    GPTR CreateVertexShader;          /* 91 */
    GPTR SetVertexShader;             /* 92 */
    GPTR GetVertexShader;             /* 93 */
    GPTR SetVertexShaderConstantF;    /* 94 */
    GPTR GetVertexShaderConstantF;    /* 95 */
    GPTR SetVertexShaderConstantI;    /* 96 */
    GPTR GetVertexShaderConstantI;    /* 97 */
    GPTR SetVertexShaderConstantB;    /* 98 */
    GPTR GetVertexShaderConstantB;    /* 99 */
    GPTR SetStreamSource;             /* 100 */
    GPTR GetStreamSource;             /* 101 */
    GPTR SetStreamSourceFreq;         /* 102 */
    GPTR GetStreamSourceFreq;         /* 103 */
    GPTR SetIndices;                  /* 104 */
    GPTR GetIndices;                  /* 105 */
    GPTR CreatePixelShader;           /* 106 */
    GPTR SetPixelShader;              /* 107 */
    GPTR GetPixelShader;              /* 108 */
    GPTR SetPixelShaderConstantF;     /* 109 */
    GPTR GetPixelShaderConstantF;     /* 110 */
    GPTR SetPixelShaderConstantI;     /* 111 */
    GPTR GetPixelShaderConstantI;     /* 112 */
    GPTR SetPixelShaderConstantB;     /* 113 */
    GPTR GetPixelShaderConstantB;     /* 114 */
    GPTR DrawRectPatch;               /* 115 */
    GPTR DrawTriPatch;                /* 116 */
    GPTR DeletePatch;                 /* 117 */
    GPTR CreateQuery;                 /* 118 */
} IDirect3DDevice9Vtbl;

GPTR WINAPI Direct3DCreate9(UINT sdk_version);   /* returns IDirect3D9*, or 0 */

#endif /* VT3_WIN32_D3D9_H */
