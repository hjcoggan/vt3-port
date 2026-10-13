# Lindbergh arcade disc: progress note (paused)

Status: paused by owner request at a safe point. No decryption achieved. Nothing deleted.

## Done

- Disc layout mapped (2048-byte disc sectors, image is 884752 sectors):
  - 0..31: 32 distinct random-looking sectors (64 KiB).
  - 32..1375: 1344 copies of one random sector (filler).
  - 1376..1887: ISO A, system ID SEGA_LINDBERGH, volume ID POWERSMASH3, 512 sectors. PVD at 1392 (note: the earlier note said 1376; 1376 is the ISO start). Root has no files. Data sectors are high-entropy and unreferenced.
  - 1888..1891: plaintext "BTID" header sector plus three sectors (see below).
  - 1892..1919: zeros.
  - 1904..884751: ISO B, system ID LINUX, volume CDROM. PVD at 1920, Joliet SVD at 1921, Rock Ridge ER at 1934. Its metadata is plaintext.
  - 1935..884314: five files, contiguous:
    - DISK0.IMG 1775316992 bytes, sectors 1935..868788 (last sector zero)
    - DISK9.IMG 372736 bytes, sectors 868789..868970 (last sector zero)
    - FRONTEND.SET 859 bytes, sector 868971
    - SU1.DAT 31092053 bytes, sectors 868972..884153
    - SU2.DAT 328302 bytes, sectors 884154..884314
  - Each file's tail sector is zero padding. Sectors 884315..884751 are zeros.
- Encryption evidence:
  - Every payload is about 8.0 bits/byte. No gzip, xz, zip, 7z, ELF or ext magic at 3 or more bytes.
  - DISK0.IMG: 16-byte blocks repeat only at 512-byte multiples. In the first 256 MB there are 93 intra-sector pairs and 338 cross-sector repeats, all at 512-byte multiples; 243 are at the same in-sector offset. No duplicate sectors. This suggests a deterministic block transform with no per-sector tweak and a 512-byte period. It is not proven.
  - SU1.DAT and SU2.DAT: no repeated 16-byte blocks at any of the 16 alignments. Not ECB-like.
  - DISK9.IMG: no non-zero repeats.
  - Cross-file: no shared 16-byte blocks among DISK9, SU1, SU2, FRONTEND.SET, the filler, or ISO A data.
  - XOR of sample DISK0, DISK9 and SU1 sectors with the filler sector gives entropy near 7.9 bits, not plaintext.
- BTID block (sectors 1888..1891): sector 1888 starts with a plaintext header containing magic e7333aec, 0x8000, the tag "BTID", the tag "SBKX", a timestamp of 2007-02-28 18:43, the title "Power Smash 3 C", and the strings "Credit(s) to Start" and "Credit(s) to Continue". The rest is mostly random-looking with zero runs.
- No key found in the image so far. The filler, the unique pre-ISO1 sectors and the BTID block do not decrypt DISK0.

Tools and outputs:
- /Users/heath/Virtua Tennis 3/vt3-port/tools/lindbergh/lb_analyze.py (subcommands: layout, entropy, repeats, filler)
- /Users/heath/Virtua Tennis 3/dump/lindbergh/analysis/ (layout, repeat, filler, entropy outputs and a BTID hexdump)

## In progress

- None active. The analysis stopped at a clean point.

## Next (if resumed)

- Interpret the BTID block fields more fully and check the other BTID-tagged sectors.
- Decide with the owner whether any key material from outside this image may be used. Nothing outside the image has been searched or tried.
- Without a key, DISK0.IMG cannot be decrypted, so no x86 binary has been extracted.
