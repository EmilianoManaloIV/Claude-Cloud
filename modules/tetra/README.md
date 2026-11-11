# modules/tetra — shared editor infrastructure

Everything the Image workspace, the Video workspace and the Blender bridge have
in common: helpers for the [operation schema](../../docs/ARCHITECTURE.md#the-operation-schema),
the widgets generated from it, the pan/zoom canvas base, the engine registry
and the "About Tetra Studio" dialog.

Include the headers from another module as `"tetra/<file>.h"` (the module
directory is on the include path), for example `#include "tetra/tetra_schema.h"`
and `#include "tetra/editor/tetra_param_panel.h"`.

| Class | Header | Registered | What it is |
| --- | --- | --- | --- |
| `TetraSchema` | `tetra_schema.h` | SCENE, static methods | Build, validate, sanitise and search operations. |
| `TetraEngineRegistry` | `tetra_engine_registry.h` | SCENE, static methods | Which engines this build has and whether they work. |
| `TetraParamPanel` | `editor/tetra_param_panel.h` | EDITOR | Form generated from an operation's parameters. |
| `TetraOperationBrowser` | `editor/tetra_operation_browser.h` | EDITOR | Searchable category tree of operations. |
| `TetraOperationDialog` | `editor/tetra_operation_dialog.h` | EDITOR | OK / Cancel / Reset dialog around a `TetraParamPanel`. |
| `TetraCanvasView` | `editor/tetra_canvas_view.h` | EDITOR | Base class of zoomable, pannable 2D document views. |
| `TetraEditorPlugin` | `editor/tetra_editor_plugin.h` | EDITOR (plugin) | Adds Project > Tools > About Tetra Studio... |

All methods listed below are bound, so they can be called from GDScript. The
two SCENE-level classes work in headless test scripts; the widgets use editor
classes (`EditorSpinSlider`, `EditorFileDialog`) and can only be created inside
the running editor (see "Tests and screenshots").

## TetraSchema

Static helpers; nothing is instantiated. `UNSET` (NaN) passed as a limit or
step means "leave the key out".

### Building (for bridge authors)

```cpp
static Dictionary make_operation(const String &p_id, const String &p_label, const String &p_category, const String &p_source, const String &p_description = String());
static Dictionary add_param(Dictionary p_operation, const Dictionary &p_param);   // appends, returns the operation

static Dictionary param_bool(const String &p_name, const String &p_label, bool p_default = false);
static Dictionary param_int(const String &p_name, const String &p_label, int64_t p_default = 0, double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
static Dictionary param_float(const String &p_name, const String &p_label, double p_default = 0.0, double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
static Dictionary param_string(const String &p_name, const String &p_label, const String &p_default = String());
static Dictionary param_text(const String &p_name, const String &p_label, const String &p_default = String());
static Dictionary param_enum(const String &p_name, const String &p_label, const Variant &p_default, const Array &p_options);
static Dictionary param_color(const String &p_name, const String &p_label, const Color &p_default = Color(0, 0, 0, 1));
static Dictionary param_vec2(const String &p_name, const String &p_label, const Vector2 &p_default = Vector2(), double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
static Dictionary param_vec3(const String &p_name, const String &p_label, const Vector3 &p_default = Vector3(), double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
static Dictionary param_file(const String &p_name, const String &p_label, const String &p_default = String());
static Dictionary param_seed(const String &p_name, const String &p_label, int64_t p_default = 0, double p_min = UNSET, double p_max = UNSET);

static Dictionary make_option(const Variant &p_value, const String &p_label);   // one {value, label}
static Array make_options(const PackedStringArray &p_values);                   // value == label

// Optional keys. Each changes the parameter in place and returns it, so calls nest.
static Dictionary set_description(Dictionary p_param, const String &p_description);
static Dictionary set_range(Dictionary p_param, double p_min, double p_max);
static Dictionary set_ui_range(Dictionary p_param, double p_ui_min, double p_ui_max);
static Dictionary set_step(Dictionary p_param, double p_step);
static Dictionary set_unit(Dictionary p_param, const String &p_unit);
static Dictionary set_group(Dictionary p_param, const String &p_group);
static Dictionary set_advanced(Dictionary p_param, bool p_advanced = true);
```

The Dictionaries contain exactly the keys of the schema; optional keys are
present only when given.

```cpp
Dictionary op = TetraSchema::make_operation("gegl:gaussian-blur", "Gaussian Blur", "Blur", "gegl", "Averages neighbouring pixels.");
TetraSchema::add_param(op, TetraSchema::set_ui_range(TetraSchema::param_float("std-dev-x", "Size X", 1.5, 0.0, 1500.0, 0.01, "px"), 0.0, 100.0));
TetraSchema::add_param(op, TetraSchema::param_enum("filter", "Filter", "auto", TetraSchema::make_options({ "auto", "fir", "iir" })));
TetraSchema::add_param(op, TetraSchema::set_advanced(TetraSchema::param_bool("clip-extent", "Clip to the input extent", true)));
```

### Reading (for everyone)

```cpp
static PackedStringArray get_param_types();   // bool int float string text enum color vec2 vec3 file seed
static PackedStringArray get_sources();       // native gegl mlt kdenlive blender

static Variant get_param_default(const Dictionary &p_param);
static Variant sanitize_value(const Dictionary &p_param, const Variant &p_value);

static Dictionary get_defaults(const Dictionary &p_operation);                                   // name -> default
static Dictionary sanitize_values(const Dictionary &p_operation, const Dictionary &p_values);    // name -> value
static String validate_operation(const Dictionary &p_operation);                                 // "" when valid
static Dictionary find_param(const Dictionary &p_operation, const String &p_name);               // {} when absent

static Dictionary category_tree(const Array &p_operations);
static Array search(const Array &p_operations, const String &p_text);
static PackedInt32Array search_indices(const Array &p_operations, const String &p_text);
```

**Defaults.** `get_param_default` returns the parameter's `default` coerced to
its type and limits. Without a usable default: `false`, `0`, `0.0`, `""`,
`Color(0, 0, 0, 1)`, a zero vector (each moved inside `min`/`max`), and for an
enum the value of its first option.

**Sanitising.** `sanitize_values` returns exactly the operation's parameters in
display order: unknown keys are dropped, missing ones get their default, and
every value is converted as follows. A value that cannot be converted becomes
the default.

| Type | Accepted | Result |
| --- | --- | --- |
| `bool` | bool; number (non-zero is true); `"true"`, `"yes"`, `"on"`, `"1"`, `"false"`, `"no"`, `"off"`, `"0"` in any case | bool |
| `int` | int; float (rounded to nearest); bool; numeric String | int clamped to the integers inside `min`/`max` |
| `float` | finite number; bool; numeric String | float clamped to `min`/`max` |
| `string`, `text`, `file` | String, StringName, NodePath, bool, number | String |
| `enum` | the `value` of an option (2 and 2.0 are the same; `"2"` also selects the option 2). Comparison is case-sensitive; labels do not count | that option's `value` |
| `color` | Color; HTML or named colour String; Vector3 (alpha 1); Vector4; array of 3 or 4 numbers | Color, not clamped |
| `vec2`, `vec3` | vector of the same size (float or integer), array of that many numbers, or one number for all components | Vector2 / Vector3, each component clamped |
| `seed` | as `int` | int clamped to `min`/`max`, which default to 0 and 4294967295 |

`step` is a hint for the UI: values are not snapped to it.

**Validation.** `validate_operation` returns one line per problem, for example
`param 'radius': 'default' is outside 'min'/'max'`. It checks: `id` and
`label` are non-empty Strings; `category` is a String (it may be empty);
`source` is one of `get_sources()`; `params` is an Array of Dictionaries with
unique non-empty `name`, non-empty `label`, a known `type`, a `default` of the
matching Variant type (an `int` default may be a whole float, as JSON produces)
that lies inside the limits, finite numeric `min`/`max`/`ui_min`/`ui_max`/`step`
with `min <= max`, `ui_min <= ui_max` and `step > 0`, String
`description`/`unit`/`group`, bool `advanced`, and for an enum a non-empty
`options` Array of `{value: String or int, label: String}` with distinct values
one of which is the default. Keys the schema does not define are ignored.

**Category tree.** `category_tree` returns the root node. A node is

```
{ "name": String, "path": String,        # "Edges", "Mesh/Edges"; both empty for the root
  "count": int,                           # operations here and below
  "categories": Array of nodes,           # sorted by name, natural order, case-insensitive
  "operations": Array of operation ids }  # sorted by label
```

Category segments are trimmed, so `Mesh / Edges` and `Mesh/Edges` are the same
folder. Operations with an empty category belong to the root.

**Search.** `search` splits the text into words; an operation matches when every
word occurs in its label, id, category or description (case-insensitive). Per
word the best of these counts: label equals the word 100, label starts with it
90, a word of the label starts with it 70, inside the label 60, the id's local
name (after the last `:`) starts with it 50, inside the id 40, inside the
category 25, inside the description 10. The whole phrase adds 1000 when it
equals the label, 500 when the label starts with it and 250 when the label
contains it. Ties go to the shorter label, then alphabetically. An empty text
returns every operation in the given order. `search_indices` returns the same
ranking as positions in the input Array.

```gdscript
var op := TetraSchema.make_operation("native:blur", "Blur", "Blur", "native")
TetraSchema.add_param(op, TetraSchema.param_float("radius", "Radius", 4.0, 0.0, 100.0, 0.1, "px"))
assert(TetraSchema.validate_operation(op) == "")
var values := TetraSchema.sanitize_values(op, {"radius": "250", "bogus": 1})   # {"radius": 100.0}
```

## TetraEngineRegistry

```cpp
static void add_engine(const String &p_name, const String &p_version, bool p_available, const String &p_details = String());
static TypedArray<Dictionary> get_engines();              // [{name, version, available, details}, ...] in order of first registration
static Dictionary get_engine(const String &p_name);       // {} when unknown; names compare case-insensitively
static bool is_engine_available(const String &p_name);
```

Call `add_engine` from your module's `initialize_*_module` (any level; the list
does not depend on module order), once per engine, and again whenever the
state changes (a second call with the same name replaces the entry). `details`
is one line: where the engine was found or why it is unavailable. The About
dialog lists the entries.

```cpp
#include "tetra/tetra_engine_registry.h"

#ifdef TETRA_HAVE_GEGL
TetraEngineRegistry::add_engine("GEGL", gegl_version_string, true, "libgegl-0.4, 312 operations");
#else
TetraEngineRegistry::add_engine("GEGL", "", false, "Built without libgegl-0.4.");
#endif
```

## TetraParamPanel : VBoxContainer

One row per parameter: the label on the left (tooltip = `description`,
ellipsised when narrow) and a control for the type on the right.

| Type | Control |
| --- | --- |
| `bool` | `CheckBox` |
| `int`, `float` | `EditorSpinSlider`. The slider spans `ui_min`..`ui_max` (falling back to `min`..`max`); typed values may go beyond the slider and are clamped to `min`/`max`. Without a range there is no slider (an int gets up/down arrows). `step` defaults to 1 and 0.001; `unit` is the suffix. |
| `enum` | `OptionButton` |
| `string` | `LineEdit`, reported on every keystroke |
| `text` | `TextEdit` |
| `color` | `ColorPickerButton` with alpha |
| `vec2`, `vec3` | `EditorSpinSlider`s labelled x, y, z, on one line where there is room and wrapping in a narrow panel |
| `file` | `LineEdit` + browse button (`EditorFileDialog`, whole file system). The path is reported on Enter, on leaving the field and when chosen in the dialog. |
| `seed` | integer field + "Randomize" button |

Parameters without a `group` come first; each group follows under a heading, in
the order the groups first appear. `advanced` parameters sit in a foldable
"Advanced" section at the end, which starts folded. An operation without
parameters shows a short note instead.

Two optional keys, used only by the browse button of a `file` parameter and
ignored everywhere else: `file_mode` (`"open"` by default, `"save"` or `"dir"`)
and `filters` (Array of `FileDialog` filter Strings such as `"*.png, *.jpg ; Images"`).

```cpp
void set_operation(const Dictionary &p_operation);   // rebuilds the form, every value at its default
Dictionary get_operation() const;
Dictionary get_values() const;                       // name -> value, always sanitised
void set_values(const Dictionary &p_values);         // sets the given names; others keep their value; unknown names ignored
void set_value(const String &p_name, const Variant &p_value);
Variant get_value(const String &p_name) const;
bool has_param(const String &p_name) const;
void reset_to_defaults();
void set_read_only(bool p_read_only);
bool is_read_only() const;
void set_scrollable(bool p_scrollable);              // default true
bool is_scrollable() const;
void set_advanced_folded(bool p_folded);             // default true; kept across set_operation
bool is_advanced_folded() const;
Size2 get_form_minimum_size() const;                 // size needed to show the form without scrolling
```

Signal: `value_changed(name: String, value: Variant)` — emitted when the user
changes a value, with the sanitised value, and only when it really changed.
`set_operation`, `set_values`, `set_value` and `reset_to_defaults` do not emit it.

The panel scrolls vertically when it is shorter than its form, so its own
minimum height is small: give it `SIZE_EXPAND_FILL` or a custom minimum size.
Inside something that already scrolls (a stack of effect panels), call
`set_scrollable(false)`; the panel then asks for the full height of its form.
The minimum width is about 190 px at 1x.

```cpp
TetraParamPanel *panel = memnew(TetraParamPanel);
panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
add_child(panel);
panel->set_operation(operation);
panel->set_values(stored_values);
panel->connect("value_changed", callable_mp(this, &MyDock::_param_changed));   // void _param_changed(const String &p_name, const Variant &p_value)
```

## TetraOperationBrowser : VBoxContainer

A search field, a `Tree` and a description area. Without a search text the
operations are in folders following their category path; with one, the best
200 matches are listed flat with their category beside them. Folders start
collapsed when there are more than 40 operations in more than one top-level
category; the folders leading to the selection are always opened, and folders
the user opens or closes stay that way. A click on a folder row toggles it.
In the search field the arrow keys move through the list and Enter activates
the selection.

```cpp
void set_operations(const Array &p_operations);   // Array of operation Dictionaries; keeps the selection if its id still exists
Array get_operations() const;
String get_selected_id() const;                   // "" when nothing is selected
Dictionary get_selected_operation() const;        // {} when nothing is selected
void select_operation(const String &p_id);        // selects and reveals; unknown or "" clears; ends a search that hides it
void set_filter_text(const String &p_text);       // selects the best match while the text is not empty
String get_filter_text() const;
void set_description_visible(bool p_visible);
bool is_description_visible() const;
void focus_search();
```

Signals:

- `operation_selected(id: String)` — the user selected another operation by
  clicking, with the keyboard, or by typing a search text (the best match is
  selected). Not emitted by `select_operation`, `set_operations` or
  `set_filter_text`, and never with an empty id.
- `operation_activated(id: String)` — double-click or Enter on an operation.

Operations are identified by `id`; of two operations with the same id only the
first is shown. Listing 1500 operations and searching them each take about
13 ms (software rendering under Xvfb, measured by `shots/operation_browser.gd`).

```cpp
TetraOperationBrowser *browser = memnew(TetraOperationBrowser);
browser->set_v_size_flags(Control::SIZE_EXPAND_FILL);
add_child(browser);
browser->set_operations(bridge->get_operations());
browser->connect("operation_activated", callable_mp(this, &MyDock::_run_operation));   // void _run_operation(const String &p_id)
```

## TetraOperationDialog : ConfirmationDialog

Title = the operation's label; content = a scrolling `TetraParamPanel`;
buttons OK, Cancel and Reset (back to the defaults); optionally a "Preview"
check box below the form. The dialog is sized to show the whole form, up to
80 % of the screen.

```cpp
void popup_operation(const Dictionary &p_operation, const Dictionary &p_initial_values = Dictionary());
Dictionary get_operation() const;
Dictionary get_values() const;
TetraParamPanel *get_param_panel() const;
void set_preview_enabled(bool p_enabled);    // shows the "Preview" check box; default off
bool is_preview_enabled() const;
void set_preview_checked(bool p_checked);    // default checked; does not emit preview_toggled
bool is_preview_checked() const;             // true only while the box is shown and checked
```

Signals:

- `values_changed(values: Dictionary)` — 150 ms after the user's last edit or
  Reset, with all values. Emitted whether or not Preview is checked; it is not
  emitted by `popup_operation`, and a pending one is dropped by OK and Cancel.
- `preview_toggled(checked: bool)` — the user toggled the Preview check box.
- `operation_confirmed(id: String, values: Dictionary)` — OK.
- `operation_cancelled(id: String)` — Cancel, Escape or the close button.

The dialog must be in the tree before `popup_operation` is called.

```cpp
dialog = memnew(TetraOperationDialog);
dialog->set_preview_enabled(true);
add_child(dialog);
dialog->connect("values_changed", callable_mp(this, &MyEditor::_update_preview));          // void (const Dictionary &p_values)
dialog->connect("operation_confirmed", callable_mp(this, &MyEditor::_apply_operation));    // void (const String &p_id, const Dictionary &p_values)
dialog->connect("operation_cancelled", callable_mp(this, &MyEditor::_drop_preview));       // void (const String &p_id)
...
dialog->popup_operation(operation, last_used_values);
```

## TetraCanvasView : Control

Base class of 2D document views. The content has a size in document units and
is shown zoomed and panned:

```
view position = (content position - view offset) * zoom
```

Navigation is built in: the mouse wheel zooms about the cursor (1.2x per notch),
Ctrl+wheel and pinch zoom, Shift+wheel scrolls sideways, middle-drag and
Space+left-drag pan (the editor's `canvas_item_editor/pan_view` shortcut and
panning settings apply). After `set_wheel_scrolls(true)` the plain wheel scrolls
vertically instead and only Ctrl+wheel zooms. Scroll bars appear on the axes
where the zoomed content is larger than the view. The centre of the view always
stays over the content: small content can be moved about, and large content can
be scrolled half a view past its edges. The offset is kept on whole view pixels.
A view that has not been positioned yet fits its content when it first gets a
size.

```cpp
void set_content_size(const Size2 &p_size);
Size2 get_content_size() const;
void set_zoom(real_t p_zoom, const Vector2 &p_view_pivot);   // keeps the content under the pivot (view pixels) in place
void set_zoom_centered(real_t p_zoom);                       // pivot = centre of the view
real_t get_zoom() const;
void set_zoom_limits(real_t p_min, real_t p_max);            // default 1/64 .. 256
real_t get_zoom_min() const;
real_t get_zoom_max() const;
void zoom_to_fit();                                          // whole content, 16 px margin, centred
void center_content();
void set_view_offset(const Vector2 &p_offset);               // content position at the view's top-left corner
Vector2 get_view_offset() const;
Vector2 view_to_content(const Vector2 &p_view_position) const;
Vector2 content_to_view(const Vector2 &p_content_position) const;
Rect2 get_visible_content_rect() const;                      // part of the content inside the view
void set_wheel_scrolls(bool p_scrolls);
bool is_wheel_scrolling() const;
bool is_panning() const;                                     // pan key held or pan drag under way
```

Signal: `view_changed` — the zoom, the offset, the content size or the size of
the view changed.

Hooks for subclasses (protected):

```cpp
virtual void _canvas_draw();                                      // during NOTIFICATION_DRAW, in view pixels
virtual bool _canvas_gui_input(const Ref<InputEvent> &p_event);   // return true to consume

void draw_set_transform_to_content();   // following draw_* calls take content coordinates
void draw_reset_transform();            // back to view pixels
void draw_checkerboard(const Rect2 &p_content_rect);   // the editor's transparency checkerboard; resets the transform
```

`_canvas_gui_input` sees every event before the base uses it for navigation,
except while a pan is armed or under way (`is_panning()`): then the pan gets
the mouse first, so Space+drag does not also paint. Do not override
`gui_input` or handle `NOTIFICATION_DRAW` yourself. The checkerboard squares
keep their size on screen at every zoom and move with the content. The three
`draw_*` helpers are also bound, so a GDScript subclass can call them from
`_draw()`.

```cpp
class PaintCanvas : public TetraCanvasView {
	GDCLASS(PaintCanvas, TetraCanvasView);

protected:
	virtual void _canvas_draw() override {
		draw_checkerboard(Rect2(Point2(), get_content_size()));
		draw_set_transform_to_content();
		draw_texture(layer_texture, Point2());
		draw_reset_transform();
	}
	virtual bool _canvas_gui_input(const Ref<InputEvent> &p_event) override {
		Ref<InputEventMouseButton> button = p_event;
		if (button.is_valid() && button->get_button_index() == MouseButton::LEFT) {
			paint_at(view_to_content(button->get_position()));
			return true;
		}
		return false;
	}
};
```

## TetraEditorPlugin and the About dialog

Registered with `EditorPlugins::add_by_type` at EDITOR level. It adds
**Project > Tools > About Tetra Studio...**, an `AcceptDialog` that says what
Tetra is, names the four upstream programs with the versions pinned in
`upstream/` (Godot's is the running editor's own), and lists every engine in
`TetraEngineRegistry` with its version, availability and details.

```cpp
static TetraEditorPlugin *get_singleton();   // null outside the editor
void show_about();
AcceptDialog *get_about_dialog() const;
```

When an upstream pin changes, update `UPSTREAMS` in `editor/tetra_editor_plugin.cpp`.

## Tests and screenshots

```sh
scripts/test.sh tetra     # headless: test_schema.gd, test_engine_registry.gd, test_host.gd
```

The widget classes belong to the editor API, which ClassDB only instantiates
inside the running editor, so a headless `--script` test cannot create them.
Their behaviour is checked by a driver that runs in the real editor (under
Xvfb) and prints `screenshot widget checks: <n> checks, <m> failed`, with one
`screenshot FAILED: ...` line per failure:

```sh
scripts/screenshot.sh build/screens/tetra-widget-checks.png 3D res://tetra/shots/widget_checks.gd
```

The other drivers in `tests/godot/project/tetra/shots/` set up a widget for a
picture:

```sh
scripts/screenshot.sh build/screens/tetra-operation-dialog.png 3D res://tetra/shots/operation_dialog.gd
scripts/screenshot.sh build/screens/tetra-operation-browser.png 3D res://tetra/shots/operation_browser.gd
scripts/screenshot.sh build/screens/tetra-param-panel.png 3D res://tetra/shots/param_panel.gd
scripts/screenshot.sh build/screens/tetra-canvas-view.png 3D res://tetra/shots/canvas_view.gd
scripts/screenshot.sh build/screens/tetra-about.png 3D res://tetra/shots/about.gd
```

A driver sets `root.gui_embed_subwindows = true` first: dialogs are separate OS
windows otherwise and would be missing from the capture.
