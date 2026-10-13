#!/usr/bin/env python3
"""Convert CRI Sofdec (.sfd) movies to MP4 with ffmpeg.

A .sfd is an MPEG-1 program stream: one MPEG-1 video (PES 0xE0) and one CRI ADX
audio stream (PES 0xC0, adpcm_adx, 48 kHz stereo in the two movies on the disc).
Checked on Resource/Movie/vt3_a.sfd and ps3_a.sfd: no second audio track or
private substream exists, so there is no separate English track to select.

Usage:
    movie_convert.py SFD [OUTDIR] [--probe] [--crf N]

SFD      path to a .sfd file
OUTDIR   default: /Users/heath/Virtua Tennis 3/dump/extracted-en/movies
--probe  print the ffprobe stream summary and exit
--crf    H.264 quality (default 18; lower is better)

Output is H.264 (libx264, yuv420p) with AAC audio, for playback on macOS and Android.
"""
import argparse
import os
import shutil
import subprocess
import sys

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/movies"


def probe(path):
    subprocess.run(
        ["ffprobe", "-hide_banner", "-show_streams", "-show_format", path],
        check=False,
    )


def convert(path, outdir, crf):
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(path))[0]
    out = os.path.join(outdir, stem + ".mp4")
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", path,
        "-map", "0:v:0", "-map", "0:a:0?",
        "-c:v", "libx264", "-preset", "medium", "-crf", str(crf), "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k",
        "-movflags", "+faststart",
        out,
    ]
    subprocess.run(cmd, check=True)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sfd")
    ap.add_argument("outdir", nargs="?", default=DEFAULT_OUT)
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--crf", type=int, default=18)
    args = ap.parse_args(argv)
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        raise SystemExit("ffmpeg and ffprobe are required")
    if args.probe:
        probe(args.sfd)
        return 0
    out = convert(args.sfd, args.outdir, args.crf)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
