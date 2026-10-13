# Decisions

## 2026-10-13: Scope and target

- **Source:** the PC release of Virtua Tennis 3 (Europe disc, Windows, English). The Xbox 360 and PS3 copies were not in the supplied files.
- **Target order:** macOS arm64 first, since it is also ARM and quicker to iterate on. Then Android arm64 on the Retroid handheld (Snapdragon 8 Gen 2), 4:3 at native resolution, with a standard controller.
- **Arcade (Lindbergh) build:** reference only. Not started.

## 2026-10-13: Reverse engineering `VT3.exe` (SafeDisc)

- The owner has approved reverse engineering the SafeDisc-protected executable as part of this research project. The copies are the owner's, for personal use.
- Dumps, unpacked binaries, and extracted assets stay on the local machine, outside this repo. None of them is committed or distributed.
- Circumventing copy protection can be restricted by local law. The owner is responsible for that judgement in their jurisdiction.

## Working rules

- Game data never goes into git. `.gitignore` covers ISOs, CABs, executables, DLLs, and extracted assets.
- Only English content is extracted and documented. Other languages are skipped.

## Analysis versus runtime (owner clarification)

- Compatibility layers (Wine, CrossOver, and similar) are allowed on the Mac for analysis only. Examples: running the original game to dump decrypted memory, tracing SECDRV IOCTL traffic, or observing behaviour.
- The shipped port must be native arm64 on both macOS and Android. No Wine, Winlator, or Proton at runtime.
- Decompiled code is written from our own analysis. Third-party code is not copied into the repo. The GPL-3 SafeDiscShim source was reviewed as a protocol reference only.
- REA's Ghidra provider needs `GHIDRA_INSTALL_DIR` in the MCP server environment. Ghidra headless needs `JAVA_HOME` set to JDK 21.

## 2026-10-14: Clean-room relaxed; prototype builds

- The owner has relaxed the clean-room requirement for the SafeDisc work. This is a private research project, nothing is distributed, and the games are owned. Reference code, including SafeDiscShim (GPL-3), may be used for analysis. Any GPL code that ends up in the repo must be flagged here before a public release. None is public at present.
- The owner donated two prototype archives. Neither is PC.
  - Feb 21, 2007: a PSP UMD image (`ULUS-10246`, `PSP_GAME`). Its game data uses the same CAPX, ADB, and anim.db layouts as the PC release. Its PRX files are encrypted (`~SCE`), so they give no symbols.
  - Dec 16, 2006: a PS3 build (`PS3_GAME`, `vt3.xaf` at 4.5 GB, `PS3UPDAT.PUP`). It's not yet analysed.
- Neither prototype is useful for PC symbols directly. They are useful as cross-platform references for the data formats.

## Decisions made while the owner was asleep (2026-10-14)

- **Build target:** the NoCD 1.0.1 VT3.exe. The SafeDisc route is retired.
- **One Win32 shim:** `runtime/shim/` is the only implementation. The recompiler's `tools/recomp/rt` links against it, and its duplicate imp_* functions are folded in.
- **Function discovery:** Ghidra's list (13,013 functions, 0 failures) seeds the recompiler, instead of direct-call discovery (1,029 functions).
- **D3D9 scope:** taken from a Wine trace of the NoCD build, logged with `WINEDEBUG=+d3d,+d3d9,+d3dx9`. The renderer implements only that subset.
- **Analysis runs:** the NoCD game was stopped after the save, so it doesn't hold the display.
- **Renderer scope:** the game's shaders are all Shader Model 2 (587 compiled, 352 ps_2_0 and 235 vs_2_0; see `docs/d3d9-shaders.md`). The D3D9 layer translates SM2 bytecode to GLSL for the GL 3.3 backend. SM1 and SM3 are out of scope.
- **Shim ownership:** the recompiler agent owns `runtime/shim/src/kernel32_*` and the import-to-shim merge. The runtime agent owns D3D9, D3DX, DirectSound, DirectInput, the user32 window code, and the SM2 translator.
