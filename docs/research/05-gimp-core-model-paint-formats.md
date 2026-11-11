# 05 — GIMP 3.2.8 core: image model, layer modes, paint engine, selections, file/data formats

Research input for the native C++17 port of GIMP's raster core into Tetra's engine-independent paint core (`modules/tetra_paint`).

- Source read: `/home/user/Claude-Cloud/upstream/gimp`, tag `GIMP_3_2_8`, commit `7addd2b77efb798bbdc4d2fb082e786a48478ced` (`meson.build`: `version: '3.2.8'`). All paths below are relative to that root unless prefixed.
- Licence of everything quoted: GPL-3.0-or-later (GIMP `COPYING`), compatible with Tetra. Ported code must keep the upstream copyright headers (e.g. `gimpoperationlayermode-blend.c`: "Copyright (C) 2017 Michael Natterer, Øyvind Kolås, Ell").
- Conventions: `in` = backdrop/destination pixel, `layer` = source pixel (the layer being composited, GEGL pad `aux`), `mask` = `aux2` (one float per pixel), `comp` = blend result, `out` = result. Pixel layout is 4 floats, `RED 0, GREEN 1, BLUE 2, ALPHA 3` (`app/core/core-types.h:35-38`). Alpha is **non-premultiplied (straight)** everywhere in the layer-mode code.

## 0. Facts about this checkout that change the plan

| Topic | Finding | Evidence |
|---|---|---|
| XCF spec text | **Not present.** `devel-docs/` contains only `README.md`, `c.vim`, `gimp-module-dependencies.svg`, `meson.build`, `os-support.txt`, `reference/`. No `xcf.txt`. Section 7 is derived purely from `app/xcf/*.c`. | `ls devel-docs` |
| `gimp-data` submodule | **Empty directory** (submodule not initialised: `git submodule status` prints `-fe4ecc0bf70fc8ff3bd929ce6abdd50fb5621072 gimp-data`). It normally provides all bundled **brushes, patterns, images, icons, cursors, splash**. So this checkout has **0 brushes and 0 patterns**. | `ls -la gimp-data`; `.gitmodules`; `meson.build:1858-1863` errors out if `gimp-data/meson.build` is missing |
| `data/` (in-tree) | dynamics 18 files, gradients 78 `.ggr`, palettes 40 `.gpl`, tool presets 39 `.gtp`, tags, tips, environ, interpreters (see §3.10) | `ls data/*` |
| `libgimpcolor` | **No GimpRGB/GimpHSV/GimpHSL/GimpCMYK and no `gimpcolorspace.c` any more.** The directory only has `gimpadaptivesupersample.c gimpbilinear.c gimpcairo.c gimpcolor-parse.c gimpcolor.c gimpcolormanaged.c gimpcolorprofile.c gimpcolortransform.c gimppixbuf.c`. All colour-model conversion in 3.x goes through **babl formats** via `GeglColor` (`"HSV float"`, `"HSL float"`, `"CIE LCH(ab) float"`, `"CMYK float"`, …). The only hand-written RGB↔HSV/HSL code left is `app/operations/layer-modes-legacy/gimpcolor-legacy.c` (quoted in §2.6). | `ls libgimpcolor` |
| babl source | **Not in the pinned upstream set** (only system `libbabl-0.1` 0.1.108 headers/so). Conversion constants in §2.7/§8 were measured by calling the system library through ctypes (script: `/tmp/claude-0/-home-user-Claude-Cloud/6d66b3ab-94cb-5da7-8187-9dab6acab107/scratchpad/gimpdoc/bablprobe.py`), not read from source. |
| Number of tools | `app/tools` has 114 `.c` files; tools are GTK-entangled and are *not* part of this doc except where they define parameters (e.g. `threshold / 255.0`). |

---

## 1. Image model

### 1.1 Class tree (GObject, all in `app/core` unless noted)

```
GimpObject (name)
 └ GimpViewable (preview, parent/children, expanded)
    ├ GimpImage                                   gimpimage.c (6502 lines), gimpimage-private.h
    ├ GimpFilter (GeglNode *node, active, is_last_node)          gimpfilter.c
    │  ├ GimpItem (id, tattoo, w/h, offset, visible, locks, colour tag, parasites)   gimpitem.c
    │  │  ├ GimpDrawable (GeglBuffer, filter stack, mode node, floating sel)        gimpdrawable.c
    │  │  │  ├ GimpLayer (opacity, mode, spaces, composite mode, lock alpha, mask)  gimplayer.c
    │  │  │  │  ├ GimpGroupLayer (children GimpDrawableStack, own GimpProjection)   gimpgrouplayer.c
    │  │  │  │  ├ GimpTextLayer  (GimpText, GimpRasterizable)   app/text/gimptextlayer.c
    │  │  │  │  ├ GimpVectorLayer (GimpPath + fill/stroke options, GimpRasterizable) app/path/gimpvectorlayer.c  [new in 3.2, XCF ≥ 24]
    │  │  │  │  └ GimpLinkLayer  (GimpLink → external file, GimpRasterizable)       gimplinklayer.c, gimplink.c [new in 3.2, XCF ≥ 25]
    │  │  │  └ GimpChannel (colour+opacity for display, show_masked, boundary cache) gimpchannel.c
    │  │  │     ├ GimpSelection (the image's selection mask; overrides most vfuncs) gimpselection.c
    │  │  │     ├ GimpLayerMask (back-pointer to layer)                             gimplayermask.c
    │  │  │     └ GimpDrawableFilterMask (mask of an NDE filter)                    gimpdrawablefiltermask.c
    │  │  └ GimpPath (GQueue of GimpStroke)       app/path/gimppath.c
    │  └ GimpDrawableFilter (non-destructive filter attached to a drawable)         gimpdrawablefilter.c
    └ GimpData → GimpBrush, GimpPattern, GimpGradient, GimpPalette, GimpDynamics, GimpToolPreset, GimpMybrush, GimpFont …
GimpProjection (GimpPickable; renders a GimpProjectable's graph into a GeglBuffer)  gimpprojection.c
GimpUndo → GimpUndoStack, GimpItemUndo, GimpDrawableUndo, GimpLayerUndo, …          gimpundo*.c
```
`GimpRasterizable` (`gimprasterizable.c`) is the 3.2 interface shared by text/vector/link layers: they keep a parametric source and a "rasterized" flag (pixels were edited by hand; undo type `GIMP_UNDO_RASTERIZABLE`).

### 1.2 `GimpImage` state (`app/core/gimpimage-private.h`, `struct _GimpImagePrivate`)

| Field | Type | Meaning |
|---|---|---|
| `ID` | gint | unique runtime id |
| `width`, `height` | gint | canvas size in px; limits `GIMP_MIN_IMAGE_SIZE 1` … `GIMP_MAX_IMAGE_SIZE 524288` (2^19) (`libgimpbase/gimplimits.h`) |
| `xresolution`, `yresolution` | gdouble | dpi; limits `GIMP_MIN_RESOLUTION 5e-3` … `GIMP_MAX_RESOLUTION 1048576.0` |
| `resolution_unit` | GimpUnit* | built-in ids `GIMP_UNIT_PIXEL 0, INCH 1, MM 2, POINT 3, PICA 4, END 5, PERCENT 65536` (`libgimpbase/gimpbaseenums.h`) or user unit |
| `base_type` | GimpImageBaseType | `GIMP_RGB 0`, `GIMP_GRAY 1`, `GIMP_INDEXED 2` |
| `precision` | GimpPrecision | see table below |
| `new_layer_mode` | GimpLayerMode | default mode for new layers |
| `show_all`, `bounding_box` | | "show all" (render outside canvas) support |
| `palette`, `babl_palette_rgb/rgba` | GimpPalette*, Babl* | indexed colormap (max 256 entries, `GIMP_IMAGE_COLORMAP_SIZE` = 768 bytes) |
| `color_profile`, `layer_space`, `hidden_profile` | GimpColorProfile*, Babl* | ICC profile of the image; babl space of layers |
| `simulation_profile`, `simulation_intent`, `simulation_bpc` | | soft-proofing settings |
| `metadata` | GimpMetadata* | Exif/XMP/IPTC (gexiv2) |
| `file`, `imported_file`, `exported_file`, `save_a_copy_file`, `untitled_file` | GFile* | |
| `xcf_compression` | gboolean | TRUE = zlib tiles when saving |
| `dirty`, `dirty_time`, `export_dirty` | | dirty counters |
| `tattoo_state` | GimpTattoo (guint32) | last used permanent item id |
| `projection`, `graph`, `visible_mask` | GimpProjection*, GeglNode* | compositing of the whole image |
| `symmetries`, `active_symmetry` | GList*, GimpSymmetry* | painting symmetries |
| `guides` | GList of GimpGuide | each: orientation (`GIMP_ORIENTATION_HORIZONTAL 0 / VERTICAL 1`), position (int px, may be off-canvas since XCF 15), style (`GimpGuideStyle`: NONE, NORMAL, MIRROR, MANDALA, SPLIT_VIEW — non-NORMAL are "custom" and not saved) |
| `grid` | GimpGrid* | style (`GIMP_GRID_DOTS, INTERSECTIONS, ON_OFF_DASH, DOUBLE_DASH, SOLID`), fg/bg colour, x/y spacing, spacing unit, x/y offset, offset unit; stored as parasite `gimp-image-grid` |
| `sample_points` | GList of GimpSamplePoint | x, y, `GimpColorPickMode` (PIXEL, RGB_PERCENT, RGB_U8, GRAYSCALE, HSV, LCH, LAB, CMYK, XYY, YUV) |
| `layers`, `channels`, `paths` | GimpItemTree* | three item trees (only layers are really hierarchical) |
| `layer_stack` | GSList* | layers in MRU order |
| `stored_layer_sets`, `stored_channel_sets`, `stored_path_sets` | GList of GimpItemList | named sets / name-pattern sets (replaced the 2.x "linked" chain) |
| `floating_sel` | GimpLayer* | the floating selection layer |
| `selection_mask` | GimpChannel* (GimpSelection) | image-sized selection |
| `parasites` | GimpParasiteList* | name → (flags, bytes); persistent ones are saved in XCF |
| `visible[MAX_CHANNELS]`, `active[MAX_CHANNELS]` | gboolean[5] | per-component visibility / editability (R,G,B,A or Gray,A or Indexed,A) |
| `quick_mask_state`, `quick_mask_inverted`, `quick_mask_color`, `quick_mask_selected` | | Quick Mask (a channel named "Qmask") |
| `undo_stack`, `redo_stack`, `group_count`, `pushing_undo_group` | | undo system (§1.9) |

**`GimpPrecision`** (`libgimpbase/gimpbaseenums.h:902-922`). Value = component type code + TRC offset. This number is written verbatim into the XCF header for file version ≥ 7.

| Enum | Value | Component | TRC | babl RGBA format (`app/gegl/gimp-babl.c: gimp_babl_format`) | bytes/component |
|---|---|---|---|---|---|
| `GIMP_PRECISION_U8_LINEAR` | 100 | u8 | linear | `RGBA u8` | 1 |
| `GIMP_PRECISION_U8_NON_LINEAR` | 150 | u8 | non-linear | `R'G'B'A u8` | 1 |
| `GIMP_PRECISION_U8_PERCEPTUAL` | 175 | u8 | perceptual | `R~G~B~A u8` | 1 |
| `GIMP_PRECISION_U16_LINEAR` | 200 | u16 | linear | `RGBA u16` | 2 |
| `GIMP_PRECISION_U16_NON_LINEAR` | 250 | u16 | non-linear | `R'G'B'A u16` | 2 |
| `GIMP_PRECISION_U16_PERCEPTUAL` | 275 | u16 | perceptual | `R~G~B~A u16` | 2 |
| `GIMP_PRECISION_U32_LINEAR` | 300 | u32 | linear | `RGBA u32` | 4 |
| `GIMP_PRECISION_U32_NON_LINEAR` | 350 | u32 | non-linear | `R'G'B'A u32` | 4 |
| `GIMP_PRECISION_U32_PERCEPTUAL` | 375 | u32 | perceptual | `R~G~B~A u32` | 4 |
| `GIMP_PRECISION_HALF_LINEAR` | 500 | half | linear | `RGBA half` | 2 |
| `GIMP_PRECISION_HALF_NON_LINEAR` | 550 | half | non-linear | `R'G'B'A half` | 2 |
| `GIMP_PRECISION_HALF_PERCEPTUAL` | 575 | half | perceptual | `R~G~B~A half` | 2 |
| `GIMP_PRECISION_FLOAT_LINEAR` | 600 | float | linear | `RGBA float` | 4 |
| `GIMP_PRECISION_FLOAT_NON_LINEAR` | 650 | float | non-linear | `R'G'B'A float` | 4 |
| `GIMP_PRECISION_FLOAT_PERCEPTUAL` | 675 | float | perceptual | `R~G~B~A float` | 4 |
| `GIMP_PRECISION_DOUBLE_LINEAR` | 700 | double | linear | `RGBA double` | 8 |
| `GIMP_PRECISION_DOUBLE_NON_LINEAR` | 750 | double | non-linear | `R'G'B'A double` | 8 |
| `GIMP_PRECISION_DOUBLE_PERCEPTUAL` | 775 | double | perceptual | `R~G~B~A double` | 8 |

`GimpComponentType`: `U8 100, U16 200, U32 300, HALF 500, FLOAT 600, DOUBLE 700`. `GimpTRCType` (`core-enums.h:483`): `GIMP_TRC_LINEAR, GIMP_TRC_NON_LINEAR, GIMP_TRC_PERCEPTUAL`. "Non-linear" = the TRC of the image's own colour profile (babl `R'G'B'`); "perceptual" = always the sRGB TRC regardless of profile (babl `R~G~B~`). For an sRGB image the two are numerically identical (measured). Default image: `GIMP_PRECISION_U8_NON_LINEAR`.

Drawable pixel layouts (`GimpImageType`, written per layer in XCF): `GIMP_RGB_IMAGE 0` (3 comps), `GIMP_RGBA_IMAGE 1` (4), `GIMP_GRAY_IMAGE 2` (1), `GIMP_GRAYA_IMAGE 3` (2), `GIMP_INDEXED_IMAGE 4` (1 byte index), `GIMP_INDEXEDA_IMAGE 5` (index + alpha). Channels/masks/selection: 1 component, format `gimp_babl_mask_format(precision)` = `"Y u8" / "Y u16" / "Y u32" / "Y half" / "Y float" / "Y double"` (always linear "Y", same component type as the image).

### 1.3 Item, drawable, layer, channel attributes

`GimpItemPrivate` (`app/core/gimpitem.c`): `ID`, `tattoo` (permanent, saved), `image`, `parasites`, `width,height`, `offset_x,offset_y` (px, relative to image origin; may be negative), bitfields `visible`, `bind_visible_to_active`, `lock_content`, `lock_position`, `lock_visibility`, `removed`, `color_tag` (`GimpColorTag`: NONE 0, BLUE, GREEN, YELLOW, ORANGE, BROWN, RED, VIOLET, GRAY 8), `offset_nodes`.

Item virtual operations (these are the hooks every item kind implements; see `gimpitem.h`): `duplicate, convert, rename, start_move/end_move, start_transform/end_transform, translate, scale, resize, flip, rotate, transform, get_clip, fill, stroke, to_selection, bounds`.

`GimpDrawablePrivate` (`gimpdrawable-private.h`): `buffer` (GeglBuffer, extent `(0,0,w,h)` in item-local coords), `shadow`, `format_profile`, `source_node`, `buffer_source_node`, `filter_stack` (GimpFilterStack of GimpDrawableFilter + the floating-sel filter), `bounding_box` (buffer extent grown by filters), `floating_selection`, `fs_filter`, `fs_crop_node`, `fs_applicator`, `mode_node`, `paint_count`, `paint_buffer`, `paint_copy_region`, `paint_update_region`, `push_resize_undo`.

`struct _GimpLayer` (`gimplayer.h`):

| Field | Type / range | Notes |
|---|---|---|
| `opacity` | gdouble 0..1 (`GIMP_OPACITY_OPAQUE` = 1.0) | |
| `mode` | GimpLayerMode | §2.1 |
| `blend_space` | GimpLayerColorSpace | `AUTO 0, RGB_LINEAR 1, RGB_NON_LINEAR 2, LAB 3, RGB_PERCEPTUAL 4` (`operations-enums.h:29-33`; UI names "Auto", "RGB (linear)", "RGB (from color profile)", "LAB", "RGB (perceptual)") |
| `composite_space` | GimpLayerColorSpace | same enum |
| `composite_mode` | GimpLayerCompositeMode | `AUTO 0, UNION 1, CLIP_TO_BACKDROP 2, CLIP_TO_LAYER 3, INTERSECTION 4` |
| `effective_mode`, `effective_blend_space`, `effective_composite_space`, `effective_composite_mode` | | what the mode node really uses; differs from the user values only for group layers (pass-through strength reduction, §1.7) |
| `excludes_backdrop` | gboolean | TRUE when the effective composite mode does not include the destination region (CLIP_TO_LAYER, INTERSECTION) |
| `lock_alpha` | gboolean | painting/editing cannot change alpha |
| `mask` | GimpLayerMask* | same size and offset as the layer |
| `apply_mask` | gboolean | mask feeds `aux2` of the mode node |
| `edit_mask` | gboolean | paint tools target the mask instead of the layer |
| `show_mask` | gboolean | the mask (as grey) is shown instead of the layer, composited NORMAL |
| `fs.drawable`, `fs.segs…` | | floating-selection attachment |

AUTO resolution (`gimplayer.c: gimp_layer_get_real_blend_space/composite_space/composite_mode`): `AUTO` → the per-mode default from `layer_mode_infos[]` (§2.1). Mutability flags in §2.1 decide whether the UI lets the user override.

`struct _GimpChannel` (`gimpchannel.h`): `color` (GeglColor; alpha = display opacity), `show_masked` (colour shows masked vs selected area), display nodes, boundary cache (`segs_in/out`), `empty`, `full`, `bounds_known`, `x1,y1,x2,y2`.

Layer mask creation types `GimpAddMaskType`: `GIMP_ADD_MASK_WHITE 0, BLACK, ALPHA, ALPHA_TRANSFER, SELECTION, COPY, CHANNEL 6`. Mask removal `GimpMaskApplyMode`: `GIMP_MASK_APPLY 0, GIMP_MASK_DISCARD 1`.

### 1.4 Non-destructive filters (`GimpDrawableFilter`, 3.0+)

`struct _GimpDrawableFilter` (`gimpdrawablefilter.c`): `drawable`, `mask` (GimpDrawableFilterMask = a channel; initialised from the selection at creation), `operation` (GeglNode of any GEGL op allowed by `gimp_gegl_op_nde_allowed()`), `has_input`, `has_custom_name`, `clip` (clip result to drawable extent vs. allow growth), `region` (`GIMP_FILTER_REGION_SELECTION` / `GIMP_FILTER_REGION_DRAWABLE`), crop/preview-split fields, `opacity` (0..1), `paint_mode` (default `GIMP_LAYER_MODE_REPLACE`), `blend_space`, `composite_space`, `composite_mode`, `add_alpha`, `color_managed`, `override_constraints`, `filter_area`, `filter_clip`, `translate/crop_before/crop_after` nodes, `applicator`, `temporary`, `to_be_merged`.

Stack API (`gimpdrawable-filters.h`): `gimp_drawable_add_filter, _remove_filter, _raise_filter, _lower_filter, _merge_filter, _merge_filters, _clear_filters, _has_visible_filters, _n_editable_filters, _get_filters`. Filters apply to the drawable's **source** output *before* the layer's own mode/opacity/mask (see graph below). In XCF they are stored per layer (v≥20) and per channel/mask (v≥26) — §7.6.

### 1.5 Projection: how the image is composited

There is no hand-written compositing loop over layers. Each `GimpFilter` owns a `GeglNode` graph; a `GimpFilterStack` (`gimpfilterstack.c`) chains the nodes of all *active* (visible) filters bottom-to-top: `gimp_filter_stack_add_node()` links `node_below → node → node_above`, where the top-most node feeds the stack graph's output proxy. `gimp_filter_stack_update_last_node()` marks the **bottom-most active** filter `is_last_node`.

Per-layer graph (`gimplayer.c: gimp_layer_get_node`, `gimpdrawable.c: gimp_drawable_get_node/get_source_node`):

```
 "input" (result of everything below) ───────────────────────────────┐
                                                                     ▼ input
 drawable source_node:                                        ┌────────────┐
   gegl:buffer-source(layer buffer) → [filter_stack graph] ──► translate ──► aux   │ mode_node  │──► "output"
                                       (NDE filters, FS)      (offset_x,y)  │ gimp:<op>  │
 mask source_node → translate(offset) ─────────────────────► aux2 (if apply_mask)  └────────────┘
```
- `mode_node` is created as `"gimp:normal"` and re-typed by `gimp_gegl_mode_node_set_mode()` (`app/gegl/gimp-gegl-nodes.c`) to `gimp_layer_mode_get_operation_name(mode)` with properties `layer-mode`, `opacity`, `blend-space`, `composite-space`, `composite-mode` (AUTO values are resolved to the mode defaults *before* being set on the node).
- `show_mask`: the mask is connected to `aux` instead of the layer, mode forced to `GIMP_LAYER_MODE_NORMAL`, blend AUTO, composite space = the layer's real composite space, composite mode AUTO (`gimp_layer_update_mode_node`).
- `apply_mask == FALSE`: nothing is connected to `aux2`.
- **Bottom layer rule** (`gimpoperationlayermode.c: prepare`): if the `input` pad has no data the node "is_last_node": if the mode does not affect the source region, `process_last_node` is used: `out.rgb = layer.rgb; out.a = layer.a * opacity * mask`; otherwise the normal function runs with composite mode forced to **UNION**.
- If a mask is connected but does not intersect the ROI, opacity is treated as 0 (`gimp_operation_layer_mode_parent_process`).
- Short-circuits (`real_parent_process`): no input + aux present, mode doesn't alter aux, opacity 1, no mask → output = aux buffer; input but no aux and mode doesn't alter input → output = input.

Group layers (`gimpgrouplayer.c`): a group owns a `GimpDrawableStack` of children and a private `GimpProjection`. **Normal group**: children are rendered in isolation (starting from transparent) into the group projection buffer; that buffer is the group's source. **Pass-through** (`GIMP_LAYER_MODE_PASS_THROUGH`, `private->pass_through`): the group's source graph is `input → children graph → output` (the backdrop flows *through* the children; `gimp_group_layer_update_source_node`), and the group's own mode node is `gimp:pass-through` (= REPLACE math, §2.5) so that group opacity/mask interpolate between "backdrop" and "backdrop with children applied". If a pass-through group `excludes_backdrop` (any active child excludes it) the mode node's input is disconnected. Strength reduction (`gimp_group_layer_get_effective_mode`): a pass-through group with no visible filters is silently treated as NORMAL when it has no active children, or one active child / all children NORMAL+UNION with identical spaces, **and** (opacity is 1 and no applied mask, or the group's composite space equals the children's).

Image level (`gimpimage.c: gimp_image_get_graph`, `gimpprojection.c`): image graph = layer stack graph → `visible_mask` (`gimp:mask-components`, hides invisible colour components) → channel stack graph (visible channels / quick mask are tinted overlays) → output. `GimpProjection` renders invalidated rectangles in chunks aligned to `GIMP_PROJECTION_UPDATE_CHUNK_WIDTH/HEIGHT = 32` px on an idle handler with priority, into a GeglBuffer whose format is `gimp_projectable_get_format(projectable)` (for an image: its projection format — the layer format with alpha; exact getter not traced).

**Order of operations per layer (what the port must reproduce):** layer pixels → NDE filter stack (each filter: op, then composited over its input with the filter's own mode/opacity/mask via a `GimpApplicator`) → floating-selection filter (if any) → translate by offset → mode node: convert `in` and `layer` to the **composite space** format; if blend space ≠ composite space convert both to blend space, run blend function, convert `comp` back; run composite function which multiplies `layer.alpha × opacity × mask` and mixes. Mask and opacity are therefore applied **inside** the composite step, after blending.

### 1.6 Tiles and buffers

- Pixel storage is `GeglBuffer` (sparse, tiled, COW, swap-backed, mipmapped). GIMP only configures `tile-cache-size`, `swap`, `swap-compression`, thread count (`app/gegl/gimp-gegl.c`). GEGL chooses tile size (GEGL default; not set by GIMP).
- XCF on-disk tiles are fixed **64×64** (`XCF_TILE_WIDTH/HEIGHT`, `app/xcf/xcf-private.h`).
- `GimpTempBuf` (`app/core/gimptempbuf.c`) = plain contiguous `width × height × bpp` array with a babl format; used for brush masks, pixmaps, pattern data and the paint buffer. This is the type to mirror with a plain `std::vector<float>`-backed image in the port.
- Shadow buffer (`gimp_drawable_get_shadow_buffer`) is a scratch target for plug-ins merged via `gimp_drawable_merge_shadow_buffer`.
- Paint-time buffering: `gimp_drawable_start_paint / _end_paint / _flush_paint / _is_painting` (`gimpdrawable.h`; state `paint_count`, `paint_buffer`, `paint_copy_region`, `paint_update_region`) route a stroke's writes through a separate paint buffer that is flushed to the real buffer in batches, so the projection is not invalidated per dab.

### 1.7 Undo system

- Two `GimpUndoStack`s per image (`undo_stack`, `redo_stack`); API `gimp_undo_stack_new, _push_undo, _pop_undo, _free_bottom, _peek, _get_depth`. A `GimpUndoStack` is itself a `GimpUndo`, which is how **groups** nest: `gimp_image_undo_group_start(image, GimpUndoType group, name)` pushes a stack; `gimp_image_undo_group_end` closes it (`group_count`, `pushing_undo_group`).
- `gimp_image_undo_push(image, GType undo_gtype, GimpUndoType, name, GimpDirtyMask, …props)` creates the step; each undo subclass implements `pop(undo, GimpUndoMode UNDO|REDO, accum)` by *swapping* stored state with current state (same object serves undo and redo) and `free`.
- Image-level API (`gimpimage-undo.h`): `gimp_image_undo, _redo, _strong_undo, _strong_redo, _undo_free, _undo_group_start/_end, _undo_push, _undo_can_compress, _undo_enable/_disable, _undo_freeze/_thaw, _undo_is_enabled, _get_undo_stack/_get_redo_stack/_get_undo_group_count`.
- Limits (`app/config/gimpcoreconfig.c`): `undo-levels` (min steps kept, range 0..2^20, default 5), `undo-size` (max memory, 0..`GIMP_MAX_MEMSIZE`; beyond it the oldest steps past `undo-levels` are freed with `gimp_undo_stack_free_bottom`).
- Drawable pixel undo (`GimpDrawableUndo`, type `GIMP_UNDO_DRAWABLE`) stores a GeglBuffer copy of the changed rectangle + x,y; paint strokes store only the bounding box `[x1,x2)×[y1,y2)` of the stroke cut from the pre-stroke copy (`gimp_paint_core_finish`).
- Events (`GimpUndoEvent`): `UNDO_PUSHED, UNDO_EXPIRED, REDO_EXPIRED, UNDO, REDO, UNDO_FREE, UNDO_FREEZE, UNDO_THAW`. `GimpDirtyMask` bits: `IMAGE 1, IMAGE_SIZE 2, IMAGE_META 4, IMAGE_STRUCTURE 8, ITEM 16, ITEM_META 32, DRAWABLE 64, PATH 128, SELECTION 256, ACTIVE_DRAWABLE 512, ALL 0xffff`.

**Complete `GimpUndoType` enum** (`app/core/core-enums.h:523-648`), in declaration order (values are sequential from 0).

Group types (`GIMP_UNDO_GROUP_FIRST = GROUP_NONE = 0` … `GIMP_UNDO_GROUP_LAST = GROUP_MISC`):

| # | Enum | UI description |
|---|---|---|
| 0 | `GIMP_UNDO_GROUP_NONE` | `<<invalid>>` (means "no group open") |
| 1 | `GIMP_UNDO_GROUP_IMAGE_SCALE` | Scale image |
| 2 | `GIMP_UNDO_GROUP_IMAGE_RESIZE` | Resize image |
| 3 | `GIMP_UNDO_GROUP_IMAGE_FLIP` | Flip image |
| 4 | `GIMP_UNDO_GROUP_IMAGE_ROTATE` | Rotate image |
| 5 | `GIMP_UNDO_GROUP_IMAGE_TRANSFORM` | Transform image |
| 6 | `GIMP_UNDO_GROUP_IMAGE_CROP` | Crop image |
| 7 | `GIMP_UNDO_GROUP_IMAGE_CONVERT` | Convert image |
| 8 | `GIMP_UNDO_GROUP_IMAGE_ITEM_REMOVE` | Remove item |
| 9 | `GIMP_UNDO_GROUP_IMAGE_ITEM_REORDER` | Reorder item |
| 10 | `GIMP_UNDO_GROUP_IMAGE_LAYERS_MERGE` | Merge layers |
| 11 | `GIMP_UNDO_GROUP_IMAGE_PATHS_MERGE` | Merge paths |
| 12 | `GIMP_UNDO_GROUP_IMAGE_QUICK_MASK` | Quick Mask |
| 13 | `GIMP_UNDO_GROUP_IMAGE_GRID` | Grid |
| 14 | `GIMP_UNDO_GROUP_IMAGE_COLORMAP_REMAP` | Colormap remapping |
| 15 | `GIMP_UNDO_GROUP_GUIDE` | Guide |
| 16 | `GIMP_UNDO_GROUP_SAMPLE_POINT` | Sample Point |
| 17 | `GIMP_UNDO_GROUP_DRAWABLE` | Layer/Channel |
| 18 | `GIMP_UNDO_GROUP_DRAWABLE_MOD` | Layer/Channel modification |
| 19 | `GIMP_UNDO_GROUP_MASK` | Selection mask |
| 20 | `GIMP_UNDO_GROUP_ITEM_VISIBILITY` | Item visibility |
| 21 | `GIMP_UNDO_GROUP_ITEM_LOCK_CONTENTS` | Lock/Unlock contents |
| 22 | `GIMP_UNDO_GROUP_ITEM_LOCK_POSITION` | Lock/Unlock position |
| 23 | `GIMP_UNDO_GROUP_ITEM_LOCK_VISIBILITY` | Lock/Unlock visibility |
| 24 | `GIMP_UNDO_GROUP_ITEM_PROPERTIES` | Item properties |
| 25 | `GIMP_UNDO_GROUP_ITEM_DISPLACE` | Move item |
| 26 | `GIMP_UNDO_GROUP_ITEM_SCALE` | Scale item |
| 27 | `GIMP_UNDO_GROUP_ITEM_RESIZE` | Resize item |
| 28 | `GIMP_UNDO_GROUP_LAYER_ADD` | Add layer |
| 29 | `GIMP_UNDO_GROUP_LAYER_ADD_ALPHA` | Add alpha channel |
| 30 | `GIMP_UNDO_GROUP_LAYER_ADD_MASK` | Add layer masks |
| 31 | `GIMP_UNDO_GROUP_LAYER_APPLY_MASK` | Apply layer masks |
| 32 | `GIMP_UNDO_GROUP_LAYER_REMOVE_ALPHA` | Remove alpha channel |
| 33 | `GIMP_UNDO_GROUP_LAYER_LOCK_ALPHA` | Lock/Unlock alpha channels |
| 34 | `GIMP_UNDO_GROUP_LAYER_OPACITY` | Set layers opacity |
| 35 | `GIMP_UNDO_GROUP_LAYER_MODE` | Set layers mode |
| 36 | `GIMP_UNDO_GROUP_CHANNEL_ADD` | Add channels |
| 37 | `GIMP_UNDO_GROUP_FS_TO_LAYER` | Floating selection to layer |
| 38 | `GIMP_UNDO_GROUP_FS_FLOAT` | Float selection |
| 39 | `GIMP_UNDO_GROUP_FS_ANCHOR` | Anchor floating selection |
| 40 | `GIMP_UNDO_GROUP_EDIT_PASTE` | Paste |
| 41 | `GIMP_UNDO_GROUP_EDIT_CUT` | Cut |
| 42 | `GIMP_UNDO_GROUP_TEXT` | Text |
| 43 | `GIMP_UNDO_GROUP_VECTOR` | Vector |
| 44 | `GIMP_UNDO_GROUP_TRANSFORM` | Transform |
| 45 | `GIMP_UNDO_GROUP_PAINT` | Paint |
| 46 | `GIMP_UNDO_GROUP_PARASITE_ATTACH` | Attach parasite |
| 47 | `GIMP_UNDO_GROUP_PARASITE_REMOVE` | Remove parasite |
| 48 | `GIMP_UNDO_GROUP_PATHS_IMPORT` | Import paths |
| 49 | `GIMP_UNDO_GROUP_MISC` | Plug-In |

Concrete undo steps ("Undo types which actually do something"; `GIMP_UNDO_IMAGE_TYPE` = 50 because `GIMP_UNDO_GROUP_LAST` = `GROUP_MISC` = 49). The integers are never serialised; use the names.

| # | Enum | UI description | # | Enum | UI description |
|---|---|---|---|---|---|
| 50 | `GIMP_UNDO_IMAGE_TYPE` | Image type | 81 | `GIMP_UNDO_GROUP_LAYER_RESUME_MASK` | Resume group layer mask |
| 51 | `GIMP_UNDO_IMAGE_PRECISION` | Image precision | 82 | `GIMP_UNDO_GROUP_LAYER_START_TRANSFORM` | Start transforming group layer |
| 52 | `GIMP_UNDO_IMAGE_SIZE` | Image size | 83 | `GIMP_UNDO_GROUP_LAYER_END_TRANSFORM` | End transforming group layer |
| 53 | `GIMP_UNDO_IMAGE_RESOLUTION` | Image resolution change | 84 | `GIMP_UNDO_GROUP_LAYER_CONVERT` | Convert group layer |
| 54 | `GIMP_UNDO_IMAGE_GRID` | Grid | 85 | `GIMP_UNDO_TEXT_LAYER` | Text layer |
| 55 | `GIMP_UNDO_IMAGE_METADATA` | Change metadata | 86 | `GIMP_UNDO_TEXT_LAYER_CONVERT` | Convert text layer |
| 56 | `GIMP_UNDO_IMAGE_COLORMAP` | Change indexed palette | 87 | `GIMP_UNDO_VECTOR_LAYER` | Vector layer |
| 57 | `GIMP_UNDO_IMAGE_HIDDEN_PROFILE` | Hide/Unhide color profile | 88 | `GIMP_UNDO_LAYER_MASK_ADD` | Add layer masks |
| 58 | `GIMP_UNDO_GUIDE` | Guide | 89 | `GIMP_UNDO_LAYER_MASK_REMOVE` | Delete layer masks |
| 59 | `GIMP_UNDO_SAMPLE_POINT` | Sample Point | 90 | `GIMP_UNDO_LAYER_MASK_APPLY` | Apply layer masks |
| 60 | `GIMP_UNDO_DRAWABLE` | Layer/Channel | 91 | `GIMP_UNDO_LAYER_MASK_SHOW` | Show layer masks |
| 61 | `GIMP_UNDO_DRAWABLE_MOD` | Layer/Channel modification | 92 | `GIMP_UNDO_CHANNEL_ADD` | New channel |
| 62 | `GIMP_UNDO_DRAWABLE_FORMAT` | Layer/Channel format | 93 | `GIMP_UNDO_CHANNEL_REMOVE` | Delete channel |
| 63 | `GIMP_UNDO_MASK` | Selection mask | 94 | `GIMP_UNDO_CHANNEL_COLOR` | Channel color |
| 64 | `GIMP_UNDO_ITEM_REORDER` | Reorder item | 95 | `GIMP_UNDO_PATH_ADD` | New path |
| 65 | `GIMP_UNDO_ITEM_RENAME` | Rename item | 96 | `GIMP_UNDO_PATH_REMOVE` | Delete path |
| 66 | `GIMP_UNDO_ITEM_DISPLACE` | Move item | 97 | `GIMP_UNDO_PATH_MOD` | Path modification |
| 67 | `GIMP_UNDO_ITEM_VISIBILITY` | Item visibility | 98 | `GIMP_UNDO_FS_TO_LAYER` | Floating selection to layer |
| 68 | `GIMP_UNDO_ITEM_COLOR_TAG` | Item color tag | 99 | `GIMP_UNDO_TRANSFORM_GRID` | Transform grid |
| 69 | `GIMP_UNDO_ITEM_LOCK_CONTENT` | Lock/Unlock content | 100 | `GIMP_UNDO_PAINT` | Paint |
| 70 | `GIMP_UNDO_ITEM_LOCK_POSITION` | Lock/Unlock position | 101 | `GIMP_UNDO_INK` | Ink |
| 71 | `GIMP_UNDO_ITEM_LOCK_VISIBILITY` | Lock/Unlock visibility | 102 | `GIMP_UNDO_FOREGROUND_SELECT` | Select foreground |
| 72 | `GIMP_UNDO_LAYER_ADD` | New layer | 103 | `GIMP_UNDO_PARASITE_ATTACH` | Attach parasite |
| 73 | `GIMP_UNDO_LAYER_REMOVE` | Delete layer | 104 | `GIMP_UNDO_PARASITE_REMOVE` | Remove parasite |
| 74 | `GIMP_UNDO_LAYER_MODE` | Set layer mode | 105 | `GIMP_UNDO_FILTER_ADD` | Add effect |
| 75 | `GIMP_UNDO_LAYER_OPACITY` | Set layer opacity | 106 | `GIMP_UNDO_FILTER_REMOVE` | Remove effect |
| 76 | `GIMP_UNDO_LAYER_LOCK_ALPHA` | Lock/Unlock alpha channel | 107 | `GIMP_UNDO_FILTER_REORDER` | Reorder effect |
| 77 | `GIMP_UNDO_LINK_LAYER` | Link layer | 108 | `GIMP_UNDO_FILTER_MODIFIED` | Effect modification |
| 78 | `GIMP_UNDO_GROUP_LAYER_SUSPEND_RESIZE` | Suspend group layer resize | 109 | `GIMP_UNDO_RASTERIZABLE` | Text, link or vector layer |
| 79 | `GIMP_UNDO_GROUP_LAYER_RESUME_RESIZE` | Resume group layer resize | 110 | `GIMP_UNDO_CANT` | Not undoable |
| 80 | `GIMP_UNDO_GROUP_LAYER_SUSPEND_MASK` | Suspend group layer mask |  |  |  |

### 1.8 Floating selection (`gimplayer-floating-selection.c`, `gimpselection.c`)

- A floating selection is an ordinary `GimpLayer` with `fs.drawable` pointing at the layer/channel/mask it floats over; `image->floating_sel` points at it. It is **not** in the layer stack: it is composited inside its host drawable's source graph by a special filter (`_gimp_drawable_add_floating_sel_filter`: `fs_filter`, `fs_crop_node`, `fs_applicator`) using the FS layer's own mode/opacity, clipped to the host drawable (and to the selection).
- API: `floating_sel_attach(layer, drawable)`, `floating_sel_anchor(layer)` (merges pixels into the host via the applicator, undo group `GIMP_UNDO_GROUP_FS_ANCHOR`, removes FS), `floating_sel_to_layer(layer)` (turns it into a regular layer; refuses when floating over a channel/mask), `floating_sel_activate_drawable`, `floating_sel_boundary`, `floating_sel_invalidate`.
- `gimp_selection_float(selection, drawables, context, cut_image, off_x, off_y)` = extract selected pixels (`gimp_selection_extract`: copies/cuts pixels under the selection, adds alpha, multiplies by the mask) → new layer "Floated Layer" → `floating_sel_attach`, clears the selection.
- While an FS exists most image operations are blocked or first anchor it. In XCF: `PROP_FLOATING_SELECTION` on the FS layer carries the file offset of the host drawable.

### 1.9 Core operations and where they live

| User operation | Core entry point(s) | File | Notes |
|---|---|---|---|
| Cut / Copy / Copy visible | `gimp_edit_cut`, `gimp_edit_copy`, `gimp_edit_copy_visible`, named variants `gimp_edit_named_cut/copy/copy_visible` | `app/core/gimp-edit.c` | result is a `GimpBuffer` (or a whole `GimpImage` for multi-layer copy) on the global clipboard |
| Paste | `gimp_edit_paste(image, drawables, paste, GimpPasteType, context, merged, viewport…)`, `gimp_edit_paste_as_new_image` | `gimp-edit.c` | `GimpPasteType`: `FLOATING, FLOATING_IN_PLACE, FLOATING_INTO, FLOATING_INTO_IN_PLACE, NEW_LAYER, NEW_LAYER_IN_PLACE, NEW_LAYER_OR_FLOATING(_IN_PLACE), NEW_MERGED_LAYER_OR_FLOATING(_IN_PLACE)` |
| Clear / Fill | `gimp_drawable_edit_clear`, `gimp_drawable_edit_fill(drawable, GimpFillOptions, undo_desc)` | `gimpdrawable-edit.c` | fill respects selection + lock-alpha; `GimpFillType`: `FOREGROUND 0, BACKGROUND, CIELAB_MIDDLE_GRAY, WHITE, TRANSPARENT, PATTERN 5` |
| Whole-drawable fill | `gimp_drawable_fill`, `gimp_drawable_fill_buffer`, `gimp_drawable_fill_boundary`, `gimp_drawable_fill_path`, `gimp_drawable_fill_scan_convert` | `gimpdrawable-fill.c` | |
| Stroke selection / path | `gimp_item_stroke`, `gimp_drawable_stroke_boundary`, `gimp_drawable_stroke_path`, `gimp_drawable_stroke_scan_convert` | `gimpdrawable-stroke.c`, `gimpstrokeoptions.c`, `gimpscanconvert.c` | §4.8 |
| Bucket fill | `gimp_drawable_bucket_fill`, `gimp_drawable_get_bucket_fill_buffer`, `gimp_drawable_get_line_art_fill_buffer` | `gimpdrawable-bucket-fill.c`, `gimplineart.c` | §4.6 |
| Gradient | `gimp_drawable_gradient`, `gimp_drawable_gradient_shapeburst_distmap`, `gimp_drawable_gradient_adjust_coords` | `gimpdrawable-gradient.c`, `app/operations/gimpoperationgradient.c` | §4.7 |
| Offset / wrap | `gimp_drawable_offset` (`GimpOffsetType`: `COLOR, TRANSPARENT, WRAP_AROUND`) | `gimpdrawable-offset.c` | |
| Scale image | `gimp_image_scale(image, new_w, new_h, GimpInterpolationType, progress)`, `gimp_image_scale_check` | `gimpimage-scale.c` | scales every item via `gimp_item_scale`, guides, sample points |
| Canvas size | `gimp_image_resize`, `gimp_image_resize_with_layers(…, GimpItemSet layer_set, resize_text_layers)`, `gimp_image_resize_to_layers`, `gimp_image_resize_to_selection` | `gimpimage-resize.c` | `GimpItemSet`: `NONE, ALL, IMAGE_SIZED, VISIBLE` |
| Crop | `gimp_image_crop(image, context, GimpFillType, x, y, w, h, crop_layers)` | `gimpimage-crop.c` | |
| Rotate 90/180/270 | `gimp_image_rotate(image, context, GimpRotationType)` (`DEGREES90 0, 180 1, 270 2`) | `gimpimage-rotate.c` | |
| Flip | `gimp_image_flip`, `gimp_image_flip_full` (`GimpOrientationType`) | `gimpimage-flip.c` | |
| Arbitrary transform | `gimp_image_transform` | `gimpimage-transform.c` | |
| Merge visible / flatten / merge down / merge group | `gimp_image_merge_visible_layers(image, context, GimpMergeType, merge_active_group, discard_invisible, progress)`, `gimp_image_flatten`, `gimp_image_merge_down`, `gimp_image_merge_group_layer`, `gimp_image_merge_visible_paths` | `gimpimage-merge.c` | `GimpMergeType`: `EXPAND_AS_NECESSARY 0, CLIP_TO_IMAGE 1, CLIP_TO_BOTTOM_LAYER 2, FLATTEN_IMAGE 3`. Implementation: builds the same GEGL graph as the projection for the chosen layers and renders it into a new layer; flatten composites over the background colour with a `gimp_gegl_create_flatten_node(bg, composite_space of bottom layer)` (NORMAL mode over `gegl:color`) and removes alpha. Merged layer inherits bottom layer's name, tattoo, parasites. |
| Convert base type | `gimp_image_convert_type(image, GimpImageBaseType, profile, progress, error)`; indexed: `gimp_image_convert_indexed(image, GimpConvertPaletteType, max_colors, remove_duplicates, GimpConvertDitherType, dither_alpha, dither_text_layers, custom_palette…)` | `gimpimage-convert-type.c`, `gimpimage-convert-indexed.c` | palette types `GENERATE, WEB, MONO, CUSTOM`; dither `NONE, FS, FS_LOWBLEED, FIXED` |
| Convert precision | `gimp_image_convert_precision(image, GimpPrecision, layer_dither, text_layer_dither, mask_dither, progress)`, `gimp_image_convert_dither_u8` | `gimpimage-convert-precision.c` | converts every drawable buffer with babl; optional `gegl:dither` |
| New layer | `gimp_layer_new(image, w, h, format, name, opacity, mode)`, `gimp_layer_new_from_buffer/_from_gegl_buffer/_from_pixbuf` | `gimplayer-new.c` | |
| Duplicate / convert between images | `gimp_item_duplicate`, `gimp_item_convert` | `gimpitem.c` | |
| Layer scale / resize / to image size | `gimp_item_scale`, `gimp_item_scale_by_factors`, `gimp_item_scale_by_origin`, `gimp_item_resize`, `gimp_item_resize_to_image`, `gimp_layer_resize_to_image` | `gimpitem.c`, `gimplayer.c` | layer `scale` uses `gimp_gegl_apply_scale` (`gegl:scale-ratio`, sampler = interpolation); resize = new buffer + copy + fill |
| Flip / rotate / transform layer | `gimp_item_flip`, `gimp_item_rotate`, `gimp_item_transform`; `gimp_drawable_transform_affine/_flip/_rotate`, `gimp_drawable_transform_buffer_affine/_flip/_rotate`, `_cut`, `_paste` | `gimpitem.c`, `gimpdrawable-transform.c` | §5 |
| Alpha add / remove | `gimp_layer_add_alpha`, `gimp_layer_remove_alpha(layer, context)` (flattens against BG colour) | `gimplayer.c` | |
| Alpha to selection | `gimp_item_to_selection(item, GimpChannelOps, antialias, feather, rx, ry)` → `gimp_channel_select_alpha` | `gimpchannel-select.c` | |
| Mask ops | `gimp_layer_create_mask(layer, GimpAddMaskType, channel)`, `gimp_layer_add_mask`, `gimp_layer_apply_mask(layer, GimpMaskApplyMode, push_undo)`, `gimp_layer_set_apply_mask/_edit_mask/_show_mask` | `gimplayer.c` | applying = multiply layer alpha by mask (`gimp_gegl_apply_opacity` with the mask), then remove |
| Lock alpha | `gimp_layer_set_lock_alpha`, `gimp_layer_can_lock_alpha`, `gimp_layer_is_alpha_locked` | `gimplayer.c` | |
| Auto-crop layer / image | `gimp_pickable_auto_shrink(pickable, x, y, w, h, &sx, &sy, &sw, &sh)` then `gimp_item_resize` / `gimp_image_crop` | `gimppickable-auto-shrink.c` | finds bounding box of content different from the border colour / transparency |
| Quick mask | `gimp_image_set_quick_mask_state`, `_quick_mask_invert`, `_set_quick_mask_color` | `gimpimage-quick-mask.c` | toggling converts selection ↔ channel "Qmask" |
| Foreground extract | `gimp_drawable_foreground_extract(drawable, GimpMattingEngine, global_iterations, levin_levels, levin_active_levels, trimap, progress)` | `gimpdrawable-foreground-extract.c` | §4.5 |

---

## 2. Layer modes — complete and exact

Files: `app/operations/operations-enums.h` (enums), `app/operations/layer-modes/gimp-layer-modes.c` (the info table `layer_mode_infos[]`, menus, `gimp_layer_mode_get_format`), `gimpoperationlayermode.c` (generic op `gimp:layer-mode`, colour-space conversion, dispatch), `gimpoperationlayermode-blend.c` (blend functions), `gimpoperationlayermode-composite.c` (composite functions; `-composite-sse2.c` is an SSE2 version of clip-to-backdrop with identical math), `gimpoperation{normal,dissolve,behind,replace,erase,merge,split,passthrough,antierase,overwrite}.c` (special ops; `gimpoperationnormal-sse2.c/-sse4.c` are vectorised copies), `app/operations/layer-modes-legacy/*.c`.

### 2.1 The mode table

`GimpLayerMode` values are sequential from 0 (`gimp_layer_modes_init()` asserts `layer_mode_infos[i].layer_mode == i`); `GIMP_LAYER_MODE_SEPARATOR = -1` is a menu separator only. **These integers are the on-disk values of XCF `PROP_MODE`.**

Flag legend (`GimpLayerModeFlags`, `operations-enums.h:186-192`): `L` = `FLAG_LEGACY (1<<0)`, `BI` = `FLAG_BLEND_SPACE_IMMUTABLE (1<<1)`, `CSI` = `FLAG_COMPOSITE_SPACE_IMMUTABLE (1<<2)`, `CMI` = `FLAG_COMPOSITE_MODE_IMMUTABLE (1<<3)`, `SUB` = `FLAG_SUBTRACTIVE (1<<4)`, `AO` = `FLAG_ALPHA_ONLY (1<<5)`, `T` = `FLAG_TRIVIAL (1<<6)`.
Context (`GimpLayerModeContext`): `LAYER 1, GROUP 2, PAINT 4, FILTER 8`, `ALL` = 15. Spaces: `lin` = `RGB_LINEAR`, `nl` = `RGB_NON_LINEAR`, `perc` = `RGB_PERCEPTUAL`, `lab` = `LAB`, `auto` = `AUTO` (field not set in the table → 0; for blend space this means "blend in the composite space, no conversion"). Composite modes: `U` = UNION, `CB` = CLIP_TO_BACKDROP. "Blend fn" is `gimp_operation_layer_mode_blend_<name>`; "—" means the op has its own `process`.
Menu group: D = `layer_mode_group_default[]`, Lg = `layer_mode_group_legacy[]`, "—" = in no menu.

| # | Enum (`GIMP_LAYER_MODE_…`) | UI name | Grp | GEGL op | Blend fn | Flags | Context | Blend space | Composite space | Composite mode (layer) | Paint composite mode |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `NORMAL_LEGACY` | Normal (legacy) | Lg | `gimp:normal` | — | L BI CSI CMI T | ALL | auto | nl | U | U |
| 1 | `DISSOLVE` | Dissolve | D+Lg | `gimp:dissolve` | — | BI CSI T | ALL | auto | auto | U | U |
| 2 | `BEHIND_LEGACY` | Behind (legacy) | Lg | `gimp:behind` | — | L BI CSI CMI | PAINT\|FILTER | auto | nl | U | U |
| 3 | `MULTIPLY_LEGACY` | Multiply (legacy) | Lg | `gimp:multiply-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 4 | `SCREEN_LEGACY` | Screen (legacy) | Lg | `gimp:screen-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 5 | `OVERLAY_LEGACY` | Old broken Overlay | — | `gimp:softlight-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 6 | `DIFFERENCE_LEGACY` | Difference (legacy) | Lg | `gimp:difference-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 7 | `ADDITION_LEGACY` | Addition (legacy) | Lg | `gimp:addition-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 8 | `SUBTRACT_LEGACY` | Subtract (legacy) | Lg | `gimp:subtract-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 9 | `DARKEN_ONLY_LEGACY` | Darken only (legacy) | Lg | `gimp:darken-only-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 10 | `LIGHTEN_ONLY_LEGACY` | Lighten only (legacy) | Lg | `gimp:lighten-only-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 11 | `HSV_HUE_LEGACY` | HSV Hue (legacy) | Lg | `gimp:hsv-hue-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 12 | `HSV_SATURATION_LEGACY` | HSV Saturation (legacy) | Lg | `gimp:hsv-saturation-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 13 | `HSL_COLOR_LEGACY` | HSL Color (legacy) | Lg | `gimp:hsl-color-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 14 | `HSV_VALUE_LEGACY` | HSV Value (legacy) | Lg | `gimp:hsv-value-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 15 | `DIVIDE_LEGACY` | Divide (legacy) | Lg | `gimp:divide-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 16 | `DODGE_LEGACY` | Dodge (legacy) | Lg | `gimp:dodge-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 17 | `BURN_LEGACY` | Burn (legacy) | Lg | `gimp:burn-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 18 | `HARDLIGHT_LEGACY` | Hard light (legacy) | Lg | `gimp:hardlight-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 19 | `SOFTLIGHT_LEGACY` | Soft light (legacy) | Lg | `gimp:softlight-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 20 | `GRAIN_EXTRACT_LEGACY` | Grain extract (legacy) | Lg | `gimp:grain-extract-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 21 | `GRAIN_MERGE_LEGACY` | Grain merge (legacy) | Lg | `gimp:grain-merge-legacy` | — | L BI CSI CMI | ALL | nl | nl | CB | CB |
| 22 | `COLOR_ERASE_LEGACY` | Color erase (legacy) | Lg | `gimp:layer-mode` | `color_erase` | L BI CSI CMI SUB | PAINT\|FILTER | nl | nl | CB | CB |
| 23 | `OVERLAY` | Overlay | D+Lg | `gimp:layer-mode` | `overlay` | — | ALL | perc | lin | CB | U |
| 24 | `LCH_HUE` | LCh Hue | D | `gimp:layer-mode` | `lch_hue` | BI | ALL | lab | lin | CB | U |
| 25 | `LCH_CHROMA` | LCh Chroma | D | `gimp:layer-mode` | `lch_chroma` | BI | ALL | lab | lin | CB | U |
| 26 | `LCH_COLOR` | LCh Color | D | `gimp:layer-mode` | `lch_color` | BI | ALL | lab | lin | CB | U |
| 27 | `LCH_LIGHTNESS` | LCh Lightness | D | `gimp:layer-mode` | `lch_lightness` | BI | ALL | lab | lin | CB | U |
| 28 | `NORMAL` | Normal | D | `gimp:normal` | — | BI T | ALL | auto | lin | U | U |
| 29 | `BEHIND` | Behind | D | `gimp:behind` | — | BI | PAINT\|FILTER | auto | lin | U | U |
| 30 | `MULTIPLY` | Multiply | D | `gimp:layer-mode` | `multiply` | — | ALL | lin | lin | CB | U |
| 31 | `SCREEN` | Screen | D | `gimp:layer-mode` | `screen` | — | ALL | perc | lin | CB | U |
| 32 | `DIFFERENCE` | Difference | D | `gimp:layer-mode` | `difference` | — | ALL | perc | lin | CB | U |
| 33 | `ADDITION` | Addition | D | `gimp:layer-mode` | `addition` | — | ALL | lin | lin | CB | U |
| 34 | `SUBTRACT` | Subtract | D | `gimp:layer-mode` | `subtract` | — | ALL | lin | lin | CB | U |
| 35 | `DARKEN_ONLY` | Darken only | D | `gimp:layer-mode` | `darken_only` | BI | ALL | auto | lin | CB | U |
| 36 | `LIGHTEN_ONLY` | Lighten only | D | `gimp:layer-mode` | `lighten_only` | BI | ALL | auto | lin | CB | U |
| 37 | `HSV_HUE` | HSV Hue | D | `gimp:layer-mode` | `hsv_hue` | BI | ALL | nl | lin | CB | U |
| 38 | `HSV_SATURATION` | HSV Saturation | D | `gimp:layer-mode` | `hsv_saturation` | BI | ALL | nl | lin | CB | U |
| 39 | `HSL_COLOR` | HSL Color | D | `gimp:layer-mode` | `hsl_color` | BI | ALL | nl | lin | CB | U |
| 40 | `HSV_VALUE` | HSV Value | D | `gimp:layer-mode` | `hsv_value` | BI | ALL | nl | lin | CB | U |
| 41 | `DIVIDE` | Divide | D | `gimp:layer-mode` | `divide` | — | ALL | lin | lin | CB | U |
| 42 | `DODGE` | Dodge | D | `gimp:layer-mode` | `dodge` | — | ALL | perc | lin | CB | U |
| 43 | `BURN` | Burn | D | `gimp:layer-mode` | `burn` | — | ALL | perc | lin | CB | U |
| 44 | `HARDLIGHT` | Hard light | D | `gimp:layer-mode` | `hardlight` | — | ALL | perc | lin | CB | U |
| 45 | `SOFTLIGHT` | Soft light | D | `gimp:layer-mode` | `softlight` | — | ALL | perc | lin | CB | U |
| 46 | `GRAIN_EXTRACT` | Grain extract | D | `gimp:layer-mode` | `grain_extract` | — | ALL | perc | lin | CB | U |
| 47 | `GRAIN_MERGE` | Grain merge | D | `gimp:layer-mode` | `grain_merge` | — | ALL | perc | lin | CB | U |
| 48 | `VIVID_LIGHT` | Vivid light | D | `gimp:layer-mode` | `vivid_light` | — | ALL | perc | lin | CB | U |
| 49 | `PIN_LIGHT` | Pin light | D | `gimp:layer-mode` | `pin_light` | — | ALL | perc | lin | CB | U |
| 50 | `LINEAR_LIGHT` | Linear light | D | `gimp:layer-mode` | `linear_light` | — | ALL | perc | lin | CB | U |
| 51 | `HARD_MIX` | Hard mix | D | `gimp:layer-mode` | `hard_mix` | — | ALL | perc | lin | CB | U |
| 52 | `EXCLUSION` | Exclusion | D | `gimp:layer-mode` | `exclusion` | — | ALL | perc | lin | CB | U |
| 53 | `LINEAR_BURN` | Linear burn | D | `gimp:layer-mode` | `linear_burn` | — | ALL | perc | lin | CB | U |
| 54 | `LUMA_DARKEN_ONLY` | Luma/Luminance darken only | D | `gimp:layer-mode` | `luma_darken_only` | — | ALL | lin | lin | CB | U |
| 55 | `LUMA_LIGHTEN_ONLY` | Luma/Luminance lighten only | D | `gimp:layer-mode` | `luma_lighten_only` | — | ALL | lin | lin | CB | U |
| 56 | `LUMINANCE` | Luminance | D | `gimp:layer-mode` | `luminance` | BI | ALL | lin | lin | CB | U |
| 57 | `COLOR_ERASE` | Color erase | D | `gimp:layer-mode` | `color_erase` | SUB | ALL | lin | lin | CB | CB |
| 58 | `ERASE` | Erase | D | `gimp:erase` | — | BI SUB AO T | ALL | auto | lin | CB | CB |
| 59 | `MERGE` | Merge | D | `gimp:merge` | — | BI T | ALL | auto | lin | U | U |
| 60 | `SPLIT` | Split | D | `gimp:split` | — | BI CSI SUB AO T | ALL | auto | auto | CB | CB |
| 61 | `PASS_THROUGH` | Pass through | D | `gimp:pass-through` | — | BI CMI T | GROUP | auto | lin | U | (unset → AUTO) |
| 62 | `REPLACE` | Replace | D | `gimp:replace` | — | BI T | FILTER | auto | lin | U | U |
| 63 | `OVERWRITE` | Overwrite | D | `gimp:overwrite` | — | BI CSI CMI T | PAINT | auto | lin | U | U |
| 64 | `ANTI_ERASE` | Anti erase (pdb-skip) | D | `gimp:anti-erase` | — | BI CSI AO | (none set) | auto | auto | U | U |

Notes:
- Menus are filtered per context with `gimp_layer_mode_get_context_array()`: a mode appears only where its context bit matches (e.g. BEHIND/COLOR_ERASE_LEGACY only for paint tools and filters, PASS_THROUGH only for groups, REPLACE only for filters, OVERWRITE only for paint).
- `gimp_layer_mode_get_group()` returns DEFAULT if the mode is in the default array, else LEGACY if in the legacy array, else DEFAULT. Default↔legacy twins are in `layer_mode_groups[][2]` (e.g. NORMAL↔NORMAL_LEGACY … COLOR_ERASE↔COLOR_ERASE_LEGACY; `-1` = no twin).
- `OVERLAY_LEGACY` (5) is mapped to `SOFTLIGHT_LEGACY` (19) by both XCF loader and saver (`xcf-load.c` PROP_MODE, `xcf-save.c` PROP_MODE).
- "Auto composite space" → `gimp_layer_mode_get_format()` picks `RGBA float` if the preferred (input) format is linear, else `R'G'B'A float`. Also: for alpha-only modes in non-UNION composite modes, and for trivial modes whose composite region excludes the destination, the composite space is forced to AUTO (no colours are mixed, so no conversion is needed).
- Buffer formats per composite space (`gimp_layer_mode_get_format`): lin → `"RGBA float"`, nl → `"R'G'B'A float"`, perc → `"R~G~B~A float"`, lab → `"CIE Lab alpha float"`; the mask pad is always `"Y float"`. All with the image's babl space.
- Included region per composite mode (`gimp_layer_mode_get_included_region`): UNION → SOURCE|DESTINATION, CLIP_TO_BACKDROP → DESTINATION, CLIP_TO_LAYER → SOURCE, INTERSECTION → neither. Bounding box of the result = (src ∩ dst) ∪ (src if SOURCE included and opacity ≠ 0) ∪ (dst if DESTINATION included).

### 2.2 Generic pipeline of `gimp:layer-mode` (`gimp_operation_layer_mode_real_process`)

```
inputs: in[4n], layer[4n] in composite-space float format; mask[n] or NULL; opacity (float)
1. if blend_space != AUTO and composite_space != AUTO and they differ:
      scan samples; a sample is "blended" iff in.a != 0 && layer.a != 0.
      runs of blended samples (split when > GIMP_COMPOSITE_BLEND_SPLIT_THRESHOLD = 32 consecutive
      unblended samples) are converted composite→blend space with babl (both in and layer),
      passed to blend_function(in_b, layer_b, comp, count), and comp is converted back blend→composite.
      For every unblended sample comp.a is set to 0 (comp.rgb undefined).
   else:
      blend_function(in, layer, comp, n) directly (comp.rgb is left untouched/undefined where
      in.a == 0 || layer.a == 0; comp.a = layer.a for non-subtractive functions).
2. composite: non-subtractive modes use composite_{union,clip_to_backdrop,clip_to_layer,intersection};
   modes with FLAG_SUBTRACTIVE use the *_sub variants. AUTO composite mode behaves as UNION here
   (but AUTO is normally resolved to the table default before the node is configured).
```
Chunking: at most `GIMP_COMPOSITE_BLEND_MAX_SAMPLES = (1<<18)/16/2 = 8192` samples per call (stack buffers). In-place operation (`in == out`) is supported by blending into a temporary.

Alpha is never colour-converted; babl leaves the alpha component untouched in all four formats.

### 2.3 Blend functions (`gimpoperationlayermode-blend.c`)

Common frame for every *non-subtractive* function:
```c
if (in[ALPHA] != 0.0f && layer[ALPHA] != 0.0f) { comp[0..2] = f(in, layer); }
comp[ALPHA] = layer[ALPHA];
```
Constants: `EPSILON 1e-6f`, `SAFE_DIV_MIN = EPSILON`, `SAFE_DIV_MAX = 1/EPSILON = 1e6`.
`safe_div(a,b)`: `if (fabsf(a) > 1e-6f) { r = a / b; r = CLAMP(r, -1e6f, 1e6f); } else r = 0;` (so `x/0` → ±1e6, tiny numerators → 0).
`i` = `in[c]` (backdrop), `l` = `layer[c]` (source), per channel c ∈ {0,1,2} unless stated. **No function clamps its result to [0,1] unless shown** — out-of-range values flow through (important for float images).

| Function | Formula for `comp[c]` |
|---|---|
| `addition` ("aka linear_dodge") | `i + l` |
| `burn` | `1 - safe_div(1 - i, l)` |
| `darken_only` | `MIN(i, l)` |
| `difference` | `fabsf(i - l)` |
| `divide` | `safe_div(i, l)` |
| `dodge` | `safe_div(i, 1 - l)` |
| `exclusion` | `0.5 - 2*(i - 0.5)*(l - 0.5)` |
| `grain_extract` | `i - l + 0.5` |
| `grain_merge` | `i + l - 0.5` |
| `hard_mix` | `(i + l < 1.0) ? 0.0 : 1.0` |
| `hardlight` | `if (l > 0.5) { v = (1 - i) * (1 - (l - 0.5)*2); v = MIN(1 - v, 1); } else { v = i * (l*2); v = MIN(v, 1); }` |
| `lighten_only` | `MAX(i, l)` |
| `linear_burn` | `i + l - 1` |
| `linear_light` | `(l <= 0.5) ? i + 2*l - 1 : i + 2*(l - 0.5)` |
| `multiply` | `i * l` |
| `overlay` | `(i < 0.5) ? 2*i*l : 1 - 2*(1 - l)*(1 - i)` |
| `pin_light` | `(l > 0.5) ? MAX(i, 2*(l - 0.5)) : MIN(i, 2*l)` |
| `screen` | `1 - (1 - i)*(1 - l)` |
| `softlight` | `m = i*l; s = 1 - (1-i)*(1-l); (1 - i)*m + i*s` |
| `subtract` | `i - l` |
| `vivid_light` | `if (l <= 0.5) { v = 1 - safe_div(1 - i, 2*l); v = MAX(v, 0); } else { v = safe_div(i, 2*(1 - l)); v = MIN(v, 1); }` |

Whole-pixel functions (operate on the RGB triple; `min/max` over the 3 channels):

- **`hsl_color`** (blend space nl): `dest_l = (min(in)+max(in))/2; src_l = (min(layer)+max(layer))/2;`
  `if (fabs(src_l) > EPSILON && fabs(1 - src_l) > EPSILON) { dest_high = dest_l > 0.5; src_high = src_l > 0.5; dest_l = MIN(dest_l, 1 - dest_l); src_l = MIN(src_l, 1 - src_l); ratio = dest_l / src_l; offset = 0; if (dest_high) offset += 1 - 2*dest_l; if (src_high) offset += 2*dest_l - ratio; comp[c] = layer[c]*ratio + offset; } else comp = (dest_l, dest_l, dest_l);`
- **`hsv_hue`** (nl): `src_delta = max(layer) - min(layer); if (src_delta > EPSILON) { dest_max = max(in); dest_delta = dest_max - min(in); dest_s = dest_max ? dest_delta / dest_max : 0; ratio = dest_s * dest_max / src_delta; offset = dest_max - max(layer) * ratio; comp[c] = layer[c]*ratio + offset; } else comp[c] = in[c];`
- **`hsv_saturation`** (nl): `dest_max = max(in); dest_delta = dest_max - min(in); if (dest_delta > EPSILON) { src_max = max(layer); src_s = src_max ? (src_max - min(layer)) / src_max : 0; ratio = src_s * dest_max / dest_delta; offset = (1 - ratio) * dest_max; comp[c] = in[c]*ratio + offset; } else comp = (dest_max, dest_max, dest_max);`
- **`hsv_value`** (nl): `dest_v = max(in); src_v = max(layer); if (fabs(dest_v) > EPSILON) comp[c] = in[c] * (src_v / dest_v); else comp = (src_v, src_v, src_v);`
- **`lch_chroma`** (blend space CIE Lab; components L,a,b): `c1 = hypotf(in.a*, in.b*); if (c1 > EPSILON) { c2 = hypotf(layer.a*, layer.b*); comp = (in.L, c2*in.a*/c1, c2*in.b*/c1); } else comp = in;`
- **`lch_color`**: `comp = (in.L, layer.a*, layer.b*)`.
- **`lch_hue`**: `c2 = hypotf(layer.a*, layer.b*); if (c2 > EPSILON) { c1 = hypotf(in.a*, in.b*); comp = (in.L, c1*layer.a*/c2, c1*layer.b*/c2); } else comp = in;`
- **`lch_lightness`**: `comp = (layer.L, in.a*, in.b*)`.
- **`luma_darken_only`** (lin): `Yd = Σ in[c]*lum[c]; Ys = Σ layer[c]*lum[c]` with `lum = babl_space_get_rgb_luminance(space)` (for sRGB measured as **(0.2224879, 0.7169038, 0.0606083)** — the D50-adapted sRGB Y row, *not* Rec.709's 0.2126/0.7152/0.0722); `comp = (Yd <= Ys) ? in : layer`.
- **`luma_lighten_only`**: same, `comp = (Yd >= Ys) ? in : layer`.
- **`luminance`** (lin): `in_Y`, `layer_Y` = babl `"RGBA float" → "Y float"` (same luminance weights); `ratio = safe_div(layer_Y, in_Y); comp[c] = in[c] * ratio`.

Subtractive function (sets `comp[ALPHA]` to the **fraction of the overlapping content that is kept**):

- **`color_erase`**: `color = in`, `bgcolor = layer` (the colour to erase):
```c
if (in[ALPHA] != 0 && layer[ALPHA] != 0) {
  alpha = 0;
  for c in 0..2 { col = CLAMP(color[c],0,1); bgcol = CLAMP(bgcolor[c],0,1);
     if (fabs(col - bgcol) > EPSILON) {
        a = (col > bgcol) ? (col - bgcol) / (1 - bgcol) : (bgcol - col) / bgcol;
        alpha = MAX(alpha, a); } }
  if (alpha > EPSILON) { alpha_inv = 1/alpha; comp[c] = (color[c] - bgcolor[c]) * alpha_inv + bgcolor[c]; }
  else comp[0..2] = 0;
  comp[ALPHA] = alpha;
} else comp[ALPHA] = 0;
```

### 2.4 Composite functions (`gimpoperationlayermode-composite.c`)

`m` = `mask ? *mask : 1`. All comparisons are exact float `== 0.0f`.

**Non-subtractive** (expects `comp.a == layer.a`):

| Mode | Code |
|---|---|
| UNION | `la = layer.a * opacity * m; na = la + (1 - la) * in.a;`<br>`if (la == 0 \|\| na == 0) out.rgb = in.rgb;`<br>`else if (in.a == 0) out.rgb = layer.rgb;`<br>`else { ratio = la / na; out[c] = ratio * (in.a * (comp[c] - layer[c]) + layer[c] - in[c]) + in[c]; }`<br>`out.a = na;` |
| CLIP_TO_BACKDROP | `la = comp.a * opacity * m;`<br>`if (in.a == 0 \|\| la == 0) out.rgb = in.rgb; else out[c] = comp[c] * la + in[c] * (1 - la);`<br>`out.a = in.a;` |
| CLIP_TO_LAYER | `la = layer.a * opacity * m;`<br>`if (la == 0) out.rgb = in.rgb; else if (in.a == 0) out.rgb = layer.rgb; else out[c] = comp[c] * in.a + layer[c] * (1 - in.a);`<br>`out.a = la;` |
| INTERSECTION | `na = in.a * comp.a * opacity * m;`<br>`out.rgb = (na == 0) ? in.rgb : comp.rgb; out.a = na;` |

UNION derivation check: for the trivial blend `comp = layer` the formula reduces to `in + (layer - in) * la/na`, i.e. ordinary "over" on straight alpha.

**Subtractive** (`comp.a` = kept fraction; used by COLOR_ERASE / COLOR_ERASE_LEGACY):

| Mode | Code |
|---|---|
| UNION | `ia = in.a; la = layer.a * opacity * m; ca = comp.a; na = ia + la - (2 - ca) * ia * la;`<br>`if (la == 0 \|\| na == 0) out.rgb = in.rgb; else if (ia == 0) out.rgb = layer.rgb;`<br>`else { ratio = ia / na; layer_coeff = 1/ia - 1; out[c] = ratio * (la * (ca * comp[c] + layer_coeff * layer[c] - in[c]) + in[c]); }`<br>`out.a = na;` |
| CLIP_TO_BACKDROP | `la = layer.a * opacity * m; ca = comp.a * la; na = 1 - la + ca;`<br>`if (in.a == 0 \|\| ca == 0) out.rgb = in.rgb; else { ratio = ca / na; out[c] = comp[c] * ratio + in[c] * (1 - ratio); }`<br>`out.a = na * in.a;` |
| CLIP_TO_LAYER | `ia = in.a; la = layer.a * opacity * m; ca = comp.a * ia; na = 1 - ia + ca;`<br>`if (la == 0) out.rgb = in.rgb; else if (ia == 0) out.rgb = layer.rgb; else { ratio = ca / na; out[c] = comp[c] * ratio + layer[c] * (1 - ratio); }`<br>`out.a = na * la;` |
| INTERSECTION | `na = in.a * layer.a * comp.a * opacity * m; out.rgb = (na == 0) ? in.rgb : comp.rgb; out.a = na;` |

### 2.5 Special-cased ops (own `process`, no blend/composite split, no colour-space conversion: they work directly in the composite-space buffer)

`la` = `layer.a * opacity * m` unless noted. "truthy" tests are C `if (x)` on a float (non-zero).

**`gimp:normal`** (NORMAL, NORMAL_LEGACY) — `gimpoperationnormal.c`:

| Composite mode | Code |
|---|---|
| UNION / AUTO | `out.a = la + in.a - la * in.a; if (out.a) { lw = la / out.a; iw = 1 - lw; out[c] = layer[c]*lw + in[c]*iw; } else out.rgb = in.rgb;` |
| CLIP_TO_BACKDROP | `out.a = in.a; if (out.a) out[c] = in[c] + (layer[c] - in[c]) * la; else out.rgb = in.rgb;` |
| CLIP_TO_LAYER | `out.a = la; out.rgb = out.a ? layer.rgb : in.rgb;` |
| INTERSECTION | `out.a = in.a * la; out.rgb = out.a ? layer.rgb : in.rgb;` |

**`gimp:dissolve`** — `gimpoperationdissolve.c` (affected region = SOURCE). `RANDOM_TABLE_SIZE 4096`; at class init `gr = g_rand_new_with_seed(314159265); random_table[i] = g_rand_int(gr)` for i in 0..4095. Per ROI row `y`: `gr = g_rand_new_with_seed(random_table[((y % 4096) + 4096) % 4096])`; then `g_rand_int(gr)` is called once for every `x` in `[0, roi.x)` (fast-forward; nothing is skipped when `roi.x <= 0`); then for each pixel:
```c
value = layer.a * opacity * 255; if (mask) value *= *mask;
if (g_rand_int_range(gr, 0, 255) >= value) {      // keep backdrop
   out.rgb = in.rgb; out.a = (mode == UNION || mode == CLIP_TO_BACKDROP) ? in.a : 0;
} else {                                          // take layer, fully opaque
   out.rgb = layer.rgb; out.a = (mode == UNION || mode == CLIP_TO_LAYER) ? 1 : in.a;
}
```
The pattern is anchored to absolute image coordinates. Bit-exact reproduction needs GLib's `GRand` (MT19937, `g_rand_int_range` rejection sampling) — GLib is not in the pinned sources; treat exactness as "needs a test against real GLib".

**`gimp:behind`** (BEHIND, BEHIND_LEGACY) — `gimpoperationbehind.c`; `s1 = in.a`, `s2 = la`:

| Composite mode | Code |
|---|---|
| UNION / AUTO | `na = s2 + (1 - s2) * s1; if (na) { ratio = in.a / na; out[c] = in[c]*ratio + layer[c]*(1 - ratio); } else out.rgb = layer.rgb; out.a = na;` |
| CLIP_TO_BACKDROP | `na = s1; out.rgb = na ? in.rgb : layer.rgb; out.a = na;` (opacity/mask unused) |
| CLIP_TO_LAYER | `na = s2; if (na) out[c] = layer[c] + (in[c] - layer[c]) * s1; else out.rgb = layer.rgb; out.a = na;` |
| INTERSECTION | `na = s1 * s2; out.rgb = na ? in.rgb : layer.rgb; out.a = na;` |

**`gimp:replace`** (REPLACE; `gimp:pass-through` subclasses it and inherits `process`) — `gimpoperationreplace.c`; `ov` = `opacity * m` (note: layer alpha is *not* part of `ov`):

| Composite mode | Code |
|---|---|
| UNION / AUTO | `na = (layer.a - in.a) * ov + in.a; ratio = ov; if (na) ratio *= layer.a / na; out[c] = (layer[c] - in[c]) * ratio + in[c]; out.a = na;` |
| CLIP_TO_BACKDROP | `out.rgb = in.rgb; out.a = in.a * (1 - ov);` |
| CLIP_TO_LAYER | `out.rgb = layer.rgb; out.a = layer.a * ov;` |
| INTERSECTION | `memset(out, 0)` (all four components 0) |

Fast paths in `gimp_operation_replace_parent_process`: opacity == 1, no mask, SOURCE included → output is the `aux` buffer itself; opacity 0 handled by the base class; UNION with `input` and `aux` sharing storage → pass `input`. `get_affected_region` for REPLACE = DESTINATION when `opacity != 0`; PASS_THROUGH resets that vfunc to NULL (ordinary behaviour).

**`gimp:erase`** (ERASE) — `gimpoperationerase.c`:

| Composite mode | Code |
|---|---|
| UNION | `na = in.a + la - 2*in.a*la; if (na != 0) { ratio = (1 - in.a) * la / na; out[c] = ratio*layer[c] + (1 - ratio)*in[c]; } else out.rgb = in.rgb; out.a = na;` |
| CLIP_TO_BACKDROP / AUTO | `out.rgb = in.rgb; out.a = (1 - la) * in.a;` |
| CLIP_TO_LAYER | `na = (1 - in.a) * la; out.rgb = (na == 0) ? in.rgb : layer.rgb; out.a = na;` |
| INTERSECTION | `out.rgb = in.rgb; out.a = 0;` |

**`gimp:merge`** (MERGE) — `gimpoperationmerge.c`:

| Composite mode | Code |
|---|---|
| UNION / AUTO | `ia = MIN(in.a, 1 - la); na = ia + la; if (na) { ratio = la / na; out[c] = in[c] + (layer[c] - in[c]) * ratio; } else out.rgb = in.rgb; out.a = na;` |
| CLIP_TO_BACKDROP | `la2 = la - (1 - in.a); if (la2 > 0) { ratio = la2 / in.a; out[c] = in[c] + (layer[c] - in[c]) * ratio; } else out.rgb = in.rgb; out.a = in.a;` |
| CLIP_TO_LAYER | `out.rgb = (la != 0) ? layer.rgb : in.rgb; out.a = la;` |
| INTERSECTION | `la2 = MAX(la - (1 - in.a), 0); out.rgb = (la2 != 0) ? layer.rgb : in.rgb; out.a = la2;` |

**`gimp:split`** (SPLIT) — `gimpoperationsplit.c`:

| Composite mode | Code |
|---|---|
| UNION | `if (la <= in.a) { na = in.a - la; out.rgb = in.rgb; } else { na = la - in.a; out.rgb = layer.rgb; } out.a = na;` |
| CLIP_TO_BACKDROP / AUTO | `out.rgb = in.rgb; out.a = MAX(in.a - la, 0);` |
| CLIP_TO_LAYER | `na = MAX(la - in.a, 0); out.rgb = (na != 0) ? layer.rgb : in.rgb; out.a = na;` |
| INTERSECTION | `out.rgb = in.rgb; out.a = 0;` |

**`gimp:anti-erase`** (ANTI_ERASE; affected region = SOURCE) — `gimpoperationantierase.c`; `v = opacity * m`:

| Composite mode | Code |
|---|---|
| UNION / AUTO | `out.a = in.a + (1 - in.a) * layer.a * v; out.rgb = in.rgb;` |
| CLIP_TO_BACKDROP | `out = in;` |
| CLIP_TO_LAYER | `out.a = layer.a * v; out.rgb = in.rgb;` |
| INTERSECTION | `out.a = in.a * layer.a * v; out.rgb = in.rgb;` |

**`gimp:overwrite`** (OVERWRITE, paint only) — `gimpoperationoverwrite.c`; only UNION/AUTO is implemented (other modes hit `g_return_val_if_reached(FALSE)`):
```c
mv = mask ? *mask : 1.0; la = layer.a * opacity * mv;
if (la > 0) { out.a = in.a + mv * (la - in.a); out[c] = in[c] + mv * (layer[c] - in[c]); }
else out = in;        // all four components
```
(the mask value is used twice on purpose: once inside `la`, once as the interpolation factor.)

### 2.6 Legacy modes (`app/operations/layer-modes-legacy/`)

All 19 legacy ops share one frame (buffers are `R'G'B'A float`, i.e. in the image's non-linear encoding; composite mode is fixed CLIP_TO_BACKDROP and not consulted):
```c
comp_alpha = MIN(in.a, layer.a) * opacity; if (mask) comp_alpha *= *mask;
new_alpha  = in.a + (1 - in.a) * comp_alpha;
if (comp_alpha && new_alpha) { ratio = comp_alpha / new_alpha;
     out[c] = F(in[c], layer[c]) * ratio + in[c] * (1 - ratio); }
else out.rgb = in.rgb;
out.a = in.a;
```
| Op (file `gimpoperation<x>legacy.c`) | `F(i, l)` (`comp`) |
|---|---|
| `multiply` | `l * i` |
| `screen` | `1 - (1 - i)*(1 - l)` |
| `softlight` (also used for `OVERLAY_LEGACY`) | `m = i*l; s = 1 - (1-i)*(1-l); (1 - i)*m + i*s` |
| `difference` | `c = i - l; (c < 0) ? -c : c` |
| `addition` | `CLAMP(i + l, 0, 1)` |
| `subtract` | `CLAMP(i - l, 0, 1)` |
| `darkenonly` | `MIN(i, l)` |
| `lightenonly` | `MAX(l, i)` |
| `divide` | `SAFE_CLAMP(i / l, 0, 1)` |
| `dodge` | `SAFE_CLAMP(i / (1 - l), 0, 1)` |
| `burn` | `c = 1 - (1 - i) / l; c = c < 0 ? 0 : c < 1 ? c : 1;` (written so that NaN from 0/0 maps to 1) |
| `hardlight` | `if (l > 128/255) { c = (1 - i) * (1 - (l - 128/255) * 2); c = MIN(1 - c, 1); } else { c = i * (l * 2); c = MIN(c, 1); }` |
| `grainextract` | `CLAMP(i - l + 128/255, 0, 1)` |
| `grainmerge` | `CLAMP(i + l - 128/255, 0, 1)` |
| `hsvhue` | convert both to HSV with `gimp_rgb_to_hsv_legacy` (doubles); `if (layer_hsv.s) out_hsv.h = layer_hsv.h;` back with `gimp_hsv_to_rgb_legacy` |
| `hsvsaturation` | `out_hsv.s = layer_hsv.s` |
| `hsvvalue` | `out_hsv.v = layer_hsv.v` |
| `hslcolor` | HSL via `gimp_rgb_to_hsl_legacy`: `out_hsl.h = layer_hsl.h; out_hsl.s = layer_hsl.s;` back with `gimp_hsl_to_rgb_legacy` |

`SAFE_CLAMP(x, low, high)` = `((x) > (low) ? (x) < (high) ? (x) : (high) : (low))` (`libgimpmath/gimpmath.h:131`; NaN → `low`).

Legacy colour conversions (`gimpcolor-legacy.c`; arrays of 4 doubles, h in [0,1)):
```c
rgb→hsv: max,min; v = max; delta = max - min;
   if (delta > 0.0001) { s = delta / max;
        if (r == max) { h = (g - b)/delta; if (h < 0) h += 6; }
        else if (g == max) h = 2 + (b - r)/delta; else h = 4 + (r - g)/delta;
        h /= 6; }
   else { s = 0; h = 0; }
hsv→rgb: if (s == 0) r=g=b=v;
   else { hue = h; if (hue == 1.0) hue = 0; hue *= 6; i = (int) hue; f = hue - i;
          w = v*(1 - s); q = v*(1 - s*f); t = v*(1 - s*(1 - f));
          switch (i) { 0:(v,t,w) 1:(q,v,w) 2:(w,v,t) 3:(w,q,v) 4:(t,w,v) 5:(v,w,q) } }
rgb→hsl: l = (max + min)/2;
   if (max == min) { s = 0; h = -1.0 /* GIMP_HSL_UNDEFINED */; }
   else { s = (l <= 0.5) ? (max-min)/(max+min) : (max-min)/(2 - max - min);
          delta = max - min; if (delta == 0) delta = 1;
          if (r == max) h = (g - b)/delta; else if (g == max) h = 2 + (b - r)/delta; else h = 4 + (r - g)/delta;
          h /= 6; if (h < 0) h += 1; }
hsl→rgb: if (s == 0) r=g=b=l;
   else { m2 = (l <= 0.5) ? l*(1 + s) : l + s - l*s; m1 = 2*l - m2;
          r = hsl_value(m1, m2, h*6 + 2); g = hsl_value(m1, m2, h*6); b = hsl_value(m1, m2, h*6 - 2); }
hsl_value(n1, n2, hue): if (hue > 6) hue -= 6; else if (hue < 0) hue += 6;
   if (hue < 1) n1 + (n2 - n1)*hue; else if (hue < 3) n2; else if (hue < 4) n1 + (n2 - n1)*(4 - hue); else n1;
```
(In the legacy ops the 4th array element is set to 1.0 and ignored.)

### 2.7 Blend / composite colour spaces and conversions

| `GimpLayerColorSpace` | babl format | Meaning | Conversion from linear RGB (sRGB image) |
|---|---|---|---|
| `RGB_LINEAR` (1) | `RGBA float` | linear light in the image's RGB primaries | identity |
| `RGB_NON_LINEAR` (2) | `R'G'B'A float` | image profile's TRC | per-channel TRC of the profile; for sRGB the sRGB TRC |
| `RGB_PERCEPTUAL` (4) | `R~G~B~A float` | always the sRGB TRC, whatever the profile | sRGB TRC |
| `LAB` (3) | `CIE Lab alpha float` | CIE L\*a\*b\* (D50) | RGB→XYZ(D50)→Lab |

Measured on the system babl 0.1.108 with sRGB space (not read from source — babl is not in the pinned checkout):
- linear 0.5 → 0.7353570 (`R'G'B'` and `R~G~B~` identical); linear 0.002 → 0.02584 (= ×12.92); linear 0.0031308 → 0.0404499; `R'G'B'` 0.5 → 0.2140412 linear; 0.04 → 0.0030960; 0.04045 → 0.0031308. This is the standard piecewise sRGB curve: encode `v <= 0.0031308 ? 12.92*v : 1.055*pow(v, 1/2.4) - 0.055`, decode `v <= 0.04045 ? v/12.92 : pow((v + 0.055)/1.055, 2.4)` (exact threshold constants inside babl not verified; values agree to 7 digits).
- linear RGB → XYZ uses the **D50-adapted** sRGB matrix: R→(0.4360348, 0.2224879, 0.0139159), G→(0.3851167, 0.7169038, 0.0970605), B→(0.1430514, 0.0606083, 0.7139290). White (1,1,1) → Lab (100, 0, 0), so the Lab white point is D50 = (0.96420, 1.00000, 0.82491). Lab is the standard CIE formula (`L = 116 f(Y/Yn) - 16`, `a = 500 (f(X/Xn) - f(Y/Yn))`, `b = 200 (f(Y/Yn) - f(Z/Zn))`); check value: linear (0.2, 0.5, 0.8) → Lab (72.98636, −12.46405, −28.65639), LCH(ab) (72.98636, 31.24966, 246.49345°).
- `"Y float"` from linear RGB = 0.2224879 R + 0.7169038 G + 0.0606083 B (check: (0.2,0.5,0.8) → 0.4514361). `"Y' float"` = sRGB-encode(Y) (check 0.4816063).

Port implications:
1. For **non-sRGB profiles** `nl` ≠ `perc`; the first port can assume sRGB (document as limitation) and treat both as the sRGB TRC.
2. Default modes composite in **linear**; images are normally stored `R'G'B'A u8`. Compositing an 8-bit image therefore means: decode to linear float → composite → (projection stays float) → encode for display. Legacy modes composite on the encoded values directly.
3. Opacity/mask application order: blend → (convert back) → composite, where the composite multiplies `layer.a` (or `comp.a`) by `opacity` and `mask` — never premultiply before blending.
4. Values are unclamped floats end-to-end; clamping happens only when a buffer is converted to an integer format.

### 2.8 Bit-accuracy checklist for the port

- Use `float` (not double) arithmetic in the same expression order; `MIN/MAX/CLAMP` are the GLib macros (`CLAMP(x,lo,hi) = x > hi ? hi : (x < lo ? lo : x)`).
- Keep the exact zero tests (`== 0.0f`, truthiness) — they decide whether `in.rgb` or `layer.rgb` is copied where alpha is 0, which matters because GIMP preserves RGB under fully transparent pixels.
- Dispatch on `(mode, composite_mode)`; resolve AUTO via the table first; for the bottom-most visible layer apply the "last node" rule of §1.5.
- NORMAL has SSE2/SSE4 variants upstream; results may differ from the C path in the last ulp. The C path in `gimpoperationnormal.c` is the reference.
- Luminance weights and Lab depend on babl; pin the constants above for sRGB and add a regression test against the GEGL bridge (`gegl_node` with `gimp:`-less equivalents is not available in system GEGL — the `gimp:*` ops live in the GIMP binary, so the test oracle must be GIMP itself or hand-computed vectors).

---

## 3. Paint engine (`app/paint`)

### 3.1 Paint cores

Class tree (`G_DEFINE_TYPE` in each file); registration order in `app/paint/gimp-paint.c`: dodge-burn, smudge, convolve, perspective-clone, heal, clone, mybrush, ink, airbrush, eraser, paintbrush, pencil.

```
GimpPaintCore (GimpObject)                      gimppaintcore.c (1720 lines), -loops.cc, -stroke.c, gimppaintcoreundo.c
 ├ GimpBrushCore                                gimpbrushcore.c (1385), gimpbrushcore-loops.cc, -kernels.h
 │  ├ GimpPaintbrush  "gimp-paintbrush"         gimppaintbrush.c
 │  │  ├ GimpPencil   "gimp-pencil"             gimppencil.c (no code; options default hard=TRUE)
 │  │  ├ GimpAirbrush "gimp-airbrush"           gimpairbrush.c
 │  │  └ GimpEraser   "gimp-eraser"             gimperaser.c
 │  ├ GimpSourceCore                            gimpsourcecore.c
 │  │  ├ GimpClone    "gimp-clone"              gimpclone.c
 │  │  │  └ GimpPerspectiveClone "gimp-perspective-clone"  gimpperspectiveclone.c
 │  │  └ GimpHeal     "gimp-heal"               gimpheal.c
 │  ├ GimpSmudge      "gimp-smudge"             gimpsmudge.c
 │  ├ GimpConvolve    "gimp-convolve"           gimpconvolve.c
 │  └ GimpDodgeBurn   "gimp-dodge-burn"         gimpdodgeburn.c
 ├ GimpInk            "gimp-ink"                gimpink.c, gimpink-blob.c, gimpinkundo.c
 └ GimpMybrushCore    "gimp-mybrush"            gimpmybrushcore.c, gimpmybrushsurface.c (libmypaint)
```
`GimpBrushCoreClass` capability flags (defaults `handles_changing_brush FALSE`, `handles_transforming_brush TRUE`, `handles_dynamic_transforming_brush TRUE`): `handles_changing_brush = TRUE` (brush pipes may switch per dab) for Paintbrush (+Pencil/Airbrush/Eraser), DodgeBurn, Smudge, SourceCore (+Clone/Heal/PerspectiveClone).

### 3.2 Options

Common `GimpPaintOptions` (`gimppaintoptions.c`; also a `GimpContext`, which supplies foreground/background, **opacity**, **paint-mode**, brush, dynamics, pattern, gradient, palette):

| Property | Type | Range | Default |
|---|---|---|---|
| `brush-size` | double | 1.0 … `GIMP_BRUSH_MAX_SIZE` (10000) | 20.0 |
| `brush-aspect-ratio` | double | −20 … 20 | 0.0 |
| `brush-angle` | double; property value in degrees, **stored internally as turns with inverted sign**: `options->brush_angle = -1.0 * value / 360.0` (`gimppaintoptions.c:570`; getter multiplies back by −360) | −180 … 180 | 0.0 |
| `brush-spacing` | double (fraction of brush size) | 0.01 … 50.0 | 0.1 |
| `brush-hardness` | double | 0 … 1 | 1.0 |
| `brush-force` | double | 0 … 1 | 0.5 |
| `brush-link-size`, `brush-link-aspect-ratio`, `brush-link-angle`, `brush-link-spacing`, `brush-link-hardness` | bool | | TRUE (option follows the brush's native value) |
| `brush-lock-to-view` | bool | | FALSE |
| `application-mode` | `GimpPaintApplicationMode` | `GIMP_PAINT_CONSTANT 0`, `GIMP_PAINT_INCREMENTAL 1` | CONSTANT |
| `hard` | bool | | FALSE (Pencil: TRUE) |
| `use-jitter`, `jitter-amount` | bool, double 0…50 | | FALSE, 0.2 |
| `dynamics-enabled` | bool | | TRUE |
| `fade-length`, `fade-unit`, `fade-reverse`, `fade-repeat` | double 0…32767, unit, bool, `GimpRepeatMode` | | 100.0, pixel, FALSE, `GIMP_REPEAT_NONE` |
| `gradient-reverse`, `gradient-blend-color-space`, `gradient-repeat` | bool, `GimpGradientBlendColorSpace`, `GimpRepeatMode` | | FALSE, `RGB_PERCEPTUAL`, `GIMP_REPEAT_NONE` |
| `use-smoothing`, `smoothing-quality`, `smoothing-factor` | bool, int 1…100, double 3…1000 | | (FALSE), 20, 50 |
| `expand-use`, `expand-amount`, `expand-fill-type`, `expand-mask-fill-type` | bool, double 1…1000, `GimpFillType`, `GimpAddMaskType` | | FALSE, 100.0, TRANSPARENT, WHITE (auto-expand layer while painting; new in 3.0) |
| `use-applicator` | bool (debug: use GEGL applicator instead of the fast loops) | | |
| view-type/size props for brush, dynamics, pattern, gradient pickers | | | UI only |

So `core->angle` in `GimpBrushCore` is a **fraction of a full turn** (e.g. `angle * 2π` in `gimp_brush_core_interpolate`, `angle *= 360` in `gimp_brush_generated_transform_mask`, `-2π·angle` in `gimp_brush_transform_matrix`).

Per-tool options:

| Tool | Options class | Properties (range, default) |
|---|---|---|
| Paintbrush | `GimpPaintOptions` | common only |
| Pencil | `GimpPencilOptions` | `hard` default TRUE |
| Airbrush | `GimpAirbrushOptions` | `rate` 0…100 (50.0); `motion-only` (FALSE); `flow` 0…100 (10.0); legacy alias `pressure` 0…100 (10.0) |
| Eraser | `GimpEraserOptions` | `anti-erase` (FALSE) |
| Clone | `GimpCloneOptions` : `GimpSourceOptions` | `clone-type` `GIMP_CLONE_IMAGE 0` / `GIMP_CLONE_PATTERN 1` (IMAGE); source: `align-mode` `GimpSourceAlignMode` {`NO`, `YES` "Aligned", `REGISTERED`, `FIXED`} (NO), `sample-merged` (FALSE), source drawables, `src-x/src-y` |
| Heal | `GimpSourceOptions` | as source options |
| Perspective clone | `GimpPerspectiveCloneOptions` | `clone-mode` `GIMP_PERSPECTIVE_CLONE_MODE_ADJUST` "Modify Perspective" / `_PAINT` "Perspective Clone" (ADJUST) + clone options |
| Smudge | `GimpSmudgeOptions` | `rate` 0…100 (50.0); `flow` 0…100 (0.0); `no-erasing` (FALSE); `sample-merged` (FALSE) |
| Convolve (Blur/Sharpen) | `GimpConvolveOptions` | `type` `GIMP_CONVOLVE_BLUR 0` / `GIMP_CONVOLVE_SHARPEN 1` (BLUR); `rate` 0…100 (50.0) |
| Dodge/Burn | `GimpDodgeBurnOptions` | `type` `GIMP_DODGE_BURN_TYPE_DODGE 0` / `_BURN 1` (DODGE); `mode` `GimpTransferMode` {`SHADOWS 0`, `MIDTONES 1`, `HIGHLIGHTS 2`} (MIDTONES); `exposure` 0…100 (50.0) |
| Ink | `GimpInkOptions` | `size` 0…200 (16.0); `tilt-angle` −90…90 (0); `size-sensitivity` 0…1 (1.0); `vel-sensitivity` 0…1 (0.8); `tilt-sensitivity` 0…1 (0.4); `blob-type` `GIMP_INK_BLOB_TYPE_CIRCLE/SQUARE/DIAMOND` (CIRCLE); `blob-aspect` 1…10 (1.0); `blob-angle` −π…π (0) |
| MyPaint brush | `GimpMybrushOptions` | `viewzoom` 0.0001…FLT_MAX (1.0); `viewrotation` −360…360 (0); `radius` −2…6 (1.0, log scale); `opaque` 0…2 (1.0); `hardness` 0…1 (1.0); `gain` −1.8…1.8 (0); `pigment` 0…1 (0); `posterize` 0…1 (0); `posterizenum` 0…1.28 (1.0); `eraser` (FALSE); `no-erasing` (FALSE) |

### 3.3 `GimpPaintCore` stroke lifecycle

State (`gimppaintcore.h`): `start_coords`, `cur_coords`, `last_coords` (`GimpCoords`), `last_paint` (GimpVector2), `distance` (in brush-space units), `pixel_dist` (in pixels), undo extents `x1,y1,x2,y2`, `use_saved_proj`, `image_pickable`, `undo_buffers` (hash drawable → full pre-stroke copy), `saved_proj_buffer`, `canvas_buffer` (`"Y float"`, size = max drawable size: the per-stroke coverage accumulator), `paint_buffer` (+`paint_buffer_x/y`: the RGBA float dab in the paint mode's composite-space format), `mask_buffer` (selection), `applicators`, `stroke_buffer` (GArray of GimpCoords for smoothing), `sym`.

`GimpCoords` (`core-types.h`): `x, y, pressure, xtilt, ytilt, wheel, distance, rotation, slider` (device axes), `velocity, direction` (synthetic, both normalised 0..1; direction in turns), `xscale, yscale, angle, reflect` (view transform).

Class vfuncs: `start`, `pre_paint`, `paint(core, drawables, options, sym, GimpPaintState, time)`, `post_paint`, `interpolate`, `get_paint_buffer`, `push_undo`. `GimpPaintState`: `GIMP_PAINT_STATE_INIT`, `GIMP_PAINT_STATE_MOTION`, `GIMP_PAINT_STATE_FINISH`.

Sequence (driven by `app/tools/gimppainttool*.c` or by `gimppaintcore-stroke.c` for programmatic strokes):
1. `gimp_paint_core_start(core, drawables, options, coords, error)`: records `start_coords = last_coords`, `cur_coords = *coords`; calls class `start` (brush core: fetch brush + dynamics, compute scale/angle/…, `spacing = options->brush_spacing`, `jitter`); duplicates every target drawable's buffer into `undo_buffers`; allocates `canvas_buffer` (zeroed "Y float"); undo extents = current point; `last_paint = (-1e6, -1e6)`; grabs the selection mask unless empty or painting on the selection itself.
2. `gimp_paint_core_paint(core, drawables, options, GIMP_PAINT_STATE_INIT, time)`, then for the first point `…STATE_MOTION`.
3. For every further input event: optional `gimp_paint_core_smooth_coords` (§3.7), `gimp_paint_core_set_current_coords`, then `gimp_paint_core_interpolate()` → class `interpolate` (brush core: place N dabs between `last_coords` and `cur_coords`, each via `gimp_paint_core_paint(…MOTION…)`).
4. `gimp_paint_core_paint(…STATE_FINISH…)`, `gimp_paint_core_finish(core, drawables, push_undo)`: if `x1==x2 || y1==y2` nothing changed; else open group `GIMP_UNDO_GROUP_PAINT`, push `GIMP_UNDO_PAINT` (stores start coords for "straight line from last point") and a drawable undo holding the pre-stroke pixels of the rectangle `[x1,x2)×[y1,y2)` cut from `undo_buffers` (aligned to the buffer's tile grid); frees buffers. `gimp_paint_core_cancel` restores the rectangle from `undo_buffers`.

`gimp_paint_core_paint`: `pre_paint` (may veto) → on MOTION save `last_paint = cur_coords.xy` → `gimp_symmetry_set_origin(sym, drawable, &cur_coords)` → class `paint` → `post_paint`.

**Pasting a dab — `gimp_paint_core_paste(core, paint_mask, mask_off_x, mask_off_y, drawable, paint_opacity, image_opacity, paint_mode, GimpPaintApplicationMode mode)`** (default path = `gimp_paint_core_loops_process`, `gimppaintcore-loops.cc`):

- `GIMP_PAINT_CONSTANT` (default): the brush mask is accumulated into `canvas_buffer` so that overlapping dabs of one stroke never exceed `paint_opacity`:
  ```c
  // COMBINE_PAINT_MASK_TO_CANVAS_BUFFER, per pixel, mask value mv in 0..1
  if (stipple /* core is GimpAirbrush */)
      canvas += (1.0 - canvas) * mv * paint_opacity;
  else if (paint_opacity > canvas)
      canvas += (paint_opacity - canvas) * mv * paint_opacity;
  ```
  then the composite mask for this dab = `canvas` (× selection mask), source = **`undo_buffer`** (the pre-stroke pixels): `dest = layer_mode(src = undo, layer = paint_buf with alpha × canvas, opacity = image_opacity)`. Net effect: the whole stroke is re-composited from the original pixels through the accumulated coverage.
- `GIMP_PAINT_INCREMENTAL`: composite mask = `mv * paint_opacity` (× selection), source = the **current** drawable buffer; `canvas_buffer` untouched. Dabs build up.
- Then `DO_LAYER_BLEND`: runs `gimp_layer_mode_get_function(paint_mode)` with blend/composite space AUTO and composite mode `gimp_layer_mode_get_paint_composite_mode(paint_mode)`, opacity `image_opacity` (the context opacity), mask = composite mask; and `MASK_COMPONENTS` if not all channels are active (`gimp_drawable_get_active_mask`: honours lock-alpha and per-channel "active").
- Undo extents grow by the paint buffer rectangle; `gimp_drawable_update`.

`gimp_paint_core_replace(...)` (reached through `gimp_brush_core_replace_canvas`; used by Smudge, DodgeBurn, Convolve, Heal): if the drawable has **no alpha** it simply calls `gimp_paint_core_paste(…, GIMP_LAYER_MODE_NORMAL, mode)`; otherwise the same mask/canvas logic runs but the final composite uses `GIMP_LAYER_MODE_REPLACE` ("replace" semantics: all four components are interpolated towards the paint buffer, so transparent paint erases), with the selection mask combined into the paint mask first (`gimp_gegl_combine_mask`).

Two opacities therefore exist: **paint_opacity** (per-dab: dynamics opacity × tool factor, capped at 1) limits accumulation; **image_opacity** (the tool "Opacity" slider) scales the final composite.

### 3.4 `GimpBrushCore`

State: `main_brush`, `brush` (current pipe member), `dynamics`, `spacing`, `scale`, `aspect_ratio`, `angle`, `reflect`, `hardness`, `symmetry_angle`, `symmetry_reflect`, `jitter`, `jitter_lut_x/y[BRUSH_CORE_JITTER_LUTSIZE = 360]` (`sin`/`cos` of i degrees), caches: `transform_brush`, `subsample_brushes[5][5]`, `solid_brushes[2][2]`, `pressure_brush`, `rand`. `EPSILON 0.00001`.

**Transform parameters** (`gimp_brush_core_eval_transform_dynamics`), evaluated at start and before each dab:
```c
max_side = MAX(brush_width, brush_height);
scale = brush_size / max_side;
if (lock_to_view && MAX(xscale, yscale) > 0) { scale /= MAX(xscale, yscale);
     if (scale * max_side > GIMP_BRUSH_MAX_SIZE) scale = GIMP_BRUSH_MAX_SIZE / max_side; }
aspect_ratio = options->brush_aspect_ratio; angle = options->brush_angle; reflect = FALSE; hardness = options->brush_hardness;
if (lock_to_view) { angle += coords->angle; reflect = coords->reflect; }
if dynamics enabled && class->handles_dynamic_transforming_brush:
   scale    *= linear_value(SIZE);
   angle    += angular_value(ANGLE);
   hardness *= linear_value(HARDNESS);
   if (output ASPECT_RATIO enabled) { a = aspect_value(ASPECT_RATIO);
        aspect_ratio = (aspect_ratio == 0.0) ? 10.0 * a : aspect_ratio * a; }
```
Symmetry adds `symmetry_angle` (degrees/360) and `symmetry_reflect` per stroke copy (`gimp_brush_core_eval_transform_symmetry`); `gimp_brush_core_get_angle()` combines them.

**Mask transform for bitmap brushes** (`app/core/gimpbrush-transform.cc`):
```c
gimp_brush_transform_get_scale(scale, aspect_ratio, &sx, &sy):
   if (aspect_ratio < 0) { sx = scale * (1 + aspect_ratio/20); sy = scale; }
   else                  { sx = scale; sy = scale * (1 - aspect_ratio/20); }
gimp_brush_transform_matrix(w, h, sx, sy, angle, reflect, &m):   // center = (w/2, h/2)
   identity; scale(sx, sy); translate(-cx*sx, -cy*sy); rotate(-2π·angle); if (reflect) scale(-1, 1); translate(cx*sx, cy*sy);
```
The destination bounding box is the transformed source rectangle; the source is sampled with bilinear interpolation through the inverse matrix (fixed-point, parallelised). Hardness < 1 for bitmap brushes = shrink + box blur: `blur_radius = floor((1 - hardness) * (sqrt(0.5) - 0.5) * MIN(width, height))`, the matrix is additionally scaled by `(MIN(w,h) - 2*blur_radius) / MIN(w,h)`, then `gimp_brush_transform_blur(result, blur_radius)`. Generated brushes rasterise analytically instead (§3.9.3). Results are cached in `GimpBrushCache` keyed by (scale, aspect, angle, reflect, hardness).

**Sub-pixel placement and hardness modes** — `gimp_brush_core_get_brush_mask(core, coords, GimpBrushApplicationMode, dynamic_force)`; returns NULL if `dynamic_force <= 0`. Mode from `gimp_paint_options_get_brush_mode`: `hard` option → `GIMP_BRUSH_HARD`; else if dynamics FORCE output enabled **or** `brush_force != 0.5` → `GIMP_BRUSH_PRESSURE`; else `GIMP_BRUSH_SOFT`.
- `SOFT` → `gimp_brush_core_subsample_mask`: `KERNEL_WIDTH = KERNEL_HEIGHT = 3`, `KERNEL_SUBSAMPLE = 4` (`gimpbrushcore-kernels.h`, "generated using kernelgen, threshold = 0.25"): `index1 = (int)(frac(x) * 5)`, `index2 = (int)(frac(y) * 5)`; for even mask width/height add `KERNEL_SUBSAMPLE >> 1` (=2) and wrap with a +1 pixel destination offset; the mask (padded by 1 px each side → `(w+2)×(h+2)`) is convolved with the 3×3 kernel `subsample[index2][index1]` (integer weights summing to 256; u8 rounding `(x + 128) / 256`, float `x / 256`). 25 cached variants.
- `HARD` → `gimp_brush_core_solidify_mask`: output `"Y float"` `(w+2)×(h+2)`, value `mask ? 1.0 : 0.0`; for even sizes a half-pixel offset (`dest_offset = 1` when `frac(x) >= 0.5`) — 2×2 cached variants.
- `PRESSURE` → `gimp_brush_core_pressurize_mask`: applied on top of the subsampled mask with "SimplePressure": `I' = MIN(2 * pressure * I, 1)` (u8: `MIN(RINT(2*pressure*x), 255)`), i.e. force 0.5 is neutral. (`FANCY_PRESSURE` variant is compiled out.)

**Dab rectangle** (`gimp_brush_core_get_paint_buffer`): `x = floor(coords.x) - brush_width/2; y = floor(coords.y) - brush_height/2` (integer division); paint buffer rect = `[x-1, x+brush_width+1) × [y-1, y+brush_height+1)` clamped to the drawable (after optional auto-expand). Mask offsets when clipped at the top/left: `off = (x < 0) ? -x : 0` (`gimp_brush_core_paste_canvas`, using the mask size `>> 1`).

**Spacing / interpolation** (`gimp_brush_core_interpolate`) — reproduce exactly for stroke-shape parity:
```c
avoid_exact_integer(last.x, last.y, cur.x, cur.y);       // nudge to ±EPSILON from integers
delta = cur - last (x, y, pressure, xtilt, ytilt, wheel, velocity); return if all zero;
pixel_dist = |delta.xy|; pixel_initial = core->pixel_dist;
if (scale == 0) { paint once at cur; pixel_dist bookkeeping; return; }
// dynamic spacing (SPACING output enabled):
dyn = linear_value(SPACING); dyn = spacing + (2.0 - spacing) * (1.0 - dyn); dyn = MAX(spacing, dyn);
// distance in brush coordinates: project delta on the brush's scaled+rotated x/y axes
xa = brush_x_axis * scale, rotated by angle*2π;  xd = dot(delta, xa) / |xa|²;   (same for y axis → yd)
dist = 0.5 * sqrt(xd² + yd²); total = dist + core->distance; initial = core->distance;
// dominant axis "stripes":
if (delta.x² > delta.y²) { st_factor = delta.x; st_offset = last.x - 0.5; } else { st_factor = delta.y; st_offset = last.y - 0.5; }
if (dynamic spacing)        { num_points = dist / dyn; s0 = floor(st_offset + 0.5); t0 = (s0 - st_offset) / st_factor; dt = dyn / dist; if (!num_points) return; }
else if (fabs(st_factor) > dist / spacing) {       // normal case: use the brush spacing
     dt = spacing / dist; n = (int)(initial / spacing + 1.0 + EPSILON); t0 = (n * spacing - initial) / dist;
     num_points = 1 + (int) floor((1 + EPSILON - t0) / dt);
     if (num_points == 0 && (delta.x == 0 || delta.y == 0)) return; }
else if (fabs(st_factor) < EPSILON) { cur.xy = last.xy; return; }     // hardly moved
else {  // stripe regime (spacing finer than 1 px along the dominant axis): one dab per integer stripe
     direction = st_factor > 0 ? 1 : -1;
     s0 = floor(st_offset + 0.5); sn = floor(st_offset + st_factor + 0.5);
     exceptions A/B/C: skip the first stripe if its position extrapolates into another pixel (t0 < 0) or falls
       in the same pixel as core->last_paint; skip the last if tn > 1 and it leaves the end pixel;
     t0 = (s0 - st_offset)/st_factor; tn = (sn - st_offset)/st_factor; dt = direction / st_factor; num_points = 1 + direction*(sn - s0);
     if (num_points >= 1) { if (tn < 1) total = initial + tn*dist; total = spacing * (int)(total/spacing + 0.5); total += (1 - tn)*dist; } }
for (n = 0; n < num_points; n++) { t = t0 + n*dt; p = (double) n / num_points;
     cur.xy = last.xy + t*delta.xy; pressure/xtilt/ytilt/wheel/velocity = last + p*delta;  // note: p, not t
     direction = cur.direction (unchanged); xscale,yscale,angle,reflect from last;
     if (core->jitter > 0) { dyn_jitter = jitter * linear_value(JITTER);
          jitter_dist = g_rand_double_range(rand, 0, dyn_jitter); a = g_rand_int_range(rand, 0, 360);
          cur.x += (x_axis.x + y_axis.x) * jitter_dist * jitter_lut_x[a] * scale;
          cur.y += (y_axis.y + x_axis.y) * jitter_dist * jitter_lut_y[a] * scale; }
     core->distance = initial + t*dist; core->pixel_dist = pixel_initial + t*pixel_dist;
     gimp_paint_core_paint(…MOTION…); }
cur = last + delta (all axes); last_coords = cur; core->distance = total; core->pixel_dist = pixel_initial + pixel_dist;
```
`brush_x_axis`/`y_axis` for bitmap brushes are `(w/2, 0)` and `(0, h/2)` (set by the loader), so `dist` is in units of "brush sizes" and `spacing` 0.1 = a dab every 10 % of the brush diameter.

**Paintbrush dab** (`_gimp_paintbrush_motion(core, drawable, options, sym, opacity)`): `fade_point = gimp_paint_options_get_fade(...)`; `opacity *= linear_value(OPACITY)`; return if 0; eval transform dynamics; `grad_point = linear_value(COLOR)`; for each symmetry stroke i: `get_paint_params` → colour source priority: (1) dynamics enabled and gradient colour available (`gimp_paint_options_get_gradient_color`, COLOR output) → colour from gradient, forces `INCREMENTAL`; (2) brush has a pixmap → pixmap tiles (`gimp_brush_core_color_area_with_pixmap`), forces `INCREMENTAL`; (3) foreground colour. Paint buffer filled with the colour (alpha forced to 1; the colour's alpha multiplies `opacity`); `force` = FORCE output value if enabled else `options->brush_force`; then `gimp_brush_core_paste_canvas(core, drawable, &coords, MIN(opacity, 1), gimp_context_get_opacity(context), paint_mode, brush_mode, force, paint_appl_mode)`.

### 3.5 Paint dynamics (`app/core/gimpdynamics.c`, `gimpdynamicsoutput.c`)

Outputs (`GimpDynamicsOutputType`): `OPACITY, SIZE, ANGLE, COLOR, HARDNESS, FORCE, ASPECT_RATIO, SPACING, RATE, FLOW, JITTER`. Each `GimpDynamicsOutput` has 8 inputs with an enable flag and a `GimpCurve` each: `use-pressure/pressure-curve`, `use-velocity`, `use-direction`, `use-tilt`, `use-wheel`, `use-rotation` (not all are listed in old `.gdyn` files), `use-random`, `use-fade`. File format `.gdyn` = GimpConfig s-expressions: `(name "…")` then one block per output `(opacity-output (use-pressure yes) (use-velocity no) (use-direction no) (use-tilt no) (use-random no) (use-fade yes) [(pressure-curve (curve-type smooth) (n-points N) (points …) …)])` (see `data/dynamics/Basic-Dynamics.gdyn`).

Evaluation = **mean of the enabled inputs after curve mapping** (`gimp_curve_map_value(curve, x)`; identity by default):

| Function | Inputs fed to the curves | No inputs enabled |
|---|---|---|
| `get_linear_value` | pressure: `pressure`; velocity: `1 - velocity`; direction: `fmod(direction + 0.5, 1)`; tilt: `1 - sqrt(xtilt² + ytilt²)`; wheel: `wheel`; rotation: `rotation`; random: `g_random_double_range(0,1)`; fade: `fade_point` | 1.0 |
| `get_angular_value` | pressure; `1 - velocity`; direction: `curve(direction)` then, if lock-to-view, `if (reflect) a = 0.5 - a; a -= coords.angle; a = fmod(fmod(a,1)+1,1)`; tilt: converted to an angle `tilt = atan(-ytilt/xtilt)/(2π) (+0.5 if xtilt > 0)`, special cases xtilt==0 → 0.25/0.75/0, then `+0.5` and wrapped to [0,1]; wheel: `1 - fmod(0.5 + wheel, 1)`; rotation: `1 - curve(rotation)`; random; fade | 0.0 (additive) |
| `get_aspect_value` | pressure; `velocity`; direction: contributes 1.0 and sets `sign = -1` when `curve(direction)` ∈ (0.875,1] ∪ (0,0.125) ∪ (0.375,0.625); tilt: `curve(MAX(|xtilt|,|ytilt|))`; wheel: like direction; rotation: folded to `4*|r|` with sign flip above 0.5; random; fade. Result `CLAMP(mean * sign, -1, 1)` | 1.0 |

Fade (`gimp_paint_options_get_fade(options, image, pixel_dist)`): `fade_out` = `fade_length` px, or `MAX(image w,h) * fade_length / 100` for percent, or `fade_length * MAX(xres,yres) / unit_factor` otherwise; `pos = fade_out > 0 ? pixel_dist / fade_out : 1.0`; `GIMP_REPEAT_NONE`: `pos >= 1` → `1 - 1e-7`; then `if (((int) pos & 1) && repeat != GIMP_REPEAT_SAWTOOTH) pos = 1 - (pos - (int) pos); else pos = pos - (int) pos;` (triangular vs sawtooth); `fade_reverse` → `1 - pos`.

Bundled dynamics (`data/dynamics`, 18 files): top level `Basic-Dynamics, Color-From-Gradient, Fade-Tapering, Random-Color, Tilt-Angle, Track-Direction, Velocity-Tapering`; `Basic/`: `Basic-Simple, Dynamics-Random, Negative-Size-Pressure, Pen-Generic, Pencil-Generic, Pencil-Shader, Pressure-Opacity, Pressure-Size`; `FX/`: `Confetti, Perspective, Speed_Size_Opacity`.

### 3.6 Symmetry painting (`app/core/gimpsymmetry*.c`, `gimpimage-symmetry.c`)

`GimpSymmetry` base (identity: 1 stroke): `gimp_symmetry_set_origin(sym, drawable, coords)`, `gimp_symmetry_get_size(sym)` (number of stroke copies), `gimp_symmetry_get_coords(sym, i)`, `gimp_symmetry_get_transform(sym, i, &angle_degrees, &reflect)` (brush transform for copy i), `gimp_symmetry_get_origin`. Stored per image as parasites (`gimp_symmetry_to_parasite` / `gimp_symmetry_from_parasite`, name from `gimp_symmetry_parasite_name(type)`).

| Class | File | Properties (range, default) | Stroke generation |
|---|---|---|---|
| `GimpMirror` | `gimpsymmetry-mirror.c` | `horizontal-symmetry`, `vertical-symmetry`, `point-symmetry` (bool, FALSE); `disable-transformation` (FALSE: brush is mirrored too); `mirror-position-x`, `mirror-position-y` (0…DBL_MAX, 0 → image centre on creation) | original + reflection across the horizontal axis (y' = 2·my − y), vertical axis (x' = 2·mx − x), point (both); up to 4 strokes; draws `GIMP_GUIDE_STYLE_MIRROR` guides |
| `GimpTiling` | `gimpsymmetry-tiling.c` | `interval-x`, `interval-y` (0…DBL_MAX, 0), `shift` (0…DBL_MAX, 0: x shift applied to alternate rows), `max-x`, `max-y` (int 0…100, 0 = unlimited) | copies of the stroke on a grid with pitch `interval_x × interval_y`; an interval < 1.0 disables that axis (count 1); with `max == 0` the count is `ceil(drawable_size / interval)` (for *stateful* tools — `sym->stateful`, e.g. MyPaint — `2·count + 1` strokes starting `count` intervals before the origin, so the stroke list never changes mid-stroke); exact row-shift arithmetic not traced line-by-line |
| `GimpMandala` | `gimpsymmetry-mandala.c` | `center-x`, `center-y` (0…DBL_MAX), `size` (int 1…100, default 6), `disable-transformation` (FALSE), `enable-reflection` (FALSE: kaleidoscope) | `slice = 2π/size`; copy i (1…size−1): translate(−c) · rotate(−i·slice) · translate(+c); with reflection, odd copies use rotate(−mid) · scale(1,−1) · rotate(mid − i·slice) where `mid = floor(atan2(dy,dx)/slice)·slice + slice/2` |

Every brush-based tool loops `for (i = 0; i < gimp_symmetry_get_size(sym); i++)` and paints one dab per copy.

### 3.7 Smoothing / stabiliser (`gimp_paint_core_smooth_coords`)

Each incoming event is appended to `stroke_buffer`; if `use-smoothing` and `smoothing-quality > 0` and at least 2 points: only x and y are replaced by a velocity-windowed Gaussian average over the last `MIN(quality, n)` points, newest first:
```c
gaussian_weight2 = SQR(smoothing_factor);
gaussian_weight  = 1 / (sqrt(2π) * smoothing_factor);
velocity_sum = 0; scale_sum = 0; x = y = 0;
for (i = len-1; i >= len - length; i--) {
    velocity_sum += pt[i].velocity * 100;
    rate = gaussian_weight * exp(-velocity_sum*velocity_sum / (2 * gaussian_weight2));
    scale_sum += rate; x += rate * pt[i].x; y += rate * pt[i].y; }
if (scale_sum != 0) { x /= scale_sum; y /= scale_sum; }
```
(Velocity itself is computed by the display/tool layer — `app/display/gimpmotionbuffer.c` — and is outside this doc.)

### 3.8 Per-tool algorithms

| Tool | Per-dab algorithm (file: function) |
|---|---|
| **Paintbrush** | `_gimp_paintbrush_motion` (§3.4), INIT/FINISH manage colour-history; paint mode = context paint mode. |
| **Pencil** | Paintbrush with `hard = TRUE` → `GIMP_BRUSH_HARD` masks (no anti-aliasing, no sub-pixel kernels). |
| **Airbrush** | `gimp_airbrush_motion`: `opacity = flow/100 * linear_value(FLOW)`; then `_gimp_paintbrush_motion`. Canvas accumulation uses the *stipple* rule (§3.3). While the pointer rests (unless `motion-only` or `rate == 0`) a GLib timeout re-stamps: `timeout_ms = (1000.0 / STAMP_MAX_FPS /*60*/) / ((rate/100) * linear_value(RATE))` → emits `stamp` signal → the tool calls paint again at the same coords. |
| **Eraser** | Paintbrush with `get_paint_params` overridden: colour = **background**; mode = `GIMP_LAYER_MODE_ANTI_ERASE` if `anti-erase`, else `GIMP_LAYER_MODE_ERASE` if the drawable has alpha, else `GIMP_LAYER_MODE_NORMAL_LEGACY` (paints background colour). |
| **Clone** | `gimp_source_core_motion` computes the source rectangle from the align mode: `NO` — source offset fixed at stroke start, every stroke restarts from the source point; `YES` (Aligned) — offset fixed at the first stroke ever; `REGISTERED` — offset (0,0) (same coords in source drawable); `FIXED` — always the same source point. `gimp_clone_motion`: `IMAGE` → `gimp_gegl_buffer_copy(src, src_rect → paint_buffer)`; `PATTERN` → `gegl_buffer_set_pattern(paint_buffer, pattern, -paint_x - src_offset_x, …)`; then `gimp_brush_core_paste_canvas(…, GIMP_PAINT_CONSTANT)` with opacity = OPACITY dynamics and force as paintbrush. Source pickable: the source drawable, or the image projection with `sample-merged`; `use_saved_proj` snapshots it when source = destination image. |
| **Perspective clone** | As clone, but the source region is mapped through the perspective matrix set in ADJUST mode: for each dab the destination rect is transformed by the inverse matrix to find the source bbox, which is rendered through a `gegl:transform` node (`gimp_perspective_clone_get_source`). |
| **Heal** | `gimp_heal_motion`: brush mask obtained with `GIMP_BRUSH_HARD`; copies source rect and destination rect to float temp buffers; `gimp_heal(src, dest, mask)`: `diff = dest − src` on the mask border is the Dirichlet condition, solve Laplace `Δdiff = 0` inside the mask with red/black Gauss-Seidel + over-relaxation (`gimp_heal_laplace_loop`, convergence 0.1 %, SSE variant), result = `src + diff` ("Georgiev, Photoshop Healing Brush"). Pasted with `gimp_brush_core_replace_canvas(…, GIMP_PAINT_INCREMENTAL)`. |
| **Smudge** | `gimp_smudge_start` fills an accumulator buffer (one per symmetry stroke, brush-sized + margin) with the pixels under the first dab. Per dab (`gimp_gegl_smudge_with_paint_process`, `app/gegl/gimp-gegl-loops.cc`): `accum = blend(accum, rate, canvas, 1 − rate)`; if no colour and flow == 0 → `paint = accum`; else `paint = blend(brush_color or pixmap, flow, accum, 1 − flow)`; where `blend(s1, r1, s2, r2)`: `a1 = r1·s1.a; a2 = r2·s2.a; ra = a1 + a2; if (ra == 0) dest = 0; else { dest.rgb = (s1.rgb·a1 + s2.rgb·a2)/ra; if (no_erasing) ra = MAX(ra, s2.a); dest.a = ra; }`. `rate = options.rate/100 × linear_value(RATE)`, `flow = options.flow/100 × linear_value(FLOW)`. Pasted with `gimp_brush_core_replace_canvas(…, GIMP_PAINT_INCREMENTAL)`. |
| **Convolve** (blur/sharpen) | 3×3 kernel `[a b a; c M c; a b a]` with a/b/c = 1 or 0 depending on brush radius (`radius_x && radius_y`, `radius_y`, `radius_x`); centre `M`: blur `MIN_BLUR 64 + percent·(MAX_BLUR 0.25 − 64)`, sharpen `MIN_SHARPEN −512 + percent·(MAX_SHARPEN −64 − (−512))`, `percent = MIN(rate/100, 1)`, `rate = options.rate × linear_value(RATE)`; divisor = sum of the matrix; `gimp_gegl_convolve(…, GIMP_NORMAL_CONVOL, alpha-weighted)`; `gimp_brush_core_replace_canvas(…, GIMP_PAINT_INCREMENTAL)`. |
| **Dodge/Burn** | `gimp_gegl_dodgeburn(src, …, exposure/100, type, mode)` (`gimp-gegl-loops.cc`), per RGB channel, alpha copied; `e = exposure` (negated for BURN): HIGHLIGHTS `factor = 1 + e·0.333333; out = s·factor`; MIDTONES `factor = (e < 0) ? 1 − e·0.333333 : 1/(1 + e); out = odd_powf(s, factor)` (`odd_powf(x,y) = x >= 0 ? powf(x,y) : −powf(−x,y)`); SHADOWS `e >= 0: factor = 0.333333·e; out = factor + s − factor·s`; `e < 0: factor = −0.333333·e; out = (s < factor) ? 0 : (s − factor)/(1 − factor)`. Source = pre-stroke image in CONSTANT mode, else current. `gimp_brush_core_replace_canvas`. |
| **Ink** | No brush. Each event → a convex polygon "blob" (`gimp_blob_ellipse/square/diamond`, `gimpink-blob.c`) in 8× sub-pixel coordinates (`SUBSAMPLE 8`): `size = options.size·(1 + size_sensitivity·(2·pressure − 1))`; `velocity = MAX(velocity, 3)`; `size = vel_sens·((4.5·size)/(1 + vel_sens·2·velocity)) + (1 − vel_sens)·size`; clamp to `options.size·(1 + size_sens)` and ≥ 1/8; tilt: `tscale = tilt_sens·10`, `x = blob_aspect·cos(blob_angle) + xtilt·tscale·cos(tilt_angle) − ytilt·tscale·sin(tilt_angle)`, `y = blob_aspect·sin(blob_angle) + ytilt·tscale·cos(tilt_angle) + xtilt·tscale·sin(tilt_angle)`, `aspect = CLAMP(hypot(x,y), 1, 10)`, `radmin = MAX(1, 8·size/aspect)`. Consecutive blobs are joined by `gimp_blob_convex_union(last, cur)`, scan-converted with 8× oversampling straight into `canvas_buffer` (coverage 0..1) and pasted with `gimp_paint_core_paste(core, NULL /*mask already in canvas*/, …, GIMP_PAINT_CONSTANT)` using the foreground colour. Own undo (`GIMP_UNDO_INK`). |
| **MyPaint brush** | Wraps libmypaint: `mypaint_brush_new_with_buckets(64)`, `mypaint_brush_from_string(brush, json)`, base values overridden from options (radius log, opaque, hardness, eraser, pigment, posterize, gain, view zoom/rotation…), a `GimpMybrushSurface` (`MyPaintSurface2` implemented over a GeglBuffer: `draw_dab`, `get_color`, begin/end atomic) and `mypaint_brush_stroke_to_2(brush, surface, x, y, pressure, xtilt, ytilt, dtime, viewzoom, viewrotation, barrel_rotation, linear)`. One brush instance per symmetry stroke. Requires libmypaint + `mypaint-brushes` data (not in this checkout). |

### 3.9 Brush, pattern, gradient, palette file formats

All multi-byte header fields are **big-endian** ("All field entries are MSB").

#### 3.9.1 `.gbr` — bitmap brush (`app/core/gimpbrush-header.h`, `gimpbrush-load.c`)

```c
struct GimpBrushHeader {      // 28 bytes (version 2)
  guint32 header_size;        // = 28 + strlen(name) + 1
  guint32 version;            // 1, 2 or 3
  guint32 width, height;      // 1 … GIMP_BRUSH_MAX_SIZE (10000)
  guint32 bytes;              // 1 = gray mask, 4 = RGBA pixmap  (2 only for version 3 after remap, see below)
  guint32 magic_number;       // 'G','I','M','P' = 0x47494D50   (version ≥ 2 only)
  guint32 spacing;            // percent of brush width           (version ≥ 2 only)
};
char name[header_size - 28];  // NUL-terminated UTF-8, max GIMP_BRUSH_MAX_NAME 256
uint8 data[width * height * bytes];
```
- **Version 1**: the header is only 20 bytes (no magic, no spacing): the loader reads 28, then seeks back 8 and adds 8 to `header_size`; spacing defaults to **25**.
- **Version 3** (CinePaint): `bytes` must be 18 (`FLOAT16_GRAY_GIMAGE`) → treated as 2 bytes/pixel big-endian half-float-like 16-bit values, converted with `(guchar)(f * 255.0 + 0.5)`; magic must match.
- `bytes == 1`: mask `"Y u8"`, value = coverage (255 = full paint). **Legacy `.gpb` trick**: if the mask is immediately followed by a valid pattern header (`GPAT`, version 1, `bytes == 3`, same size), that RGB block becomes the brush pixmap.
- `bytes == 4`: stored RGBA; split into pixmap `"R'G'B' u8"` (3 bytes) + mask = alpha byte.
- After load: `spacing = header.spacing`, `x_axis = (width/2.0, 0)`, `y_axis = (0, height/2.0)`. MIME `image/x-gimp-gbr`.

#### 3.9.2 `.gih` — brush pipe / image hose (`gimpbrushpipe-load.c`, `gimpbrushpipe.c`, `libgimpbase/gimpparasiteio.c`)

Text header then concatenated `.gbr` blobs:
```
<name>\n                                  (≤ 1023 bytes)
<n_brushes> <param string>\n              e.g. "8 ncells:8 cellwidth:64 cellheight:64 step:20 dim:1 cols:8 rows:1 placement:constant rank0:8 sel0:angular"
<gbr #1><gbr #2>…<gbr #n_brushes>         (each a complete version-2 GBR including its own header and name)
```
Parameter tokens are separated by space/CR/LF, each `key:value` (`gimp_pixpipe_params_parse`): `ncells`, `step`, `dim` (clamped 1…`GIMP_PIXPIPE_MAXDIM` 4), `cols`, `rows`, `cellwidth`, `cellheight`, `placement`, `rank<i>`, `sel<i>` (i < dim). Only `dim`, `rank[]`, `sel[]` are used by the core ("placement is not used at all"). Empty param string → `dim = 1, rank[0] = n_brushes, select[0] = incremental`.
`stride[0] = Π rank / rank[0]`, `stride[i] = stride[i-1] / rank[i]`; load fails ("Inconsistent parameters") unless `stride[dim-1] == 1`.

Selection per dab (`gimp_brush_pipe_select_brush(brush, last_coords, current_coords)`; `PipeSelectModes`: `CONSTANT, INCREMENTAL, ANGULAR, VELOCITY, RANDOM, PRESSURE, TILT_X, TILT_Y`):

| `sel<i>` string | Index `ix` for dimension i |
|---|---|
| `incremental` | `(index[i] + 1) % rank[i]` |
| `angular` | `(int) RINT((1.0 - direction + 0.25) * rank[i]) % rank[i]` |
| `velocity` | `ROUND(velocity * rank[i])` |
| `random` | `g_random_int_range(0, rank[i])` |
| `pressure` | `RINT(pressure * (rank[i] - 1))` |
| `xtilt` | `RINT(xtilt / 2.0 * rank[i]) + rank[i] / 2` |
| `ytilt` | `RINT(ytilt / 2.0 * rank[i]) + rank[i] / 2` |
| anything else | `index[i]` (constant) |

then `index[i] = CLAMP(ix, 0, rank[i]-1)`, `brushix = Σ stride[i] * index[i]`, clamped to `[0, n_brushes-1]`.

#### 3.9.3 `.vbr` — generated (parametric) brush (`gimpbrushgenerated-load.c`, `gimpbrushgenerated.c`)

Plain text, one value per line:
```
GIMP-VBR
1.0            | 1.5
<name>
[<shape>]      (1.5 only: enum nick of GimpBrushGeneratedShape: "circle" | "square" | "diamond")
<spacing>      (percent, double, C locale)
<radius>       (px; GIMP_BRUSH_GENERATED_MIN_RADIUS … MAX_RADIUS)
[<spikes>]     (1.5 only: int 2 … 20)
<hardness>     (0 … 1)
<aspect_ratio> (1 … 20)
<angle>        (degrees 0 … 180)
```
Defaults for 1.0: shape circle, spikes 2. Property defaults in the class: spikes 2, hardness 0.0, aspect 1.0, angle 0.0.

Rasterisation (`gimp_brush_generated_calc`, `OVERSAMPLING 4`), output `"Y float"`:
```c
// size (gimp_brush_generated_get_size):
angle_deg = RINT(angle_deg * 1000) / 1000; s = sin(rad); c = cos(rad); if (reflect) c = -c;
short_radius = radius / aspect_ratio;
x_axis = ( c*radius, -s*radius );  y_axis = ( s*short_radius, c*short_radius );
circle : half_w = sqrt(x_axis.x² + y_axis.x²);          half_h = sqrt(x_axis.y² + y_axis.y²);
square : half_w = |x_axis.x| + |y_axis.x|;               half_h = |x_axis.y| + |y_axis.y|;
diamond: half_w = MAX(|x_axis.x|, |y_axis.x|);           half_h = MAX(|x_axis.y|, |y_axis.y|);
if (spikes > 2) { half_w = half_h = sqrt(radius² + short_radius²); y_axis = (s*radius, c*radius); }
w = MAX(1, ceil(half_w*2)); h = MAX(1, ceil(half_h*2)); make both odd (w++ / h++ if even);

// profile lookup table (gimp_brush_generated_calc_lut):
length = OVERSAMPLING * ceil(1 + sqrt(2 * SQR(ceil(radius + 1.0))));
exponent = ((1.0 - hardness) < 0.0000004) ? 1000000.0 : 0.4 / (1.0 - hardness);
gauss(f): if (f < -0.5) { f = -1 - f; return 2*f*f; }  if (f < 0.5) return 1 - 2*f*f;  f = 1 - f; return 2*f*f;
for (x = 0; x < OVERSAMPLING; x++) { d = fabs((x + 0.5)/OVERSAMPLING - 0.5);
     buffer[x] = (d > radius) ? 0 : gauss(pow(d/radius, exponent)); sum += buffer[x]; }
for (x = 0; d < radius || sum > 0.00001; d += 1.0/OVERSAMPLING) {     // running box average of 4 samples
     sum -= buffer[x % OVERSAMPLING];
     buffer[x % OVERSAMPLING] = (d > radius) ? 0 : gauss(pow(d/radius, exponent));
     sum += buffer[x % OVERSAMPLING];
     lookup[x++] = sum / OVERSAMPLING; }
while (x < length) lookup[x++] = 0;

// pixels, centre at (w/2, h/2) integer division:
cs = cos(-2π/spikes); ss = sin(-2π/spikes);
for (y = (spikes % 2 ? -half_h : 0); y <= half_h; y++) for (x = -half_w; x <= half_w; x++) {
    tx = c*x - s*y;  ty = fabs(s*x + c*y);
    if (spikes > 2) { ang = atan2(ty, tx);
         while (ang > π/spikes) { (tx, ty) = (cs*tx - ss*ty, ss*tx + cs*ty); ang -= 2π/spikes; } }
    ty *= aspect_ratio;
    d = circle: sqrt(tx² + ty²) | square: MAX(|tx|, |ty|) | diamond: |tx| + |ty|;
    a = (d < radius + 1) ? lookup[(int) RINT(d * OVERSAMPLING)] : 0;
    center[y*w + x] = a;  if (spikes % 2 == 0) center[-y*w - x] = a;     // point-mirror the other half
}
```
When painting, `gimp_brush_generated_transform_mask(brush, scale, aspect_ratio, angle, reflect, hardness)` re-rasterises with `radius*scale`, `ratio = MIN(fabs(aspect_ratio) * 19.0/20.0 + 1.0, 20)`, `if (aspect_ratio < 0) angle += 0.25; angle = fmod(fmod(angle,1)+1,1) * 360` (angle arrives in turns), and the tool hardness (the tool-option values replace the brush's own aspect/angle/hardness).

#### 3.9.4 MyPaint brushes (`gimpmybrush-load.c`)

`.myb` = MyPaint brush JSON (version 3) or the old v2 text format; loaded with `mypaint_brush_from_string`; preview from the sibling `<name>_prev.png`. Only `radius_logarithmic`, `opaque`, `hardness`, `eraser`, `offset_by_random`, pigment/posterize/gain base values are surfaced to options; everything else is interpreted by libmypaint.

#### 3.9.5 `.pat` — pattern (`gimppattern-header.h`, `gimppattern-load.c`)

```c
struct GimpPatternHeader {    // 24 bytes
  guint32 header_size;        // = 24 + strlen(name) + 1   (must be > 24)
  guint32 version;            // must be 1
  guint32 width, height;      // 1 … GIMP_PATTERN_MAX_SIZE (10000)
  guint32 bytes;              // 1 "Y' u8", 2 "Y'A u8", 3 "R'G'B' u8", 4 "R'G'B'A u8"
  guint32 magic_number;       // 'G','P','A','T' = 0x47504154
};
char name[header_size - 24];  // NUL-terminated, max GIMP_PATTERN_MAX_NAME 256
uint8 data[width * height * bytes];   // row-major, interleaved
```
If the first 4 bytes are `"8BPT"` the file is a Photoshop `.pat` and is routed to `gimp_pattern_load_photoshop_pattern`. Patterns can also be any GdkPixbuf-loadable image dropped in the patterns folder (`gimp_pattern_load_pixbuf`).

#### 3.9.6 `.ggr` — gradient (`gimpgradient-load.c`, `gimpgradient.c`)

```
GIMP Gradient
Name: <utf-8 name>                 (optional line; old files omit it and use the file name)
<num_segments>                     (≥ 1)
<left> <middle> <right> <r0> <g0> <b0> <a0> <r1> <g1> <b1> <a1> <type> <color> [<left_color_type> <right_color_type>]
… one line per segment …
```
All numbers are C-locale doubles 0..1; colours are **non-linear sRGB** (`"R'G'B'A double"`). Constraints: `left <= middle <= right`, first `left == 0.0`, each `left == previous right`, last `right == 1.0`. The last two integers are optional (2 or 4 ints accepted).

| Field | Enum | Values |
|---|---|---|
| `type` | `GimpGradientSegmentType` | 0 `LINEAR`, 1 `CURVED`, 2 `SINE` "Sinusoidal", 3 `SPHERE_INCREASING`, 4 `SPHERE_DECREASING`, 5 `STEP` |
| `color` | `GimpGradientSegmentColor` | 0 `RGB`, 1 `HSV_CCW`, 2 `HSV_CW` |
| `left/right_color_type` | `GimpGradientColor` | 0 `FIXED`, 1 `FOREGROUND`, 2 `FOREGROUND_TRANSPARENT`, 3 `BACKGROUND`, 4 `BACKGROUND_TRANSPARENT` (non-fixed endpoints are resolved against the context at evaluation time: `gimp_gradient_segment_get_left/right_flat_color`) |

Evaluation (`gimp_gradient_get_color_at(gradient, context, seg, pos, reverse, GimpGradientBlendColorSpace, &color)`; `EPSILON 1e-10`):
```c
pos = CLAMP(pos, 0, 1); if (reverse) pos = 1 - pos;  seg = segment containing pos (cached search from seg);
seg_len = right - left;
if (seg_len < EPSILON) { middle = 0.5; pos = 0.5; } else { middle = (seg.middle - left)/seg_len; pos = (pos - left)/seg_len; }
factor:
  LINEAR : pos <= middle ? (middle < EPSILON ? 0.0 : 0.5*pos/middle)
                         : (pos -= middle, middle = 1 - middle, middle < EPSILON ? 1.0 : 0.5 + 0.5*pos/middle)
  CURVED : middle < EPSILON ? 1.0 : (1 - middle < EPSILON ? 0.0 : exp(-G_LN2 * log(pos) / log(middle)))
  SINE   : p = LINEAR(middle,pos); (sin(-π/2 + π*p) + 1)/2
  SPHERE_INCREASING: p = LINEAR(middle,pos) - 1; sqrt(1 - p*p)
  SPHERE_DECREASING: p = LINEAR(middle,pos);     1 - sqrt(1 - p*p)
  STEP   : pos >= middle  (0 or 1)
colour:
  RGB segments: component-wise lerp left + (right - left)*factor in the blend colour space:
       GIMP_GRADIENT_BLEND_RGB_PERCEPTUAL (0): "R'G'B' double";  RGB_LINEAR (1): "RGB double";  CIE_LAB (2): "CIE Lab float"
  HSV_CCW: in "HSV float" (h,s,v ∈ 0..1): s,v lerp; if (lh < rh) lh += (rh - lh)*factor; else { lh += (1 - (lh - rh))*factor; if (lh > 1) lh -= 1; }
  HSV_CW : s,v lerp; if (rh < lh) lh -= (lh - rh)*factor; else { lh -= (1 - (rh - lh))*factor; if (lh < 0) lh += 1; }
  alpha = left_alpha + (right_alpha - left_alpha)*factor
```
**Upstream quirk to be aware of** (`gimpgradient.c:577-581`): the `GIMP_GRADIENT_BLEND_RGB_LINEAR` case that fetches the endpoints has **no `break`** and falls through into the `RGB_PERCEPTUAL` fetch, so the endpoints are read as `R'G'B'` but the interpolated result is then *stored* as linear `"RGB double"`. A faithful port reproduces a visibly different result from a "correct" linear blend; decide explicitly (recommended: implement the intended behaviour — fetch linear, lerp, store linear — and note the deviation).

#### 3.9.7 `.gpl` — palette (`gimppalette-load.c`)

```
GIMP Palette
Name: <name>            (optional; if absent the file name is used and the line is already a colour/comment)
Columns: <0…256>        (optional, only directly after Name:; out of range → 0)
# comment lines start with '#'; empty lines ignored
<R> <G> <B>[ \t<entry name>]      decimal 0…255 each, separated by spaces/tabs; values clamped to 0…255
```
Colours are 8-bit non-linear sRGB (`"R'G'B' u8"`). Other palette importers exist (`gimp_palette_load_act`, `_riff`, `_psp`, `_aco`, `_acb`, `_ase`, `_css`, `_sbz`, `_procreate`, chosen by `gimp_palette_load_detect_format` — see `gimppalette-load.h`); only `.gpl` is written (`gimppalette-save.c`).

#### 3.9.8 Tool presets `.gtp` and others

`.gtp` (`app/core/gimptoolpreset*.c`) = GimpConfig s-expression, e.g. `data/tool-presets/Paint/Airbrush.gtp`: `(icon-name "gimp-tool-airbrush") (name "Airbrush") (tool-options "GimpAirbrushOptions" (tool "gimp-airbrush-tool") (foreground (color-rgb 0.0 0.023605 0.142004)) (brush "2. Hardness 050") (dynamics "Pressure Opacity") (gradient "Tropical Colors") (brush-size 500.0) (rate 30.0))` followed by the apply-flags `(use-fg-bg yes|no) (use-opacity-paint-mode …) (use-brush …) (use-dynamics …) (use-mybrush …) (use-gradient …) (use-pattern …) (use-palette …) (use-font …)`. Tags for all data live in `data/tags/gimp-tags-default.xml.in`.

### 3.10 Bundled data in this checkout

| Kind | Location | Count here | Notes |
|---|---|---|---|
| Brushes (`.gbr/.gih/.vbr`) | `gimp-data/brushes` | **0 — submodule not populated** | must come from a GIMP install or the `gimp-data` repo (`https://gitlab.gnome.org/GNOME/gimp-data.git`, pinned commit `fe4ecc0bf70fc8ff3bd929ce6abdd50fb5621072`) |
| Patterns (`.pat`) | `gimp-data/patterns` | **0** | same |
| MyPaint brushes | external `mypaint-brushes` package | 0 | not part of GIMP |
| Gradients (`.ggr`) | `data/gradients` | 78 | e.g. `Default.ggr` (1 segment black→white), `Full_saturation_spectrum_CCW/CW`, flags, `Flare_*`, `Radial_*` … |
| Palettes (`.gpl`) | `data/palettes` | 40 | `Default.gpl`, `Web.gpl`, `Visibone.gpl`, `Tango.gpl`, `Named_Colors.gpl`, `Ega.gpl` … |
| Dynamics (`.gdyn`) | `data/dynamics` | 18 | §3.5 |
| Tool presets (`.gtp`) | `data/tool-presets` | 39 | `Crop` 7, `FX` 8, `Paint` 16, `Selection` 2, `Sketch` 6 |
| Tags | `data/tags` | 1 XML + DTD | |

Licences: the in-tree `data/` files carry no per-file licence header; they are covered by the repository's top-level `COPYING` (GPL-3.0-or-later) / `LICENSE` — fine for Tetra. The licence terms of the `gimp-data` assets (brushes/patterns/images) **could not be determined from this checkout** because the submodule is empty; check that repository's own LICENSE files before bundling anything from it. Recommendation: ship generated brushes (`.vbr`-style parametric round/square/diamond, which need no assets) as Tetra's defaults and add an importer for user-supplied `.gbr/.gih/.pat`.

---

## 4. Selections and fills

### 4.1 Model

- The selection is one `GimpSelection` (subclass of `GimpChannel`) per image, **image-sized**, offset (0,0), single component in the image's precision (`gimp_image_get_mask_format`, e.g. `"Y u8"` for an 8-bit image, `"Y float"` for float). All selection algorithms compute in `"Y float"` 0..1 and write through babl, so an 8-bit image quantises selection values to 0..255 — a port should keep a float (or at least 8-bit) mask per image and treat 0 = unselected, 1 = fully selected.
- `GimpChannel` caches `empty`, `full`, bounds `x1,y1,x2,y2` (`gimp_channel_bounds` → `gimp_gegl_mask_bounds`) and the outline segments for marching ants (`gimp_channel_boundary` → `gimp_boundary_find(buffer, region, format, GIMP_BOUNDARY_WITHIN_BOUNDS|IGNORE_BOUNDS, x1, y1, x2, y2, threshold = 0.5 (GIMP_BOUNDARY_HALF_WAY), &n_segs)`; `app/core/gimpboundary.c`: `gimp_boundary_find/_sort/_simplify/_offset`).
- An **empty** selection means "everything editable" for paint/fill: `gimp_item_mask_bounds` / `gimp_item_mask_intersect(item, &x, &y, &w, &h)` return the drawable ∩ selection bounds, or the full drawable when the selection is empty. Paint/fill multiply their coverage by the selection value at each pixel (selection is sampled at `drawable offset + pixel`).
- `GimpSelection` overrides the `GimpChannel` vfuncs to add image-level behaviour (e.g. shrinking/growing pushes `GIMP_UNDO_MASK`; moving the selection with content floats it). Saved channels are plain `GimpChannel`s in `image->channels`.

### 4.2 Combine operations

`GimpChannelOps` (`libgimpbase`): `GIMP_CHANNEL_OP_ADD 0`, `GIMP_CHANNEL_OP_SUBTRACT 1`, `GIMP_CHANNEL_OP_REPLACE 2`, `GIMP_CHANNEL_OP_INTERSECT 3`.

Per-pixel rules (`app/gegl/gimp-gegl-mask-combine.cc`; `p` = existing mask value, `v` = new shape/add-on value, both float 0..1):

| Op | Shape rendering (`gimp_gegl_mask_combine_rect/_ellipse/_ellipse_rect`) | Buffer combine (`gimp_gegl_mask_combine_buffer`) |
|---|---|---|
| REPLACE | mask cleared first (`gimp_channel_combine_clear`), then `p = v` | `p = v` inside the add-on rect, cleared outside |
| ADD | `p = MIN(p + v, 1.0)` | `CLAMP(p + v, 0, 1)` |
| SUBTRACT | `p = MAX(p - v, 0.0)` | `CLAMP(p - v, 0, 1)` |
| INTERSECT | `p = MIN(p, v)`; everything outside the shape's bounding rect is cleared (`gimp_channel_combine_clear_complement`) | `MIN(p, v)`, complement cleared |

`gimp_channel_combine_*` wrappers (`gimpchannel-combine.c`): `_rect`, `_ellipse`, `_ellipse_rect` (rounded rectangle with corner radii `rx, ry`, clamped to `w/2, h/2`), `_mask`, `_buffer`, `_items`; each brackets the edit with `gimp_channel_combine_start/_end` which maintain the cached bounds/empty flags cheaply.

Ellipse / rounded-rect anti-aliasing (`gimp_gegl_mask_combine_ellipse_rect`): for each boundary pixel the signed distance `d` from the pixel centre to the ellipse edge is approximated analytically (solving the quadratic along the gradient direction, normalised by `sqrt(SQR(MIN(du/dv, dv/du)) + 1)`), coverage = `CLAMP(0.5 + d, 0, 1)`; interior spans are filled with 1. Without antialias the test is `d >= 0`.

### 4.3 Selection commands (`gimpchannel.c` real implementations)

| Command | Function | Implementation |
|---|---|---|
| All | `gimp_channel_all` | fill with 1.0; `empty = FALSE`, bounds = whole image |
| None | `gimp_channel_clear` | clear; `empty = TRUE` |
| Invert | `gimp_channel_invert` | `gegl:invert-linear` (`1 - p`); an empty mask becomes "all" directly |
| Feather | `gimp_channel_feather(channel, radius_x, radius_y, edge_lock, push_undo)` | `gimp_gegl_apply_feather` = `gegl:gaussian-blur` with **`std-dev = radius / 3.5`** per axis ("3.5 is completely magic"), abyss policy CLAMP when `edge_lock` else NONE; region grown by `ceil(radius)` |
| Sharpen | `gimp_channel_sharpen` | `gegl:threshold` at **0.5** (→ 0 or 1) |
| Grow | `gimp_channel_grow(channel, radius_x, radius_y, push_undo)` | `gimp:grow` (`app/operations/gimpoperationgrow.c`): elliptical max filter (sliding-window maxima with a precomputed circle/ellipse border table `compute_border`); negative radii → shrink |
| Shrink | `gimp_channel_shrink(channel, radius_x, radius_y, edge_lock, push_undo)` | `gimp:shrink` (`gimpoperationshrink.c`): elliptical min filter; `edge_lock` = treat outside-image as selected |
| Border | `gimp_channel_border(channel, radius_x, radius_y, GimpChannelBorderStyle, edge_lock, push_undo)` | `HARD`/`FEATHERED` → `gimp:border` (`gimpoperationborder.c`) with `feather` flag; `SMOOTH` → graph `grow(radius) − shrink(radius)` built from `gimp:grow`, `gimp:shrink`, `gegl:subtract` (when a radius is 1 the code substitutes `gegl:nop`/1-px ops); both radii must be > 0 |
| Remove holes ("flood") | `gimp_channel_flood` | `gimp:flood` (`gimpoperationflood.c`, "See https://wiki.gimp.org/wiki/Algorithms:Flood"): morphological flooding — treats the mask as a height map and fills every basin not connected to the image border, i.e. removes interior holes |
| From path | `gimp_channel_select_path` | scan-convert the path (§4.8) then combine |
| From channel / alpha / component | `gimp_channel_select_channel`, `gimp_channel_select_alpha`, `gimp_channel_select_component(channel, GimpChannelType, op, feather, rx, ry)`, `gimp_channel_select_by_index` | extract plane as `"Y float"`/`"A float"`, optional feather, combine |

### 4.4 Shape selections (`gimpchannel-select.c`)

All have the tail `(GimpChannelOps op, gboolean antialias?, gboolean feather, gdouble feather_radius_x, gdouble feather_radius_y, gboolean push_undo)`. When `feather` is TRUE the shape is rendered into a fresh image-sized `"Y float"` add-on buffer with REPLACE, blurred with `gimp_gegl_apply_feather(…, edge_lock = TRUE)`, then merged with `gimp_channel_combine_buffer(channel, add_on, op, 0, 0)`; otherwise the shape is combined directly.

| Function | Shape |
|---|---|
| `gimp_channel_select_rectangle(channel, x, y, w, h, op, feather, rx, ry, push_undo)` | axis-aligned rectangle (no antialias) |
| `gimp_channel_select_ellipse(channel, x, y, w, h, op, antialias, feather, rx, ry, push_undo)` | ellipse inscribed in the rectangle |
| `gimp_channel_select_round_rect(channel, x, y, w, h, corner_radius_x, corner_radius_y, op, antialias, feather, rx, ry, push_undo)` | rounded rectangle |
| `gimp_channel_select_polygon(channel, undo_desc, n_points, GimpVector2 *points, op, antialias, feather, rx, ry, push_undo)` | free/polygon select: points → `GimpScanConvert` polyline (closed) → `gimp_channel_select_scan_convert` |
| `gimp_channel_select_scan_convert(channel, undo_desc, scan_convert, offset_x, offset_y, op, antialias, feather, rx, ry, push_undo)` | renders any scan-convert object (cairo fill, even-odd handled by cairo path) into an add-on buffer |
| `gimp_channel_select_buffer(channel, undo_desc, add_on, off_x, off_y, op, feather, rx, ry)` | generic "combine this mask buffer" |

`GimpScanConvert` (`app/core/gimpscanconvert.c`) is a thin wrapper around **cairo** path filling/stroking into an A8 surface: `gimp_scan_convert_new`, `_add_polyline(sc, n_points, points, closed)`, `_add_bezier`, `_stroke(sc, width, GimpJoinStyle, GimpCapStyle, miter, dash_offset, dash_info)`, `_set_pixel_ratio`, `_set_clip_rectangle`, `_render(sc, buffer, off_x, off_y, antialias)`, `_render_value`, `_compose`, `_compose_value`, `_render_full`, `_new_from_boundary`. The port needs its own polygon/stroke rasteriser here (Godot has none exposed for float masks; a small scanline AA polygon filler + stroker is enough).

### 4.5 Fuzzy select, select-by-colour, foreground select, scissors

**API** (`app/core/gimppickable-contiguous-region.cc`):
```c
GeglBuffer * gimp_pickable_contiguous_region_by_seed  (GimpPickable *pickable, gboolean antialias, gfloat threshold,
                                                       gboolean select_transparent, GimpSelectCriterion select_criterion,
                                                       gboolean diagonal_neighbors, gint x, gint y);
GeglBuffer * gimp_pickable_contiguous_region_by_color (GimpPickable *pickable, gboolean antialias, gfloat threshold,
                                                       gboolean select_transparent, GimpSelectCriterion select_criterion,
                                                       GeglColor *color, const GeglRectangle *roi);
GeglBuffer * gimp_pickable_contiguous_region_by_line_art (GimpPickable *pickable, GimpLineArt *line_art,
                                                       GeglBuffer *fill_buffer, const Babl *fill_format, gconstpointer fill_color,
                                                       gfloat fill_threshold, gint fill_offset_x, gint fill_offset_y,
                                                       gint x, gint y);
```
Both return a `"Y float"` mask the size of the pickable. Callers: `gimp_channel_select_fuzzy(channel, drawable, sample_merged, x, y, threshold, select_transparent, select_criterion, diagonal_neighbors, op, antialias, feather, rx, ry)` and `gimp_channel_select_by_color(channel, drawables, sample_merged, color, threshold, select_transparent, select_criterion, op, antialias, feather, rx, ry)`. **Threshold semantics:** the tool option is 0..255 and is passed as `options->threshold / 255.0` (`app/tools/gimpfuzzyselecttool.c:145`, `gimpbycolorselecttool.c:157`, `gimpbucketfilltool.c:436`); core functions take 0..1. `sample_merged` → pickable = the image projection (coordinates in image space), else the drawable (drawable space; result offset by the layer offset). The source is `gimp_pickable_get_buffer_with_effects` (NDE filters applied).

**Comparison format** (`choose_format`) and `GimpSelectCriterion`:

| Criterion | Value | Pixels are compared in babl format | Distance `max` |
|---|---|---|---|
| `GIMP_SELECT_CRITERION_COMPOSITE` | 0 | `R'G'B'A float` (indexed → `R'G'B'A float`; gray → `Y'(A) float`): the image's base type at `GIMP_PRECISION_FLOAT_NON_LINEAR` | `max_b |c1[b] - c2[b]|` over colour components (alpha excluded) |
| `…_RGB_RED` / `_RGB_GREEN` / `_RGB_BLUE` | 1/2/3 | `R'G'B'A float` | `|c1[k] - c2[k]|`, k = 0/1/2 |
| `…_HSV_HUE` | 4 | `HSVA float` (h,s,v ∈ 0..1) | if both saturations `> EPSILON (1e-6)`: `d = |h1 - h2|; MIN(d, 1 - d)`; exactly one saturated: `10.0` ("infinite"); neither: `0` |
| `…_HSV_SATURATION` / `_HSV_VALUE` | 5/6 | `HSVA float` | `|s1 - s2|` / `|v1 - v2|` |
| `…_LCH_LIGHTNESS` | 7 | `CIE L alpha float` | `|L1 - L2| / 100` |
| `…_LCH_CHROMA` | 8 | `CIE LCH(ab) alpha float` | `|C1 - C2| / 100` |
| `…_LCH_HUE` | 9 | `CIE LCH(ab) alpha float` | if both chroma `> 100·EPSILON`: `d = |H1 - H2| / 360; MIN(d, 1 - d)`; one: `10.0`; neither: `0` |
| `…_ALPHA` | 10 | `R'G'B'A float` | `|a1 - a2|` |

**`pixel_difference(col1 /*reference*/, col2 /*candidate*/, antialias, threshold, n_components, has_alpha, select_transparent, criterion)`** returns the *selection value* (0..1), exactly:
```c
if (!select_transparent && has_alpha && col2[alpha] == 0.0) return 0.0;          // never select fully transparent pixels
if (select_transparent && has_alpha) max = fabs(col1[alpha] - col2[alpha]);      // compare alpha only
else max = <criterion distance above>;
if (antialias && threshold > 0.0) { aa = 1.5 - (max / threshold);
     return (aa <= 0.0) ? 0.0 : (aa < 0.5) ? aa * 2.0 : 1.0; }
else return (max > threshold) ? 0.0 : 1.0;
```
(antialias: full selection up to `max = threshold`, linear ramp to 0 at `max = 1.5·threshold`.) `select_transparent` is forced FALSE unless the seed pixel / reference colour is fully transparent (`alpha == 0`). `by_color` adds `threshold += EPSILON` first and evaluates every pixel independently (parallel over the ROI).

**Flood fill** (`find_contiguous_region`): scanline seed fill with a FIFO queue of segments `(y, old_y, start, end)`. `find_contiguous_segment` checks the seed pixel (`diff == 0` → reject), then extends left and right while `pixel_difference != 0`, writing the returned values into the mask row; for each found segment `(start, end)` (exclusive ends) it pushes the rows `y − 1` and `y + 1` (if inside the extent) for re-scanning over `x ∈ (start, end)`, skipping pixels already non-zero in the mask; with `diagonal_neighbors` the pushed range is widened by one pixel on each side (`new_start = start − 1`, `new_end = end + 1`, clamped to the extent). The reference colour is always the **seed** pixel (not the running neighbour).

**Line-art fill** (`gimp_pickable_contiguous_region_by_line_art`, `app/core/gimplineart.c`): `GimpLineArt` asynchronously computes a closed line-art mask from a source (properties: `select-transparent`, `threshold`, `max-grow`, `automatic-closure`, `spline-max-length`, `segment-max-length`; signals `computing-start`/`computing-end`; it thresholds strokes, detects end points/curvature, closes gaps with splines and straight segments, and computes a distance map); the fill floods the closed regions then grows into the line pixels up to `max-grow`. This is a large algorithm (≈ 3k lines; based on Fourey et al., "A Fast and Efficient Semi-guided Algorithm for Flat Coloring Line-arts") → **bridge/defer**, not first-wave native.

**Foreground select** (`gimp_drawable_foreground_extract`, `gimpdrawable-foreground-extract.c`): builds a graph `buffer-source(drawable) + buffer-source(trimap)` → `gegl:matting-global` (`iterations = global_iterations`) or `gegl:matting-levin` (`levels`, `active_levels`) → sink; `GimpMattingEngine`: `GIMP_MATTING_ENGINE_GLOBAL`, `GIMP_MATTING_ENGINE_LEVIN`. The trimap is painted by the tool (foreground / background / unknown). → GEGL bridge.

**Scissors (intelligent scissors)** is implemented entirely in the tool layer: `app/tools/gimpiscissorstool.c` + `gimptilehandleriscissors.c` (gradient-magnitude/direction cost map tiles) with a dynamic-programming shortest path between clicked nodes; the result is a polyline converted with `GimpScanConvert` → `gimp_channel_select_scan_convert`. No `app/core` API; port = reimplement live-wire (Mortensen & Barrett) or defer.

Other selection producers: rectangle/ellipse tools (`gimprectangleselecttool.c`, options incl. `round-corners`, `corner-radius`, `fixed-rule`/`fixed-rule-active`, `shrink-merged`, `highlight`, `highlight-opacity` in `gimprectangleoptions.c`), free select (`gimpfreeselecttool.c` → polygon), paths → selection, Quick Mask, "by colour" tool, `gimp_item_to_selection` for alpha/mask/channel.

### 4.6 Bucket fill

Tool options (`app/tools/gimpbucketfilloptions.c`, property names verbatim): `fill-mode` (`GimpBucketFillMode`: `GIMP_BUCKET_FILL_FG`, `_BG`, `_PATTERN`), `fill-area` (`GimpBucketFillArea`: `GIMP_BUCKET_FILL_SELECTION` "Fill whole selection", `GIMP_BUCKET_FILL_SIMILAR_COLORS`, `GIMP_BUCKET_FILL_LINE_ART`), `fill-transparent`, `sample-merged`, `diagonal-neighbors`, `antialias`, `feather`, `feather-radius`, `threshold` (0..255), `fill-criterion` (`GimpSelectCriterion`); line-art: `line-art-source` (`GimpLineArtSource`: `SAMPLE_MERGED`, `ACTIVE_LAYER`, `LOWER_LAYER`, `UPPER_LAYER`), `fill-color-as-line-art`, `fill-color-as-line-art-threshold`, `line-art-threshold`, `line-art-max-grow`, `line-art-stroke-border`, `line-art-stroke-tool`, `line-art-automatic-closure`, `line-art-max-gap-length`; plus context opacity and paint mode.

Core (`gimpdrawable-bucket-fill.c`):
```c
void gimp_drawable_bucket_fill (GimpDrawable *drawable, GimpFillOptions *options, gboolean fill_transparent,
                                GimpSelectCriterion fill_criterion, gdouble threshold, gboolean sample_merged,
                                gboolean diagonal_neighbors, gdouble seed_x, gdouble seed_y);
GeglBuffer * gimp_drawable_get_bucket_fill_buffer (… same params …, GeglBuffer **mask_buffer, gdouble *mask_x, gdouble *mask_y,
                                gint *mask_offset_x, gint *mask_offset_y, gint *mask_width, gint *mask_height);
```
Algorithm: (1) `new_mask = gimp_pickable_contiguous_region_by_seed(pickable, antialias, threshold, fill_transparent, criterion, diagonal_neighbors, x, y)`; (2) intersect its bounds with the selection bounds (bail if disjoint), translating when sample-merged; (3) optional feather of the mask (`gimp_fill_options` `feather`/`feather-radius`); (4) `buffer = gimp_fill_options_create_buffer(options, drawable, rect, pattern offsets)` — solid colour or tiled pattern; (5) `gimp_gegl_apply_opacity(buffer, mask)` (multiply alpha by the mask); (6) `gimp_drawable_apply_buffer(drawable, buffer, GEGL_RECTANGLE(0,0,w,h), TRUE, "Bucket Fill", gimp_context_get_opacity(options), gimp_context_get_paint_mode(options), GIMP_LAYER_COLOR_SPACE_AUTO, GIMP_LAYER_COLOR_SPACE_AUTO, gimp_layer_mode_get_paint_composite_mode(paint_mode), NULL, mask_x, mask_y)` — which also applies the selection and lock-alpha; then `gimp_drawable_update`. "Fill whole selection" = `gimp_drawable_edit_fill`.

`GimpFillOptions` properties (`gimpfilloptions.c`): `style` (`GimpFillStyle`: `FG_COLOR`, `BG_COLOR`, `PATTERN`), `custom-style` (`GimpCustomStyle`: `SOLID_COLOR`, `PATTERN`), `antialias`, `feather`, `feather-radius`; plus opacity/paint-mode/pattern/colours from `GimpContext`.

### 4.7 Gradient tool (`gimpdrawable-gradient.c`, `app/operations/gimpoperationgradient.c` = op `gimp:gradient`)

```c
void gimp_drawable_gradient (GimpDrawable *drawable, GimpContext *context, GimpGradient *gradient, GeglDistanceMetric metric,
        GimpLayerMode paint_mode, GimpGradientType gradient_type, gdouble opacity, gdouble offset, GimpRepeatMode repeat,
        gboolean reverse, GimpGradientBlendColorSpace blend_color_space, gboolean supersample, gint max_depth, gdouble threshold,
        gboolean dither, gdouble startx, gdouble starty, gdouble endx, gdouble endy, GimpProgress *progress);
```
`GimpGradientType`: `LINEAR 0, BILINEAR 1, RADIAL 2, SQUARE 3, CONICAL_SYMMETRIC 4, CONICAL_ASYMMETRIC 5, SHAPEBURST_ANGULAR 6, SHAPEBURST_SPHERICAL 7, SHAPEBURST_DIMPLED 8, SPIRAL_CLOCKWISE 9, SPIRAL_ANTICLOCKWISE 10`. `GimpRepeatMode`: `NONE 0` (extend), `SAWTOOTH 1`, `TRIANGULAR 2`, `TRUNCATE 3`.

Setup: `(sx, sy)` = start, `dist = hypot(end − start)`, `vec = (end − start)/dist` (unit axis), `offset` in percent. Each pixel is evaluated at its **centre** (`x += 0.5; y += 0.5`), with `(x, y)` below meaning the offset from the start point.

| Type | `factor` (before repeat) |
|---|---|
| all non-shapeburst | `dist == 0` → 0 |
| LINEAR | `off = offset/100; rat = (vec·(x,y))/dist;` `if (rat >= 0 && rat < off) 0; else if (off == 1) (rat > 1 ? 2 : rat < 0 ? -1 : rat == 1 ? 1 : 0); else if (rat < 0) rat/(1 − off); else (rat − off)/(1 − off)` |
| BILINEAR | `rat = (vec·(x,y))/dist;` `if (|rat| < off) 0; else if (off == 1) (|rat| > 1 ? 2 : |rat| == 1 ? 1 : 0); else (|rat| − off)/(1 − off)` |
| RADIAL | `rat = sqrt(x² + y²)/dist;` `if (rat < off) 0; else if (off == 1) (rat > 1 ? 2 : rat == 1 ? 1 : 0); else (rat − off)/(1 − off)` |
| SQUARE | same as RADIAL with `rat = MAX(|x|, |y|)/dist` |
| CONICAL_SYMMETRIC | at the start point: 0.5; else `rat = clamp(vec·normalize(x,y), −1, 1); rat = acos(rat)/π; rat = pow(rat, offset/10 + 1); CLAMP(rat, 0, 1)` |
| CONICAL_ASYMMETRIC | at start: 0.5; else `ang = (atan2(x, y) + π) − (atan2(vec.x, vec.y) + π); if (ang < 0) ang += 2π; rat = pow(ang/(2π), offset/10 + 1); CLAMP(rat, 0, 1)` |
| SPIRAL_CLOCKWISE / ANTICLOCKWISE | at start: 0.5; else `ang0 = atan2(vec.x, vec.y) + π; ang1 = atan2(x, y) + π; ang = clockwise ? ang1 − ang0 : ang0 − ang1; if (ang < 0) ang += 2π; r = sqrt(x² + y²)/dist; fmod(ang/(2π) + r + offset/100, 1.0)` |
| SHAPEBURST_ANGULAR | `v = 1 − D(x,y);` `off = offset/100; if (v < off) 0; else if (off == 1) (v >= 1 ? 1 : 0); else (v − off)/(1 − off)` |
| SHAPEBURST_SPHERICAL | `off = 1 − offset/100; v = D; if (v > off) v = 1; else if (off == 0) v = (v <= 0 ? 0 : 1); else v /= off; 1 − sin(0.5π·v)` |
| SHAPEBURST_DIMPLED | same normalisation; `cos(0.5π·v)` |

`D(x,y)` = normalised distance map of the selection/drawable shape (absolute pixel coords), produced by `gimp_drawable_gradient_shapeburst_distmap(drawable, metric, off_x, off_y, progress)` = `gegl:distance-transform` on the selection mask (or alpha), normalised to 0..1.

Repeat: `SAWTOOTH`: `factor − floor(factor)`; `TRIANGULAR`: `f = |factor|; i = (uint) f; f −= floor(f); if (i & 1) f = 1 − f`; `TRUNCATE`: outside [0,1] → fully transparent (0,0,0,0); `NONE`: clamp via the colour lookup.
Colour: with the gradient cache (always built by the op: `cache_size = ceil(hypot(start − end)) × GRADIENT_CACHE_N_SUPERSAMPLES (4)`, min 2, max `GRADIENT_CACHE_MAX_SIZE = (1<<20)/(sizeof(double)·4)` = 32768 entries) `index = ROUND(CLAMP(factor,0,1) × (cache_size − 1))` into the precomputed RGBA table; otherwise `gimp_gradient_get_color_at(gradient, NULL, last_seg, factor, reverse, blend_color_space, &color)` (§3.9.6), read back as `"RGBA double"` when the blend space is `RGB_LINEAR`, else `"R'G'B'A double"`.

Supersampling: adaptive (`gimp_adaptive_supersample_area(x1, y1, x2, y2, max_depth, threshold, render_func, render_data, put_pixel_func, put_pixel_data, progress_func, progress_data)`, `libgimpcolor/gimpadaptivesupersample.c`) — renders pixel corners, recursively subdivides (up to `max_depth`) where the colour difference of the 4 corners exceeds `threshold`.
Dither (`gradient_dither_pixel`): one `g_rand_int` per pixel; each byte `b` gives noise `b/256/256 − 0.5/256`; grey pixels (r==g==b) get the same noise on all three channels, otherwise successive bytes per channel; alpha only dithered when `0 < a < 1`; RGB clamped to [0,1].
The gradient buffer is finally applied with `gimp_drawable_apply_buffer(…, opacity, paint_mode, …)` clipped by the selection.

### 4.8 Stroke selection / path

`gimp_item_stroke(item, drawables, context, GimpStrokeOptions, paint_options, push_undo, progress, error)`. `GimpStrokeOptions` (extends `GimpFillOptions`): `method` (`GimpStrokeMethod`: `GIMP_STROKE_LINE 0`, `GIMP_STROKE_PAINT_METHOD 1`), `width`, `unit`, `cap-style` (`GIMP_CAP_BUTT/ROUND/SQUARE`), `join-style` (`GIMP_JOIN_MITER/ROUND/BEVEL`), `miter-limit`, `dash-offset`, `dash-info` (array; presets `GimpDashPreset`: CUSTOM, LINE, LONG_DASH, MEDIUM_DASH, SHORT_DASH, SPARSE_DOTS, NORMAL_DOTS, DENSE_DOTS, STIPPLES, DASH_DOT, DASH_DOT_DOT), `paint-options`, `emulate-brush-dynamics`.
- LINE: `gimp_drawable_stroke_boundary` (selection outline from `gimp_boundary_sort`ed segments) / `gimp_drawable_stroke_path` → `GimpScanConvert` + `gimp_scan_convert_stroke` → `gimp_drawable_stroke_scan_convert`: render coverage mask, create fill buffer (colour/pattern), apply mask as alpha, `gimp_drawable_apply_buffer` with context opacity/mode.
- PAINT_METHOD: `gimp_paint_core_stroke`, `_stroke_boundary`, `_stroke_path` (`gimppaintcore-stroke.c`): feeds the polyline (paths are flattened with `gimp_stroke_interpolate(stroke, 1.0, &closed)`, `gimppaintcore-stroke.c:265`) through `gimp_paint_core_start / interpolate / finish` of the chosen paint core; with `emulate-brush-dynamics` pressure ramps 0→1→0 and velocity are synthesised along the stroke.

---

## 5. Transforms

### 5.1 Math helpers (`libgimpmath`: `gimpmatrix.c/.h`, `gimpvector.c/.h`; plain C doubles — directly portable)

`GimpMatrix2 { gdouble coeff[2][2]; }`, `GimpMatrix3 { gdouble coeff[3][3]; }` (row-major; point transform `x' = (c00 x + c01 y + c02)/w`, `y' = (c10 x + c11 y + c12)/w`, `w = c20 x + c21 y + c22`, with `w == 0 → 1`), `GimpMatrix4`. **All `gimp_matrix3_<op>(m, …)` functions pre-multiply**: `m = Op · m` (the new operation is applied *after* the existing transform), e.g. `gimp_matrix3_translate` adds `x·row2` to row 0 and `y·row2` to row 1; `gimp_matrix3_mult(left, right)` stores `left · right` into `right`.

Functions: `gimp_matrix2_identity, _mult, _determinant, _invert, _transform_point`; `gimp_matrix3_identity, _mult, _translate, _scale, _rotate, _xshear, _yshear, _affine, _determinant, _invert, _is_identity, _is_diagonal, _is_affine, _is_simple, _equal, _transform_point`; `gimp_matrix4_identity, _mult, _to_deg, _transform_point`; `gimp_vector2_{new,set,length,mul,normalize,neg,add,sub,inner_product,cross_product,rotate,normal}` (+ `_val` by-value variants), same for `gimp_vector3_*`, `gimp_vector_2d_to_3d`, `gimp_vector_3d_to_2d`. Also `libgimpmath/gimpmath.h`: `RINT`, `ROUND(x) = (int)(x + 0.5)`, `SIGNED_ROUND`, `SQR`, `CLAMP0255`, `SAFE_CLAMP`, `gimp_deg_to_rad`, `gimp_rad_to_deg`.

Matrix builders (`app/core/gimp-transform-utils.c`), all operate on a caller-initialised matrix:

| Function | Result |
|---|---|
| `gimp_transform_matrix_flip(matrix, GimpOrientationType flip_type, axis)` | mirror about `x = axis` (horizontal) or `y = axis` |
| `gimp_transform_matrix_flip_free(matrix, x1, y1, x2, y2)` | mirror about an arbitrary line |
| `gimp_transform_matrix_rotate(matrix, GimpRotationType, center_x, center_y)` | 90/180/270 about a centre |
| `gimp_transform_matrix_rotate_rect(matrix, x, y, w, h, angle)` | rotate about the rect centre |
| `gimp_transform_matrix_rotate_center(matrix, cx, cy, angle)` | `translate(−c) · rotate(angle) · translate(+c)` |
| `gimp_transform_matrix_scale(matrix, x, y, w, h, t_x, t_y, t_w, t_h)` | `identity; translate(−x,−y); scale(t_w/w, t_h/h); translate(t_x, t_y)` |
| `gimp_transform_matrix_shear(matrix, x, y, w, h, orientation, amount)` | about the rect centre: `xshear(amount/height)` (horizontal) or `yshear(amount/width)` |
| `gimp_transform_matrix_perspective(matrix, x, y, w, h, t_x1,t_y1 … t_x4,t_y4)` | rect → quad. `translate(−x,−y); scale(1/w, 1/h);` then unit-square→quad: `dx1 = x2−x4; dx2 = x3−x4; dx3 = x1−x2+x4−x3` (same for y); if `dx3 == 0 && dy3 == 0` affine: `[[x2−x1, x4−x2, x1],[y2−y1, y4−y2, y1],[0,0,1]]`; else `det2 = dx1·dy2 − dy1·dx2; c20 = det2 ? (dx3·dy2 − dy3·dx2)/det2 : 1; c21 = det2 ? (dx1·dy3 − dy1·dx3)/det2 : 1; c00 = x2−x1+c20·x2; c01 = x3−x1+c21·x3; c02 = x1; c10 = y2−y1+c20·y2; c11 = y3−y1+c21·y3; c12 = y1; c22 = 1`; finally `gimp_matrix3_mult(&trafo, matrix)` |
| `gimp_transform_matrix_generic(matrix, input_points[4], output_points[4])` | general 4-point homography (used by handle/unified tools) |
| `gimp_transform_polygon`, `_polygon_coords`, `_polygon_is_convex`, `gimp_transform_bezier_coords` | transform polygons with proper clipping at the `w = 0` horizon |
| `gimp_transform_get_rotate_center`, `gimp_transform_get_flip_axis` | defaults for auto-centre |

3D transform helpers: `app/core/gimp-transform-3d-utils.c` (camera/focal-length perspective for `gimptransform3dtool.c`).

### 5.2 Applying a transform (`gimpdrawable-transform.c`, `gimpitem.c`, `app/gegl/gimp-gegl-apply-operation.c`)

```c
GeglBuffer * gimp_drawable_transform_buffer_affine (GimpDrawable *drawable, GimpContext *context, GeglBuffer *orig_buffer,
      gint orig_offset_x, gint orig_offset_y, const GimpMatrix3 *matrix, GimpTransformDirection direction,
      GimpInterpolationType interpolation_type, GimpTransformResize clip_result,
      GimpColorProfile **buffer_profile, gint *new_offset_x, gint *new_offset_y, GimpProgress *progress);
GeglBuffer * gimp_drawable_transform_buffer_flip   (…, GimpOrientationType flip_type, gdouble axis, gboolean clip_result, …);
GeglBuffer * gimp_drawable_transform_buffer_rotate (…, GimpRotationType rotate_type, gdouble center_x, gdouble center_y, gboolean clip_result, …);
```
plus drawable-level wrappers `gimp_drawable_transform_affine/_flip/_rotate` (which do cut → transform → paste, floating the selection if one exists: `gimp_drawable_transform_cut`, `gimp_drawable_transform_paste`).

- `GimpTransformDirection`: `GIMP_TRANSFORM_FORWARD 0`, `GIMP_TRANSFORM_BACKWARD 1` ("Corrective": the matrix is inverted first).
- `GimpInterpolationType`: `NONE 0, LINEAR 1, CUBIC 2, NOHALO 3, LOHALO 4`; the value is passed **directly** as the GEGL `sampler` property (`GeglSamplerType` NEAREST/LINEAR/CUBIC/NOHALO/LOHALO) of `gegl:transform` (`gimp_gegl_apply_transform`) and `gegl:scale-ratio` (`gimp_gegl_apply_scale`). NoHalo/LoHalo are GEGL samplers (Nicolas Robidoux) — for those use the GEGL bridge; nearest/bilinear/bicubic are easy natively.
- `GimpTransformResize` (clip modes), resolved by `gimp_transform_resize_boundary(inv, resize, u1, v1, u2, v2, &x1, &y1, &x2, &y2)` (`gimp-transform-resize.c`): `ADJUST 0` — bounding box of the transformed corners (layer grows); `CLIP 1` — keep the original extent; `CROP 2` — largest axis-aligned rectangle inside the transformed quad (`gimp_transform_resize_crop(points, n, aspect = 0, …)`); `CROP_WITH_ASPECT 3` — same but preserving the source aspect `(u2−u1)/(v2−v1)`.
- Alpha is added when needed (non-simple transform or ADJUST), layer masks are transformed with the layer, group layers transform their children (`GIMP_UNDO_GROUP_LAYER_START/END_TRANSFORM`), paths transform their anchors, channels/selection transform with CLIP semantics.
- Flips are done by swapping pixels row/column-wise inside buffer iterators (`gimp_drawable_transform_buffer_flip`: loops over `roi.width / 2` resp. `roi.height / 2`), and 90°/180°/270° rotations by index remapping (`gimp_drawable_transform_buffer_rotate`) — no resampling.

### 5.3 Tool → core mapping

| Tool (`app/tools`) | Core function used |
|---|---|
| Flip (`gimpfliptool.c`) | `gimp_item_flip` / `gimp_drawable_transform_buffer_flip` / `gimp_image_flip` |
| Rotate (`gimprotatetool.c`) | `gimp_transform_matrix_rotate_center` → generic affine apply |
| Scale (`gimpscaletool.c`) | `gimp_transform_matrix_scale`; whole-item scaling uses `gimp_item_scale` |
| Shear (`gimpsheartool.c`) | `gimp_transform_matrix_shear` |
| Perspective (`gimpperspectivetool.c`) | `gimp_transform_matrix_perspective` |
| Unified transform (`gimpunifiedtransformtool.c`), Handle transform (`gimphandletransformtool.c`) | `gimp_transform_matrix_generic` / perspective from 4 corner handles (`GimpGenericTransformTool`) |
| 3D transform (`gimptransform3dtool.c`) | `gimp-transform-3d-utils.c` matrix → same affine/perspective apply |
| All matrix tools | `GimpTransformTool`/`GimpTransformGridTool` → `gimp_item_transform(item, context, matrix, direction, interpolation, clip, progress)`; target chosen by `GimpTransformType` (`LAYER, SELECTION, PATH, IMAGE`) |
| Warp (`gimpwarptool.c`) | not a matrix: records strokes into a `gegl:warp` node chain (properties set from `GimpWarpOptions`: `behavior`, `size`, `hardness`, `strength`, `spacing`, stroke path) producing a displacement field, applied via `GimpDrawableFilter`; → GEGL bridge |
| Cage (`gimpcagetool.c`) | `app/operations/gimpoperationcagecoefcalc.c` (Green coordinates) + `gimpoperationcagetransform.c` + `gimpcageconfig.c`; → port later or bridge through GIMP-specific ops (not in system GEGL) |
| Crop (`gimpcroptool.c`) | `gimp_image_crop` / `gimp_item_resize` |
| Align (`gimpaligntool.c`) | `gimp_image_arrange_objects` (`gimpimage-arrange.c`) |
| Offset (`gimpoffsettool.c`) | `gimp_drawable_offset` / `gimp:offset` op |
| Move | `gimp_item_translate`; multi-item: `gimp_image_item_list_translate` (`gimpimage-item-list.h`) |

---

## 6. Text and paths (overview)

### 6.1 Text (`app/text`)

- `GimpText` (`gimptext.c`) is a GimpConfig object with properties: `text` (plain) **or** `markup` (Pango markup), `font` (GimpFont), `font-size`, `font-size-unit`, `antialias`, `hint-style` (`GimpTextHintStyle` NONE/SLIGHT/MEDIUM/FULL), `kerning`, `language`, `base-direction` (`GimpTextDirection`: LTR, RTL, TTB_RTL, TTB_RTL_UPRIGHT, TTB_LTR, TTB_LTR_UPRIGHT), `base-dir-auto`, `color`, `outline` (`GimpTextOutline`: NONE "Filled", STROKE_ONLY, STROKE_FILL) with `outline-custom-style`, `outline-pattern`, `outline-foreground`, `outline-width`, `outline-unit`, `outline-direction` (OUTER/INNER/CENTERED), `outline-cap-style`, `outline-join-style`, `outline-miter-limit`, `outline-antialias`, `outline-dash-offset`, `outline-dash-info`; `justify` (`GimpTextJustification` LEFT/RIGHT/CENTER/FILL), `indent`, `line-spacing`, `letter-spacing`, `box-mode` (dynamic/fixed), `box-width`, `box-height`, `box-unit`, `transformation` (GimpMatrix2), `offset-x`, `offset-y`, `border`; deprecated `hinting`, plus `gimp` back-pointer.
- `GimpTextLayout` (`gimptextlayout.c`) wraps a **PangoLayout** (`pango_layout_new`, `_set_auto_dir`, `_set_wrap(PANGO_WRAP_WORD_CHAR)`, `_set_font_description`, `_set_justify`, `_set_alignment`, markup → attributes, letter spacing, indent, line spacing) at the image resolution; API `gimp_text_layout_new(text, image, xres, yres, error)`, `_get_size`, `_get_offsets`, `_get_resolution`, `_get_transform`, `_get_pango_layout`, `_transform/_untransform_{point,rect,distance}`. `gimp_text_layout_render(layout, cairo_t *cr, base_dir, path)` (`gimptextlayout-render.c`) draws with **pangocairo** (or emits a cairo path for text→path).
- `GimpTextLayer` (`gimptextlayer.c`): on any text change re-renders into the layer buffer (`gimp_text_layer_render`: cairo ARGB32 surface → layer format, coloured with the text colour / outline), resizes the layer to the layout extents; `auto_rename`, `modified` (rasterized) flags. Transform of a text layer keeps it editable by storing the matrix (`gimptextlayer-transform.c`). Text → path: `gimp_text_path_new(image, text)` (`gimptext-path.c`).
- Persistence: parasite **`gimp-text-layer`** on the layer (`gimp_text_parasite_name()`, `gimptext-parasite.c`) holding the GimpConfig-serialised `GimpText` (an s-expression list of the properties named above, `(property value)` each; the exact text was not dumped from a sample file in this pass; "Format of font information in text layer was changed in GIMP 3.0" → XCF 19, `gimpimage.c: gimp_image_get_xcf_version`) + XCF `PROP_TEXT_LAYER_FLAGS` (bit 0 `TEXT_LAYER_XCF_DONT_AUTO_RENAME`, bit 1 `TEXT_LAYER_XCF_MODIFIED`); legacy `gimp-gdyntext`-style parasites are converted (`gimp_text_from_gdyntext_parasite`). The rendered pixels are always stored too, so an importer can treat text layers as raster and keep the parasite for round-tripping.
- Port note: Godot already has TextServer (HarfBuzz/FreeType/ICU, BiDi, vertical layout); do **not** port Pango. Map `GimpText` properties onto `TextParagraph`/`TextLine` and rasterise through Godot.

### 6.2 Paths (`app/path`)

- `GimpPath` (`gimppath.c`, a `GimpItem`): `GQueue *strokes` of `GimpStroke`; ops: add/remove stroke, anchor get/select/move, transform, `gimp_path_bounds`, `gimp_path_get_length`, `gimp_path_get_distance`, `gimp_path_interpolate`, freeze/thaw.
- `GimpStroke` (`gimpstroke.h`): `gint id; GQueue *anchors; gboolean closed;` with a large vtable (`anchor_get`, `anchor_get_next`, `anchor_select`, `anchor_move_relative/_absolute`, `point_move_relative`, `open/close`, `duplicate`, `make_bezier`, `translate/scale/rotate/flip/flip_free/transform`, `get_length`, `get_distance`, `interpolate(stroke, precision, &closed)` → `GArray` of `GimpCoords`, `nearest_point_get`, `get_point_at_dist`, `control_points_get`, …). The only concrete type is `GimpBezierStroke` (`gimpbezierstroke.c`; `GimpPathStrokeType`: `GIMP_PATH_STROKE_TYPE_BEZIER`).
- `GimpAnchor` (`gimpanchor.h`): `GimpCoords position; GimpAnchorType type; gboolean selected;` with `GIMP_ANCHOR_ANCHOR 0` (end point, "E") and `GIMP_ANCHOR_CONTROL 1` (handle, "H"). A Bézier stroke stores anchors in the repeating pattern **H E H  H E H …** (each end point is surrounded by its incoming and outgoing handle); a cubic segment between end points E_i and E_{i+1} uses `E_i, H_out(i), H_in(i+1), E_{i+1}`. `GimpAnchorFeatureType`: `NONE, EDGE, ALIGNED, SYMMETRIC` (handle constraints while editing).
- SVG: `gimp_path_import_file(image, file, merge, scale, parent, position, &ret_paths, error)` and `gimp_path_import_buffer(image, buffer, len, merge, scale, …)` (`gimppath-import.c`: GMarkup-based parser for `<path d>`, `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polyline>`, `<polygon>`, nested `<g>`/`<svg>` transforms, viewBox/units; arcs are converted to Béziers); export `gimp_path_export_file(image, paths, file, error)`, `gimp_path_export_string(image, path)` (`gimppath-export.c`: one `<path id=… fill="none" stroke="black" stroke-width="1" d="M x,y C … Z"/>` per GimpPath, image size as width/height/viewBox).
- `GimpVectorLayer` (`gimpvectorlayer.c`, `gimpvectorlayeroptions.c`; 3.2): layer rendered from a path with `enable-fill`, `enable-stroke`, `GimpFillOptions` (style/antialias/colour/pattern) and `GimpStrokeOptions` (width, cap, join, miter, dashes); re-rendered when the path changes; XCF `PROP_VECTOR_LAYER` (§7.3).
- Selections ↔ paths: `gimp_channel_select_path`, `gimp_path_warp` (text along path), `gimp_drawable_fill_path`, `gimp_drawable_stroke_path`.

---

## 7. XCF file format (derived from `app/xcf/xcf-load.c`, `xcf-save.c`, `xcf-read.c`, `xcf-write.c`, `xcf-private.h`, `app/core/gimpimage.c: gimp_image_get_xcf_version`)

No spec text exists in this checkout (§0). Everything below is what the 3.2.8 reader/writer actually do.

### 7.1 Primitives

- Byte order: **big-endian** for every integer, float and offset.
- `uint32`/`int32`: 4 bytes. `float`: IEEE-754 single, 4 bytes. `byte`: 1.
- **offset**: 4 bytes for file version ≤ 10, **8 bytes for version ≥ 11** (`info->bytes_per_offset`, `xcf_load_magic_version`). Offsets are absolute file positions; a 0 offset terminates lists.
- **string**: `uint32 n` = byte length **including the trailing NUL**, followed by `n` bytes (UTF-8); `n == 0` means NULL/empty and no bytes follow (`xcf_read_string`, `xcf_write_string`). Reader caps at `MAX_XCF_STRING_LEN`.
- **property**: `uint32 type; uint32 payload_size; byte payload[payload_size]`. Unknown types are skipped by size. Lists end with `PROP_END` (type 0, size 0).
- **parasite**: `string name; uint32 flags; uint32 size; byte data[size]` (max `MAX_XCF_PARASITE_DATA_LEN` = 256 MiB).

### 7.2 File layout

```
byte   magic[14]      "gimp xcf file\0" (version 0)  or  "gimp xcf vNNN\0" (NNN = 3-digit decimal version, e.g. "gimp xcf v011")
uint32 width, height
uint32 base_type      0 RGB, 1 GRAY, 2 INDEXED
uint32 precision      only if version >= 4 (see table below)
property list         image properties … PROP_END
offset layer_offsets[]     zero-terminated; top layer first, pre-order through groups
offset channel_offsets[]   zero-terminated (includes the selection mask channel if non-empty, flagged by PROP_SELECTION)
offset path_offsets[]      zero-terminated; only if version >= 18
… layer / channel / path / hierarchy / tile data, in any order, referenced by the offsets …
```
Precision field decoding (`xcf_load_image_header`):

| File version | Meaning of the uint32 |
|---|---|
| < 4 | absent; precision = `GIMP_PRECISION_U8_NON_LINEAR` (150) |
| 4 | 0 → U8_NON_LINEAR, 1 → U16_NON_LINEAR, 2 → U32_LINEAR, 3 → HALF_LINEAR, 4 → FLOAT_LINEAR |
| 5, 6 | 100 U8_LINEAR, 150 U8_NON_LINEAR, 200 U16_LINEAR, 250 U16_NON_LINEAR, 300 U32_LINEAR, 350 U32_NON_LINEAR, **400 HALF_LINEAR, 450 HALF_NON_LINEAR, 500 FLOAT_LINEAR, 550 FLOAT_NON_LINEAR** (old numbering!) |
| ≥ 7 | the `GimpPrecision` enum value itself (§1.2: 100…775) |

Sanity limits enforced by the loader: `base_type` in 0..2 else fail; width/height outside `1…GIMP_MAX_IMAGE_SIZE (524288)` are replaced by 1×1 ("be lenient"); offsets must point forward of the table position.

**Version history** (minimum version the writer picks = max over the features used; `gimp_image_get_xcf_version`):

| Ver | Needed for | GIMP |
|---|---|---|
| 0 | baseline (`"gimp xcf file"`); v0 colormaps are broken (loader substitutes a grey ramp) | ≤ 2.6 |
| 1 | indexed colormap | |
| 2 | layer modes SOFTLIGHT/GRAIN_EXTRACT/GRAIN_MERGE/COLOR_ERASE (legacy) | 2.6 |
| 3 | layer groups | 2.8 |
| 4–6 | early high-bit-depth precision encodings (read-only today) | 2.9 dev |
| 7 | any precision other than 8-bit non-linear | 2.10 |
| 8 | zlib tile compression | 2.10 |
| 9 | modes OVERLAY (new), LCH_HUE/CHROMA/COLOR/LIGHTNESS | 2.10 |
| 10 | all other new 2.10 modes (NORMAL … PASS_THROUGH); the writer bumps 10 → 11 immediately | 2.10 |
| 11 | **64-bit offsets** (files ≥ 4 GB) | 2.10 |
| 12 | correct big-endian storage of > 8-bit tile data | 2.10 |
| 13 | masks on layer groups | 2.10 |
| 14 | multiple selected layers | 3.0 |
| 15 | off-canvas guides | 3.0 |
| 16 | stored item sets / pattern sets; multiple selected channels | 3.0 |
| 17 | visibility locks; position/alpha locks on groups | 3.0 |
| 18 | paths stored as first-class blocks (third offset table); multiple selected paths, path colour tags/locks | 3.0 |
| 19 | new font serialisation in text-layer parasites | 3.0 |
| 20 | layer effects (NDE filters) | 3.0 |
| 21 | user-unit storage without singular/plural strings | 3.0 |
| 22 | effect op-version string (the writer uses ≥ 22 whenever a layer has filters) | 3.0 |
| 23 | `GIMP_LAYER_COLOR_SPACE_RGB_PERCEPTUAL` (value 4) as blend/composite space. Before 23, value 2 was the only non-linear RGB choice (2.10 called it "RGB (perceptual)"; 3.x reads it as `RGB_NON_LINEAR`, babl `R'G'B'`) | 3.0 |
| 24 | vector layers | 3.2 |
| 25 | link layers | 3.2 |
| 26 | effects on channels / layer masks | 3.2.6 |

### 7.3 Properties — complete list (`PropType`, `xcf-private.h`)

"Where": I = image, L = layer, C = channel/mask/selection, E = effect, P = path (v ≥ 18 blocks).

| Id | Name | Where | Payload (big-endian) / semantics |
|---|---|---|---|
| 0 | `PROP_END` | all | size 0; terminates the list |
| 1 | `PROP_COLORMAP` | I | `uint32 n_colors; byte rgb[3·n]` (n ≤ 256). Only applied for INDEXED images. Version 0: payload is garbage → grey ramp |
| 2 | `PROP_ACTIVE_LAYER` | L | size 0; layer is selected (may occur on several layers since v14) |
| 3 | `PROP_ACTIVE_CHANNEL` | C | size 0; channel is selected |
| 4 | `PROP_SELECTION` | C | size 0; this channel **is** the selection mask |
| 5 | `PROP_FLOATING_SELECTION` | L | `offset` of the drawable (layer or channel block) this floating selection is attached to |
| 6 | `PROP_OPACITY` | L, C | `uint32` 0…255 (writer: `(uint32)(opacity * 255.999)`, reader: `/ 255.0`) |
| 7 | `PROP_MODE` | L, E | `int32` `GimpLayerMode` (§2.1); 5 (`OVERLAY_LEGACY`) is rewritten to 19 (`SOFTLIGHT_LEGACY`) on both load and save |
| 8 | `PROP_VISIBLE` | L, C, E, P | `uint32` bool (for effects: "active") |
| 9 | `PROP_LINKED` | L, C | `uint32` bool; obsolete chain-link; reader turns linked items into a stored item set; never written |
| 10 | `PROP_LOCK_ALPHA` | L | `uint32` bool |
| 11 | `PROP_APPLY_MASK` | L | `uint32` bool (always written; FALSE when no mask) |
| 12 | `PROP_EDIT_MASK` | L | `uint32` bool |
| 13 | `PROP_SHOW_MASK` | L | `uint32` bool |
| 14 | `PROP_SHOW_MASKED` | C | `uint32` bool |
| 15 | `PROP_OFFSETS` | L | `int32 x; int32 y` (reader zeroes values outside ±`GIMP_MAX_IMAGE_SIZE`) |
| 16 | `PROP_COLOR` | C | `byte r, g, b` (`R'G'B' u8`) — channel display colour |
| 17 | `PROP_COMPRESSION` | I | `byte`: 0 none, 1 RLE, 2 zlib, 3 fractal (never implemented). Written only when ≠ 0 |
| 18 | `PROP_GUIDES` | I | `n = size / 5` × { `int32 position; int8 orientation` } with orientation 1 = horizontal, 2 = vertical (`XcfOrientationType`). v < 15: negative or off-canvas positions are dropped. Custom (symmetry) guides are not saved |
| 19 | `PROP_RESOLUTION` | I | `float xres; float yres` (dpi); out-of-range → default template resolution |
| 20 | `PROP_TATTOO` | I, L, C, P | `uint32`: item tattoo; on the image: the tattoo counter state |
| 21 | `PROP_PARASITES` | I, L, C, P | concatenated parasite records until `size` bytes are consumed |
| 22 | `PROP_UNIT` | I | `uint32` built-in unit id (1 inch, 2 mm, 3 point, 4 pica; anything ≤ 0 or ≥ 5 → inch) |
| 23 | `PROP_PATHS` | I | legacy (≤ 2.2-style) path list; only written for v < 18 when all paths are "compat"; reader: `xcf_load_old_paths` |
| 24 | `PROP_USER_UNIT` | I | `float factor; uint32 digits;` then strings: v < 21: 5 strings (identifier/name, symbol, abbreviation, singular, plural); v ≥ 21: 3 strings (name, symbol, abbreviation) |
| 25 | `PROP_VECTORS` | I | paths for v < 18: `uint32 version (=1); uint32 active_index; uint32 num_paths;` per path: `string name; uint32 tattoo; uint32 visible; uint32 linked; uint32 num_parasites; uint32 num_strokes;` parasites…; per stroke: `uint32 stroke_type (1 = Bézier); uint32 closed; uint32 num_axes (2…6); uint32 num_control_points;` per point: `uint32 anchor_type (0 anchor, 1 control); float coords[num_axes]` (x, y, pressure, xtilt, ytilt, wheel) |
| 26 | `PROP_TEXT_LAYER_FLAGS` | L | `uint32` bits: 1 `DONT_AUTO_RENAME`, 2 `MODIFIED` (written only when non-zero) |
| 27 | `PROP_OLD_SAMPLE_POINTS` | I | `n = size / 8` × { `int32 x; int32 y` }; skipped if `PROP_SAMPLE_POINTS` was already read (the writer emits 39 first, then 27) |
| 28 | `PROP_LOCK_CONTENT` | L, C, P | `uint32` bool |
| 29 | `PROP_GROUP_ITEM` | L | size 0; the layer is a group |
| 30 | `PROP_ITEM_PATH` | L | `uint32 index[size / 4]`: position path from the root of the layer tree (written only for layers with a parent). If a floating selection exists the top-level index is decremented by the reader |
| 31 | `PROP_GROUP_ITEM_FLAGS` | L | `uint32` bits: 1 `XCF_GROUP_ITEM_EXPANDED` |
| 32 | `PROP_LOCK_POSITION` | L, C, P | `uint32` bool |
| 33 | `PROP_FLOAT_OPACITY` | L, C, E | `float` 0…1; written right after `PROP_OPACITY` and overrides it |
| 34 | `PROP_COLOR_TAG` | L, C, P | `uint32` `GimpColorTag` 0…8 |
| 35 | `PROP_COMPOSITE_MODE` | L, E | `int32` `GimpLayerCompositeMode`; **layers: if the in-memory value is AUTO the writer stores the *negative* of the mode's current default; the reader maps a negative value back to AUTO when `-value` still equals the default for the layer's mode, else keeps `-value` as an explicit setting.** (Requires `PROP_MODE` to precede it — the writer guarantees the order MODE, BLEND_SPACE, COMPOSITE_SPACE, COMPOSITE_MODE.) |
| 36 | `PROP_COMPOSITE_SPACE` | L, E | `int32` `GimpLayerColorSpace`, same negative-AUTO encoding |
| 37 | `PROP_BLEND_SPACE` | L, E | `int32` `GimpLayerColorSpace`, same negative-AUTO encoding |
| 38 | `PROP_FLOAT_COLOR` | C | `float r, g, b` (`R'G'B' float`), written after `PROP_COLOR` |
| 39 | `PROP_SAMPLE_POINTS` | I | `n = size / 20` × { `int32 x; int32 y; uint32 pick_mode; uint32 padding[2]` }; pick_mode > `GIMP_COLOR_PICK_MODE_LAST` → PIXEL |
| 40 | `PROP_ITEM_SET` | I | `uint32 item_type` (0 layers, 1 channels, 2 paths — reader accepts only 0/1); `uint32 method` (`GimpSelectMethod` 0 plain text, 1 regex, 2 glob; `0xFFFFFFFF` = explicit named set); `string name_or_pattern` |
| 41 | `PROP_ITEM_SET_ITEM` | L, C | `uint32 n` = index of the (non-pattern) item set this item belongs to, in file order of the `PROP_ITEM_SET`s of its type |
| 42 | `PROP_LOCK_VISIBILITY` | L, C, P | `uint32` bool |
| 43 | `PROP_SELECTED_PATH` | P | size 0 |
| 44 | `PROP_FILTER_REGION` | E | `uint32` `GimpFilterRegion` (0 selection, 1 drawable) |
| 45 | `PROP_FILTER_ARGUMENT` | E | `string property_name; uint32 type;` value by `FilterPropType`: 1 INT / 7 UINT / 5 ENUM → `uint32`; 2 BOOL → `uint32`; 3 FLOAT → `float` (doubles are narrowed!); 4 STRING → `string`; 6 CONFIG → `string` (GimpConfig serialisation); 8 COLOR → colour record (below); 0 UNKNOWN |
| 46 | `PROP_FILTER_CLIP` | E | `uint32` bool |
| 47 | `PROP_VECTOR_LAYER` | L | `uint32 rasterized; uint32 path_tattoo; uint32 enable_fill; uint32 enable_stroke; uint32 fill_style (GimpCustomStyle); uint32 fill_antialias; color fill_color; data-id fill_pattern; uint32 stroke_style; uint32 stroke_antialias; color stroke_color; data-id stroke_pattern; float stroke_width; uint32 cap_style; uint32 join_style; float miter_limit; uint32 n_dashes; float dash[n_dashes]` |
| 48 | `PROP_LINK_LAYER` | L | `uint32 flags; string relative_path; uint32 width; uint32 height` |
| 49 | `PROP_TRANSFORM` | L (link layers) | `int32 offset_x; int32 offset_y; uint32 interpolation; float m[9]` (row-major 3×3) — 48 bytes |

Serialised helper records: **colour** (`xcf_save_color`/`xcf_load_color`): `string babl_encoding` (e.g. `"R'G'B'A float"`; a NULL colour is a single zero uint32), `uint32 data_length` (must equal the format's bytes-per-pixel), `byte data[data_length]` (**raw host-endian** pixel bytes — written with `xcf_write_int8`, no byte swap), `uint32 icc_length` (0 for sRGB), `byte icc[icc_length]`. **data-id** (`xcf_save_data_id`/`xcf_load_data`): `string name;` then, if the name is present, `string collection; uint32 is_internal`.

Upstream hazard worth knowing: `xcf_save_effect_props` calls `xcf_save_prop(…, PROP_BLEND_SPACE / PROP_COMPOSITE_SPACE / PROP_COMPOSITE_MODE, error, value)` with **one** vararg while those cases read **two** (`mode`, then the value) — the value stored for an *effect's* blend space/composite space/composite mode is therefore not trustworthy (the reader takes the int32 as-is). Validate the range and fall back to AUTO when importing effects.

### 7.4 Layer block

```
uint32 width, height
uint32 type            GimpImageType: 0 RGB, 1 RGBA, 2 GRAY, 3 GRAYA, 4 INDEXED, 5 INDEXEDA
string name
property list          writer order: [GROUP_ITEM] [ITEM_PATH] [ACTIVE_LAYER] [FLOATING_SELECTION] OPACITY FLOAT_OPACITY VISIBLE COLOR_TAG
                       LOCK_CONTENT LOCK_ALPHA LOCK_POSITION LOCK_VISIBILITY APPLY_MASK EDIT_MASK SHOW_MASK OFFSETS MODE BLEND_SPACE
                       COMPOSITE_SPACE COMPOSITE_MODE TATTOO [TEXT_LAYER_FLAGS | VECTOR_LAYER | LINK_LAYER [TRANSFORM]] [GROUP_ITEM_FLAGS]
                       [PARASITES] [ITEM_SET_ITEM…] END
offset hierarchy       pixel data (see 7.5)
offset mask            0 if none; else a channel block (7.6) that is the layer mask
offset effects[]       zero-terminated list of effect blocks — read only if version >= 20 (3.2.8 always writes at least the terminating 0)
```
- Pixel format = image precision × `type` (§1.2); bytes-per-pixel `bpp = n_components × bytes_per_component`; the hierarchy's own `bpp` field must match or the layer fails to load.
- **Groups**: the hierarchy is present but **ignored** by the reader (content is recomputed from children). Children follow their parent in the layer table and carry `PROP_ITEM_PATH` = indices from the root (e.g. `[2, 0]` = first child of the third top-level layer). The reader appends each layer at the end of its parent container, so table order = top-to-bottom stacking order.
- Invalid width/height are tolerated (forced to 1×1) only for group/text/link layers.
- **Text layers** are ordinary layers plus the `gimp-text-layer` parasite (in `PROP_PARASITES`) and optional `PROP_TEXT_LAYER_FLAGS`; `gimp_text_layer_xcf_load_hack` swaps the object type after the props are read.
- Mask flags (`apply/edit/show`) are stored on the layer; the mask itself is attached after all layers are loaded (`xcf_load_add_masks`), as are effects (`xcf_load_add_effects`).
- Floating selection: the FS layer is *not* added to the stack; `PROP_FLOATING_SELECTION` gives the offset of its host block; attach after loading (`floating_sel_attach`).

### 7.5 Hierarchy → level → tiles (`xcf_save_buffer`, `xcf_load_buffer`, `xcf_load_level`)

```
hierarchy:  uint32 width, height, bpp
            offset level_offsets[]      zero-terminated; n_levels = max over w,h of (1 + number of halvings until size <= 64)
level 0:    uint32 width, height        (must equal the buffer size)
            offset tile_offsets[]       zero-terminated; ntiles = ceil(w/64) * ceil(h/64), row-major, tile i covers
                                        x = (i % cols) * 64, y = (i / cols) * 64, clipped at the right/bottom edge
levels ≥ 1: dummy: uint32 width/2^k, height/2^k, then a **uint32** 0 (yes, 32-bit even in 64-bit-offset files) — ignored by readers
```
Only level 0 carries data. An all-zero first tile offset means "empty level" (buffer stays zero/transparent). The stored length of tile *i* is `offset[i+1] − offset[i]`; for the last tile (next offset 0) the reader assumes the maximum `64·64·bpp·1.5` (`XCF_TILE_MAX_DATA_LENGTH_FACTOR`) and reads what is available; any length above that maximum is an error. Tiles whose decoded data is entirely zero are simply not written into the buffer.

Tile payload by `PROP_COMPRESSION`:
- **0 none**: `tile_w × tile_h × bpp` bytes, pixel-interleaved. Version ≤ 11: raw bytes as written by the producing machine; version ≥ 12: each component big-endian (`xcf_read_component(bpc = bpp / n_components)`).
- **1 RLE** (`xcf_load_tile_rle` / `xcf_save_tile_rle`): the tile is first put in big-endian component order (v ≥ 12), then encoded **one byte-plane at a time**: for `i in 0…bpp−1` the bytes at offsets `i, i+bpp, i+2·bpp, …` (count = `tile_w·tile_h`) form one stream:
  ```
  repeat until `count` bytes of this plane are produced:
    op = next byte
    if op >= 128:                       # literal run
        len = 256 - op                  # 1…128
        if len == 128: len = (next byte << 8) | next byte      # long form, opcode 128
        copy `len` bytes verbatim
    else:                               # repeat run
        len = op + 1                    # 1…128
        if len == 128: len = (next byte << 8) | next byte      # long form, opcode 127
        v = next byte; emit v `len` times
  ```
  (Encoder: runs ≥ 128 use the long form with a 16-bit length ≤ 32768.) Running past the data or overshooting the plane → the tile is rejected (`bogus_rle`).
- **2 zlib**: one zlib (deflate) stream per tile containing the big-endian, pixel-interleaved tile (`inflateInit` / `deflateInit(Z_DEFAULT_COMPRESSION)`); decoded size must not exceed the tile size.
- 3 "fractal": error.

### 7.6 Channel / mask block, effects, paths

```
channel:  uint32 width, height;  string name;
          property list (writer order: [ACTIVE_CHANNEL] [SELECTION] OPACITY FLOAT_OPACITY VISIBLE COLOR_TAG LOCK_CONTENT LOCK_POSITION
                         LOCK_VISIBILITY SHOW_MASKED COLOR FLOAT_COLOR TATTOO [PARASITES] [ITEM_SET_ITEM…] END)
          offset hierarchy                 (bpp = bytes per component of the image precision; one component)
          offset effects[]                 zero-terminated — read only if version >= 26
```
Layer masks use the same structure (loaded by `xcf_load_layer_mask`, created black, same size as the layer). The selection is a channel flagged `PROP_SELECTION` and replaces `image->selection_mask`'s buffer instead of being added to the channel list.

```
effect (v >= 20):  string name (0-length if the user did not rename)   string icon_name   string gegl_operation (e.g. "gegl:gaussian-blur")
                   [string op_version — only if file version >= 22]
                   property list: VISIBLE FLOAT_OPACITY MODE BLEND_SPACE COMPOSITE_SPACE COMPOSITE_MODE FILTER_CLIP FILTER_REGION
                                  FILTER_ARGUMENT… END
                   offset mask     → channel block (the filter mask)
```
Effects whose operation is not allowed (`gimp_gegl_op_nde_allowed`) or whose `op_version` differs from the installed op are discarded with a warning; tool-based filters (operation `"GraphNode"`) and temporary filters are never saved. In the file the effect list is ordered as `GIMP_LIST(filters)->queue->head` first (top of the stack first).

```
path block (v >= 18):  string name;  uint32 payload_size;
                       property list ([SELECTED_PATH] VISIBLE COLOR_TAG LOCK_CONTENT LOCK_POSITION LOCK_VISIBILITY TATTOO [PARASITES] END)
                       uint32 version (=1);  uint32 num_strokes;
                       per stroke: uint32 type (1 = XCF_STROKETYPE_BEZIER_STROKE); uint32 closed; uint32 num_axes (writer: 2);
                                   uint32 num_control_points;  per point: uint32 anchor_type (0 ANCHOR, 1 CONTROL); float coords[num_axes]
```

### 7.7 Image-level parasites that matter (`xcf_load_image`, `xcf_save_image_props`)

| Parasite name | Content |
|---|---|
| `icc-profile` (`GIMP_ICC_PROFILE_PARASITE_NAME`) | raw ICC profile of the image; `simulation-icc-profile`, `image-simulation-intent` (1 byte), `image-simulation-bpc` for soft-proofing |
| `gimp-image-grid` | GimpGrid config text |
| `gimp-image-metadata` | serialised GimpMetadata XML (Exif/XMP/IPTC); legacy `exif-data`, `gimp-metadata` are migrated |
| symmetry parasites (`gimp_symmetry_parasite_name(type)`) | serialised symmetry settings; identity symmetry is not saved |
| `gimp-comment` | image comment |
| `gimp-xcf-compatibility-mode` | checked by the loader (legacy) |
| `gimp-text-layer` (on layers) | text layer definition |

### 7.8 Importer / exporter recipe for Tetra

Importer (8-bit and float RGBA, modes, opacity, masks, groups, offsets):
1. Read magic → version → `bytes_per_offset`; header; decode precision.
2. Image props: keep COMPRESSION, RESOLUTION, COLORMAP, GUIDES, PARASITES(`icc-profile`), UNIT; skip the rest by size.
3. Layer table in order; for each: header, props (OPACITY/FLOAT_OPACITY, VISIBLE, OFFSETS, MODE (5→19), BLEND/COMPOSITE_SPACE/COMPOSITE_MODE with the negative-AUTO rule, LOCK_*, APPLY/EDIT/SHOW_MASK, GROUP_ITEM, ITEM_PATH, COLOR_TAG, FLOATING_SELECTION, TEXT_LAYER_FLAGS, PARASITES), then offsets; decode level-0 tiles unless group; decode the mask channel; optionally effects (keep op name + args for the GEGL bridge).
4. Convert sample types: u8/u16/u32 → float by `/max`; half → float; keep the TRC tag (linear / non-linear / perceptual) on the image; GRAY/INDEXED → expand through the colormap or to RGB as needed.
5. Build the tree from ITEM_PATH; attach masks; anchor or keep the floating selection; channel table → saved channels + selection.

Exporter (basic): write version 11 minimum (64-bit offsets; avoids the 4 GB/endian pitfalls) or the minimum from the table for the features used (12 if > 8-bit, 13 for group masks, 23 if perceptual space is used); `PROP_COMPRESSION` 1 (RLE) is the safest for old readers; always emit both OPACITY and FLOAT_OPACITY; emit dummy mip levels exactly as upstream does; write the terminating 0 effect offset after the mask offset.

---

## 8. Colour

### 8.1 Colour values in 3.x

- **`GimpRGB` is gone.** Every colour in core/libgimp is a `GeglColor` (GObject wrapping one pixel + a babl format with space). Read/write with `gegl_color_get_pixel(color, babl_format("…"), buf)` / `gegl_color_set_pixel`, `gegl_color_get_rgba`, `gegl_color_get_format`, `gegl_color_get_bytes`. Helpers left in `libgimpcolor/gimpcolor.c`: `gimp_color_set_alpha(color, alpha)`, `gimp_color_is_perceptually_identical(c1, c2)` (alpha differs by > 1e-4 → different; else CIEDE2000 distance < 0.6 — `gimp_color_get_CIE2000_distance`, constant `CONST_25_POWER_7`), `gimp_color_is_out_of_gamut(color, space)`, `gimp_color_is_out_of_self_gamut` (`CHANNEL_EPSILON 1e-3`), `gimp_babl_format_get_with_alpha`, `GimpParamSpecColor` (`gimp_param_spec_color`, `_from_string`, `_has_alpha`). Parsing: `gimp_color_parse_css`, `gimp_color_parse_hex`, `gimp_color_parse_name`, `gimp_color_list_names` (`gimpcolor-parse.c`).
- Context colours (foreground/background), channel colour, grid colours, gradient segment endpoints, palette entries, text colour, quick-mask colour are all `GeglColor` → they may be in **any** model/space (e.g. a CMYK or Lab foreground colour). The port needs an equivalent "colour + encoding + space" value type, or must normalise to linear-sRGB float RGBA at the boundary and document the loss.
- Serialisation in GimpConfig files (`libgimpconfig/gimpconfig-serialize.c:318-390`, parser symbols in `gimpscanner.c`): the generic form `(color "<babl encoding>" <data_length> <data> <icc_length> [<icc data>])` — encoding string from `babl_format_get_encoding`, raw pixel bytes written with `gimp_config_writer_data`, ICC length 0 when the space is sRGB (palette-format colours are first converted to `R'G'B'A u8`); the legacy form `(color-rgba r g b a)` with `R'G'B'A double` values printed by `g_ascii_dtostr` (used when writing for old XCF versions, cf. `gimp_config_set_xcf_version`); the reader also accepts `color-rgb`, `color-hsv`, `color-hsva`; a NULL colour is written as `NULL`. In XCF: §7.3 colour record.

### 8.2 Colour models (all via babl formats; semantics measured on babl 0.1.108, sRGB space)

| Model | babl format | Definition as observed |
|---|---|---|
| RGB linear | `RGB(A) float/double` | linear light, image primaries |
| RGB non-linear | `R'G'B'(A)` | TRC of the space (sRGB curve for sRGB) |
| RGB perceptual | `R~G~B~(A)` | sRGB curve always |
| Gray | `Y`, `Y'`, `Y~` | `Y = 0.2224879 R + 0.7169038 G + 0.0606083 B` (linear RGB, D50-adapted sRGB weights); `Y'` = TRC(Y) |
| HSV | `HSV(A) float` | computed from **non-linear** `R'G'B'`: `V = max`, `S = (max − min)/max` (0 if max = 0), `H` = standard hexcone hue / 360 ∈ [0,1). Check: R'G'B' (0.2, 0.5, 0.8) → (0.583333, 0.75, 0.8). (From *linear* (0.2,0.5,0.8) the same format gives (0.567557, 0.465395, 0.906332) = HSV of the encoded values.) |
| HSL | `HSL(A) float` | from non-linear R'G'B': `L = (max + min)/2`, `S = (max − min)/(max + min)` if `L <= 0.5` else `(max − min)/(2 − max − min)`, H as HSV. Check: (0.2, 0.5, 0.8) → (0.583333, 0.6, 0.5) |
| CIE Lab | `CIE Lab (alpha) float` | D50, L 0…100, a/b ≈ ±128 (§2.7) |
| CIE LCh | `CIE LCH(ab) (alpha) float` | `C = hypot(a, b)`, `H = atan2(b, a)` in **degrees** 0…360. Check: linear (0.2,0.5,0.8) → (72.98636, 31.24966, 246.49345) |
| CMYK (naive, no profile) | `CMYK float` | computed from **linear** RGB: `K = 1 − max(R,G,B)`, `C = (1 − R − K)/(1 − K)`, same for M, Y. Check: linear (0.2,0.5,0.8) → (0.75, 0.375, 0, 0.2); R'G'B' (0.2,0.5,0.8) → (0.945175, 0.645526, 0, 0.396173). `cmyk float` is the inverted ("ink-less") variant: (0.25, 0.625, 1.0, 0.8). With a CMYK simulation profile GIMP uses lcms2 via babl spaces instead. |
| xyY, Yu'v' | `CIE xyY float`, `CIE Yuv float` | colour-picker display modes only (`GimpColorPickMode`) |

The exact rounding/edge handling inside babl (achromatic hue value, threshold constants) was **not** read from source (babl is not in the pinned set). For the native port: implement the textbook formulas above in double/float, and cross-check against the system babl through the GEGL bridge in unit tests.

Hand-written conversions that *are* in the tree: `gimpcolor-legacy.c` (§2.6), used only by the legacy HSV/HSL layer modes; and the gradient HSV interpolation (§3.9.6) which uses babl `HSV float`.

### 8.3 Precision, gamma, and what "linear" means in 3.x

- An image stores pixels at its `GimpPrecision` (component type + TRC). The TRC choice is *storage encoding only*; it does not decide how layers are blended. Blending/compositing spaces are chosen **per layer mode** (§2.1), independent of storage: default modes composite in linear light (most blend in perceptual sRGB-TRC space), legacy modes work on the image-profile-encoded values, exactly reproducing 2.8 behaviour for 8-bit sRGB images.
- Paint tools: the paint buffer format is `gimp_layer_mode_get_format(paint_mode, AUTO, AUTO, paint_composite_mode, drawable_format)` → float RGBA in the mode's composite space; the paint colour is converted from its GeglColor into that format; the result is converted back to the drawable format on write (so 8-bit images quantise after every dab).
- Gradients blend in `GimpGradientBlendColorSpace` (perceptual RGB default), selections/masks are linear `Y`, brush masks are coverage (no TRC), patterns are `R'G'B'(A) u8`, palette entries `R'G'B' u8`.
- Histogram/levels/curves have their own per-filter "linear/non-linear/perceptual" switches (`GimpTRCType`), outside this doc.
- Dithering on precision reduction: `gimp_image_convert_precision(… layer_dither, text_layer_dither, mask_dither …)` using `gegl:dither` (`GeglDitherMethod`).

### 8.4 Colour management overview

- Image profile: `gimp_image_get_color_profile / _set_color_profile / _assign_color_profile` (re-tag) / `_convert_color_profile(image, dest_profile, intent, bpc, progress, error)` (convert pixels: `gimp_gegl_convert_color_profile`), stored as the `icc-profile` parasite (`gimp_image_get/set_icc_parasite`, `_get/set_icc_profile`, `_validate_icc_profile`); `gimp_image_get/set_use_srgb_profile` + `hidden_profile` ("treat as sRGB" toggle); import policy `GimpColorProfilePolicy` (ASK, KEEP, CONVERT_BUILTIN, CONVERT_PREFERRED) in `gimp_image_import_color_profile`.
- No profile → built-in sRGB (`gimp_color_profile_new_rgb_srgb()`); other built-ins: `_rgb_srgb_linear`, `_rgb_adobe`, `_d65_gray_srgb_trc`, `_d65_gray_linear`, `_d50_gray_lab_trc`, `_new_linear_from_color_profile`, `_new_srgb_trc_from_color_profile` (`libgimpcolor/gimpcolorprofile.h`). `GimpColorProfile` wraps an lcms2 profile; `gimp_color_profile_get_space(profile, intent, error)` yields the **babl space** used to tag every layer format (`image->layer_space`), which is how babl conversions become profile-aware.
- `GimpColorTransform` (`libgimpcolor/gimpcolortransform.c`): babl fish when both profiles are babl-representable matrix/TRC profiles, else an lcms2 transform; cached per image: `transform_to_srgb_u8`, `transform_to_srgb_double`, `transform_from_srgb_double`.
- Soft-proofing: `simulation_profile`, `simulation_intent` (`GimpColorRenderingIntent`: PERCEPTUAL, RELATIVE_COLORIMETRIC, SATURATION, ABSOLUTE_COLORIMETRIC), `simulation_bpc`; display filters and monitor profile handling live in `app/display` / `libgimpwidgets` (out of scope).
- Port stance: v1 = assume sRGB primaries (linear working space, sRGB TRC for non-linear/perceptual); preserve the ICC blob on import/export; route real colour management through the GEGL/babl bridge later.

---

## 9. Portability map

### 9.1 Near-standalone code (pure array/float loops; port directly)

| What | File(s) | Only needs replacing |
|---|---|---|
| Blend functions (all except the three luminance ones) | `app/operations/layer-modes/gimpoperationlayermode-blend.c` | `gfloat/gint`, `MIN/MAX/CLAMP`, unused `GeglOperation*` arg |
| `luma_darken_only`, `luma_lighten_only`, `luminance` | same | `babl_space_get_rgb_luminance` (3 constants per space), babl `RGBA→Y` fish, `gegl_scratch_new` |
| Composite functions (8) | `gimpoperationlayermode-composite.c` | nothing but types |
| Special mode loops | `gimpoperation{normal,behind,replace,erase,merge,split,antierase,overwrite}.c` (`*_process`) | `GimpOperationLayerMode` fields `opacity`, `composite_mode` |
| Dissolve | `gimpoperationdissolve.c` | **`GRand`** (MT19937 + `g_rand_int_range`) |
| Legacy modes + legacy HSV/HSL | `app/operations/layer-modes-legacy/*.c`, `gimpcolor-legacy.c` | types, `g_return_if_fail` |
| Mode table / defaults | `gimp-layer-modes.c` | drop `GeglOperation` cache and babl format lookup; keep the table as `constexpr` data |
| Generated brush rasteriser | `app/core/gimpbrushgenerated.c` (`gauss`, `_calc_lut`, `_get_size`, `_calc`) | `GimpTempBuf`, `gegl_scratch`, `GimpVector2` |
| Sub-pixel kernels | `app/paint/gimpbrushcore-kernels.h` | none (static tables) |
| Mask subsample / pressurize / solidify | `app/paint/gimpbrushcore-loops.cc` | `gegl_parallel_distribute_range/area`, `GimpTempBuf`, babl format identity tests |
| Bitmap brush transform + blur | `app/core/gimpbrush-transform.cc` | `GimpMatrix3`, `GimpTempBuf`, `gegl_parallel_*` |
| Stroke interpolation maths | `app/paint/gimpbrushcore.c` (`gimp_brush_core_interpolate`) | pull the function out of the GObject; `g_rand_*` for jitter |
| Paint canvas-accumulation loops | `app/paint/gimppaintcore-loops.cc` (C++ template mix-ins) | `GeglBufferIterator` — rewrite as plain loops over rectangles (the template machinery is only there to fuse passes) |
| Smudge / dodge-burn / convolve inner loops | `app/gegl/gimp-gegl-loops.cc` | `GeglBufferIterator`, `gegl_parallel_distribute_area` |
| Heal Laplace solver | `app/paint/gimpheal.c` (`gimp_heal_laplace_iteration/_loop`, `gimp_heal`) | `GeglBuffer` get/set at the edges |
| Ink blobs | `app/paint/gimpink-blob.c` | `g_malloc`, types |
| Gradient segment maths | `app/core/gimpgradient.c` (`gimp_gradient_calc_*_factor`, HSV hue interpolation) | `GeglColor` endpoints → RGBA structs |
| Gradient shapes, repeat, dither | `app/operations/gimpoperationgradient.c` (`gradient_calc_*_factor`, `gradient_render_pixel`, `gradient_dither_pixel`) | `GeglSampler` for the shapeburst distance map, `GRand` |
| Adaptive supersampling, bilinear | `libgimpcolor/gimpadaptivesupersample.c`, `gimpbilinear.c` | callbacks only |
| Mask combine + AA ellipse/round-rect | `app/gegl/gimp-gegl-mask-combine.cc` | iterator wrappers |
| Colour distance for fuzzy select | `app/core/gimppickable-contiguous-region.cc` (`pixel_difference`) | none; the flood fill needs a row accessor + `GQueue` → `std::deque` |
| Grow / shrink / border / flood kernels | `app/operations/gimpoperation{grow,shrink,border,flood}.c` | GeglOperation shell around row-buffer algorithms |
| Boundary tracing | `app/core/gimpboundary.c` | `GeglBuffer` row reads, `g_renew` |
| Matrix / vector maths | `libgimpmath/gimpmatrix.c`, `gimpvector.c` | GType boxed registration, `g_return_if_fail` |
| Transform matrices & clip rectangles | `app/core/gimp-transform-utils.c`, `gimp-transform-resize.c`, `gimp-transform-3d-utils.c` | types only |
| XCF RLE codec | `app/xcf/xcf-load.c` (`xcf_load_tile_rle`), `xcf-save.c` (`xcf_save_tile_rle`) | none |
| File parsers `.gbr .gih .vbr .pat .ggr .gpl` | `app/core/gimp{brush,brushpipe,brushgenerated,pattern,gradient,palette}-load.c` | `GInputStream`/`GDataInputStream`, `g_ntohl`, `gimp_ascii_strtod`, `gimp_any_to_utf8` (tiny; rewrite) |

### 9.2 GObject/GEGL-entangled (re-design, don't transliterate)

| Area | Why entangled | Tetra approach |
|---|---|---|
| `GimpImage`, `GimpItem`, `GimpDrawable`, `GimpLayer`, `GimpGroupLayer`, `GimpChannel`, `GimpSelection` | GObject properties/signals, `GimpContainer` trees, GeglBuffer storage, per-item GeglNode graphs | plain C++ document model (value-semantic structs + tile store); explicit dirty-rect propagation instead of signals |
| Projection (`gimpprojection.c`, `gimpfilterstack.c`, node graphs) | GEGL graph evaluation, idle chunk rendering | native recursive tile compositor implementing §1.5 + §2 directly |
| NDE filters (`gimpdrawablefilter.c`, `gimpapplicator.c`) | arbitrary GEGL ops | GEGL bridge (system libgegl) for the op, native compositing of the result |
| Undo (`gimpundo*.c`, ~40 undo classes) | GObject class per undo type, tied to object identity | command pattern over the new model (tile-level COW snapshots) |
| Paint core classes | GObject vtables, `GimpContext`, `GimpSymmetry`, `GimpDynamics`, GeglBuffers | port the algorithms of §3 into non-virtual C++ with explicit state |
| Scan conversion (`gimpscanconvert.c`) | cairo | own AA polygon fill/stroke rasteriser |
| Text (`app/text`) | Pango + cairo + fontconfig | Godot TextServer |
| Line-art fill, foreground select, warp, cage, NoHalo/LoHalo sampling, `gegl:distance-transform` (shapeburst), gaussian blur for feathering | large algorithms or GEGL ops | GEGL bridge first; native later where interactive latency matters (feather = separable Gaussian with σ = r/3.5 is trivial natively) |
| Colour management (`gimpcolorprofile.c`, `gimpcolortransform.c`, `gimpimage-color-profile.c`) | lcms2 + babl spaces | bridge; sRGB-only natively |
| MyPaint brushes | libmypaint | optional third-party lib; out of first scope |
| XCF load/save drivers | `GimpImage` API + GIO | reimplement on the new model from §7 |
| GimpConfig text formats (`.gdyn`, `.gtp`, text/grid/symmetry parasites) | `GScanner`-based s-expression (de)serialiser driven by GObject property introspection (`libgimpconfig`) | small hand-written s-expression reader + per-type property tables |
| Metadata | gexiv2 | preserve blob or use an external lib |

### 9.3 What breaks without GLib (inventory)

- **Types & macros**: `gint/guint/gint32/guint32/gint64/gfloat/gdouble/gboolean/guchar/gsize/goffset/gpointer`, `TRUE/FALSE`, `MIN/MAX/CLAMP/ABS`, `G_PI/G_LN2/G_SQRT2`, `G_N_ELEMENTS`, `G_MAXINT/G_MAXSIZE/G_MAXFLOAT`, `g_return_if_fail/_val_if_fail/_if_reached`, `g_assert`, `G_LIKELY`, `g_alloca`, `RINT/ROUND/SQR` (gimpmath.h). Note `CLAMP`'s evaluation order `x > high ? high : (x < low ? low : x)` (NaN passes through unchanged) vs `SAFE_CLAMP` (NaN → low) — `std::clamp` has UB-adjacent NaN behaviour, write explicit helpers.
- **Random numbers**: `GRand`/`g_random_*` in dissolve mode (deterministic, seeded), brush jitter (`core->rand`), brush-pipe `random` selection (`g_random_int_range`), dynamics random input (`g_random_double_range`), gradient dithering (`g_rand_int`). Only dissolve needs to be reproducible across sessions; for file-level parity with GIMP it needs an MT19937 with GLib's seeding and `int_range` rejection method.
- **Containers / memory**: `GList`, `GSList`, `GQueue`, `GArray`, `GPtrArray`, `GHashTable`, `GString`, `g_new/g_malloc/g_free`, `g_strdup*`, `g_memdup2` → STL.
- **Object system**: GObject properties, signals (`notify::`, `update`, `invalidate-preview`…), interfaces (`GimpPickable`, `GimpProjectable`, `GimpColorManaged`, `GimpProgress`, `GimpRasterizable`), weak pointers, `GValue`/`GParamSpec` (also used by XCF effect arguments and GimpConfig).
- **Byte order & I/O**: `g_ntohl/g_ntohs/GINT64_FROM_BE/GUINT16_FROM_BE`, `GInputStream/GOutputStream/GSeekable/GDataInputStream/GFile`, `g_ascii_strtod` (locale-independent number parsing is required for `.vbr/.ggr` and config files; use `std::from_chars`), `g_utf8_validate`/`gimp_any_to_utf8`.
- **Threads/timers**: `GMutex/GRWLock` (layer-mode fish cache), `GThreadPool/GAsyncQueue` (XCF tile compression), `g_timeout_add` (airbrush), idle sources (projection), `gegl_parallel_distribute_*`.
- **babl/GEGL**: all `babl_format("…")`/`babl_fish`/`babl_process` conversions (TRC, Lab, HSV/HSL, luminance, u8↔float), `GeglBuffer` (sparse tiles, abyss policies, iterators, COW dup), `GeglColor`, `GeglSampler`, `GeglRectangle` helpers (`gegl_rectangle_intersect/_bounding_box/_align_to_buffer`), `gegl_scratch_*`.
- **Other libs**: cairo (scan conversion, text), Pango/HarfBuzz/fontconfig (text), lcms2 (profiles), gexiv2 (metadata), zlib (XCF, also `.xcf.gz` via plug-ins), libmypaint, gdk-pixbuf (pixbuf patterns, previews).

### 9.4 Suggested native porting order (smallest closed sets first)

1. Pixel/TRC helpers (sRGB TRC, Lab D50, luminance weights) + the 65-entry mode table.
2. Blend + composite + special + legacy mode kernels with a test-vector suite (§2); tile compositor with groups, masks, pass-through, last-node rule (§1.5).
3. Selection mask ops: combine rect/ellipse/buffer, invert/all/none, feather (Gaussian σ = r/3.5), sharpen (0.5 threshold), grow/shrink/border, fuzzy select + by-colour (§4).
4. Brush stack: generated brushes (§3.9.3), `.gbr/.gih` loaders, transform/hardness/subsample kernels, spacing interpolation, CONSTANT/INCREMENTAL canvas accumulation, dynamics, symmetry, smoothing (§3.3–3.7); tools Paintbrush/Pencil/Airbrush/Eraser, then Smudge/Clone/Convolve/DodgeBurn, then Heal/Ink.
5. Fill/gradient: bucket fill, gradient shapes + `.ggr`, patterns `.pat`, palettes `.gpl` (§3.9, §4.6–4.7).
6. Transforms with nearest/bilinear/bicubic + matrix helpers and clip modes (§5).
7. XCF import, then export (§7).
8. Bridges: NDE filters, line-art, matting, warp/cage, NoHalo/LoHalo, ICC, text-layer editing via Godot TextServer.

### 9.5 Open items / not determined in this pass

- Exact babl constants/edge cases for TRC thresholds, HSV/HSL achromatic handling, Lab `f()` linear segment (babl not in the pinned sources) — values in §2.7/§8.2 are measurements.
- GLib `GRand` internals (needed for bit-exact Dissolve) — not in the pinned sources.
- Row-shift arithmetic of `GimpTiling`; exact perspective-clone source mapping; `gimp_brush_transform_blur` kernel shape; the 25 `subsample[][]` kernel tables (copy from `gimpbrushcore-kernels.h` when implementing).
- Old `PROP_PATHS` (id 23) record layout (`xcf_load_old_paths`) — only needed for pre-2.4 files.
- Serialised text of the `gimp-text-layer` parasite (no sample dumped); byte-level escaping used by `gimp_config_writer_data` for the generic colour form.
- Licences of `gimp-data` assets (submodule empty).
- Velocity/direction synthesis from raw input events (`app/display/gimpmotionbuffer.c`) — needed for dynamics parity, outside the core directories covered here.
