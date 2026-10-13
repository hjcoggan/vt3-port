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
