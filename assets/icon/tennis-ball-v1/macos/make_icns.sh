#!/bin/bash
# Builds AppIcon.icns from a 1024 px master PNG (transparent outside the rounded square).
# Usage: make_icns.sh MASTER.png OUT.icns
set -euo pipefail
master="$1"; out="$2"
work="$(mktemp -d)"
set_="$work/AppIcon.iconset"
mkdir -p "$set_"
for base in 16 32 128 256 512; do
  sips -z "$base" "$base" "$master" --out "$set_/icon_${base}x${base}.png" >/dev/null
  sips -z "$((base * 2))" "$((base * 2))" "$master" --out "$set_/icon_${base}x${base}@2x.png" >/dev/null
done
iconutil -c icns "$set_" -o "$out"
rm -rf "$work"
echo "wrote $out"
