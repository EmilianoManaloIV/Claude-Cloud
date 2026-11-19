#!/usr/bin/env bash
# Prove that a packaged Linux archive is self-contained:
#   scripts/package-verify.sh dist/tetra-studio-linux-x86_64.tar.gz
# Unpacks it into a fresh directory, runs scripts/package-smoke.gd through the
# launcher with an empty environment, and checks with the dynamic loader's own
# trace that GEGL, babl, MLT, ffmpeg, GLib and every plug-in were loaded from
# the archive and not from the system.
# Set TETRA_SMOKE_SKIP_BLENDER=1 for an archive packaged without Blender.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCHIVE="$(realpath "${1:?usage: package-verify.sh <tetra-studio-linux-x86_64.tar.gz>}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/unpack" "$WORK/home" "$WORK/project"
tar -xzf "$ARCHIVE" -C "$WORK/unpack"
APP="$(echo "$WORK"/unpack/*/)"
touch "$WORK/project/project.godot"
cp "$ROOT/scripts/package-smoke.gd" "$WORK/project/"

cd "$WORK"
status=0
env -i PATH=/usr/bin:/bin HOME="$WORK/home" \
	TETRA_SMOKE_SKIP_BLENDER="${TETRA_SMOKE_SKIP_BLENDER:-0}" \
	LD_DEBUG=libs LD_DEBUG_OUTPUT="$WORK/trace" \
	timeout 600 "${APP}tetra-studio" --headless --path "$WORK/project" --script res://package-smoke.gd \
	>"$WORK/smoke.log" 2>&1 || status=$?
grep -v -e '^$' "$WORK/smoke.log" || true
if [ "$status" -ne 0 ] || ! grep -q '^SMOKE OK' "$WORK/smoke.log"; then
	echo "verify: the smoke test failed (exit status $status)" >&2
	exit 1
fi

# Every shared object the editor process initialised, by path. Child
# processes (melt, Blender) write their own trace files and are not counted.
editor_trace="$(grep -l 'tetra-studio-editor' "$WORK"/trace.* | head -n 1)"
grep -oE 'calling init: \S+' "$editor_trace" | awk '{ print $3 }' | sort -u >"$WORK/loaded.txt"
total="$(wc -l <"$WORK/loaded.txt")"
bundled="$(grep -c "^$APP" "$WORK/loaded.txt" || true)"
echo "verify: $total shared objects loaded, $bundled from the archive; from the host:"
grep -v "^$APP" "$WORK/loaded.txt" | sed 's/^/  /'

leaked="$(grep -v "^$APP" "$WORK/loaded.txt" | grep -E 'gegl|babl|mlt|frei0r|libav|libsw|glib|gobject|gio|pixbuf|pango|cairo' || true)"
if [ -n "$leaked" ]; then
	echo "verify: these were loaded from the system instead of the archive:" >&2
	echo "$leaked" >&2
	exit 1
fi
for needed in libgegl-0.4 libbabl-0.1 libmlt-7 libmlt++-7 libavcodec lib/gegl-0.4/ lib/babl-0.1/ lib/mlt-7/libmltcore lib/mlt-7/libmltavformat lib/frei0r-1/; do
	if ! grep -q "^$APP.*$needed" "$WORK/loaded.txt"; then
		echo "verify: nothing matching '$needed' was loaded from the archive" >&2
		exit 1
	fi
done
echo "verify: OK - GEGL, babl, MLT, ffmpeg and their plug-ins all came from the archive"
