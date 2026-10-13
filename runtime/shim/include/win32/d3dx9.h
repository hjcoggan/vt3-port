/*
 * D3DX 9 entry points the game imports (d3dx9_32.dll). Guest-width signatures: every
 * pointer is GPTR, and every FLOAT passed by value is DWORD (its bits), because the
 * adapter generator cannot pass floats. d3dx9_math.c and d3dx9_stub.c implement them.
 */
#ifndef VT3_WIN32_D3DX9_H
#define VT3_WIN32_D3DX9_H

#include "win32/types.h"

HRESULT WINAPI D3DXCompileShader(GPTR src_data, UINT data_len, GPTR defines, GPTR include, GPTR function_name,
                                 GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs, GPTR ppConstantTable);
HRESULT WINAPI D3DXCompileShaderFromFileA(GPTR filename, GPTR defines, GPTR include, GPTR function_name,
                                          GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs,
                                          GPTR ppConstantTable);
HRESULT WINAPI D3DXGetShaderConstantTable(GPTR byte_code, GPTR constant_table);
HRESULT WINAPI D3DXCreateTexture(GPTR device, UINT width, UINT height, UINT miplevels, DWORD usage, UINT format,
                                 UINT pool, GPTR texture);
HRESULT WINAPI D3DXCreateTextureFromFileInMemoryEx(GPTR device, GPTR srcdata, UINT srcdatasize, UINT width,
                                                   UINT height, UINT miplevels, DWORD usage, UINT format,
                                                   UINT pool, DWORD filter, DWORD mipfilter, DWORD colorkey,
                                                   GPTR srcinfo, GPTR palette, GPTR texture);
HRESULT WINAPI D3DXCreateCubeTextureFromFileInMemory(GPTR device, GPTR srcdata, UINT srcdatasize, GPTR texture);

GPTR WINAPI D3DXVec4TransformArray(GPTR out, UINT outstride, GPTR v, UINT vstride, GPTR m, UINT n);
GPTR WINAPI D3DXMatrixScaling(GPTR out, DWORD sx, DWORD sy, DWORD sz);
GPTR WINAPI D3DXMatrixRotationZ(GPTR out, DWORD angle);
GPTR WINAPI D3DXMatrixLookAtRH(GPTR out, GPTR eye, GPTR at, GPTR up);
GPTR WINAPI D3DXMatrixOrthoOffCenterRH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf);
GPTR WINAPI D3DXVec3Project(GPTR out, GPTR v, GPTR viewport, GPTR projection, GPTR view, GPTR world);
GPTR WINAPI D3DXMatrixPerspectiveFovRH(GPTR out, DWORD fovy, DWORD aspect, DWORD zn, DWORD zf);
GPTR WINAPI D3DXVec3Normalize(GPTR out, GPTR v);
GPTR WINAPI D3DXVec3Transform(GPTR out, GPTR v, GPTR m);
GPTR WINAPI D3DXMatrixTranslation(GPTR out, DWORD x, DWORD y, DWORD z);
GPTR WINAPI D3DXMatrixTranspose(GPTR out, GPTR m);
GPTR WINAPI D3DXMatrixOrthoOffCenterLH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf);
GPTR WINAPI D3DXMatrixMultiply(GPTR out, GPTR m1, GPTR m2);
GPTR WINAPI D3DXPlaneFromPoints(GPTR out, GPTR p1, GPTR p2, GPTR p3);
GPTR WINAPI D3DXMatrixPerspectiveOffCenterRH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf);

#endif /* VT3_WIN32_D3DX9_H */
