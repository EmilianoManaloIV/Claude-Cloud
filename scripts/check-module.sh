#!/usr/bin/env bash
# Type-check Godot-side module sources against the engine headers without
# running SCons. Takes seconds per file and is safe to run concurrently, which
# a second SCons process in the same tree is not.
#
#   scripts/check-module.sh modules/tetra_paint/editor/foo.cpp [more.cpp ...]
#
# The compiler flags come from upstream/godot/compile_commands.json (written by
# scripts/build.sh), using an entry from the same module as each file so that
# module-specific flags (pkg-config cflags and the like) are applied.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="$ROOT/upstream/godot/compile_commands.json"

if [ ! -f "$DB" ]; then
	echo "compile_commands.json not found; run scripts/build.sh once first." >&2
	exit 1
fi
if [ "$#" -eq 0 ]; then
	echo "usage: $0 <file.cpp> [...]" >&2
	exit 2
fi

exec python3 - "$ROOT" "$DB" "$@" <<'PY'
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

root, db_path, files = sys.argv[1], sys.argv[2], sys.argv[3:]
modules_dir = os.path.join(root, "modules")

with open(db_path) as fh:
    db = json.load(fh)


def entry_args(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def module_of(path):
    rel = os.path.relpath(path, modules_dir)
    return None if rel.startswith("..") else rel.split(os.sep)[0]


by_module = {}
for entry in db:
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    mod = module_of(path)
    if mod and mod not in by_module:
        by_module[mod] = entry


def check(path):
    path = os.path.abspath(path)
    mod = module_of(path)
    entry = by_module.get(mod)
    if entry is None:
        return path, 2, "no compile_commands entry for module '%s' (is it built yet?)\n" % mod
    args = entry_args(entry)
    out = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg == "-c" or arg.endswith((".cpp", ".c", ".cc")):
            continue
        out.append(arg)
    out += ["-fsyntax-only", "-fdiagnostics-color=never", path]
    proc = subprocess.run(out, cwd=entry["directory"], capture_output=True, text=True)
    return path, proc.returncode, proc.stdout + proc.stderr


failed = 0
with ThreadPoolExecutor(max_workers=min(4, len(files))) as pool:
    for path, code, output in pool.map(check, files):
        rel = os.path.relpath(path, root)
        if code == 0 and not output.strip():
            print("ok    " + rel)
        elif code == 0:
            print("warn  " + rel)
            print(output, end="")
        else:
            failed += 1
            print("FAIL  " + rel)
            print(output, end="")
sys.exit(1 if failed else 0)
PY
