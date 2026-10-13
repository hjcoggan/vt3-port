#!/usr/bin/env python3
"""Export fixed-size record tables from Resource/database/ to JSON.

The player database files are flat arrays of fixed-size records with no header.
This tool splits a file into records of --record-size bytes and writes:

    name       first printable ASCII run at the start of the record (if any)
    words      the record as little-endian u32 words, in hex
    hex        the full record

Verified layout (PC release):
    tns_player_tbl_ALL.bin  record size 188 (0xBC); 25 records; name at offset 0
                            (for example "Roger Federer(CHE)"). Field meanings
                            beyond the name are not yet known.

Other tables in the same folder (tns_racket_tbl.bin, tns_swing_tbl_*.bin,
tns_IKInfo_*.bin, tns_playstyle_tbl_dataM.bin) are binary and their record sizes
are not yet confirmed. Use --record-size to try a guess.

Usage:
    db_extract.py FILE --record-size N [OUTDIR]

Default OUTDIR: /Users/heath/Virtua Tennis 3/dump/extracted-en/database
"""
import argparse
import json
import os
import re
import struct
import sys

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/database"


def export(path, record_size):
    data = open(path, "rb").read()
    if record_size <= 0 or len(data) % record_size:
        print("warning: %d bytes is not a multiple of %d; trailing %d bytes ignored"
              % (len(data), record_size, len(data) % record_size))
    records = []
    for k in range(len(data) // record_size):
        rec = data[k * record_size : (k + 1) * record_size]
        m = re.match(rb"[ -~]{2,}", rec)
        name = m.group().decode("latin-1") if m else None
        words = list(struct.unpack_from("<%dI" % (record_size // 4), rec, 0)) if record_size % 4 == 0 else None
        records.append({
            "index": k,
            "name": name,
            "words": ["%08x" % w for w in words] if words else None,
            "hex": rec.hex(),
        })
    return {"file": os.path.basename(path), "bytes": len(data), "record_size": record_size, "records": records}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--record-size", type=int, required=True)
    ap.add_argument("outdir", nargs="?", default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    os.makedirs(args.outdir, exist_ok=True)
    out = export(args.file, args.record_size)
    dst = os.path.join(args.outdir, os.path.splitext(out["file"])[0] + ".json")
    with open(dst, "w") as f:
        json.dump(out, f, indent=1)
    print("%s: %d records of %d bytes -> %s" % (out["file"], len(out["records"]), args.record_size, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
