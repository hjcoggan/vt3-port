#!/bin/bash
# Rebuilds every icon asset from vt3_logo_icon.svg: the macOS master and AppIcon.icns, and the Android
# adaptive-icon foreground PNG. Needs qlmanage, swift and sips (macOS). Run from any folder.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
qlmanage -t -s 1024 -o "$work" "$here/vt3_logo_icon.svg" >/dev/null 2>&1
swift "$here/mask_icon.swift" "$work/vt3_logo_icon.svg.png" "$here/vt3_logo_icon_1024.png"
"$here/make_icns.sh" "$here/vt3_logo_icon_1024.png" "$here/AppIcon.icns"
python3 "$here/make_android_foreground.py" "$work/android_foreground.svg"
qlmanage -t -s 1024 -o "$work" "$work/android_foreground.svg" >/dev/null 2>&1
sips -z 432 432 "$work/android_foreground.svg.png" --out "$repo/android/game/res/drawable-nodpi/ic_launcher_foreground.png" >/dev/null
rm -rf "$work"
echo "icon assets rebuilt"
