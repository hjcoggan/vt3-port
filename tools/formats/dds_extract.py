#!/usr/bin/env python3
"""Carve DDS textures out of CAPX (.xpac) archives.

Many texture bundles in game.xpac, stages.xpac and frontend.xpac hold standard
Direct3D 9 DDS files, either as the whole payload or embedded inside another
resource (for example a TPEX block in frontend.xpac). This tool finds each
"DDS " magic followed by a 124-byte header, computes the full byte size from
the header (all mip levels), and writes it out as a normal .dds file.

Supported pixel formats: DXT1, DXT3, DXT5 and uncompressed RGB/RGBA (8/16/24/32
bit). Numeric FourCC float formats (for example 0x71 = A16B16G16R16F) are sized
using their bytes per pixel. DX10 extended headers are reported and skipped.

Usage:
    dds_extract.py ARCHIVE [OUTDIR] [--png] [--limit N]

ARCHIVE   path to a .xpac file, for example .../data1/Resource/game.xpac
OUTDIR    default: /Users/heath/Virtua Tennis 3/dump/extracted-en/textures/<archive>
--png     also convert each DDS to PNG with ffmpeg (top mip only)
--limit   stop after N DDS files (for testing)

Writes OUTDIR/index.tsv with one line per file: output name, entry index,
entry hash, payload offset, dimensions, format, mip count, byte size.

Game data is never written inside the repository. Keep OUTDIR outside it.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capx  # noqa: E402

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/textures"
MAGIC = b"DDS \x7c\x00\x00\x00"
HEADER_BYTES = 128  # 4-byte magic + 124-byte header

DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_ALPHAPIXELS = 0x1
DDSD_MIPMAPCOUNT = 0x20000

# Block size for block-compressed FourCCs.
BLOCK_FOURCC = {b"DXT1": 8, b"DXT2": 16, b"DXT3": 16, b"DXT4": 16, b"DXT5": 16, b"ATI1": 8, b"ATI2": 16, b"BC4U": 8, b"BC5U": 16}
# Bytes per pixel for numeric FourCCs (D3DFORMAT values), uncompressed.
PPX_NUMERIC = {36: 8, 110: 4, 111: 2, 112: 4, 113: 8, 114: 4, 115: 8, 116: 16}


def parse_dds(pay, pos):
    """Return a dict describing the DDS at pay[pos:], or None if it is not usable."""
    if len(pay) < pos + HEADER_BYTES:
        return None
    size = struct.unpack_from("<I", pay, pos + 4)[0]
    if size != 124:
        return None
    flags, height, width, pitch = struct.unpack_from("<IIII", pay, pos + 8)
    mips = struct.unpack_from("<I", pay, pos + 28)[0]
    pf_flags = struct.unpack_from("<I", pay, pos + 80)[0]
    fourcc = pay[pos + 84 : pos + 88]
    bpp_bits = struct.unpack_from("<I", pay, pos + 88)[0]
    if height == 0 or width == 0 or height > 16384 or width > 16384:
        return None
    levels = mips if (flags & DDSD_MIPMAPCOUNT) and mips > 0 else 1
    info = {
        "width": width,
        "height": height,
        "levels": levels,
        "pitch": pitch,
        "pf_flags": pf_flags,
        "fourcc": fourcc.decode("latin-1"),
        "bits": bpp_bits,
        "dx10": fourcc == b"DX10",
    }
    if pf_flags & DDPF_FOURCC:
        if fourcc in BLOCK_FOURCC:
            block = BLOCK_FOURCC[fourcc]
            total = 0
            w, h = width, height
            for _ in range(levels):
                total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
                w, h = max(1, w // 2), max(1, h // 2)
            info["format"] = fourcc.decode("latin-1")
        elif fourcc == b"DX10":
            info["format"] = "DX10"
            return info  # extended header not handled; caller skips it
        else:
            numeric = struct.unpack("<I", fourcc)[0]
            bpp = PPX_NUMERIC.get(numeric)
            if bpp is None:
                info["format"] = "fourcc_%08x" % numeric
                return info
            total = 0
            w, h = width, height
            for _ in range(levels):
                total += w * h * bpp
                w, h = max(1, w // 2), max(1, h // 2)
            info["format"] = "numeric_%d" % numeric
    else:
        bpp = (bpp_bits + 7) // 8
        if bpp not in (1, 2, 3, 4):
            info["format"] = "rgb_bits_%d" % bpp_bits
            return info
        total = 0
        w, h = width, height
        for _ in range(levels):
            total += w * h * bpp
            w, h = max(1, w // 2), max(1, h // 2)
        info["format"] = "rgb%d" % bpp_bits
    info["bytes"] = total
    info["total_size"] = HEADER_BYTES + total
    return info


def to_png(dds_path, png_path):
    """Top mip to PNG via ffmpeg. Float and unknown FourCCs are not decodable by ffmpeg."""
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not found")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", dds_path, "-frames:v", "1", png_path],
        check=False,
    )


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--png", action="store_true", help="also write PNG (needs ffmpeg)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N DDS files")
    args = ap.parse_args(argv)

    stem = capx.archive_stem(args.archive)
    outdir = args.outdir or os.path.join(DEFAULT_OUT, stem)
    os.makedirs(outdir, exist_ok=True)
    index_lines = ["name\tentry\thash\toffset\twidth\theight\tlevels\tformat\tbytes"]
    written = 0
    skipped = 0
    warnings_pitch = 0

    for e, pay in capx.iter_payloads(args.archive):
        pos = pay.find(MAGIC)
        n = 0
        while pos != -1:
            info = parse_dds(pay, pos)
            if info is None or "bytes" not in info:
                skipped += 1
                pos = pay.find(MAGIC, pos + 4)
                continue
            end = pos + info["total_size"]
            if end > len(pay):
                skipped += 1
                pos = pay.find(MAGIC, pos + 4)
                continue
            name = "%s_%04d_%08x_%02d_%dx%d_%s.dds" % (
                stem, e.index, e.hash, n, info["width"], info["height"], info["format"],
            )
            path = os.path.join(outdir, name)
            with open(path, "wb") as o:
                o.write(pay[pos:end])
            index_lines.append(
                "%s\t%d\t%08x\t%d\t%d\t%d\t%d\t%s\t%d"
                % (name, e.index, e.hash, e.offset + pos, info["width"], info["height"],
                   info["levels"], info["format"], end - pos)
            )
            png_ok = info["format"] in ("DXT1", "DXT3", "DXT5") or info["format"].startswith("rgb")
            if args.png and png_ok:
                to_png(path, path[:-4] + ".png")
            if info["format"] in ("DXT1", "DXT3", "DXT5"):
                top = max(1, (info["width"] + 3) // 4) * max(1, (info["height"] + 3) // 4) * BLOCK_FOURCC[info["format"].encode()]
                if top != info["pitch"]:
                    warnings_pitch += 1
            written += 1
            n += 1
            if args.limit and written >= args.limit:
                break
            pos = pay.find(MAGIC, end)
        if args.limit and written >= args.limit:
            break

    with open(os.path.join(outdir, "index.tsv"), "w") as idx:
        idx.write("\n".join(index_lines) + "\n")
    print("%s: wrote %d DDS files to %s (skipped %d, pitch mismatches %d)"
          % (os.path.basename(args.archive), written, outdir, skipped, warnings_pitch))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
