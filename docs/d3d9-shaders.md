# D3D9 shader bytecode in Virtua Tennis 3 (PC)

Scope: find the compiled D3D9 shaders the game uses. Only `docs/d3d9-shaders.md` and `tools/formats/shaders/` were written. Nothing committed.

## Result

- **587 distinct shaders:** 352 `ps_2_0` and 235 `vs_2_0`. Found in 5 executables, 737 hits in the NoCD 1.0.1 build.
- **No other shader models.** No `vs_1_x`, `ps_1_x`, `vs_3_0` or `ps_3_0` streams were found. SM1 and SM3 are absent.
- **Location:** only in `VT3.exe`, in `.rdata` (virtual addresses 0x6cb880 to 0x75e538 in the NoCD 1.0.1 build). No shader in any `.xpac`, DLL, engine module, movie or data file (see Coverage).
- **Compiler:** Microsoft (R) D3DX9 Shader Compiler. Every shader carries a `CTAB` constant table, so constant names and registers are known.
- **Size:** 112-1728 bytes per shader; 1-78 instructions. Most are small: the median vs is 820 bytes, the median ps is 824 bytes.

### By binary

| Binary (short code) | Distinct blobs | Notes |
|---|---|---|
| NoCD 1.0.1 (`N1`) | 572 | `dump/patches/.../Virtua-Tennis-3_NoCD_Win_EN_for-version-101/VT3.exe` |
| Fenix NoCD 1.0.1 (`F`) | 572 | same blobs as `N1` |
| Patch 1.0.1 exe (`P`) | 572 | same blobs as `N1` |
| NoCD 1.0 (`N0`) | 337 | `dump/patches/.../for-version-10/VT3.exe` |
| SafeDisc disc copy (`D`) | 337 | `dump/pc/VT3.exe` (SafeDisc-packed; shaders are in `.rdata`, which is not encrypted) |

- Union 587. 322 blobs are in all five binaries.
- Version 1.0.1 adds 250 blobs that are absent from 1.0. Fifteen 1.0 blobs are absent from 1.0.1. The cause was not investigated.

## Method

1. Find candidate version tokens: 4-byte pattern `minor major FE FF` (vs) or `minor major FF FF` (ps), with minor 0-5 and major 0-3.
2. Walk the token stream with `tools/formats/shaders/d3d9_sm.py`. The walk must:
   - use only allowed versions (`vs_1_0`, `vs_1_1`, `vs_2_0`, `vs_2_1`, `vs_3_0`, `ps_1_0`-`ps_1_4`, `ps_2_0`, `ps_2_1`, `ps_3_0`);
   - use only known opcodes, and reject opcode 0 (nop), which is what zero bytes decode to;
   - in SM2 and later, give every operand token bit 31 set (except constant values after `def`/`defi`/`defb`), and require the next instruction token to be an opcode or the end token `0x0000FFFF`;
   - contain at least one instruction.
3. Check false positives: 200 MB of random bytes gave 3 candidates and 0 valid streams.
4. Decode each shader's `CTAB` comment (constant names, sets and indices) and disassemble it.

Recall fix: an earlier walker rejected any `def` with a positive float constant, because it required bit 31 on every operand. That hid most shaders (145 vs 737 hits in `N1`). The rule is now limited to the destination register.

## Shader summary

| | count | bytes (min / median / max) | instructions (min / median / max) |
|---|---|---|---|
| `vs_2_0` | 235 | 136 / 820 / 1716 | 4 / 33 / 78 |
| `ps_2_0` | 352 | 112 / 824 / 1728 | 1 / 28 / 70 |

### Basic opcodes (what they do, counted over the 587 distinct shaders)

| Opcode | Meaning | vs_2_0 count | ps_2_0 count |
|---|---|---|---|
| `mov` | copy; swizzle, write mask and modifiers apply | 912 | 962 |
| `add / sub` | a + b, a - b (per component) | 543 | 1042 |
| `mul` | a * b (per component) | 1192 | 1340 |
| `mad` | a * b + c (per component) | 2137 | 1227 |
| `mova` | float to address register a0 (rounded); indexes constants as c[n + a0] | 74 | 0 |
| `dp3 / dp4` | dot product of 3 or 4 components, written to each masked destination component | 887 | 763 |
| `rcp / rsq` | 1/x and 1/sqrt(x), scalar, applied per component | 288 | 171 |
| `frc` | fractional part x - floor(x) | 87 | 0 |
| `min / max` | component-wise minimum / maximum | 172 | 0 |
| `slt / sge` | 1.0 if a < b (slt) or a >= b (sge), else 0.0 | 36 | 0 |
| `lrp` | linear interpolation: a * b + (1 - a) * c | 12 | 19 |
| `abs` | absolute value | 6 | 16 |
| `nrm` | normalise a vector: x / length(x) | 102 | 320 |
| `pow` | x raised to y | 0 | 317 |
| `cmp` | dst = (src0 >= 0) ? src1 : src2 | 0 | 111 |
| `sincos` | sine and cosine of an input | 1 | 0 |
| `texld` | sample a texture: dst = sampler(coords); ps_2_0 form 'texld r0, t0, s0' | 0 | 1031 |
| `dcl` | declare an input (vs: usage, e.g. dcl_normal v3; ps: texture register or sampler type) | 870 | 2594 |
| `def / defi` | inline float / int constant (raw values follow the destination) | 627 | 365 |
| `loop / endloop` | loop over an integer register i# (counter in aL) | 134 | 0 |
| `if / else / endif` | conditional block on a boolean/predicate | 126 | 0 |

Operand modifiers seen: `-` (negate), `abs()`, `x2()`, `bias()`, swizzles (`.xy`, `.zwzw`). Write masks `dst.xy`. Relative addressing through `a0` (`c15 + a0.x`).

## Constants (CTAB)

Names are as the compiler recorded them. Meanings are inferred from the names and from how each shader uses the register. Treat them as hypotheses, not verified facts. Counts are shaders that declare the name.

| Name (register) | Inferred meaning | Shaders |
|---|---|---|
| `g_transform_mat (c0-c3)` | 4x4 transform (model-view-projection in most vs) | 210 |
| `g_world_mat (c4)` | world matrix | 142 |
| `g_vb_mat (c15, 180 regs)` | skinning palette: 3 rows per bone, 60 bones; indexed with mova | 68 |
| `g_shadow_tex_mat (c248)` | shadow-map texture projection (high register index) | 70 |
| `g_eye_pos (c8)` | camera position | 115 |
| `g_light_vec / g_light_color` | light direction and colour | 234 / 257 |
| `g_ambient, g_diffuse_color` | ambient and material colour | 35 / 203 |
| `g_uv0_scale (c14)` | texture coordinate scale | 161 |
| `g_shadow_epsilon, g_shadow_power` | shadow-map bias and sharpness | 111 each |
| `g_fresnel_const (c4, c12)` | Fresnel term | 108 |
| `g_samp_diffuse (s0), g_samp_normal (s2), g_samp_specular (s3)` | material textures | 263 / 114 / 46 |
| `g_samp_diffuse_env (s4), g_samp_specular_env (s5), g_samp_spherical_specular_env (s5)` | environment maps | 233 / 35 / 60 |
| `g_samp_shadowmap (s6), g_samp_planar_shadow (s8)` | shadow maps | 111 / 12 |
| `g_color_conversion (c0-c2)` | 3-row colour matrix (post-processing) | 12 |
| `g_dof, g_glare_value, g_afterimage_factor, g_samp_glare, g_samp_blured, g_samp_depth, g_samp_previous` | post-processing: depth of field, glare, afterimage | 16 / 5 / 12 / 12 / 16 / 16 / 12 |
| `g_ScoreBoardOffsets (c16), g_ScreenTwo (b#), g_SplitScreen (b#)` | HUD and split-screen layout (booleans) | 6 / 21 / 21 |

## Vertex inputs (`dcl` usage)

Counts are shaders, not declarations. All 235 `vs_2_0` shaders declare `position`. Others: `texcoord` (any index) 192, `normal` 172, `tangent` 69, `blendweight` and `blendindices` 68 each (the skinned shaders), `color` 29. Pixel shaders: 344 of 352 declare texture-coordinate registers (`dcl tN`), 328 declare a `2d` sampler and 233 a `cube` sampler.

Most common vertex input sets: `normal, position, texcoord` (54); `blendindices, blendweight, normal, position, tangent, texcoord` (38); `position, texcoord` (37).

## Worked example: skinning (S393)

A 16-instruction `vs_2_0` shader. It splits the blend index into integer and fraction, loads it into `a0`, and dots the vertex with three rows of the palette at `c15` (`g_vb_mat`, 180 constants = 3 rows x 60 bones). That count is consistent with the 60-bone Player skeleton (see `docs/formats.md`); the shader alone does not prove it. Then it weights the result by `v1.x` and transforms by `g_transform_mat` (c0-c3).

```
vs_2_0
dcl_position v0
dcl_blendweight v1
dcl_blendindices v2
frc r0.x, v2.x
add r0.x, -r0.x, v2.x
mova a0.x, r0.x
dp4 r0.x, v0, c15, a0.x
dp4 r0.y, v0, c16, a0.x
dp4 r0.z, v0, c17, a0.x
mul r0.xyz, r0, v1.x
mul r1, r0.y, c1
mad r1, c0, r0.x, r1
mad r0, c2, r0.z, r1
add r0, r0, c3
mov oPos, r0
mov oT0.xy, r0.zwzw
```

Stride note: the static mesh layout in `docs/formats.md` (stride 32 with 8 unexplained bytes) may be `blendweight` and `blendindices` for skinned meshes. This is a hypothesis; it has not been checked against a skinned mesh.

## Coverage

- `.xpac` archives: `game`, `frontend`, `stages`, `Motion`, `KANSEI`, `SE`, `BGM`, `VOICE_ADX` (`data1`). Every `data2` archive is byte-identical. **0 shaders.**
- Other game files: 121 files under `data1` (`Resource`, `Root`, `ConfigText`, `_Engine`, `_Support`), 6 DirectX/setup files, 8 engine-module files, and the 1.0.1 patch installer. **0 shaders.**
- Movies (`vt3_a.sfd`, `ps3_a.sfd`): 0.
- The random-data control (200 MB) gave 0 valid streams.
- Limitation: the walker only accepts opcodes in its table. A shader that used an unlisted opcode would be missed. None was seen, but this does not prove the list is complete.

## Tools (`tools/formats/shaders/`)

- `d3d9_sm.py`: finder, walker and disassembler (`walk`, `disassemble`, `parse_ctab`, `find_candidates`).
- `scan_d3d9.py FILE... [--tsv OUT] [--disasm OFFSET]`: list blobs per file, with the entry for CAPX archives. Write TSV outside the repo.
- `summarize_d3d9.py FILE... [--md OUT] [--tsv OUT] [--disasm-dir DIR]`: deduplicate by MD5 and report version, size, opcode histogram, CTAB constants, and where each blob occurs.
- Outputs were written to scratch space only, not to the repo or `dump/`.

## Table of all distinct blobs

Columns: ID; version; size in bytes; instruction count; opcode histogram; CTAB constants (name@register); binaries where the blob occurs (see By binary); virtual address in the NoCD 1.0.1 build (`-` if absent there); MD5 prefix. Disassembly for each ID can be regenerated with `summarize_d3d9.py --disasm-dir`.

| ID | Version | Bytes | Instr | Opcodes | Constants (CTAB) | Found in | VA (N1) | MD5 |
|---|---|---|---|---|---|---|---|---|
| S01 | ps_2_0 | 112 | 2 | dcl:1 mov:1 | - | FN1PN0D | 0x6d9130 | 7964497402bf |
| S02 | ps_2_0 | 112 | 2 | dcl:1 mov:1 | - | FN1PN0D | 0x6df880 | ca5a2ad7572e |
| S03 | ps_2_0 | 136 | 3 | mov:2 def:1 | - | FN1PN0D | 0x6d93f0 | 1437baaac750 |
| S04 | ps_2_0 | 152 | 1 | mov:1 | g_diffuse_color@c8 | FN1PN0D | 0x6e6a28 | 9f423191a60e |
| S05 | ps_2_0 | 180 | 3 | dcl:1 mov:1 mul:1 | g_diffuse_color@c8 | FN1PN0D | 0x6f3078 | 850e81aff423 |
| S06 | ps_2_0 | 188 | 4 | dcl:2 mov:1 texld:1 | g_TexDst@s0 | FN1PN0D | 0x6cebf0 | 3b999bae1153 |
| S07 | ps_2_0 | 188 | 4 | dcl:2 mov:1 texld:1 | g_Texture@s0 | FN1PN0D | 0x6cf310 | 99c3ee3dd75e |
| S08 | ps_2_0 | 188 | 4 | dcl:2 mov:1 texld:1 | g_samp_base@s2 | FN1PN0D | 0x6d23b0 | 9b842829d23e |
| S09 | ps_2_0 | 188 | 4 | mov:3 dcl:1 | g_diffuse_color@c8 | FN1PN0D | 0x6e6b98 | f206e7055e1a |
| S10 | ps_2_0 | 188 | 4 | mov:3 dcl:1 | g_diffuse_color@c8 | FN1PN0D | 0x6e6c58 | ffc499d9d5d1 |
| S11 | ps_2_0 | 188 | 4 | mov:3 dcl:1 | g_diffuse_color@c8 | FN1P | 0x712370 | 14cf7b155349 |
| S12 | ps_2_0 | 196 | 4 | dcl:2 mov:1 texld:1 | g_texture_sampler@s0 | FN1PN0D | 0x6cb908 | 15fe790da0ed |
| S13 | ps_2_0 | 216 | 6 | dcl:3 mov:1 mul:1 texld:1 | g_Texture@s0 | FN1PN0D | 0x6cbe80 | 20d461b53cf6 |
| S14 | ps_2_0 | 216 | 6 | dcl:3 mov:1 mul:1 texld:1 | g_texture@s0 | FN1PN0D | 0x6d9058 | 61886585622b |
| S15 | ps_2_0 | 216 | 5 | mov:3 def:1 dp3:1 | g_diffuse_color@c8 | FN1PN0D | 0x6e6ac0 | ead5ad235391 |
| S16 | ps_2_0 | 220 | 6 | dcl:3 mov:1 mul:1 texld:1 | g_trail_sampler@s0 | FN1PN0D | 0x6cbc38 | 1de2d0c876e5 |
| S17 | ps_2_0 | 220 | 5 | dcl:1 def:1 dp3:1 mov:1 mul:1 | g_diffuse_color@c8 | FN1PN0D | 0x6f3130 | abfb9b7078bf |
| S18 | ps_2_0 | 224 | 6 | dcl:3 mov:1 mul:1 texld:1 | g_diffuse_texture@s0 | FN1PN0D | 0x6cefc8 | de2c3e4394c3 |
| S19 | ps_2_0 | 228 | 3 | mov:3 | g_diffuse_color@c8 g_glare_value@c7 | FN1PN0D | 0x6e6d18 | 20da9c8b388a |
| S20 | ps_2_0 | 248 | 5 | dcl:2 mov:1 mul:1 texld:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d0d68 | e42f1c900811 |
| S21 | ps_2_0 | 252 | 8 | mov:5 dcl:1 def:1 dp3:1 | g_diffuse_color@c8 | FN1P | 0x712430 | c1f034a8a3eb |
| S22 | ps_2_0 | 256 | 5 | dcl:2 mov:2 texld:1 | g_glare_value@c7 g_samp_diffuse@s0 | FN1PN0D | 0x6da100 | c1c8bef4c5b1 |
| S23 | ps_2_0 | 256 | 8 | mov:3 dcl:2 def:1 dp3:1 mul:1 | g_diffuse_color@c8 | FN1P | 0x71e240 | 16a3420dc9c0 |
| S24 | ps_2_0 | 260 | 5 | dcl:2 mov:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1PN0D | 0x6e7678 | acb89604427d |
| S25 | ps_2_0 | 264 | 6 | mov:5 dcl:1 | g_diffuse_color@c8 g_glare_value@c7 | FN1P | 0x712530 | 7de3ee4b80c1 |
| S26 | ps_2_0 | 272 | 6 | dcl:2 mad:1 mov:1 mul:1 texld:1 | g_brightness@c0 g_samp@s0 | FN1PN0D | 0x6d21e8 | 2099c629d19e |
| S27 | ps_2_0 | 280 | 8 | dcl:4 texld:2 add:1 mov:1 | g_samp_base@s2 g_samp_glare@s4 | FN1PN0D | 0x6d2630 | cc66a3b03256 |
| S28 | ps_2_0 | 284 | 7 | dcl:3 mov:2 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1PN0D | 0x6f5280 | 5c509bf5dee7 |
| S29 | ps_2_0 | 288 | 7 | dcl:3 mul:2 mov:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1PN0D | 0x6f3210 | 043140e72ffe |
| S30 | ps_2_0 | 296 | 8 | dcl:3 texld:2 mad:1 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d0e60 | d9bf536ef9b0 |
| S31 | ps_2_0 | 296 | 8 | dcl:3 mov:3 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1P | 0x712d18 | 194f496e3488 |
| S32 | ps_2_0 | 300 | 7 | dcl:2 def:1 dp3:1 mov:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1PN0D | 0x6e7780 | b4d2370e1c46 |
| S33 | ps_2_0 | 304 | 7 | mov:2 dcl:1 def:1 dp3:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e6e00 | e9dd752bb45f |
| S34 | ps_2_0 | 304 | 7 | dcl:2 def:1 dp3:1 mov:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_illumination@s1 | FN1PN0D | 0x6eb938 | 7145dc855e85 |
| S35 | ps_2_0 | 316 | 10 | dcl:4 mov:2 texld:2 def:1 lrp:1 | g_TexDst@s0 g_TexSrc@s2 | FN1PN0D | 0x6ce580 | eb6b522d9de7 |
| S36 | ps_2_0 | 316 | 9 | dcl:3 texld:2 def:1 dp3:1 mov:1 mul:1 | g_samp_diffuse@s0 g_samp_illumination@s1 | FN1PN0D | 0x6df600 | 9a7a0299147b |
| S37 | ps_2_0 | 324 | 6 | dcl:2 mov:2 mul:1 texld:1 | g_diffuse_color@c8 g_glare_value@c7 g_samp_diffuse@s0 | FN1PN0D | 0x6e7c18 | 94ea04018a64 |
| S38 | ps_2_0 | 324 | 10 | dcl:4 mov:3 mul:2 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1P | 0x71e340 | b7d030e41e66 |
| S39 | ps_2_0 | 328 | 9 | dcl:3 mul:2 def:1 dp3:1 mov:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1PN0D | 0x6f3330 | 351e7b1d38e1 |
| S40 | ps_2_0 | 336 | 8 | dcl:3 mov:2 mad:1 mul:1 texld:1 | g_ambient@c5 g_light_color@c2 g_samp_diffuse@s0 | FN1PN0D | 0x6da200 | 2e743f52e289 |
| S41 | ps_2_0 | 336 | 10 | dcl:3 mov:3 def:1 dp3:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1P | 0x712e40 | b44df0ecb820 |
| S42 | ps_2_0 | 340 | 10 | dp4:3 mov:3 dcl:2 def:1 texld:1 | g_color_conversion@c0 g_samp_base@s2 | FN1PN0D | 0x6d3190 | 3dc14ef9905a |
| S43 | ps_2_0 | 340 | 10 | mov:4 dcl:2 def:1 dp3:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse_env@s4 | FN1P | 0x712638 | aedf24391ff3 |
| S44 | ps_2_0 | 340 | 10 | dcl:3 mov:3 def:1 dp3:1 mul:1 texld:1 | g_diffuse_color@c8 g_samp_illumination@s1 | FN1P | 0x716e10 | 49ae9131b0d3 |
| S45 | ps_2_0 | 344 | 10 | dcl:5 texld:3 lrp:1 mov:1 | g_TexDstNormal@s1 g_TexSrc@s2 g_TexSrcNormal@s3 | FN1PN0D | 0x6ce830 | e3989e253f27 |
| S46 | ps_2_0 | 344 | 11 | dcl:4 texld:3 mad:2 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d0f88 | 02ad272a321e |
| S47 | ps_2_0 | 352 | 12 | dcl:4 mov:3 texld:2 def:1 dp3:1 mul:1 | g_samp_diffuse@s0 g_samp_illumination@s1 | FN1P | 0x70af50 | 1e1cc06ac074 |
| S48 | ps_2_0 | 356 | 9 | dcl:4 texld:2 mad:1 mov:1 mul:1 | g_afterimage_factor@c0 g_samp_base@s2 g_samp_previous@s3 | FN1PN0D | 0x6d2960 | d2f474763758 |
| S49 | ps_2_0 | 356 | 9 | dcl:4 mul:2 texld:2 mov:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1PN0D | 0x6f2470 | 26c58899d565 |
| S50 | ps_2_0 | 360 | 9 | mov:4 dcl:3 mul:1 texld:1 | g_diffuse_color@c8 g_glare_value@c7 g_samp_diffuse@s0 | FN1P | 0x713208 | 494a08bfca0d |
| S51 | ps_2_0 | 364 | 10 | dcl:4 mov:2 texld:2 def:1 lrp:1 | g_Colour@c0 g_TexDst@s0 g_TexSrc@s2 | FN1PN0D | 0x6ce6c0 | fc90cf824761 |
| S52 | ps_2_0 | 364 | 12 | dcl:4 mov:3 mul:2 def:1 dp3:1 texld:1 | g_diffuse_color@c8 g_samp_diffuse@s0 | FN1P | 0x71e488 | 819ad24d6525 |
| S53 | ps_2_0 | 384 | 10 | dcl:3 mul:2 texld:2 def:1 dp3:1 mov:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1PN0D | 0x6ebc30 | a562d540e663 |
| S54 | ps_2_0 | 388 | 10 | dcl:5 texld:2 mad:1 mov:1 mul:1 | g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6da350 | 31ed09110c2b |
| S55 | ps_2_0 | 392 | 14 | dcl:5 texld:4 mad:3 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d10e0 | 9c74e73a6879 |
| S56 | ps_2_0 | 392 | 12 | dcl:5 mov:3 mul:2 texld:2 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1P | 0x71d5a8 | e8f49bfd82d6 |
| S57 | ps_2_0 | 396 | 10 | dcl:3 mov:2 mul:2 texld:2 def:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e7d60 | 43169e26f809 |
| S58 | ps_2_0 | 396 | 11 | dcl:4 mul:2 texld:2 def:1 dp3:1 mov:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1PN0D | 0x6f25d8 | e75064bd3494 |
| S59 | ps_2_0 | 412 | 11 | dcl:3 mov:2 mul:2 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e83a0 | 674ac3ed5cca |
| S60 | ps_2_0 | 412 | 12 | dcl:5 mov:3 texld:2 mad:1 mul:1 | g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x705a90 | 61e7391119da |
| S61 | ps_2_0 | 420 | 13 | dcl:3 mov:3 add:2 def:1 dp3:1 mad:1 nrm:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 | FN1PN0D | 0x6d9478 | d7e0dce6e083 |
| S62 | ps_2_0 | 420 | 13 | dcl:4 mov:3 mul:2 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1P | 0x717158 | ce3bf6c339ef |
| S63 | ps_2_0 | 424 | 13 | dcl:3 add:2 dp3:2 mov:2 def:1 mad:1 nrm:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 | FN1PN0D | 0x6d9920 | 3d1c11b8b0a3 |
| S64 | ps_2_0 | 432 | 14 | dcl:4 dp4:3 mov:3 texld:2 add:1 def:1 | g_color_conversion@c0 g_samp_base@s2 g_samp_glare@s4 | FN1PN0D | 0x6d3540 | aa296e1664f1 |
| S65 | ps_2_0 | 432 | 13 | dcl:4 mov:4 mul:2 texld:2 def:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x713370 | a1d49b82d5a1 |
| S66 | ps_2_0 | 432 | 14 | dcl:5 mov:3 mul:2 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 | FN1P | 0x71d730 | 167eb7e01c25 |
| S67 | ps_2_0 | 440 | 17 | dcl:6 texld:5 mad:4 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d1268 | 513294713a51 |
| S68 | ps_2_0 | 440 | 13 | dcl:4 mul:3 mov:2 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6f3478 | c5d225d4c06d |
| S69 | ps_2_0 | 444 | 14 | dcl:6 texld:3 abs:1 add:1 lrp:1 mov:1 mul:1 | g_dof@c0 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 | FN1P | 0x6d2470 | ef816ccc568d |
| S70 | ps_2_0 | 444 | 11 | dcl:3 mov:2 mul:2 def:1 dp3:1 mad:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 | FN1PN0D | 0x6e8e28 | a95c3895186e |
| S71 | ps_2_0 | 444 | 15 | mov:5 dcl:3 add:2 def:1 dp3:1 mad:1 nrm:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 | FN1P | 0x7050e0 | cfcdfae61c32 |
| S72 | ps_2_0 | 444 | 14 | dcl:6 texld:3 abs:1 add:1 lrp:1 mov:1 mul:1 | g_dof@c0 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 | N0D | - | 6ac096a029fc |
| S73 | ps_2_0 | 448 | 13 | dcl:6 texld:3 add:1 mad:1 mov:1 mul:1 | g_afterimage_factor@c0 g_samp_base@s2 g_samp_glare@s4 g_samp_previous@s3 | FN1PN0D | 0x6d2d20 | 64da68a48f7b |
| S74 | ps_2_0 | 448 | 15 | mov:4 dcl:3 add:2 dp3:2 def:1 mad:1 nrm:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 | FN1P | 0x7055b8 | de0d331586a3 |
| S75 | ps_2_0 | 448 | 14 | dcl:4 mov:4 mul:2 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x713a30 | 85c3a16506ad |
| S76 | ps_2_0 | 456 | 11 | dcl:5 mul:2 texld:2 mad:1 mov:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e86e0 | 4bb25a748924 |
| S77 | ps_2_0 | 456 | 14 | dcl:4 add:2 mul:2 texld:2 def:1 dp3:1 mad:1 mov:1 | g_diffuse_color@c8 g_samp_illumination@s1 g_samp_planar_shadow@s8 | FN1PN0D | 0x6eba68 | 0c79cf807259 |
| S78 | ps_2_0 | 472 | 13 | dcl:7 texld:3 mad:2 mov:1 | g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular_env@s5 | FN1PN0D | 0x6e1b28 | 913bff75059b |
| S79 | ps_2_0 | 476 | 16 | dcl:5 mov:4 mul:3 texld:2 def:1 dp3:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x71e5f8 | fe9c738cf07b |
| S80 | ps_2_0 | 480 | 13 | dcl:5 mov:3 mul:2 texld:2 mad:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x713db0 | c4bb82221cfc |
| S81 | ps_2_0 | 480 | 14 | dcl:4 mov:4 mul:2 def:1 dp3:1 mad:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 | FN1P | 0x714540 | 9786811917cf |
| S82 | ps_2_0 | 488 | 20 | dcl:7 texld:6 mad:5 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d1420 | dca4597f6bf0 |
| S83 | ps_2_0 | 492 | 17 | dcl:5 mov:3 add:2 mul:2 texld:2 def:1 dp3:1 mad:1 | g_diffuse_color@c8 g_samp_illumination@s1 g_samp_planar_shadow@s8 | FN1P | 0x716f68 | 62d85795296b |
| S84 | ps_2_0 | 496 | 14 | dcl:4 mad:2 mov:2 add:1 def:1 dp3:1 nrm:1 pow:1 texld:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e7120 | e0ecf7e342c9 |
| S85 | ps_2_0 | 496 | 13 | dcl:5 mul:2 texld:2 def:1 dp3:1 mad:1 mov:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e9220 | 141ef78c6510 |
| S86 | ps_2_0 | 500 | 14 | dcl:4 dp3:2 mad:2 add:1 def:1 mov:1 nrm:1 pow:1 texld:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 | FN1PN0D | 0x6f4c18 | 6c06f671a421 |
| S87 | ps_2_0 | 508 | 15 | dcl:4 dp4:3 mov:3 texld:2 def:1 mad:1 mul:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_samp_base@s2 g_samp_previous@s3 | FN1PN0D | 0x6d39a0 | 78ebfbeee281 |
| S88 | ps_2_0 | 512 | 17 | dcl:5 add:2 mov:2 mul:2 texld:2 def:1 dp3:1 mad:1 nrm:1 | g_ambient@c5 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1PN0D | 0x6e1468 | 92c7aae8b6e7 |
| S89 | ps_2_0 | 520 | 16 | dcl:4 mov:4 mad:2 add:1 def:1 dp3:1 nrm:1 pow:1 texld:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 | FN1P | 0x712790 | ebf41fc4d1ec |
| S90 | ps_2_0 | 520 | 15 | dcl:5 mov:3 mul:2 texld:2 def:1 dp3:1 mad:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x714970 | 03edd41f7bfd |
| S91 | ps_2_0 | 524 | 15 | dcl:6 mul:3 texld:2 def:1 dp3:1 mad:1 mov:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6f3630 | aa48daf116ef |
| S92 | ps_2_0 | 524 | 16 | dcl:4 mov:3 dp3:2 mad:2 add:1 def:1 nrm:1 pow:1 texld:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 | FN1P | 0x71fdf8 | 7f66d5a52a19 |
| S93 | ps_2_0 | 532 | 18 | dcl:8 texld:4 add:2 abs:1 lrp:1 mov:1 mul:1 | g_dof@c0 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 | FN1P | 0x6d2748 | cb24f2dbc174 |
| S94 | ps_2_0 | 532 | 18 | dcl:8 texld:4 add:2 abs:1 lrp:1 mov:1 mul:1 | g_dof@c0 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 | N0D | - | f8f5cc1f6414 |
| S95 | ps_2_0 | 536 | 23 | dcl:8 texld:7 mad:6 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d1608 | 9f57e27ae706 |
| S96 | ps_2_0 | 536 | 19 | dcl:5 mov:4 add:2 mul:2 texld:2 def:1 dp3:1 mad:1 nrm:1 | g_ambient@c5 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1P | 0x70cda8 | 938218e18b66 |
| S97 | ps_2_0 | 548 | 18 | dcl:6 mul:3 texld:3 add:2 def:1 dp3:1 mad:1 mov:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 g_samp_planar_shadow@s8 | FN1PN0D | 0x6f2768 | f8d7de3ee11c |
| S98 | ps_2_0 | 548 | 17 | dcl:6 mov:3 mul:3 texld:2 def:1 dp3:1 mad:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x71e7d8 | 3e30f2789f45 |
| S99 | ps_2_0 | 560 | 17 | dcl:6 dp3:2 mad:2 texld:2 add:1 def:1 mov:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6da7c8 | 2be3f1f92623 |
| S100 | ps_2_0 | 560 | 16 | dcl:6 texld:4 mov:2 mul:2 def:1 mad:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_specular_env@s5 | FN1PN0D | 0x6f86c8 | 082afde8bbdb |
| S101 | ps_2_0 | 568 | 17 | dcl:5 dp3:2 mad:2 mov:2 add:1 def:1 mul:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 | FN1PN0D | 0x6e8fe8 | 08b501950b81 |
| S102 | ps_2_0 | 580 | 18 | dcl:5 mul:3 add:2 mov:2 texld:2 def:1 dp3:1 mad:1 nrm:1 | g_ambient@c5 g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1PN0D | 0x6f3dc0 | 5536ec84177b |
| S103 | ps_2_0 | 584 | 26 | dcl:9 texld:8 mad:7 mov:1 mul:1 | g_samp@s0 g_weight@c0 | FN1PN0D | 0x6d1820 | 36825f831123 |
| S104 | ps_2_0 | 584 | 19 | dcl:6 mov:3 dp3:2 mad:2 texld:2 add:1 def:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x705f38 | 369a50f0bf38 |
| S105 | ps_2_0 | 584 | 21 | dcl:7 mov:3 mul:3 texld:3 add:2 def:1 dp3:1 mad:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_illumination@s1 g_samp_planar_shadow@s8 | FN1P | 0x71d8e0 | 24e6af50bedc |
| S106 | ps_2_0 | 592 | 17 | dcl:6 mad:2 texld:2 add:1 def:1 dp3:1 mov:1 mul:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e88a8 | b3e2db0ceae1 |
| S107 | ps_2_0 | 592 | 19 | dcl:5 mov:4 dp3:2 mad:2 add:1 def:1 mul:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 | FN1P | 0x714720 | f34dc01cd46f |
| S108 | ps_2_0 | 596 | 19 | dcl:8 texld:4 mul:2 abs:1 add:1 lrp:1 mad:1 mov:1 | g_afterimage_factor@c0 g_dof@c1 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_previous@s3 | FN1P | 0x6d2ac8 | 500fb0efd21b |
| S109 | ps_2_0 | 596 | 20 | dcl:6 dp4:3 mov:3 texld:3 abs:1 add:1 def:1 lrp:1 mul:1 | g_color_conversion@c0 g_dof@c3 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 | FN1P | 0x6d32e8 | 9cf27be23f61 |
| S110 | ps_2_0 | 596 | 21 | dcl:7 texld:3 add:2 mad:2 mov:2 mul:2 def:1 dp3:1 nrm:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ec478 | 6b20eb4e3038 |
| S111 | ps_2_0 | 596 | 19 | dcl:8 texld:4 mul:2 abs:1 add:1 lrp:1 mad:1 mov:1 | g_afterimage_factor@c0 g_dof@c1 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_previous@s3 | N0D | - | 7827b114f113 |
| S112 | ps_2_0 | 596 | 20 | dcl:6 dp4:3 mov:3 texld:3 abs:1 add:1 def:1 lrp:1 mul:1 | g_color_conversion@c0 g_dof@c3 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 | N0D | - | bbb1a9dac754 |
| S113 | ps_2_0 | 600 | 19 | dcl:6 dp4:3 mov:3 texld:3 add:1 def:1 mad:1 mul:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_samp_base@s2 g_samp_glare@s4 g_samp_previous@s3 | FN1PN0D | 0x6d3e90 | fc2d5df8d782 |
| S114 | ps_2_0 | 604 | 20 | dcl:5 mov:4 mul:3 add:2 texld:2 def:1 dp3:1 mad:1 nrm:1 | g_ambient@c5 g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1P | 0x71efb0 | 6e86f9ff53d8 |
| S115 | ps_2_0 | 608 | 18 | dcl:6 dp3:2 mad:2 texld:2 add:1 def:1 mov:1 mul:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6e9410 | 40dff3954753 |
| S116 | ps_2_0 | 612 | 18 | dcl:7 mul:3 texld:3 add:1 def:1 dp3:1 mad:1 mov:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 | FN1PN0D | 0x6e9d40 | 222e203f9fe6 |
| S117 | ps_2_0 | 616 | 19 | dcl:6 mov:3 mad:2 texld:2 add:1 def:1 dp3:1 mul:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x713f90 | ac9beccb18dc |
| S118 | ps_2_0 | 620 | 23 | dcl:7 mov:4 texld:3 add:2 mad:2 mul:2 def:1 dp3:1 nrm:1 | g_diffuse_color@c8 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7179f8 | ca017982df3b |
| S119 | ps_2_0 | 624 | 19 | dcl:7 texld:3 mad:2 add:1 def:1 dp3:1 mov:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1PN0D | 0x6de0d0 | 75154960105c |
| S120 | ps_2_0 | 632 | 20 | dcl:6 mov:3 dp3:2 mad:2 texld:2 add:1 def:1 mul:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x714b78 | 24fd55979c30 |
| S121 | ps_2_0 | 636 | 20 | dcl:7 dp3:2 mad:2 mul:2 texld:2 add:1 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6f3840 | c84c2a8cb38c |
| S122 | ps_2_0 | 636 | 20 | dcl:7 mov:3 mul:3 texld:3 add:1 def:1 dp3:1 mad:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 | FN1P | 0x7154f0 | 1b5aa615c3d3 |
| S123 | ps_2_0 | 640 | 20 | dcl:7 texld:3 dp3:2 mad:2 add:1 def:1 mov:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1PN0D | 0x6de710 | 485e2c7add39 |
| S124 | ps_2_0 | 648 | 22 | dcl:7 mad:3 texld:3 add:2 dp3:2 mov:2 def:1 mul:1 nrm:1 | g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6daca8 | ae0822d1df5a |
| S125 | ps_2_0 | 648 | 21 | dcl:7 mov:3 texld:3 mad:2 add:1 def:1 dp3:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1P | 0x709990 | 9e99facee3e1 |
| S126 | ps_2_0 | 652 | 21 | dcl:8 texld:3 dp3:2 mad:2 add:1 def:1 mov:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1PN0D | 0x6e03b0 | 5efbad31ae57 |
| S127 | ps_2_0 | 660 | 22 | dcl:7 mov:3 dp3:2 mad:2 mul:2 texld:2 add:1 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x71ea00 | 04d5b6a4fffb |
| S128 | ps_2_0 | 664 | 22 | dcl:7 mov:3 texld:3 dp3:2 mad:2 add:1 def:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1P | 0x70a000 | 343117b5881d |
| S129 | ps_2_0 | 672 | 20 | dcl:7 texld:3 mad:2 mul:2 add:1 def:1 dp3:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1PN0D | 0x6efd80 | 6a304809c2a7 |
| S130 | ps_2_0 | 672 | 24 | dcl:7 mov:4 mad:3 texld:3 add:2 dp3:2 def:1 mul:1 nrm:1 | g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x706448 | 8a63c442267f |
| S131 | ps_2_0 | 676 | 23 | dcl:8 mov:3 texld:3 dp3:2 mad:2 add:1 def:1 mul:1 nrm:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1P | 0x70bc90 | 5d04f22db307 |
| S132 | ps_2_0 | 684 | 23 | dcl:10 texld:5 add:2 mul:2 abs:1 lrp:1 mad:1 mov:1 | g_afterimage_factor@c0 g_dof@c1 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 g_samp_previous@s3 | FN1P | 0x6d2ee0 | f37770e95eee |
| S133 | ps_2_0 | 684 | 24 | dcl:8 texld:4 dp4:3 mov:3 add:2 abs:1 def:1 lrp:1 mul:1 | g_color_conversion@c0 g_dof@c3 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 | FN1P | 0x6d36f0 | c1164477996c |
| S134 | ps_2_0 | 684 | 23 | dcl:6 mul:4 mad:3 add:2 dp3:2 mov:2 def:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 | FN1PN0D | 0x6da9f8 | b86fcb5f1dda |
| S135 | ps_2_0 | 684 | 23 | dcl:10 texld:5 add:2 mul:2 abs:1 lrp:1 mad:1 mov:1 | g_afterimage_factor@c0 g_dof@c1 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 g_samp_previous@s3 | N0D | - | b126af7d1f8c |
| S136 | ps_2_0 | 684 | 24 | dcl:8 texld:4 dp4:3 mov:3 add:2 abs:1 def:1 lrp:1 mul:1 | g_color_conversion@c0 g_dof@c3 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 | N0D | - | 515a260ed088 |
| S137 | ps_2_0 | 688 | 21 | dcl:7 texld:3 dp3:2 mad:2 mul:2 add:1 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1PN0D | 0x6f0420 | b83a5d238609 |
| S138 | ps_2_0 | 696 | 22 | dcl:7 mov:3 texld:3 mad:2 mul:2 add:1 def:1 dp3:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1P | 0x71b438 | dbc29a3907cb |
| S139 | ps_2_0 | 700 | 22 | dcl:7 mad:3 texld:3 add:2 mov:2 mul:2 def:1 dp3:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ebdb0 | 10559a5b6058 |
| S140 | ps_2_0 | 708 | 25 | dcl:6 mov:4 mul:4 mad:3 add:2 dp3:2 def:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 | FN1P | 0x706180 | 4c70d3c1d133 |
| S141 | ps_2_0 | 712 | 24 | dcl:7 mul:4 mad:3 mov:3 texld:3 add:1 def:1 dp3:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x702768 | 55daa82cd637 |
| S142 | ps_2_0 | 712 | 23 | dcl:7 mov:3 texld:3 dp3:2 mad:2 mul:2 add:1 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 | FN1P | 0x71bb08 | 7a86d5141696 |
| S143 | ps_2_0 | 716 | 23 | dcl:7 mad:3 texld:3 add:2 dp3:2 mov:2 mul:2 def:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ec6d0 | 610cc0baedbb |
| S144 | ps_2_0 | 716 | 23 | dcl:7 mad:3 texld:3 add:2 dp3:2 mov:2 mul:2 def:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6f2990 | 60f06c8a6ce6 |
| S145 | ps_2_0 | 724 | 24 | dcl:7 mov:4 mad:3 texld:3 add:2 mul:2 def:1 dp3:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x717300 | 649a06cb11f2 |
| S146 | ps_2_0 | 728 | 24 | dcl:8 mad:3 texld:3 add:2 dp3:2 mov:2 mul:2 def:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6f1d70 | 2a2efba7423d |
| S147 | ps_2_0 | 736 | 26 | dcl:7 mov:5 mul:4 mad:3 texld:3 add:1 def:1 dp3:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x72c690 | a9b022f56e3d |
| S148 | ps_2_0 | 740 | 24 | dcl:8 mul:3 texld:3 add:2 dp3:2 mad:2 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 | FN1PN0D | 0x6e9fa8 | 3be470a93554 |
| S149 | ps_2_0 | 740 | 27 | dcl:5 mov:5 add:3 mad:3 mul:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fbfb8 | efde4436bb6c |
| S150 | ps_2_0 | 740 | 25 | dcl:7 mov:4 mad:3 texld:3 add:2 dp3:2 mul:2 def:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x717c68 | 8f5f6ac68555 |
| S151 | ps_2_0 | 740 | 25 | dcl:7 mov:4 mad:3 texld:3 add:2 dp3:2 mul:2 def:1 nrm:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x71db28 | 6b214a1a38cd |
| S152 | ps_2_0 | 748 | 25 | dcl:8 texld:4 dp4:3 mov:3 mul:2 abs:1 add:1 def:1 lrp:1 mad:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_dof@c4 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_previous@s3 | FN1P | 0x6d3ba0 | 34a98b0b5943 |
| S153 | ps_2_0 | 748 | 24 | dcl:7 add:3 mad:3 mul:3 texld:3 cmp:1 def:1 mov:1 pow:1 rcp:1 | g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6da4d8 | 172db7aeae7e |
| S154 | ps_2_0 | 748 | 25 | dcl:8 texld:4 dp4:3 mov:3 mul:2 abs:1 add:1 def:1 lrp:1 mad:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_dof@c4 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_previous@s3 | N0D | - | 532a9d98b76d |
| S155 | ps_2_0 | 752 | 26 | dcl:7 mul:4 mad:3 mov:3 texld:3 def:2 dp3:2 add:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x702e48 | d886f9150607 |
| S156 | ps_2_0 | 764 | 25 | dcl:5 add:4 mad:4 mov:2 mul:2 pow:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6d9620 | 611504696075 |
| S157 | ps_2_0 | 764 | 26 | dcl:7 mul:6 mad:3 add:2 dp3:2 mov:2 def:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 | FN1PN0D | 0x6f3ac0 | 850680c13389 |
| S158 | ps_2_0 | 764 | 26 | dcl:8 mov:3 mul:3 texld:3 add:2 dp3:2 mad:2 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 | FN1P | 0x715770 | 7621a15f75ca |
| S159 | ps_2_0 | 764 | 29 | mov:7 dcl:5 add:3 mad:3 mul:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_spherical_specular_env@s5 | FN1P | 0x726650 | 8a9dda653fde |
| S160 | ps_2_0 | 768 | 25 | dcl:5 add:4 mad:4 dp3:2 mul:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6d9ac8 | 2f191cf778d5 |
| S161 | ps_2_0 | 772 | 26 | dcl:7 add:3 mad:3 mov:3 mul:3 texld:3 cmp:1 def:1 pow:1 rcp:1 | g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x705c30 | 393ff82d58bb |
| S162 | ps_2_0 | 776 | 25 | dcl:7 mul:5 mad:3 add:2 dp3:2 texld:2 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6ea650 | 27d0203834b0 |
| S163 | ps_2_0 | 776 | 28 | dcl:7 mov:5 mul:4 mad:3 texld:3 def:2 dp3:2 add:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x72cda0 | b9e0542f73b6 |
| S164 | ps_2_0 | 780 | 28 | dcl:6 mov:4 add:3 mad:3 mul:3 def:2 dp3:2 texld:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fc2a0 | f43ac3f2d9e4 |
| S165 | ps_2_0 | 788 | 27 | dcl:5 add:4 mad:4 mov:4 mul:2 pow:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7052a0 | 797cf63a07e8 |
| S166 | ps_2_0 | 788 | 28 | dcl:7 mul:6 mov:4 mad:3 add:2 dp3:2 def:1 nrm:1 pow:1 texld:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 | FN1P | 0x71ec98 | c3aa42636d41 |
| S167 | ps_2_0 | 792 | 26 | dcl:7 mul:5 dp3:3 mad:3 add:2 texld:2 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1PN0D | 0x6eadc0 | 2e4eed71833d |
| S168 | ps_2_0 | 792 | 27 | dcl:5 add:4 mad:4 mov:3 dp3:2 mul:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 texld:1 | g_ambient@c5 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x705778 | e7145ffb367c |
| S169 | ps_2_0 | 796 | 28 | dcl:8 mad:4 add:3 dp3:3 texld:3 mov:2 nrm:2 def:1 mul:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6daf30 | 362579ba3096 |
| S170 | ps_2_0 | 800 | 27 | dcl:7 mul:5 mad:3 mov:3 add:2 dp3:2 texld:2 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x715e48 | 59ca10d3d668 |
| S171 | ps_2_0 | 804 | 30 | dcl:6 mov:6 add:3 mad:3 mul:3 def:2 dp3:2 texld:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x726950 | d513e1fea81b |
| S172 | ps_2_0 | 812 | 30 | dcl:8 add:3 mad:3 mov:3 mul:3 texld:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fa290 | e0e94d9171a4 |
| S173 | ps_2_0 | 816 | 25 | dcl:7 mul:4 add:3 mad:3 texld:3 cmp:1 def:1 mov:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e8af8 | 667cce87570c |
| S174 | ps_2_0 | 816 | 28 | dcl:7 mul:5 dp3:3 mad:3 mov:3 add:2 texld:2 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 | FN1P | 0x7165e8 | c63d37f4f3f8 |
| S175 | ps_2_0 | 820 | 30 | dcl:8 mad:4 mov:4 add:3 dp3:3 texld:3 nrm:2 def:1 mul:1 pow:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7066e8 | 987727071c51 |
| S176 | ps_2_0 | 824 | 27 | dcl:6 add:4 mad:4 dp3:2 mul:2 pow:2 texld:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6d9dc8 | fc58dc1f4076 |
| S177 | ps_2_0 | 824 | 30 | dcl:7 mul:5 add:4 dp3:3 mad:3 mov:2 nrm:2 texld:2 def:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1PN0D | 0x6e1668 | fef6e0ec35b6 |
| S178 | ps_2_0 | 828 | 31 | dcl:8 add:3 dp3:3 mad:3 mov:3 mul:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fade0 | 57c6a858a2b0 |
| S179 | ps_2_0 | 832 | 26 | dcl:7 mul:4 add:3 mad:3 texld:3 cmp:1 def:1 dp3:1 mov:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e9a00 | cc517e71f757 |
| S180 | ps_2_0 | 832 | 28 | dcl:7 mul:6 dp3:3 mad:3 add:2 mov:2 texld:2 def:1 nrm:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_specular@s3 | FN1PN0D | 0x6f0ae0 | ccb4ff04254c |
| S181 | ps_2_0 | 836 | 29 | dcl:10 texld:5 dp4:3 mov:3 add:2 mul:2 abs:1 def:1 lrp:1 mad:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_dof@c4 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 g_samp_previous@s3 | FN1P | 0x6d40e8 | da32b73d622c |
| S182 | ps_2_0 | 836 | 32 | dcl:8 mov:5 add:3 mad:3 mul:3 texld:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x724f90 | 645a3ebe0b35 |
| S183 | ps_2_0 | 836 | 29 | dcl:10 texld:5 dp4:3 mov:3 add:2 mul:2 abs:1 def:1 lrp:1 mad:1 | g_afterimage_factor@c3 g_color_conversion@c0 g_dof@c4 g_samp_base@s2 g_samp_blured@s0 g_samp_depth@s1 g_samp_glare@s4 g_samp_previous@s3 | N0D | - | 2906b1d26ad3 |
| S184 | ps_2_0 | 840 | 27 | dcl:7 mul:4 add:3 mad:3 mov:3 texld:3 cmp:1 def:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7141f8 | 7e475342f0f6 |
| S185 | ps_2_0 | 844 | 29 | dcl:8 mad:4 add:3 dp3:3 texld:3 mov:2 mul:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ec9a0 | ee5d0032711f |
| S186 | ps_2_0 | 848 | 32 | dcl:7 mul:5 add:4 mov:4 dp3:3 mad:3 nrm:2 texld:2 def:1 pow:1 | g_ambient@c5 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1P | 0x70cfc0 | d1950ae4ad81 |
| S187 | ps_2_0 | 856 | 28 | dcl:7 mul:4 add:3 mad:3 mov:3 texld:3 cmp:1 def:1 dp3:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x715198 | 91d2078a7c4c |
| S188 | ps_2_0 | 856 | 30 | dcl:7 mul:6 mov:4 dp3:3 mad:3 add:2 texld:2 def:1 nrm:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_specular@s3 | FN1P | 0x71c1f8 | 9dbc8a493efc |
| S189 | ps_2_0 | 868 | 28 | dcl:6 add:4 mad:4 mul:3 mov:2 pow:2 texld:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e7310 | 127f41d41aa2 |
| S190 | ps_2_0 | 868 | 31 | dcl:8 mad:4 mov:4 add:3 dp3:3 texld:3 mul:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x717f50 | 4e2b024f25f3 |
| S191 | ps_2_0 | 872 | 31 | dcl:9 mad:4 add:3 dp3:3 mul:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6f4378 | 3be992ac7c3a |
| S192 | ps_2_0 | 872 | 28 | dcl:6 add:4 mad:4 mul:3 dp3:2 pow:2 texld:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f4e10 | e967cbd38512 |
| S193 | ps_2_0 | 876 | 31 | dcl:7 mul:6 add:4 dp3:3 mad:3 mov:2 nrm:2 texld:2 def:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1PN0D | 0x6f4008 | c493272a3bcd |
| S194 | ps_2_0 | 876 | 32 | dcl:8 mul:4 add:3 dp3:3 mad:3 mov:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fce40 | c80ec8ad727e |
| S195 | ps_2_0 | 892 | 30 | dcl:9 mul:5 mad:4 dp3:3 texld:3 add:2 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular_env@s5 | FN1PN0D | 0x6eb0d8 | a48421f871e3 |
| S196 | ps_2_0 | 892 | 30 | dcl:6 add:4 mad:4 mov:4 mul:3 pow:2 texld:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x712998 | ede2fa079457 |
| S197 | ps_2_0 | 896 | 33 | dcl:9 mad:4 mov:4 add:3 dp3:3 mul:3 texld:3 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x71f598 | 25200f074e6e |
| S198 | ps_2_0 | 896 | 30 | dcl:6 add:4 mad:4 mov:3 mul:3 dp3:2 pow:2 texld:2 cmp:1 def:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x720008 | 511f7fc3242e |
| S199 | ps_2_0 | 900 | 33 | dcl:7 mul:6 add:4 mov:4 dp3:3 mad:3 nrm:2 texld:2 def:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_normal@s2 | FN1P | 0x71f210 | d28ed5e77420 |
| S200 | ps_2_0 | 900 | 34 | dcl:8 mov:5 mul:4 add:3 dp3:3 mad:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x727538 | 14e54f575d05 |
| S201 | ps_2_0 | 908 | 30 | dcl:7 add:4 mad:4 mul:4 dp3:2 pow:2 texld:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e9670 | fae411d767b9 |
| S202 | ps_2_0 | 920 | 32 | dcl:10 mul:5 mad:4 texld:4 dp3:3 add:2 def:1 mov:1 nrm:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_specular_env@s5 | FN1PN0D | 0x6ded70 | 33755ddbd7ce |
| S203 | ps_2_0 | 928 | 35 | dcl:10 mad:4 mul:4 texld:4 add:3 mov:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fa5c0 | b39e5c105fcd |
| S204 | ps_2_0 | 932 | 32 | dcl:7 add:4 mad:4 mul:4 mov:3 dp3:2 pow:2 texld:2 cmp:1 def:1 nrm:1 rcp:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x714df0 | c0b7ed322451 |
| S205 | ps_2_0 | 940 | 34 | dcl:9 mul:5 texld:4 add:3 mad:3 mov:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x700008 | 41d940d00979 |
| S206 | ps_2_0 | 944 | 34 | dcl:10 mul:5 mad:4 texld:4 dp3:3 mov:3 add:2 def:1 nrm:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_specular_env@s5 | FN1P | 0x70a690 | 819c0abeac1e |
| S207 | ps_2_0 | 952 | 33 | dcl:9 mul:8 add:3 dp3:3 mad:3 texld:3 def:1 mov:1 nrm:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_planar_shadow@s8 g_samp_specular@s3 | FN1PN0D | 0x6f11f0 | 829ceb995c4b |
| S208 | ps_2_0 | 956 | 35 | dcl:9 mul:5 texld:4 add:3 dp3:3 mad:3 mov:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x700cd8 | 34cd3a868526 |
| S209 | ps_2_0 | 960 | 35 | dcl:9 mad:5 mul:5 add:4 dp3:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6dbaa0 | 817be5db8e95 |
| S210 | ps_2_0 | 960 | 35 | dcl:9 mad:5 mul:5 add:4 dp3:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6e5830 | 74a51e396ece |
| S211 | ps_2_0 | 960 | 32 | dcl:8 add:4 mad:4 mul:4 texld:3 dp3:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ea290 | 05f524fb728f |
| S212 | ps_2_0 | 964 | 36 | dcl:9 mov:5 mul:5 texld:4 add:3 mad:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1P | 0x72a290 | a212912e6cc5 |
| S213 | ps_2_0 | 972 | 36 | dcl:10 mad:5 mul:5 add:4 dp3:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6e4f38 | 0fa59d9ed958 |
| S214 | ps_2_0 | 972 | 33 | dcl:10 mul:6 mad:4 texld:4 dp3:3 add:2 def:1 mov:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_specular_env@s5 | FN1PN0D | 0x6f0e20 | ce2f32a68c54 |
| S215 | ps_2_0 | 976 | 36 | dcl:9 mad:5 mul:5 add:4 dp3:4 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6dcd98 | 2da25ca86585 |
| S216 | ps_2_0 | 976 | 33 | dcl:9 add:4 mad:4 mul:4 texld:4 pow:2 cmp:1 def:1 dp3:1 mov:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6de340 | 06b074c3a6f2 |
| S217 | ps_2_0 | 976 | 35 | dcl:9 mul:8 add:3 dp3:3 mad:3 mov:3 texld:3 def:1 nrm:1 pow:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_planar_shadow@s8 g_samp_specular@s3 | FN1P | 0x71c938 | 6003f8974f6c |
| S218 | ps_2_0 | 980 | 35 | dcl:9 add:5 mad:5 texld:4 mul:3 dp3:2 mov:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6db250 | 76246ca57dee |
| S219 | ps_2_0 | 980 | 37 | dcl:9 mov:5 mul:5 texld:4 add:3 dp3:3 mad:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1P | 0x72ab70 | 310be982dc4b |
| S220 | ps_2_0 | 984 | 37 | dcl:9 mad:5 mul:5 add:4 mov:4 dp3:3 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7072a0 | 9c96d4cf3396 |
| S221 | ps_2_0 | 984 | 37 | dcl:9 mad:5 mul:5 add:4 mov:4 dp3:3 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x711118 | cbf6364730ed |
| S222 | ps_2_0 | 984 | 34 | dcl:8 add:4 mad:4 mul:4 mov:3 texld:3 dp3:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x715a70 | c7d2ed26e8d9 |
| S223 | ps_2_0 | 988 | 37 | dcl:10 mad:5 mul:5 add:4 dp3:4 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6e4620 | 70cea7458be7 |
| S224 | ps_2_0 | 988 | 37 | dcl:10 mul:6 mad:5 add:4 dp3:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6e6110 | e48f8063c4f8 |
| S225 | ps_2_0 | 992 | 34 | dcl:9 add:4 mad:4 mul:4 texld:4 dp3:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6de990 | 3dd63edabf2b |
| S226 | ps_2_0 | 996 | 38 | dcl:10 mad:5 mul:5 add:4 mov:4 dp3:3 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7107f0 | 0eeea1437889 |
| S227 | ps_2_0 | 996 | 35 | dcl:10 mul:6 mad:4 texld:4 dp3:3 mov:3 add:2 def:1 nrm:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_specular_env@s5 | FN1P | 0x71c550 | 04d9c93c63ec |
| S228 | ps_2_0 | 1000 | 38 | dcl:9 mad:5 mul:5 add:4 dp3:4 mov:4 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7085f8 | 39ccb12f7c42 |
| S229 | ps_2_0 | 1000 | 35 | dcl:9 add:4 mad:4 mul:4 texld:4 mov:3 pow:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x709c18 | 3e53bbd12f7c |
| S230 | ps_2_0 | 1004 | 35 | dcl:10 add:4 mad:4 mul:4 texld:4 dp3:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e0640 | f5a3f3f062b5 |
| S231 | ps_2_0 | 1004 | 37 | dcl:9 add:5 mad:5 mov:4 texld:4 mul:3 dp3:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x706a20 | 059e15d708df |
| S232 | ps_2_0 | 1012 | 36 | dcl:9 mul:6 mad:5 add:4 dp3:3 texld:3 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ed5b0 | 61b25177b8d4 |
| S233 | ps_2_0 | 1012 | 39 | dcl:10 mad:5 mul:5 add:4 dp3:4 mov:4 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x70fea8 | a9f5e25d3320 |
| S234 | ps_2_0 | 1012 | 39 | dcl:10 mul:6 mad:5 add:4 mov:4 dp3:3 texld:3 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x711a28 | 373285b88b6d |
| S235 | ps_2_0 | 1016 | 36 | dcl:9 add:4 mad:4 mul:4 texld:4 mov:3 dp3:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x70a298 | b9c9fd71a77f |
| S236 | ps_2_0 | 1024 | 34 | dcl:9 mul:5 add:4 mad:4 texld:4 pow:2 cmp:1 def:1 dp3:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f0020 | e794e5294cd6 |
| S237 | ps_2_0 | 1028 | 37 | dcl:9 mul:6 mad:5 add:4 dp3:4 texld:3 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6ee978 | b34eb1c61ed7 |
| S238 | ps_2_0 | 1028 | 37 | dcl:10 add:4 mad:4 mul:4 texld:4 mov:3 dp3:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x70bf38 | 2919b84da2b6 |
| S239 | ps_2_0 | 1032 | 35 | dcl:9 add:5 mad:5 mul:4 texld:4 mov:2 cmp:1 def:1 dp3:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ec070 | 5163dfcfa664 |
| S240 | ps_2_0 | 1036 | 38 | dcl:9 mul:6 mad:5 add:4 mov:4 dp3:3 texld:3 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x718ba8 | 8d669e419bad |
| S241 | ps_2_0 | 1040 | 35 | dcl:9 mul:5 add:4 mad:4 texld:4 dp3:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f06d0 | a99a687819b6 |
| S242 | ps_2_0 | 1040 | 38 | dcl:10 mul:6 mad:5 add:4 dp3:4 texld:3 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1PN0D | 0x6f9738 | 6064db5a89d4 |
| S243 | ps_2_0 | 1044 | 37 | dcl:9 mul:6 mad:5 add:4 texld:4 mov:3 cmp:1 def:1 dp3:1 pow:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x702a30 | 47b36ffe01be |
| S244 | ps_2_0 | 1048 | 36 | dcl:9 add:5 mad:5 mul:4 texld:4 dp3:2 mov:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6eccf0 | dbd5b50880ae |
| S245 | ps_2_0 | 1048 | 36 | dcl:9 add:5 mad:5 mul:4 texld:4 dp3:2 mov:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f2c60 | bab5b0817e36 |
| S246 | ps_2_0 | 1048 | 39 | dcl:9 mul:8 mad:5 add:3 dp3:3 mov:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fd670 | 29f3e2f04501 |
| S247 | ps_2_0 | 1048 | 36 | dcl:9 mul:5 add:4 mad:4 texld:4 mov:3 pow:2 cmp:1 def:1 dp3:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71b6f0 | d5674387eca4 |
| S248 | ps_2_0 | 1052 | 39 | dcl:9 mul:6 mad:5 add:4 dp3:4 mov:4 texld:3 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x719fd0 | 5bb6ec3315ff |
| S249 | ps_2_0 | 1056 | 39 | dcl:11 mul:6 texld:5 mad:4 add:3 mov:3 def:2 dp3:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_planar_shadow@s8 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x7003b8 | 01c35d434ed2 |
| S250 | ps_2_0 | 1056 | 37 | dcl:9 add:5 mad:5 mov:4 mul:4 texld:4 cmp:1 def:1 dp3:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7175d8 | 07ad5be13d74 |
| S251 | ps_2_0 | 1060 | 37 | dcl:10 add:5 mad:5 mul:4 texld:4 dp3:2 mov:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f2048 | 0aeb9a87246d |
| S252 | ps_2_0 | 1064 | 40 | dcl:9 mul:8 mad:5 dp3:4 add:3 mov:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fe000 | 4aa957bdd83e |
| S253 | ps_2_0 | 1064 | 37 | dcl:9 mul:5 add:4 mad:4 texld:4 mov:3 dp3:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71bdd0 | cb39a4f92301 |
| S254 | ps_2_0 | 1064 | 40 | dcl:10 mul:6 mad:5 add:4 dp3:4 mov:4 texld:3 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 | FN1P | 0x7245e0 | c35ad6f062b7 |
| S255 | ps_2_0 | 1068 | 39 | dcl:7 mad:6 add:5 mul:5 mov:4 def:2 dp3:2 pow:2 texld:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fc5b0 | ea62fb460850 |
| S256 | ps_2_0 | 1068 | 39 | dcl:9 mul:6 mad:5 mov:5 add:4 texld:4 cmp:1 def:1 dp3:1 pow:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x72c970 | f1f7502a5e5f |
| S257 | ps_2_0 | 1072 | 38 | dcl:9 add:5 mad:5 mov:4 mul:4 texld:4 dp3:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7182b8 | f080cf9e7f1c |
| S258 | ps_2_0 | 1072 | 38 | dcl:9 add:5 mad:5 mov:4 mul:4 texld:4 dp3:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71de10 | 39504e5911b3 |
| S259 | ps_2_0 | 1072 | 41 | dcl:9 mul:8 mad:5 mov:5 add:3 dp3:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x727d98 | 91263936afbd |
| S260 | ps_2_0 | 1084 | 39 | dcl:9 mul:6 mad:5 add:4 texld:4 mov:3 def:2 dp3:2 cmp:1 pow:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x7034a8 | 380848996d85 |
| S261 | ps_2_0 | 1084 | 39 | dcl:10 add:5 mad:5 mov:4 mul:4 texld:4 dp3:2 cmp:1 def:1 nrm:1 pow:1 rcp:1 | g_diffuse_color@c8 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71d168 | eda000cb2dde |
| S262 | ps_2_0 | 1088 | 42 | dcl:9 mul:8 mad:5 mov:5 dp3:4 add:3 texld:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_spherical_specular_env@s5 | FN1P | 0x728758 | b12d76ead52a |
| S263 | ps_2_0 | 1092 | 39 | dcl:10 mul:6 mad:5 add:4 dp3:4 texld:4 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular@s3 | FN1PN0D | 0x6f15a8 | a0b7a6323e37 |
| S264 | ps_2_0 | 1092 | 41 | dcl:7 mad:6 mov:6 add:5 mul:5 def:2 dp3:2 pow:2 texld:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_ambient@c5 g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x726c78 | ea889a18585f |
| S265 | ps_2_0 | 1108 | 41 | dcl:9 mul:6 mad:5 mov:5 add:4 texld:4 def:2 dp3:2 cmp:1 pow:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x72d430 | 81bfac37f9df |
| S266 | ps_2_0 | 1112 | 45 | dcl:10 add:6 mad:6 dp3:5 mov:4 mul:4 texld:4 def:2 nrm:2 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fb5b0 | d786ed23ce39 |
| S267 | ps_2_0 | 1116 | 42 | dcl:10 mad:7 add:5 dp3:5 mul:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6dbe60 | e1bab9fc463c |
| S268 | ps_2_0 | 1116 | 41 | dcl:10 mul:6 mad:5 add:4 dp3:4 mov:4 texld:4 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular@s3 | FN1P | 0x71cd08 | c07b2b1e2a71 |
| S269 | ps_2_0 | 1120 | 41 | dcl:8 mad:6 add:5 mul:5 mov:4 texld:3 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fc9e0 | 7e97ba776e0a |
| S270 | ps_2_0 | 1128 | 39 | dcl:9 mul:8 add:5 mad:5 texld:3 dp3:2 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ea958 | e7e6fc3279d0 |
| S271 | ps_2_0 | 1132 | 43 | dcl:10 mad:7 dp3:6 add:5 mul:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6dd168 | 1d70f9155a48 |
| S272 | ps_2_0 | 1132 | 43 | dcl:10 mad:7 dp3:6 add:5 mul:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6e0a30 | 1391ebd0728d |
| S273 | ps_2_0 | 1136 | 47 | dcl:10 add:6 mad:6 mov:6 dp3:5 mul:4 texld:4 def:2 nrm:2 pow:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_spherical_specular_env@s5 | FN1P | 0x725c18 | 04601b64bd19 |
| S274 | ps_2_0 | 1140 | 44 | dcl:10 mad:7 add:5 dp3:5 mul:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x707678 | 84f5c346b2dc |
| S275 | ps_2_0 | 1144 | 42 | dcl:10 add:6 mad:6 mul:4 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6db628 | 3421c820d0c2 |
| S276 | ps_2_0 | 1144 | 44 | dcl:11 mad:7 dp3:6 add:5 mul:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6df960 | 2b32f9f26bc3 |
| S277 | ps_2_0 | 1144 | 43 | dcl:10 mul:9 mad:5 dp3:4 texld:4 add:3 mov:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x7015a8 | fc4b25394c4c |
| S278 | ps_2_0 | 1144 | 43 | dcl:8 mad:6 mov:6 add:5 mul:5 texld:3 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7270c0 | 5bd22341f944 |
| S279 | ps_2_0 | 1148 | 43 | dcl:10 mad:6 add:5 mul:5 texld:4 mov:3 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fa960 | db94333397e7 |
| S280 | ps_2_0 | 1152 | 41 | dcl:9 mul:8 add:5 mad:5 mov:3 texld:3 dp3:2 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x716168 | 853cfb9e8e89 |
| S281 | ps_2_0 | 1156 | 52 | def:12 mad:12 texld:12 add:11 dcl:2 mov:2 mul:1 | Sampler0@s0 fScale@c0 | FN1PN0D | 0x6d8aa0 | 000d983c3ea5 |
| S282 | ps_2_0 | 1156 | 45 | dcl:10 mad:7 dp3:6 add:5 mul:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x7089e0 | fb0d868cfb78 |
| S283 | ps_2_0 | 1156 | 45 | dcl:10 mad:7 dp3:6 add:5 mul:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x70c340 | 95a684c280d4 |
| S284 | ps_2_0 | 1160 | 46 | dcl:10 add:6 mad:6 dp3:5 mul:5 mov:4 texld:4 def:2 nrm:2 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fe9b0 | f5b9c1f76b5f |
| S285 | ps_2_0 | 1164 | 44 | dcl:10 mad:6 add:5 mul:5 texld:4 dp3:3 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fb120 | d5a4d86f0230 |
| S286 | ps_2_0 | 1168 | 43 | dcl:10 mad:7 mul:6 add:5 dp3:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6ed9a8 | 544ac7f78a59 |
| S287 | ps_2_0 | 1168 | 44 | dcl:10 add:6 mad:6 mov:4 mul:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x706e10 | 752fe4ae45cc |
| S288 | ps_2_0 | 1168 | 46 | dcl:11 mad:7 dp3:6 add:5 mul:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x70b210 | 0edee2430414 |
| S289 | ps_2_0 | 1168 | 45 | dcl:10 mul:9 mad:5 mov:5 dp3:4 texld:4 add:3 def:2 nrm:1 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_specular@s3 g_samp_spherical_specular_env@s5 | FN1P | 0x72b470 | c08815122bba |
| S290 | ps_2_0 | 1172 | 45 | dcl:10 mad:6 add:5 mov:5 mul:5 texld:4 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7252d8 | fe0de24c47b8 |
| S291 | ps_2_0 | 1184 | 44 | dcl:10 mad:7 dp3:6 mul:6 add:5 texld:4 mov:2 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1PN0D | 0x6eed80 | 52d7bc82aaee |
| S292 | ps_2_0 | 1184 | 48 | dcl:10 add:6 mad:6 mov:6 dp3:5 mul:5 texld:4 def:2 nrm:2 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_spherical_specular_env@s5 | FN1P | 0x729138 | 0fdcd0698055 |
| S293 | ps_2_0 | 1188 | 46 | dcl:10 mad:6 add:5 mov:5 mul:5 texld:4 dp3:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x725770 | 389bb0e550be |
| S294 | ps_2_0 | 1192 | 43 | dcl:10 add:6 mad:6 mul:5 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ed108 | 6f9db068fe2b |
| S295 | ps_2_0 | 1192 | 45 | dcl:10 mad:7 mul:6 add:5 dp3:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x718fb8 | 3621e5958da1 |
| S296 | ps_2_0 | 1208 | 46 | dcl:10 mad:7 dp3:6 mul:6 add:5 mov:4 texld:4 nrm:2 def:1 pow:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_specular_env@s5 | FN1P | 0x71a3f0 | 59cce179cc48 |
| S297 | ps_2_0 | 1212 | 45 | dcl:10 mad:6 mul:6 add:5 texld:4 dp3:3 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fd1b0 | 220c3059005f |
| S298 | ps_2_0 | 1216 | 45 | dcl:10 add:6 mad:6 mul:5 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7186e8 | 484ccd6d84e2 |
| S299 | ps_2_0 | 1220 | 45 | dcl:11 add:6 mad:6 mul:6 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f46e0 | 46a34c3bf99c |
| S300 | ps_2_0 | 1236 | 47 | dcl:10 mad:6 mul:6 add:5 mov:5 texld:4 dp3:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7278c0 | cb4efdb8dd44 |
| S301 | ps_2_0 | 1244 | 44 | dcl:11 mul:8 mad:6 add:5 texld:4 dp3:3 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6eb458 | 212f42822b52 |
| S302 | ps_2_0 | 1244 | 47 | dcl:11 add:6 mad:6 mul:6 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71f918 | 76008e9cd481 |
| S303 | ps_2_0 | 1268 | 46 | dcl:11 mul:8 mad:6 add:5 texld:4 dp3:3 mov:3 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x716918 | 6d2cb163bf65 |
| S304 | ps_2_0 | 1272 | 46 | dcl:12 mul:8 mad:6 add:5 texld:5 dp3:3 pow:2 cmp:1 def:1 mov:1 nrm:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6df108 | 8512a06a2d51 |
| S305 | ps_2_0 | 1276 | 47 | dcl:11 mul:7 mad:6 add:5 texld:5 mov:3 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x7007d8 | 73a736e2386e |
| S306 | ps_2_0 | 1292 | 48 | dcl:11 mul:7 mad:6 add:5 texld:5 dp3:3 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x701098 | e8dcf571f8d9 |
| S307 | ps_2_0 | 1296 | 48 | dcl:12 mul:8 mad:6 add:5 texld:5 dp3:3 mov:3 pow:2 cmp:1 def:1 nrm:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x70aa40 | 077dbf99ded9 |
| S308 | ps_2_0 | 1300 | 49 | dcl:11 mul:7 mad:6 add:5 mov:5 texld:5 def:2 dp3:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x72a658 | cd85ef627e2a |
| S309 | ps_2_0 | 1308 | 49 | dcl:11 mul:8 add:7 mad:7 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6dc2c0 | d8f303354d89 |
| S310 | ps_2_0 | 1308 | 49 | dcl:11 mul:8 add:7 mad:7 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e5bf0 | 317cae038fd3 |
| S311 | ps_2_0 | 1316 | 50 | dcl:11 mul:7 mad:6 add:5 mov:5 texld:5 dp3:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x72af48 | 3358fe0cf618 |
| S312 | ps_2_0 | 1320 | 50 | dcl:12 mul:8 add:7 mad:7 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e5308 | 7a39df653a39 |
| S313 | ps_2_0 | 1324 | 50 | dcl:11 mul:8 add:7 mad:7 dp3:4 texld:4 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6dd5d8 | 740b78cad4da |
| S314 | ps_2_0 | 1332 | 51 | dcl:11 mul:8 add:7 mad:7 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x707af0 | 85b527995d98 |
| S315 | ps_2_0 | 1332 | 51 | dcl:11 mul:8 add:7 mad:7 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7114f0 | 48296d11880a |
| S316 | ps_2_0 | 1336 | 51 | dcl:12 mul:8 add:7 mad:7 dp3:4 texld:4 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e4a00 | 7193bd34dc38 |
| S317 | ps_2_0 | 1336 | 51 | dcl:12 mul:9 add:7 mad:7 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e64f0 | e96f33cc993f |
| S318 | ps_2_0 | 1344 | 52 | dcl:12 mul:8 add:7 mad:7 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x710bd8 | 1ca36bae42dd |
| S319 | ps_2_0 | 1348 | 52 | dcl:11 mul:8 add:7 mad:7 dp3:4 mov:4 texld:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x708e68 | 3c6382570cd7 |
| S320 | ps_2_0 | 1352 | 54 | dcl:10 mul:9 mad:8 dp3:7 add:6 mov:4 texld:4 def:2 nrm:2 pow:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_spherical_specular_env@s5 | FN1PN0D | 0x6fee38 | 72b2e256b4dd |
| S321 | ps_2_0 | 1360 | 50 | dcl:11 mul:9 add:7 mad:7 texld:4 dp3:3 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ede38 | 2769f9296750 |
| S322 | ps_2_0 | 1360 | 53 | dcl:12 mul:8 add:7 mad:7 dp3:4 mov:4 texld:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7102a0 | 2a3fcd3074bd |
| S323 | ps_2_0 | 1360 | 53 | dcl:12 mul:9 add:7 mad:7 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x711e20 | cb1a51368763 |
| S324 | ps_2_0 | 1376 | 51 | dcl:11 mul:9 add:7 mad:7 dp3:4 texld:4 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ef220 | 2520d457e62f |
| S325 | ps_2_0 | 1384 | 52 | dcl:11 mul:9 add:7 mad:7 mov:4 texld:4 dp3:3 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x719460 | 78e50a7e8ed4 |
| S326 | ps_2_0 | 1388 | 52 | dcl:12 mul:9 add:7 mad:7 dp3:4 texld:4 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6f9b48 | bdc037298e0b |
| S327 | ps_2_0 | 1396 | 53 | dcl:11 mul:11 mad:7 add:6 texld:4 dp3:3 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fda88 | 6b8a8f66c4a2 |
| S328 | ps_2_0 | 1400 | 53 | dcl:11 mul:9 add:7 mad:7 dp3:4 mov:4 texld:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71a8a8 | e7d418d944cc |
| S329 | ps_2_0 | 1412 | 54 | dcl:11 mul:11 mad:7 add:6 dp3:4 texld:4 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fe428 | 8a6c7017bdb9 |
| S330 | ps_2_0 | 1412 | 54 | dcl:12 mul:9 add:7 mad:7 dp3:4 mov:4 texld:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x724a08 | 41202894c892 |
| S331 | ps_2_0 | 1420 | 55 | dcl:11 mul:11 mad:7 add:6 mov:5 texld:4 dp3:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7281c8 | 054f692aac01 |
| S332 | ps_2_0 | 1436 | 56 | dcl:11 mul:11 mad:7 add:6 mov:5 dp3:4 texld:4 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x728b98 | dc78d7741bf6 |
| S333 | ps_2_0 | 1452 | 58 | dcl:12 mad:9 add:8 mul:6 dp3:5 texld:5 mov:4 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6fba08 | 528fc395a657 |
| S334 | ps_2_0 | 1464 | 56 | dcl:12 mad:9 add:8 mul:8 dp3:5 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6dc7e0 | ee8dc4dd29c0 |
| S335 | ps_2_0 | 1476 | 60 | dcl:12 mad:9 add:8 mov:6 mul:6 dp3:5 texld:5 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x726088 | 642c6760d2a2 |
| S336 | ps_2_0 | 1480 | 57 | dcl:12 mad:9 add:8 mul:8 dp3:6 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ddb08 | 3fa0c743dfd7 |
| S337 | ps_2_0 | 1480 | 57 | dcl:12 mad:9 add:8 mul:8 dp3:6 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6e0ea0 | f6e0307bc64d |
| S338 | ps_2_0 | 1488 | 58 | dcl:12 mad:9 add:8 mul:8 dp3:5 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x708028 | cea793a0062d |
| S339 | ps_2_0 | 1492 | 58 | dcl:13 mad:9 add:8 mul:8 dp3:6 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6dfdd8 | 58f5f0aaf523 |
| S340 | ps_2_0 | 1492 | 57 | dcl:12 mul:12 mad:7 add:6 texld:5 dp3:4 mov:3 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x701a20 | 2c0580390927 |
| S341 | ps_2_0 | 1500 | 59 | dcl:12 mad:9 add:8 mul:7 dp3:5 texld:5 mov:4 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ff380 | ca30f9e9dc56 |
| S342 | ps_2_0 | 1504 | 59 | dcl:12 mad:9 add:8 mul:8 dp3:6 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7093b0 | 512c2f45c00f |
| S343 | ps_2_0 | 1504 | 59 | dcl:12 mad:9 add:8 mul:8 dp3:6 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x70c7c8 | d2cd233d0d50 |
| S344 | ps_2_0 | 1516 | 57 | dcl:12 mad:9 mul:9 add:8 dp3:5 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ee388 | 8985b1aa523c |
| S345 | ps_2_0 | 1516 | 60 | dcl:13 mad:9 add:8 mul:8 dp3:6 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x70b6a0 | db166983dc8f |
| S346 | ps_2_0 | 1516 | 59 | dcl:12 mul:12 mad:7 add:6 mov:5 texld:5 dp3:4 def:2 pow:2 cmp:1 nrm:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_shadowmap@s6 g_samp_specular@s3 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x72b900 | bd36b1b6df61 |
| S347 | ps_2_0 | 1524 | 61 | dcl:12 mad:9 add:8 mul:7 mov:6 dp3:5 texld:5 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7295d8 | 272d5be03503 |
| S348 | ps_2_0 | 1532 | 58 | dcl:12 mad:9 mul:9 add:8 dp3:6 texld:5 mov:2 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ef780 | 271487d9c7ec |
| S349 | ps_2_0 | 1540 | 59 | dcl:12 mad:9 mul:9 add:8 dp3:5 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x7199c8 | fb66f38c5a4a |
| S350 | ps_2_0 | 1556 | 60 | dcl:12 mad:9 mul:9 add:8 dp3:6 texld:5 mov:4 nrm:2 pow:2 cmp:1 def:1 rcp:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x71ae20 | fa54d6ef6ab4 |
| S351 | ps_2_0 | 1704 | 68 | dcl:12 mul:12 mad:10 add:9 dp3:7 texld:5 mov:4 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1PN0D | 0x6ff960 | 0088b6fdb4ce |
| S352 | ps_2_0 | 1728 | 70 | dcl:12 mul:12 mad:10 add:9 dp3:7 mov:6 texld:5 def:2 nrm:2 pow:2 cmp:1 rcp:1 rsq:1 | g_diffuse_color@c8 g_direct_specular@c0 g_fresnel_const@c4 g_light_color@c2 g_light_vec@c3 g_samp_diffuse@s0 g_samp_diffuse_env@s4 g_samp_normal@s2 g_samp_shadowmap@s6 g_samp_spherical_specular_env@s5 g_shadow_epsilon@c10 g_shadow_power@c9 | FN1P | 0x729bd0 | fe441ec7b09e |
| S353 | vs_2_0 | 136 | 4 | dcl:2 mov:2 | - | FN1PN0D | 0x6cb880 | 97634d2cebae |
| S354 | vs_2_0 | 184 | 4 | dcl:2 add:1 mov:1 | g_off@c0 | FN1PN0D | 0x6d1a68 | f4343b9db35c |
| S355 | vs_2_0 | 192 | 4 | dcl:2 add:1 mov:1 | g_offset_base@c0 | FN1PN0D | 0x6d64b0 | d94a25025110 |
| S356 | vs_2_0 | 200 | 5 | add:2 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1b20 | 12a819cc4b19 |
| S357 | vs_2_0 | 216 | 6 | add:3 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1be8 | 3e8bbb43bb0c |
| S358 | vs_2_0 | 228 | 5 | mad:3 dcl:1 mul:1 | g_transform_mat@c0 | FN1PN0D | 0x72f0f8 | 60f7a5cc382d |
| S359 | vs_2_0 | 232 | 7 | add:4 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1cc0 | bca8c7e07bbf |
| S360 | vs_2_0 | 244 | 7 | add:2 dcl:2 mad:1 mov:1 mul:1 | g_ProjectionMat@c0 | FN1PN0D | 0x6cecb0 | 22b9139c4305 |
| S361 | vs_2_0 | 244 | 5 | add:2 dcl:2 mov:1 | g_offset_base@c0 g_offset_glare@c1 | FN1PN0D | 0x6d6698 | 491fc219ed57 |
| S362 | vs_2_0 | 248 | 8 | add:5 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1da8 | 3e0fb9258be1 |
| S363 | vs_2_0 | 248 | 5 | add:2 dcl:2 mov:1 | g_offset_base@c0 g_offset_previous@c1 | FN1PN0D | 0x6d68f0 | 252df046eca2 |
| S364 | vs_2_0 | 252 | 7 | mad:3 mov:2 dcl:1 mul:1 | g_transform_mat@c0 | FN1PN0D | 0x6d0278 | 9c3a6ec18b69 |
| S365 | vs_2_0 | 252 | 7 | mad:3 dcl:2 mov:1 mul:1 | g_transform_mat@c0 | FN1PN0D | 0x730868 | 568ff8b181c4 |
| S366 | vs_2_0 | 264 | 9 | add:6 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1ea0 | d4c5cf2d8690 |
| S367 | vs_2_0 | 268 | 9 | dcl:3 add:2 mov:2 mad:1 mul:1 | g_ProjectionMat@c0 | FN1PN0D | 0x6ceae0 | 4adef6a506a7 |
| S368 | vs_2_0 | 272 | 9 | dcl:3 mad:2 mov:2 add:1 mul:1 | g_ProjectionMat@c0 | FN1PN0D | 0x6d8f48 | 10e4bc4687db |
| S369 | vs_2_0 | 272 | 9 | dcl:3 mad:3 mov:2 mul:1 | transform@c0 | FN1PN0D | 0x6d9200 | a874d38d5ec1 |
| S370 | vs_2_0 | 276 | 9 | dcl:3 mad:3 mov:2 mul:1 | modelViewProj@c0 | FN1PN0D | 0x6cbb20 | b8978e3cb7ed |
| S371 | vs_2_0 | 280 | 9 | dcl:3 mad:2 mov:2 add:1 mul:1 | g_ModelViewProjection@c0 | FN1PN0D | 0x6cbd68 | a56de09e68ad |
| S372 | vs_2_0 | 280 | 10 | add:7 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d1fa8 | 32cc3cc4ed8f |
| S373 | vs_2_0 | 296 | 11 | add:8 dcl:2 mov:1 | g_off@c0 | FN1PN0D | 0x6d20c0 | af2065230943 |
| S374 | vs_2_0 | 296 | 6 | add:3 dcl:2 mov:1 | g_offset_base@c0 g_offset_blured@c1 g_offset_depth@c2 | FN1PN0D | 0x6d6570 | fa0ebc0da83f |
| S375 | vs_2_0 | 300 | 6 | add:3 dcl:2 mov:1 | g_offset_base@c0 g_offset_glare@c1 g_offset_previous@c2 | FN1PN0D | 0x6d6b48 | 0fb15e7d0a6d |
| S376 | vs_2_0 | 300 | 6 | mad:3 dcl:1 mov:1 mul:1 | g_planar_shadow_color@c13 g_transform_mat@c0 | FN1PN0D | 0x7331a8 | b4d039d1ea20 |
| S377 | vs_2_0 | 304 | 7 | mad:3 dcl:2 mul:2 | g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x72f1e0 | 628743bf88de |
| S378 | vs_2_0 | 304 | 10 | mad:3 mov:2 mul:2 dcl:1 def:1 rcp:1 | g_transform_mat@c0 | FN1P | 0x7482c8 | afa537653571 |
| S379 | vs_2_0 | 324 | 8 | mad:3 dcl:2 mov:2 mul:1 | g_planar_shadow_color@c13 g_transform_mat@c0 | FN1PN0D | 0x734310 | bb8e1506ee3a |
| S380 | vs_2_0 | 328 | 9 | dcl:3 mad:3 mul:2 mov:1 | g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x72f528 | 0790d0cf0bfe |
| S381 | vs_2_0 | 328 | 9 | dcl:3 mad:3 mul:2 mov:1 | g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x730968 | da70dd0e4c1f |
| S382 | vs_2_0 | 328 | 12 | mad:3 mov:3 dcl:2 mul:2 def:1 rcp:1 | g_transform_mat@c0 | FN1P | 0x7494f8 | ac4bb7e18321 |
| S383 | vs_2_0 | 332 | 8 | mad:3 dcl:1 def:1 dp4:1 mov:1 mul:1 | g_depth_vector@c11 g_transform_mat@c0 | FN1PN0D | 0x72f930 | 13b43bf23b96 |
| S384 | vs_2_0 | 348 | 7 | add:4 dcl:2 mov:1 | g_offset_base@c0 g_offset_blured@c1 g_offset_depth@c2 g_offset_glare@c3 | FN1PN0D | 0x6d6790 | 36f22402b792 |
| S385 | vs_2_0 | 352 | 7 | add:4 dcl:2 mov:1 | g_offset_base@c0 g_offset_blured@c1 g_offset_depth@c2 g_offset_previous@c3 | FN1PN0D | 0x6d69e8 | ef4b8fcce52d |
| S386 | vs_2_0 | 376 | 8 | mad:3 dcl:2 mul:2 mov:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x7332d8 | 3ac20f00b879 |
| S387 | vs_2_0 | 380 | 12 | mad:3 mul:3 dcl:2 mov:2 def:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x7483f8 | 9b4838df917f |
| S388 | vs_2_0 | 400 | 8 | add:5 dcl:2 mov:1 | g_offset_base@c0 g_offset_blured@c1 g_offset_depth@c2 g_offset_glare@c3 g_offset_previous@c4 | FN1PN0D | 0x6d6c78 | 611587456417 |
| S389 | vs_2_0 | 404 | 14 | dcl:3 mad:3 mov:3 mul:3 def:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x748578 | a30e646db433 |
| S390 | vs_2_0 | 404 | 14 | dcl:3 mad:3 mov:3 mul:3 def:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x749640 | 7e18a75d83d0 |
| S391 | vs_2_0 | 408 | 10 | mad:3 dcl:2 mul:2 def:1 dp4:1 mov:1 | g_depth_vector@c11 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x72fa80 | f5cddf247010 |
| S392 | vs_2_0 | 416 | 14 | mad:6 mul:3 dcl:2 def:1 dp3:1 rsq:1 | g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x7348d8 | 218b1385be85 |
| S393 | vs_2_0 | 436 | 16 | dcl:3 dp4:3 add:2 mad:2 mov:2 mul:2 frc:1 mova:1 | g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x6d0378 | acbeaafb24c4 |
| S394 | vs_2_0 | 456 | 17 | mad:6 mul:4 dcl:2 def:1 dp3:1 mov:1 rcp:1 rsq:1 | g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x74c140 | 5eb20c62db9f |
| S395 | vs_2_0 | 480 | 15 | mad:4 dcl:3 mov:2 mul:2 def:1 dp3:1 max:1 min:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x730fb0 | be3f92db15af |
| S396 | vs_2_0 | 488 | 15 | mad:6 mul:3 dcl:2 def:1 dp3:1 mov:1 rsq:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x747250 | a9ea1d6bcc23 |
| S397 | vs_2_0 | 492 | 16 | mad:6 mul:4 dcl:3 def:1 dp3:1 rsq:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x734a78 | 63b0b9a36ae2 |
| S398 | vs_2_0 | 496 | 16 | mad:6 mul:3 dcl:2 def:1 dp3:1 dp4:1 mov:1 rsq:1 | g_depth_vector@c11 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x735010 | b55befcaba00 |
| S399 | vs_2_0 | 500 | 17 | mad:5 dcl:2 dp3:2 mov:2 add:1 def:1 max:1 min:1 mul:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 | FN1PN0D | 0x731190 | 5060e436f35e |
| S400 | vs_2_0 | 516 | 16 | mad:8 mul:3 dcl:1 def:1 dp4:1 mov:1 rcp:1 | g_depth_vector@c11 g_shadow_tex_mat@c248 g_transform_mat@c0 | FN1PN0D | 0x72ffa0 | 28b5e820cc0d |
| S401 | vs_2_0 | 516 | 18 | mad:6 dcl:4 mul:4 def:1 dp3:1 mov:1 rsq:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x736170 | 8fad3f93c574 |
| S402 | vs_2_0 | 524 | 18 | mad:9 dcl:3 mul:3 add:1 def:1 mov:1 | g_ModelMatrix@c0 g_ProjMatrix@c8 g_ViewMatrix@c4 | FN1PN0D | 0x6cf100 | 641405230953 |
| S403 | vs_2_0 | 528 | 18 | mad:5 dcl:3 mul:3 mov:2 def:1 dp3:1 max:1 min:1 rcp:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x749d28 | 46b2315fc676 |
| S404 | vs_2_0 | 532 | 24 | dcl:5 def:4 dp3:4 mad:3 mov:3 add:2 frc:1 mul:1 sincos:1 | projection@c0 | FN1PN0D | 0x6cedb0 | cd428c74ece3 |
| S405 | vs_2_0 | 532 | 19 | mad:6 mul:5 dcl:3 def:1 dp3:1 mov:1 rcp:1 rsq:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74c308 | 44794056842a |
| S406 | vs_2_0 | 548 | 20 | mad:6 dcl:2 dp3:2 mov:2 mul:2 add:1 def:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 | FN1P | 0x749f38 | 0bcac019d9f3 |
| S407 | vs_2_0 | 556 | 21 | mad:6 mul:5 dcl:4 mov:2 def:1 dp3:1 rcp:1 rsq:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74d2b8 | d8f4a0907d34 |
| S408 | vs_2_0 | 564 | 22 | mad:4 mul:4 mov:3 add:2 dcl:2 else:2 endif:2 if:2 def:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x7301a8 | 36887119dd87 |
| S409 | vs_2_0 | 564 | 20 | mad:6 dcl:5 mul:4 dp3:3 def:1 mov:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x730ab0 | 8640f8ebcb68 |
| S410 | vs_2_0 | 564 | 22 | mad:4 mul:4 mov:3 add:2 dcl:2 else:2 endif:2 if:2 def:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | N0D | - | a06a8d9bf52a |
| S411 | vs_2_0 | 572 | 18 | mad:5 mov:3 dcl:2 dp3:2 add:1 def:1 max:1 min:1 mul:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_planar_shadow_color@c13 g_transform_mat@c0 | FN1PN0D | 0x734458 | 644c619d9f26 |
| S412 | vs_2_0 | 576 | 19 | mad:5 dcl:3 dp3:2 mov:2 mul:2 add:1 def:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x731388 | 499f8ad23c91 |
| S413 | vs_2_0 | 580 | 22 | mad:6 dp3:3 mov:3 add:2 dcl:2 mul:2 def:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 | FN1PN0D | 0x732c50 | cb0dad1b3760 |
| S414 | vs_2_0 | 580 | 19 | mad:6 dcl:3 mul:3 mov:2 def:1 dp3:1 max:1 min:1 nrm:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x7404f8 | 8cb5fee95208 |
| S415 | vs_2_0 | 588 | 24 | mad:4 mov:4 mul:4 dcl:3 add:2 else:2 endif:2 if:2 def:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x7303e0 | 5a748fc67637 |
| S416 | vs_2_0 | 588 | 24 | mad:4 mov:4 mul:4 dcl:3 add:2 else:2 endif:2 if:2 def:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | N0D | - | ab764beb022a |
| S417 | vs_2_0 | 604 | 21 | mad:5 dcl:3 mov:3 mul:3 dp3:2 add:1 def:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x732418 | 0d0a655bf846 |
| S418 | vs_2_0 | 604 | 21 | mad:6 dcl:4 mov:3 mul:3 def:1 dp3:1 max:1 min:1 nrm:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x741660 | e48965d5995f |
| S419 | vs_2_0 | 604 | 25 | mul:5 mad:4 mov:4 add:2 dcl:2 else:2 endif:2 if:2 def:1 rcp:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x748dc0 | 84401c5e7201 |
| S420 | vs_2_0 | 604 | 23 | mad:6 dcl:5 mul:5 dp3:3 mov:2 def:1 rcp:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x7497d8 | f0580716c9ac |
| S421 | vs_2_0 | 616 | 23 | mad:10 mul:5 dcl:2 def:2 dp3:1 mov:1 rcp:1 rsq:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x735200 | 43a782ccf10b |
| S422 | vs_2_0 | 620 | 22 | mad:6 mul:4 dcl:3 mov:3 def:1 dp3:1 max:1 min:1 nrm:1 rcp:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x757620 | bbc4a3339a4b |
| S423 | vs_2_0 | 624 | 22 | mad:6 dcl:3 mul:3 dp3:2 mov:2 add:1 def:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x74a160 | 2de7b1f42239 |
| S424 | vs_2_0 | 628 | 26 | def:4 dcl:3 dp4:3 mad:3 mov:3 add:2 dp3:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x6d0530 | a4445b81a7ac |
| S425 | vs_2_0 | 628 | 23 | mad:5 dcl:4 mov:4 mul:3 dp3:2 add:1 def:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x7329d8 | a7427db03aeb |
| S426 | vs_2_0 | 628 | 27 | mov:5 mul:5 mad:4 dcl:3 add:2 else:2 endif:2 if:2 def:1 rcp:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x749020 | 351b0d729ac0 |
| S427 | vs_2_0 | 628 | 25 | mad:7 dp3:3 mov:3 mul:3 add:2 dcl:2 def:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 | FN1P | 0x74bb88 | 26b95f1b4496 |
| S428 | vs_2_0 | 640 | 25 | mad:10 mul:6 dcl:4 mov:3 add:1 def:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x736628 | e2b1d005005d |
| S429 | vs_2_0 | 644 | 24 | mad:6 dcl:4 mov:4 mul:4 def:1 dp3:1 max:1 min:1 nrm:1 rcp:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x758420 | 673de5c188e4 |
| S430 | vs_2_0 | 652 | 27 | def:5 dp4:5 dcl:3 mad:3 mov:3 add:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x6d07a8 | 2cf5ce771ac8 |
| S431 | vs_2_0 | 652 | 27 | def:5 dp4:5 dcl:3 mad:3 mov:3 add:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x6d0a38 | 0465d9741da6 |
| S432 | vs_2_0 | 652 | 24 | mad:6 mul:4 dcl:3 mov:3 dp3:2 add:1 def:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x74b2d0 | 5e4c1079e9a3 |
| S433 | vs_2_0 | 664 | 27 | mad:10 mul:6 dcl:5 mov:4 add:1 def:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x7368a8 | ebae3709b350 |
| S434 | vs_2_0 | 676 | 25 | def:4 dcl:3 dp4:3 mad:3 add:2 dp3:2 mov:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x733450 | 69069cdb31b0 |
| S435 | vs_2_0 | 676 | 25 | mad:10 mul:3 dcl:2 dp3:2 mov:2 add:1 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x7418c0 | db15e99de3fb |
| S436 | vs_2_0 | 676 | 26 | mad:6 dcl:4 mov:4 mul:4 dp3:2 add:1 def:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x74b8e0 | 68c05aea6cde |
| S437 | vs_2_0 | 680 | 28 | mad:10 mul:7 dcl:4 mov:4 add:1 def:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74d4e8 | 72ae0fba02a1 |
| S438 | vs_2_0 | 688 | 25 | mad:10 mul:4 dcl:3 add:2 mov:2 nrm:2 def:1 dp3:1 | g_eye_pos@c8 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x736378 | ac318b799d8f |
| S439 | vs_2_0 | 688 | 30 | mad:5 add:4 mul:4 slt:4 dcl:3 frc:3 lrp:2 sge:2 abs:1 def:1 mova:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 | FN1PN0D | 0x7479f8 | aa5d5fec6d36 |
| S440 | vs_2_0 | 692 | 25 | mad:10 mul:6 dcl:3 def:2 dp3:1 mov:1 rcp:1 rsq:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x735468 | 606a014b7c02 |
| S441 | vs_2_0 | 700 | 26 | mad:9 mov:3 mul:3 dcl:2 def:2 dp3:2 add:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 | FN1PN0D | 0x7319a8 | 9ba96c46eb26 |
| S442 | vs_2_0 | 700 | 26 | def:5 dp4:5 dcl:3 mad:3 add:2 mov:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x7336f8 | 392813e51aa0 |
| S443 | vs_2_0 | 700 | 26 | def:5 dp4:5 dcl:3 mad:3 add:2 mov:2 defi:1 endloop:1 frc:1 loop:1 mova:1 mul:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x7339b8 | 290daacbead2 |
| S444 | vs_2_0 | 704 | 27 | def:5 dp4:5 dcl:4 mad:3 add:2 mul:2 defi:1 endloop:1 frc:1 loop:1 mov:1 mova:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 | FN1PN0D | 0x72f670 | e1d05b007220 |
| S445 | vs_2_0 | 712 | 28 | dp3:6 mad:6 dcl:5 mul:4 mov:3 add:2 def:1 nrm:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x730ce8 | c85c33adfb58 |
| S446 | vs_2_0 | 712 | 32 | mad:5 add:4 dcl:4 mul:4 slt:4 frc:3 lrp:2 sge:2 abs:1 def:1 mov:1 mova:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 | FN1PN0D | 0x747ca8 | 977b261ff204 |
| S447 | vs_2_0 | 716 | 28 | mad:10 mul:4 mov:3 dcl:2 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x7586a8 | a873003f502b |
| S448 | vs_2_0 | 732 | 28 | mad:10 mul:7 dcl:3 def:2 mov:2 rcp:2 dp3:1 rsq:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74c520 | 9e0275f4e3f4 |
| S449 | vs_2_0 | 740 | 34 | mad:5 mul:5 add:4 slt:4 dcl:3 frc:3 lrp:2 mov:2 sge:2 abs:1 def:1 mova:1 rcp:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 | FN1P | 0x75df50 | 2d5407ed840e |
| S450 | vs_2_0 | 748 | 29 | mad:10 mul:4 mov:3 dcl:2 def:2 dp3:2 rcp:2 add:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 | FN1P | 0x74a7d8 | 13e387dc52f5 |
| S451 | vs_2_0 | 752 | 31 | mad:7 mul:6 dcl:3 mov:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x735b90 | 110246346b9b |
| S452 | vs_2_0 | 752 | 27 | mad:10 mul:4 dcl:3 dp3:2 mov:2 add:1 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x741b68 | 18db68fd16ed |
| S453 | vs_2_0 | 752 | 31 | dp3:6 mad:6 dcl:5 mul:5 mov:4 add:2 def:1 nrm:1 rcp:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x749a38 | d398dc3a4dcd |
| S454 | vs_2_0 | 752 | 31 | mad:7 mul:6 dcl:3 mov:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | N0D | - | 9dbdd38c9e57 |
| S455 | vs_2_0 | 756 | 30 | mad:11 mul:4 dp3:3 mov:3 add:2 dcl:2 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x745358 | 21a59c523728 |
| S456 | vs_2_0 | 756 | 31 | def:5 dp4:5 dcl:4 mad:3 mov:3 mul:3 add:2 defi:1 endloop:1 frc:1 loop:1 mova:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 | FN1P | 0x748710 | 40fa34d26735 |
| S457 | vs_2_0 | 764 | 36 | mad:5 mul:5 add:4 dcl:4 slt:4 frc:3 mov:3 lrp:2 sge:2 abs:1 def:1 mova:1 rcp:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 | FN1P | 0x75e238 | a3a3b3d96ca0 |
| S458 | vs_2_0 | 776 | 28 | mad:9 mul:4 dcl:3 mov:3 def:2 dp3:2 add:1 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 | FN1PN0D | 0x731c68 | f3201f4e4254 |
| S459 | vs_2_0 | 776 | 29 | mad:10 dcl:4 mul:4 mov:3 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x744280 | 0edd537215eb |
| S460 | vs_2_0 | 780 | 31 | mad:10 mov:4 mul:4 dp3:3 add:2 dcl:2 def:2 max:1 min:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 | FN1PN0D | 0x732e98 | 7b9a80722783 |
| S461 | vs_2_0 | 780 | 28 | mad:10 mul:5 dcl:3 mov:3 def:2 dp3:1 max:1 min:1 nrm:1 rcp:1 | g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x740b40 | 7a3d2b46a68a |
| S462 | vs_2_0 | 780 | 29 | mad:10 mul:5 dcl:3 mov:3 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x744588 | cc9edbabf715 |
| S463 | vs_2_0 | 780 | 29 | mad:10 mul:4 dcl:3 mov:3 add:2 dp3:2 nrm:2 def:1 max:1 min:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x744c70 | e21fdb455865 |
| S464 | vs_2_0 | 780 | 33 | mad:7 mul:7 dcl:3 mov:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 rcp:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74cc98 | 93ecac265fb7 |
| S465 | vs_2_0 | 792 | 30 | mad:10 mul:5 dcl:3 mov:3 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x758978 | cd7cb9804dfe |
| S466 | vs_2_0 | 796 | 33 | mad:11 mul:5 mov:4 dp3:3 add:2 dcl:2 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x75c388 | 1f8148d7050c |
| S467 | vs_2_0 | 816 | 32 | mad:10 mul:5 dcl:4 mov:4 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75b1e8 | a0d2c81c64b2 |
| S468 | vs_2_0 | 820 | 31 | mad:10 mul:6 mov:4 dcl:3 def:2 rcp:2 dp3:1 max:1 min:1 nrm:1 | g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x757890 | e88fb17f66c3 |
| S469 | vs_2_0 | 820 | 32 | mad:10 mul:6 mov:4 dcl:3 dp3:2 add:1 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75b518 | 2c1fff86845a |
| S470 | vs_2_0 | 820 | 32 | mad:10 mul:5 mov:4 dcl:3 add:2 dp3:2 nrm:2 def:1 max:1 min:1 rcp:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75bc50 | e71a29fdefdd |
| S471 | vs_2_0 | 824 | 31 | mad:10 mul:5 dcl:3 mov:3 def:2 dp3:2 rcp:2 add:1 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x74aac8 | 543cb73b2ce4 |
| S472 | vs_2_0 | 828 | 34 | mad:11 mul:5 mov:4 dp3:3 add:2 dcl:2 def:2 rcp:2 max:1 min:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 | FN1P | 0x74be00 | 5f8dee212207 |
| S473 | vs_2_0 | 832 | 32 | mad:11 mul:5 dcl:3 dp3:3 mov:3 add:2 def:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x745650 | 7e2ee4331629 |
| S474 | vs_2_0 | 840 | 34 | mad:14 mul:8 dcl:4 mov:4 def:2 add:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x737e58 | 9979241d462b |
| S475 | vs_2_0 | 840 | 34 | mad:7 mov:5 mul:5 dcl:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 max:1 min:1 nrm:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x741318 | 49324e709e48 |
| S476 | vs_2_0 | 840 | 34 | mad:7 mov:5 mul:5 dcl:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 max:1 min:1 nrm:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | N0D | - | 35a577e46b11 |
| S477 | vs_2_0 | 852 | 38 | mad:8 mul:6 add:4 dcl:4 slt:4 frc:3 lrp:2 sge:2 abs:1 def:1 dp3:1 mova:1 rsq:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x747f70 | d8225e72b167 |
| S478 | vs_2_0 | 860 | 36 | mad:6 mov:6 mul:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x732678 | 557727e86995 |
| S479 | vs_2_0 | 860 | 33 | mad:14 mul:7 dcl:4 mov:3 add:2 def:1 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x739580 | 03b52781a425 |
| S480 | vs_2_0 | 860 | 36 | mad:6 mov:6 mul:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | N0D | - | 1e11fd5ff1b8 |
| S481 | vs_2_0 | 864 | 36 | mad:14 mul:8 dcl:5 mov:5 def:2 add:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x7381a0 | af676b9c01b3 |
| S482 | vs_2_0 | 868 | 36 | mad:7 mul:6 mov:5 dcl:3 add:2 def:2 else:2 endif:2 if:2 dp3:1 max:1 min:1 nrm:1 rcp:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x7580b8 | c48eb75c0da7 |
| S483 | vs_2_0 | 872 | 35 | mad:11 mul:6 mov:4 dcl:3 dp3:3 add:2 def:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75c6a8 | 4d70c272f7de |
| S484 | vs_2_0 | 876 | 34 | mad:14 mul:5 mov:3 dcl:2 def:2 dp3:2 add:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x742820 | d6c33b422f96 |
| S485 | vs_2_0 | 880 | 37 | mad:14 mul:9 mov:5 dcl:4 def:2 rcp:2 add:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74eb48 | 2bde33dca584 |
| S486 | vs_2_0 | 884 | 35 | mad:14 mul:7 dcl:5 mov:4 add:2 def:1 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73b4e8 | ee43d6446ebc |
| S487 | vs_2_0 | 888 | 35 | mad:14 mul:8 dcl:4 mov:4 add:2 def:1 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73bca0 | 80cc6cd0b9e3 |
| S488 | vs_2_0 | 892 | 41 | mad:8 mul:7 add:4 dcl:4 slt:4 frc:3 lrp:2 sge:2 abs:1 def:1 dp3:1 mov:1 mova:1 rcp:1 rsq:1 | g_ScoreBoardOffsets@c16 g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x75e538 | 0054196a1e97 |
| S489 | vs_2_0 | 896 | 38 | mad:7 mul:6 mov:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 rcp:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 | FN1P | 0x74b560 | 8be6a5a85dd1 |
| S490 | vs_2_0 | 900 | 36 | def:6 mad:6 dp4:5 dcl:4 mul:4 add:3 mov:2 defi:1 endloop:1 frc:1 loop:1 mova:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 | FN1PN0D | 0x72fc18 | d4095f6159db |
| S491 | vs_2_0 | 900 | 36 | mad:14 mul:8 dcl:4 mov:4 add:2 def:1 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x750338 | 550b1baccb4d |
| S492 | vs_2_0 | 904 | 39 | mad:14 mul:9 mov:6 dcl:5 def:2 rcp:2 add:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x74eeb8 | e82643a453a0 |
| S493 | vs_2_0 | 912 | 37 | mad:14 mul:8 dcl:5 mov:5 add:2 def:1 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73c018 | 7c6124636a78 |
| S494 | vs_2_0 | 912 | 37 | mad:14 mul:8 dcl:5 mov:5 add:2 def:1 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73fd10 | 1f4c1849a215 |
| S495 | vs_2_0 | 916 | 37 | mad:14 mul:6 mov:4 dcl:2 def:2 dp3:2 rcp:2 add:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x7596a8 | fb617a8b7606 |
| S496 | vs_2_0 | 924 | 38 | mad:14 mul:8 dcl:5 mov:5 add:2 def:1 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x752390 | e043f743e7dd |
| S497 | vs_2_0 | 928 | 38 | mad:14 mul:9 mov:5 dcl:4 add:2 def:1 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x752b98 | 151f13db12b2 |
| S498 | vs_2_0 | 936 | 39 | mad:7 dp3:6 dcl:5 def:4 mul:4 dp4:3 add:2 mov:2 defi:1 endloop:1 frc:1 loop:1 mova:1 rsq:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x734c68 | 4d37a5ae5c7f |
| S499 | vs_2_0 | 952 | 36 | mad:14 mul:6 dcl:3 mov:3 def:2 dp3:2 add:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x742b90 | e266f9e01045 |
| S500 | vs_2_0 | 952 | 40 | def:6 mad:6 dp4:5 mul:5 dcl:4 mov:4 add:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 | FN1P | 0x748a08 | 2f260195188f |
| S501 | vs_2_0 | 952 | 40 | mad:14 mul:9 mov:6 dcl:5 add:2 def:1 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x752f38 | c3d294ac8ce6 |
| S502 | vs_2_0 | 952 | 40 | mad:14 mul:9 mov:6 dcl:5 add:2 def:1 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x756de8 | 4c8910a9d9bc |
| S503 | vs_2_0 | 956 | 39 | mad:15 mul:6 mov:4 dp3:3 add:2 dcl:2 def:2 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_world_mat@c4 | FN1PN0D | 0x745990 | 433cc898cc2f |
| S504 | vs_2_0 | 980 | 38 | mad:14 mul:7 mov:4 dcl:3 def:2 dp3:2 add:1 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x744898 | 31c9ce0c06dc |
| S505 | vs_2_0 | 980 | 38 | mad:14 mul:6 mov:4 dcl:3 add:2 def:2 dp3:2 nrm:2 max:1 min:1 rcp:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x744f80 | edf31aacc2f2 |
| S506 | vs_2_0 | 980 | 40 | mad:7 def:6 dp4:5 dcl:4 dp3:4 mov:3 mul:3 add:2 defi:1 endloop:1 frc:1 loop:1 mova:1 rsq:1 | g_planar_shadow_color@c13 g_transform_mat@c0 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x747438 | f38bd9134471 |
| S507 | vs_2_0 | 984 | 39 | mad:12 mul:7 dp3:4 add:3 dcl:3 mov:3 max:2 min:2 def:1 nrm:1 rsq:1 | g_eye_pos@c8 g_fresnel_const@c12 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x7469d8 | 1ac7cebbd885 |
| S508 | vs_2_0 | 992 | 42 | def:6 mad:6 dp3:5 dp4:5 dcl:4 mov:4 add:3 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 mul:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x7315c8 | dcec67aed643 |
| S509 | vs_2_0 | 992 | 39 | mad:14 mul:7 mov:4 dcl:3 def:2 dp3:2 rcp:2 add:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x759a40 | 64077749781c |
| S510 | vs_2_0 | 996 | 42 | mad:15 mul:7 mov:5 dp3:3 add:2 dcl:2 def:2 rcp:2 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_world_mat@c4 | FN1P | 0x75ca10 | 8fd03fa02da1 |
| S511 | vs_2_0 | 1008 | 42 | mad:11 mul:6 mov:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x743aa0 | 6faaccb7766f |
| S512 | vs_2_0 | 1008 | 42 | mad:11 mul:6 mov:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | N0D | - | 3ef1e30ad099 |
| S513 | vs_2_0 | 1020 | 41 | mad:14 mul:8 mov:5 dcl:3 def:2 dp3:2 rcp:2 add:1 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75b850 | ad9c25818f89 |
| S514 | vs_2_0 | 1020 | 41 | mad:14 mul:7 mov:5 dcl:3 add:2 def:2 dp3:2 nrm:2 rcp:2 max:1 min:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75bf88 | 92a17e6b9229 |
| S515 | vs_2_0 | 1024 | 42 | mad:7 dp3:6 dcl:5 def:4 mov:4 dp4:3 mul:3 add:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 | g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x740740 | 7dd013dff4ae |
| S516 | vs_2_0 | 1024 | 42 | mad:12 mul:8 dp3:4 mov:4 add:3 dcl:3 max:2 min:2 def:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_fresnel_const@c12 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75d688 | 56ca4907e74e |
| S517 | vs_2_0 | 1032 | 41 | mad:15 mul:7 mov:4 dcl:3 dp3:3 add:2 def:2 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x745d50 | 03e97ee5a04f |
| S518 | vs_2_0 | 1032 | 45 | def:6 mad:6 dp3:5 dp4:5 mov:5 dcl:4 add:3 mul:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_vb_mat@c15 | FN1P | 0x74a3d0 | fe18ec9fb38d |
| S519 | vs_2_0 | 1036 | 44 | mad:11 mul:7 mov:5 add:3 dcl:3 def:2 dp3:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75a9c8 | 336a17cde053 |
| S520 | vs_2_0 | 1060 | 42 | mad:18 mul:9 dcl:4 mov:4 add:2 def:2 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73a410 | 9772bd8fbcb8 |
| S521 | vs_2_0 | 1072 | 44 | mad:15 mul:8 mov:5 dcl:3 dp3:3 add:2 def:2 rcp:2 max:1 min:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75cdf8 | b5e3560750ba |
| S522 | vs_2_0 | 1084 | 44 | mad:18 mul:9 dcl:5 mov:5 add:2 def:2 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73b860 | 298c1df85bf6 |
| S523 | vs_2_0 | 1088 | 44 | mad:18 mul:10 mov:5 dcl:4 add:2 def:2 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73da80 | 6a30c438b213 |
| S524 | vs_2_0 | 1088 | 47 | mad:12 mul:7 mov:6 add:4 dcl:3 dp3:3 def:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x746158 | 8b91d63c886e |
| S525 | vs_2_0 | 1088 | 47 | mad:12 mul:7 mov:6 add:4 dcl:3 dp3:3 def:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | N0D | - | eb7403309622 |
| S526 | vs_2_0 | 1100 | 45 | mad:18 mul:10 mov:5 dcl:4 add:2 def:2 rcp:2 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x751240 | be35094063fb |
| S527 | vs_2_0 | 1112 | 46 | mad:18 mul:10 mov:6 dcl:5 add:2 def:2 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x73dec0 | 293f041f0603 |
| S528 | vs_2_0 | 1112 | 46 | mad:18 mul:10 mov:6 dcl:5 add:2 def:2 dp3:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x7400a0 | c70f5a2fac71 |
| S529 | vs_2_0 | 1116 | 49 | mad:12 mul:8 mov:6 add:4 dcl:3 dp3:3 def:2 else:2 endif:2 if:2 max:1 min:1 nrm:1 rcp:1 rsq:1 | g_ScreenTwo@b1 g_SplitScreen@b0 g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75d228 | 6dcd4f0fed19 |
| S530 | vs_2_0 | 1124 | 47 | mad:18 mul:10 mov:6 dcl:5 add:2 def:2 rcp:2 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x752730 | 4f3d963e8330 |
| S531 | vs_2_0 | 1128 | 47 | mad:18 mul:11 mov:6 dcl:4 add:2 def:2 rcp:2 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x754a68 | 6226825b789b |
| S532 | vs_2_0 | 1132 | 48 | mad:10 dp3:6 mul:6 dcl:5 def:5 add:3 dp4:3 mov:3 defi:1 endloop:1 frc:1 loop:1 mova:1 rcp:1 rsq:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x735720 | eb456e951a2a |
| S533 | vs_2_0 | 1152 | 49 | mad:18 mul:11 mov:7 dcl:5 add:2 def:2 rcp:2 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x754ed0 | e36f1772c372 |
| S534 | vs_2_0 | 1152 | 49 | mad:18 mul:11 mov:7 dcl:5 add:2 def:2 rcp:2 dp3:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x7571a0 | 3c7765d436bf |
| S535 | vs_2_0 | 1172 | 51 | mad:10 mul:7 dp3:6 dcl:5 def:5 mov:4 add:3 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 rsq:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74c800 | d7a036f2f96d |
| S536 | vs_2_0 | 1184 | 48 | mad:16 mul:9 dp3:4 mov:4 add:3 dcl:3 def:2 max:2 min:2 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_fresnel_const@c12 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1PN0D | 0x746db0 | 7a47a0ac0a98 |
| S537 | vs_2_0 | 1188 | 51 | mad:9 def:7 dp3:5 dp4:5 mov:5 add:4 dcl:4 mul:3 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_vb_mat@c15 | FN1PN0D | 0x731f70 | fc89ddd28ba1 |
| S538 | vs_2_0 | 1188 | 54 | mad:12 dp3:8 dcl:6 mov:6 mul:6 def:4 add:3 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x736b40 | dbd9dee02841 |
| S539 | vs_2_0 | 1212 | 56 | mad:12 dp3:8 dcl:7 mov:7 mul:6 def:4 add:3 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x736fe8 | af71c37a0ee7 |
| S540 | vs_2_0 | 1220 | 51 | mad:10 dp3:6 dcl:5 def:5 mov:5 mul:5 add:3 dp4:3 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rcp:1 | g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x740e50 | 9a5beb78a098 |
| S541 | vs_2_0 | 1224 | 51 | mad:16 mul:10 mov:5 dp3:4 add:3 dcl:3 def:2 max:2 min:2 rcp:2 nrm:1 rsq:1 | g_eye_pos@c8 g_fresnel_const@c12 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_world_mat@c4 | FN1P | 0x75da88 | a4d5aaeb2367 |
| S542 | vs_2_0 | 1228 | 54 | mad:9 def:7 mov:6 dp3:5 dp4:5 add:4 dcl:4 mul:4 rcp:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_vb_mat@c15 | FN1P | 0x74ae00 | 7a1166105651 |
| S543 | vs_2_0 | 1228 | 57 | mad:12 dp3:8 mov:7 mul:7 dcl:6 def:4 add:3 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74d790 | 58419cb90e82 |
| S544 | vs_2_0 | 1236 | 56 | mad:12 dcl:6 def:6 dp3:6 mov:6 mul:6 dp4:5 add:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x7374a8 | edeb3639b547 |
| S545 | vs_2_0 | 1236 | 56 | mad:12 dcl:6 def:6 dp3:6 mov:6 mul:6 dp4:5 add:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x737980 | 06fcff45d510 |
| S546 | vs_2_0 | 1240 | 52 | mad:10 def:6 dcl:5 dp3:5 dp4:5 add:4 mov:4 mul:4 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x741e58 | d314c6ded9df |
| S547 | vs_2_0 | 1252 | 59 | mad:12 dp3:8 mov:8 dcl:7 mul:7 def:4 add:3 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74dc60 | d922667db6ed |
| S548 | vs_2_0 | 1260 | 54 | mad:10 dp3:6 mov:6 mul:6 dcl:5 def:5 add:3 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 | g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x757bc8 | 84036d7787a6 |
| S549 | vs_2_0 | 1264 | 54 | mad:10 dcl:6 def:6 dp3:5 dp4:5 mov:5 add:4 mul:4 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x742330 | 35bba6654020 |
| S550 | vs_2_0 | 1276 | 59 | mad:12 mov:7 mul:7 dcl:6 def:6 dp3:6 dp4:5 add:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74e148 | 246bb9f8de61 |
| S551 | vs_2_0 | 1276 | 59 | mad:12 mov:7 mul:7 dcl:6 def:6 dp3:6 dp4:5 add:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74e648 | dca13ea8d764 |
| S552 | vs_2_0 | 1280 | 55 | mad:10 def:6 dcl:5 dp3:5 dp4:5 mov:5 mul:5 add:4 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x758c90 | 1e786e64b04b |
| S553 | vs_2_0 | 1304 | 57 | mad:10 dcl:6 def:6 mov:6 dp3:5 dp4:5 mul:5 add:4 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x759190 | 0e6a7a0f0c07 |
| S554 | vs_2_0 | 1384 | 63 | mad:15 dp3:8 mul:8 mov:7 dcl:6 def:5 add:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x738500 | 3d47dde936a3 |
| S555 | vs_2_0 | 1404 | 62 | mad:15 dp3:9 mul:7 dcl:6 mov:6 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x7398e0 | 294701125bb0 |
| S556 | vs_2_0 | 1408 | 65 | mad:15 dp3:8 mov:8 mul:8 dcl:7 def:5 add:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x738a68 | d5e599be6b44 |
| S557 | vs_2_0 | 1424 | 66 | mad:15 mul:9 dp3:8 mov:8 dcl:6 def:5 add:4 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74f240 | 486c42301cff |
| S558 | vs_2_0 | 1432 | 65 | mad:15 mul:8 def:7 mov:7 dcl:6 dp3:6 dp4:5 add:4 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x738fe8 | b1e7825bfb28 |
| S559 | vs_2_0 | 1432 | 64 | mad:15 dp3:9 mul:8 mov:7 dcl:6 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73c3a8 | 0fbc3b6052de |
| S560 | vs_2_0 | 1436 | 61 | mad:13 def:7 mul:6 add:5 dcl:5 dp3:5 dp4:5 mov:5 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x742f48 | 082e05f3764f |
| S561 | vs_2_0 | 1444 | 65 | mad:15 dp3:9 mul:8 mov:7 dcl:6 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x7506c0 | a90fb1487215 |
| S562 | vs_2_0 | 1448 | 68 | mad:15 mov:9 mul:9 dp3:8 dcl:7 def:5 add:4 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74f7d0 | eab6019df336 |
| S563 | vs_2_0 | 1452 | 64 | mad:15 dp3:7 mul:7 dcl:6 def:6 mov:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x739e60 | 73d42ebd54e5 |
| S564 | vs_2_0 | 1456 | 66 | mad:15 dp3:9 mov:8 mul:8 dcl:7 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73c940 | 248eba1d1395 |
| S565 | vs_2_0 | 1460 | 63 | mad:13 def:7 dcl:6 mov:6 mul:6 add:5 dp3:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x7434e8 | f9cfb048f949 |
| S566 | vs_2_0 | 1472 | 68 | mad:15 mul:9 mov:8 def:7 dcl:6 dp3:6 dp4:5 add:4 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 | g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x74fd78 | 8c2e4c58166f |
| S567 | vs_2_0 | 1472 | 67 | mad:15 dp3:9 mul:9 mov:8 dcl:6 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x7532f0 | 4148265cd317 |
| S568 | vs_2_0 | 1476 | 64 | mad:13 def:7 mul:7 mov:6 add:5 dcl:5 dp3:5 dp4:5 rcp:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x759e20 | 0a2b2bd5ff32 |
| S569 | vs_2_0 | 1480 | 66 | mad:15 mul:8 dp3:7 mov:7 dcl:6 def:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73cef0 | 8dab38c4e5b8 |
| S570 | vs_2_0 | 1480 | 66 | mad:15 mul:8 dp3:7 mov:7 dcl:6 def:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73d4b8 | 727b411d8b48 |
| S571 | vs_2_0 | 1492 | 67 | mad:15 mul:8 dp3:7 mov:7 dcl:6 def:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x750c68 | 02e6b8f125ce |
| S572 | vs_2_0 | 1496 | 69 | mad:15 dp3:9 mov:9 mul:9 dcl:7 add:5 def:4 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x7538b0 | ad646a821aa9 |
| S573 | vs_2_0 | 1500 | 66 | mad:13 def:7 mov:7 mul:7 dcl:6 add:5 dp3:5 dp4:5 rcp:2 defi:1 endloop:1 frc:1 loop:1 max:1 min:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x75a3e8 | 7055e1143847 |
| S574 | vs_2_0 | 1520 | 69 | mad:15 mul:9 mov:8 dp3:7 dcl:6 def:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x753e88 | 4296ff0c29e3 |
| S575 | vs_2_0 | 1520 | 69 | mad:15 mul:9 mov:8 dp3:7 dcl:6 def:6 add:5 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x754478 | 889aa5f5c935 |
| S576 | vs_2_0 | 1600 | 71 | mad:18 dp3:9 mul:9 mov:7 add:6 dcl:6 def:5 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73a838 | 909791c85eb3 |
| S577 | vs_2_0 | 1628 | 73 | mad:18 mul:10 dp3:9 mov:8 add:6 dcl:6 def:5 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73e318 | 067807a8babe |
| S578 | vs_2_0 | 1640 | 74 | mad:18 mul:10 dp3:9 mov:8 add:6 dcl:6 def:5 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x751690 | 37aeec365ee5 |
| S579 | vs_2_0 | 1648 | 73 | mad:18 mul:9 def:7 dp3:7 mov:7 add:6 dcl:6 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73ae78 | f9a0d0b328f9 |
| S580 | vs_2_0 | 1652 | 75 | mad:18 mul:10 dp3:9 mov:9 dcl:7 add:6 def:5 dp4:3 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73e978 | 301cc643ac6c |
| S581 | vs_2_0 | 1668 | 76 | mad:18 mul:11 dp3:9 mov:9 add:6 dcl:6 def:5 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x755350 | a12241030e31 |
| S582 | vs_2_0 | 1676 | 75 | mad:18 mul:10 mov:8 def:7 dp3:7 add:6 dcl:6 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73eff0 | 40a252aee549 |
| S583 | vs_2_0 | 1676 | 75 | mad:18 mul:10 mov:8 def:7 dp3:7 add:6 dcl:6 dp4:5 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rcp:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1PN0D | 0x73f680 | 59cce7bd79a3 |
| S584 | vs_2_0 | 1688 | 76 | mad:18 mul:10 mov:8 def:7 dp3:7 add:6 dcl:6 dp4:5 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x751cf8 | 82b62d792473 |
| S585 | vs_2_0 | 1692 | 78 | mad:18 mul:11 mov:10 dp3:9 dcl:7 add:6 def:5 dp4:3 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x7559d8 | 64cb09ad3320 |
| S586 | vs_2_0 | 1716 | 78 | mad:18 mul:11 mov:9 def:7 dp3:7 add:6 dcl:6 dp4:5 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x756078 | ae695fb4dec3 |
| S587 | vs_2_0 | 1716 | 78 | mad:18 mul:11 mov:9 def:7 dp3:7 add:6 dcl:6 dp4:5 rcp:2 defi:1 endloop:1 frc:1 loop:1 mova:1 nrm:1 rsq:1 | g_eye_pos@c8 g_light_vec@c10 g_shadow_tex_mat@c248 g_transform_mat@c0 g_uv0_scale@c14 g_vb_mat@c15 g_world_mat@c4 | FN1P | 0x756730 | fe29e60ef02a |

## Open items

- Explain the 15 blobs present in the SafeDisc disc copy but absent from 1.0.1. Not investigated.
- Confirm the constant meanings with a renderer or by stepping through a draw call.
- Check whether the stride-32 extra bytes are `blendweight`/`blendindices` on a skinned mesh.
- Decide the render path for the post-processing shaders (`g_samp_*` blur, glare, DOF) before porting.
