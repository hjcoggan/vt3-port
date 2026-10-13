#!/usr/bin/env python3
"""Cross-platform helpers for Virtua Tennis 3 data: CAPX tables, the PS3 xaf0
file tree, and name-to-hash matching.

Subcommands (all paths are arguments; game data is never written to the repo):

  tables FILE...                 Summarise CAPX headers and table invariants.
  xaf FILE [--json OUT]          Walk the PS3 vt3.xaf0 file tree and list paths.
                                 The 6339 records end at 0x94B20, so the first
                                 1 MB of vt3.xaf is enough (stream it from the
                                 7z; do not extract the whole 4.5 GB file).
  match ARCHIVE NAMES            Hash each name (one per line, or a JSON list of
                                 path strings) and count how many appear in
                                 ARCHIVE. With --rank, also check the packer's
                                 sort order: the name at rank i must hash to
                                 record i's name_hash (use on a complete list).
  shared A B                     Count name_hash values shared by two archives,
                                 and how many of those also have the same size.

Layout details: docs/formats-crossplatform.md.
Requires only the standard library. Run with `python3 -I`.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capx_hash import assign_indices, capx_key, capx_name_hash  # noqa: E402

HDR = struct.Struct("<4sIII")
REC = struct.Struct("<IIII")


def read_capx(path):
    """Return (header_dict, records) where records are (hash, offset, size, index_id)."""
    with open(path, "rb") as f:
        head = f.read(16)
        magic, count, hsize, align = HDR.unpack(head)
        if magic != b"CAPX":
            raise ValueError("%s: not a CAPX archive" % path)
        f.seek(hsize)
        table = f.read(REC.size * count)
    records = [REC.unpack_from(table, i * REC.size) for i in range(count)]
    return {"count": count, "header_size": hsize, "align": align}, records


def cmd_tables(args):
    for path in args.files:
        hdr, recs = read_capx(path)
        hashes = [r[0] for r in recs]
        ids = sorted(r[3] for r in recs)
        offs = [r[1] for r in recs]
        sizes = [r[2] for r in recs]
        size = os.path.getsize(path)
        print(os.path.basename(path))
        print("  count=%d header=0x%x align=0x%x file_size=%d" % (hdr["count"], hdr["header_size"], hdr["align"], size))
        print("  hash_sorted=%s index_id_is_permutation=%s duplicate_hashes=%d" % (
            hashes == sorted(hashes), ids == list(range(hdr["count"])), len(hashes) - len(set(hashes))))
        print("  offsets_2048_aligned=%s max_end_inside_file=%s" % (
            all(o % 0x800 == 0 for o in offs), max(o + s for o, s in zip(offs, sizes)) <= size))
    return 0


def xaf_records(data, count):
    """Yield (index, path, is_dir, size) by walking the child/next links from record 0."""
    recs = []
    for i in range(count):
        off = 0x100 + 0x60 * i
        name = data[off:off + 0x40].split(b"\0", 1)[0].decode("ascii", "replace")
        meta = struct.unpack_from("<8I", data, off + 0x40)
        recs.append({"name": name, "type": meta[0], "parent": meta[1], "next": meta[2], "child": meta[3], "size": meta[4]})

    def s32(x):
        return x - (1 << 32) if x >= (1 << 31) else x

    for r in recs:
        r["parent"], r["next"], r["child"] = s32(r["parent"]), s32(r["next"]), s32(r["child"])
    out = []
    stack = [(0, "")]
    seen = set()
    while stack:
        first, prefix = stack.pop()
        c = first
        while 0 <= c < count and c not in seen:
            seen.add(c)
            r = recs[c]
            path = (prefix + "/" + r["name"]) if prefix else r["name"]
            out.append({"index": c, "path": path, "is_dir": r["type"] == 0, "size": r["size"]})
            if r["type"] == 0 and r["child"] >= 0:
                stack.append((r["child"], path))
            c = r["next"]
    return out


def cmd_xaf(args):
    with open(args.file, "rb") as f:
        head = f.read(0x20)
        if head[:4] != b"xaf0":
            raise SystemExit("not an xaf0 container")
        # Header: u32 at 0x0C = record count, 0x10 = folder count, 0x14 = file count.
        n_records, n_dirs, n_files = struct.unpack_from("<III", head, 0x0C)
        f.seek(0)
        data = f.read(0x100 + 0x60 * n_records)
    entries = xaf_records(data, n_records)
    nfiles = sum(1 for e in entries if not e["is_dir"])
    ndirs = sum(1 for e in entries if e["is_dir"])
    print("xaf0 walked=%d (files=%d dirs=%d); header says records=%d files=%d dirs=%d" % (
        len(entries), nfiles, ndirs, n_records, n_files, n_dirs))
    if args.json:
        with open(args.json, "w") as o:
            json.dump([{"path": e["path"], "type": 0 if e["is_dir"] else 1, "size": e["size"]} for e in entries], o)
        print("wrote", args.json)
    return 0


def load_names(path):
    with open(path, "rb") as f:
        raw = f.read()
    if path.endswith(".json"):
        items = json.loads(raw.decode("utf-8"))
        return [x["path"] if isinstance(x, dict) else x for x in items]
    return [l.strip() for l in raw.decode("latin-1").splitlines() if l.strip()]


def cmd_match(args):
    _, recs = read_capx(args.archive)
    table = {}
    for r in recs:
        table.setdefault(r[0], []).append(r)
    names = load_names(args.names)
    hits = [(n, capx_name_hash(n)) for n in names]
    matched = [(n, h) for n, h in hits if h in table]
    print("%s: %d names, %d hash-match entries (%d distinct archive entries)" % (
        os.path.basename(args.archive), len(names), len(matched), len({h for _, h in matched})))
    if args.rank:
        ranked = assign_indices(names)
        by_index = {r[3]: r for r in recs}
        good = bad = 0
        for i, path in ranked.items():
            if i in by_index and by_index[i][0] == capx_name_hash(path):
                good += 1
            else:
                bad += 1
        print("  rank check: %d consistent, %d inconsistent (rank = index_id and hash equal)" % (good, bad))
    return 0


def cmd_shared(args):
    _, ra = read_capx(args.a)
    _, rb = read_capx(args.b)
    ha = {r[0]: r for r in ra}
    hb = {r[0]: r for r in rb}
    common = set(ha) & set(hb)
    same = [h for h in common if ha[h][2] == hb[h][2]]
    print("%s (%d) vs %s (%d): shared hashes=%d, same size=%d" % (
        os.path.basename(args.a), len(ra), os.path.basename(args.b), len(rb), len(common), len(same)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("tables")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_tables)
    p = sub.add_parser("xaf")
    p.add_argument("file")
    p.add_argument("--json")
    p.set_defaults(fn=cmd_xaf)
    p = sub.add_parser("match")
    p.add_argument("archive")
    p.add_argument("names")
    p.add_argument("--rank", action="store_true", help="check the sort-rank rule (only for a complete name list)")
    p.set_defaults(fn=cmd_match)
    p = sub.add_parser("shared")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_shared)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
