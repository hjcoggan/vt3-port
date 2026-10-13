# Data formats (PC release)

Findings from the locally extracted PC disc. Nothing here is a copy of game data.

## Disc layout

- The Europe disc is an ISO9660 image with Joliet. The root holds `VT3.exe`, `setup.exe`, `DirectX9c/`, `data1.cab`, `data2.cab`, and `engine32.cab`.
- `data1.cab` and `data2.cab` are InstallShield archives, not plain CAB files. Unpack them with `unshield`. Together they hold 258 files, about 8 GB.
- The game content lives under `Resource/` in those archives.

## CAPX archives (`.xpac`)

Layout (little-endian):

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | magic `CAPX` |
| 0x04 | 4 | entry count |
| 0x08 | 4 | header size (0x10) |
| 0x0C | 4 | alignment (0x800) |
| 0x10 | 16 × count | records: `name_hash`, `offset`, `size`, `unknown` |

- Every offset is 2048-byte aligned and within the file.
- `name_hash` is not decoded yet. `unknown` is a small integer, not yet decoded.
- Payload kinds seen so far (by first two bytes):

| Archive | Entries | Payloads |
| --- | --- | --- |
| `Sound/KANSEI.xpac` | 113 | ADX |
| `Sound/BGM.xpac` | 131 | ADX |
| `Sound/VOICE_ADX.xpac` | 3535 | ADX |
| `Sound/SE.xpac` | 9 | magic `4055` (not yet decoded) |
| `game.xpac` | 1174 | mostly `0100`, plus a few others |
| `frontend.xpac` | 1499 | mostly `5245` and `5054` (`RE`/`TP`?), plus `0100` and `0200` |
| `Motion.xpac` | 353 | mostly `1200` |
| `stages.xpac` | 359 | mostly `0100` |

Run `tools/capx_extract.py --list <archive>` to reproduce these counts.

## Audio (CRI ADX)

- ADX streams start with `80 00`. They are 48 kHz stereo in the sampled entries.
- Decodes cleanly with ffmpeg (`-i x.adx x.wav`). Checked on one 2.84-second clip.
- Each CAPX entry is one standalone ADX stream.

## Video (CRI Sofdec)

- `.sfd` files are MPEG program streams (`00 00 01 BA`). Four files, about 687 MB in total.

## Other files

- `.adb` (70 files): binary, not yet decoded.
- `.db`, `.ini`, `.txt`, `.ips`, `.rgs`, `.tlb`: mostly config and string-table text. `.txt` is UTF-16LE with BOM.
- `.bmp`, `.ico`, `.pal`: standard. `.pal` is RIFF.
