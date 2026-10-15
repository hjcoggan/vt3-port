#!/bin/bash
# Builds VT3.app: the desktop game binary, the icon, and a launcher that sets the data root.
# Usage: make_app.sh BINARY IMAGE_BIN OUT_DIR
# The data stays outside the app (VT3_GAME_ROOT points at the extracted English archives).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
binary="$1"; image="$2"; out="$3"
app="$out/VT3.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$binary" "$app/Contents/MacOS/vt3-bin"
cp "$image" "$app/Contents/Resources/image.bin"
cp "$here/Info.plist" "$app/Contents/Info.plist"
cp "$here/../icon/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
cat > "$app/Contents/MacOS/VT3" <<'LAUNCH'
#!/bin/bash
# Starts the game from the bundle's Resources folder, where image.bin is.
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here/Resources"
export VT3_GAME_ROOT="${VT3_GAME_ROOT:-/Users/heath/Virtua Tennis 3/dump/pc/extracted/data2}"
exec "$here/MacOS/vt3-bin" "$@"
LAUNCH
chmod +x "$app/Contents/MacOS/VT3" "$app/Contents/MacOS/vt3-bin"
echo "built $app"
