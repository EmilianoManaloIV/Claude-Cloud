# Tetra Studio

One editor that brings Godot, GIMP, Kdenlive and Blender together.

Tetra Studio is the Godot 4.7.2 editor built from source with four extra C++
modules. Godot provides the Unity/Unreal-style shell and all of its own
features. The modules add layered image editing, video editing and a live
bridge to Blender, mostly by driving the real engines behind those programs
(GEGL, MLT and Blender itself) rather than re-implementing them.

The work is in progress. See **Status** below for what exists today.

## Status

| Area | Engine layer | Editor UI |
| --- | --- | --- |
| Shared widgets (`modules/tetra`) | Done, tested | Done: generated parameter forms, operation browser, pan/zoom canvas, About dialog |
| Image (`modules/tetra_paint`) | Done, tested | Partly written, not yet registered in the editor |
| Video (`modules/tetra_video`) | Done, tested | Partly written, not yet registered in the editor |
| Blender (`modules/tetra_model`) | Done, tested | A "Blender" dock and toolbar menu appear in the 3D editor; not yet verified end to end |

"Engine layer" means classes registered in Godot's ClassDB, usable from
GDScript today and covered by the headless tests (20 test files).

### Image engine (GIMP)

- Layered documents in 32-bit float linear RGBA with 39 of GIMP's layer modes,
  using GIMP's formulas and per-mode blend spaces.
- Selections (rectangle, ellipse, polygon, fuzzy, by colour; feather, grow,
  shrink, invert) that every edit respects.
- Paint strokes: paintbrush, pencil, eraser, airbrush, smudge, clone, with
  GIMP's brush hardness curve and stroke application rules.
- Bucket fill, GIMP's gradient shapes, flips, rotations, scaling, cropping.
- 15 native colour and filter operations, plus every usable GEGL operation
  found by introspection (135 with GEGL 0.4.48).
- Undo/redo for everything. OpenRaster (`.ora`) load and save with layers;
  `.ora` files in a project import as textures.
- Not done: XCF import.

### Video engine (Kdenlive)

- A multitrack timeline document with Kdenlive's editing rules (no overlaps,
  linked audio/video, trim, split, ripple delete, insert/remove space) and
  full undo/redo.
- Projects are plain MLT XML that `melt` accepts.
- MLT playback and frame access, with audio pushed through Godot's audio
  server. Sound was only exercised with the dummy audio driver.
- Kdenlive's effect catalog: 439 effects parsed, 344 usable with the MLT
  installed here, plus 281 MLT filters that have no Kdenlive definition.
- Rendering through `melt` with Kdenlive's 33 render presets and 3 built in.
- Not done: keyframes (animated parameters are edited as constants), proper
  title placement.

### Blender bridge

- A long-lived Blender process (the `bpy` Python module or a Blender
  executable) driven over a JSON-lines protocol.
- Blender's operators, modifiers and mesh primitives are discovered by RNA
  introspection and described with the same schema as GEGL and MLT
  operations.
- Any Godot mesh can go through a Blender modifier stack or operator list and
  come back as an `ArrayMesh`.
- Import and export through Blender's own file formats; Cycles rendering of a
  scene description.
- Not done: a native Blender-style mesh edit mode in the viewport.

## Building

```sh
scripts/fetch-upstream.sh godot kdenlive
scripts/build.sh
scripts/setup-blender-bridge.sh   # optional: Blender 5.3 as a Python module
scripts/run.sh
scripts/test.sh
```

Details, prerequisites and the screenshot tooling are in
[docs/DEVELOPING.md](docs/DEVELOPING.md). The design is described in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Each module has its own
`README.md` with its API.

## Upstream sources

Pinned as shallow git submodules under `upstream/`:

| Project | Version |
| --- | --- |
| Godot | 4.7.2-stable |
| Blender | v5.3.0 |
| GIMP | 3.2.8 |
| Kdenlive | 26.08.3 |
| MLT | 7.42.0 |
| GEGL | 0.4.72 |

Only Godot is compiled. Kdenlive's `data/` directory supplies the effect and
render-preset catalogs at run time. The others are reference sources. GEGL
and MLT are linked from the system packages, which are older than the pinned
sources (0.4.48 and 7.22 on Ubuntu 24.04).

## Licence

GPL-3.0-or-later. Tetra ports code from GIMP and Kdenlive and links GPL
engines. Godot itself is MIT-licensed.
