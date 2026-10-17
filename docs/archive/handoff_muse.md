# Handoff: Adreno T-pose fix attempt — 256-case switch reverted (game restored)

**Status:** Muse's translator experiment is reverted. The Retroid build boots and renders the menu
again; the Adreno skinning T-pose is still unfixed. Read this together with `docs/HANDOFF_BEAM.md`
(background and the original device evidence).

## What happened

Muse tried to remove dynamic UBO indexing from the skinned vertex shaders by replacing
`vsc[base + a0.x]` with `vt3_vsc_dyn(base + a0.x)`, a helper emitted as a **256-case switch with
constant-indexed returns** (`sm2_glsl.c`: `ctx.uses_rel`, `reg_base()`, `emit_header()`).

It translated cleanly and the generated GLSL had no dynamic `vsc[]` reads, but on the Retroid
(Adreno 740) the game hung at the **first skinned draw** (`skin check 1: vs 98f59399`): no further
log output, white screen, then `ANR ... Input dispatching timed out`. There were no validation,
SPIR-V, or Adreno errors. The 256-case switch expanded to ~38 KB of SPIR-V for that VS, and
`vkCreateGraphicsPipelines` apparently never returned. This is a driver compiler pathology, not
invalid SPIR-V — but it means the approach is unusable as-is.

The experiment also never got far enough to show whether it fixed the T-pose, so it is **not**
evidence for or against the dynamic-UBO-index hypothesis in `HANDOFF_BEAM.md`.

## Current state (verified)

- `runtime/src/render/sm2_glsl.c` is back to `HEAD` (`10cde08`). No translator changes remain.
- `runtime/src/platform/android_entry.c` keeps the `vt3.env` reader from the Beam session. That is
  intentional — it is the only way to set `VT3_*` switches on the device.
- `shaders.pack` regenerated from the reverted translator:
  `569 shaders, 0 failed`, md5 **`fd742109e9caa519ce777fee09d25173`** (the known-good pack).
- `libvt3android-stripped.so` rebuilt and the APK reinstalled on `c27c2f60`; new pack and
  `vt3.env` pushed.
- Launched with `vt3.env` still holding `VT3_SKIN_CHECK=4`, `VT3_TRACE_HASH=98f59399`: menu
  renders at ~50 fps, ~42 draws/frame, pipelines create normally, no ANR. Screenshot:
  `../dump/android/after_revert_menu.png`.
- Muse's change is preserved outside the repo at
  `../dump/android/handoff_muse_dynswitch.patch` and `../dump/android/sm2_glsl_dynswitch.c` if
  anyone wants to retry a smaller version.

## Restore / rebuild recipe (from repo root)

```sh
# Translator is HEAD; android_entry.c keeps the env reader.
"/Users/heath/Virtua Tennis 3/tools/venv/bin/python" tools/formats/shaders/gen_spirv_pack.py \
  ../dump/android/shaders.pack

# VT3_GAME_DIR must be ABSOLUTE. The relative form in HANDOFF_BEAM.md no longer configures:
# CMake resolves it under the build dir and cannot find ../dump/recomp/vt3_nocd/vt3.c.
ANDROID_NDK=/opt/homebrew/share/android-ndk cmake -S runtime -B ../dump/android/cmake-arm64-main \
  -DCMAKE_TOOLCHAIN_FILE=/opt/homebrew/share/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_PLATFORM=android-26 -DANDROID_ABI=arm64-v8a \
  -DVT3_GAME_DIR="/Users/heath/Virtua Tennis 3/dump/recomp/vt3_nocd" -G Ninja
cmake --build ../dump/android/cmake-arm64-main --target vt3android -j 8

# Bare llvm-strip is not on PATH; use the NDK copy.
/opt/homebrew/share/android-ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip \
  -o ../dump/android/libvt3android-stripped.so ../dump/android/cmake-arm64-main/libvt3android.so

"/Users/heath/Virtua Tennis 3/tools/venv/bin/python" android/build_apk.py game vt3android \
  --so ../dump/android/libvt3android-stripped.so --install c27c2f60

/opt/homebrew/bin/adb -s c27c2f60 push ../dump/android/shaders.pack \
  /sdcard/Android/data/net.vt3port.game/files/shaders.pack
/opt/homebrew/bin/adb -s c27c2f60 push ../dump/android/vt3.env \
  /sdcard/Android/data/net.vt3port.game/files/vt3.env
```

Always `adb shell am force-stop net.vt3port.game` before relaunching; a `monkey` launch racing a
running game once produced a spurious ANR. Run one game at a time.

Pack md5s for reference:

| pack | md5 |
| --- | --- |
| reverted / known good | `fd742109e9caa519ce777fee09d25173` |
| Muse 256-case switch | `b0434a40c06a7ccc4c7784ba197f9dcc` |

## The T-pose problem is unchanged

The CPU-side palette is correct (`skin_check` shows animated, nonzero, in-range weights), but
Adreno still renders bind pose at `vs 98f59399`. The 256-case test did not survive long enough to
render, so the cause is still the open question in `HANDOFF_BEAM.md`.

Suggested order for the next attempt:

1. **Constant bank as a `samplerBuffer` / `texelFetch`** (most principled). This removes
   `OpAccessChain` dynamic UBO indexing entirely without a giant switch, and matches how the GL
   path indexes uniforms. Needs a `vk.c` descriptor/binding change plus translator changes; the
   constant data is already uploaded per draw.
2. **Re-run Beam step 1 alone** (move `a0`/`aL` to function scope, no switch) as a cheap A/B.
   It changes codegen without changing the number of dynamic reads.
3. If retrying a switch, keep it small and measure first: emit cases only for the bases the shader
   actually uses, compile **one** test pipeline with timing around `vkCreateGraphicsPipelines`,
   and check for a hang before touching all 569 programs. A 256-arm function per relative shader
   is hostile to this compiler.
4. Last resort: CPU-skin the affected draws in the shim, as Beam already does for verification.

Unrolling the skinning loop alone (Beam suggestion 2a) removes `vsc[195 + aL.x]` but leaves the
palette read `vsc[15 + a0.x]` dynamic, so it does not by itself test the hypothesis.

## Device facts

- Serial `c27c2f60`; adb at `/opt/homebrew/bin/adb`.
- Adreno (TM) 740, OpenGL ES 3.2, Vulkan driver `adreno`, Android 16 / SDK 36.
- App `net.vt3port.game`; data at `/sdcard/Android/data/net.vt3port.game/files`.
- Build dir `/Users/heath/Virtua Tennis 3/dump/android/cmake-arm64-main` (source
  `/Users/heath/Virtua Tennis 3/vt3-port/runtime`).

## Artifacts (in `../dump`, not the repo)

- `android/shaders.pack` — reverted pack (`fd7421...`).
- `android/libvt3android-stripped.so`, installed APK `net.vt3port.game`.
- `android/vt3.env` — device debug switches.
- `android/handoff_muse_dynswitch.patch`, `android/sm2_glsl_dynswitch.c` — Muse's experiment.
- `android/after_revert_menu.png` — menu after the revert.
- `android/vt3_wait.png` — the ANR dialog from the switch build.
- ANR traces are at `/data/anr/` on the device (system-readable only; not pulled).

## Commits / identity

- Nothing from this investigation is committed; the working tree contains only the intentional
  `android_entry.c` env reader plus the untracked handoffs.
- Author/committer: `hjcoggan <191880570+hjcoggan@users.noreply.github.com>`.
- End commits with a `Co-Authored-By` trailer naming the session/model.
- Never commit game data (lives in `../dump`). Push to `https://github.com/hjcoggan/vt3-port`.
