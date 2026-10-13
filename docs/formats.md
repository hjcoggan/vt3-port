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

## Progress note (decoding in progress, not final)

Shared code lives in `tools/formats/`: `capx.py` (archive reader), `cri_utf.py` (CRI `@UTF` tables).

**Done**

- **Textures, D3D9 DDS.** `frontend.xpac`, `game.xpac` and `stages.xpac` hold standard DDS files, whole or embedded in another resource (for example a `TPEX` block). `tools/formats/dds_extract.py` finds each `DDS ` magic and sizes it from the header, including mips. Checked on frontend: 598 files, all with pitch matching the header, and decoded PNGs look right. Not yet run on `game.xpac` or `stages.xpac`.
- **Sound effects, CRI Atom ACB.** `SE.xpac` payloads are ACB cue sheets (`@UTF` tables). Each sound element holds its ADX stream in a nested `AAX` blob, plus the original file name (`lnkname`) and cue names. `tools/formats/se_extract.py` gives 404 elements from 9 ACBs, decoded to WAV with ffmpeg (checked: 44.1 kHz mono). Uncertain: the cue-to-element chain is inferred from one archive.
- **Movies, CRI Sofdec.** Each `.sfd` has one MPEG-1 video (PES `0xE0`, 1280x720) and one ADX stereo track (PES `0xC0`). No second audio track or private substream exists, so there is no separate English track to select. `tools/formats/movie_convert.py` writes H.264 + AAC MP4. Converted: `vt3_a.mp4`, `ps3_a.mp4`.
- **Frontend container.** `frontend.xpac` payloads are chunked `RELO` blocks. Each has a 4CC type (`PTEX`, `FONT`, `TEXT`, ...), version 1, and the marker `11 22 33 44`. Meaning of the fields after the type is uncertain.
- **Model table, `game.xpac` type `0x0d`.** Layout: `u32` type (13), `u32` total size, then `count` records of 40 bytes (32-byte name, `u32` offset, `u32` size). Sub-blocks start with 4x4 matrices. Vertex and index layout is not decoded.
- **CAPX table.** The records are sorted ascending by `name_hash`. The fourth field is a permutation of 0..count-1.

**Name hash (not decoded)**

- Tested against loose files, `anim.db` names, path strings in `VT3.exe`, and four known BGM names (from loose ADX files whose sizes match). Tested: CRC32 (several variants), FNV-1/1a, Jenkins OAAT and lookup2, DJB2/SDBM/ELF/Java31, Murmur2/3 with several seeds, Adler32. No confirmed match. The few hits were consistent with random collisions.
- `VT3.exe` `.text` is packed (entropy 8.0), so constant scanning is blocked until it is unpacked.
- Names do exist in the data: `anim.db` (see below) and the SE cue sheets.

**Animation names (partly decoded)**

- `Anims/*/anim.db`: `u32` count, then `count` records of 48 bytes. The name is a null-terminated string at record offset 14. Size checks pass for 5 of 6 files (`Trophy/anim.db` does not match). Other fields are not yet understood.

**In progress**

- Mesh and skeleton layout in `game.xpac` and `stages.xpac` (the `0x0d` table, `hierachy` files).
- `Motion.xpac` animation layout (`0x12` payloads, `pack*.adb`).

**Next**

- Run DDS extraction on `game.xpac` and `stages.xpac`, and make a PNG pass.
- Mesh converter to glTF or OBJ, once the vertex layout is known.
- Decode `anim.db` fields and the `Motion.xpac` packs.
- Decode the database `.bin` and `.adb` tables, and the text tables.
- Bulk ADX to WAV for `KANSEI`, `BGM` and `VOICE_ADX` (existing `tools/capx_extract.py --wav`).
