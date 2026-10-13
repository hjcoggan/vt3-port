# D3D9 scope: what VT3 actually calls

Measured on 2026-10-14 by running the NoCD PC build under Wine. This is analysis, not shipped code. Companion to [runtime-plan.md](runtime-plan.md). Logs live under `dump/runtime/logs/` and are not committed.

## Method

- Binary: `dump/patches/Virtua-Tennis-3_NoCD_Win_EN_for-version-101/VT3.exe`, SHA-256 `4bfcecc4abf16c505b18a5da958dc489ff4f906b221fef78aeb839f65918d69e` (the NoCD patch copy). The protected original was not run.
- Prefix: `dump/runtime/wineprefix`, cloned from the earlier NoCD prefix. Wine Devel 11.18 from `tools/wine`, under Rosetta. D3D9 runs through wined3d on OpenGL (macOS GL 4.1, Metal).
- Run A (main): `WINEDEBUG=+d3d,+d3d9,+d3dx9`, about 5 minutes. 19,240 frames presented, about 2.6 million D3D9 calls. Log 1.66 GB.
- Run B (shaders): `WINEDEBUG=+d3d_shader,+d3d9`, 90 seconds. All 242 shader creations and their listings.
- Run C (D3DX): `WINEDEBUG=+d3dx9_24,+d3dx9_32,+d3dx9_36`, 75 seconds. No D3DX lines.

Caveats:

- `+d3dx9` matched nothing. Wine logs D3DX under its own channel names. The only D3DX-related trace is 9 `D3DCompile` calls on the shader compile path. D3DX use is therefore taken from the import table.
- The title screen is not confirmed. The window could not be inspected, because screen-recording permission is still pending on this Mac. The run reached a steady render loop, which fits an attract or title loop. That is not verified.
- Wine's trace shows what the d3d9 layer receives, not the game's control flow. Counts are per run.
- Some trace lines are interleaved across threads. Only 70 of the 140 `CreateTexture` trace lines carry arguments, so format breakdowns use those. Totals come from call names.

## Imports

From the NoCD PE import table (`pefile`):

| DLL | Imports |
|---|---|
| d3d9.dll | `Direct3DCreate9`, `D3DPERF_SetOptions` |
| d3dx9_32.dll (21) | `D3DXVec4TransformArray`, `D3DXMatrixScaling`, `D3DXMatrixRotationZ`, `D3DXMatrixLookAtRH`, `D3DXMatrixOrthoOffCenterRH`, `D3DXVec3Project`, `D3DXMatrixPerspectiveFovRH`, `D3DXVec3Normalize`, `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXVec3Transform`, `D3DXMatrixTranslation`, `D3DXMatrixTranspose`, `D3DXMatrixOrthoOffCenterLH`, `D3DXMatrixMultiply`, `D3DXPlaneFromPoints`, `D3DXCreateTexture`, `D3DXCreateTextureFromFileInMemoryEx`, `D3DXMatrixPerspectiveOffCenterRH`, `D3DXGetShaderConstantTable`, `D3DXCreateCubeTextureFromFileInMemory` |
| DSOUND.dll | ordinal 11 (`DirectSoundCreate`) |
| DINPUT8.dll | `DirectInput8Create` |
| WINMM.dll | `timeBeginPeriod`, `timeEndPeriod`, `timeSetEvent`, `timeKillEvent` |

The D3DX functions are imported but not traced, so their call counts are unknown. The shader compile path ran 9 times in both run A and run C (`D3DCompile2VKD3D`). That fits `D3DXCompileShaderFromFileA`, so shaders are compiled at load time from source, not shipped only as bytecode.

## Interface methods (run A)

Counts are calls in run A. Wine-internal entries are helper names from Wine's own tracing, not game calls.

**IDirect3D9** (138 calls, 9 methods)

| calls | method |
|---|---|
| 33 | d3d9_Release |
| 33 | d3d9_AddRef |
| 32 | d3d9_QueryInterface |
| 32 | d3d9_CheckDeviceFormat |
| 2 | d3d9_GetDeviceCaps |
| 2 | d3d9_GetAdapterIdentifier |
| 2 | d3d9_CreateDevice |
| 1 | d3d9_GetAdapterDisplayMode |
| 1 | d3d9_GetAdapterCount |

**IDirect3DDevice9** (2349189 calls, 40 methods)

| calls | method |
|---|---|
| 988988 | SetSamplerState |
| 746325 | SetRenderState |
| 57721 | SetViewport |
| 53281 | SetTexture |
| 47614 | DrawIndexedPrimitive |
| 45552 | SetTextureStageState |
| 44707 | SetPixelShader |
| 38482 | TestCooperativeLevel |
| 38482 | Clear |
| 38480 | SetPixelShaderConstantF |
| 32710 | SetVertexShader |
| 27016 | SetVertexShaderConstantF |
| 27016 | SetVertexDeclaration |
| 27016 | SetStreamSource |
| 27016 | SetIndices |
| 19241 | BeginScene |
| 19240 | SetRenderTarget |
| 19240 | SetDepthStencilSurface |
| 19240 | Present |
| 19240 | EndScene |
| 5694 | SetFVF |
| 5694 | DrawPrimitiveUP |
| 391 | StretchRect |
| 221 | AddRef |
| 140 | CreateTexture |
| 122 | CreatePixelShader |
| 120 | CreateVertexShader |
| 56 | GetDeviceCaps |
| 32 | GetDisplayMode |
| 32 | GetDirect3D |
| 32 | GetCreationParameters |
| 14 | CreateVertexBuffer |
| 11 | CreateVertexDeclaration |
| 8 | CreateCubeTexture |
| 6 | Release |
| 4 | CreateIndexBuffer |
| 2 | CreateDepthStencilSurface |
| 1 | QueryInterface |
| 1 | GetRenderTarget |
| 1 | GetDepthStencilSurface |

**IDirect3DSurface9** (156750 calls, 5 methods)

| calls | method |
|---|---|
| 77194 | AddRef |
| 77176 | Release |
| 1082 | UnlockRect |
| 1082 | LockRect |
| 216 | GetDesc |

**IDirect3DTexture9** (2394 calls, 6 methods)

| calls | method |
|---|---|
| 842 | 2d_UnlockRect |
| 842 | 2d_LockRect |
| 469 | 2d_QueryInterface |
| 176 | 2d_GetLevelDesc |
| 36 | 2d_GetLevelCount |
| 29 | 2d_GetSurfaceLevel |

**IDirect3DCubeTexture9** (510 calls, 6 methods)

| calls | method |
|---|---|
| 168 | cube_QueryInterface |
| 120 | cube_GetCubeMapSurface |
| 96 | cube_UnlockRect |
| 96 | cube_LockRect |
| 20 | cube_GetLevelDesc |
| 10 | cube_GetLevelCount |

**IDirect3DVertexBuffer9** (54035 calls, 2 methods)

| calls | method |
|---|---|
| 27018 | Lock |
| 27017 | Unlock |

**IDirect3DIndexBuffer9** (54032 calls, 2 methods)

| calls | method |
|---|---|
| 27016 | Unlock |
| 27016 | Lock |

**IDirect3DVertexDeclaration9** (1 calls, 1 methods)

| calls | method |
|---|---|
| 1 | declaration_Release |

**IDirect3DSwapChain9** (3 calls, 2 methods)

| calls | method |
|---|---|
| 2 | QueryInterface |
| 1 | Release |

**(Wine-internal helper)** (2172 calls, 8 methods)

| calls | method |
|---|---|
| 750 | d3d9_texture_incref |
| 749 | d3d9_texture_decref |
| 642 | d3d9_surface_create |
| 12 | d3d9_vertex_declaration_create |
| 12 | d3d9_texture_2d_GetType |
| 4 | d3d9_texture_cube_GetType |
| 2 | d3d9_device_create_surface |
| 1 | d3d9_swapchain_create |

## Render states (`SetRenderState`, run A)

Names and codes come from the mingw-w64 14.0.0 `d3d9types.h`. Values are the ones seen.

| calls | D3DRS state | code | values seen (count) |
|---|---|---|---|
| 111677 | D3DRS_ZENABLE | 0x7 | 0 (65420), 1 (46257) |
| 101646 | D3DRS_SCISSORTESTENABLE | 0xae | 0 (101646) |
| 92436 | D3DRS_CULLMODE | 0x16 | 2 = CW (32710), 1 = NONE (32710), 3 = CCW (27016) |
| 78967 | D3DRS_ALPHABLENDENABLE | 0x1b | 1 (78967) |
| 59726 | D3DRS_ZWRITEENABLE | 0xe | 0 (32710), 1 (27016) |
| 58242 | D3DRS_SRCBLEND | 0x13 | 5 = SRCALPHA (58242) |
| 58242 | D3DRS_DESTBLEND | 0x14 | 6 = INVSRCALPHA (31226), 2 = ONE (27016) |
| 51951 | D3DRS_ALPHATESTENABLE | 0xf | 0 (32710), 1 (19241) |
| 27407 | D3DRS_COLORWRITEENABLE | 0xa8 | 7 (27407) |
| 25532 | D3DRS_BLENDOP | 0xab | 1 = ADD (25532) |
| 19241 | D3DRS_ALPHAFUNC | 0x19 | 7 = GREATEREQUAL (19241) |
| 19241 | D3DRS_ALPHAREF | 0x18 | 0 (19241) |
| 19241 | D3DRS_ZFUNC | 0x17 | 4 = LESSEQUAL (19241) |
| 11388 | D3DRS_STENCILENABLE | 0x34 | 0 (11388) |
| 5694 | D3DRS_AMBIENTMATERIALSOURCE | 0x93 | 1 (5694) |
| 5694 | D3DRS_DIFFUSEMATERIALSOURCE | 0x91 | 1 (5694) |

Sixteen states in total. Lighting is never enabled, because `D3DRS_LIGHTING` is never set. Fog, stencil operations, and point sprites are not used.

## Texture stages and samplers (run A)

| calls | Stage | State | Values (count) |
|---|---|---|---|
| 5694 | 0 | D3DTSS_COLOROP (0x1) | 4 = MODULATE (5694), 1 = DISABLE (5694, stage 1) |
| 5694 | 0 | D3DTSS_COLORARG1 (0x2) | 0 = DIFFUSE (5694) |
| 5694 | 0 | D3DTSS_COLORARG2 (0x3) | 2 = TEXTURE (5694) |
| 5694 | 0 | D3DTSS_ALPHAOP (0x4) | 4 = MODULATE (5694), 1 = DISABLE (5694, stage 1) |
| 5694 | 0 | D3DTSS_ALPHAARG1 (0x5) | 0 = DIFFUSE (5694) |
| 5694 | 0 | D3DTSS_ALPHAARG2 (0x6) | 2 = TEXTURE (5694) |

Stage 0 is MODULATE(diffuse, texture) on colour and alpha. Stage 1 is disabled.

Samplers (stages 0 to 7 all use the same settings, so counts are per stage):

| D3DSAMP state | Values (count) |
|---|---|
| D3DSAMP_ADDRESSU (0x1) | 1 = WRAP (153928 across stages), 3 = CLAMP (27016) |
| D3DSAMP_ADDRESSV (0x2) | 1 = WRAP (153928), 3 = CLAMP (27016) |
| D3DSAMP_MAGFILTER (0x5) | 2 = LINEAR (159622) |
| D3DSAMP_MINFILTER (0x6) | 3 = ANISOTROPIC (153928), 2 = LINEAR (5694) |
| D3DSAMP_MIPFILTER (0x7) | 2 = LINEAR (153928) |
| D3DSAMP_MAXANISOTROPY (0xa) | 4 (153928) |

Sampler calls total 988,988 (about 51 per frame, across all stages).

## Vertex formats, primitives, textures, buffers

| calls | Item | Details |
|---|---|---|
| 5694 | SetFVF 0x144 | XYZRHW \| DIFFUSE \| TEX1. This is the only FVF used. |
| 47614 | DrawIndexedPrimitive | D3DPT_TRIANGLELIST (4), vertex declarations and stream sources |
| 5694 | DrawPrimitiveUP | D3DPT_TRIANGLESTRIP (5), XYZRHW (UI quads), stride 28 |
| 11 | CreateVertexDeclaration | 12 `vertex_declaration_create` internal helpers, 1 release |
| 27016 | SetVertexDeclaration / SetStreamSource / SetIndices | one set per indexed draw |
| 6 | CreateVertexBuffer | usage 0x208 (write-only, dynamic), pool DEFAULT |
| 1 | CreateVertexBuffer | usage 0x8 (write-only), pool MANAGED |
| 2 | CreateIndexBuffer | INDEX16, usage 0x208, pool DEFAULT |
| 27017 | VB Lock | flags 0x2000 (D3DLOCK_DISCARD); 1 lock with flags 0 |
| 27016 | IB Lock | flags 0x2000 (D3DLOCK_DISCARD) |
| 1082 | Surface LockRect | |
| 842 | Texture 2D LockRect | 96 cube-texture locks |
| 19241 | Clear | flags 0x1 (target), and 19241 with flags 0x3 (target and depth) |
| 19240 | SetRenderTarget / SetDepthStencilSurface | one pair per frame |
| 391 | StretchRect | use not characterised |
| 19240 | Present | one per frame |
| 2 | CreateDepthStencilSurface | D3DFMT_D24S8 |

Textures created (from the argument-bearing lines, out of 140 `CreateTexture` calls and 8 `CreateCubeTexture` calls):

| calls | Entry point | Format | Usage | Pool |
|---|---|---|---|---|
| 50 | CreateTexture | DXT5 (fourCC) | 0 | MANAGED |
| 4 | CreateTexture | DXT1 (fourCC) | 0 | MANAGED |
| 10 | CreateTexture | G16R16F | render target (0x1) | DEFAULT |
| 4 | CreateTexture | A8R8G8B8 | render target (0x1) | DEFAULT |
| 1 | CreateTexture | R5G6B5 | render target (0x1) | DEFAULT |
| 1 | CreateTexture | X8R8G8B8 | 0 | MANAGED |
| 2 | CreateCubeTexture | DXT3 (fourCC) | 0 | MANAGED |
| 1 | CreateCubeTexture | A16B16G16R16F | 0 | MANAGED |
| 1 | CreateCubeTexture | DXT5 (fourCC) | 0 | MANAGED |

`CheckDeviceFormat` was called 32 times: 16 DXT5 and 8 DXT1 for textures, 4 DXT3, 2 A16B16G16R16F, and 2 DXT5 for surfaces.

## Shader models (run B) and the full set

Run B is a subset. The binary holds more shaders than one run loads. The full survey is [d3d9-shaders.md](d3d9-shaders.md) (commit ffa6977): **587 distinct SM2 shaders** (352 `ps_2_0`, 235 `vs_2_0`), all in `VT3.exe` `.rdata`. There are no SM1 or SM3 streams. The NoCD 1.0.1 build alone has 572 distinct blobs (344 ps, 228 vs). The translator must cover the full 587, not only the 121 listings from run B.

Every shader has a `CTAB` constant table, so constant names are known. The meanings of the constant registers are hypotheses, not verified facts, until a trace confirms them.

## Shader models (run B)

- Vertex shaders: **vs_2_0 only**. 120 `CreateVertexShader` calls, 60 distinct listings.
- Pixel shaders: **ps_2_0 only**. 122 `CreatePixelShader` calls, 61 distinct listings.
- All 242 creations happen early in run B (before trace line 6,834 of about 1.7 million). No vs_1_x, ps_1_x or ps_3_0.
- Constant updates: `SetVertexShaderConstantF` 27016 calls, `SetPixelShaderConstantF` 38480 calls.

Instruction mix over the 121 distinct listings (from `shader_trace`): `add` 240, `mov` 194, `texld` 127 (+ `texld_pp` 37), `mul` 102 (+ `mul_pp` 9, `mul_sat` 16), `mad` 66 (+ `mad_pp` 29), `dp4` 64, `dp3` 7, `lrp` 20, `abs` 16, `frc` 5, `rcp` 3, `rsq` 1, `sincos` 1, `mova` 4, `mov_pp` 9, `mov_sat` 1, `def` 52, `defi` 3, `loop` 3, `endloop` 3. Declarations: `dcl_2d` 125, `dcl_position0` 60, `dcl_texcoord0` 51 (+1, +2), `dcl_color` 6, `dcl_weight` 4, `dcl_blend` 4, `dcl_normal0` 1, `dcl_pp` 37.

Implication (survey counts, all 587 shaders):

| Feature | Needed by | Translator handling |
|---|---|---|
| arithmetic `mov add sub mul mad` (+ `_pp`, `_sat`) | all | direct GLSL; `_pp` ignored, `_sat` clamps |
| `dp3 dp4 rcp rsq frc min max slt sge lrp abs nrm pow cmp sincos` | vs and ps | GLSL builtins; scalar forms replicated |
| `mova` and relative constant addressing (`c15 + a0.x`) | vs (74 shaders) | `a0` as `ivec4`; `vsc[n + a0.x]` |
| `loop / endloop` over `i#` with `aL` | vs (134) | GLSL `for`; counter from the int constant |
| `if / else / endif` on `b#` | vs (126) | GLSL `if` on the bool constant array |
| `texld` (ps, 1031) | ps | `texture(sN, uv)`; `texldp` projects |
| `def / defi` | all | constant defaults uploaded before draw |
| `dcl` (usage / texture type) | all | input locations and sampler types |

### Translator status (2026-10-14)

Implemented in `runtime/src/render/sm2_glsl.c` (interface `runtime/include/vt3/vt3_sm2.h`). It emits GLSL 330 core. Constant meanings are not encoded; the translator reproduces what the bytecode says.

- **Coverage:** all 572 distinct NoCD 1.0.1 blobs translate, with 0 failures. Those 572 are 228 `vs_2_0` and 344 `ps_2_0`. The other 15 of the 587 survey blobs come from other binaries and are not yet extracted.
- **Driver check:** `vt3rt --sm2-check <dir>` compiles all 228 vertex and 344 pixel shaders in a GL 3.3 core context (0 failures). It also links 21,280 sampled vertex/pixel pairs (0 failures). The blobs are extracted to `dump/runtime/shaders/bin`, outside the repo.
- **Semantic check:** `vt3rt --sm2-draw-test` assembles a `vs_2_0`/`ps_2_0` pair by hand (`dcl`, `mov`, `def`), draws through the GL backend, and reads back exact pixels. It covers `def` constants reaching the uniform array, the colour varying at location 15, and an indexed draw. It passes.
- **Not covered:** `texkill`, `rep`, `call`, `ifc` and predicates (none is in the opcode table). Loop, `rcp`, `rsq`, `pow`, `nrm` and `sincos` semantics stay hypotheses (listed above). They are not checked against hardware or a D3D reference.
- **Not built:** the D3D9 COM device layer (guest vtables, thunks, render and resource state, DrawIndexedPrimitive dispatch into the new programmable draw call). Until it exists, the subset is not wired into `runtime/shim/`.

Not yet decided: `texkill`, `rep`, `call`, `ifc` and predicates. None of them appears in the opcode list above.

Hypotheses to test before the translator is trusted: `loop` reads count, start and step from `i#` as (x, y, z); `rcp` and `rsq` use the `.x` component; `nrm` normalises all four components; `pow` uses `|x|`; `sincos` reads `.x`. Each is a guess from the names and the D3D9 reference, not checked on hardware.

## Implications for the shim

Needed for the first working scene:

1. IDirect3D9 and IDirect3DDevice9 objects, with the 40 device methods above, as guest vtables. All other slots log and return E_NOTIMPL.
2. Render state storage for the 16 states above, with blend, cull, depth, alpha test and colour write mapped to GL.
3. Texture stages 0 and 1 (MODULATE/DISABLE), and samplers 0 to 7 (wrap or clamp, linear and anisotropic filters).
4. Vertex declarations, stream sources, index buffers, and dynamic vertex and index buffers with DISCARD locks.
5. Triangle lists (indexed) and triangle strips with XYZRHW (`DrawPrimitiveUP`).
6. Textures: A8R8G8B8, X8R8G8B8, R5G6B5 and DXT1/3/5 (CPU decode or GL S3TC), plus render-target textures. Cube textures are needed.
7. Depth-stencil D24S8, Clear with target and depth, render-target switching, and StretchRect as a copy.
8. The SM2.0 translator for the instruction list above. Status: implemented and validated (see translator status); the COM device layer is not built yet.

Not needed based on this run: fixed-function lighting, fog, stencil, point sprites, `DrawPrimitive` (non-indexed), `DrawIndexedPrimitiveUP`, `SetTransform`, and vs_1_x or ps_3_0.

Still open: the D3DX shader compile path at load time (9 calls), which needs either an offline precompile of the 9 sources or a host HLSL compiler. The timers `timeSetEvent` and `timeKillEvent` are imported but not yet in the shim.
