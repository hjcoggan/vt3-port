#!/usr/bin/env python3
"""Extract sound effects from Sound/SE.xpac (CRI Atom cue sheets, ACB).

Each SE.xpac entry is one CRI Atom cue sheet ("@UTF" tables). Its nested
TBLSDL table (sound elements) holds the original file name in 'name'
(for example "PS2_BGM_Load_Mono02.aif") and a nested AAX table whose 'data'
blob is a complete CRI ADX stream. This tool walks the tables, writes each
ADX blob, and records the cue and synth names alongside it.

Outputs, per archive entry:
    OUTDIR/<stem>_<entry:04d>_<hash>_<sound>.adx   the raw ADX blob
    OUTDIR/<...>.wav                               decoded with ffmpeg (--wav)
    OUTDIR/index.tsv                               one line per sound element

Usage:
    se_extract.py ARCHIVE [OUTDIR] [--wav]

Default OUTDIR: /Users/heath/Virtua Tennis 3/dump/extracted-en/sound/SE
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capx  # noqa: E402
import cri_utf  # noqa: E402

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/sound/SE"


def find_tables(table, want, out):
    """Collect every nested table called `want` (recursing into @UTF blobs)."""
    if table.name == want:
        out.append(table)
    for row in table.rows:
        for value in row.values():
            if isinstance(value, bytes) and value[:4] == b"@UTF":
                find_tables(cri_utf.parse(value), want, out)
    return out


def safe(name):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "unnamed"


def sound_elements(root):
    """Yield dicts for each sound element: file name, cue names, rate, channels and ADX bytes.

    Chain: TBLSDL.name == TBLSYN.lnkname, TBLSYN.synname == TBLCUE.synth.
    """
    synth_by_file = {}
    for t in find_tables(root, "TBLSYN", []):
        for r in t.rows:
            synth_by_file.setdefault(r.get("lnkname"), []).append(r.get("synname"))
    cues_by_synth = {}
    for t in find_tables(root, "TBLCUE", []):
        for r in t.rows:
            cues_by_synth.setdefault(r.get("synth"), []).append(r.get("name"))

    for t in find_tables(root, "TBLSDL", []):
        for r in t.rows:
            adx = None
            for value in r.values():
                if isinstance(value, bytes) and value[:4] == b"@UTF":
                    for aax in find_tables(cri_utf.parse(value), "AAX", []):
                        for ar in aax.rows:
                            if isinstance(ar.get("data"), bytes):
                                adx = ar["data"]
            if adx is None:
                continue
            cues = []
            for synth in synth_by_file.get(r.get("name"), []):
                cues.extend(cues_by_synth.get(synth, []))
            yield {
                "name": r.get("name") or "",
                "cues": [c for c in cues if c],
                "nch": r.get("nch"),
                "sfreq": r.get("sfreq"),
                "data": adx,
            }


def to_wav(adx_path, wav_path):
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not found")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", adx_path, wav_path],
        check=True,
    )


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--wav", action="store_true", help="also decode to WAV with ffmpeg")
    args = ap.parse_args(argv)

    stem = capx.archive_stem(args.archive)
    outdir = args.outdir or DEFAULT_OUT
    os.makedirs(outdir, exist_ok=True)
    lines = ["file\tentry\thash\tsound_name\tcue_names\tnch\tsfreq\tbytes"]
    total = 0
    for e, pay in capx.iter_payloads(args.archive):
        if pay[:4] != b"@UTF":
            continue
        root = cri_utf.parse(pay)
        for i, item in enumerate(sound_elements(root)):
            base = "%s_%04d_%08x_%02d_%s" % (stem, e.index, e.hash, i, safe(os.path.splitext(item["name"] or "snd")[0]))
            adx = os.path.join(outdir, base + ".adx")
            with open(adx, "wb") as o:
                o.write(item["data"])
            if args.wav:
                to_wav(adx, os.path.join(outdir, base + ".wav"))
            lines.append(
                "%s\t%d\t%08x\t%s\t%s\t%s\t%s\t%d"
                % (base + ".adx", e.index, e.hash, item["name"], ",".join(c or "" for c in item["cues"]),
                   item["nch"], item["sfreq"], len(item["data"]))
            )
            total += 1
    with open(os.path.join(outdir, "index_%s.tsv" % stem), "w") as idx:
        idx.write("\n".join(lines) + "\n")
    print("%s: wrote %d sound elements to %s" % (os.path.basename(args.archive), total, outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
