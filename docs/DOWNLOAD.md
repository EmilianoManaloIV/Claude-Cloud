# Tetra Studio download

This archive is self-contained. It holds the Tetra Studio editor (the Godot
editor with the Tetra Image, Video and Blender workspaces) together with
everything those workspaces use: GEGL and its filters, MLT with `melt`, FFmpeg
and the frei0r effects, Kdenlive's effect and render-preset catalogs, and
Blender 5.3. Nothing else has to be installed.

## Running

Unpack the archive anywhere, then:

- **Linux:** run `./tetra-studio` in the unpacked folder.
- **Windows:** double-click `TetraStudio.bat` in the unpacked folder.

Start the editor through that launcher, not through the program in `bin/`
directly: the launcher tells the editor where the bundled plug-ins, `melt`,
Blender and the Kdenlive catalogs are. Arguments are passed on to the editor,
for example `./tetra-studio --editor --path my_project`.

## What is in the folder

| | Linux | Windows |
| --- | --- | --- |
| Launcher | `tetra-studio` | `TetraStudio.bat` |
| Editor | `bin/tetra-studio-editor` | `bin\TetraStudio.exe` |
| Shared libraries | `lib/` | `bin\*.dll` |
| GEGL filters, babl extensions | `lib/gegl-0.4`, `lib/babl-0.1` | same |
| MLT modules and data, `melt` | `lib/mlt-7`, `share/mlt-7`, `bin/melt` | `lib\mlt-7`, `share\mlt-7`, `bin\melt.exe` |
| frei0r effects | `lib/frei0r-1` | same |
| Kdenlive catalogs | `kdenlive-data/` | same |
| Blender 5.3.0 | `blender/` | same |

## What the computer must provide

- **Linux:** a 64-bit x86 desktop with glibc 2.39 or newer (Ubuntu 24.04,
  Debian 13, Fedora 40 or later) and the libraries every desktop system has:
  the C++ runtime (`libstdc++`), graphics drivers (OpenGL or Vulkan, `libdrm`,
  `libgbm`), the X11 or Wayland client libraries, ALSA, zlib and expat. These
  are deliberately not bundled, because they have to match the system.
- **Windows:** 64-bit Windows 10 or 11 with working graphics drivers (Vulkan
  or OpenGL 3.3). The Windows editor is built without the Direct3D 12
  renderer.

## Notes

- The MLT modules that depend on Qt, OpenCV and Movit are not included. The
  Video workspace uses the frei0r, GDK (images and titles) and FFmpeg modules
  instead.
- To use a different Blender, set *Editor Settings > Tetra > Blender*; it
  takes precedence over the bundled one.
- Third-party components and their licences are listed in `THIRD_PARTY.md`.

Licence: GPL-3.0-or-later. Source: the repository this archive was built from.
