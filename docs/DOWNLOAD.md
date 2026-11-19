# Tetra Studio download

This archive holds the Tetra Studio editor: the Godot editor with the Tetra
Image, Video and Blender modules built in.

## Running

- **Linux:** `./tetra-studio`
- **Windows:** `TetraStudio.exe`

Set the environment variable `TETRA_KDENLIVE_DATA` to the `kdenlive-data`
folder next to the executable so the Video workspace finds its effect and
render-preset catalogs.

## What each platform needs

| | Linux | Windows |
| --- | --- | --- |
| Godot features | Built in | Built in |
| Image workspace, native tools | Built in | Built in |
| GEGL filters | Needs `libgegl-0.4` installed (`sudo apt install libgegl-0.4-0t64`) | Not included |
| Video playback and rendering | Needs MLT installed (`sudo apt install libmlt++7 melt frei0r-plugins`) | Not included |
| Blender bridge | Needs Blender, or Python 3.13 with `pip install bpy==5.3.0` | Same |

The Linux build links GEGL and MLT from the system, so it will not start on
a machine without them. The Windows build is made without GEGL and MLT: the
filters and the video engine report themselves as unavailable.

Point the Blender bridge at Blender in *Editor Settings > Tetra > Blender*.

Licence: GPL-3.0-or-later. Source: the repository this archive was built from.
