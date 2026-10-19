#!/usr/bin/env bash
# Push the game data onto a device and make it readable by the app.
#
# The APK reads its archives from /sdcard/Android/data/net.vt3port.game/files (VT3_ANDROID_ROOT):
#   image.bin, shaders.pack, vt3.env and the data2/ tree.
#
# adb push creates the data2 directories owned by shell:ext_data_rw with mode 2770. On devices whose
# app processes do not get the ext_data_rw supplementary group (seen on a Galaxy S26, Android 16)
# the app can traverse its own files/ dir but not data2/, so every XPAC stat fails with EACCES and the
# game reports the archives as missing. The chmod at the end fixes that; run it after every push.
#
# Usage: deploy_data.sh SERIAL GAME_DATA_DIR [IMAGE_BIN] [SHADERS_PACK] [VT3_ENV]
#   GAME_DATA_DIR  the data2 directory (holds Resource/, Root/, ConfigText/, ...)
#   IMAGE_BIN      defaults to <repo>/../dump/recomp/vt3_nocd/image.bin
#   SHADERS_PACK   defaults to <repo>/../dump/android/shaders.pack
#   VT3_ENV        defaults to <repo>/../dump/android/vt3.env
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DUMP="$(dirname "$REPO")/dump"
ADB="${ADB:-adb}"
PKG=net.vt3port.game
DEST=/sdcard/Android/data/$PKG/files

if [ $# -lt 2 ]; then
    sed -n '2,/^set -euo/{/^set -euo/!p}' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

serial=$1
data_dir=${2%/}
image=${3:-$DUMP/recomp/vt3_nocd/image.bin}
pack=${4:-$DUMP/android/shaders.pack}
env_file=${5:-$DUMP/android/vt3.env}

for f in "$data_dir" "$image" "$pack"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

echo "pushing image.bin"
"$ADB" -s "$serial" push "$image" "$DEST/image.bin" >/dev/null
echo "pushing shaders.pack"
"$ADB" -s "$serial" push "$pack" "$DEST/shaders.pack" >/dev/null
if [ -e "$env_file" ]; then
    echo "pushing vt3.env"
    "$ADB" -s "$serial" push "$env_file" "$DEST/vt3.env" >/dev/null
fi

echo "pushing data2 (this is the large one)"
"$ADB" -s "$serial" push "$data_dir" "$DEST/" >/dev/null

# adb push leaves the tree as shell:ext_data_rw 2770. The app may not be in that group, so it cannot
# traverse data2/ (every XPAC stat fails with EACCES). Give world read+execute on dirs and read on
# files. a+rwX (not just a+rX) so the shim can also write vt3.ini (its widescreen config) directly
# into data2/ on devices where the app is not in ext_data_rw.
echo "making data2 world-accessible (read + write)"
# Only data2 needs fixing: files/ is already the app's own directory (owned by its uid). The FUSE
# sdcard rejects chmod on files/ itself ("Operation not permitted"), so do not touch the parent.
"$ADB" -s "$serial" shell "chmod -R a+rwX $DEST/data2"

"$ADB" -s "$serial" shell "ls -ld $DEST/data2 $DEST/data2/Resource"
echo "done"
