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
