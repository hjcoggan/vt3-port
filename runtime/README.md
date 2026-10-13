# vt3rt: native runtime skeleton

The runtime that the recompiled Virtua Tennis 3 (PC) will run on. Plain C11,
built with CMake, SDL3 for the platform layer. It contains no game code and
no game data. Nothing runs under Wine, Winlator, or Proton.

Design note: [../docs/runtime-plan.md](../docs/runtime-plan.md).

## Build (macOS, Apple Silicon)

Needs Xcode command-line tools, CMake, and SDL3 from Homebrew:

    brew install sdl3          # 3.4.x; already installed on this Mac
    cd runtime
    cmake -B build
    cmake --build build        # zero warnings with -Wall -Wextra

## Run

    ./build/vt3rt                          # 640x480 window, 1 s 440 Hz tone, Esc quits
    ./build/vt3rt --window 1280x960 --integer
    ./build/vt3rt --logical 800x600 --aspect 4:3 --frames 300 --no-audio

| Option | Meaning |
| --- | --- |
| `--window WxH` | Window size in points. Default 640x480. |
| `--logical WxH` | Game resolution. Default 640x480. |
| `--aspect W:H` | Display aspect for letterboxing. Default 4:3. |
| `--integer` | Whole-number scaling of the logical buffer where it fits; fractional otherwise. |
| `--frames N` | Exit after N frames. Default: until quit. |
| `--no-audio`, `--no-vsync` | Turn off audio output or vsync. |
| `--game-root PATH` | Directory that Win32 paths resolve against. |

The window clears to `0xFF1B3A4B` (ARGB) and draws one gradient quad. This
checks the GL path, not the game.

## Self-test

    ./build/vt3rt --selftest                                  # unit checks only
    ./build/vt3rt --selftest "<path>/KANSEI.xpac"             # plus ADX check
    ctest --test-dir build                                    # unit checks; ADX reported as skipped

The unit checks cover letterbox maths, the ring buffer, the guest heap
(including coalescing and realloc), Win32 path mapping (case-insensitive,
with backslashes), and the ADX predictor coefficients. The ADX check takes
a raw `.adx` or a CAPX `.xpac` (its first ADX entry). It decodes the stream
and checks that the frame count before the end matches the header, that the
streaming and whole-buffer decoders agree, and that the duration matches the
header. The archive is read from wherever you point it. Nothing is copied
into the repo.

Exit codes: 0 pass, 1 fail, 77 unit checks passed but no ADX path given
(ctest reports SKIPPED).

## Interface layouts

The COM vtables in `shim/include/win32/` were generated from the mingw-w64
14.0.0 headers (`d3d9.h`, `dsound.h`, `dinput.h`), not typed from memory. Two
checks guard them:

- `tests/layout_check.c` is built into `vt3rt`. It asserts every slot offset
  (slot N at byte offset 4*N, guest pointers are 32-bit) and the packed
  WAVEFORMATEX size.
- `tests/sdk_layout_check.c` is compiled against the real SDK. It asserts
  every slot of the SDK vtables, 35 header constants, and the WAVEFORMATEX
  size. Run it with the 32-bit mingw compiler:

        i686-w64-mingw32-gcc -c -I"$(i686-w64-mingw32-gcc -print-sysroot)/i686-w64-mingw32/include" \
            tests/sdk_layout_check.c -o /dev/null

  It exits 0 when every assertion holds. Changing a slot order or constant
  fails it at compile time (a deliberately swapped slot was confirmed to fail).

Not checked by these: kernel32 prototypes (the guest ABI uses 32-bit pointers
on purpose, so the signatures differ on the type level), and the winmm
prototypes, which were compared by eye against `timeapi.h`.

## ADX decoder: how it was checked

`src/audio/adx.c` is written from the public format description (header
layout and the predictor in the multimedia.cx and Wikipedia descriptions).
It is not a copy of any third-party decoder. The sources disagree on two
details, so the choices below were made by comparing against ffmpeg's
`adpcm_adx` output, bit for bit:

| Choice | Result |
| --- | --- |
| 12-bit fixed-point coefficients (14-bit is the other candidate) | 14-bit mismatches on every file tested. 12-bit is used. |
| Round coefficients to nearest (floor is the other candidate) | Floor mismatches on all four 44.1 kHz streams. Nearest is used. |
| Scale used as-is (`+1` is the other candidate) | `+1` mismatches on every file tested. Scale is used as-is. |

Final check: **321 streams** (all 113 KANSEI entries plus 208 VOICE_ADX
entries, every 17th, covering 48 kHz and 44.1 kHz), **0 mismatching sample
values**. ffmpeg pads its output to whole 32-sample blocks. This decoder
trims to the header's `total_samples`. The comparison harness ran from a
scratch directory outside the repo and is not committed.

Scope: encoding type 3 only, 18-byte blocks, 4-bit samples, 1 to 8 channels.
Type 4 (exponential scale) and type 2 (fixed coefficients) return
`VT3_ADX_ERR_UNSUPPORTED`. Output is S16.

## Layout

    runtime/
      CMakeLists.txt
      include/vt3/          public headers for the runtime
        vt3_adx.h           ADX decoder
        vt3_audio.h         ring buffer, audio output (DirectSound analogue)
        vt3_guest.h         guest address space (32-bit guest pointers)
        vt3_input.h         game-side buttons and axes
        vt3_log.h           logging
        vt3_platform.h      window, GL context, events, config
        vt3_renderer.h      renderer front end, letterbox, backend table
      src/
        main.c              entry point, argument parsing, demo loop
        core/               log.c, guest.c
        platform/           platform_sdl.c (SDL3 window, GL 3.3 context, events)
        render/             renderer.c (front end), gl33.c (OpenGL 3.3 core backend),
                            letterbox.c
        audio/              adx.c, ringbuf.c, audio_sdl.c
        input/              input.c (state), input_sdl.c (SDL to game mapping, STUB table)
      shim/
        include/win32/      types.h, kernel32.h, winmm.h, d3d9.h, dsound.h, dinput.h
                            (COM layouts checked against the mingw-w64 SDK headers)
        include/vt3shim/    shim.h (init, game root)
        src/                shim_core.c (handles, errors, guest helpers),
                            kernel32_heap.c, kernel32_file.c, kernel32_time.c,
                            d3d9_stub.c, dsound_stub.c, dinput8_stub.c
      tests/
        selftest.c          unit checks and ADX check
        layout_check.c      compile-time vtable layout check (built into vt3rt)
        sdk_layout_check.c  the same checks against the real mingw-w64 SDK (run by hand)

## What is implemented, and what is stubbed

| Area | State |
| --- | --- |
| Window, events, quit | Implemented (SDL3). Esc and window close quit. |
| GL 3.3 core backend | Implemented: logical-size FBO, letterbox blit (nearest for integer, linear otherwise), textured and untextured triangles, A8R8G8B8 / X8R8G8B8 / A8 textures. Context reports GL 4.1 (Metal) on this Mac. |
| Renderer front end | Implemented: backend-neutral; the Android GLES 3.0 backend will slot in as a second `vt3_gfx_ops`. |
| Audio output | Implemented: S16 ring, SDL pull callback, sine generator, underrun counter. |
| ADX decoder | Implemented for type 3, verified against ffmpeg (see above). |
| Gamepad and keyboard | Implemented with a **placeholder** button table (see TODOs in `input_sdl.c`). |
| Guest memory | Implemented: 1 GiB reservation, guest address = offset, image base 0x00400000, heap at 0x20000000. |
| kernel32 heap | Implemented: first-fit, coalescing, realloc. Linear search (TODO). |
| kernel32 file I/O | Read-only: CreateFileA (OPEN_EXISTING only), ReadFile, SetFilePointer, GetFileSize, CloseHandle. Case-insensitive per path component. WriteFile and overlapped I/O return "not implemented". |
| kernel32 timers | Implemented: GetTickCount, timeGetTime, QueryPerformanceCounter/Frequency, Sleep. |
| Direct3D 9 | **Sketch only.** `d3d9.h` has the IDirect3D9 (17) and IDirect3DDevice9 (119) vtables, checked slot by slot against the SDK. `Direct3DCreate9` logs and returns NULL. |
| DirectSound | **Sketch only.** `dsound.h` has IDirectSound (11), IDirectSoundBuffer (21), and WAVEFORMATEX (packed, 18 bytes), all checked against the SDK. `DirectSoundCreate` returns DSERR_NODRIVER. |
| DirectInput 8 | **Sketch only.** `dinput.h` has IDirectInput8A (11) and IDirectInputDevice8A (32), checked against the SDK. `DirectInput8Create` logs and returns E_NOTIMPL. |
| Android | Not started. |

Unimplemented shim calls log a warning once per API name (`vt3s_unimpl`).

## TODOs

- `input_sdl.c`: the whole button table, the keyboard fallback, and the stick deadzone. Match them to VT3.exe's DirectInput usage once the code is recovered.
- `gl33.c`: map D3DRS_* render states to GL blend, depth, and cull state. Only standard alpha blending is set. Add subrectangle uploads (`UpdateTexture`) and DXT formats.
- `kernel32_heap.c`: hash or size-class lookup. `HeapDestroy` does not return regions to the arena.
- `shim_core.c`: the handle table and `g_last_error` are not thread-safe. Only the heap has a lock.
- D3D9: vtable thunks in guest memory, the rest of the vertex formats, and a `Present` that calls `vt3_gfx_present` and `vt3_platform_swap`.
- Audio: a DirectSound buffer adapter over `vt3_ringbuf`, and an output format check against the game's `SetFormat`.
- Android: a GLES 3.0 backend, the SDL3 Android entry point, and a controller map for the Retroid.
