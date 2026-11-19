#!/usr/bin/env bash
# Build Tetra Studio: the Godot editor (from upstream/godot) plus the Tetra
# workspaces in modules/. Extra arguments are passed straight to SCons, e.g.
#   scripts/build.sh                 # incremental editor build
#   scripts/build.sh -j8 dev_build=yes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if command -v cygpath >/dev/null 2>&1; then
	ROOT="$(cygpath -m "$ROOT")" # MSYS2: SCons is a native program and needs C:/ paths
fi
GODOT="$ROOT/upstream/godot"

if [ ! -f "$GODOT/SConstruct" ]; then
	echo "upstream/godot is missing. Run scripts/fetch-upstream.sh first." >&2
	exit 1
fi

"$ROOT/scripts/apply-patches.sh"

JOBS="${TETRA_JOBS:-$(nproc)}"
PLATFORM="${TETRA_PLATFORM:-linuxbsd}"
LINKER_ARG=()
if [ "$PLATFORM" != linuxbsd ]; then
	: # cross builds use the toolchain's own linker
elif command -v mold >/dev/null 2>&1; then
	LINKER_ARG=(linker=mold)
elif command -v ld.lld >/dev/null 2>&1; then
	LINKER_ARG=(linker=lld)
fi

# The Windows editor is built in an MSYS2 UCRT64 shell (see docs/DEVELOPING.md).
# GEGL and MLT are DLLs there, so the C++ runtime cannot be linked statically
# (-static would make the linker skip their import libraries).
PLATFORM_ARGS=()
if [ "$PLATFORM" = windows ] && [ -n "${MSYSTEM:-}" ]; then
	PLATFORM_ARGS=(use_mingw=yes use_static_cpp=no d3d12=no)
fi

# Two SCons processes must never run in the same tree: queue behind a lock.
LOCK=()
if command -v flock >/dev/null 2>&1; then
	LOCK=(flock "${TMPDIR:-/tmp}/tetra-scons.lock")
fi

cd "$GODOT"
exec "${LOCK[@]}" scons \
	platform="$PLATFORM" \
	target=editor \
	debug_symbols=no \
	scu_build=yes \
	compiledb=yes \
	custom_modules="$ROOT/modules" \
	"${LINKER_ARG[@]}" \
	"${PLATFORM_ARGS[@]}" \
	-j"$JOBS" \
	"$@"
