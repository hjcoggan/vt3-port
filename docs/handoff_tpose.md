# Handoff: Adreno skinning T-pose — RESOLVED

## Status: fixed
**Root cause:** Adreno 740 reads the skinning shader's **blend-indices vertex element**
(`R8G8B8A8_USCALED`, a UBYTE4) as zero. Every skinned shader computes its bone index with
`mova a0.x, r0.w` from `dot(blendIndices, decodeTable)`, so a zero index collapses the relative
constant read `vsc[15 + a0.x]` to bone 0 and the whole mesh renders as a rigid bind/T-pose.

**Fix:** `runtime/shim/src/d3d9_draw.c` now expands the four index bytes into a `float4` in the
shim's per-draw vertex copy (stride + 12, later attribute offsets + 12). The shader's
`layout(location = 2) in vec4` then receives the real 0..255 bone indices on every driver.

**Verified on Retroid Pocket Nova (Adreno 740):** players, sideline figures and cloth all animate;
the bind/T-pose is gone. The bytes were always correct — the CPU `skin_check` verifier read them —
so no game data changed.

## How it was found (abridged)
- Palette data and decode table were correct on-device, and the GPU did read the palette (a
  palette shift moved the mesh).
- `VT3_DBG_PALETTE=bone0` (shift only bone 0) moved the *whole* body off-screen → the GPU's
  effective bone index was 0 for every vertex.
- `VT3_DBG_INDEX=50` + the bone-0 shift still moved the whole body → even forcing the index bytes
  did not reach the shader.
- `VT3_DBG_A0=raw` (`a0 = int(vi5[aL.x])`) + bone-0 shift still moved it → the blend-indices
  attribute itself read as zero on the GPU.

(The temporary `VT3_DBG_*` / `VT3_FIX_LOOPREL` translator and shim probes were removed after the
fix; only the float4 expansion remains.)

---

# (original in-progress notes below)

## Status
The game runs; the T-pose is **not fixed**. Root cause narrowed but not confirmed.
This handoff captures every experiment done and the exact working-tree/device state so the next
session (ideally a model that can actually *see* the Retroid screenshots — this one cannot read
images) can continue without redoing the archaeology.

## The one thing to know first
This session's model **cannot view images**. Every "screenshot says X" below was inferred from the
user's verbal descriptions plus draw/palette logs, not from reading pixels. That is why the
conclusions are marked *inferred* and some experiments were inconclusive. A multimodal model will
resolve them in minutes.

## Background (read these)
- `docs/HANDOFF_BEAM.md` — original device evidence, build/deploy commands, device facts.
- `docs/handoff_muse.md` — the failed 256-case-switch experiment, already reverted; the restore
  recipe that works.

## What we know so far

### 1. The palette path is live on the GPU
`VT3_DBG_PALETTE=shift` (add +300 to the x-translation `.w` of every palette row 15,18,21,…)
moved the skinned body off-court while the clothes (a different shader) stayed. So Adreno does
read the uploaded `vsc[15 + a0.x]` rows. The UBO data is correct:
- `c195..c198` decode table is `(1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)` — unit selectors (logged).
- CPU `skin_check` reports animated, in-range palette (`max index ~135-171`, `zero bones 0`).

### 2. The T-pose is specific to the 4-bone skinning shaders
All skinned VS use the same skeleton: `defi i0, N` loop count, `mova a0.x, r0.w`, decode
`c195+aL`, palette `c15/16/17 + a0`. The only structural difference is the loop count:

| vs hash | loop (defi i0) | verdict (inferred) |
| --- | --- | --- |
| `98f59399` | 4 | **T-pose** |
| `c83a01a9` | 4 | **T-pose** |
| `d8b1ba5f` | 4 | **T-pose** |
| `d21892bb` | 4 | **T-pose** |
| `2e3a60a2` | 2 | works |
| `315a2b56` | 3 | works |
| `69b96662` | 3 | works |
| `2594fefc` | 3 | works |
| `38fae929` | 2 | works |
| `78147e1c` | 3 | works |

`VT3_SKIP_MESH` mapping (wildcard `:0` = any prim count):
- Skipping the 2/3-bone set removed the ball-boys and the 1P's *body* (clothes remain).
- Skipping the 4-bone set removed the 1P's torso/upper body (opponent, a 2/3-bone mesh, stayed).

So 4-bone meshes (torso/body of the human player, and other 4-bone characters) render bind/T-pose.

### 3. Constant-indexing the decode read did NOT fix it
`VT3_FIX_LOOPREL=1` rewrites `vsc[195 + aL.x]` to a constant-indexed ternary
`(aL.x==0 ? vsc[195] : aL.x==1 ? vsc[196] : …)`. The 4-bone body still T-posed. This means the
failure is **not** (only) the decode-table dynamic read.

## Remaining hypotheses (in order of likelihood)
1. **Adreno mis-executes the *palette* dynamic read `vsc[15 + a0.x]` where `a0.x` is a
   non-uniform per-vertex integer.** The decode read is fixed, but `a0` still feeds a dynamic
   UBO index. Two sub-cases to distinguish:
   - (a) dynamic index ignored → always reads `vsc[15]` (bone 0) → exact bind pose.
   - (b) dynamic index miscompiled → garbage (would be a mangled mesh, not T-pose; less likely).
   The "T-pose is bone 0" theory fits: `c15..c17` (bone 0) is an animated 3x4 matrix (logged:
   `0.996 0.0616 0.0605 -0.676 …`), so a rigid transform of the bind mesh by bone 0 = T-pose shape.
2. **A register-pressure/codegen issue in the 4-iteration loop** (4-bone shaders use more live
   temps across the dynamic index than 2/3-bone). Would explain the 4-vs-2/3 split.

## Debug knobs currently in the (uncommitted) tree

`runtime/shim/src/d3d9_draw.c`:
- `VT3_DBG_PALETTE=shift|zero|identity|bone0|spread` — rewrites palette rows before upload
  (rows 15..194).
- `VT3_DBG_INDEX=N` — forces BLENDINDICES in the vertex copy to N.
- `VT3_SKIP_MESH=hash:prims[,…]` — `prims 0` is now a wildcard (was exact-match only).
- One-shot logs of `c195..c198`, `c15..c17`.

`runtime/src/render/sm2_glsl.c`:
- `VT3_DBG_A0=N|raw` — forces `mova` to write `a0 = N`, or (raw) `a0 = int(vi5[aL.x])`.
  NOTE: the original `raw` used `vi5.x` only (per-iteration component bug); it was never
  re-tested with `vi5[aL.x]`. Worth retrying.
- `VT3_FIX_LOOPREL=1` — decode read becomes a constant-indexed select (see finding 3).
- Added `ctx.blend_idx` (which v# is `dcl_blendindices`) for the raw mode.

These change the generated GLSL, so **the pack and the device build must be regenerated with the
same env vars** (the pack is keyed by the hash of the generated text; device and pack must agree).

## Decisive next experiments (pick the fastest)

**A. Prove hypothesis 1 with the `spread` palette test (already coded, needs correct reading).**
`VT3_DBG_PALETTE=spread` gives each bone a distinct x offset (`+20*k`). If the 4-bone body smears
across the court, the palette index IS honoured and hypothesis 1 is wrong. If it stays T-posed at
one spot, the driver ignores `a0.x` → hypothesis 1a confirmed. (This run was already done but the
result could not be read by this model.)

**B. Fix `raw` mode and re-test.** `VT3_DBG_A0=raw` with `a0.x = int(vi5[aL.x])` bypasses the
decode table entirely (valid here because the table is unit selectors). If the 4-bone body then
animates, the decode dynamic read is the problem after all; if it T-poses, the palette dynamic
read is. (The current raw-mode code still has the `.x`-only bug.)

**C. If 1a confirmed: eliminate dynamic UBO indexing for the palette too.**
Options, least-to-most invasive:
  1. Constant-index the palette read per *bone count* — but bone indices are per-vertex and
     arbitrary, so a switch over the used window (max index ~171) is needed → that was Muse's
     256-case switch, which hangs Adreno's compiler. A *smaller* switch over only the actual
     index window (0..~180) may compile; measure `vkCreateGraphicsPipelines` time first.
  2. Move the constant bank to a **`samplerBuffer` / `texelFetch`** (dynamic texture fetch is
     safe on Adreno) — the principled fix; needs `vk.c` descriptor/binding + translator changes.
  3. CPU-skin the affected draws in the shim (bulletproof, but must also reproduce the shader's
     normal/tangent/shadow-UV/variance outputs, so it is the most work).

## How to reproduce / build (unchanged from handoff_muse.md)
Pack regeneration is env-sensitive while the debug knobs exist. Clean pack:
```
"/Users/heath/Virtua Tennis 3/tools/venv/bin/python" tools/formats/shaders/gen_spirv_pack.py ../dump/android/shaders.pack
```
(with no `VT3_*` set → md5 `fd742109e9caa519ce777fee09d25173`).
Build + deploy: see `docs/handoff_muse.md` (absolute `VT3_GAME_DIR`, NDK `llvm-strip`, F1 quick-play,
`adb shell am force-stop` first). Device `c27c2f60`.

## Current state of things (do not trust blindly)
- Working tree has the temporary debug changes above plus the intentional `vt3.env` reader in
  `android_entry.c`. None are committed.
- Device currently runs the **`VT3_DBG_PALETTE=spread`** build; `vt3.env` on device has
  `VT3_DBG_PALETTE=spread`, `VT3_SKIN_CHECK=1`, `VT3_TRACE_HASH=98f59399`.
- Pack on device/`../dump/android/shaders.pack` is the clean `fd7421…` (spread is shim-only).

## Commits / identity
- Nothing committed. Author/committer: `hjcoggan <191880570+hjcoggan@users.noreply.github.com>`.
- End commits with a `Co-Authored-By` trailer naming the session/model.
- Never commit game data (lives in `../dump`). Push to `https://github.com/hjcoggan/vt3-port`.
