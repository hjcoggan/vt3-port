#!/usr/bin/env python3
"""List and extract entries from CAPX (.xpac) archives in the PC release of
Virtua Tennis 3.

Layout (little-endian), as far as it has been reverse engineered so far:

    0x00  char[4]  magic        "CAPX"
    0x04  u32      count        number of table entries
    0x08  u32      header_size  0x10; the table starts here
    0x0C  u32      align        0x800; entry offsets are multiples of this
    0x10  record[count], 16 bytes each:
              u32 name_hash   (unknown hash of the asset name; not yet decoded)
              u32 offset      absolute file offset of the payload
              u32 size        payload size in bytes
              u32 unknown     small integer, meaning not yet known

Audio payloads are CRI ADX streams (magic 0x80 0x00). Other payload kinds
are recorded by magic in --list output.

Usage:
    capx_extract.py --list ARCHIVE
    capx_extract.py ARCHIVE OUTDIR [--wav]

Game data is never written into this repository. Point OUTDIR at a folder
outside the repo (for example "dump/extracted/..." in the workspace).
"""
import argparse
import mmap
import os
import shutil
import struct
import subprocess
import sys
from collections import Counter

HEADER = struct.Struct("<4sIII")
RECORD = struct.Struct("<IIII")


def read_table(data):
    magic, count, header_size, align = HEADER.unpack_from(data, 0)
    if magic != b"CAPX":
        raise ValueError("not a CAPX archive (magic=%r)" % magic)
    entries = []
    for i in range(count):
        name_hash, offset, size, unknown = RECORD.unpack_from(
            data, header_size + i * RECORD.size
        )
        if offset + size > len(data):
            raise ValueError("entry %d runs past end of file" % i)
        entries.append(
            {
                "index": i,
                "hash": name_hash,
                "offset": offset,
                "size": size,
                "unknown": unknown,
            }
        )
    return entries


def sniff(payload):
    if payload[:2] == b"\x80\x00":
        return "adx"
    return payload[:4].hex() or "empty"


def cmd_list(path):
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entries = read_table(data)
        kinds = Counter()
        for e in entries:
            kinds[sniff(data[e["offset"] : e["offset"] + 2])] += 1
        print("%s: %d entries" % (os.path.basename(path), len(entries)))
        for kind, n in kinds.most_common():
            print("  %-12s %d" % (kind, n))


def to_wav(adx_path, wav_path):
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not found; install it or run without --wav")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", adx_path, wav_path],
        check=True,
    )


def cmd_extract(path, outdir, wav):
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(path))[0]
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entries = read_table(data)
        for e in entries:
            payload = data[e["offset"] : e["offset"] + e["size"]]
            kind = sniff(payload)
            ext = "adx" if kind == "adx" else "bin"
            base = "%s_%04d_%08x" % (stem, e["index"], e["hash"])
            out = os.path.join(outdir, base + "." + ext)
            with open(out, "wb") as o:
                o.write(payload)
            if wav and ext == "adx":
                to_wav(out, os.path.join(outdir, base + ".wav"))
    print("extracted %d entries from %s into %s" % (len(entries), path, outdir))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="print a summary instead of extracting")
    ap.add_argument("--wav", action="store_true", help="also convert ADX payloads to WAV (needs ffmpeg)")
    ap.add_argument("archive")
    ap.add_argument("outdir", nargs="?")
    args = ap.parse_args(argv)
    if args.list:
        cmd_list(args.archive)
        return 0
    if not args.outdir:
        ap.error("OUTDIR is required unless --list is given")
    cmd_extract(args.archive, args.outdir, args.wav)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
