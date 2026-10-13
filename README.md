# Virtua Tennis 3 port

Goal: a native port of the **PC release** of Virtua Tennis 3 (2007, Windows, Direct3D 9, English), running first on the Apple Silicon Mac, then on an Android Snapdragon 8 Gen 2 handheld in 4:3 at native resolution, with a controller.

## Status

| Area | State |
| --- | --- |
| Original game files | Local only, under `Virtua Tennis 3/dump/` (not in this repo) |
| PC release identified | Yes: `VT3.exe`, Windows PE (i386), Direct3D 9 |
| Game data (assets) | Mostly readable. CAPX archives and InstallShield cabs extracted locally |
| Audio | CRI ADX confirmed. Decodes to WAV with ffmpeg (checked on one clip) |
| Game code | **Blocked.** `VT3.exe` is SafeDisc-protected (see below) |
| Arcade (Lindbergh) build | Not readable. Disk images are encrypted-looking from the first byte |
| Port runtime | Skeleton builds and runs on macOS (SDL3, GL 3.3, ADX audio). See `runtime/` |

## Blocker: SafeDisc on `VT3.exe`

`VT3.exe` has the usual SafeDisc signature:

- `.text` is effectively random (entropy 8.00/8 bits): the game code is encrypted on disk.
- The import table lists only a handful of functions. The real imports are resolved at runtime.
- The entry point is inside an `stxt371` section, which is SafeDisc's loader stub.

So the game's logic cannot be analysed or recompiled from the file on disk. Getting at it means running SafeDisc's loader and dumping the decrypted image, or emulating that loader. The owner has approved this for the research project. Decisions and the legal caveat are in [docs/decisions.md](docs/decisions.md).

## Repo layout

- `tools/capx_extract.py` lists and extracts `.xpac` (CAPX) archives. It can convert ADX entries to WAV.
- `docs/formats.md` records the data formats found so far.
- `docs/decisions.md` records open decisions for the owner.

## Data policy

- Game files are never committed. `.gitignore` excludes ISOs, CABs, executables, DLLs, and extracted assets.
- Only English content is extracted. Other languages are skipped.
- Game files stay on the local machine.

## Environment

- Analysis venv: `Virtua Tennis 3/tools/venv` (pefile, capstone, lief, pycdlib). Outside the repo.
- Needs `ffmpeg` (WAV conversion) and `unshield` / `cabextract` (InstallShield cab extraction).
