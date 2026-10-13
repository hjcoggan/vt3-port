/*
 * Build-time layout check for the shim's COM vtables (host compile, no SDK needed).
 * The expected slot order comes from the mingw-w64 14.0.0 SDK headers. The same
 * order is checked against the real SDK in tests/sdk_layout_check.c.
 */
#include "win32/d3d9.h"
#include "win32/dsound.h"
#include "win32/dinput.h"
#include "vt3/vt3_renderer.h"
#include <stddef.h>

/* IDirect3D9Vtbl: 17 slots */
_Static_assert(offsetof(IDirect3D9Vtbl, QueryInterface) == 0, "IDirect3D9Vtbl slot 0");
_Static_assert(offsetof(IDirect3D9Vtbl, AddRef) == 4, "IDirect3D9Vtbl slot 1");
_Static_assert(offsetof(IDirect3D9Vtbl, Release) == 8, "IDirect3D9Vtbl slot 2");
_Static_assert(offsetof(IDirect3D9Vtbl, RegisterSoftwareDevice) == 12, "IDirect3D9Vtbl slot 3");
_Static_assert(offsetof(IDirect3D9Vtbl, GetAdapterCount) == 16, "IDirect3D9Vtbl slot 4");
_Static_assert(offsetof(IDirect3D9Vtbl, GetAdapterIdentifier) == 20, "IDirect3D9Vtbl slot 5");
_Static_assert(offsetof(IDirect3D9Vtbl, GetAdapterModeCount) == 24, "IDirect3D9Vtbl slot 6");
_Static_assert(offsetof(IDirect3D9Vtbl, EnumAdapterModes) == 28, "IDirect3D9Vtbl slot 7");
_Static_assert(offsetof(IDirect3D9Vtbl, GetAdapterDisplayMode) == 32, "IDirect3D9Vtbl slot 8");
_Static_assert(offsetof(IDirect3D9Vtbl, CheckDeviceType) == 36, "IDirect3D9Vtbl slot 9");
_Static_assert(offsetof(IDirect3D9Vtbl, CheckDeviceFormat) == 40, "IDirect3D9Vtbl slot 10");
_Static_assert(offsetof(IDirect3D9Vtbl, CheckDeviceMultiSampleType) == 44, "IDirect3D9Vtbl slot 11");
_Static_assert(offsetof(IDirect3D9Vtbl, CheckDepthStencilMatch) == 48, "IDirect3D9Vtbl slot 12");
_Static_assert(offsetof(IDirect3D9Vtbl, CheckDeviceFormatConversion) == 52, "IDirect3D9Vtbl slot 13");
_Static_assert(offsetof(IDirect3D9Vtbl, GetDeviceCaps) == 56, "IDirect3D9Vtbl slot 14");
_Static_assert(offsetof(IDirect3D9Vtbl, GetAdapterMonitor) == 60, "IDirect3D9Vtbl slot 15");
_Static_assert(offsetof(IDirect3D9Vtbl, CreateDevice) == 64, "IDirect3D9Vtbl slot 16");

/* IDirect3DDevice9Vtbl: 119 slots */
_Static_assert(offsetof(IDirect3DDevice9Vtbl, QueryInterface) == 0, "IDirect3DDevice9Vtbl slot 0");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, AddRef) == 4, "IDirect3DDevice9Vtbl slot 1");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Release) == 8, "IDirect3DDevice9Vtbl slot 2");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, TestCooperativeLevel) == 12, "IDirect3DDevice9Vtbl slot 3");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetAvailableTextureMem) == 16, "IDirect3DDevice9Vtbl slot 4");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, EvictManagedResources) == 20, "IDirect3DDevice9Vtbl slot 5");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetDirect3D) == 24, "IDirect3DDevice9Vtbl slot 6");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetDeviceCaps) == 28, "IDirect3DDevice9Vtbl slot 7");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetDisplayMode) == 32, "IDirect3DDevice9Vtbl slot 8");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetCreationParameters) == 36, "IDirect3DDevice9Vtbl slot 9");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetCursorProperties) == 40, "IDirect3DDevice9Vtbl slot 10");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetCursorPosition) == 44, "IDirect3DDevice9Vtbl slot 11");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, ShowCursor) == 48, "IDirect3DDevice9Vtbl slot 12");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateAdditionalSwapChain) == 52, "IDirect3DDevice9Vtbl slot 13");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetSwapChain) == 56, "IDirect3DDevice9Vtbl slot 14");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetNumberOfSwapChains) == 60, "IDirect3DDevice9Vtbl slot 15");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Reset) == 64, "IDirect3DDevice9Vtbl slot 16");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Present) == 68, "IDirect3DDevice9Vtbl slot 17");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetBackBuffer) == 72, "IDirect3DDevice9Vtbl slot 18");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetRasterStatus) == 76, "IDirect3DDevice9Vtbl slot 19");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetDialogBoxMode) == 80, "IDirect3DDevice9Vtbl slot 20");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetGammaRamp) == 84, "IDirect3DDevice9Vtbl slot 21");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetGammaRamp) == 88, "IDirect3DDevice9Vtbl slot 22");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateTexture) == 92, "IDirect3DDevice9Vtbl slot 23");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateVolumeTexture) == 96, "IDirect3DDevice9Vtbl slot 24");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateCubeTexture) == 100, "IDirect3DDevice9Vtbl slot 25");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateVertexBuffer) == 104, "IDirect3DDevice9Vtbl slot 26");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateIndexBuffer) == 108, "IDirect3DDevice9Vtbl slot 27");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateRenderTarget) == 112, "IDirect3DDevice9Vtbl slot 28");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateDepthStencilSurface) == 116, "IDirect3DDevice9Vtbl slot 29");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, UpdateSurface) == 120, "IDirect3DDevice9Vtbl slot 30");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, UpdateTexture) == 124, "IDirect3DDevice9Vtbl slot 31");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetRenderTargetData) == 128, "IDirect3DDevice9Vtbl slot 32");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetFrontBufferData) == 132, "IDirect3DDevice9Vtbl slot 33");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, StretchRect) == 136, "IDirect3DDevice9Vtbl slot 34");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, ColorFill) == 140, "IDirect3DDevice9Vtbl slot 35");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateOffscreenPlainSurface) == 144, "IDirect3DDevice9Vtbl slot 36");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetRenderTarget) == 148, "IDirect3DDevice9Vtbl slot 37");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetRenderTarget) == 152, "IDirect3DDevice9Vtbl slot 38");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetDepthStencilSurface) == 156, "IDirect3DDevice9Vtbl slot 39");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetDepthStencilSurface) == 160, "IDirect3DDevice9Vtbl slot 40");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, BeginScene) == 164, "IDirect3DDevice9Vtbl slot 41");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, EndScene) == 168, "IDirect3DDevice9Vtbl slot 42");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Clear) == 172, "IDirect3DDevice9Vtbl slot 43");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetTransform) == 176, "IDirect3DDevice9Vtbl slot 44");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetTransform) == 180, "IDirect3DDevice9Vtbl slot 45");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, MultiplyTransform) == 184, "IDirect3DDevice9Vtbl slot 46");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetViewport) == 188, "IDirect3DDevice9Vtbl slot 47");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetViewport) == 192, "IDirect3DDevice9Vtbl slot 48");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetMaterial) == 196, "IDirect3DDevice9Vtbl slot 49");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetMaterial) == 200, "IDirect3DDevice9Vtbl slot 50");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetLight) == 204, "IDirect3DDevice9Vtbl slot 51");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetLight) == 208, "IDirect3DDevice9Vtbl slot 52");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, LightEnable) == 212, "IDirect3DDevice9Vtbl slot 53");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetLightEnable) == 216, "IDirect3DDevice9Vtbl slot 54");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetClipPlane) == 220, "IDirect3DDevice9Vtbl slot 55");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetClipPlane) == 224, "IDirect3DDevice9Vtbl slot 56");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetRenderState) == 228, "IDirect3DDevice9Vtbl slot 57");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetRenderState) == 232, "IDirect3DDevice9Vtbl slot 58");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateStateBlock) == 236, "IDirect3DDevice9Vtbl slot 59");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, BeginStateBlock) == 240, "IDirect3DDevice9Vtbl slot 60");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, EndStateBlock) == 244, "IDirect3DDevice9Vtbl slot 61");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetClipStatus) == 248, "IDirect3DDevice9Vtbl slot 62");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetClipStatus) == 252, "IDirect3DDevice9Vtbl slot 63");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetTexture) == 256, "IDirect3DDevice9Vtbl slot 64");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetTexture) == 260, "IDirect3DDevice9Vtbl slot 65");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetTextureStageState) == 264, "IDirect3DDevice9Vtbl slot 66");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetTextureStageState) == 268, "IDirect3DDevice9Vtbl slot 67");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetSamplerState) == 272, "IDirect3DDevice9Vtbl slot 68");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetSamplerState) == 276, "IDirect3DDevice9Vtbl slot 69");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, ValidateDevice) == 280, "IDirect3DDevice9Vtbl slot 70");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPaletteEntries) == 284, "IDirect3DDevice9Vtbl slot 71");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetPaletteEntries) == 288, "IDirect3DDevice9Vtbl slot 72");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetCurrentTexturePalette) == 292, "IDirect3DDevice9Vtbl slot 73");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetCurrentTexturePalette) == 296, "IDirect3DDevice9Vtbl slot 74");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetScissorRect) == 300, "IDirect3DDevice9Vtbl slot 75");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetScissorRect) == 304, "IDirect3DDevice9Vtbl slot 76");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetSoftwareVertexProcessing) == 308, "IDirect3DDevice9Vtbl slot 77");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetSoftwareVertexProcessing) == 312, "IDirect3DDevice9Vtbl slot 78");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetNPatchMode) == 316, "IDirect3DDevice9Vtbl slot 79");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetNPatchMode) == 320, "IDirect3DDevice9Vtbl slot 80");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawPrimitive) == 324, "IDirect3DDevice9Vtbl slot 81");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawIndexedPrimitive) == 328, "IDirect3DDevice9Vtbl slot 82");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawPrimitiveUP) == 332, "IDirect3DDevice9Vtbl slot 83");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawIndexedPrimitiveUP) == 336, "IDirect3DDevice9Vtbl slot 84");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, ProcessVertices) == 340, "IDirect3DDevice9Vtbl slot 85");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateVertexDeclaration) == 344, "IDirect3DDevice9Vtbl slot 86");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetVertexDeclaration) == 348, "IDirect3DDevice9Vtbl slot 87");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetVertexDeclaration) == 352, "IDirect3DDevice9Vtbl slot 88");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetFVF) == 356, "IDirect3DDevice9Vtbl slot 89");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetFVF) == 360, "IDirect3DDevice9Vtbl slot 90");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateVertexShader) == 364, "IDirect3DDevice9Vtbl slot 91");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetVertexShader) == 368, "IDirect3DDevice9Vtbl slot 92");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetVertexShader) == 372, "IDirect3DDevice9Vtbl slot 93");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetVertexShaderConstantF) == 376, "IDirect3DDevice9Vtbl slot 94");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetVertexShaderConstantF) == 380, "IDirect3DDevice9Vtbl slot 95");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetVertexShaderConstantI) == 384, "IDirect3DDevice9Vtbl slot 96");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetVertexShaderConstantI) == 388, "IDirect3DDevice9Vtbl slot 97");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetVertexShaderConstantB) == 392, "IDirect3DDevice9Vtbl slot 98");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetVertexShaderConstantB) == 396, "IDirect3DDevice9Vtbl slot 99");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetStreamSource) == 400, "IDirect3DDevice9Vtbl slot 100");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetStreamSource) == 404, "IDirect3DDevice9Vtbl slot 101");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetStreamSourceFreq) == 408, "IDirect3DDevice9Vtbl slot 102");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetStreamSourceFreq) == 412, "IDirect3DDevice9Vtbl slot 103");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetIndices) == 416, "IDirect3DDevice9Vtbl slot 104");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetIndices) == 420, "IDirect3DDevice9Vtbl slot 105");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreatePixelShader) == 424, "IDirect3DDevice9Vtbl slot 106");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPixelShader) == 428, "IDirect3DDevice9Vtbl slot 107");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetPixelShader) == 432, "IDirect3DDevice9Vtbl slot 108");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantF) == 436, "IDirect3DDevice9Vtbl slot 109");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetPixelShaderConstantF) == 440, "IDirect3DDevice9Vtbl slot 110");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantI) == 444, "IDirect3DDevice9Vtbl slot 111");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetPixelShaderConstantI) == 448, "IDirect3DDevice9Vtbl slot 112");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantB) == 452, "IDirect3DDevice9Vtbl slot 113");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, GetPixelShaderConstantB) == 456, "IDirect3DDevice9Vtbl slot 114");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawRectPatch) == 460, "IDirect3DDevice9Vtbl slot 115");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DrawTriPatch) == 464, "IDirect3DDevice9Vtbl slot 116");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, DeletePatch) == 468, "IDirect3DDevice9Vtbl slot 117");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, CreateQuery) == 472, "IDirect3DDevice9Vtbl slot 118");

/* IDirectSoundVtbl: 11 slots */
_Static_assert(offsetof(IDirectSoundVtbl, QueryInterface) == 0, "IDirectSoundVtbl slot 0");
_Static_assert(offsetof(IDirectSoundVtbl, AddRef) == 4, "IDirectSoundVtbl slot 1");
_Static_assert(offsetof(IDirectSoundVtbl, Release) == 8, "IDirectSoundVtbl slot 2");
_Static_assert(offsetof(IDirectSoundVtbl, CreateSoundBuffer) == 12, "IDirectSoundVtbl slot 3");
_Static_assert(offsetof(IDirectSoundVtbl, GetCaps) == 16, "IDirectSoundVtbl slot 4");
_Static_assert(offsetof(IDirectSoundVtbl, DuplicateSoundBuffer) == 20, "IDirectSoundVtbl slot 5");
_Static_assert(offsetof(IDirectSoundVtbl, SetCooperativeLevel) == 24, "IDirectSoundVtbl slot 6");
_Static_assert(offsetof(IDirectSoundVtbl, Compact) == 28, "IDirectSoundVtbl slot 7");
_Static_assert(offsetof(IDirectSoundVtbl, GetSpeakerConfig) == 32, "IDirectSoundVtbl slot 8");
_Static_assert(offsetof(IDirectSoundVtbl, SetSpeakerConfig) == 36, "IDirectSoundVtbl slot 9");
_Static_assert(offsetof(IDirectSoundVtbl, Initialize) == 40, "IDirectSoundVtbl slot 10");

/* IDirectSoundBufferVtbl: 21 slots */
_Static_assert(offsetof(IDirectSoundBufferVtbl, QueryInterface) == 0, "IDirectSoundBufferVtbl slot 0");
_Static_assert(offsetof(IDirectSoundBufferVtbl, AddRef) == 4, "IDirectSoundBufferVtbl slot 1");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Release) == 8, "IDirectSoundBufferVtbl slot 2");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetCaps) == 12, "IDirectSoundBufferVtbl slot 3");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetCurrentPosition) == 16, "IDirectSoundBufferVtbl slot 4");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetFormat) == 20, "IDirectSoundBufferVtbl slot 5");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetVolume) == 24, "IDirectSoundBufferVtbl slot 6");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetPan) == 28, "IDirectSoundBufferVtbl slot 7");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetFrequency) == 32, "IDirectSoundBufferVtbl slot 8");
_Static_assert(offsetof(IDirectSoundBufferVtbl, GetStatus) == 36, "IDirectSoundBufferVtbl slot 9");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Initialize) == 40, "IDirectSoundBufferVtbl slot 10");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Lock) == 44, "IDirectSoundBufferVtbl slot 11");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Play) == 48, "IDirectSoundBufferVtbl slot 12");
_Static_assert(offsetof(IDirectSoundBufferVtbl, SetCurrentPosition) == 52, "IDirectSoundBufferVtbl slot 13");
_Static_assert(offsetof(IDirectSoundBufferVtbl, SetFormat) == 56, "IDirectSoundBufferVtbl slot 14");
_Static_assert(offsetof(IDirectSoundBufferVtbl, SetVolume) == 60, "IDirectSoundBufferVtbl slot 15");
_Static_assert(offsetof(IDirectSoundBufferVtbl, SetPan) == 64, "IDirectSoundBufferVtbl slot 16");
_Static_assert(offsetof(IDirectSoundBufferVtbl, SetFrequency) == 68, "IDirectSoundBufferVtbl slot 17");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Stop) == 72, "IDirectSoundBufferVtbl slot 18");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Unlock) == 76, "IDirectSoundBufferVtbl slot 19");
_Static_assert(offsetof(IDirectSoundBufferVtbl, Restore) == 80, "IDirectSoundBufferVtbl slot 20");

/* IDirectInput8AVtbl: 11 slots */
_Static_assert(offsetof(IDirectInput8AVtbl, QueryInterface) == 0, "IDirectInput8AVtbl slot 0");
_Static_assert(offsetof(IDirectInput8AVtbl, AddRef) == 4, "IDirectInput8AVtbl slot 1");
_Static_assert(offsetof(IDirectInput8AVtbl, Release) == 8, "IDirectInput8AVtbl slot 2");
_Static_assert(offsetof(IDirectInput8AVtbl, CreateDevice) == 12, "IDirectInput8AVtbl slot 3");
_Static_assert(offsetof(IDirectInput8AVtbl, EnumDevices) == 16, "IDirectInput8AVtbl slot 4");
_Static_assert(offsetof(IDirectInput8AVtbl, GetDeviceStatus) == 20, "IDirectInput8AVtbl slot 5");
_Static_assert(offsetof(IDirectInput8AVtbl, RunControlPanel) == 24, "IDirectInput8AVtbl slot 6");
_Static_assert(offsetof(IDirectInput8AVtbl, Initialize) == 28, "IDirectInput8AVtbl slot 7");
_Static_assert(offsetof(IDirectInput8AVtbl, FindDevice) == 32, "IDirectInput8AVtbl slot 8");
_Static_assert(offsetof(IDirectInput8AVtbl, EnumDevicesBySemantics) == 36, "IDirectInput8AVtbl slot 9");
_Static_assert(offsetof(IDirectInput8AVtbl, ConfigureDevices) == 40, "IDirectInput8AVtbl slot 10");

/* IDirectInputDevice8AVtbl: 32 slots */
_Static_assert(offsetof(IDirectInputDevice8AVtbl, QueryInterface) == 0, "IDirectInputDevice8AVtbl slot 0");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, AddRef) == 4, "IDirectInputDevice8AVtbl slot 1");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Release) == 8, "IDirectInputDevice8AVtbl slot 2");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetCapabilities) == 12, "IDirectInputDevice8AVtbl slot 3");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, EnumObjects) == 16, "IDirectInputDevice8AVtbl slot 4");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetProperty) == 20, "IDirectInputDevice8AVtbl slot 5");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SetProperty) == 24, "IDirectInputDevice8AVtbl slot 6");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Acquire) == 28, "IDirectInputDevice8AVtbl slot 7");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Unacquire) == 32, "IDirectInputDevice8AVtbl slot 8");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetDeviceState) == 36, "IDirectInputDevice8AVtbl slot 9");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetDeviceData) == 40, "IDirectInputDevice8AVtbl slot 10");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SetDataFormat) == 44, "IDirectInputDevice8AVtbl slot 11");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SetEventNotification) == 48, "IDirectInputDevice8AVtbl slot 12");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SetCooperativeLevel) == 52, "IDirectInputDevice8AVtbl slot 13");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetObjectInfo) == 56, "IDirectInputDevice8AVtbl slot 14");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetDeviceInfo) == 60, "IDirectInputDevice8AVtbl slot 15");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, RunControlPanel) == 64, "IDirectInputDevice8AVtbl slot 16");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Initialize) == 68, "IDirectInputDevice8AVtbl slot 17");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, CreateEffect) == 72, "IDirectInputDevice8AVtbl slot 18");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, EnumEffects) == 76, "IDirectInputDevice8AVtbl slot 19");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetEffectInfo) == 80, "IDirectInputDevice8AVtbl slot 20");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetForceFeedbackState) == 84, "IDirectInputDevice8AVtbl slot 21");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SendForceFeedbackCommand) == 88, "IDirectInputDevice8AVtbl slot 22");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, EnumCreatedEffectObjects) == 92, "IDirectInputDevice8AVtbl slot 23");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Escape) == 96, "IDirectInputDevice8AVtbl slot 24");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, Poll) == 100, "IDirectInputDevice8AVtbl slot 25");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SendDeviceData) == 104, "IDirectInputDevice8AVtbl slot 26");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, EnumEffectsInFile) == 108, "IDirectInputDevice8AVtbl slot 27");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, WriteEffectToFile) == 112, "IDirectInputDevice8AVtbl slot 28");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, BuildActionMap) == 116, "IDirectInputDevice8AVtbl slot 29");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, SetActionMap) == 120, "IDirectInputDevice8AVtbl slot 30");
_Static_assert(offsetof(IDirectInputDevice8AVtbl, GetImageInfo) == 124, "IDirectInputDevice8AVtbl slot 31");

_Static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX size");
_Static_assert(sizeof(GPTR) == 4, "guest pointers are 32-bit");

/* D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1: 16 + 4 + 8 bytes, no padding. */
_Static_assert(sizeof(vt3_tlvertex) == 28, "vt3_tlvertex is the 28-byte XYZRHW|DIFFUSE|TEX1 vertex");
_Static_assert(offsetof(vt3_tlvertex, color) == 16, "DIFFUSE follows XYZRHW");
_Static_assert(offsetof(vt3_tlvertex, u) == 20, "TEX1 follows DIFFUSE");
