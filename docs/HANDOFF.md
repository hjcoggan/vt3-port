# Handoff: Virtua Tennis 3 port (2026-10-17, evening)

The PC release (NoCD 1.0.1 build) is statically recompiled to C and runs natively on macOS and on
Android (Retroid Pocket, Snapdragon 8 Gen 2). Mac and Android now share one renderer, Vulkan
(`runtime/src/render/vk.c`, MoltenVK on the Mac), so a fix in it reaches both. Game data stays in
`../dump` and is never committed.

## Where things stand

- **Mac:** Vulkan is the default. `VT3_RENDERER=gl` forces OpenGL (`gl33.c`, still the reference);
  with the variable unset the game falls back to OpenGL when no Vulkan instance can be made.
- **Android:** Vulkan only. Player models, clothes and sideline figures now render correctly on the
  Retroid (confirmed by the owner). The Retroid's primary use is the goal; keep it asleep when idle.
- Menus, tournament flow, serving, rallies, scoring all work on both. Mac audio is clean.
- Court shadows are fixed on both backends (below). The .app now bundles the Vulkan loader.

## Performance (session of 2026-10-18)

In-match frame rate, averaged over a full rally. The Mac figures come from one fixed scripted match
and compare directly against each other; the Retroid's do not, for the reason under the table:

| | Mac | Retroid |
|---|---|---|
| before | 79.3 fps (12.60 ms) | 35 fps |
| after | 102.3 fps (9.78 ms) | 49.6-62.9 fps depending on the match |

The Mac figures are from one fixed scripted match and compare directly. The Retroid's do not: each
launch draws a new opponent and venue and the work varies with them, so the two numbers bracket it
rather than trend it.

Everything is host CPU; the GPU is ~3.5 ms a frame on the Retroid, so it is nowhere near the limit.
Five things got it there, all on `main`:

- **`9e95268`** the guest's x87 work runs in the host double unit instead of the bit-by-bit soft
  float, guarded so it only fires for the control word this game runs in. ~1.2x on its own.
- **`839f335`** the x87 *register file* is held as host doubles, so the guard and the conversion at
  both ends of every operation go too and an add is an add. `vt_x80` is now a typedef for `double`,
  which is what lets the translated code stand unchanged - it declares `vt_x80 t = vt_fld32(...)` at
  ~22,000 sites and never touches a field. Mac, uncapped: 117.1 fps against 102.3.
- **`1ffabb4`** typed stores instead of `memcpy` in `wr16/32/64`, so the guest memory base is not
  reloaded after every store.
- **`4669aea`** a direct-mapped cache for `vt_icall` (which binary searched 572 import addresses on
  every host call, 9.6% of CPU) and the per-draw constant copies: the shim copied the device's 4 KB
  vertex and pixel arrays, the renderer copied them again, on top of clearing whole uniform blocks.
  The device arrays now go straight through.
- **`909f94f`** `0.0` used to fail the fast-path guard and fall into the soft path, which is most of
  why `pack_round` was 4.7% of CPU.

**`build_native.py` keys the translated object's cache on the runtime headers as well as `vt3.c`.**
The translated code includes `vt3rt.h` and declares `vt3_cpu`, so an object built against an older
copy of the header links happily and is then wrong at run time - the struct's layout had changed
under it. The check used to compare mtimes against `vt3.c` alone.

**The double register file has no precision control.** `vt_fadd` warns once if it ever sees a
control word other than 0x037F, and a full match run produces exactly one such warning: 0x0C3F,
single precision toward zero, from a library `control87` path. There is no constant 0xC3F in the
translated code and only four FPU env save/restore sites in the whole game, so it is not game math.
The rounding mode is still honoured by every store and integer conversion.

**Measurements on the Retroid must be checked against a screenshot.** Navigating with
`keyevent 54` (Z) after a match ends walks the menus instead of keeping a rally going, and a menu
runs at ~118 fps. Capture a frame at the start and the end of the window and confirm both show the
match, or the number means nothing.

`VT3_FPS_CAP` defaults to the display's rate: 60 on the Mac, 120 on the Retroid. The Mac's pacing is
free at 60. On the Retroid, holding the game to 60 would show each frame twice over while the frame
times still land unevenly - judder. At 120 the budget is 8.3 ms, below what a frame costs today, so
nothing sleeps and the game runs flat out, and if it ever reaches 120 the same code holds it there.
`VT3_FPS_CAP=0` still turns the limit off entirely.

**In-match rates only compare within one run.** Every launch picks a different opponent and venue,
and the load varies with them - measured uncapped, one match ran at 62.9 fps and another at 49.6.
Screenshots were taken at both ends of every window below, because Z walks the menus once a match
ends and a menu runs at ~118 fps. Do not attribute a difference between two runs to a change you
made until you have seen the same scene.

Running at a true, steady 120 would need roughly 1.9x more, and the two things that could supply
it are the x87 register file being held as host doubles rather than x80 (x87 is 27% of CPU;
generated code touches the register file only through `FPUSH`/`FPOP`/`FST`, so it needs no
recompilation of `vt3.c`), and `rep movs` translating to `memmove` instead of a dword loop
(`fn_00421e90` is 20.7%, and its 64-byte struct copies are scalar).

## Fixed this session (all on `main`)

| Commit | Fix |
|---|---|
| *this session* | **Menu sound: the mixer's gargle and clicks.** The per-buffer mix is rewritten as `ds_mix_render` (declared in `win32/dsound.h`, checked by the new `test_dsound_mixer` in `tests/selftest.c`). (1) Q32 phase stepping with **linear interpolation** replaces nearest-sample conversion: a 22.05/44.1 kHz buffer (the menu SFX) is resampled smoothly instead of sample-held, which aliases into the hard, gargly edge, while 48 kHz (the streamed music) passes through bit-exact because the phase fraction is always zero. (2) The stale-block policy from e8d1a37 cut to silence the instant the mixer stepped into a block the game had not rewritten, and every miss was a click; the block now drives a Q15 **gate that ramps over 2 ms** (`VT3_MIX_RAMP_FRAMES`) out and back in, and `Play`/`SetCurrentPosition` start with the gate closed, so a sound fades in instead of clicking mid-waveform. Old audio is still never replayed. (3) **`SetFrequency` and `SetPan` were accepted and then ignored**: `b->freq` now drives the playback rate and both cursors, and pan attenuates the far channel linearly. Also: the setters take `g_mu` (the mixer could read a half-written WAVEFORMATEX), `GetCurrentPosition` takes one clock read for both cursors, the fresh-block bookkeeping carries `last_blk` in the buffer instead of restarting at every 4 ms chunk (the block at each chunk boundary kept its mark for ever and could replay at a loop wrap), and `DuplicateSoundBuffer` copies volume/pan/frequency. Bench (x86 VM, worst case): 240x realtime mixing 32 concurrent 22.05->48 kHz buffers, ~87 ns an output frame. **Needs a listen on the Mac and the Retroid**; `VT3_TRACE_AUDIO=1` still prints playing buffers, peak and underruns once a second, and `VT3_DUMP_AUDIO=path` captures the mix as raw s16 stereo 48 kHz. |
| *this session* | **On-screen touch controls (Android), with auto-hide for real hardware.** A runtime-drawn overlay in the style of [rokobuljan/gamepad](https://github.com/rokobuljan/gamepad): a thumb stick lower-left (movement; also drives the d-pad so menus navigate), the A/B/X/Y diamond lower-right, LB/RB pills in the top corners, and SELECT/START pills bottom-centre. Geometry is emitted as translucent triangles through the fixed-function XYZRHW path (`vt3_gfx_draw`, `tex = 0`), so it looks the same on Vulkan and GL and scales with the frame; labels come from the public-domain 8x8 font in `src/ui/font8x8.h`. `d3d9_gpu_present()` calls `vt3_touch_frame()` after the game's frame; `input_android.c` routes touch motion (per pointer, so move + shoot works) and maps it through the same letterbox present() uses. The controls hide themselves on the first key or pad event and stay hidden (`VT3_TOUCH_FORCE=1` overrides). Verified on the Retroid (`c27c2f60`): the stick drives menu navigation and the quit dialog (5x pad-down + pad-A), buttons select, and both auto-hide paths log and clear the overlay. See `docs/TOUCH_CONTROLS.md`. |
| *this session* | **Native widescreen (Hor+) on macOS and Android.** The game ships its own widescreen mode (`WIDESCREEN = 1`, config `0x8598a0+0x122` -> engine `+0x9c`), whose camera keeps the vertical FOV and widens horizontally. `vt3shim_init()` now brings the window up first, picks a frame at the drawable's aspect (height capped at 1080), merges `DX/RES/WIDTH`/`DX/RES/HEIGHT`/`WIDESCREEN=1` into `<game_root>/vt3.ini`, and patches the camera's fixed 16:9 aspect constant (`0x6cf0e8`) to the real frame aspect (2.167 on a Galaxy S26, 2.069 on the Mac) so a non-16:9 panel is not pillarboxed. The engine flag is re-asserted every frame. The resolution goes through the game's own INI, not a D3D9 back-buffer override, because the game caches it for the per-frame viewport/HUD (an override only painted the top-left corner). `VT3_NO_WIDESCREEN` disables it, `VT3_RENDER_HEIGHT` overrides the height cap. Full reference: `docs/WIDESCREEN.md`. |
| *this session* | **Android full-display, upright frame on Adreno.** `create_swapchain()` now presents with `preTransform = IDENTITY` where supported: Android reports `currentExtent` already in the display's current orientation (3120x1440 in landscape) while `currentTransform` is still `ROTATE_90`, and the mismatch made the compositor rotate the image again (sideways, squashed). `vk_present()` ignores `VK_SUBOPTIMAL_KHR` on Android (the expected result of that choice; rebuilding would recreate the swapchain every frame). `deploy_data.sh` now `chmod -R a+rwX data2` so the app can both read the archives and write `vt3.ini`. |
| 5897344 .. 3416541 | Vulkan on the Mac through MoltenVK: render targets, viewports, clear rects and colour mask in `vk.c`; the platform picks Vulkan before the window exists (`vt3_vk_probe`), OpenGL is the fallback. |
| 1c37b9e | Vulkan's per-frame vertex/index rings were too small and silently dropped draws (2625 index drops in 100 s). Now 32 MB / 16 MB per frame; drops and the white-texture fallback warn once. |
| 8890cab | x87: rounding a 64-bit mantissa up past all ones was never detected (`1 << 64`). The result was an invalid number the guest read as NaN. It caused the NaN camera, light and bone uploads. 10 NaN uploads per 100 s match before, 0 after. |
| dce147b | Translator zeroes temporary registers. SPIR-V on Metal/Adreno leaves them undefined, which gave T-posed models and missing torsos on Android. **Every shader hash changes: regenerate `shaders.pack` after any change to `sm2_glsl.c`.** |
| 286a448 | **Adreno T-pose root cause (the real fix).** Adreno 740 reads the skinning shader's blend-indices vertex element (`UBYTE4`, bound as `R8G8B8A8_USCALED`) as zero, so the bone index `a0.x` collapsed to 0 and every skinned mesh rendered as a rigid bind/T-pose (this is separate from the `dce147b` zero-init). `d3d9_draw.c` now expands the four index bytes into a `float4` in the shim's per-draw vertex copy (stride + 12, following attribute offsets + 12). No game data or shader changed. |
| ee0d643 | Renderer now carries per-draw min/mag texture-filter state and creates point/linear/anisotropic samplers. **This did not fix the missing net**; the net root cause was sampler address mode (see `04d62fa` below). |
| bd01b0f | **Xbox 360 gamepad layout (macOS & Android).** Verified against `VT3.exe`'s `FUN_004018c0`, `FUN_00476300`, `FUN_004763c0`, and `0x0085603c` (`[12,13,14,15,2,0,3,1,4,7]`): DI button 0 (`A`) is Top Spin / Menu Select, DI button 2 (`B` and `X`) is Slice / Drop Shot / Menu Cancel, DI button 3 (`Y`) is Lob / World Tour Home, DI button 4 (`LB`/`RB`/`LT`/`RT`) is Instruct COM Partner / Incoming Mail, DI button 7 (`Start`) is Pause, and `RB`/`RT` assert `DIK_LMENU` (`0x38`) for World Tour Tour Calendar. It also mapped `Back` to `DIK_V` and left DI button 1 unmapped, which lost Change Camera (see the row below). Added `debug.vt3.swap_ab_xy` on Android and `VT3_GAMEPAD_USE_LABELS` on SDL3. |
| *this session* | **Change Camera restored on Back/Select.** `bd01b0f` left DI button 1 unmapped and its `Back`->`DIK_V` bridge fed the wrong action: `VT3.exe`'s keyboard remap (`FUN_00401bb0`) sends `DIK_V` (`0x2F`) to raw `0x1000000` -> action `0x80000` (Instruct COM Partner), while its own pad table (`FUN_004018c0`, slot 7) puts Change Camera on DI button 1 -> raw `0x10` -> action `0x200`. The shim's `fill_pad` now asserts button 1 while Back/Select is down, and the `DIK_V` bridge is gone. `VT3_SELECT` overrides the button index, `VT3_SELECT_KEY` an optional DIK offset for measuring alternatives. Verified on the Retroid (`c27c2f60`): Select toggles the close/wide camera in a live doubles match, and the A/B/X/Y/Start layout is unchanged. |
| 04d62fa | **Missing net mesh fixed.** The net's UVs span many texture tiles (24x4 for the court-net draw, measured with `VT3_TRACE_UV`; earlier notes said 30x4), while D3D9 sampler address U/V are `WRAP` (value 1). Both backends hardcoded `CLAMP_TO_EDGE`, so only UVs within the first tile rendered the net grid; the remainder sampled a transparent edge. Per-draw address modes are now carried from `SetSamplerState` into Vulkan sampler selection and GL texture wrap state. Verified on the Retroid: the grid now covers the full net. |
| *this session* | **Net mesh fixed: samplers could never reach a mip level.** The net is a 128x128 wire-textile (`tex_230`, the draw's `gl 230`) tiled 24x4 by `c14`, so at any real distance it must be sampled from a small mip — but `sampler_for` and the fallback `g.samp` never set `mipmapMode`/`lodMax`, and Vulkan defaults those to `NEAREST` and **0.0**. Every sample was hard-clamped to mip 0 no matter how far away the net was, so the fine grid minified to sub-texel and rendered as a scrambled, coarse-looking pattern (also why the earlier mip attempt `b6e14e2` could not have worked: it built a chain nothing could select — and see the row below for why it black-screened). Now: full mip chain on 2D upload (`full_mip_count`, `TRANSFER_SRC` only when `mips > 1`, per-level `barrier_mip` with each level transitioning from its own layout) **and** `mipmapMode LINEAR`, `minLod 0`, `maxLod VK_LOD_CLAMP_NONE` in both `sampler_for` and `g.samp`. Verified on the Mac (0 validation messages, clean fine mesh) and on the Retroid (`c27c2f60`): the net now matches the PC reference's uniform fine mesh, no black screen, memory unchanged (~55 MB graphics of 11.4 GB), and no frame-rate cost (A/B from the log ring buffer: 38.6 fps before, 39.5 after; thermal status 0). |
| *this session* | **Net mipmap pyramid: first attempt reverted.** The `b6e14e2` attempt black-screened the Retroid (game ran, no Vulkan errors) and was dropped by `13380c0`; the row above supersedes it. Kept for the record because the failure was misdiagnosed as a blit/barrier problem: the upload-time blit was only half of it, and the samplers could not select a mip anyway. Cube textures and render targets still use one level. |
| 7cd9a86 | `VT3_VK_DEBUG=1` routes validation-layer messages into the log (desktop). |
| e8d1a37 | DirectSound: unwritten 256-byte blocks mix as silence instead of replaying old audio, and the mixer thread asks for audio priority. Mac: 13 exact 128 ms repeats per 60 s match before, 0 after. **Not yet confirmed on the Retroid**: if the game's writer is starved there, repeats become gaps. Back out with `git revert e8d1a37`. |
| db04521 | Shadow debugging aids (below). |
| *this session* | Planar-shadow blur ran under the previous mesh's declaration (NORMAL at 12, TEXCOORD at 16, stride 24) with a 20-byte pos+uv quad, so TEXCOORD spilled (16+8 > 20) and sampled the next vertex's X as V: scrambled slivers, streaky court, missing player shadows. **Root cause:** the game draws the quad with `SetFVF(0x102)`, but the shim kept the declaration the previous mesh left set. `d3d9_device.c` now enforces D3D9's rule that an FVF and a vertex declaration are mutually exclusive (`SetFVF` clears the declaration, `SetVertexDeclaration` clears the FVF), so the quad's layout is derived from its FVF. Shim-level, so GL and Vulkan are fixed together. |
| *this session* | **Court-change / result / CONTINUE backgrounds** were the same root cause on the picture quads (also FVF `0x102`): they drew right after mesh passes, so the stale mesh declaration made the picture's position/UV read the wrong bytes — rotated ~90° with edge-clamped streaks. The `SetFVF` fix above corrects them; the previous session's heuristic in `d3d9_draw.c` (which only recognised the one stale layout) is removed. Traced the quad (`VT3_TRACE_QUAD_BB`) and compared before/after captures: result and CONTINUE backgrounds now upright and clean on GL, and the same two-attribute layout on Vulkan (verified with a Vulkan sanity run). |
| *this session* | `assets/macos/make_app.sh` bundles the Vulkan loader, SDL3 and MoltenVK into `Contents/Frameworks`, writes a relative-path MoltenVK ICD, copies `shaders.pack` into `Resources`, and ad-hoc signs. The launcher sets `VK_ICD_FILENAMES` and `VT3_SHADER_PACK` at the bundle. A missing ICD at run time still falls back to OpenGL; a missing loader library at launch (dyld failure, no fallback) no longer happens. Verified: bundled .app starts Vulkan on Apple Silicon with the bundled pack (569 programs). |
| 3269ef4 | **Android sleep/wake black screen.** When the device sleeps the window surface is destroyed (`APP_CMD_TERM_WINDOW`) and on wake a new one arrives (`APP_CMD_INIT_WINDOW`), but the renderer kept presenting to the surface built from the old, dead window — the game and its audio ran on over a black screen. `vk.c`'s `vk_present` now notices the new window and rebuilds in the right order: retire the swapchain **before** destroying the old surface (a swapchain must not outlive its surface, and `oldSwapchain` must belong to the same surface as the new swapchain), rebuild the per-image semaphores, then make a fresh surface and swapchain. A failed rebuild leaves `g.window` unset so the next frame retries; while no window exists the frame is submitted empty and the surface is left alone. **Not verified on the Retroid** (the device was offline when this was built): the builds the user tested still had the surface-first ordering, and one also had a `d3d9_gpu_present` early-return that skipped `vt3_platform_pump_events`, so `APP_CMD_INIT_WINDOW` was never delivered on wake and the screen stayed black. |
| *this session* | **Android volume HUD never appeared.** `vt3_input_android_event` returned 1 (consumed) for *every* key, so the system never saw the volume keys. Volume up/down/mute now return 0 and are left to the system. |

## Open issues, with leads

1. **Court shadows: fixed this session, needs a Retroid look.** The in-play blur now uses the quad's
   real layout (see Fixed table). Mac GL and Vulkan both show clean courts with player shadows at the
   feet on hard and clay. The old notes are kept for the record:
   - Per frame in play: clear back buffer; mask pass into target `0x20300350` (gl 1, A8R8G8B8
     1024x768; skinned and rigid draws, ONE/ONE, depth off, cull CW); one 4-vertex strip into
     `0x20322810` (gl 2, R5G6B5 1024x768) reading gl 1; two 512x512 shadow-map passes; the scene,
     which samples gl 2 (`s8`) at the same screen position as the pixel it shades.
   - That strip is a plain 2x2 box blur (VS `1b40f853`, PS `c3a461ab`, half-texel offsets, weights
     0.25). The **same shaders blur the 512x512 shadow maps, and those work.**
   - Root cause found: the in-play blur runs under the previous mesh's declaration (NORMAL at 12,
     TEXCOORD at 16) with a 20-byte pos+uv quad, so TEXCOORD spills and samples the next vertex.
     `VT3_SHADOW_COPY=1` (GL) and `VT3_DROP_OOB_ATTR=1` are now just debugging aids; the FVF /
     declaration rule below is the real fix (the earlier `d3d9_draw.c` layout quirk was a workaround,
     since removed).
   - Pitfall: `dump_targets` rebinds texture unit 0. Dump before a draw's textures are bound.
2. **Court-change / result screens: fixed this session.** Same root cause as the shadow blur on the same
   quad family: the game sets `SetFVF(0x102)` for its picture quads, but the shim left the previous
   mesh's vertex declaration set, so position/UV read the wrong bytes and the picture came out rotated
   with edge-clamped streaks. `d3d9_device.c` now makes `SetFVF` and `SetVertexDeclaration` mutually
   exclusive (as D3D9 requires), which fixes every such quad in one place. Traced with
   `VT3_TRACE_QUAD_BB` (VS/PS hash, FVF, stride, declaration): the picture quads now derive exactly two
   attributes (POSITION at 0, TEXCOORD at 12) from FVF `0x102` on both GL and Vulkan. GL before/after
   captures show the result and CONTINUE backgrounds upright, and a Vulkan sanity run renders the same
   changeover screen identically to GL (same layout, same scaling). The earlier heuristic that only
   recognised the blur's one stale layout is gone. Old notes for the record:
   - Reproduced on the RESULT and CONTINUE screens with the shadow fix in place, so it was a different
     quad/shader than the in-play blur (VSes `dce123fc`, `4a2fc5d6`, `cd2adbf0`, `80d94c77`: all
     `vs_pos = vi0; vo_T0.xy = vi1.xy; gl_Position = (x, -y, z, w)`, i.e. FVF `0x102`).
   - The NaN fix did not help (it is in the build).
3. **Menu gargle: root cause found (2026-10-23 session), game-side stall is the remainder.**
   Everything the game plays - music and menu SFX alike - streams through ONE 73728-byte,
   48 kHz 6-channel looping DirectSound buffer; there are no secondary buffers. The game's
   single audio-writer thread stalls for 100-300 ms during menu loads (Retroid; 40-113 ms on the
   Mac), and on wake its stream writer SKIPS the region it could not write: 11-16% of Locks start
   17-70 ms past where the previous write ended. Skipped blocks then go a full 128 ms lap without
   a write, so the mixer inevitably reads them unmarked: with the gate, a 2 ms fade to silence
   and back (the gargle); with `VT3_DS_NO_GATE=1`, a 128 ms stale repeat and clicks (e8d1a37's
   original artefact). No shim timing lever can recover content the game never wrote - measured:
   raising the writer thread's priority (a since-removed `VT3_DS_WRITER_PRIO` experiment) left writer-gap percentiles unchanged (the thread is blocked on guest
   locks, not CPU-starved; the worst stalls show the whole process idle), and a true 96 ms
   read-behind (the since-removed `VT3_DS_MIX_LAG=96` experiment) left stale counts unchanged too. Device numbers, 57 s menu
   session each: gate on 654 stale events / 130-427 ms of below-full gate per second / 12 of 26
   blips shredded; no-gate 16 of 26 blips shredded; underruns 0 throughout, so the sink (AAudio,
   ring, mixer cadence) is healthy. The gate stays the default because it keeps e8d1a37's
   no-repeat property. **Found and measured (2026-10-23): the stall is the runtime's own guest
   lock (gil).** All guest threads (main/loader, audio writer, winmm timer thread) serialize on
   one pthread mutex held across whole stretches of guest code; at the release points the busy
   main thread re-won the unfair futex almost every time. The new `VT3_TRACE_SLOW=ms` trace on
   the Retroid: 74-77% of write gaps >=30 ms are covered by gil holds (the worst gaps 94-100%,
   tid 1), the winmm timer fires 100-360 ms late inside the same windows, and no slow file read
   or single texture op explains them (converts <=23 ms; the big holds are chains of 8-17 ms
   holds with sub-ms re-acquires - the writer loses every handoff). Tried and **reverted after a
   listening A/B on the Retroid (2026-10-23)**: `gil_handoff` sleeping 2000 us instead of 50 us
   (traces improved: below-full gate 3501 -> 866 ms a minute, write gaps p99 120 -> 40 ms) and
   priming the 40 ms device ring with 30 ms of silence. Both sounded worse - more crackle - than
   the same build without them. Likely reason for the prime: it leaves the mixer only 10 ms of
   ring, so it falls behind its timeline and resyncs (an audible jump). The trace numbers did
   not predict what the ear heard; judge audio changes by listening. `VT3_GIL_YIELD_US` stays
   as an override with the 50 us default. Remaining: the worst menu loads still hold the gil in
   one unbroken 100-456 ms run (pure guest code with no shim call to hand off at), so gate dips
   persist at loads at a lower level. Trace recipe: `VT3_TRACE_AUDIO=1` in `vt3.env`, drive menus, then read the
   per-second `stale N events at byte B` lines against the Lock timeline; `VT3_DUMP_AUDIO` captures
   the mix for envelope analysis (bursts with internal 1 ms zeros = shredded blips). |
4. **Net: fixed this session** (see Fixed table). `04d62fa` fixed the missing U/V `WRAP` address mode
   so the 24x4 tiled net mesh renders across the full court, and the row above fixes the sampler LOD
   that clamped every sample to mip 0 (the coarse/scrambled grid). Both verified on the Retroid
   against the PC reference: a uniform fine mesh, no shimmer. Remaining: **shadows duplicated** was
   reported on Android before the zero-init fix; re-check with the current build.
5. **Screen shape.** The game should render cleanly at 4:3, 16:10, 16:9, 18:9 and 19.5:9. Letterbox
   is the right default (`runtime/src/render/letterbox.c`; integer mode ignores the display aspect,
   off by default). True widescreen needs a Hor+ change to the game's projection (guest code); size
   it by finding where the game sets its projection matrix. The Retroid is 1280x960, so 4:3 today.
6. **Packaging: done this session** (see Fixed table). Still to do: test the bundled .app on a Mac
   without Homebrew's SDL3/Vulkan/MoltenVK to prove it launches there.
7. Vulkan memory is about 128 MB host-visible (two frames of rings): watch it on the Retroid.
   MoltenVK logs "Metal does not support disabling primitive restart" (harmless).
8. Frame rate on the Retroid: **~39 fps average in play** (measured from the log ring buffer across
   a run: 38.6 before this session's mip work, 39.5 after — so the mip chain is not a cost; the older
   "53-55 fps" figure in previous handoffs predates this session's renderer commits and is not
   reproducible here). Venue and opponent are random per run, so compare within one build only, and
   check `Thermal Status` first (it was 0 while measuring). The GPU is not the wait (3.5 ms/frame,
   fence wait ~0.01 ms), so the stall is CPU-side (loading).
9. Route flake: the Tournament route sometimes never reaches a match (about 59-62 textures at the
   last key, no error), more often while a build is using the CPU. Rerun.

## Build and test

```bash
PY="/Users/heath/Virtua Tennis 3/tools/venv/bin/python"     # venv lives outside the repo
# Mac build (about 3 s; a new OUT dir reuses the translated vt3.c object)
$PY tools/recomp/build_native.py ../dump/recomp/vt3_x
# Run a match and capture: BIN OUTDIR SECS SHOT_TIMES...   (Vulkan needs the SPIR-V pack)
VT3_SHADER_PACK=../dump/android/shaders.pack PLAY_POINTS=1 \
  tools/recomp/run_match.sh ../dump/recomp/vt3_x/vt3_native ../dump/recomp/runs/x 60 45
# OpenGL for comparison: add VT3_RENDERER=gl
# Regenerate the pack after any translator change (about a minute)
$PY tools/formats/shaders/gen_spirv_pack.py ../dump/android/shaders.pack
```

- The route is Tournament with quiet waits; in play by about 28 s; the venue and opponent are random
  each run, so only same-run or same-venue frames compare directly.
- Validation: `VT3_VK_DEBUG=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
  VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d`.
- Homebrew: `molten-vk vulkan-loader vulkan-headers vulkan-validationlayers glslang`.
- Run one game at a time and check `pgrep -x vt3_native`: concurrent runs skew timing and audio.

**From scratch** (what players run; no files from `../dump`): `python build.py android --exe VT3.exe`
translates with `tools/recomp/vt3_seeds.txt` (function entries, addresses only, from the Ghidra
dump) and `tools/recomp/vt3_table_sizes.txt`, extracts the SM2 blobs from VT3.exe for the shader
pack, then runs the CMake and APK steps below into `build/`. The output matches the maintainer
build except that `../dump/recomp/vt3_nocd/vt3.c` predates the rep-movs fast path from e5bdaf9.

**Android** (configure once from the repo root; use a build dir for this tree):
```bash
ANDROID_NDK=/opt/homebrew/share/android-ndk cmake -S runtime -B ../dump/android/cmake-arm64-main \
  -DCMAKE_TOOLCHAIN_FILE=/opt/homebrew/share/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_PLATFORM=android-26 -DANDROID_ABI=arm64-v8a \
  -DVT3_GAME_DIR="/Users/heath/Virtua Tennis 3/dump/recomp/vt3_nocd" -G Ninja
cmake --build ../dump/android/cmake-arm64-main --target vt3android -j 8   # 5-10 min: huge vt3.c
/opt/homebrew/share/android-ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip \
  -o ../dump/android/libvt3android-stripped.so ../dump/android/cmake-arm64-main/libvt3android.so
$PY android/build_apk.py game vt3android --so ../dump/android/libvt3android-stripped.so --asset ../dump/android/shaders.pack --install c27c2f60
# First launch opens the installer (pick the ISO and VT3.exe); deploy_data.sh + touch files/install.ok skips it.
adb -s c27c2f60 push ../dump/android/shaders.pack /sdcard/Android/data/net.vt3port.game/files/shaders.pack
adb -s c27c2f60 push ../dump/android/vt3.env /sdcard/Android/data/net.vt3port.game/files/vt3.env
adb -s c27c2f60 shell am force-stop net.vt3port.game   # avoid a monkey relaunch racing a live game
adb -s c27c2f60 shell monkey -p net.vt3port.game -c android.intent.category.LAUNCHER 1
```
Push the pack **before** launching. `VT3_GAME_DIR` must be absolute (CMake resolves a relative one
under the build dir and cannot find `../dump/recomp/vt3_nocd/vt3.c`). Bare `llvm-strip` is not on
`PATH`; use the NDK's. The game reads `VT3_*` switches from `vt3.env` (KEY=VALUE) on the device, since
there is no shell to set the environment. Wake the Retroid with `input keyevent 224` and a swipe up,
sleep it with `keyevent 223`; `adb exec-out screencap -p > shot.png` is black while it dozes.

**Game data permissions.** `adb push` creates the `data2` tree owned by `shell:ext_data_rw` with mode
`2770`. On some devices (verified on a Galaxy S26, Android 16) the app process does not get the
`ext_data_rw` supplementary group, so it can traverse its own `files/` dir but not the pushed `data2/`
subtree: `GetFileAttributesA` fails with `EACCES` and the game reports every `*.XPAC` as missing.
Make the pushed tree world-readable and world-writable after any data push (`a+rwX`, not just
`a+rX`, because the shim also writes its `vt3.ini` widescreen config into `data2/`):
```bash
adb -s SERIAL shell chmod -R a+rwX /sdcard/Android/data/net.vt3port.game/files/data2
```
`image.bin` and `shaders.pack` are pushed as mode 666 and are readable either way; only the
directories need it. Do not `chmod` `files/` itself — the FUSE sdcard rejects it and it already
belongs to the app. `android/deploy_data.sh` does this at the end of every push.

## Debug toolbox (env vars, all off by default)

Shaders are named by an FNV-1a hash of their bytecode. The same 32-bit hash over a file in
`../dump/runtime/shaders/bin/*.bin` finds it; `../dump/android/tools/vt3sm2 IN.bin OUT.glsl` prints
the GLSL.

| Variable | What it does |
|---|---|
| `VT3_TRACE_DRAW=N`, `_AFTER=s`, `_MIN=prims`, `VT3_TRACE_HASH=hex` | Per-draw trace. The hash filter only applies with the draw trace on. |
| `VT3_TRACE_QUAD=1` | Layout, raw vertices, constants and state of small draws into off-screen targets. |
| `VT3_TRACE_QUAD_BB=s` | The same for small draws into the back buffer (screen pictures such as the changeover / result background), deduplicated by shader and layout; waits `s` seconds first, `0` for the whole run. |
| `VT3_TRACE_UV=1` | For the draws `VT3_TRACE_HASH` names: each float attribute's raw vertex span and its span after the `c14` UV scale (`vo_T0 = vi1 * c14`), plus the shader hashes. Use it to tell "the UVs in the buffer are wrong" from "the constant scaling is wrong"; the draw trace above now prints `c14` and each sampler's min/mag/address state too. |
| `VT3_DUMP_RT=dir`, `VT3_DUMP_RT_AT=s` (GL) | Write every 8-bit render target to TGA once. |
| `VT3_DUMP_QUAD=dir` (GL) | The same, just before and after the mask blur draw (`pre_` / `post_` files). |
| `VT3_SHADOW_COPY=1` (GL) | Replace the mask blur with a copy. |
| `VT3_DROP_OOB_ATTR=1` | Leave out vertex-declaration elements past the end of the vertex. |
| `VT3_TRACE_BLUR=1` (GL) | GL error, framebuffer status and scissor after the blur draw. |
| `VT3_VK_DEBUG=1` | Validation messages into the log (desktop Vulkan). |
| `VT3_SKIN_CHECK=N`, `VT3_PIXEL=...`, `VT3_DUMP_TEX=dir`, `VT3_TRACE_NAN`, `VT3_TRACE_RTSEQ`, `_STRETCH`, `_FPS` | As before: CPU skinning check, pixel watch, texture dump, NaN uploads, target sequence, copies, frame rate. |
| `VT3_SKIP_MESH`, `VT3_SKIP_TARGET=WxH`, `VT3_NO_VSM`, `VT3_SKIN_NOCULL`, `VT3_SKIN_FLIPCULL` | State experiments. |
| `VT3_TRACE_AUDIO`, `VT3_DUMP_AUDIO=file` | Audio trace; raw s16 stereo 48 kHz dump of the mix. With the trace on, the once-a-second line also carries `t=` (wall clock, for aligning with dumps), ring fill, mixer-timeline gap, resync count and underruns, and one per-buffer line: size/format/loop, cumulative Lock/Play counts, milliseconds below-full gate, stale events (transitions of the stale-block gate) with the byte they hit, and the mixer's read-behind versus the game's play cursor. See the "Menu gargle" note under Open issues. |
| `VT3_DS_NO_GATE=1` | Pin the stale-block gate open: the mixer ignores the fresh marks, so an unwritten block replays the previous lap (the pre-e8d1a37 behaviour: 128 ms repeats and clicks) instead of fading to silence. A/B experiment for what the gate itself contributes. |
| `VT3_TRACE_SLOW=ms` | Slow-call trace with that threshold in ms: every guest-lock hold that long at release ("gil hold: tid T held N ms"), a thread's wait for the lock itself ("gil acquire"), ReadFile/CreateFileA calls that long with the current holder's tid, winmm timers firing that long past due ("timer late"), and texture create/upload/convert ops that long. This is what traced the menu gargle to the guest lock; the default build logs none of it. |
| `VT3_GIL_YIELD_US=us` | Sleep in `gil_handoff` after releasing the guest lock (default 50). 2000 measured better in traces but sounded worse on the Retroid. Wider means the waiters (audio writer, winmm timer) win the lock; 50 us measured much worse for audio, 0 starves waiters for seconds. |
| `VT3_TRACE_READS=1` | Log every ReadFile of 4 KB or more with its file offset. |
| `VT3_INPUT_SCRIPT` | `t:KEY` absolute, `wN[@T]:KEY` quiet wait, `rN:KEY` repeat. |
| `VT3_TOUCH`, `VT3_TOUCH_FORCE`, `VT3_TOUCH_SCALE`, `VT3_TOUCH_ALPHA`, `VT3_TOUCH_COLOR`, `VT3_TOUCH_DEBUG` | On-screen touch controls (Android; `VT3_TOUCH=1` also draws them on the desktop for a layout check). See `docs/TOUCH_CONTROLS.md`. |

Ruled out this session (do not retry): swapped sin/cos (only `fsin`/`fcos` are lowered), the D3DX
matrix routines (match the documented formulas), `texldp` (handled), alpha-test scale (same in both
backends), vertex formats (all supported on MoltenVK), uniform descriptor ranges (exact block sizes
**black out the 3D scene**; keep `VK_WHOLE_SIZE`), the cull/front-face mapping (swapping it did not
help), stencil (the shim never enables it), bone palettes (identical on GL and Vulkan).

## Working with models

- **Claude subagents** (read-only Explore agents are cheap) for code reading. Verify what they say.
- **OpenCode free models:** `/Applications/OpenCode.app/Contents/Resources/opencode-cli run -m
  opencode/<model> --file <attachment> "<prompt>"`. Models that answered well: `deepseek-v4.1-flash-free`,
  `glm-5.3-flash-free`, `muse-spark-1.3-contributor-free`, `big-pickle`, `longcat-2.5-preview-free`,
  `mimo-v2.6-flash-free`. `nemotron-3-ultra-free` timed out (504). They mis-cite line numbers and
  sometimes propose fixes that contradict the code; use them for hypotheses, then check. Write the
  run into a script file and start it as a background job: a foreground call that times out kills
  its children, and the output files come back empty.
- **Antigravity** (`/Applications/Gemini.app/Contents/Helpers/agy --model gemini-3.8-flash-high
  --print='<prompt>'`): headless mode cannot read files or run shell commands, and
  `--dangerously-skip-permissions` is blocked here. Paste code into the prompt for review.
- Use sibling worktrees (`git worktree add ../vt3-port-x -b x main`) for long agent tasks so the
  build and capture paths still resolve. Worktrees `vt3-port-vk`, `vt3-port-audio`, `vt3-port-wip`
  and `.claude/worktrees/agent-*` are left over; delete with `git worktree remove` when done. The
  earlier uncommitted work is kept as `../wip-antigravity-2026-10-16.patch` (not for merging: races
  and a stale-object build bug).
- A stray `dis.py` in the Claude scratchpad shadows Python's `dis`; run helper scripts with `python -I`.

## Game facts worth knowing

- Textures come from relocated `.cog`/`.gog` packs (DDS loader `FUN_0040ba10`); outfits are chosen
  by pack index (`FUN_004094b0`), not tinted. Ghidra decompile: `../dump/recomp/vt3_nocd101_all.c`.
- Player cloth uses two vertex streams (UVs in 0, simulated positions and normals in 1).
- `loop aL, iN` bounds come from the shader's own `defi`; the draw record carries no integer constants.
- The streaming music buffer is 48 kHz, 6 channels, 16-bit, 128 ms; the game writes 70-100 ms ahead.
- Key shaders: skinned body VS `98f59399` (4 bones), 2-bone VS `2e3a60a2`, body PS `c62249b4`, cloth
  VS `3ee6cb60`, mask blur VS `1b40f853` / PS `c3a461ab`.

## Conventions

- Commits use the noreply identity (`191880570+hjcoggan@users.noreply.github.com`) and end with the
  Co-Authored-By trailer the session names. Push reviewed progress. Never commit game data.
- Builds take seconds: build inline. Long self-contained investigations go to subagents.
