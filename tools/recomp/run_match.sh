#!/bin/bash
# Run vt3_native to a match and take window captures.
#
# usage: run_match.sh BIN OUTDIR SECS SHOT_TIMES...
#   BIN         a vt3_native build (see build_native.py)
#   OUTDIR      receives out.txt, err.txt (the game's log), run_info.txt and shot_<t>.png
#   SECS        how long the game runs before it is killed
#   SHOT_TIMES  seconds after launch at which to capture the window
# Environment variables pass through to the game (VT3_TRACE_*, VT3_SKIN_CHECK, ...).
#
# Default route: Tournament, the menu path from the title screen to a match (Düsseldorf, Federer as
# 1P). It walks the same menu path each time; the opponent can differ from run to run.
# Quick Play (F1) picks a random match on each launch, so it is opt-in: QUICK_PLAY=1 run_match.sh ...
# An explicit VT3_INPUT_SCRIPT overrides both routes.
#
# The game data lives outside the repo, in ../dump (set VT3_DUMP to move it).
BIN=$1; OUT=$2; SECS=$3; shift 3
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DUMP="${VT3_DUMP:-$(dirname "$REPO")/dump}"
TOURNAMENT_SCRIPT="28:DOWN,29:RETURN,34:RETURN,40:RETURN,46:RETURN,62:Z,70:Z"
QUICK_PLAY_SCRIPT="12:RETURN,16:BACKSPACE,20:F1"
if [[ -z "$VT3_INPUT_SCRIPT" ]]; then
  if [[ -n "$QUICK_PLAY" ]]; then VT3_INPUT_SCRIPT="$QUICK_PLAY_SCRIPT"; else VT3_INPUT_SCRIPT="$TOURNAMENT_SCRIPT"; fi
fi
mkdir -p "$OUT"; rm -f "$OUT"/shot_*.png
export VT3_GAME_ROOT="$DUMP/pc/extracted/data2"
export VT3_INPUT_SCRIPT
printf 'binary: %s\ninput script: %s\nshot times: %s\n' "$BIN" "$VT3_INPUT_SCRIPT" "$*" > "$OUT/run_info.txt"
"$BIN" "$DUMP/recomp/vt3_nocd/image.bin" >"$OUT/out.txt" 2>"$OUT/err.txt" &
PID=$!
start=$(date +%s)
for t in "$@"; do
  while (( $(date +%s) - start < t )); do sleep 0.5; done
  W=$("$DUMP/recomp/diagnosis/input/winid" $PID | head -1 | cut -d' ' -f1)
  [[ -n "$W" ]] && screencapture -x -o -l $W "$OUT/shot_${t}.png"
done
while (( $(date +%s) - start < SECS )); do sleep 1; done
kill -9 $PID 2>/dev/null
echo "done $OUT"
