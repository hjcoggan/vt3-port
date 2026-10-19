#!/bin/bash
# Builds VT3.app: the desktop game binary, the icon, and a launcher that sets the data root.
# Usage: make_app.sh BINARY IMAGE_BIN OUT_DIR [DATA_DIR]
# DATA_DIR (the unpacked data2 folder) defaults to ~/Library/Application Support/VT3/data2.
# The data stays outside the app (VT3_GAME_ROOT points at the extracted English archives).
#
# The Vulkan loader (libvulkan) is linked dynamically, so the bundle carries its own copy in
# Contents/Frameworks, with MoltenVK and its ICD alongside it and the SPIR-V shader pack in
# Resources. A missing loader library would fail in dyld before main runs, which the OpenGL
# fallback cannot cover; a missing ICD at run time still falls back to OpenGL.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
binary="$1"; image="$2"; out="$3"
data="${4:-$HOME/Library/Application Support/VT3/data2}"
app="$out/VT3.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks" \
    "$app/Contents/Resources/vulkan/icd.d"
cp "$binary" "$app/Contents/MacOS/vt3-bin"
cp "$image" "$app/Contents/Resources/image.bin"
cp "$here/Info.plist" "$app/Contents/Info.plist"
cp "$here/../icon/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"

# Frameworks: SDL3 and the Vulkan loader follow the binary's absolute Homebrew paths;
# MoltenVK comes from the loader's default ICD location. Only direct non-system dependencies
# are bundled (otool -L shows no other Homebrew libraries for these three).
bundle_lib() {
    src="$1"; name="$(basename "$src")"
    # Resolve Homebrew symlinks to the real file so the bundle keeps working after upgrades.
    if [[ -L "$src" ]]; then src="$(cd "$(dirname "$src")" && pwd)/$(readlink "$src")"; fi
    cp -f "$src" "$app/Contents/Frameworks/$name"
    chmod 644 "$app/Contents/Frameworks/$name"
    echo "$app/Contents/Frameworks/$name"
}
sdl_ref="$(otool -L "$app/Contents/MacOS/vt3-bin" | awk '/libSDL3/{print $1; exit}')"
vulkan_ref="$(otool -L "$app/Contents/MacOS/vt3-bin" | awk '/libvulkan/{print $1; exit}')"
molten_src=""
for cand in /opt/homebrew/lib/libMoltenVK.dylib /opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib; do
    if [[ -f "$cand" ]]; then molten_src="$cand"; break; fi
done
if [[ -n "${sdl_ref:-}" ]]; then
    sdl_name="$(basename "$sdl_ref")"
    bundle_lib "$sdl_ref" >/dev/null
    install_name_tool -change "$sdl_ref" "@executable_path/../Frameworks/$sdl_name" "$app/Contents/MacOS/vt3-bin"
fi
if [[ -n "${vulkan_ref:-}" ]]; then
    vulkan_name="$(basename "$vulkan_ref")"
    bundle_lib "$vulkan_ref" >/dev/null
    install_name_tool -change "$vulkan_ref" "@executable_path/../Frameworks/$vulkan_name" "$app/Contents/MacOS/vt3-bin"
fi
if [[ -n "$molten_src" ]]; then
    bundle_lib "$molten_src" >/dev/null
    # The loader resolves a relative ICD library_path against the JSON file's directory,
    # mirroring the Homebrew layout (etc/vulkan/icd.d -> ../../../lib).
    cat > "$app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'ICD'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
ICD
else
    echo "warning: libMoltenVK.dylib not found; Vulkan will fall back to OpenGL" >&2
fi

# Shader pack: the Vulkan renderer needs the SPIR-V pack built from the game shaders
# (tools/formats/shaders/gen_spirv_pack.py). Prefer the caller's pack, else the tree default.
pack="${VT3_SHADER_PACK:-$repo/../dump/android/shaders.pack}"
if [[ -f "$pack" ]]; then
    cp -f "$pack" "$app/Contents/Resources/shaders.pack"
else
    echo "warning: shader pack not found at $pack; Vulkan will fail without it" >&2
fi
cat > "$app/Contents/MacOS/VT3" <<LAUNCH
#!/bin/bash
export VT3_GAME_ROOT="\${VT3_GAME_ROOT:-$data}"
LAUNCH
cat >> "$app/Contents/MacOS/VT3" <<'LAUNCH'
# Starts the game from the bundle's Resources folder, where image.bin is.
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here/Resources"
if [[ -f "$here/Resources/shaders.pack" ]]; then
    export VT3_SHADER_PACK="${VT3_SHADER_PACK:-$here/Resources/shaders.pack}"
fi
if [[ -f "$here/Resources/vulkan/icd.d/MoltenVK_icd.json" ]]; then
    export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$here/Resources/vulkan/icd.d/MoltenVK_icd.json}"
fi
exec "$here/MacOS/vt3-bin" "$@"
LAUNCH
chmod +x "$app/Contents/MacOS/VT3" "$app/Contents/MacOS/vt3-bin"
# Ad-hoc sign so the edited binary and bundled libraries launch on Apple Silicon.
codesign --force --deep --sign - "$app" 2>/dev/null || true
echo "built $app"
