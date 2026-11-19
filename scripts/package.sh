#!/usr/bin/env bash
# Package a built editor into dist/ as a self-contained, relocatable archive:
# the editor plus every library, plug-in and program its workspaces use, so a
# user unpacks one archive and runs it without installing anything else.
#
#   scripts/package.sh linux   upstream/godot/bin/godot.linuxbsd.editor.x86_64
#   scripts/package.sh windows upstream/godot/bin/godot.windows.editor.x86_64.exe
#
# linux    runs on the build machine (GEGL, MLT, melt and frei0r installed);
#          writes dist/tetra-studio-linux-x86_64.tar.gz
# windows  runs in an MSYS2 UCRT64 shell with the same packages and ntldd;
#          writes dist/tetra-studio-windows-x86_64.zip
#
# Environment:
#   TETRA_SKIP_BLENDER=1       leave Blender out (the bridge then needs one installed)
#   TETRA_BLENDER_ARCHIVE=file use this Blender portable archive instead of downloading
#   TETRA_BLENDER_VERSION      Blender release to bundle (default 5.3.0)
#   TETRA_MLT_SKIP="a b"       MLT modules left out (default: the Qt, OpenCV and GPU ones)
#   TETRA_KEEP_STAGE=1         keep the unpacked tree in dist/ next to the archive
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USAGE="usage: package.sh <linux|windows> <binary>"
NAME="${1:?$USAGE}"
BINARY="$(realpath "${2:?$USAGE}")"
STAGE="$ROOT/dist/tetra-studio-$NAME-x86_64"
BLENDER_VERSION="${TETRA_BLENDER_VERSION:-5.3.0}"
# MLT modules that are not shipped: Qt and OpenCV would add several hundred
# megabytes, movit needs a GPU context, and SDL 1 is superseded by SDL 2. The
# Video workspace prefers the services of the modules that are shipped
# (frei0r.cairoblend, pixbuf, pango).
MLT_SKIP="${TETRA_MLT_SKIP:-qt qt6 glaxnimate glaxnimate-qt6 opencv movit sdl}"

log() { echo "package: $*" >&2; }

# ---------------------------------------------------------------- shared ----

copy_common() {
	mkdir -p "$STAGE/kdenlive-data"
	local item
	for item in effects transitions kdenliveeffectscategory.rc profiles.xml; do
		if [ -e "$ROOT/upstream/kdenlive/data/$item" ]; then
			cp -r "$ROOT/upstream/kdenlive/data/$item" "$STAGE/kdenlive-data/"
		fi
	done
	[ -d "$STAGE/kdenlive-data/effects" ] || { log "upstream/kdenlive/data is missing (scripts/fetch-upstream.sh kdenlive)"; exit 1; }
	cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
	cp "$ROOT/docs/DOWNLOAD.md" "$STAGE/README.md"
	cp "$ROOT/THIRD_PARTY.md" "$STAGE/THIRD_PARTY.md"
}

# fetch_blender <linux-x64.tar.xz|windows-x64.zip>: prints the archive path.
fetch_blender() {
	local file="blender-$BLENDER_VERSION-$1"
	if [ -n "${TETRA_BLENDER_ARCHIVE:-}" ]; then
		echo "$TETRA_BLENDER_ARCHIVE"
		return
	fi
	local cache="$ROOT/build/downloads"
	mkdir -p "$cache"
	if [ ! -s "$cache/$file" ]; then
		local series="${BLENDER_VERSION%.*}" base ok=0
		for base in \
			"https://download.blender.org/release/Blender$series" \
			"https://mirrors.ocf.berkeley.edu/blender/release/Blender$series" \
			"https://mirror.clarkson.edu/blender/release/Blender$series"; do
			log "downloading $base/$file"
			if curl -fL --retry 3 --retry-delay 5 -sS -o "$cache/$file.part" "$base/$file"; then
				mv "$cache/$file.part" "$cache/$file"
				ok=1
				break
			fi
		done
		if [ "$ok" != 1 ]; then
			log "could not download Blender $BLENDER_VERSION. Set TETRA_BLENDER_ARCHIVE to a downloaded $file, or TETRA_SKIP_BLENDER=1 to package without Blender."
			exit 1
		fi
	fi
	echo "$cache/$file"
}

mlt_module_skipped() {
	local module="$1" skip
	module="${module#libmlt}"
	module="${module%%.*}"
	for skip in $MLT_SKIP; do
		[ "$module" = "$skip" ] && return 0
	done
	return 1
}

# ----------------------------------------------------------------- linux ----

# Libraries that must come from the host, as on the AppImage exclude list: the
# C library and the compiler runtime (a bundled libstdc++ older than the
# host's breaks the host's GPU drivers), everything that talks to the GPU
# driver or the display server, ALSA (tied to the host's configuration), and
# zlib/expat, which the host's Mesa drivers load into the same process.
LINUX_HOST_LIBS='^(ld-linux.*|linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libutil|libnsl|libanl|libcrypt|libBrokenLocale|libnss_.*|libthread_db|libmvec|libstdc\+\+|libgcc_s|libGL|libEGL|libGLX.*|libGLdispatch|libOpenGL|libGLESv2|libgbm|libdrm.*|libglapi|libvulkan|libX11|libX11-xcb|libxcb.*|libwayland-.*|libasound|libz|libexpat)\.so'

# Copy into lib/ every library the given files need, recursively (ldd lists
# the whole closure), except the host libraries above.
linux_bundle_deps() {
	local path name
	ldd "$@" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | sort -u | while read -r path; do
		name="$(basename "$path")"
		if [[ "$name" =~ $LINUX_HOST_LIBS ]] || [ -e "$STAGE/lib/$name" ]; then
			continue
		fi
		case "$path" in "$STAGE"/*) continue ;; esac
		cp -L "$path" "$STAGE/lib/$name"
		chmod 644 "$STAGE/lib/$name"
	done
}

package_linux() {
	local libdir mlt_lib mlt_data frei0r melt pixbuf_dir
	libdir="$(pkg-config --variable=libdir gegl-0.4)"
	mlt_lib="$(pkg-config --variable=moduledir mlt-framework-7 2>/dev/null || true)"
	mlt_data="$(pkg-config --variable=mltdatadir mlt-framework-7 2>/dev/null || true)"
	[ -d "$mlt_lib" ] || mlt_lib="$libdir/mlt-7"
	[ -d "$mlt_data" ] || mlt_data="/usr/share/mlt-7"
	frei0r="$(for d in /usr/lib/frei0r-1 "$libdir/frei0r-1" /usr/lib64/frei0r-1; do [ -d "$d" ] && echo "$d" && break; done || true)"
	melt="$(command -v melt-7 || command -v melt || true)"
	command -v patchelf >/dev/null || { log "patchelf is missing (apt install patchelf)"; exit 1; }
	for required in "$libdir/gegl-0.4" "$libdir/babl-0.1" "$mlt_lib" "$mlt_data" "$frei0r" "$melt"; do
		[ -e "$required" ] || { log "missing on this machine: install libgegl, libmlt++, melt and frei0r-plugins"; exit 1; }
	done

	mkdir -p "$STAGE/bin" "$STAGE/lib/mlt-7" "$STAGE/share"
	cp "$BINARY" "$STAGE/bin/tetra-studio-editor"
	cp -L "$melt" "$STAGE/bin/melt"
	chmod 755 "$STAGE/bin/tetra-studio-editor" "$STAGE/bin/melt"

	# Plug-in directories. GEGL, babl and MLT find them through the
	# environment variables the launcher sets.
	cp -rL "$libdir/gegl-0.4" "$libdir/babl-0.1" "$STAGE/lib/"
	cp -rL "$frei0r" "$STAGE/lib/frei0r-1"
	cp -rL "$mlt_data" "$STAGE/share/mlt-7"
	local module
	for module in "$mlt_lib"/*.so; do
		if mlt_module_skipped "$(basename "$module")"; then
			log "leaving out MLT module $(basename "$module")"
		else
			cp -L "$module" "$STAGE/lib/mlt-7/"
		fi
	done

	# Image loaders for MLT's pixbuf producer (SVG, TIFF, GIF, ...). Their
	# index holds absolute paths, so the launcher rebuilds it on each start.
	pixbuf_dir="$(pkg-config --variable=gdk_pixbuf_binarydir gdk-pixbuf-2.0 2>/dev/null || true)"
	local query="$libdir/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"
	if [ -d "$pixbuf_dir/loaders" ] && [ -x "$query" ]; then
		mkdir -p "$STAGE/lib/gdk-pixbuf"
		cp -rL "$pixbuf_dir/loaders" "$STAGE/lib/gdk-pixbuf/"
		cp -L "$query" "$STAGE/bin/gdk-pixbuf-query-loaders"
	fi

	log "collecting shared libraries"
	local files=()
	while IFS= read -r -d '' file; do files+=("$file"); done \
		< <(find "$STAGE/bin" "$STAGE/lib" -type f \( -name '*.so' -o -perm -u+x \) -print0)
	linux_bundle_deps "${files[@]}"

	# A plug-in whose libraries could not be found would fail to load on the
	# user's machine too: say so now.
	local unresolved
	unresolved="$(LD_LIBRARY_PATH="$STAGE/lib" ldd "${files[@]}" 2>/dev/null | grep 'not found' | sort -u || true)"
	if [ -n "$unresolved" ]; then
		log "warning: unresolved libraries:"
		echo "$unresolved" >&2
	fi

	# Each bundled program, library and plug-in finds lib/ through its own
	# RUNPATH. The launcher deliberately does not set LD_LIBRARY_PATH: child
	# processes would inherit it, and Blender (or a Python with bpy) must load
	# its own copies of libraries such as TBB, not the ones in lib/.
	log "setting RUNPATH on $(( ${#files[@]} )) programs and plug-ins and on the libraries"
	local elf rel
	while IFS= read -r -d '' elf; do
		[ "$(head -c 4 "$elf" | tail -c 3)" = ELF ] || continue
		rel="$(realpath --relative-to="$(dirname "$elf")" "$STAGE/lib")"
		patchelf --set-rpath "\$ORIGIN/$rel" "$elf"
	done < <(find "$STAGE/bin" "$STAGE/lib" -type f -print0)

	if [ "${TETRA_SKIP_BLENDER:-0}" = 1 ]; then
		log "TETRA_SKIP_BLENDER=1: packaging without Blender"
	else
		local archive
		archive="$(fetch_blender linux-x64.tar.xz)"
		mkdir -p "$STAGE/blender"
		tar -xJf "$archive" -C "$STAGE/blender" --strip-components=1
		[ -x "$STAGE/blender/blender" ] || { log "$archive holds no blender executable"; exit 1; }
	fi

	cat >"$STAGE/tetra-studio" <<'EOF'
#!/bin/sh
# Tetra Studio launcher: points the editor at the libraries, plug-ins and
# programs shipped in this folder, then starts it. The folder can live anywhere.
# (The shared libraries in lib/ are found without help: every bundled file
# carries a relative RUNPATH.)
HERE="$(dirname "$(readlink -f "$0")")"
PATH="$HERE/bin:$HERE/blender${PATH:+:$PATH}"
GEGL_PATH="$HERE/lib/gegl-0.4"
BABL_PATH="$HERE/lib/babl-0.1"
MLT_REPOSITORY="$HERE/lib/mlt-7"
MLT_DATA="$HERE/share/mlt-7"
MLT_PROFILES_PATH="$HERE/share/mlt-7/profiles/"
MLT_PRESETS_PATH="$HERE/share/mlt-7/presets"
FREI0R_PATH="$HERE/lib/frei0r-1"
TETRA_MELT="$HERE/bin/melt"
TETRA_KDENLIVE_DATA="$HERE/kdenlive-data"
export PATH GEGL_PATH BABL_PATH MLT_REPOSITORY MLT_DATA \
	MLT_PROFILES_PATH MLT_PRESETS_PATH FREI0R_PATH TETRA_MELT TETRA_KDENLIVE_DATA
if [ -x "$HERE/bin/gdk-pixbuf-query-loaders" ]; then
	cache="${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/tetra-studio"
	if mkdir -p "$cache" 2>/dev/null \
		&& "$HERE/bin/gdk-pixbuf-query-loaders" "$HERE"/lib/gdk-pixbuf/loaders/*.so >"$cache/pixbuf-loaders.cache" 2>/dev/null; then
		GDK_PIXBUF_MODULE_FILE="$cache/pixbuf-loaders.cache"
		export GDK_PIXBUF_MODULE_FILE
	fi
fi
exec "$HERE/bin/tetra-studio-editor" "$@"
EOF
	chmod 755 "$STAGE/tetra-studio"

	log "libraries expected from the host:"
	ldd "${files[@]}" "$STAGE"/lib/*.so* 2>/dev/null | awk '$2 == "=>" { print $1 }' | sort -u \
		| grep -E "$LINUX_HOST_LIBS" | tr '\n' ' ' >&2
	echo >&2

	cd "$ROOT/dist"
	tar -czf "tetra-studio-$NAME-x86_64.tar.gz" "tetra-studio-$NAME-x86_64"
}

# --------------------------------------------------------------- windows ----

# Print, one per line, the MSYS2 DLLs the given files import, recursively.
windows_deps() {
	ntldd -R "$@" 2>/dev/null | tr '\\' '/' | awk '$2 == "=>" && $3 ~ /\.dll$/ { print $3 }' | sort -u | while read -r path; do
		path="$(cygpath -u "$path")"
		case "$path" in "$PREFIX"/*) echo "$path" ;; esac
	done
}

# True when a DLL imports something that is neither found nor a Windows API set.
windows_has_missing_deps() {
	ntldd -R "$1" 2>/dev/null | grep -i 'not found' | grep -qviE 'api-ms-win-|ext-ms-'
}

package_windows() {
	: "${MINGW_PREFIX:?package.sh windows must run in an MSYS2 MinGW shell (UCRT64)}"
	PREFIX="$(cygpath -u "$MINGW_PREFIX")"
	command -v ntldd >/dev/null || { log "ntldd is missing (pacman -S ${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}-ntldd)"; exit 1; }

	# The layout mirrors the MSYS2 prefix (bin/, lib/, share/): GEGL, babl,
	# MLT and its frei0r module locate their plug-ins relative to the folder
	# above the one that holds their DLL or the running program.
	mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share"
	cp "$BINARY" "$STAGE/bin/TetraStudio.exe"
	if [ -f "${BINARY%.exe}.console.exe" ]; then
		cp "${BINARY%.exe}.console.exe" "$STAGE/bin/TetraStudio.console.exe"
	fi
	local melt found=0
	for melt in melt.exe melt-7.exe; do
		if [ -f "$PREFIX/bin/$melt" ]; then
			cp "$PREFIX/bin/$melt" "$STAGE/bin/"
			found=1
		fi
	done
	[ "$found" = 1 ] || { log "melt.exe is missing from $PREFIX/bin"; exit 1; }

	local dir
	for dir in lib/gegl-0.4 lib/babl-0.1 lib/frei0r-1 lib/gdk-pixbuf-2.0 lib/mlt-7 lib/mlt share/mlt-7 share/mlt; do
		if [ -d "$PREFIX/$dir" ]; then
			cp -r "$PREFIX/$dir" "$STAGE/$dir"
		fi
	done
	for dir in lib/gegl-0.4 lib/babl-0.1 lib/frei0r-1; do
		[ -d "$STAGE/$dir" ] || { log "$PREFIX/$dir is missing"; exit 1; }
	done
	ls -d "$STAGE"/lib/mlt* "$STAGE"/share/mlt* >/dev/null || { log "the MLT module or data directory is missing"; exit 1; }
	find "$STAGE/lib" -type f \( -name '*.a' -o -name '*.la' -o -name '*.def' \) -delete

	local module
	for module in "$STAGE"/lib/mlt*/*.dll; do
		if mlt_module_skipped "$(basename "$module")"; then
			log "leaving out MLT module $(basename "$module")"
			rm -f "$module"
		elif windows_has_missing_deps "$module"; then
			log "leaving out MLT module $(basename "$module"): its libraries are not installed"
			rm -f "$module"
		fi
	done

	log "collecting DLLs"
	local files=()
	while IFS= read -r -d '' file; do files+=("$file"); done \
		< <(find "$STAGE/bin" "$STAGE/lib" -type f \( -name '*.dll' -o -name '*.exe' \) -print0)
	windows_deps "${files[@]}" | while read -r dll; do
		[ -e "$STAGE/bin/$(basename "$dll")" ] || cp "$dll" "$STAGE/bin/"
	done
	log "$(find "$STAGE/bin" -name '*.dll' | wc -l) DLLs bundled"

	if [ "${TETRA_SKIP_BLENDER:-0}" = 1 ]; then
		log "TETRA_SKIP_BLENDER=1: packaging without Blender"
	else
		local archive
		archive="$(fetch_blender windows-x64.zip)"
		unzip -q "$archive" -d "$STAGE/blender.tmp"
		mv "$STAGE"/blender.tmp/*/ "$STAGE/blender"
		rmdir "$STAGE/blender.tmp"
		[ -f "$STAGE/blender/blender.exe" ] || { log "$archive holds no blender.exe"; exit 1; }
	fi

	# CRLF line endings: cmd.exe mis-parses batch files with bare LF.
	sed 's/$/\r/' >"$STAGE/TetraStudio.bat" <<'EOF'
@echo off
rem Tetra Studio launcher: points the editor at the libraries, plug-ins and
rem programs shipped in this folder, then starts it. The folder can live anywhere.
rem Set TETRA_CONSOLE=1 to run in this console window and wait for the editor.
setlocal
set "HERE=%~dp0"
set "PATH=%HERE%bin;%HERE%blender;%PATH%"
set "GEGL_PATH=%HERE%lib\gegl-0.4"
set "BABL_PATH=%HERE%lib\babl-0.1"
set "FREI0R_PATH=%HERE%lib\frei0r-1"
set "TETRA_KDENLIVE_DATA=%HERE%kdenlive-data"
if exist "%HERE%bin\melt.exe" (set "TETRA_MELT=%HERE%bin\melt.exe") else (set "TETRA_MELT=%HERE%bin\melt-7.exe")
if not defined TETRA_CONSOLE goto gui
"%HERE%bin\TetraStudio.console.exe" %*
exit /b %ERRORLEVEL%
:gui
start "Tetra Studio" "%HERE%bin\TetraStudio.exe" %*
EOF

	cd "$ROOT/dist"
	rm -f "tetra-studio-$NAME-x86_64.zip"
	zip -qr "tetra-studio-$NAME-x86_64.zip" "tetra-studio-$NAME-x86_64"
}

# ------------------------------------------------------------------ main ----

rm -rf "$STAGE"
mkdir -p "$STAGE"
copy_common
case "$NAME" in
	linux) package_linux ;;
	windows) package_windows ;;
	*) echo "$USAGE" >&2; exit 1 ;;
esac

cd "$ROOT/dist"
du -sh "$STAGE" >&2
if [ "${TETRA_KEEP_STAGE:-0}" != 1 ]; then
	rm -rf "$STAGE"
fi
ls -la "$ROOT/dist"
# GitHub refuses release assets of 2 GiB or more.
for archive in "$ROOT"/dist/tetra-studio-"$NAME"-x86_64.*; do
	if [ -f "$archive" ] && [ "$(stat -c %s "$archive")" -ge 2147483648 ]; then
		log "error: $(basename "$archive") is 2 GiB or larger and cannot be attached to a GitHub release"
		exit 1
	fi
done
