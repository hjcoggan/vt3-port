# Cross-platform data formats and the CAPX name hash

Findings from the PC release, the Feb 2007 PSP UMD image, and the Dec 2006 PS3 prototype.
Nothing here contains game data. Dumps stay in `dump/`.

Sources: `dump/pc/extracted/data1` and `data2` (the two `Resource/` trees are byte-identical),
`dump/prototypes/feb2007/extracted/PSP_GAME/USRDIR/VT3/`, and `dump/prototypes/dec2006_ps3/`
(the first 2.4 MB of `vt3.xaf`, plus a name list derived from it).

## 1. CAPX name hash: solved

For an asset path, with the extension included:

1. Take the path, replace `/` with `\`, and upper-case it. Example: `RESOURCE\SOUND\SE\CRIAU\FOOT.CSB`.
2. Compute `h = sum(byte_i * 131^i) mod 2^32`, where `i` counts from the start of the string.
   Equivalently, Horner's rule run from the last byte to the first:
   `h = 0; for c in reversed(s): h = h*131 + c`.

The result is the record's `name_hash`. The reference implementation is
`tools/formats/crossplatform/capx_hash.py` (`capx_name_hash`), and `--selftest` checks it.

Checked against the archives:

| Archive | Entries | Names that hash-match |
| --- | --- | --- |
| PC `Sound/VOICE_ADX.xpac` | 3535 | 3535 |
| PC `Sound/KANSEI.xpac` | 113 | 113 |
| PC `Sound/SE.xpac` | 9 | 9 |
| PSP `RESOURCE/LANGUAGE.XPAC` | 488 | 488 (names from `LANGUAGE_XSCR.TXT`) |
| PC `frontend.xpac` | 1499 | 492 (union of PS3 names, PSP language list, EXE strings) |
| PC `Motion.xpac` | 353 | 353 (349 from PS3 names, rest from the EXE) |

### The index field

The fourth record field (`index_id`) is the entry's rank in the packer's list. Sort the paths
by the same upper-cased key used for hashing, and entry `i` is the path with rank `i`.
This holds for 3535/3535 voice, 113/113 KANSEI, 9/9 SE, and 488/488 PSP language entries.
The earlier "permutation" observation in `formats.md` is explained by this.

### How it was found

- The table is sorted by hash, and every archive's `index_id` is a permutation.
- The PS3 tree gives the same names as the PC voice archive, with the same counts (3535, 113, 9).
  Sizes match one-to-one for the voice files.
- Changing only the last digit of a name changes the hash by a fixed amount. That amount depends
  only on the name length, and it is multiplied by 131 for each extra character. That is the
  signature of a base-131 polynomial hash.
- The per-length residual became constant once `/` was replaced by `\`. Upper-casing the whole
  path then gave an exact match with no extra constants. The earlier lower-case fit
  (a seed of `0xa644c120` plus `0x0dca8ca0`) matched the same voice pairs. It only worked
  because lower-casing changes a fixed set of characters, so the constants absorb the
  difference. The upper-case form is the true input.

### Ruled out before the solution

These were tested over prefixes, extension removal and replacement, case, separators, and
length-prefixed and NUL-terminated inputs, against the voice set and the full PS3 name list
(6264 names; several million tests in total):

- Linearity test. Any CRC-32 (any polynomial, init, xorout, reflection) is GF(2)-affine in the
  input bytes. For same-length names, the hash difference from a one-byte change must depend
  only on position and XOR delta. This holds for zlib CRC-32 controls and fails for the voice
  names. So the hash is not CRC-family.
- Residual test. Hash minus a standard CRC-32 family value (added, subtracted, XORed or
  multiplied) is not constant across names.
- FNV-1 and FNV-1a (two bases), DJB2 and DJB2a, SDBM, ELF, AP, JS, RS, Java, Lua, Hsieh
  SuperFastHash, OAAT, Murmur2 and Murmur3 (seeds 0, 0x9747B28C, 0xFFFFFFFF), xxHash32,
  Jenkins lookup2 and lookup3, Adler-32.
- BKDR-131, -31, -1313, -13131 in forward order. The solved rule is the same polynomial run in
  reverse order, so the forward runs did not reach it.
- MD5, SHA-1, SHA-224/256/384/512, SHA3-256, BLAKE2b and BLAKE2s, truncated to 4 bytes
  (both endiannesses, first and last 4 bytes), and UTF-16LE inputs.

Hash sharing across platforms also rules out a content hash: PC and PSP `FRONTEND`/`frontend`
share 406 hash values, but the payload sizes differ for all 406. The hash is a name hash.

## 2. Cross-platform comparison

Every name below uses the same hash, so a name that exists on PC and PSP has the same record
hash in both.

| Archive | PC entries | PSP entries | Shared hashes | Same size |
| --- | --- | --- | --- | --- |
| frontend / FRONTEND | 1499 | 443 | 406 | 0 |
| frontend / LANGUAGE | 1499 | 488 | 488 | 466 |
| Motion / MOTION | 353 | 352 | 351 | 25 |
| game / GAME | 1174 | 821 | 0 | 0 |
| BGM / BGM | 131 | 93 | 0 | 0 |
| KANSEI / KANSEI | 113 | 113 | 0 | 0 |
| SE / SE | 9 | 9 | 0 | 0 |
| VOICE_ADX / VOICE_ADX | 3535 | 3535 | 0 | 0 |

The zero overlaps for voice, KANSEI and BGM come from the extension, not the name. PC and PS3 use
`.adx`. PSP uses `.AT3` (ATRAC3 in RIFF: the PSP payloads start with `RI`). With `.AT3`, the PS3
stems hash to 3535/3535 PSP voice entries, 113/113 PSP KANSEI entries, and 87/93 PSP BGM entries.
PSP `SE` and PSP `GAME` do not match any tested name form yet.

PS3 (Dec 2006) against PC, by name and by size:

- Voice: all 3535 names match, and the sizes match one-to-one.
- BGM: all 103 PS3 names have a PC entry of the same size; 99 names hash-match.
- KANSEI: all 113 names match, but only 31 sizes match, so the payloads differ between builds.
- Language: 482 names match, but only 20 sizes match.
- Motion: 349 names match.

## 3. PS3 `vt3.xaf0` (Dec 2006 prototype): file table

`vt3.xaf` (4.5 GB) starts with a file table. Only the first ~0.6 MB is needed to read it.

- Header: `xaf0` magic, `u32` version `0x00030001`, `u32` block size `0x800`, `u32` record count
  at `0x0C` (6339), folder count at `0x10` (75), file count at `0x14` (6264). The `u64` fields at
  `0x18`, `0x20` and `0x28` are not decoded.
- Records from `0x100`, stride `0x60`: name at `+0x00` (`0x40` bytes, NUL-padded), then `u32`
  type (0 folder, 1 file), parent index, next-sibling index, first-child index (`-1` for none),
  size, size (again), `id`, and `0`.
- The tree is linked by child and sibling indices. Walking it gives 6264 files and 75 folders,
  matching the header counts.
- Payload offset = `id * 0x800`. Confirmed on two PNG magics (`ICON0.PNG` at `0x95000`, and
  `ICON0_00.PNG` at `0x9F800`). The table ends at `0x94B20`, so the first file starts at sector `0x12A`.

The tree is the full PS3 `resource/` layout: `sound/{BGM,KANSEI,SE/CriAu,VOICE_ADX/...}`,
`model/...`, `motion/...`, `sprite/...`, `texture/...`, `language/...`, `database/...`, and
`contents/{gamedata,savedata}`. No `.xpac` archives appear in the tree, so the PS3 build keeps
its assets as loose files. The PS3 files are a Dec 2006 build, so some sizes differ from PC.

PSP `LANGUAGE_XSCR.TXT` is a plain-text packer list with upper-case backslash paths and a
build-machine prefix. It is the only list found that uses the packer's exact form.

## 4. Coverage

Names known from the PS3 tree, the PSP language list, and strings in `VT3.exe`. Counts are
distinct hash values.

| PC archive | Entries | Explained | Unexplained |
| --- | --- | --- | --- |
| game.xpac | 1174 | 0 | 1174 |
| frontend.xpac | 1499 | 492 | 1007 |
| Motion.xpac | 353 | 353 | 0 |
| stages.xpac | 359 | 1 | 358 |
| Sound/BGM.xpac | 131 | 103 | 28 |
| Sound/KANSEI.xpac | 113 | 113 | 0 |
| Sound/SE.xpac | 9 | 9 | 0 |
| Sound/VOICE_ADX.xpac | 3535 | 3535 | 0 |
| total | 7173 | 4606 | 2567 |

The EXE's literal path strings add names for frontend, Motion, BGM and stages. Its model paths
(for example `resource/model/player/P_M_HEN_game_all01_base_AN.efs`) do not match `game.xpac`.
Only `RESOURCE\STAGES\STAGEANIMS.MOO` matches `stages.xpac`.

## 5. Tools

All in `tools/formats/crossplatform/`:

- `capx_hash.py`: `capx_name_hash(path)`, `capx_key(path)`, `assign_indices(paths)`, and
  `--selftest`.
- `capx_xplat.py`:
  - `tables FILE...` checks CAPX headers and invariants.
  - `xaf FILE [--json OUT]` walks the PS3 `xaf0` tree.
  - `match ARCHIVE NAMES [--rank]` hashes a name list and checks the rank rule.
  - `shared A B` counts shared hashes and sizes.

## 6. Open items and next steps

1. Find the names for `game.xpac` (1174) and `stages.xpac` (358). The next places to look are
   the EXE's data sections after unpacking (not literal strings), and the PSP `EBOOT` once its
   decryption is solved. The port does not need the names: the runtime can hash any path it
   requests with `capx_name_hash`.
2. PSP `SE` (9 entries) and PSP `GAME` (821) use names outside the tested forms.
3. Decode the PS3 `xaf0` `u64` fields at `0x18`, `0x20` and `0x28`.
4. `formats.md` still says `name_hash` is undecoded and the fourth field is unknown. Both are
   now explained here. The `formats.md` owner should update those lines.
5. The QuickBMS script's `SOME_CRC` field is not a CRC-32 of the name. It is the reversed
   BKDR-131 hash above.
