# Developing Tetra Studio

## Prerequisites (Ubuntu 24.04)

```sh
sudo apt-get install build-essential scons pkg-config mold python3 \
    libx11-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libgl1-mesa-dev \
    libglu1-mesa-dev libasound2-dev libpulse-dev libudev-dev libwayland-dev \
    libxkbcommon-dev libdbus-1-dev libfontconfig-dev \
    libgegl-dev libmlt++-dev melt frei0r-plugins ffmpeg
```

`libgegl-dev` and `libmlt++-dev` are optional. Without them the GEGL filter
bridge and the MLT video engine are left out of the build and everything else
still works.

The Blender bridge needs no build-time dependency. At run time it uses either
the `bpy` Python module or a Blender executable:

```sh
scripts/setup-blender-bridge.sh   # creates build/blender-venv with bpy 5.3 (needs Python 3.13)
```

## Getting the sources

```sh
git clone <this repository> && cd <checkout>
scripts/fetch-upstream.sh godot kdenlive   # what the build and the Video workspace's catalogs need
scripts/fetch-upstream.sh                  # also the Blender, GIMP, MLT and GEGL reference sources
```

The upstream trees are shallow git submodules pinned to release tags. Only
`upstream/godot` is compiled.

## Building and running

```sh
scripts/build.sh            # first build: about 15 minutes on 4 cores; later builds are incremental
scripts/run.sh              # launch the editor (project manager)
scripts/run.sh --editor --path my_project
```

`scripts/build.sh` applies `patches/godot/*.patch` and runs SCons in
`upstream/godot` with `custom_modules=modules`. Extra arguments go to SCons.
Concurrent invocations queue behind a lock, because two SCons processes must
not run in the same tree. The binary is
`upstream/godot/bin/godot.linuxbsd.editor.x86_64`.

After one full build, a single module file can be type-checked in seconds
without SCons:

```sh
scripts/check-module.sh modules/tetra_paint/editor/paint_canvas.cpp
```

## Tests

```sh
scripts/test.sh                 # run every headless test
scripts/test.sh tetra_paint     # only one module's tests
```

Tests are GDScript files named `tests/godot/project/<module>/test_*.gd`. Each
one `extends "res://tetra_test.gd"`, implements `run()`, and uses `check()`,
`check_eq()` and `check_near()`; the process exits non-zero if any check
fails. They run the real editor binary with `--headless`, so they exercise
exactly the classes the editor UI uses.

To look at the editor itself on a machine without a display:

```sh
scripts/screenshot.sh build/screens/image.png Image    # main-screen name: 2D, 3D, Script, Image, Video
```

It starts the editor under Xvfb with software OpenGL, switches to the named
main screen, waits, and saves the window as a PNG. A driver script can be
passed as a third argument to set up state before the capture.

## Layout

See [ARCHITECTURE.md](ARCHITECTURE.md) for the module layout, the operation
schema shared by the bridges, and the rules for module code.
