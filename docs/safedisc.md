# SafeDisc on VT3.exe: mechanism and unpack status

Status: **not unpacked.** Under emulation the parent's `.text` stays encrypted. No decrypted image has been produced. The blocker is a helper DLL and a kernel driver, both needed to decrypt. Details below.

Scope: PC release, `dump/pc/VT3.exe`, PE32 i386, ImageBase 0x400000. VAs are given with that base. No game binaries or assets are in this file.

## Sections

| Section | RVA | Virtual size | Raw size | Entropy | Note |
| --- | --- | --- | --- | --- | --- |
| `.text` | 0x1000 | 0x2c77e6 | 0x2c8000 | 8.00 | Encrypted. Not decrypted at rest. |
| `PSFD00` | 0x2c9000 | 0x1df3 | 0x2000 | 7.79 | Protection data. Not examined. |
| `.rdata` | 0x2cb000 | 0x15fc39 | 0x160000 | 4.49 | Normal. |
| `.data` | 0x42b000 | 0x3b1d08 | 0x11c000 | 7.94 | Mostly zero; high entropy in part. |
| `.rsrc` | 0x7dd000 | 0xf6 | 0x1000 | 3.14 | |
| `stxt774` | 0x7de000 | 0x2044 | 0x3000 | 6.23 | Stub data. |
| `stxt371` | 0x7e1000 | 0x3452 | 0x4000 | 5.89 | Stub code. Entry point RVA 0x7e406e. |

The overlay starts at file offset 0x550000 and runs to the end of the file (6923818 bytes). It holds the payloads described below.

## Entry stub (parent process)

- The stub is self-modifying. It writes an `E9` jump over the entry point (`0xbe406e`) to choose between two paths. The selector is a byte at VA `0xbe400d` (RVA 0x7e400d), which is 0 in the file.
- It resolves about 40 kernel32 and user32 functions at run time by calling `GetProcAddress` with module handles. The names are decoded by a rolling-XOR routine at VA `0xbe1070`. The on-disk import table has 14 entries; the rest are resolved at run time.
- A decryptor-like routine at VA `0xbe3ce0` takes a descriptor at VA `0xbe3fd9` and calls back through function pointers it fills at run time. On failure it returns 1, and the stub then calls `ExitProcess` through a pointer at `0xbe3ff9`.
- Before any decryption, the stub does the following:
  1. Opens a named mutex (`XeTuM bOY759`).
  2. Gets `%TEMP%` (`C:\WINDOWS\Temp\`), creates a directory `~E5.0001.dir.0000` there, and checks `C:\WINDOWS`.
  3. Copies payloads out of its own overlay (`C:\Games\VT3\VT3.EXE`, reads at offsets 0x550000 and later) into the temp directory.
  4. Starts the launcher with `"<temp>\~E5.0001" <parent PID> "<temp dir>"`.

## Payloads in the overlay

| Written as | Overlay file offset | Size | What it is |
| --- | --- | --- | --- |
| `~E5.0001` | 0x5c8b6b | 72192 | PE32 launcher. Its PDB path says `cleanup`. It uses window and event APIs and receives the parent PID. |
| `PFDRUN.PFD` | 0x5da592 | 40960 | Data file, high entropy. Probably per-disc key and config material. Not decoded. |
| `~DF394B.TMP` | 0x5e45ba | 745500 | PE32 DLL-like helper, ImageBase 0x66700000, PDB name `SecServ`. Imports `ReadProcessMemory`, `WriteProcessMemory`, `VirtualProtect`, `DeviceIoControl`, `CreateFileMappingA`, `MapViewOfFile`. Its `.txt*` sections are high entropy, so it is packed too. Strings include `\\.\%s` and `\\.\Global\%s`. |
| `~EFE2.TMP` | (not written) | 0 | Opened for writing, never written in emulation. |

`SECDRV.SYS` (in `dump/pc/`) is the SafeDisc kernel driver. It has PDB `secdrv`, calls `IoCreateDevice` and `ZwMapViewOfSection`, and names `~df394b.tmp` and `secserv.dll` in its strings. Its `pnidata` section is high entropy, so it is packed as well.

## Chain (inferred from the strings and imports)

1. Parent stub unpacks the payloads above into `%TEMP%`.
2. The launcher is started and gets the parent PID.
3. The parent `LoadLibrary`s `~DF394B.TMP` into its own process. It then gets a function from it through `GetProcAddress`, using the name string `Ox12121212`.
4. The helper talks to `SECDRV.SYS` through `DeviceIoControl` on a `\\.\Global\<name>` device. The driver owns the decryption key material.
5. The helper writes decrypted code into the parent image. This step is inferred from the imports and was not observed.

The parent does not call `DeviceIoControl` itself. The driver is the only component that reaches kernel level.

## Key derivation

Not established.

- Whether the key depends on the physical disc is still open. The ISO is 2048-byte user-data sectors only (2623963136 bytes = 1281232 × 2048). SafeDisc normally keys off material outside user data (subchannel, lead-in, weak sectors). If the driver needs that, the ISO cannot supply it.
- `PFDRUN.PFD` and the driver's IOCTL answers are the next things to check.

## Emulation results

Harness: `tools/safedisc/emu_stub.py` (Unicorn 2.1.4, x86-32, arm64 host). It maps the image, sets up TEB/PEB and FS through a GDT entry, and stubs imports as hook slots that log calls.

- With `CreateProcessA` failing, the stub exits with `ExitProcess(1)` after the temp-directory checks. The stub checks its environment, so the harness needs a plausible `%TEMP%` and `C:\WINDOWS` (the virtual filesystem in the harness provides them).
- With `CreateProcessA` succeeding (default, `CHILD_OK=1`), the parent loads the helper DLL, gets its function, and reaches the original entry point at VA `0x654155`. The helper is not emulated, so its call returns 0.
- Result at the OEP: **zero writes to `.text`.** The decrypted fraction is 0%. `.text` entropy stays at 8.00.

## Wine (analysis route)

Blocked, and not installed.

- Rosetta 2 is not installed (`/Library/Apple/usr/share/rosetta` is missing). An x86 Wine build cannot run without it.
- The Homebrew Wine casks (`wine-stable`, `wine@devel`, `wine@staging`) are disabled because they fail Gatekeeper. The Gcenx tap is not added.
- Installing Rosetta needs administrator rights. Getting around the Gatekeeper block would mean overriding a security check. Neither was done. The owner needs to decide and do the Rosetta install, and decide whether to approve an unsigned Wine build.

## What is still missing

1. The helper DLL's entry export and its decryption routine, which is packed in turn.
2. The driver: its IOCTL codes and answers, and whether those depend on disc data.
3. A decrypted `.text` dump. Any dump before then would match the file on disk.
4. Import rebuilding. Hook slots would have to be replaced by real resolved addresses after dumping.

Recommended next step, in order:
- Run `emu_stub.py` with a CD-backed `D:` mapped to the ISO and log the `DeviceIoControl` requests the helper sends to `\\.\Global\...`. That shows whether the key needs disc data.
- Emulate the helper DLL with `ntdll`-level stubs for `CreateFileMappingA`, `MapViewOfFile` and `WriteProcessMemory`, so its writes into the parent image are captured.
- If the driver answers depend on disc sectors that the ISO lacks, the static route is closed and the owner needs to supply a raw disc image.

## Scripts

- `tools/safedisc/emu_stub.py`: Unicorn harness. Output of payloads goes to `dump/pc/safedisc/`.
- `tools/safedisc/disasm_range.py`: disassemble a VA range with capstone.

## Progress note (paused by owner)

Status is unchanged: no decrypted `.text`. Nothing was deleted, and no emulation run is in progress.

Done since the first write-up:
- Stub logic is read from the headless Ghidra decompile, `dump/pc/ghidra/vt3_packed_stub.c`. `FUN_00be3ce0` decodes two names through `FUN_00be1070`, resolves a function table, and copies the whole module into a `VirtualAlloc`'d buffer. It then calls an export of the helper on that copy. Zeroing the buffer and returning 0 is the success path to the original entry point.
- The helper `~DF394B.TMP` exports `Ox12121212` at RVA 0x44c2, which is the function the stub calls. It also exports `Ox12345678` (RVA 0x36118) and `_DllMain@12` (RVA 0x3246).
- The reference IOCTL `0xef002407` is present as plaintext in the helper (file offsets 0x59484 and 0x594bc). So is `0x98a64100`. The helper's code therefore looks readable, not fully encrypted.
- Static check of `SECDRV.SYS`: the driver contains `0xef002407`, `0x98a64100`, `0x5278d11b`, `0x56791283`, `0x587c1284`, `0x2c8` and `0xf367ac7f`. That is consistent with fixed answers, but it does not prove the answers are static. Not yet tested at run time.
- Emulation harness (`tools/safedisc/emu_stub.py`) now loads the helper as a real PE at its preferred base 0x66700000, with relocations, imports and export lookup. It handles the device-open and CD-handle cases, logs `DeviceIoControl` calls, and logs disc reads. It also writes `parent_image_at_stop.bin` and reports how many `.text` bytes changed.

In progress:
- The run gets past `GetProcAddress` and into the helper. It then calls `LoadLibraryA("Kernel32.dll")`, which returns a fake handle (0x10010000). The helper then reads memory at 0x10030000 and faults. This is the current blocker.

Next:
- Give system DLLs a real module record, or change the handle scheme so the helper can read from them.
- Once the helper runs, log its `DeviceIoControl` codes and compare them with the reference values. The fixed-answer test needs the real request layout, which is not yet confirmed.
- Log any disc reads, to see whether the helper or driver touches the ISO or a raw CD device.

## Progress note 2: Wine run with an analysis shim

Status: still not decrypted. `.text` is byte-identical to the file in every snapshot. Work is in `dump/safedisc-shim/wineshim/` (vt3shim.c, version.def), outside the repo. It is not committed.

What works under Wine 11.18 (Rosetta, analysis only):
- Device open: ntdll hooks redirect `\\.\SecDrv` / `\\.\Global\SecDrv` to a decoy handle (duplicated from one cached handle).
- The helper sends its driver requests. `0x3e`, `0x3c` and `0x3f` get replies. The copier (`0x6675aa10`) and validator (`0x6675a730`) pass, and the copier returns `0x6e`.
- Validator inputs: one argument, the output buffer (block + 0x514). It reads version major/minor/patch, and a gate `0x6675a6d0` (IsWow64Process) selects a branch. With version 4.00.48 or 4.3.86 it returns success. Failure returns 0, so the copier returns `0x6f`, and the wrapper `0x6675aa90` returns 0. The copier's timing check fails with `0x70` when `KUSER[0] - stored > 0x190`. Neither check fires in these runs.

Blocker: the helper's user-mode anti-debug probes. After the `0x3f` loop (indices 0 to 0x60) the helper runs the probe at `0x66708410` inside an SEH frame. The probe executes `SIDT`, then reads and writes through the IDT base, then `CLI`, `MOV DR0..DR6` and `DIV 0`, and expects the exception to be dispatched through `0x66781328` to `0x6676c483` and a table at `0x6679b4d4`. Under Wine/WOW64 the read of the IDT base at `0xfff88190` is an unhandled page fault. Wine's crash debugger then waits, which looks like a stall but is a crash. Tracing SEH with Wine's `+seh` channel shows only the stub's own unwinds.

Driver answers, re-derived from `SECDRV.SYS`:
- `0x3c` returns DR7 & 0x500 (`0x400` by default). Input extra size must be 0.
- `0x3d` returns the constant `0x2c8`. Input extra size must be 0.
- `0x3e` returns `0x5278d11b`. Input extra size must be 0.
- `0x3f` returns the IDT entry for an index (0 to 0x60). Input extra size must be 4. This build computes real IDT entries, unlike the reference's fixed 0.
- `0x43` needs input extra size 0xC, and returns DR values by sub-command.
- The common tail writes version 4.00.48, and the verification words come from a time-based source (`[0x12be0]`) with `ntoskrnl` randomness.

Next steps:
1. Patch or emulate the probe function `0x66708410` in-process so it does not fault, or find why the dispatcher table does not resume execution.
2. Try a Wine prefix without WOW64, to see whether the unwinder behaves differently.
3. Once the helper passes the probe, check the `0x40` to `0x43` replies and then watch for `.text` writes.

## Closing note: decryption route retired

The Wine-based unpacking route is retired, on the owner's decision. The owner also asked that no more decryption runs be made and that the anti-debug probe not be neutralised.

Why:
- Under Wine/WOW64, the helper's user-mode anti-debug probe faults on the IDT-base read and the fault is not caught. The game then crashes and Wine's crash debugger waits. Getting past it would mean neutralising the probe, which the owner has ruled out.
- Without the probe passing, no decrypted `.text` was obtained. `.text` stayed byte-identical to the file in every snapshot.

Replacement: a NoCD 1.0.1 build of `VT3.exe` has been found, with no SafeDisc stub and plain `.text`. It takes the place of the decryption step. Its path is `dump/patches/Virtua-Tennis-3_NoCD_Win_EN_for-version-101/VT3.exe`. It was not inspected or modified in this work.

Status of the artefacts from this route:
- The analysis shim source is kept outside the repo, in `dump/safedisc-shim/wineshim/`. Its answer logic was derived partly from SafeDiscShim, which the owner approved for analysis. It was moved out of the repo on the owner's decision.
- The static analysis, the Unicorn harness (`tools/safedisc/emu_stub.py`), the disassembly helper and the snapshot dumps in `dump/pc/safedisc/` remain as records of the work.
