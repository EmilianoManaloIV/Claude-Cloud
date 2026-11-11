#!/usr/bin/env bash
# Fetch the pinned upstream sources (git submodules, shallow).
#   scripts/fetch-upstream.sh            # everything
#   scripts/fetch-upstream.sh godot mlt  # only what the build itself needs
#
# Only upstream/godot is required to build. The others are the reference
# sources the Tetra workspaces were ported from (and, for mlt/gegl, optional
# from-source engine builds).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Blender's mirror carries LFS pointers we do not need.
export GIT_LFS_SKIP_SMUDGE=1

if [ "$#" -eq 0 ]; then
	set -- godot blender gimp kdenlive mlt gegl
fi

for name in "$@"; do
	git submodule update --init --depth 1 "upstream/$name"
done

"$ROOT/scripts/apply-patches.sh"
