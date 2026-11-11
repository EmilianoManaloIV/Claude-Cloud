#!/usr/bin/env bash
# Install Blender as a Python module (bpy) into build/blender-venv so the
# Blender bridge works without a separate Blender installation. The bpy
# version matches the Blender tag pinned in upstream/blender.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$ROOT/build/blender-venv"
BPY_VERSION="${TETRA_BPY_VERSION:-5.3.0}"
PYTHON="${TETRA_PYTHON:-python3.13}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
	echo "bpy $BPY_VERSION needs Python 3.13; set TETRA_PYTHON to a 3.13 interpreter." >&2
	exit 1
fi

"$PYTHON" -m venv "$VENV"
"$VENV/bin/pip" install --quiet "bpy==$BPY_VERSION"
"$VENV/bin/python" -c 'import bpy; print("bpy", bpy.app.version_string, "ready in", bpy.__path__[0] if hasattr(bpy, "__path__") else "venv")'
