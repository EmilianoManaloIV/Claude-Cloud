#!/usr/bin/env bash
# Apply the small patch set in patches/<name>/ to upstream/<name>.
# Idempotent: a patch that is already applied is skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

shopt -s nullglob
for dir in "$ROOT"/patches/*/; do
	name="$(basename "$dir")"
	target="$ROOT/upstream/$name"
	[ -d "$target" ] || continue
	for patch in "$dir"*.patch; do
		if git -C "$target" apply --reverse --check "$patch" >/dev/null 2>&1; then
			continue # already applied
		fi
		if git -C "$target" apply --check "$patch" >/dev/null 2>&1; then
			git -C "$target" apply "$patch"
			echo "applied $(basename "$patch") to upstream/$name"
		else
			echo "error: $(basename "$patch") does not apply to upstream/$name" >&2
			exit 1
		fi
	done
done
