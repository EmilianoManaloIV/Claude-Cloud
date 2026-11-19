#!/usr/bin/env bash
# Package a built editor into dist/ as a downloadable archive.
#   scripts/package.sh linux   upstream/godot/bin/godot.linuxbsd.editor.x86_64
#   scripts/package.sh windows upstream/godot/bin/godot.windows.editor.x86_64.exe
# The archive holds the editor, Kdenlive's effect and render-preset catalogs
# (read at run time by the Video workspace), the licence and a short README.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:?usage: package.sh <linux|windows> <binary>}"
BINARY="$(realpath "${2:?usage: package.sh <linux|windows> <binary>}")"
STAGE="$ROOT/dist/tetra-studio-$NAME"

rm -rf "$STAGE"
mkdir -p "$STAGE/kdenlive-data"
if [ "$NAME" = windows ]; then
	cp "$BINARY" "$STAGE/TetraStudio.exe"
else
	cp "$BINARY" "$STAGE/tetra-studio"
	chmod +x "$STAGE/tetra-studio"
fi
for item in effects transitions kdenliveeffectscategory.rc profiles.xml; do
	if [ -e "$ROOT/upstream/kdenlive/data/$item" ]; then
		cp -r "$ROOT/upstream/kdenlive/data/$item" "$STAGE/kdenlive-data/"
	fi
done
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/docs/DOWNLOAD.md" "$STAGE/README.md"

cd "$ROOT/dist"
if [ "$NAME" = windows ]; then
	zip -qr "tetra-studio-$NAME.zip" "tetra-studio-$NAME"
else
	tar -czf "tetra-studio-$NAME.tar.gz" "tetra-studio-$NAME"
fi
rm -rf "$STAGE"
ls -la "$ROOT/dist"
