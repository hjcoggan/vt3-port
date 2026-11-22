#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
xdg_state="${XDG_STATE_HOME:-$HOME/.local/state}"
default_data="$xdg_data/vt3/data2"
export VT3_GAME_ROOT="${VT3_GAME_ROOT:-$default_data}"
export VT3_SAVE_DIR="${VT3_SAVE_DIR:-$xdg_state/vt3}"
export VT3_SHADER_PACK="${VT3_SHADER_PACK:-$here/libexec/vt3/shaders.pack}"
mkdir -p "$VT3_SAVE_DIR"
# The game opens .\vt3_save.bin relative to the working directory.
cd "$VT3_SAVE_DIR"
exec "$here/libexec/vt3/vt3-bin" "$here/libexec/vt3/image.bin" "$@"
