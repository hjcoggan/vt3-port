# Handoff: Virtua Tennis 3 port (2026-10-16)

The PC release (NoCD 1.0.1 build) is statically recompiled to C and runs natively on macOS
(SDL3 + OpenGL 3.3) and Android (Vulkan, Retroid Pocket, Snapdragon 8 Gen 2). Game data stays in
`../dump` and is never committed.

## Where things stand

**Mac (Vulkan through MoltenVK, the default; OpenGL with `VT3_RENDERER=gl`, or when no Vulkan driver is found): a full match plays.** Mac and Android now share `runtime/src/render/vk.c`. Visual parity of the Vulkan match scene with GL is not yet checked by eye.

**Mac (OpenGL reference, still correct):**
- Players render with bodies and clothing; line judges, ball kids and umpire render.
- Scene and variance shadow map render through render targets; StretchRect copies work.
- Viewports and clear rectangles are applied, so cutscene and menu backgrounds (court change,
  match point, result, CONTINUE?) show their scenes.
- Menus, tournament flow, serving, rallies, scoring, result screen all work.

**Android (Vulkan): current with `main` as of 2026-10-16 05:23.**
- APK and `shaders.pack` (569 shaders) rebuilt from `main` and installed on the Retroid.
- Menus, player select and a match render; players have bodies and clothes.
- Missing: Vulkan render targets and viewport, so the scene is darker, there are no
  shadows, and the pause menu sits on black. On Android, ENTER pauses during a match.

## Fixed this session (all on `main`)

| Commit | Fix |
|---|---|
| f2d1634 | `loop aL, iN` read its bounds from the never-set `vsi` uniform; the shader's own `defi` is now emitted as literals. Skinning ran zero times, so every skinned mesh (players, judges, ball kids) collapsed to a point. |
| e6fbf5e | Draws read every vertex stream. Player cloth uses two streams (UVs in 0, simulated positions/normals in 1); only stream 0 was read, so shirts and shorts collapsed. |
| 9ccf085 | Viewports, `SetRenderTarget` viewport reset, clear rectangles; scissor rows on render textures. |
| 9ceb13f | `gl_target_flipped()` backs every flip decision; trace timing counts from the first draw. |
| 8f2c032, 96373cb, 90e140d | Test loop: quiet-wait input script, repeating keys, match in about 28 s. |
| 026c563, b93dfb3, fddee5a, 6bf88d1, b0342a5 | Build/run tools in the repo; debug traces (below). |

## Build and test

```bash
# Mac build (about 3 s; a new OUT dir reuses the translated vt3.c object)
tools/venv/bin/python tools/recomp/build_native.py ../dump/recomp/vt3_loopfix
# Build another tree (an agent worktree, an older commit) into its own dir
tools/venv/bin/python tools/recomp/build_native.py ../dump/recomp/vt3_x --root <tree>

# Run to a match and capture: BIN OUTDIR SECS SHOT_TIMES...
tools/recomp/run_match.sh ../dump/recomp/vt3_loopfix/vt3_native /tmp/cap 40 36
PLAY_POINTS=1 tools/recomp/run_match.sh ... 150 40 80 120   # auto-play: points, changeovers, CONTINUE?
```
(`tools/venv` is `/Users/heath/Virtua Tennis 3/tools/venv`, outside the repo.)

- The route is Tournament with quiet waits (`wN:KEY` waits until no texture has been created for
  N s). In play by about 28 s. Federer is always 1P; the tournament and opponent vary per run.
- Without `PLAY_POINTS` the match waits at the first 1P serve (correct game behaviour).
- `run_match.sh` warns when the route never reached a match.
- Logs go to `OUTDIR/err.txt`. Capture tiles: `ffmpeg -i s_%02d.png -filter_complex tile=4x3 sheet.png`.

**Android:**
```bash
tools/venv/bin/python tools/formats/shaders/gen_spirv_pack.py ../dump/android/shaders.pack
cmake --build ../dump/android/cmake-arm64 --target vt3android -j 8
/opt/homebrew/share/android-ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip -o ../dump/android/libvt3android-stripped.so ../dump/android/cmake-arm64/libvt3android.so
tools/venv/bin/python android/build_apk.py game vt3android --so ../dump/android/libvt3android-stripped.so --install c27c2f60
adb -s c27c2f60 push ../dump/android/shaders.pack /sdcard/Android/data/net.vt3port.game/files/shaders.pack
```
- Regenerate the pack after any translator change (`runtime/src/render/sm2_glsl.c`).
- Retroid: keep it asleep when not in use. Wake with `input keyevent 224` plus a swipe up
  (screen 1280x960), sleep with `input keyevent 223`. Screenshots are black while dozing.

## Debug toolbox (env vars, all off by default)

Shaders are named by an FNV-1a hash of their bytecode (handles change every run). The same hash
over a file in `../dump/recomp/diagnosis/shaders_dump/*.bin` identifies it.

| Variable | What it does |
|---|---|
| `VT3_TRACE_DRAW=N`, `VT3_TRACE_AFTER=s`, `VT3_TRACE_MIN=prims`, `VT3_TRACE_HASH=hex` | Per-draw trace: shaders, hashes, state, constants, samplers. `VT3_TRACE_LIGHT`, `_PSC`, `_VSC` add more. |
| `VT3_SKIN_CHECK=N`, `VT3_SKIN_AFTER=s` | CPU-skins draws and reports NDC/world boxes, weights, palette, and a per-mesh facing-vs-cull count. Rigid meshes too when `VT3_TRACE_HASH` names them. |
| `VT3_PIXEL="x,y;..."`, `VT3_PIXEL_AFTER=s`, `VT3_PIXEL_TEX=1` | For one frame, logs every draw that changes a watched back-buffer pixel. Names the draw behind any wrong colour. |
| `VT3_DUMP_TEX=dir` | Every decoded texture upload as TGA, plus D3DX DDS inputs. |
| `VT3_TRACE_LOCK` | Texture uploads with a guest backtrace. |
| `VT3_TRACE_NAN` | Vertex-constant uploads containing NaN/Inf, once per guest caller, with backtrace. |
| `VT3_TRACE_RTSEQ`, `_STRETCH`, `_FRAMES`, `_FPS`, `_VIEWPORT` | Frame structure: target switches, clears, copies, presents, frame rate. |
| `VT3_SKIP_MESH=hash:prims,...`, `VT3_SKIP_FRAME_DRAWS=lo-hi` | Drop draws. |
| `VT3_SKIN_NOCULL`, `VT3_SKIN_FLIPCULL`, `VT3_NO_VSM`, `VT3_SHADOW_NOFLIP`, `VT3_NO_ZTEST` | State experiments. |
| `VT3_INPUT_SCRIPT` | `t:KEY` absolute, `wN[@T]:KEY` quiet wait, `rN:KEY` repeat. |

Shim code can read the guest caller: `vt3s_guest_caller`, `vt3s_guest_esp`, `vt3s_guest_backtrace()`.

## Game facts worth knowing

- Frame structure: clear back buffer; planar-shadow mask pass into a 1024x768 target (characters
  flattened along the light, additive, depth off); two 512x512 variance shadow-map passes; main
  scene; HUD. In some phases the main scene goes to the 1024x768 target and is copied.
- Key shaders: body VS `98f59399` (`vs_2322e090`, 4-bone), 2-bone VS `2e3a60a2`, body PS
  `c62249b4` (`ps_23233000`), collar/cloth PS `6c3b5f94`, cloth VS `3ee6cb60` (rigid, two streams),
  belly patch `VT3_P_hara` PS `ea052190` (tex×tex, correct as is).
- Textures come from relocated `.cog`/`.gog` resource packs (DDS loader `FUN_0040ba10`). Materials:
  apply `FUN_00413270`, texture bind `FUN_00419500`. Outfits are chosen by pack index
  (`FUN_004094b0`), not tinted.
- Ghidra decompile: `../dump/recomp/vt3_nocd101_all.c`.

## Open issues, with leads

1. **NaN matrices between points.** `VT3_TRACE_NAN` shows NaN cameras (`c0..c3` from 0x413318,
   0x417c19, 0x4067c2), light matrices (`c248..c251`), eye position (`c8`, NaN w) and bone palettes.
   The court-change camera is also rolled exactly 90 degrees. Suspect x87/SSE emulation in
   `tools/recomp/rt/vt3fpu.c` / `vt3simd.c` (for example `fsincos` stack order), or
   `runtime/shim/src/d3dx9_math.c`. Unfinished agent worktree: `.claude/worktrees/agent-a3454fdf1375c93b9`.
2. **Crackle and stutter.** Heard on menu clicks and during level loading, not on the court (the
   streamed music is clean). Measured on the Mac: the device never underruns, and the game's writes
   land 70–100 ms ahead of the play cursor on a 128 ms streaming buffer, so the stream is in time.
   The underrun counter misses fully empty callbacks, so silent gaps are invisible to it. Leading
   idea, not confirmed: short sound effects start from stale or unwritten data at onset, because
   `write_cursor` on a stopped buffer adds the 50 ms lead (`runtime/shim/src/dsound_stub.c`). The
   uncommitted audio work was reviewed and is not merged (data races, see the review notes in
   `/Users/heath/Virtua Tennis 3/wip-antigravity-2026-10-16.patch`).
3. **Vulkan render targets, viewport, scissor clears, colour write mask** (`runtime/src/render/vk.c`).
   Use `gl33.c` as the reference: `gl_target_flipped`, `gl_rect_low`, `gl_rt_*`, `apply_state`.
   Unfinished worktree: `.claude/worktrees/agent-ad3799b0eac9d84e9`.
4. **Shadows unverified.** Long, flat player shadows may be correct for a low sun. Needs a
   reference screenshot of the original game.
5. **Frame rate.** 30–50 fps was measured with four other game instances running; re-measure alone.
6. **Quiet-wait route.** The Singles and player-select screens create no textures, so those steps
   use fixed margins and can occasionally miss under heavy load.

The agent worktrees are on branches `worktree-agent-*`. Review before merging; delete with
`git worktree remove <path>` when done.

## Done: Vulkan on the Mac through MoltenVK (2026-10-16)

The Mac build now runs the same Vulkan renderer as Android, with OpenGL as the fallback. Commits
`5897344`..`3416541`. MoltenVK, the Vulkan loader and the validation layers come from Homebrew
(`molten-vk`, `vulkan-loader`, `vulkan-headers`, `vulkan-validationlayers`). The loader is linked
dynamically, so the .app must bundle it, and that packaging is still to do. Remaining gaps: visual
parity with GL by eye, and the Vulkan path with `VT3_TRACE_*` traces.

## Conventions

- Commits use the noreply identity and end with the Co-Authored-By trailer. Push reviewed progress.
- Never commit game data. `../dump` holds data, builds and captures.
- Builds take seconds: build inline. Hand long, self-contained investigations to subagents in
  worktrees (`build_native.py --root` into their own OUT dir).
