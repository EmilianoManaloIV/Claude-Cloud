# tetra_paint — the Image workspace and its engine

Tetra's GIMP: a layered raster document with GIMP's layer modes, selections,
paint tools, fills, transforms, colour operations and filters, the GEGL filter
bridge and OpenRaster file support, and the **Image** main screen of the
editor built on it (see [Workspace](#workspace)). The workspace drives the
engine through the ClassDB API described here, and so do the headless tests in
`tests/godot/project/tetra_paint/`.

| Class | File | What it is |
| --- | --- | --- |
| `TetraImageDocument` (RefCounted) | `tetra_image_document.h` | One open image: layers, selection, undo history and every edit. |
| `TetraGegl` (static methods only) | `gegl/tetra_gegl.h` | GEGL operations found by introspection, applied to images. |
| `TetraImageLoaderORA` (not in ClassDB) | `tetra_ora_loader.h` | `ImageFormatLoader` for `.ora`, so layered files import as textures. |

Source layout: `tetra_image_document.cpp` (layers, compositing, undo),
`tetra_image_selection.cpp`, `tetra_image_paint.cpp`, `tetra_image_edit.cpp`
(fills, transforms), `tetra_image_operations.cpp` (native operations),
`tetra_image_io.cpp` (load, OpenRaster, export), `tetra_layer_modes.cpp`
(GIMP's layer-mode maths), `tetra_paint_utils.*` (thread-pool loops, sRGB,
Gaussian blur), `gegl/tetra_gegl.cpp`.

## Model

- **Pixels** are 32-bit float RGBA, **linear light, straight alpha**, one
  buffer of the canvas size per layer. Values are not clamped. Fine up to about
  4096 x 4096 (256 MB per layer there); there is no tiling and layers have no
  offset or size of their own. `create`, `scale_image`, `resize_canvas` and
  `load` refuse sizes for which `is_size_supported(width, height)` (static)
  is false: a side above 16384 or more than 8192 x 8192 pixels in all (1 GiB
  per layer). The New Image and size dialogs show a warning instead.
- **Every `Color` in the API is linear.** Convert UI colours with
  `Color.srgb_to_linear()`.
- **Images going in** (`layer_set_image`, `load`, `filter_image`,
  `TetraGegl.apply`): integer formats are taken as sRGB and linearised, float
  formats are taken as linear. **Images coming out**: `composite()` is
  `FORMAT_RGBA8` sRGB for display; `composite_float()`, `layer_get_image()`,
  `compute_operation()` and `filter_image()` are `FORMAT_RGBAF` linear;
  `get_selection_mask()` is `FORMAT_RF`.
- **Layers** are ordered bottom to top and identified by stable int ids
  (never reused within a document; undo brings a layer back with its id).
- **Selection**: one float mask of the canvas size. An empty selection means
  everything is editable. Fills, strokes and operations are blended by the
  mask value and respect a layer's `lock_alpha`. Layer transforms and
  `layer_set_image` are raw: they ignore the selection and the lock.
- **Compositing** follows GIMP: each visible layer is combined with what is
  below through its mode, in the mode's default blend space (linear,
  perceptual or CIE Lab, from GIMP's mode table), composited in linear light
  with the mode's default composite mode ("union" for Normal, Dissolve,
  Behind and Merge, "clip to backdrop" for the others). The bottom visible
  layer is shown as it is whatever its mode (only its opacity applies), as in
  GIMP. Rows are composited on the `WorkerThreadPool` and only the requested
  rectangle is touched.

## Signals

| Signal | When |
| --- | --- |
| `changed(rect: Rect2i)` | Pixels in `rect` of the composite may have changed. Emitted by every edit, by each `stroke_add_point`, by undo/redo. |
| `layers_changed()` | The layer list, a layer property or the active layer changed. |
| `selection_changed()` | The selection mask changed. |
| `size_changed()` | The canvas size changed (create, load, scale, canvas size, crop, rotate, undo of those). |
| `undo_changed()` | The undo or redo stack changed. |

`get_and_clear_dirty_rect() -> Rect2i` returns the union of all `changed`
rectangles since the last call, for UIs that refresh once per frame.

## TetraImageDocument

### Document and layers

```
create(width, height, fill = Color(1, 1, 1, 1)) -> Error     # one layer "Background"; alpha 0 = transparent; clears history
get_size() -> Vector2i, get_width(), get_height()

add_layer(name, index = -1) -> int            # transparent; -1 = on top; becomes active; returns the id
remove_layer(id) -> Error
duplicate_layer(id) -> int                    # placed above the source, named "<name> copy"
move_layer(id, new_index) -> Error            # index in the bottom-to-top stack
merge_down(id) -> Error                       # into the layer below; result keeps that layer's name and mode
flatten() -> Error                            # all visible layers into one (alpha is kept)
get_layer_ids() -> PackedInt32Array           # bottom to top
get_layer_count() -> int, get_layer_index(id) -> int
get_active_layer() -> int, set_active_layer(id)

layer_get_name / layer_set_name(id, String)
layer_is_visible / layer_set_visible(id, bool)
layer_get_opacity / layer_set_opacity(id, float 0..1)
layer_get_mode / layer_set_mode(id, LayerMode)
layer_get_lock_alpha / layer_set_lock_alpha(id, bool)
layer_get_image(id) -> Image                              # RGBAF copy, canvas size
layer_set_image(id, image, offset = Vector2i()) -> Error  # replaces the layer; image placed at offset, clipped, rest transparent

composite(rect = Rect2i()) -> Image          # RGBA8 sRGB; empty rect = whole canvas; rect is clipped
composite_float(rect = Rect2i()) -> Image    # RGBAF linear
get_and_clear_dirty_rect() -> Rect2i
```

`layer_set_image` records and repaints only the bounding box of the pixels
that really differ, so committing a preview that changed a small area is cheap.

### Layer modes

`enum LayerMode`, values in GIMP's menu order, so a value is also the index
into `get_layer_mode_names()`:

`LAYER_MODE_NORMAL, DISSOLVE, BEHIND, COLOR_ERASE, ERASE, MERGE, SPLIT,
LIGHTEN_ONLY, LUMA_LIGHTEN_ONLY, SCREEN, DODGE, ADDITION, DARKEN_ONLY,
LUMA_DARKEN_ONLY, MULTIPLY, BURN, LINEAR_BURN, OVERLAY, SOFT_LIGHT, HARD_LIGHT,
VIVID_LIGHT, PIN_LIGHT, LINEAR_LIGHT, HARD_MIX, DIFFERENCE, EXCLUSION, SUBTRACT,
GRAIN_EXTRACT, GRAIN_MERGE, DIVIDE, HSV_HUE, HSV_SATURATION, HSL_COLOR,
HSV_VALUE, LCH_HUE, LCH_CHROMA, LCH_COLOR, LCH_LIGHTNESS, LUMINANCE`
(each prefixed `LAYER_MODE_`), then `LAYER_MODE_MAX` (= 39).

```
static get_layer_mode_names() -> PackedStringArray        # GIMP's labels, index = enum value
static blend_colors(mode, backdrop, layer, opacity = 1.0) -> Color   # one pixel through a mode
```

Dissolve reproduces GIMP's pattern exactly (same random generator and
seeds), anchored to canvas coordinates. GIMP's legacy modes, Pass through,
Replace, Overwrite and Anti erase are not included. The sRGB transfer curve
stands in for both of GIMP's "perceptual" and "non-linear" spaces (the same
thing for sRGB images).

### Undo

Every mutating method records one named step. Pixel edits store only the
changed rectangle of the layer (the stored data is swapped with the layer on
undo and redo, so a step is kept once); selection edits store the changed
part of the mask; structural edits store the layer list and those pixel
buffers that changed.

```
begin_undo_group(name), end_undo_group()     # everything in between is one step; groups nest
undo() -> bool, redo() -> bool               # false if there is nothing to do or a stroke/group is open
can_undo(), can_redo()
get_undo_names() -> PackedStringArray        # oldest first
get_redo_names() -> PackedStringArray
clear_undo_history()
set_undo_memory_limit(bytes), get_undo_memory_limit()    # default 1 GiB; the oldest steps are dropped, the newest always stays
get_undo_memory_usage() -> int
```

`set_active_layer` is not an undo step. `create` and `load` clear the history.

### Selection

`enum SelectOp { SELECT_REPLACE, SELECT_ADD, SELECT_SUBTRACT, SELECT_INTERSECT }`

```
select_all(), select_none(), invert_selection()
select_rect(rect, op = REPLACE)
select_ellipse(rect, op = REPLACE, antialias = true)             # ellipse inscribed in rect
select_polygon(points: PackedVector2Array, op = REPLACE, antialias = true)   # closed, even-odd
select_by_color(point: Vector2i, threshold = 0.0588, contiguous = false, sample_merged = false, op = REPLACE)
select_fuzzy(point, threshold = 0.0588, sample_merged = false, op = REPLACE) # = contiguous select_by_color
select_mask(mask: Image, op = REPLACE)       # canvas-sized image; its red channel is the mask
feather_selection(radius)                    # Gaussian, sigma = radius / 3.5 as in GIMP
grow_selection(pixels), shrink_selection(pixels)   # circular; shrinking also retreats from the canvas border
get_selection_mask() -> Image                # FORMAT_RF, zeros when empty
is_selection_empty() -> bool
selection_bounds() -> Rect2i                 # Rect2i() when empty
```

Colour selection uses GIMP's rule: the largest difference of the sRGB-encoded
R, G, B components against the seed pixel must be within `threshold` (0..1;
GIMP's default 15/255); fully transparent pixels never match an opaque seed
and a fully transparent seed matches by alpha. The source is the active layer,
or the composite with `sample_merged`. There is no antialiasing of the
colour-selection edge.

### Painting

`enum PaintTool { TOOL_PAINTBRUSH, TOOL_PENCIL, TOOL_ERASER, TOOL_AIRBRUSH, TOOL_SMUDGE, TOOL_CLONE }`

```
static get_default_stroke_settings(tool) -> Dictionary
stroke_begin(layer_id, tool, settings: Dictionary) -> Error
stroke_add_point(position: Vector2, pressure = 1.0)    # canvas pixels (pixel centres at .5); emits changed(rect of the new dabs)
stroke_end()                                           # records one undo step named after the tool
stroke_cancel()                                        # restores the layer, records nothing
is_stroke_active() -> bool
stroke_line(layer_id, tool, settings, from, to) -> Error   # begin + two points + end
```

Settings (missing keys take the defaults):

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `color` | Color (linear) | black | Paint colour; its alpha multiplies `opacity`. Unused by eraser, smudge and clone. |
| `size` | float | 20 | Brush diameter in pixels (minimum 1). |
| `hardness` | float 0..1 | 0.5 (pencil: 1) | GIMP's generated-brush falloff. The pencil ignores it: hard edge, snapped to pixels. |
| `opacity` | float 0..1 | 1 | Opacity of the whole stroke. |
| `flow` | float 0..1 | 1 (airbrush: 0.1) | Coverage of one dab. |
| `spacing` | float, % of size | 10 | Distance between dabs (GIMP's default brush spacing); never closer than one pixel. |
| `mode` | LayerMode | NORMAL | Paint mode (ignored by the eraser and smudge). |
| `pressure_size` | bool | false | Pressure scales the size. |
| `pressure_opacity` | bool | false | Pressure scales the dab coverage. |
| `rate` | float 0..1 | 0.5 | Smudge only: how much colour the brush carries along. |
| `source_point` | Vector2 | — | Clone only: the point copied to the first stroke point. |
| `source_offset` | Vector2 | (0, 0) | Clone only, when `source_point` is absent: source = destination + offset. |

Behaviour, as in GIMP's default "constant" paint application: within one
stroke the dabs accumulate into a coverage map that never exceeds the dab
coverage, and the stroke is composited from the pre-stroke pixels with the
stroke opacity and mode, so overlapping dabs of one stroke do not build up.
The airbrush is the exception: it builds up with every dab, also while the
pointer rests (call `stroke_add_point` again with the same position on a
timer). The eraser removes alpha (and does nothing on a layer with locked
alpha). Clone copies from the layer as it was when the stroke began. While a
stroke is open every other mutating method fails with `ERR_BUSY`.

`stroke_begin` keeps a reference to the layer's pixels; the first dab copies
the layer once (about 30 ms at 2048 x 2048).

### Fills and edits

```
fill(layer_id, color) -> Error                 # sets the selection, or everything, to the colour (its alpha included)
clear(layer_id) -> Error                       # makes the selection transparent
bucket_fill(layer_id, point: Vector2i, color, threshold = 0.0588, contiguous = true, sample_merged = false) -> Error
gradient_fill(layer_id, start: Vector2, end: Vector2, color_a, color_b, type = GRADIENT_LINEAR, repeat = REPEAT_NONE) -> Error
pick_color(point: Vector2i, sample_merged = false, radius = 0) -> Color   # active layer or composite; radius = average over a square
```

`enum GradientType { GRADIENT_LINEAR, GRADIENT_BILINEAR, GRADIENT_RADIAL,
GRADIENT_SQUARE, GRADIENT_CONICAL, GRADIENT_CONICAL_ASYMMETRIC,
GRADIENT_SPIRAL_CLOCKWISE, GRADIENT_SPIRAL_ANTICLOCKWISE }`,
`enum GradientRepeat { REPEAT_NONE, REPEAT_SAWTOOTH, REPEAT_TRIANGULAR }`.
The shapes are GIMP's; the two colours are mixed in perceptual RGB (GIMP's
default) and the gradient is painted over the layer in Normal mode, so
transparent gradient colours let the layer show through.

### Transforms

```
flip_layer(layer_id, vertical = false) -> Error
rotate_layer(layer_id, degrees) -> Error       # 90, 180 or 270 clockwise about the canvas centre; content leaving the canvas is cut
translate_layer(layer_id, offset: Vector2i) -> Error
flip_image(vertical = false) -> Error          # all layers and the selection
rotate_image(degrees) -> Error                 # 90 / 180 / 270 clockwise; swaps width and height
scale_image(size: Vector2i, interpolation = INTERPOLATION_CUBIC) -> Error
resize_canvas(size: Vector2i, offset = Vector2i()) -> Error    # offset = where the old top-left corner lands
crop(rect) -> Error, crop_to_selection() -> Error
```

`enum Interpolation { INTERPOLATION_NEAREST, INTERPOLATION_LINEAR, INTERPOLATION_CUBIC }`.
Scaling is alpha-weighted and widens its filter when shrinking.

### Operations (shared schema)

```
static get_native_operations() -> Array       # operation Dictionaries, source "native"
static get_operations() -> Array              # native + TetraGegl.list_operations()
apply_operation(layer_id, operation_id, values: Dictionary) -> Error   # through the selection and lock_alpha; one undo step
compute_operation(layer_id, operation_id, values) -> Image   # what the layer would become; the document is not changed
static filter_image(image, operation_id, values) -> Image    # the bare operation on any image; no selection; thread-safe
```

Both accept native ids and `gegl:*` ids. Missing values take the defaults.

| id | Category | Parameters (default) |
| --- | --- | --- |
| `native:invert` | Colors | `linear` (false) |
| `native:desaturate` | Colors/Desaturate | `mode`: luminance, luma, lightness, average, value |
| `native:brightness-contrast` | Colors | `brightness` (0), `contrast` (0), both -1..1 |
| `native:levels` | Colors | `channel`: value/red/green/blue/alpha, `low-input` (0), `high-input` (1), `gamma` (1), `low-output` (0), `high-output` (1), `clamp-input`, `clamp-output`, `trc`: non-linear/linear |
| `native:hue-saturation` | Colors | `hue`, `lightness`, `saturation` (master range, -1..1) |
| `native:color-balance` | Colors | `{shadows,midtones,highlights}-{cyan-red,magenta-green,yellow-blue}` (0), `preserve-luminosity` (true) |
| `native:colorize` | Colors | `hue` (0.5), `saturation` (0.5), `lightness` (0) |
| `native:threshold` | Colors | `channel`: value/red/green/blue/alpha/rgb/luminance, `low` (0.5), `high` (1) |
| `native:posterize` | Colors | `levels` (3) |
| `native:gaussian-blur` | Filters/Blur | `std-dev-x` (1.5), `std-dev-y` (1.5) |
| `native:unsharp-mask` | Filters/Enhance | `std-dev` (3), `scale` (0.5), `threshold` (0) |
| `native:pixelize` | Filters/Blur | `size-x` (16), `size-y` (16) |
| `native:edge-sobel` | Filters/Edge-Detect | `horizontal`, `vertical`, `keep-sign` (all true) |
| `native:emboss` | Filters/Distorts | `type`: emboss/bumpmap, `azimuth` (30), `elevation` (45), `depth` (20) |
| `native:noise-rgb` | Filters/Noise | `correlated`, `independent`, `linear`, `gaussian`, `red`/`green`/`blue` (0.2), `alpha` (0), `seed` |

The colour operations use GIMP's maths and working spaces; blur, pixelize,
sobel, emboss and noise follow GEGL (the tests compare them with the real
GEGL operations). Known differences: `native:unsharp-mask` uses a hard
threshold; `native:edge-sobel` is centred on the pixel (GEGL's own result is
shifted one pixel down-right); `native:noise-rgb` has its own random numbers;
hue-saturation has the master range only.

### Files

```
load(path) -> Error            # .ora with all layers, or any image Godot loads as one layer named after the file
save_ora(path) -> Error        # OpenRaster; written to "<path>.tmp" and renamed, so a failed save keeps the old file
export_image(path, quality = 0.9) -> Error   # flattened; by extension: png, jpg/jpeg (on white), webp (lossless at quality 1), exr (linear float)
```

OpenRaster: `mimetype` first and stored, `stack.xml`, one 8-bit sRGB PNG per
layer cropped to its content with `x`/`y`, `mergedimage.png`,
`Thumbnails/thumbnail.png` (at most 256 px). Name, opacity, visibility and
mode (`composite-op`) round-trip; modes without an SVG name are written as
`tetra:<mode>` and read back, and are Normal for other programs. The active
layer is written as `selected="true"` and lock alpha as
`tetra-lock-alpha="true"`. Loading flattens groups (their opacity and
visibility are folded into their layers). OpenRaster layers are 8-bit, so
float precision and values above 1 are not kept; use EXR export for those.
A failed `load` leaves the document unchanged.

`.ora` files are also plain images for Godot (`Image.load_from_file`, the
texture importer): the loader returns `mergedimage.png`, or composites the
layers when it is missing.

## TetraGegl

```
static is_available() -> bool        # false when the editor was built without libgegl-0.4
static get_version() -> String       # "0.4.48"; "" without GEGL
static list_operations() -> Array    # operation schemas, id "gegl:<name>", source "gegl"
static apply(operation_id, values: Dictionary, image) -> Image   # RGBAF, input size; null on failure
```

The list is built once by introspection (`gegl_list_operations`, property
specs, the `title`/`categories`/`description` keys and the `unit` UI
metadata). Kept are operations with an output and at most the main input
(generators such as `gegl:checkerboard` or `gegl:text` included); left out
are hidden, core, programming, meta, file, display and compositing
operations and those that need a path, curve or buffer object. Categories map
to GIMP's menus (`Filters/Blur`, `Filters/Enhance`, `Filters/Distorts`,
`Filters/Light and Shadow`, `Filters/Noise`, `Filters/Edge-Detect`,
`Filters/Generic`, `Filters/Artistic`, `Filters/Map`, `Filters/Render`,
`Colors`, `Colors/Tone Mapping`, ...). Parameter types: `float`, `int`,
`bool`, `enum` (value = the GEnum nick), `string`, `text`, `file`, `color`
(linear), `seed`; `min`/`max`, `ui_min`/`ui_max` and `unit` (`px`, `°`) where
GEGL provides them.

GEGL starts on the first call that needs it (about a second for the first
`list_operations`), with the application licence set to GPL3 so the GPL3+
operations are present, and without OpenCL. Its log messages only show with
`--verbose`. All calls are serialised by a mutex, so they may come from any
thread. The result is always the canvas rectangle of the input: operations
that grow or move the image are clipped to it.

The module registers itself in `TetraEngineRegistry` as "GEGL" at start-up
(details "libgegl-0.4; the operations are listed on first use") and updates
the entry to "<n> operations" once the list exists.

## Threading rules

- A `TetraImageDocument` belongs to one thread (normally the main thread).
  Its own heavy loops already run on the `WorkerThreadPool`.
- From other threads only call the static `TetraImageDocument.filter_image`,
  `get_native_operations`, `get_operations` and the `TetraGegl` methods.
- `compute_operation` reads the document; call it from a worker thread only
  if nothing changes the document meanwhile. The safe pattern is the snapshot
  shown below.

## Typical call sequences

Open and display:

```gdscript
var doc := TetraImageDocument.new()
doc.changed.connect(_on_changed)                 # queue a refresh of the rect
if doc.load(path) != OK: ...
var view := doc.composite()                      # RGBA8 sRGB -> ImageTexture
# Textures cannot be updated in part: keep one ImageTexture per tile (for
# example 256 px) and on refresh re-composite only the tiles that intersect
# doc.get_and_clear_dirty_rect():
tile_texture.update(doc.composite(tile_rect))
```

Paint a stroke with incremental refresh:

```gdscript
var settings := TetraImageDocument.get_default_stroke_settings(TetraImageDocument.TOOL_PAINTBRUSH)
settings["color"] = ui_color.srgb_to_linear()
settings["size"] = 24.0
doc.stroke_begin(doc.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH, settings)   # mouse down
doc.stroke_add_point(canvas_position, pressure)   # mouse down and every motion event
#   -> changed(rect) for each call; refresh the tiles under get_and_clear_dirty_rect() once per frame
doc.stroke_end()                                  # mouse up: one undo step ("Paintbrush")
```

Preview and commit a filter:

```gdscript
var layer := doc.get_active_layer()
# Quick: on the main thread.
var preview := doc.compute_operation(layer, "gegl:gaussian-blur", values)
# Or off the main thread, on a snapshot:
var snapshot := doc.layer_get_image(layer)
WorkerThreadPool.add_task(func() -> void:
	var filtered := TetraImageDocument.filter_image(snapshot, "gegl:gaussian-blur", values)
	_show_preview.call_deferred(filtered))
# Show the preview without touching the document, e.g. on a scratch copy.
# Commit, either directly (runs the operation again, through the selection):
doc.apply_operation(layer, "gegl:gaussian-blur", values)
# or with the preview from compute_operation, already limited to the selection:
doc.begin_undo_group("Gaussian Blur")
doc.layer_set_image(layer, preview)
doc.end_undo_group()
```

A `filter_image` result is the bare filter output, not limited by the
selection; to commit it through the selection use `apply_operation` instead.

## Workspace

`editor/` adds the main screen **Image** next to 2D, 3D and Script
(`TetraImageEditorPlugin`, Ctrl+F6). It edits one image at a time.

| Class (EDITOR level) | File | What it is |
| --- | --- | --- |
| `TetraImageEditorPlugin` | `editor/tetra_image_editor_plugin.*` | The main-screen plugin; opens image resources in the workspace. |
| `TetraImageWorkspace` (VBoxContainer) | `editor/tetra_image_workspace.*`, `..._menus.cpp` | Menu bar, toolbox, canvas, Tool Options, Layers, status bar; the scripting surface. |
| `TetraImageCanvas` (TetraCanvasView) | `editor/tetra_image_canvas.*` | Tiled display of the composite and the pointer tools. |
| `TetraImageLayersPanel` (VBoxContainer) | `editor/tetra_image_layers_panel.*` | The layer list with mode, opacity and stack buttons. |
| `TetraImageColorSwatch` (Control) | `editor/tetra_image_color_swatch.*` | Foreground/background colour area. |
| `TetraImageTools` (not in ClassDB) | `editor/tetra_image_tools.*` | Tool names, shortcuts, hints and option schemas. |

### What the user can do

- **Start**: the welcome page offers New Image... and Open...; double-clicking
  a png, jpg, jpeg, webp, exr, tga, bmp or ora file in the FileSystem dock
  opens it here (the plugin `handles()` `Texture2D` and `Image` resources
  whose file has one of these extensions).
- **Canvas**: wheel zooms about the pointer, middle-drag or Space+drag pans.
  The composite is shown through 256 px textures; an edit re-composites its
  rectangle once per frame and uploads only the tiles it touches. Above
  100 % the pixels are shown unfiltered. Transparency shows the editor's
  checkerboard, the selection is outlined with marching ants, and the brush
  outline follows the pointer. A pen's pressure reaches the stroke (enable
  "Pressure Changes Size/Opacity" in the Tool Options); the other end of a
  pen erases. Right-click or Escape cancels the gesture under way.
- **Tools** (toolbox on the left, GIMP's keys):

  | Tool | Key | Gesture |
  | --- | --- | --- |
  | Rectangle Select / Ellipse Select | R / E | Drag. Shift adds, Ctrl subtracts, Shift+Ctrl intersects; a click selects nothing. |
  | Fuzzy Select | U | Click the area of similar colour. |
  | Move | M | Drag the active layer. |
  | Zoom | Z | Click zooms in, Ctrl+click out. |
  | Color Picker | O | Click sets the foreground colour, Ctrl+click the background. |
  | Bucket Fill | Shift+B | Click. |
  | Gradient | G | Drag from start to end (foreground to background). |
  | Pencil / Paintbrush / Eraser / Airbrush | N / P / Shift+E / A | Drag; Shift+click draws a straight line from the last point. |
  | Clone | C | Ctrl+click sets the source, then paint. |
  | Smudge | S | Drag. |

  X swaps and D resets the colours, [ and ] change the brush size. The
  **Tool Options** panel is a `TetraParamPanel` generated from
  `TetraImageTools::get_options_schema()` (size, hardness, opacity, flow,
  spacing, paint mode, threshold, gradient shape and repeat, selection mode
  and feather, ...); every tool keeps its own values. Clicking a square of the
  colour area opens Godot's ColorPicker.
- **Layers**: top layer first, with eye, thumbnail, name (double-click
  renames) and alpha lock; mode and opacity of the active layer; new,
  duplicate, raise, lower, merge down, delete.
- **Menus**: File (New... Ctrl+N, Open... Ctrl+O, Save Ctrl+S, Save As...
  Ctrl+Shift+S, Export As... Ctrl+Shift+E, Close Ctrl+W), Edit (Undo Ctrl+Z
  and Redo Ctrl+Shift+Z or Ctrl+Y with the step name, Clear Delete, Fill with
  FG Ctrl+, / BG Ctrl+.), Select (All Ctrl+A, None Ctrl+Shift+A, Invert
  Ctrl+I, Feather..., Grow..., Shrink...), View (zoom in =, out -, 100 % 1,
  fit Ctrl+Shift+J), Image (flip, rotate, Scale Image..., Canvas Size...,
  Crop to Selection, Flatten Image), Layer (New Ctrl+Shift+N, Duplicate
  Ctrl+Shift+D, Raise, Lower, Merge Down, Delete, Flip), and **Colors** and
  **Filters**, built from `TetraImageDocument.get_operations()` (native and
  GEGL) as submenus by category. Filters also has Repeat (Ctrl+F) and Search
  Filters... (/), a `TetraOperationBrowser`.
- **Filters**: an operation with parameters opens a `TetraOperationDialog`.
  The preview is computed on a worker thread from a snapshot of the layer
  (a newer request replaces a waiting one) and shown on the canvas as a real
  edit inside an open undo group; OK keeps it as one undo step, Cancel takes
  it back and leaves the history untouched. Operations without parameters are
  applied at once. The dialog reopens with the values used last. Its colour
  pickers show display (sRGB) colours; the workspace converts them to the
  linear colours the engines take, and back for defaults and remembered
  values.
- **Saving**: Save writes to the file the image came from. `.ora` keeps the
  layers; png, jpg, jpeg, webp and exr are written flattened. When such a
  file has more than one layer, Save asks whether to "Export Flattened" to it
  or to "Save as .ora" beside it. After Export Flattened the image still
  counts as unsaved, because its layers are in no file; when the `.ora`
  beside it already exists, Save as .ora opens the file dialog instead of
  overwriting it. Files inside the project are passed to
  `EditorFileSystem` (`update_file`, and a re-import when the file is already
  imported), so textures in open scenes show the new pixels. Unsaved changes
  mark the title with `(*)`; New, Open, Close and opening another file ask
  first, and quitting the editor lists the image among the unsaved work
  (the editor's "Save" button there can only save an image that needs no
  file name or format choice; otherwise the message says to save it in the
  workspace).

All keys can be changed in the editor's shortcut settings (`tetra_image/*`).
They act only while the workspace is shown and the keyboard focus is inside
it, and never while text is being typed.

### Scripting surface

`TetraImageWorkspace.get_singleton()` (static, bound) returns the workspace
inside the running editor. Everything a menu or a tool does is reachable:

```
new_document(width, height, fill = Color.WHITE) -> TetraImageDocument   # fill is a display (sRGB) colour; alpha 0 = transparent; does not ask
open_file(path) -> Error                 # does not ask
request_open_file(path)                  # asks first when there are unsaved changes
save() -> Error                          # to get_document_path(): .ora with layers, otherwise flattened
save_as(path) -> Error                   # .ora, png, jpg, jpeg, webp, exr; becomes the document's file
#   a flat file of an image with several layers leaves is_modified() true: the layers are not in it
export_as(path) -> Error                 # flattened copy; the document keeps its file
close_document()
get_document() -> TetraImageDocument, has_document(), get_document_path(), is_modified(), can_save_directly()

set_tool(name) -> bool, get_tool() -> String, get_tool_names() -> PackedStringArray
#   paintbrush pencil eraser airbrush smudge clone bucket-fill gradient color-picker
#   rect-select ellipse-select fuzzy-select move zoom
get_tool_options_schema(tool) -> Dictionary     # operation "tool:<name>" of the shared schema
get_tool_options(tool) -> Dictionary
set_tool_option(tool, name, value)              # percentages 0..100, thresholds 0..255
set_foreground_color(color), get_foreground_color(), set_background_color(color), get_background_color()   # display colours
swap_colors(), reset_colors()

get_operations() -> Array, are_operations_loaded() -> bool    # listed on a worker thread when the workspace is first shown
apply_operation(id, values = {}) -> Error       # at once on the active layer, one undo step
show_operation_dialog(id, values = {})          # dialog with live preview; without parameters applies in the background (colour values, as everywhere in the API, are linear)
repeat_last_operation()
is_filter_running() -> bool, is_preview_shown() -> bool

undo() -> bool, redo() -> bool
menu_option(option)                             # TetraImageWorkspace.FILE_NEW ... VIEW_ZOOM_FIT, the menu items
zoom_to_fit(), set_zoom(zoom), get_zoom()

get_image_canvas() -> TetraImageCanvas, get_layers_panel() -> TetraImageLayersPanel
get_tool_options_panel() -> TetraParamPanel, get_operation_dialog() -> TetraOperationDialog
get_menu_bar() -> MenuBar, get_menu(name) -> PopupMenu     # "File" "Edit" "Select" "View" "Image" "Layer" "Colors" "Filters"
get_status_text() -> String, get_title_text() -> String
```

Signals: `document_changed()`, `operations_loaded()`, `operation_applied(id)`.

`TetraImageCanvas` performs pointer gestures with the active tool, in image
pixels:

```
tool_press(position, pressure = 1.0, shift = false, ctrl = false)
tool_drag(position, pressure = 1.0)
tool_release(), tool_cancel(), is_tool_active()
set_clone_source(position), has_clone_source_point()
set_input_locked(bool), is_input_locked()
get_tile_count(), get_tile_update_count(), get_outline_point_count()    # for tests
```

Signals: `pointer_moved(position)`, `pointer_exited()`,
`color_picked(color, background)`, `status_message(text)`; plus everything of
`TetraCanvasView`.

`TetraImageLayersPanel`: `new_layer()`, `duplicate_layer()`, `raise_layer()`,
`lower_layer()`, `merge_down()`, `delete_layer()`, `get_listed_names()` (top
layer first), `get_layer_tree() -> Tree`.

```gdscript
var ws := TetraImageWorkspace.get_singleton()
var doc := ws.new_document(1024, 768)
ws.set_tool("paintbrush")
ws.set_tool_option("paintbrush", "size", 40.0)
ws.set_foreground_color(Color.RED)
var canvas := ws.get_image_canvas()
canvas.tool_press(Vector2(100, 100))
canvas.tool_drag(Vector2(400, 300))
canvas.tool_release()
ws.apply_operation("gegl:gaussian-blur", {"std-dev-x": 4.0, "std-dev-y": 4.0})
ws.save_as("res://painting.ora")
```

Two engine methods exist for the workspace's preview and are bound as well:
`cancel_undo_group()` reverts and drops everything since the outermost
`begin_undo_group()` (the redo stack is kept), and
`apply_filtered_image(layer_id, filtered, name)` commits an already filtered
image through the selection and `lock_alpha` as one undo step.

## Build

`SCsub` looks for `gegl-0.4` with pkg-config. When it is found, the module is
compiled with `TETRA_GEGL_ENABLED` and the GEGL libraries are added to the
final link; otherwise `TetraGegl` reports itself unavailable and everything
else works. OpenRaster reading and writing uses the engine's bundled minizip.

## Tests

`scripts/test.sh tetra_paint` runs `test_document.gd` (layers, modes,
compositing, undo), `test_selection.gd`, `test_paint.gd`, `test_edit.gd`
(fills, gradients, transforms), `test_operations.gd`, `test_gegl.gd` and
`test_files.gd`. They also write `build/screens/paint-layer-modes.png`,
`paint-strokes.png`, `paint-gradients.png` and `paint-gegl.png`.

The workspace classes exist only inside the running editor, so their
behaviour is checked by a driver that runs there (under Xvfb) and prints
`screenshot image workspace checks: <n> checks, <m> failed`, with one
`screenshot FAILED: ...` line per failure:

```sh
scripts/screenshot.sh build/screens/image-checks.png Image res://tetra_paint/shots/workspace_checks.gd
```

It covers the welcome page, the tiled canvas (one tile uploaded for a stroke
inside one tile, painting speed on 2048 x 2048, real mouse events), every
tool and its key, the focus rules of the keys, the layers panel, the menus
and their dialogs, the filter preview (supersede, cancel, commit, repeat,
search, colours picked in a filter dialog, replacing the image while a filter
is being applied) and opening, saving and re-importing a project file. The other
drivers in `tests/godot/project/tetra_paint/shots/` make pictures:

```sh
scripts/screenshot.sh build/screens/image-workspace.png Image res://tetra_paint/shots/workspace_layers.gd
scripts/screenshot.sh build/screens/image-filter-dialog.png Image res://tetra_paint/shots/workspace_filter_dialog.gd
scripts/screenshot.sh build/screens/image-selection-menu.png Image res://tetra_paint/shots/workspace_selection_menu.gd
scripts/screenshot.sh build/screens/image-zoom.png Image res://tetra_paint/shots/workspace_zoom.gd
```
