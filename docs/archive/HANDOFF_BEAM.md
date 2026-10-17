# Handoff: Android (Retroid) T-pose investigation

## Problem
Mac build (`/Users/heath/Virtua Tennis 3/dump`, Mach-O arm64, Vulkan default) renders players
normally. The Android (Retroid Pocket Nova, Adreno 740, Vulkan) build renders every skinned
mesh (players, sideline figures) in a bind/T-pose; clothes stay attached to the body, the
racket ends up on the ground, and the 1P player's back has a texture smudge.

## Decisive experiment already done
Added a device-side `vt3.env` env-file reader in `runtime/src/platform/android_entry.c` so
debug switches can be set without a shell. Set `VT3_SKIN_CHECK=4`, `VT3_TRACE_HASH=98f59399`
(the 4-bone skinned body VS), `VT3_TRACE_VSC=1`, `VT3_TRACE_LIGHT=1`.

The resulting `skin check` log on the Retroid shows the CPU-side palette is animated and
correct:
- `weight sum 1.000..1.000`, `max index 135-171`, `out of range 0`, `zero bones 0`
- `v0 pos -0.209 17.44 1.47 w 1 0 0 0 idx 141 -> skinned -0.047 16.18 2.60 ...`
- `world x -1.71..0.90 y 9.35..16.61 z 0.11..2.67` (a real pose, not bind space)

So the shim uploads a real, animated bone palette and the CPU skinning reproduces it, but
the GPU (Adreno 740) renders the bind/T-pose. The SPIR-V is identical to the one that runs
correctly on Mac Vulkan (MoltenVK) — same pack, same hash (`shaders.pack` md5
`fd742109e9caa519ce777fee09d25173` on device matches both local packs).

## Root cause hypothesis
Adreno 740 mis-executes the skinned VS's **dynamic UBO indexing**. The only shaders that
dynamically index the constant buffer are the skinning ones, and only they are broken.
The skinning loop reads:
- `vsc[195 + aL.x]` (decode table, aL.x = 0..3) to get the bone index,
- then `vsc[15 + a0.x]`, `vsc[16 + a0.x]`, `vsc[17 + a0.x]` (palette rows).
`a0.x = int(round(dot(vi5, vsc[195 + aL.x])))`. All other draws read fixed `vsc[k]`
indices and render correctly. Both `vsc[195+aL.x]` and `vsc[15+a0.x]` are `OpAccessChain
%_ptr_Uniform_v4float %_ %int_0 %dynamic` in the SPIR-V, which validates clean and runs on
MoltenVK.

This is the same class of Adreno issue as the `dce147b` temp-zeroing fix (undefined
temporaries), but that fix did not resolve it: temps are zeroed, yet the T-pose persists.

## Relevant code
- `runtime/src/render/sm2_glsl.c`: SM2->GLSL 450 translator. The skinning VS
  `98f59399` (4 bones) / `2e3a60a2` (2 bones) / `3ee6cb60` (cloth) are generated from
  `../dump/runtime/shaders/bin/59cce7bd...vs_2_0.bin` etc. `a0`/`aL` are file-scope
  `ivec4` globals; temps `r0..r6` are file-scope `vec4`.
- `runtime/src/render/vk.c`: Vulkan backend; loads SPIR-V from `shaders.pack` by
  `fnv1a64` of generated GLSL text (`pack_load`, `module_for_text`). UBO block is
  `vec4 vsc[256]; ivec4 vsi[16]; ivec4 vsb4[4]` (4416 bytes), uploaded with
  `memcpy(vsb, d->vsc, VT3_CONST_REGS * 16u)` and `VK_WHOLE_SIZE` descriptor range.
- `runtime/shim/src/d3d9_draw.c`: `skin_check` (CPU skinning verifier), layout derivation,
  def-constant copy into `vsc_copy` at lines 930-951 (so `vsc[195..201]` decode table is
  present in the UBO).
- `runtime/src/platform/android_entry.c`: now reads `vt3.env` (KEY=VALUE) so debug switches
  reach the device.
- `tools/formats/shaders/gen_spirv_pack.py`: rebuilds `vt3sm2` from sm2tool.c + sm2_glsl.c,
  then compiles every shader with glslangValidator to SPIR-V and packs by hash.

## Key shaders
- skinned body VS `98f59399` (4 bones), 2-bone VS `2e3a60a2`, body PS `c62249b4`,
  cloth VS `3ee6cb60`. Hash = FNV-1a over the shader's `ntok*4` bytecode bytes.
  Disassemble with `../dump/android/tools/vt3sm2 IN.bin OUT.glsl`.

## Device facts
- serial `c27c2f60`, adb at `/opt/homebrew/bin/adb`.
- Adreno (TM) 740, OpenGL ES 3.2, Vulkan driver `adreno`, Android 16 / SDK 36.
- App `net.vt3port.game`, versionName 0.1, installed 2026-10-17 11:57.
- Build dir `/Users/heath/Virtua Tennis 3/dump/android/cmake-arm64-main` (source
  `/Users/heath/Virtua Tennis 3/vt3-port/runtime`).

## Build / deploy (from repo root)
```
ANDROID_NDK=/opt/homebrew/share/android-ndk cmake -S runtime -B ../dump/android/cmake-arm64-main \
  -DCMAKE_TOOLCHAIN_FILE=/opt/homebrew/share/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_PLATFORM=android-26 -DANDROID_ABI=arm64-v8a -DVT3_GAME_DIR=../dump/recomp/vt3_nocd -G Ninja
cmake --build ../dump/android/cmake-arm64-main --target vt3android -j 8
llvm-strip -o ../dump/android/libvt3android-stripped.so ../dump/android/cmake-arm64-main/libvt3android.so
$PY android/build_apk.py game vt3android --so ../dump/android/libvt3android-stripped.so --install c27c2f60
adb -s c27c2f60 push ../dump/android/shaders.pack /sdcard/Android/data/net.vt3port.game/files/shaders.pack
adb -s c27c2f60 push ../dump/android/vt3.env /sdcard/Android/data/net.vt3port.game/files/vt3.env
adb -s c27c2f60 shell monkey -p net.vt3port.game -c android.intent.category.LAUNCHER 1
```
Regenerate the pack after ANY change to `sm2_glsl.c`:
```
$PY tools/formats/shaders/gen_spirv_pack.py ../dump/android/shaders.pack
```
Mac match run: `VT3_SHADER_PACK=../dump/android/shaders.pack PLAY_POINTS=1 tools/recomp/run_match.sh <BIN> <OUTDIR> SECS SHOT_TIMES...`
Build Mac: `"/Users/heath/Virtua Tennis 3/tools/venv/bin/python" tools/recomp/build_native.py ../dump/recomp/vt3_x`

## What to try next
1. Test whether making `a0`/`aL` function-scope (instead of file-scope globals) in the
   generated GLSL changes Adreno's behaviour. Small translator change in `emit_header` /
   `translate_walk`: declare them inside `main()` rather than at file scope. Rebuild pack,
   push, screenshot + logcat.
2. If no, try forcing the palette read through a non-`OpAccessChain` path: e.g. build a
   `switch (a0.x)` over the possible register offsets (0,3,6,... up to max index) selecting
   `vsc[15 + k]` with a *constant* index. Adreno handles constant-indexed UBO reads; the
   failure is on dynamic ones.
3. If no, try making the bone palette a `samplerBuffer`/storage buffer read with a dynamic
   index, or pre-transforming vertices on the CPU in the shim (heavy, but bulletproof).
4. Keep the `vt3.env` mechanism — it is the only way to reach debug switches on device.

## Ruled out
- Pack mismatch (md5 identical, no "no SPIR-V" errors in logcat).
- CPU-side palette (skin_check shows animated, nonzero, in-range).
- Compiler flags (both -ffp-contract=off).
- Temp zeroing alone (dce147b) — still T-poses.
- FVF/vertex-declaration shim changes (platform-independent; Mac works with them).

## Commits / identity
- Author/committer: `hjcoggan <191880570+hjcoggan@users.noreply.github.com>`.
- End commits with a `Co-Authored-By` trailer naming the session/model.
- Never commit game data (lives in `../dump`).
- Run one game at a time (`pgrep -x vt3_native`).
- Push to `https://github.com/hjcoggan/vt3-port`.