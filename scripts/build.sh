#!/usr/bin/env bash
# Build Tetra Studio: the Godot editor (from upstream/godot) plus the Tetra
# workspaces in modules/. Extra arguments are passed straight to SCons, e.g.
#   scripts/build.sh                 # incremental editor build
#   scripts/build.sh -j8 dev_build=yes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GODOT="$ROOT/upstream/godot"

if [ ! -f "$GODOT/SConstruct" ]; then
	echo "upstream/godot is missing. Run scripts/fetch-upstream.sh first." >&2
	exit 1
fi

"$ROOT/scripts/apply-patches.sh"

JOBS="${TETRA_JOBS:-$(nproc)}"
LINKER_ARG=()
if command -v mold >/dev/null 2>&1; then
	LINKER_ARG=(linker=mold)
elif command -v ld.lld >/dev/null 2>&1; then
	LINKER_ARG=(linker=lld)
fi

cd "$GODOT"
exec scons \
	platform=linuxbsd \
	target=editor \
	debug_symbols=no \
	scu_build=yes \
	compiledb=yes \
	custom_modules="$ROOT/modules" \
	"${LINKER_ARG[@]}" \
	-j"$JOBS" \
	"$@"
