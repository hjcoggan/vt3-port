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
- `name_hash` is decoded (see "CAPX name hash: decoded" below). `unknown` is the entry's rank when all names are sorted by that same key.
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

**Animation names and packs (Anims/, Player set verified)**

- `anim.db` (48-byte variant, used by Player, Ball, umpire, ballperson, Morph): `u32` count, then `count` records of 48 bytes. Name: null-terminated at record offset 14. `+0` u32 is `0x7FFFFFFF` in 813 of 1079 Player records (other values not understood). `+4` is a sequential index for records 0 to 565 and differs after that. `+8` has low 24 bits equal to k for records 0 to 565 (high byte seen as `01` in samples). `+12` is `00 FF`. `+44` is `0xCCCC0000`. Size check passes for all five skeletons.
- `Trophy/anim.db` is a different, variable-length variant (5 records, 164 bytes). Not parsed.
- `pack*.adb`: `u32` count, `u32` 8, then `count` `u32` entry start offsets (entry k starts at word 2+k). The last entry runs to end of file. Player packs hold 611 + 41 + ... = 1079 entries, equal to the `anim.db` count. Working hypothesis: `anim.db` record k is global pack entry k (packs in numeric order). Supported by the counts, not yet proven per clip.
- Entry header (first 36 bytes, samples only): byte0 = `07` (`06` once), byte1 = bone count. Equals the `hierachy` count for 1076 of 1079 Player entries and 15 of 15 ballperson entries. Byte2 = frame count (guess; for example `MYA_RUN_F_NAB` = 61). `u32` at +12 = `0x24` in every sample. Offsets at +16 and +20 are not understood. Keyframe encoding is not decoded.
- `hierachy`: `u8` bone count, then one parent byte per bone (`FF` = root). Player: 60 bones.
- PSP prototype cross-check: `PLAYER/ANIM.DB` is byte-identical to the PC file. `PACK0.ADB` has the same header, but its per-entry offsets differ.
- `tools/formats/anim_extract.py` writes `dump/extracted-en/anims/<skeleton>.json`.

**Motion.xpac (partly decoded)**

- 278 `0x12` payloads. Each starts with `u32` 18, `u32` 8, then 18 entry starts (the same container shape as the packs). 5004 entries in total.
- Entry header (all 5004 entries): byte0 = `02`. byte1 = bone count: 60 (4660 entries), 2 (278, the ball skeleton) and 67 (66). `u16` at +2 = `0x0082`. `u32` at +4 = `0x0C`, at +8 = `0xE0`, at +12 = `0x30000000`.
- Bytes `0x0C` to `0xF0`: a nibble table. Values 0 and 4 dominate (about 1.4 M and 0.57 M nibbles across all entries). Meaning not decoded. The pattern `0x44` repeated suggests a per-bone or per-channel code.
- `0xF8`: float 1.0. From `0xFC`, the 60-bone entries have 7 increasing `u32` offsets (for example `0x770, 0x93C, 0xB48, 0xD80, 0xE6C, 0xF70, 0x104C`), all inside the entry. Later there are more offsets interleaved with float parameters (0.5, -0.48, -1.16, 4.8). The records are not yet split.
- Pack indexing: the offsets point within the same entry (largest is about 0.38 of entry length). The nibble values are 0 to 15. No field matched a `pack*.adb` index (those run up to 1079), so there is no evidence that these entries index the pack files.

**Mesh table, `game.xpac` type `0x0D` (geometry decoded for entries 696 and 1080)**

- Four `0x0D` entries: 594, 696, 959, 1080 (the PSP build has the same table, at entry 223). Layout: `u32` count (13), `u32` total size, then 13 records of 40 bytes (32-byte name, `u32` offset, `u32` size). Offsets are relative to the payload start. Sub-blocks begin at `0x4C0`.
- Names seen: `O_ECO_*` props (winch, chain, meat, pile, alligator), `O_CMN_machine`, `O_PAN_*` (balloons, arrows, shadow).
- Sub-blocks contain 4x4 float matrices, name tables, nested DDS textures (`O_PAN_*`), and `u16` triangle index runs. Example: entry 696, block `O_CMN_machine` at payload `0x58140`, has a 902-value run after a `u32` `1`, maximum index 412.
- **Geometry blocks (entries 696 and 1080): decoded.** A block starts with `u32` 1, then a `u16` triangle list. The list ends at the first value `>= 0x2000`. Vertices start at the 16-byte boundary next to the list end (floor or ceiling, whichever scores better). Vertex layout, all little-endian:
  - `+0` float32 x, y, z (y up)
  - `+12` packed normal: 4 bytes, `n = byte / 127.5 - 1` for x, y, z (the fourth byte is 0)
  - `+16` float32 u, v (texture coordinate 0)
  - `+24` extra 8 bytes (stride 32) or 12 bytes (stride 36, seen only on `O_ECO_alligator`), not decoded
  - Stride 24 for most props and balloons, 32 for the winch bases and the machines, 36 for the alligator.
- Checks: for every block the packed normals are unit length (fraction 1.000) and face normals agree with vertex normals (fraction 1.000, so winding is CCW, matching glTF). Triangle counts are 300 for the machines, 2 for the shadow quad, 266 for the balloons, and 785 vertices with 943 triangles for the alligator.
- Index count: the list is 902 values for the machines, 300 triangles with 2 unused values. Reading as a strip gives facing agreement of about 0.5, so the list reading is correct.
- Visual check: a front-view wireframe of `O_CMN_machine` shows a ball machine (column, head unit, wheels).
- PSP cross-check: the PSP entry 223 `O_PAN_Shadow` contains the same positions (`-8.9858, 0.1123, -8.9858` and `8.9858, 0.1123, 8.9858`), so `+0` position is right. The PSP block uses a `SUMO` container and a different vertex packing, so the layout does not carry over directly.
- Header-style blocks (entries 594 and 959, and the PSP `O_PAN_*` blocks): no index list at block+4. The `0x4C0` header area holds matrices and a section table. The geometry offsets in that table are not decoded.
- Export: `tools/formats/mesh_extract.py` writes one GLB per entry (`dump/extracted-en/meshes/game_0696.glb`, `game_1080.glb`). Each file holds 13 meshes, with positions, normals, UV0, and indices. A JSON sidecar lists each block's stride and checks. Textures are not referenced yet.

**Player database (partly decoded)**

- `Resource/database/player/tns_player_tbl_ALL.bin`: 188-byte records, 25 of them. Name at offset 0 (for example `Roger Federer(CHE)`). Other fields not yet mapped. `tools/formats/db_extract.py` exports the raw words as JSON.
- `tns_racket_tbl.bin`: 136-byte records, 66 of them (period found by autocorrelation). Fields not yet mapped.
- `tns_swing_tbl_MEN.bin`: 220-byte records, 411 of them. Name at offset 0 (for example `MYA_TP_F3_H1_SIM`). 392 of the 408 distinct names match animation names in `Anims/*/anim.db`, so swing records are keyed by animation. `tns_swing_tbl_WOMEN.bin`: 220-byte records, 333 of them, with `MAK_*` names.
- `tns_playstyle_tbl_dataM.bin` (8928 bytes, all values plausible float32, record size unknown) and `tns_IKInfo_*.bin`: not decoded.
- `tools/formats/db_extract.py --record-size N` exports these as JSON (records, words, hex).

**Text**

- `ConfigText/English_US.TXT` is UTF-16LE with BOM and CRLF (confirmed). It contains the `Config.exe` UI strings. `tools/formats/text_extract.py` converts it to UTF-8.
- The game's own English strings were not found in the PC dump. The exe refers to `resource/language/*.bin`, which is not present. The PSP build's `GAME.xpac` includes a `VECT` container with plain text (`VECTOR:`, `BEGIN:`, `COLOR:`). PSP only.
- `.ips` files are InstallShield installer tables, not game text.

**Textures status**

- `game.xpac`: 2500 DDS (1406 DXT1, 3 DXT3, 1091 DXT5), 2500 PNG, 0 pitch mismatches.
- `stages.xpac`: 4217 DDS (2625 DXT1, 16 DXT3, 1487 DXT5, 87 numeric FourCC 113 float, 2 RGB32), 4130 PNG (float format has no PNG), 0 pitch mismatches.
- Output: `dump/extracted-en/textures/{game,stages,frontend}/`.

**Next**

- Mesh: map the header-style blocks (entries 594 and 959, PSP `O_PAN_*`) through their section table. Decode the extra 8 and 12 bytes at stride 32 and 36, and check whether the alligator is skinned.
- Motion: split the 7 offset records per 60-bone entry, decode the nibble table, and tie the 60-bone sets to Player animations (`anim.db`).
- Animation packs: decode the entry header offsets (+16, +20) and the channel data, checked against `hierachy`.
- Player table fields (188-byte records), the swing table fields (220-byte records, keyed by animation name), racket fields, playstyle record size, IK tables.
- Bulk ADX to WAV for `KANSEI`, `BGM` and `VOICE_ADX` (`tools/capx_extract.py --wav`).

## CAPX name hash: QuickBMS script cross-check (2026-10-14)

- The community QuickBMS script for CAPX (`virtua_tennis_xpac.bms`, from the QuickBMS script set) matches our layout. It reads the magic, the file count, a 0x10 header size, and the alignment. It then reads 16-byte records: a field it calls `SOME_CRC`, the offset, the size, and an `ID`. Our `tools/capx_extract.py` already reads the same fields.
- The script doesn't name files. It extracts with empty names, so it doesn't solve the hash.
- Tested: zlib CRC32, JAMCRC, CRC32-BZIP2, CRC32-MPEG2, and CRC32C. Each was run over about 3,600 printable strings from the first 128 KB of `SE.xpac`, in original, lowercase, and uppercase forms, with and without the path. None matched any of the 9 stored hashes.
- Next option: the hash may be seeded or use a different polynomial. Test it against names with known source, such as the cue names and file names inside the ACB sheets.

## CAPX name hash: decoded (2026-10-14)

- Key: take the asset path, change `/` to `\`, upper-case it, and keep the extension. For example `RESOURCE\SOUND\SE\CRIAU\FOOT.CSB`.
- Hash: `h = sum(byte_i * 131^i) mod 2^32`, indexing from the start of the key. Equivalently, Horner's rule over the reversed key with multiplier 131, seed 0, and no final add.
- `unknown` is the rank of the path among all names sorted by that key.
- Verified: VOICE_ADX 3535/3535, KANSEI 113/113, SE 9/9, PSP LANGUAGE 488/488, Motion 353/353, BGM 99/131 (the rest differ in name, not hash). An independent spot-check on SE.xpac agreed.
- Implementation: `tools/formats/crossplatform/capx_hash.py` (`capx_name_hash`, with `--selftest`). Findings: `docs/formats-crossplatform.md`.
- Unnamed so far: game.xpac (1174 entries), stages (358), frontend (1007). The runtime doesn't need names. It can hash any path directly.
- The QuickBMS-era CRC tests in the note above are superseded. The hash isn't a standard CRC.
