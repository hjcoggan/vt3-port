#!/usr/bin/env python3
"""Find D3D9 shader bytecode blobs in files and report version, size and opcode histogram.

Uses d3d9_sm.py (same directory). A blob is accepted only when its token stream
walks cleanly to the 0x0000FFFF end token (see d3d9_sm.py for the rules).

For CAPX (.xpac) archives the blob is attributed to the payload entry that contains
it (entry index and entry hash), so the table can say which resource holds it.

Usage:
    scan_d3d9.py FILE [FILE ...] [--tsv OUT.tsv] [--disasm OFFSET] [--min-size N]

    --tsv      write one line per blob (file, offset, entry, version, size, tokens,
               instruction count, top opcodes, md5 of the blob)
    --disasm   print the disassembly of the blob starting at OFFSET (hex ok) in FILE
    --min-size skip blobs smaller than N bytes (default 0)
"""
import argparse
import hashlib
import mmap
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import d3d9_sm as sm  # noqa: E402

try:
    import capx  # noqa: E402
except ImportError:  # pragma: no cover
    capx = None


def entry_for(offset, table):
    for e in table:
        if e.offset <= offset < e.offset + e.size:
            return e
    return None


def scan_file(path, min_size=0):
    """Yield (offset, walk_result, entry_or_None) for each blob in path."""
    size = os.path.getsize(path)
    if size == 0:
        return
    table = None
    with open(path, "rb") as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            if capx is not None and data[:4] == b"CAPX":
                table = capx.read_table(data)
            last_end = -1
            for i in sm.find_candidates(data):
                if i < last_end:
                    continue
                r = sm.walk(data, i)
                if r is None or r["size"] < min_size:
                    continue
                last_end = r["end"]
                ent = entry_for(i, table) if table else None
                blob = bytes(data[i:r["end"]])
                r["md5"] = hashlib.md5(blob).hexdigest()
                r["blob"] = blob
                yield i, r, ent
        finally:
            data.close()


def describe(r):
    top = sorted(r["histogram"].items(), key=lambda kv: -kv[1])
    return ",".join("%s:%d" % kv for kv in top)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--tsv")
    ap.add_argument("--disasm")
    ap.add_argument("--min-size", type=int, default=0)
    args = ap.parse_args(argv)

    if args.disasm:
        if len(args.files) != 1:
            ap.error("--disasm takes exactly one file")
        off = int(args.disasm, 0)
        with open(args.files[0], "rb") as f:
            data = f.read(off + 65536)
        r = sm.walk(data, off)
        if r is None:
            raise SystemExit("no valid shader at %s" % hex(off))
        print("\n".join(sm.disassemble(r)))
        return 0

    rows = [("file", "offset", "entry", "kind", "version", "bytes", "tokens", "instructions", "opcodes", "md5")]
    total = 0
    for path in args.files:
        found = 0
        for off, r, ent in scan_file(path, args.min_size):
            found += 1
            total += 1
            entry_txt = "" if ent is None else "%d/%08x" % (ent.index, ent.hash)
            ver = "%s_%d_%d" % (r["kind"], r["major"], r["minor"])
            ninst = len(r["instructions"])
            rows.append((path, "0x%x" % off, entry_txt, r["kind"], ver,
                         str(r["size"]), str(r["tokens"]), str(ninst), describe(r), r["md5"]))
        print("%s: %d shader blobs" % (path, found))
    if args.tsv:
        with open(args.tsv, "w") as o:
            for row in rows:
                o.write("\t".join(row) + "\n")
        print("wrote %s (%d blobs)" % (args.tsv, total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
