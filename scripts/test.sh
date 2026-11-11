#!/usr/bin/env bash
# Run the headless tests in tests/godot/project/<module>/test_*.gd.
#   scripts/test.sh                # all modules
#   scripts/test.sh tetra_paint    # one module
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/upstream/godot/bin/godot.linuxbsd.editor.x86_64"
PROJECT="$ROOT/tests/godot/project"

if [ ! -x "$BIN" ]; then
	echo "Editor binary not found. Run scripts/build.sh first." >&2
	exit 1
fi
if [ -z "${TETRA_BLENDER_PYTHON:-}" ] && [ -x "$ROOT/build/blender-venv/bin/python" ]; then
	export TETRA_BLENDER_PYTHON="$ROOT/build/blender-venv/bin/python"
fi
export TETRA_REPO_ROOT="$ROOT"

shopt -s nullglob
failed=0
count=0
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
for test in "$PROJECT"/${1:-*}/test_*.gd; do
	rel="${test#"$PROJECT"/}"
	count=$((count + 1))
	timeout "${TETRA_TEST_TIMEOUT:-600}" "$BIN" --headless --path "$PROJECT" --script "res://$rel" >"$log" 2>&1
	status=$?
	grep -v -e '^Godot Engine' -e '^Tetra Studio v' -e '^$' "$log"
	# A script error aborts run() without failing a check, so the exit status
	# alone is not enough: also require a clean log and at least one check.
	if [ "$status" -ne 0 ] || grep -q -e 'SCRIPT ERROR' -e 'Parse Error' "$log" \
		|| ! grep -q -E ': [1-9][0-9]* checks, 0 failed' "$log"; then
		echo "FAIL  $rel"
		failed=$((failed + 1))
	else
		echo "ok    $rel"
	fi
done

echo "$count test files, $failed failed"
[ "$failed" -eq 0 ]
