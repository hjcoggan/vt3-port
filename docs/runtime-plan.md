# Runtime plan

Status as of 2026-10-14. Code: [runtime/](../runtime/). Build and usage: [runtime/README.md](../runtime/README.md).

## Fixed decisions

- **Approach:** static recompilation of `VT3.exe` (x86 to C), plus a Win32 and D3D9 API shim in native C. Nothing runs under Wine, Winlator, or Proton at runtime. The Wine-style run stays analysis-only.
- **Platform layer:** SDL3 for the window, events, gamepad and keyboard, and audio. Homebrew `sdl3` 3.4.x. The SDL2 fallback is not needed.
- **Graphics:** a renderer abstraction. The first backend is OpenGL 3.3 core on macOS, using an SDL GL context. The Android backend will be OpenGL ES 3.0 and is not written yet.
- **Audio:** an abstraction for the DirectSound calls. The shim feeds PCM. The ADX decoder is plain C, written from the public format description.
- **Input:** SDL gamepad buttons map to a game-side button set. The table is a stub.
- **Resolution:** 4:3. The window and the logical size are configurable. Both default to 640x480. The renderer letterboxes, with integer or fractional scaling.

## Decisions I made

1. **Guest memory.** One reservation of 1 GiB (`MAP_NORESERVE`). A guest address is an offset into it, so the PE image base 0x00400000 is a single add. The shim heap starts at 0x20000000. Guest address 0 is NULL.
2. **Shim ABI.** Pointers are 32-bit guest addresses (`GPTR`). HANDLEs are small non-zero ids from a table, never host pointers. `WINAPI` is empty: the recompiler marshals the x86 stdcall stack into plain C calls. No `<windows.h>`.
3. **Heap.** Host-side block metadata, first-fit, coalescing. The guest cannot corrupt the allocator's state.
4. **File I/O.** Win32 paths map onto a game root. Matching is case-insensitive per component, because the data was built for a case-insensitive filesystem and Linux or Android are not.
5. **Renderer.** The game draws into a logical-size offscreen target. `present` letterboxes that target into the drawable (black bars) and blits it. Integer scaling uses nearest. Fractional uses linear. The backend gets only the destination rectangle, so the front end holds all the letterbox logic.
6. **Vertex format.** The shim path starts with XYZRHW + DIFFUSE + TEX1 only (28-byte `vt3_tlvertex`), which matches D3D9's pre-transformed format. Arbitrary FVFs come later.
7. **Audio path.** Single-producer, single-consumer byte ring (C11 atomics). SDL pulls from it in a callback. Underruns come out as silence and are counted. The ring is the DirectSound secondary buffer: Lock/Unlock writes into it, and GetCurrentPosition reads the played byte count.
8. **ADX constants.** 12-bit fixed-point predictor coefficients, rounded to nearest, with the 15-bit scale used as-is. The output is trimmed to the header's sample count. Checked bit-exact against ffmpeg on 321 streams (113 KANSEI and 208 VOICE_ADX, including 44.1 kHz). The sources disagree on these three points, so the choices come from the comparison. Details in the README.
9. **OpenGL headers.** Types and enums come from the macOS SDK's `OpenGL/gl3.h`. Function pointers come from `SDL_GL_GetProcAddress`, so the binary does not link the OpenGL framework. An X-macro table drives the loader. The GLES backend can reuse the table pattern.
10. **Input placeholder.** The default mapping is Xbox-style. It is only there so the runtime is usable. The real mapping will come from what VT3.exe asks DirectInput for.

## Open questions

- **Recompiler and import calls.** How recompiled code reaches shim functions. A thunk per import at fixed guest addresses seems right, but the import table is nearly empty because of SafeDisc (see [safedisc.md](safedisc.md)). Import names may have to come from the unpacked image or from runtime observation.
- **COM objects.** D3D9 and DirectSound are COM. Their vtables need guest-memory slots that point at thunks. Who owns the thunk region, and how do host objects map to guest `this` pointers? The shim sketches assume this but do not build it.
- **Is the ADX decoder needed?** VT3 may decode ADX in its own code, through CRI middleware compiled into `VT3.exe` (translated by the recompiler), or in a DLL. The shim would only need the decoder if CRI's code is replaced at a call boundary. This decides whether `adx.c` is used at runtime or only in tests.
- **Memory size.** Is 1 GiB enough for VT3? The PE image size, stacks, TLS, and any large buffers need checking. If it needs more, the 32-bit guest space allows up to 4 GiB, but the heap base may need to move.
- **Render states.** The game's D3DRS and sampler state need mapping to GL state. Also D3DPOOL and device-lost behaviour, DXT texture formats, D3DX calls, and the vsync and `Present` timing the game expects.
- **Audio format.** The game's DirectSound format (rate, channels, bit depth) is unknown until the binary is read. The ring currently supports S16 only.
- **Input.** DirectInput8 offsets and default bindings, and whether the game uses a device enumeration path that needs a fake device list.
- **Timers.** Whether the game paces frames from `timeGetTime`, `QueryPerformanceCounter`, or `Sleep`. These are monotonic here. If the game assumes a particular tick rate, that needs matching.
- **Threads.** The shim's handle table and last-error are not thread-safe. The heap is locked. The game's threading model is not known yet.
- **Android.** SDL3 on Android, GLES 3.0 with an EGL context, the audio latency budget, the Retroid controller's button layout, and whether 4:3 fills the handheld's screen or needs integer scaling to 1920x1440.
- **Licensing.** The runtime uses SDL3 (zlib) and no GPL code. The ADX decoder is written from the public description. If any GPL reference code gets used later, it needs flagging here, as the decisions log asks.
