#!/usr/bin/env python3
"""Lindbergh disc analysis helpers (read-only).

Subcommands:
  layout  <image.iso> [--out FILE]   Walk volume descriptors, directory records,
                                     and classify every disc sector (zero /
                                     plaintext / high-entropy).
  entropy <file>                     Whole-file entropy summary and magic check.
  repeats <file> [--mb N] [--cross]  Repeated 16-byte blocks in 2048-byte sectors:
                                     distance histogram (intra-sector, and
                                     cross-sector with --cross) over the first N MB.
                                     Zero blocks are ignored.
  filler  <image.iso> START END      Sector census of a disc range: distinct
                                     sectors and how often the most common one occurs.

No keys or game data are embedded here. Pass paths as arguments.
Run with: python3 -I lb_analyze.py <subcommand> ...
"""
import argparse
import collections
import math
import re
import struct
import sys

SEC = 2048
BLK = 16


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def sector_class(b):
    if b.count(0) == len(b):
        return "zero"
    if len(set(b)) < 200:
        return "plain"
    return "high"


def read_dir(buf, base, extent, length):
    out = []
    if length <= 0 or length > 16 * SEC:
        raise ValueError(f"implausible directory length {length}")
    off = (base + extent) * SEC
    end = off + length
    while off < end:
        rl = buf[off]
        if rl == 0:
            off = (off // SEC + 1) * SEC
            continue
        ext = struct.unpack("<I", buf[off + 2:off + 6])[0]
        size = struct.unpack("<I", buf[off + 10:off + 14])[0]
        flags = buf[off + 25]
        nl = buf[off + 32]
        raw = buf[off + 33:off + 33 + nl]
        out.append((raw, ext, size, flags))
        off += rl
    return out


def cmd_layout(args):
    with open(args.image, "rb") as f:
        buf = f.read()
    total = len(buf) // SEC
    lines = [f"image {args.image}", f"bytes {len(buf)} sectors {total}"]
    pvds = []
    for s in range(total):
        o = s * SEC
        if buf[o + 1:o + 6] == b"CD001":
            vt = buf[o]
            if vt == 1:
                vol = struct.unpack("<I", buf[o + 80:o + 84])[0]
                root_ext = struct.unpack("<I", buf[o + 158:o + 162])[0]
                root_len = struct.unpack("<I", buf[o + 166:o + 170])[0]
                lines.append(f"PVD sector {s}: volume {vol} sectors, system id {buf[o+8:o+40].decode('ascii','replace').strip()!r}, "
                             f"volume id {buf[o+40:o+72].decode('ascii','replace').strip()!r}, root extent {root_ext} len {root_len}")
                pvds.append((s, root_ext, root_len))
            else:
                lines.append(f"descriptor type {vt} at sector {s}")
    base_map = {}
    for s, root_ext, root_len in pvds:
        base = s - 16  # ISO-relative sector 16 is the PVD
        base_map[s] = base
        lines.append(f"  ISO starting at disc sector {base} (PVD at {s}), root entries:")
        try:
            entries = read_dir(buf, base, root_ext, root_len)
        except ValueError as exc:
            lines.append(f"    cannot parse root: {exc}")
            continue
        for raw, ext, size, flags in entries:
            if raw in (b"\x00", b"\x01"):
                continue
            name = raw.decode("ascii", "replace") if raw.isascii() else raw.decode("utf-16-be", "replace")
            nsec = (size + SEC - 1) // SEC
            lines.append(f"    {name!r}: disc sectors {base + ext}..{base + ext + nsec - 1} "
                         f"(bytes {(base + ext) * SEC}..{(base + ext) * SEC + size}), size {size}")
    # classify all sectors, run-length encoded
    runs = []
    prev = None
    start = 0
    for s in range(total):
        c = sector_class(buf[s * SEC:(s + 1) * SEC])
        if c != prev:
            if prev is not None:
                runs.append((start, s - 1, prev))
            prev = c
            start = s
    runs.append((start, total - 1, prev))
    lines.append("sector class runs (start..end, class):")
    for a, b, c in runs:
        lines.append(f"  {a}..{b} ({b - a + 1} sectors) {c}")
    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    sys.stdout.write(text)


def cmd_entropy(args):
    data = open(args.file, "rb").read()
    print(f"{args.file} size {len(data)}")
    print(f"overall entropy (bits/byte): {entropy(data[:8 << 20]):.4f} (first 8 MB)")
    step = max(65536, len(data) // 16)
    samples = [(o, round(entropy(data[o:o + 65536]), 3)) for o in range(0, len(data), step)]
    print("windowed entropy, 64 KiB windows:", samples)
    sigs = {b"\x1f\x8b\x08": "gzip", b"\xfd7zXZ\x00": "xz", b"PK\x03\x04": "zip",
            b"7z\xbc\xaf\x27\x1c": "7z", b"\x7fELF": "ELF", b"\x53\xef": "ext2/3 magic (2 bytes, weak)",
            b"CD001": "ISO9660", b"SEGA": "text SEGA", b"LZMA": "text LZMA"}
    hits = []
    for k, v in sigs.items():
        for m in re.finditer(re.escape(k), data):
            hits.append((v, m.start()))
            if len([h for h in hits if h[0] == v]) >= 3:
                break
    print("signature hits (first 3 per signature):", hits[:20] if hits else "none")
    print("note: 2-byte signatures occur by chance about once per 64 KiB; only >=3-byte hits are meaningful.")


def cmd_repeats(args):
    path = args.file
    limit = args.mb * (1 << 20) if args.mb else None
    f = open(path, "rb")
    dist = collections.Counter()
    seen = {}
    intra = 0
    cross_pairs = 0
    cross_same = 0
    cross_mod = collections.Counter()
    n = 0
    while True:
        if limit and n * SEC >= limit:
            break
        b = f.read(SEC)
        if len(b) < SEC:
            break
        if b == bytes(SEC):
            n += 1
            continue
        idx = collections.defaultdict(list)
        for i in range(0, SEC, BLK):
            idx[b[i:i + BLK]].append(i // BLK)
        for k, v in idx.items():
            if k == bytes(BLK):
                continue
            if len(v) > 1:
                for a in range(len(v)):
                    for c in range(a + 1, len(v)):
                        dist[v[c] - v[a]] += 1
                        intra += 1
        if args.cross:
            local = {}
            for i in range(0, SEC, BLK):
                x = b[i:i + BLK]
                if x == bytes(BLK) or x in local:
                    continue
                local[x] = i // BLK
                if x in seen:
                    cross_pairs += 1
                    d = (i // BLK) - seen[x][1]
                    if d == 0:
                        cross_same += 1
                    cross_mod[d % 32] += 1
            for x, p in local.items():
                if x not in seen:
                    seen[x] = (n, p)
        n += 1
    print(f"{path}: sectors scanned {n}")
    print(f"intra-sector non-zero repeat pairs: {intra}")
    print("intra distances (16-byte blocks):", sorted(dist.items()))
    if dist:
        mult = sum(c for d, c in dist.items() if d % 32 == 0)
        print(f"  of which distance is a multiple of 32 blocks (512 bytes): {mult}")
    if args.cross:
        print(f"cross-sector repeats (same block value in an earlier sector): {cross_pairs}")
        print(f"  at same in-sector offset: {cross_same}")
        print("  in-sector offset difference mod 32 blocks (512 B):", sorted(cross_mod.items()))


def cmd_filler(args):
    f = open(args.image, "rb")
    f.seek(args.start * SEC)
    secs = [f.read(SEC) for _ in range(args.start, args.end)]
    cnt = collections.Counter(secs)
    top, tc = cnt.most_common(1)[0]
    print(f"sectors {args.start}..{args.end - 1}: {len(secs)} sectors, {len(cnt)} distinct")
    print(f"most common sector occurs {tc} times; it is zero: {top == bytes(SEC)}")
    odd = [args.start + i for i, s in enumerate(secs) if s != top]
    print(f"sectors differing from the most common one: {len(odd)}")
    if odd:
        print("  first differing disc sectors:", odd[:40])
        print("  last differing disc sectors:", odd[-10:])
    print("entropy of most common sector:", round(entropy(top), 3))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("layout")
    a.add_argument("image")
    a.add_argument("--out")
    a.set_defaults(fn=cmd_layout)
    b = sub.add_parser("entropy")
    b.add_argument("file")
    b.set_defaults(fn=cmd_entropy)
    c = sub.add_parser("repeats")
    c.add_argument("file")
    c.add_argument("--mb", type=int, default=0, help="scan only the first N MB (0 = whole file)")
    c.add_argument("--cross", action="store_true", help="also count cross-sector repeats (memory grows with N)")
    c.set_defaults(fn=cmd_repeats)
    d = sub.add_parser("filler")
    d.add_argument("image")
    d.add_argument("start", type=int)
    d.add_argument("end", type=int)
    d.set_defaults(fn=cmd_filler)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
