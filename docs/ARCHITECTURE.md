# Tetra Studio architecture

Tetra Studio is one editor that brings together four creative programs:

| Upstream | Version pinned in `upstream/` | What Tetra takes from it |
| --- | --- | --- |
| Godot | 4.7.2-stable | The editor itself. Tetra is the Godot editor built from source, so every Godot feature is present unchanged. |
| GIMP (+ GEGL 0.4.72) | GIMP_3_2_8 | The **Image** workspace: layered raster editing with GIMP's layer modes and paint tools, and every GEGL filter through libgegl. |
| Kdenlive (+ MLT 7.42.0) | v26.08.3 | The **Video** workspace: multitrack timeline, Kdenlive's effect catalog and render presets, playback and rendering through MLT. |
| Blender | v5.3.0 | The **Blender bridge**: every Blender operator, modifier and Cycles rendering, applied to meshes in the Godot scene through a live Blender process. |

## The host

`upstream/godot` is built with `custom_modules=modules`. Nothing in Godot is
forked apart from the small patch set in `patches/godot/` (branding). The
Unity/Unreal-style shell — scene tree, inspector, file system dock, 2D and 3D
viewports, script editor, debugger, export — is Godot's.

Tetra adds four C++ modules:

| Module | Adds |
| --- | --- |
| `modules/tetra` | Shared editor infrastructure: generated parameter panels, operation browser, pan/zoom canvas base. |
| `modules/tetra_paint` | The Image workspace (main-screen tab), the layered document, the GEGL bridge, layered-image file support. |
| `modules/tetra_video` | The Video workspace (main-screen tab): bin, monitor, timeline, effects, render; the MLT engine. |
| `modules/tetra_model` | The Blender bridge and its panel in the 3D editor. |

## Two tiers

**Native tier.** What must be immediate — painting, compositing layers,
dragging clips on a timeline — is implemented in the modules themselves, in
C++ against Godot's types, porting the upstream algorithm where one exists
(layer-mode maths from GIMP, timeline rules from Kdenlive).

**Engine bridges.** Everything else is reached through the real upstream
engine and discovered by introspection, not re-implemented feature by feature:

| Bridge | Engine | Discovery | Lives in |
| --- | --- | --- | --- |
| Filters | GEGL (`libgegl-0.4`) | `gegl_list_operations` + GParamSpec introspection | `tetra_paint/gegl` |
| Video | MLT (`libmlt++-7`, `melt`) | MLT repository metadata + Kdenlive's effect XML | `tetra_video/mlt` |
| 3D | Blender (`bpy` module or a Blender executable) | RNA introspection of operators and modifiers | `tetra_model/bridge` |

The bridges are optional at build time and at run time. Without GEGL, MLT or
Blender the corresponding features report that the engine is missing and the
rest of the editor works.

## The operation schema

Every bridge describes what it offers with the same plain data, so one piece
of UI (`TetraOperationBrowser` + `TetraParamPanel` in `modules/tetra`) serves
all of them. An operation added upstream shows up without new UI code.

An **operation** is a `Dictionary`:

| Key | Type | Meaning |
| --- | --- | --- |
| `id` | String | Unique within its source: `gegl:gaussian-blur`, `mlt:brightness`, `blender:mesh.subdivide`. |
| `label` | String | Name shown to the user. |
| `description` | String | Tooltip text. Optional. |
| `category` | String | Slash-separated menu path: `Blur`, `Mesh/Edges`. |
| `source` | String | `native`, `gegl`, `mlt`, `kdenlive` or `blender`. |
| `params` | Array of Dictionary | The parameters, in display order. |

A **parameter** is a `Dictionary`:

| Key | Type | Meaning |
| --- | --- | --- |
| `name` | String | Identifier exactly as the engine knows it: `std-dev-x`. |
| `label` | String | Name shown to the user. |
| `description` | String | Tooltip text. Optional. |
| `type` | String | `bool`, `int`, `float`, `string`, `text`, `enum`, `color`, `vec2`, `vec3`, `file` or `seed`. |
| `default` | Variant | bool, int, float, String, Color, Vector2 or Vector3 to match `type`. For `enum`, the option's `value`. |
| `min`, `max` | float | Hard limits. Optional. |
| `ui_min`, `ui_max` | float | Slider range. Optional; falls back to `min`/`max`. |
| `step` | float | Optional. |
| `unit` | String | Suffix such as `px` or `°`. Optional. |
| `options` | Array of `{value, label}` | Choices for `enum`. |
| `group` | String | Optional sub-heading. |
| `advanced` | bool | Optional; hidden behind an "advanced" toggle. |

Parameter **values** travel as a `Dictionary` from `name` to value.

## Module layout

```
modules/<module>/
  config.py, SCsub, register_types.{h,cpp}
  *.h *.cpp        engine-facing classes, registered in ClassDB so scripts and tests can drive them
  <bridge>/        gegl/, mlt/ or bridge/ — code that talks to the upstream engine
  editor/          editor UI (EditorPlugin, docks, canvases)
  icons/           SVG icons
tests/godot/project/<module>/test_*.gd   headless tests of the ClassDB API
```

## Rules for module code

- Godot's own conventions: tabs, `p_` parameter prefix, `GDCLASS`, `memnew`,
  `_bind_methods`, `TTR()` for user-visible strings. The engine is compiled
  with `-fno-exceptions`.
- Class names start with `Tetra`. Engine-facing classes are registered with
  `GDREGISTER_CLASS` and expose their operations through `_bind_methods`, so
  every feature can be exercised from a headless script.
- Heavy work runs off the main thread (`WorkerThreadPool` or `Thread`) and
  reports back with `call_deferred`.
- Every user action that changes a document can be undone.
- Code ported from GPL upstream sources starts with a comment naming the
  original file and its copyright holders. Tetra is GPL-3.0-or-later.

## Unified workflows

- A layered image saved in the project is a texture: Tetra registers an image
  loader for OpenRaster (`.ora`), so Godot's texture importer consumes layered
  files directly, and saving in the Image workspace re-imports them.
- A mesh in the scene can be sent through any Blender operator or modifier
  stack and comes back as an ordinary `ArrayMesh`.
- The Video workspace saves plain MLT XML and renders through MLT to files the
  project can play.

## Building and testing

See [DEVELOPING.md](DEVELOPING.md).
