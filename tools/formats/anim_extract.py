#!/usr/bin/env python3
"""Index the animation data in Resource/Anims/<skeleton>/ (PC release).

Each skeleton folder holds:
    anim.db      animation name table (see docs/formats.md, "Animation names")
    hierachy     bone hierarchy: u8 bone count, then one parent index per bone
                 (0xFF = root). Uncertain: the trailing bytes, if any.
    pack*.adb    animation packs: u32 count, then count+1 u32 offsets
                 (the first is 8, the second is the first entry's start)

The tool writes one JSON file per skeleton that maps each animation name to its
pack file and entry, and decodes the 36-byte entry header that is understood so
far. Keyframe data is NOT decoded yet; the raw entries can be dumped with
--raw for later work.

Usage:
    anim_extract.py SKELETON_DIR [OUTDIR] [--raw]

SKELETON_DIR  e.g. .../data1/Resource/Anims/Player
OUTDIR        default: /Users/heath/Virtua Tennis 3/dump/extracted-en/anims
--raw         also write each pack entry to OUTDIR/<skeleton>/raw/ as .bin
"""
import argparse
import json
import os
import re
import struct
import sys

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/anims"
RECORD = 48  # anim.db record size for the 48-byte variant
MAGIC_COUNT = 4


def find_ci(folder, pattern):
    """Return the paths in folder whose names match pattern (case-insensitive)."""
    if not os.path.isdir(folder):
        return []
    rx = re.compile(pattern, re.IGNORECASE)
    return [os.path.join(folder, n) for n in os.listdir(folder) if rx.fullmatch(n)]


def read_hierachy(path):
    """Return the list of parent indices (-1 for root), or None if missing."""
    found = find_ci(os.path.dirname(path), r"hierachy|HIER_RED")
    if not found:
        return None
    path = found[0]
    d = open(path, "rb").read()
    n = d[0]
    parents = []
    for i in range(n):
        v = d[1 + i]
        parents.append(-1 if v == 0xFF else v)
    return {"bone_count": n, "parents": parents, "trailing_bytes": len(d) - 1 - n}


def read_anim_db(path):
    """Return (records, variant). records are dicts with name and raw fields.

    The 48-byte variant is used by Player, Ball, umpire, ballperson and Morph.
    Trophy/anim.db uses a variable-length variant that is not parsed yet.
    """
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    if len(d) != MAGIC_COUNT + RECORD * n:
        return None, "unparsed_variable_length"
    records = []
    for k in range(n):
        R = MAGIC_COUNT + RECORD * k
        f0, f1, f2 = struct.unpack_from("<III", d, R)
        name = d[R + 14 : R + RECORD].split(b"\0")[0].decode("latin-1")
        records.append({"index": k, "name": name, "f0": f0, "f1": f1, "f2": f2})
    return records, "48-byte"


def read_pack(path):
    """Return the list of (start, end) byte ranges of the entries in a pack."""
    d = open(path, "rb").read()
    if len(d) < 8:
        return d, []
    n = struct.unpack_from("<I", d, 0)[0]
    if n == 0:
        return d, []
    # Words 1..n+1 hold 8 and the n entry start offsets (entry k starts at word 2+k).
    # The last entry runs to the end of the file (checked on Ball and Player packs).
    starts = list(struct.unpack_from("<%dI" % n, d, 8))
    ends = starts[1:] + [len(d)]
    return d, list(zip(starts, ends))


def entry_header(payload):
    """Decode the parts of the entry header that are understood. Others are kept raw."""
    if len(payload) < 24:
        return {"raw_head": payload.hex()}
    b0, bones, frames = payload[0], payload[1], payload[2]
    u16 = struct.unpack_from("<HHH", payload, 4)
    u32 = struct.unpack_from("<III", payload, 12)
    return {
        "version_byte": b0,
        "bone_count_guess": bones,   # matches hierachy count in the samples checked
        "frame_count_guess": frames,  # plausible in the samples checked; not confirmed
        "u16_4_6_8": list(u16),
        "u32_12_16_20": list(u32),
        "header_bytes": 36 if len(payload) >= 36 else len(payload),
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skeleton_dir")
    ap.add_argument("outdir", nargs="?", default=DEFAULT_OUT)
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args(argv)

    name = os.path.basename(os.path.normpath(args.skeleton_dir))
    anim_db = find_ci(args.skeleton_dir, r"anim\.db")
    records, variant = read_anim_db(anim_db[0]) if anim_db else (None, "missing")
    hier = read_hierachy(os.path.join(args.skeleton_dir, "hierachy"))
    packs = sorted(
        find_ci(args.skeleton_dir, r"pack\d+\.adb"),
        key=lambda p: int(re.findall(r"\d+", os.path.basename(p))[0]),
    )

    entries = []  # global list of (pack_file, local_index, payload)
    for pack in packs:
        d, ranges = read_pack(pack)
        for local, (a, b) in enumerate(ranges):
            entries.append((os.path.basename(pack), local, d[a:b], a))

    out = {
        "skeleton": name,
        "anim_db_variant": variant,
        "hierachy": hier,
        "pack_files": [os.path.basename(p) for p in packs],
        "pack_entry_count": len(entries),
        "anim_count": len(records) if records is not None else None,
        "mapping_note": "anim.db record k is assumed to describe global pack entry k (packs in numeric order). Verified for the Player set in the counts; records 566 onward have different index fields.",
        "animations": [],
    }
    if records is not None:
        for k, rec in enumerate(records):
            pack_name, local, payload, a = entries[k] if k < len(entries) else (None, None, b"", None)
            item = {
                "index": k,
                "name": rec["name"],
                "anim_db_fields": [rec["f0"], rec["f1"], rec["f2"]],
                "pack": pack_name,
                "pack_local_index": local,
                "entry_bytes": len(payload),
                "header": entry_header(payload) if payload else None,
            }
            out["animations"].append(item)
    else:
        out["animations_unparsed"] = "anim.db variant not supported yet"
        out["pack_entries"] = [
            {"pack": p, "local": l, "bytes": len(pl), "header": entry_header(pl)} for p, l, pl, _ in entries
        ]

    os.makedirs(args.outdir, exist_ok=True)
    jpath = os.path.join(args.outdir, "%s.json" % name)
    with open(jpath, "w") as f:
        json.dump(out, f, indent=1)

    if args.raw:
        rawdir = os.path.join(args.outdir, name, "raw")
        os.makedirs(rawdir, exist_ok=True)
        for k, (p, l, pl, _) in enumerate(entries):
            with open(os.path.join(rawdir, "%05d_%s_%03d.bin" % (k, os.path.splitext(p)[0], l)), "wb") as o:
                o.write(pl)

    print("%s: %d anims (%s), %d pack entries, wrote %s" % (name, len(records) if records else 0, variant, len(entries), jpath))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
