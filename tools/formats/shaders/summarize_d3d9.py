#!/usr/bin/env python3
"""Summarise D3D9 shader blobs found in one or more binaries (deduplicated by MD5).

For each distinct blob it reports: id, version, size, token count, instruction count,
opcode histogram, the CTAB constant table (target, constant names and registers),
and the binaries and file offsets where the blob occurs. Optionally writes a
disassembly for each blob.

Usage:
    summarize_d3d9.py FILE [FILE ...] [--md OUT.md] [--tsv OUT.tsv] [--disasm-dir DIR]

--md          markdown table (one row per distinct blob)
--tsv         the same data as tab-separated values
--disasm-dir  write DIR/S01.asm ... for each distinct blob (keep outside the repo)
"""
import argparse
import collections
import hashlib
import mmap
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9_sm as sm  # noqa: E402


def blobs_in(path):
    size = os.path.getsize(path)
    if size == 0:
        return
    with open(path, "rb") as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            last_end = -1
            for i in sm.find_candidates(data):
                if i < last_end:
                    continue
                r = sm.walk(data, i)
                if r is None:
                    continue
                last_end = r["end"]
                yield i, r, bytes(data[i:r["end"]])
        finally:
            data.close()


def ctab_info(r):
    for _off, payload in r["comments"]:
        c = sm.parse_ctab(payload)
        if c:
            return c
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--md")
    ap.add_argument("--tsv")
    ap.add_argument("--disasm-dir")
    args = ap.parse_args(argv)

    distinct = {}  # md5 -> dict
    per_file = collections.Counter()
    for path in args.files:
        for off, r, blob in blobs_in(path):
            per_file[path] += 1
            md5 = hashlib.md5(blob).hexdigest()
            if md5 not in distinct:
                distinct[md5] = {"walk": r, "blob": blob, "occurrences": []}
            distinct[md5]["occurrences"].append((os.path.basename(os.path.dirname(path)) + "/" + os.path.basename(path), off))

    ordered = sorted(distinct.items(), key=lambda kv: (kv[1]["walk"]["kind"], kv[1]["walk"]["major"], kv[1]["walk"]["minor"], len(kv[1]["blob"])))
    rows = []
    for n, (md5, d) in enumerate(ordered, 1):
        r = d["walk"]
        ct = ctab_info(r)
        hist = sorted(r["histogram"].items(), key=lambda kv: (-kv[1], kv[0]))
        consts = ""
        if ct:
            consts = ";".join("%s@%s%d" % (name, {0: "b", 1: "i", 2: "c", 3: "s"}.get(rs, "r%d" % rs), ri) for name, rs, ri, _cnt in ct["constants"])
        rows.append({
            "id": "S%02d" % n, "md5": md5, "version": "%s_%d_%d" % (r["kind"], r["major"], r["minor"]),
            "bytes": len(d["blob"]), "tokens": r["tokens"], "instructions": len(r["instructions"]),
            "opcodes": " ".join("%s:%d" % kv for kv in hist),
            "ctab_target": ct["target"] if ct else "", "ctab_consts": consts,
            "occurrences": "%d (%s)" % (len(d["occurrences"]), ", ".join(sorted(set(o[0] for o in d["occurrences"])))),
            "first": "%s @0x%x" % d["occurrences"][0],
        })
        if args.disasm_dir:
            os.makedirs(args.disasm_dir, exist_ok=True)
            with open(os.path.join(args.disasm_dir, "S%02d.asm" % n), "w") as o:
                o.write("\n".join(sm.disassemble(r)) + "\n")
                if ct:
                    o.write("; CTAB creator=%s target=%s\n" % (ct["creator"].strip(), ct["target"]))
                    for name, rs, ri, cnt in ct["constants"]:
                        o.write("; const %s regset=%d index=%d count=%d\n" % (name, rs, ri, cnt))

    print("binaries: %d; blobs per binary: %s" % (len(args.files), dict(per_file)))
    print("distinct blobs: %d" % len(rows))
    if args.tsv:
        cols = ["id", "version", "bytes", "tokens", "instructions", "ctab_target", "ctab_consts", "occurrences", "first", "opcodes", "md5"]
        with open(args.tsv, "w") as o:
            o.write("\t".join(cols) + "\n")
            for row in rows:
                o.write("\t".join(str(row[c]) for c in cols) + "\n")
    if args.md:
        with open(args.md, "w") as o:
            o.write("| id | version | bytes | instr | opcodes | constants (CTAB) | found in | md5 |\n")
            o.write("|---|---|---|---|---|---|---|---|\n")
            for row in rows:
                o.write("| %s | %s | %d | %d | %s | %s | %s | %s |\n" % (
                    row["id"], row["version"], row["bytes"], row["instructions"], row["opcodes"],
                    row["ctab_consts"] or "-", row["occurrences"], row["md5"][:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
