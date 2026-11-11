#!/usr/bin/env bash
# Capture the editor window without a display (Xvfb + software OpenGL).
#   scripts/screenshot.sh <out.png> [main screen: 2D|3D|Script|Image|Video] [driver.gd] [wait seconds]
# The optional driver is a res:// path inside tests/godot/project (see
# tests/godot/project/tools/editor_shot.gd).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/upstream/godot/bin/godot.linuxbsd.editor.x86_64"
PROJECT="${TETRA_SHOT_PROJECT:-$ROOT/tests/godot/project}"
OUT="$(realpath -m "${1:?usage: screenshot.sh <out.png> [screen] [driver] [wait]}")"
SIZE="${TETRA_SHOT_SIZE:-1600x900}"

mkdir -p "$(dirname "$OUT")"
if [ -z "${TETRA_BLENDER_PYTHON:-}" ] && [ -x "$ROOT/build/blender-venv/bin/python" ]; then
	export TETRA_BLENDER_PYTHON="$ROOT/build/blender-venv/bin/python"
fi
export TETRA_REPO_ROOT="$ROOT"

timeout "${TETRA_SHOT_TIMEOUT:-180}" xvfb-run -a -s "-screen 0 ${SIZE}x24" "$BIN" \
	--editor --path "$PROJECT" --rendering-driver opengl3 --audio-driver Dummy --resolution "$SIZE" \
	--script res://tools/editor_shot.gd -- "--out=$OUT" "--screen=${2:-}" "--driver=${3:-}" "--wait=${4:-2}" 2>&1 \
	| grep -e '^screenshot ' -e 'SCRIPT ERROR' -e 'Parse Error' -e '^ERROR: .*tetra' || true
[ -f "$OUT" ]
