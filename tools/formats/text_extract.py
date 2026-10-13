#!/usr/bin/env python3
"""Convert the PC release's text files to UTF-8.

Formats handled:
    *.TXT / *.txt  UTF-16LE with BOM (FF FE) and CRLF line endings
                   (ConfigText/*.TXT and similar). Written as UTF-8 with LF.
    *.ini          plain text INI (default.ini, corecomp.ini, FontData.ini).
                   Copied as UTF-8 if it decodes; else decoded as cp1252.

Not handled here:
    *.ips          InstallShield string tables (the installer's own UI text).
                   Their layout is not decoded yet. Game strings are not in this form.

Usage:
    text_extract.py FILE [FILE ...] [--outdir DIR]

Default OUTDIR: /Users/heath/Virtua Tennis 3/dump/extracted-en/text
Only English content is converted; other language files are skipped by the caller.
"""
import argparse
import os
import sys

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/text"


def decode(data):
    if data[:2] == b"\xff\xfe":
        return data[2:].decode("utf-16-le")
    if data[:2] == b"\xfe\xff":
        return data[2:].decode("utf-16-be")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("cp1252", "replace")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--outdir", default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    os.makedirs(args.outdir, exist_ok=True)
    for path in args.files:
        with open(path, "rb") as f:
            text = decode(f.read())
        text = text.replace("\r\n", "\n")
        dst = os.path.join(args.outdir, os.path.basename(path) + ".utf8.txt")
        with open(dst, "w", encoding="utf-8") as o:
            o.write(text)
        print("%s -> %s (%d chars)" % (os.path.basename(path), dst, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
