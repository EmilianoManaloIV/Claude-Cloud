#!/usr/bin/env bash
# Launch Tetra Studio. Arguments are passed to the editor binary, e.g.
#   scripts/run.sh                          # project manager
#   scripts/run.sh --editor --path my_game  # open a project
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/upstream/godot/bin/godot.linuxbsd.editor.x86_64"

if [ ! -x "$BIN" ]; then
	echo "Editor binary not found. Run scripts/build.sh first." >&2
	exit 1
fi

# Let the Blender bridge find the bpy virtualenv created by setup-blender-bridge.sh.
if [ -z "${TETRA_BLENDER_PYTHON:-}" ] && [ -x "$ROOT/build/blender-venv/bin/python" ]; then
	export TETRA_BLENDER_PYTHON="$ROOT/build/blender-venv/bin/python"
fi

exec "$BIN" "$@"
