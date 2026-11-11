# 02 — Godot 4.7.2 engine APIs the Tetra workspaces build on

Scope: 3D viewport editing, meshes, images/textures, audio/video, processes/threads/IPC, the existing
Blender importer, linking system libraries from a custom module, and scene-integration hooks.

Everything below was read out of `/home/user/Claude-Cloud/upstream/godot` (4.7.2-stable, `version.py`:
`major=4 minor=7 patch=2 status="stable"`, `short_name="godot"`, `name="Tetra Studio"` after the
branding patch). Paths are relative to that root. Line numbers are those of the pinned tree and are
given so that a claim can be re-checked quickly; signatures are copied from the headers.

Toolchain observed on the build host: GCC 13.3.0, clang 18.1.3, `pkg-config` 1.8.1; `gegl-0.4` and
`mlt++-7` both resolve through pkg-config. `scripts/build.sh` builds with
`platform=linuxbsd target=editor debug_symbols=no scu_build=yes compiledb=yes custom_modules=<repo>/modules`
(+ `linker=mold|lld` when available). `warnings`, `werror`, `disable_exceptions` are left at their
defaults (see §7).

---

## 0. Cross-cutting facts that differ from "the Godot 4 you remember"

| Topic | 4.7.2 reality | Evidence |
|---|---|---|
| Editor source layout | Reorganised. 3D editor is `editor/scene/3d/`, gizmo plugins `editor/scene/3d/gizmos/`, `EditorPlugin` is `editor/plugins/editor_plugin.h`, `EditorFileSystem`/`EditorPaths` are `editor/file_system/`, `EditorSettings` is `editor/settings/editor_settings.h`, preview system is `editor/inspector/editor_resource_preview.h`, docks are `editor/docks/`, texture editors `editor/scene/texture/`. | directory listing |
| RenderingServer enums | Moved out of the class into `namespace RenderingServerEnums`, aliased `#define RSE RenderingServerEnums` (`servers/rendering/rendering_server_enums.h`, last line). Write `RSE::ARRAY_MAX`, `RSE::PRIMITIVE_LINES`, `RSE::SHADOW_CASTING_SETTING_OFF`, `RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING`, `RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR`. `RS::ARRAY_MAX` no longer exists. `Mesh::ARRAY_*` still exists and is defined as `= RSE::ARRAY_*`. | `scene/resources/mesh.h:61-169` |
| RenderingServer structs | `RenderingServerTypes::SurfaceData`, `::TextureInfo`, `::MeshInfo` … in `servers/rendering/rendering_server_types.h`. | `rendering_server.h:193` |
| `RS` | Still `#define RS RenderingServer` (`servers/rendering/rendering_server.h:1175`). | |
| `RS::free()` | Deprecated; use `free_rid(RID)` (`rendering_server.h:951-955`). | |
| `Geometry3D` | A **namespace** of free/inline functions in `core/math/geometry_3d.h` (not a class with statics). Call syntax `Geometry3D::ray_intersects_triangle(...)` is unchanged. | `geometry_3d.h:40` |
| 3D navigation | Factored into `View3DController` (`scene/debugger/view_3d_controller.h`); the region-select rectangle lives in `view_3d_controller->cursor`. | `node_3d_editor_plugin.h:37,395` |
| Bottom panel / docks | `EditorPlugin::add_control_to_bottom_panel` / `add_control_to_dock` are inside `#ifndef DISABLE_DEPRECATED` and `protected`. New API: subclass `EditorDock` (`editor/docks/editor_dock.h`) and call `EditorPlugin::add_dock(EditorDock*)` or `EditorDockManager::get_singleton()->add_dock(dock)`. | `editor_plugin.h:148-168`, GridMap plugin |
| New GPU texture API | `DrawableTexture2D` (`scene/resources/drawable_texture_2d.h`) + `shader_type texture_blit;` + `RS::texture_drawable_blit_rect(...)`: render a rect into a texture with a shader, no SubViewport. Directly relevant to the painter (§3.6). | |
| Sockets | `StreamPeerTCP` now derives from `StreamPeerSocket`; there is also `StreamPeerUDS` / `UDSServer` (Unix domain sockets) in `core/io/`. | §5.4 |
| `callable_mp` | Header is `core/object/callable_mp.h` (must be included explicitly, in-tree code does). | |
| RTTI / exceptions | RTTI is on (no `-fno-rtti` anywhere). Exceptions are **off** by default: `-fno-exceptions` on all C++ (`SConstruct:931-935`). | §7 |

### 0.1 Module registration pattern (what every Tetra module's `register_types.cpp` must look like)

Modelled on `modules/gridmap/register_types.cpp:43-56` and `modules/gltf/register_types.cpp:111-163`:

```cpp
#include "register_types.h"
#include "core/object/class_db.h"
#ifdef TOOLS_ENABLED
#include "editor/plugins/editor_plugin.h"   // EditorPlugins::add_by_type
#include "editor/editor_node.h"             // EditorNode::add_init_callback
#endif

void initialize_tetra_model_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// GDREGISTER_CLASS(...) for runtime classes.
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// GDREGISTER_CLASS(...) for editor-only classes if they must be visible to ClassDB.
		EditorPlugins::add_by_type<TetraModelEditorPlugin>();   // instantiated by EditorNode later
		// EditorNode::add_init_callback(_editor_init);         // runs once EditorNode exists (gltf does this)
	}
#endif
}
void uninitialize_tetra_model_module(ModuleInitializationLevel p_level) {}
```

`EditorPlugins::add_by_type<T>()` stores a creator in a fixed array `MAX_CREATE_FUNCS = 128`
(`editor_plugin.h:282-311`). The plugin constructor runs when `Node3DEditor::get_singleton()` already
exists (the Path3D plugin constructor calls it, `path_3d_editor_plugin.cpp:1022`), provided the 3D
editor plugin is created first — which is true for module plugins: `EditorNode`'s constructor does
`add_editor_plugin(memnew(Node3DEditorPlugin));` at `editor/editor_node.cpp:9392` and only afterwards
`for (int i = 0; i < EditorPlugins::get_plugin_count(); i++) { add_editor_plugin(EditorPlugins::create(i)); }`
(`:9420-9421`). GridMap defers creation of its UI to `NOTIFICATION_ENTER_TREE`
(`grid_map_editor_plugin.cpp:1965-1972`), which is the safest pattern.

### 0.2 Include cheat-sheet (exact paths in this tree)

| Need | Header |
|---|---|
| `callable_mp`, `callable_mp_static` | `core/object/callable_mp.h` |
| `GDCLASS`, `ClassDB::bind_method`, `D_METHOD`, `GDREGISTER_CLASS` | `core/object/class_db.h` |
| `TTR()`, `TTRC()`, `vformat`, `String` | `core/string/ustring.h` (`TTR` declared at `:744`) |
| `SNAME()` | `core/string/string_name.h` |
| `SceneStringName(pressed)` etc. | `scene/scene_string_names.h` |
| `EditorStringName(EditorIcons)`, `EditorStringName(Editor)` | `editor/editor_string_names.h` |
| `EDSCALE` | `editor/themes/editor_scale.h` |
| `EDITOR_GET`, `EDITOR_DEF`, `ED_SHORTCUT`, `ED_IS_SHORTCUT`, `ED_GET_SHORTCUT`, `EditorSettings` | `editor/settings/editor_settings.h` |
| `GLOBAL_GET`, `GLOBAL_DEF`, `ProjectSettings` | `core/config/project_settings.h` |
| `EditorNode`, `EditorProgress` | `editor/editor_node.h` |
| `EditorMainScreen` | `editor/editor_main_screen.h` |
| `EditorUndoRedoManager` | `editor/editor_undo_redo_manager.h` |
| `EditorDock`, `EditorDockManager` | `editor/docks/editor_dock.h`, `editor/docks/editor_dock_manager.h` |
| `EditorFileSystem`, `EditorPaths` | `editor/file_system/editor_file_system.h`, `editor/file_system/editor_paths.h` |
| `EditorFileDialog` | `editor/gui/editor_file_dialog.h` |
| `Key`, `KeyModifierMask`, `keycode_get_string` | `core/os/keyboard.h` |
| `Input` (mouse mode, key state) | `core/input/input.h` |
| `InputEventMouseButton/Motion/Key/PanGesture/MagnifyGesture` | `core/input/input_event.h` |
| `RenderingServer`, `RS`, `RSE` | `servers/rendering/rendering_server.h` |
| `Geometry2D`, `Geometry3D` | `core/math/geometry_2d.h`, `core/math/geometry_3d.h` |
| `MeshInstance3D`, `Camera3D` | `scene/3d/mesh_instance_3d.h`, `scene/3d/camera_3d.h` |
| `ArrayMesh`, `SurfaceTool`, `ImmediateMesh`, `StandardMaterial3D` | `scene/resources/mesh.h`, `scene/resources/surface_tool.h`, `scene/resources/immediate_mesh.h`, `scene/resources/material.h` |
| `Image`, `ImageLoader`, `ImageTexture` | `core/io/image.h`, `core/io/image_loader.h`, `scene/resources/image_texture.h` |

Editor-only code in a module must sit behind `#ifdef TOOLS_ENABLED` or live in files that are only added to
the build when `env.editor_build` is true (the pattern used by `modules/theora/SCsub` and `modules/gridmap`).

---

## 1. Editing inside the 3D viewport (Blender-style Edit Mode on `MeshInstance3D`)

### 1.1 Where things live

| Class | Header | Notes |
|---|---|---|
| `EditorPlugin`, `EditorPlugins` | `editor/plugins/editor_plugin.h` | `Node` subclass |
| `EditorPluginList` | `editor/plugins/editor_plugin_list.h/.cpp` | the three forwarding lists |
| `Node3DEditor`, `Node3DEditorViewport`, `Node3DEditorViewportContainer`, `Node3DEditorSelectedItem`, `Node3DEditorPlugin`, `ViewportRotationControl`, `ViewportNavigationControl` | `editor/scene/3d/node_3d_editor_plugin.h` (1103 lines; .cpp is 11342 lines) | |
| `EditorNode3DGizmo`, `EditorNode3DGizmoPlugin` | `editor/scene/3d/node_3d_editor_gizmos.h/.cpp` | |
| `Gizmo3DHelper` | `editor/scene/3d/gizmos/gizmo_3d_helper.h` | box/cylinder/capsule/cone handle maths |
| `View3DController` | `scene/debugger/view_3d_controller.h` | navigation, cursor, nav scheme enums |
| Built-in gizmo plugins | `editor/scene/3d/gizmos/*_gizmo_plugin.{h,cpp}` | `mesh_instance_3d_gizmo_plugin.cpp` is the one for `MeshInstance3D` |
| Path3D editor | `editor/scene/3d/path_3d_editor_plugin.{h,cpp}` | subgizmo reference implementation |
| Polygon3D editor | `editor/scene/3d/polygon_3d_editor_plugin.{h,cpp}` | child `MeshInstance3D` overlay pattern |
| Skeleton3D editor | `editor/scene/3d/skeleton_3d_editor_plugin.{h,cpp}` | `AFTER_GUI_INPUT_CUSTOM` + subgizmos + custom point shader |
| MeshInstance3D editor | `editor/scene/3d/mesh_instance_3d_editor_plugin.{h,cpp}` | "Mesh" menu in the 3D toolbar |
| GridMap editor | `modules/gridmap/editor/grid_map_editor_plugin.{h,cpp}` | a *module* doing in-viewport editing with raw RS instances; closest structural model for Tetra |
| CSG gizmos | `modules/csg/editor/csg_gizmos.{h,cpp}` | a module gizmo plugin |
| `EditorUndoRedoManager` | `editor/editor_undo_redo_manager.h` | |
| `EditorSelection` | `editor/editor_data.h` | `get_node_editor_data<T>()` |

### 1.2 Plugin activation: `handles` → `edit` → `make_visible`

Signatures (`editor_plugin.h:192-219`):

```cpp
virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
virtual void forward_3d_draw_over_viewport(Control *p_overlay);
virtual void forward_3d_force_draw_over_viewport(Control *p_overlay);
virtual String get_plugin_name() const;
virtual const Ref<Texture2D> get_plugin_icon() const;
virtual bool has_main_screen() const;
virtual void make_visible(bool p_visible);
virtual void edit(Object *p_object);
virtual bool handles(Object *p_object) const;
virtual bool can_auto_hide() const;                       // default true
virtual Dictionary get_state() const;  virtual void set_state(const Dictionary &p_state);
virtual void clear();
virtual void edited_scene_changed() {}
int update_overlays() const;
void set_input_event_forwarding_always_enabled();
void set_force_draw_over_forwarding_enabled();
void add_control_to_container(CustomControlContainer p_location, Control *p_control);
void add_node_3d_gizmo_plugin(const Ref<EditorNode3DGizmoPlugin> &p_gizmo_plugin);
```

In C++ you override the virtuals directly (all in-tree plugins do); the `GDVIRTUAL _forward_3d_gui_input`
etc. are only the script-facing fallbacks called by the base implementation (`editor_plugin.cpp:325-337`).

Flow (`editor/editor_node.cpp`):

1. Something is selected/inspected → `EditorNode::push_item()` → `_edit_current()` →
   `editor_data.get_handling_sub_editors(p_object)` collects every non-main-screen plugin whose
   `handles(obj)` is true (`editor_node.cpp:2918`).
2. For a newly active plugin `EditorNode::_plugin_over_edit(plugin, object)` (`editor_node.cpp:1272-1285`):
   `editor_plugins_over->add_plugin(plugin); plugin->edit(object); plugin->make_visible(true);`
   (note: `edit` **before** `make_visible(true)`; for an already-active plugin the order is
   `make_visible(true)` then `edit(obj)`, `editor_node.cpp:2953-2954`).
3. When the object is deselected and `can_auto_hide()` is true:
   `editor_plugins_over->remove_plugin(plugin); plugin->edit(nullptr); plugin->make_visible(false);`
   So `edit(nullptr)` is a normal call — handle it.
4. Only plugins that are in `editor_plugins_over` receive `forward_3d_gui_input` /
   `forward_3d_draw_over_viewport`. A plugin that called `set_input_event_forwarding_always_enabled()`
   is additionally in `editor_plugins_force_input_forwarding` and gets input even when its object is
   not selected; one that called `set_force_draw_over_forwarding_enabled()` gets
   `forward_3d_force_draw_over_viewport` always (`editor_plugin.cpp:257-265`).

`handles()` receives the *edited object*, i.e. the single selected node (or a `MultiNodeEdit` for
multi-selection — a mesh edit plugin whose `handles` does `Object::cast_to<MeshInstance3D>(p_object)`
is simply inactive for multi-selections).

### 1.3 `forward_3d_gui_input`: exact event flow and what the return values do

Enum (`editor_plugin.h:107-111`): `AFTER_GUI_INPUT_PASS`, `AFTER_GUI_INPUT_STOP`, `AFTER_GUI_INPUT_CUSTOM`.

The viewport's input sink is `Node3DEditorViewport::_sinput` (`node_3d_editor_plugin.cpp:2054`), connected
to the `gui_input` signal of the overlay `Control *surface` (`:3887`). Order of processing for **every**
event (mouse button, motion, key, gestures):

1. Numpad emulation rewrite for key events (`:2057-2062`).
2. If a drag-and-drop is in progress → return (`:2064-2067`).
3. **Vertex-snap mode** (shortcut `spatial_editor/vertex_snap`, default **B**). The B key press/release toggles
   `vertex_snap_mode` here, *before plugins run* (`:2069-2100`), and while B is held or a vertex-snap drag is
   active the left/right **mouse buttons are consumed before plugins see them** (`:2106-2196`, ends in
   `return`). Vertex-snap mouse *motion* is handled later, after the plugin stage (`:2633-2671`), so a plugin
   returning `STOP` for motion pre-empts it. Blender's "B = box select" therefore collides with this: the key
   press itself cannot be intercepted by a plugin; the editor shortcut has to be rebound (see 1.10).
4. Freelook redirection (`_redirect_freelook_input`, `:2102`).
5. **Plugins** (`:2198-2230`):
   ```cpp
   EditorPlugin::AfterGUIInput after = EditorPlugin::AFTER_GUI_INPUT_PASS;
   Camera3D *input_camera = previewing ? previewing : camera;
   switch (en->get_editor_plugins_force_input_forwarding()->forward_3d_gui_input(input_camera, p_event, true)) { ... }
   switch (en->get_editor_plugins_over()->forward_3d_gui_input(input_camera, p_event, false)) { ... }
   ```
   `EditorPluginList::forward_3d_gui_input` (`editor_plugin_list.cpp:45-63`) calls **every** plugin in the
   list (no short-circuit) and combines: any `STOP` ⇒ `STOP`; else any `CUSTOM` ⇒ `CUSTOM`; else `PASS`.
   Plugins flagged "always forward" are skipped in the second list to avoid double delivery.
6. `STOP` ⇒ `_sinput` returns immediately: no navigation, no selection, no gizmo, no shortcuts.
7. Otherwise `view_3d_controller->gui_input(p_event, surface->get_global_rect())` handles orbit/pan/zoom
   (`:2233-2237`), then mouse buttons, motion and keys are processed by the built-in editor.

Effect of `AFTER_GUI_INPUT_CUSTOM` — it only disables the editor's *node picking/selection*:

- On LMB press, the "single item selection" block (`clicked = _select_ray(...)`, start of region select)
  is skipped (`:2461-2512`).
- On LMB release, `_select_clicked()` / `_select_region()` are skipped (`:2581-2593`).
- Everything else still runs: gizmo handle hit-testing, the **transform gizmo**
  (`_transform_gizmo_select`, `:2407`), **subgizmo** picking (`:2412-2455`), navigation, shortcuts.

This is exactly what `Skeleton3DEditorPlugin` uses for bone edit mode
(`skeleton_3d_editor_plugin.cpp:1569-1579`): return `CUSTOM` on every event while in edit mode so clicking
never changes the node selection, while the standard transform gizmo keeps working on subgizmos.

Practical table for a mesh Edit Mode plugin:

| Event | Return | Why |
|---|---|---|
| Not in edit mode / no target | `PASS` | behave as stock editor |
| LMB press/release used for element picking, box/lasso select, tool drags | `STOP` (and `update_overlays()`) | otherwise the editor starts node region-select or deselects the node |
| LMB events you did not consume, while in edit mode | `CUSTOM` | keeps the node selected, still lets built-in transform gizmo/subgizmos work |
| Mouse motion during your own drag | `STOP` | prevents gizmo hover highlight + region-select updates |
| Mouse motion otherwise | `PASS` (GridMap returns `PASS` unless painting, `grid_map_editor_plugin.cpp:990-998`) | lets navigation continue |
| MMB / wheel / pan / magnify gestures | `PASS` | navigation lives *after* plugins; returning STOP kills orbit/zoom |
| RMB press | `PASS` unless you cancel an operation | RMB starts freelook (`:2249-2293`); GridMap comments "Allow freelook to be enabled" (`:938`) |
| Alt+LMB under Maya/Modo nav scheme | `PASS` | both Path3D and GridMap check `EDITOR_GET("editors/3d/navigation/navigation_scheme")` against `View3DController::NAV_SCHEME_MAYA/NAV_SCHEME_MODO` and bail out (`path_3d_editor_plugin.cpp:608-612`) |
| Key press you handle (G/R/S/E/X/Tab/1/2/3…) | `STOP`, and call `accept_event()` on a Control if you want to stop propagation to global shortcuts (GridMap does `accept_event(); return STOP;`, `:821-824`) | |
| Mouse captured (`Input::MOUSE_MODE_CAPTURED`) | `PASS` | freelook is active; GridMap checks this first (`:811-813`) |

`p_camera` is the viewport's editor camera (or the previewed scene camera when "Preview" is on:
`previewing ? previewing : camera`). Mouse positions in the events are in **surface-local pixels**, which
are identical to SubViewport pixels (see 1.6), so they can be fed to `Camera3D::project_ray_origin`
directly — every in-tree plugin does.

Key events only arrive when the viewport surface has keyboard focus (`surface->set_focus_mode(FOCUS_ALL)`,
`:6773`).

### 1.4 Drawing over the viewport and `update_overlays`

`Node3DEditorViewport::_draw` (`:4065-4067`) is the `draw` handler of `surface` and begins with:

```cpp
EditorNode::get_singleton()->get_editor_plugins_over()->forward_3d_draw_over_viewport(surface);
EditorNode::get_singleton()->get_editor_plugins_force_over()->forward_3d_force_draw_over_viewport(surface);
```

So `p_overlay` **is** the viewport's `surface` Control (full-rect, `clip_contents=true`), and plugin drawing
happens *before* the editor's own overlay (focus frame, region-select rectangle, message text, vertex-snap
circles, rotation arc) — the editor draws on top of you. Call any `CanvasItem::draw_*` on `p_overlay`.
It is called once per visible viewport (up to 4); to know which viewport you are drawing for, match
`p_overlay` against `Node3DEditor::get_singleton()->get_editor_viewport(i)->get_surface()` and take that
viewport's `get_camera_3d()`.

`int EditorPlugin::update_overlays() const` (`editor_plugin.cpp:307-323`): if the 3D editor is visible it
calls `vp->update_surface()` (= `surface->queue_redraw()`) on each visible `Node3DEditorViewport` and
returns the count; otherwise it redraws the 2D viewport control.

**Overlays are not redrawn automatically when the camera moves.** The navigation path
(`View3DController` → signal `cursor_interpolated` → `Node3DEditorViewport::_cursor_interpolated`,
`.cpp:3039-3060`) only sets the camera transform/projection; it does not `queue_redraw()` the surface
(only `fov_scaled` is wired to `surface->queue_redraw`, `.cpp:7185`). Consequences:

- Anything that must stay glued to 3D geometry while orbiting (vertex dots, edge highlights, face fills)
  should be **real 3D geometry** (RS instances / gizmo meshes, §2.11), not 2D overlay drawing.
- Use the 2D overlay for things that live in screen space anyway (box/lasso/circle-select outlines,
  tool HUD text, numeric readouts, snapping markers during a drag — during a drag you get motion events and
  can call `update_overlays()` each time).
- If a camera-dependent 2D overlay is unavoidable, enable `set_process(true)` on the plugin while in edit
  mode, compare `camera->get_global_transform()` / projection against the last drawn ones and call
  `update_overlays()` on change.

Reference implementation (text overlay), `grid_map_editor_plugin.cpp:1992-2005`:

```cpp
void GridMapEditorPlugin::forward_3d_draw_over_viewport(Control *p_overlay) {
	Point2 msgpos = Point2(20 * EDSCALE, p_overlay->get_size().y - 20 * EDSCALE);
	Ref<Font> font = p_overlay->get_theme_font(SceneStringName(font), SNAME("Label"));
	int font_size = p_overlay->get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	p_overlay->draw_string(font, msgpos + Point2(1, 1), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.8));
	p_overlay->draw_string(font, msgpos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1, 1));
}
```

The editor's own box-select rectangle is the style to copy (`:4077-4089`):

```cpp
surface->draw_rect(selection_rect, get_theme_color(SNAME("box_selection_fill_color"), EditorStringName(Editor)));
surface->draw_rect(selection_rect, get_theme_color(SNAME("box_selection_stroke_color"), EditorStringName(Editor)), false, Math::round(EDSCALE));
```

and its vertex markers (`:4118-4123`): test `camera->is_position_behind(world)` first, then
`surface->draw_circle(camera->unproject_position(world), radius, color, true, -1.0, true)`.

### 1.5 `Node3DEditor` public API (singleton: `Node3DEditor::get_singleton()`)

`class Node3DEditor : public VBoxContainer` (`node_3d_editor_plugin.h:604-1056`).

| Member | Signature / value | Use |
|---|---|---|
| `VIEWPORTS_COUNT` | `static const unsigned int = 4` | |
| `ToolMode` | `TOOL_MODE_TRANSFORM, TOOL_MODE_MOVE, TOOL_MODE_ROTATE, TOOL_MODE_SCALE, TOOL_MODE_SELECT, TOOL_MODE_LIST_SELECT, TOOL_LOCK_SELECTED, TOOL_UNLOCK_SELECTED, TOOL_GROUP_SELECTED, TOOL_UNGROUP_SELECTED, TOOL_RULER, TOOL_MAX` | `get_tool_mode()` |
| `ToolOptions` | `TOOL_OPT_LOCAL_COORDS, TOOL_OPT_USE_SNAP, TOOL_OPT_USE_TRACKBALL, TOOL_OPT_PRESERVE_CHILDREN_TRANSFORM, TOOL_OPT_MAX` | |
| viewports | `Node3DEditorViewport *get_editor_viewport(int p_idx)`; `Node3DEditorViewport *get_last_used_viewport()` | |
| camera viewport size | `static Size2i get_camera_viewport_size(Camera3D *p_camera)` (`.cpp:11199`) | window size, or project viewport size for the scene root, else `sub_viewport->get_size()` |
| snapping | `bool is_snap_enabled() const` (= toolbar toggle XOR snap key); `real_t get_translate_snap() const` (÷10 while Shift held); `real_t get_rotate_snap() const` (÷3 with Shift); `real_t get_scale_snap() const` (÷2 with Shift); `Vector3 snap_point(Vector3 p_target, Vector3 p_start = Vector3(0,0,0)) const` (`p_target.snapf(get_translate_snap())` if enabled) | `.cpp:11217-11258` |
| vertex snap | `bool is_vertex_snap_origin_mode() const`; `bool is_vertex_snap_use_collision() const` | |
| local/global | `bool are_local_coords_enabled() const`; `void set_local_coords_enabled(bool on) const` | |
| transform gizmo | `Transform3D get_gizmo_transform() const`; `bool is_gizmo_visible() const`; `void update_transform_gizmo()`; `void select_gizmo_highlight_axis(int p_axis)`; `real_t gizmo_view_rotation_scale` | |
| gizmo meshes | `Ref<ArrayMesh> get_move_gizmo(int idx)`, `get_axis_gizmo`, `get_move_plane_gizmo`, `get_rotate_gizmo`, `get_scale_gizmo`, `get_scale_plane_gizmo`, `get_trackball_sphere_gizmo()` | reusable arrow meshes |
| gizmos | `void add_gizmo_plugin(Ref<EditorNode3DGizmoPlugin>)`; `void remove_gizmo_plugin(...)`; `void update_all_gizmos(Node *p_node = nullptr)`; `void update_gizmo_opacity()`; `void refresh_dirty_gizmos()` | |
| subgizmos | `bool is_current_selected_gizmo(const EditorNode3DGizmo *p_gizmo)`; `bool is_subgizmo_selected(int p_id)`; `Vector<int> get_subgizmo_selection()`; `void clear_subgizmo_selection(Object *p_obj = nullptr)` | |
| hover | `Ref<EditorNode3DGizmo> get_current_hover_gizmo() const`; `int get_current_hover_gizmo_handle(bool &r_secondary) const`; setters | |
| selection | `Node3D *get_single_selected_node()` (null unless exactly one Node3D is selected) | |
| UI containers | `void add_control_to_menu_panel(Control*)`, `remove_control_from_menu_panel`, `add_control_to_left_panel`, `add_control_to_right_panel`, `remove_control_from_left_panel`, `remove_control_from_right_panel`, `move_control_to_left_panel`, `move_control_to_right_panel`, `VSplitContainer *get_shader_split()` | §1.9 |
| gizmo BVH | `DynamicBVH::ID insert_gizmo_bvh_node(Node3D*, const AABB&)`, `update_gizmo_bvh_node`, `remove_gizmo_bvh_node`, `Vector<Node3D*> gizmo_bvh_ray_query(const Vector3 &p_ray_start, const Vector3 &p_ray_end)`, `gizmo_bvh_frustum_query(const Vector<Plane>&)` | node picking is gizmo-collision based |
| camera settings | `float get_znear() const`, `get_zfar()`, `get_fov()` (the View ▸ Settings… dialog values) | |
| misc | `void set_custom_camera(Node*)`, `Ref<Environment> get_viewport_environment()`, `void edit(Node3D*)`, `void clear()`, `Dictionary get_state() const`, `void set_state(const Dictionary&)`, preview-material accessors, `void snap_selected_nodes_to_floor()` | |

### 1.6 `Node3DEditorViewport` public API, camera, sizes, render layers

`class Node3DEditorViewport : public Control` (`node_3d_editor_plugin.h:113-542`). Public part:

```cpp
static constexpr int32_t GIZMO_BASE_LAYER = 27;  // per-viewport transform gizmo: layer 27 + viewport index
static constexpr int32_t GIZMO_EDIT_LAYER = 26;  // all EditorNode3DGizmo instances
static constexpr int32_t GIZMO_GRID_LAYER = 25;
static constexpr int32_t MISC_TOOL_LAYER  = 24;  // free for tool visuals (GridMap uses it)

void update_surface() { surface->queue_redraw(); }
void update_transform_gizmo_view();
void update_transform_gizmo_highlight();
void set_state(const Dictionary &p_state);  Dictionary get_state() const;  void reset();
Vector3 get_ray_pos(const Vector2 &p_pos) const;   // = camera->project_ray_origin(p_pos)
Vector3 get_ray(const Vector2 &p_pos) const;       // = camera->project_ray_normal(p_pos)
Point2  point_to_screen(const Vector3 &p_point);   // = camera->unproject_position(p_point)
void focus_selection();
SubViewport *get_viewport_node() { return viewport; }
Camera3D    *get_camera_3d()     { return camera; }
Control     *get_surface()       { return surface; }
```

- The editor camera's cull mask is
  `((1 << 20) - 1) | (1 << (GIZMO_BASE_LAYER + p_index)) | (1 << GIZMO_EDIT_LAYER) | (1 << GIZMO_GRID_LAYER) | (1 << MISC_TOOL_LAYER)`
  (`.cpp:6770`). A game camera's default `layers = 0xfffff` (`scene/3d/camera_3d.h`) never sees layers
  24-27. **Put every edit-mode overlay instance on `1 << GIZMO_EDIT_LAYER` (hidden when the user turns
  "View Gizmos" off, `.cpp:4593-4597`) or `1 << MISC_TOOL_LAYER` (always visible in editor viewports).**
- Viewport size = `surface->get_size()` = `viewport->get_size()`. "Half Resolution" is implemented with
  `viewport->set_scaling_3d_scale(...)` (`_update_shrink`, `.cpp:640-644`), **not** with a SubViewportContainer
  shrink, so mouse coordinates are never scaled. (Older Godot divided by a shrink factor; `_select_ray`
  still has the vestigial name `shrinked_pos = p_pos`.)
- Getting "the active camera" from outside an input callback:
  `Node3DEditor::get_singleton()->get_last_used_viewport()->get_camera_3d()`; inside callbacks use the
  `p_camera` argument. To map a camera back to its viewport (Path3D idiom, `path_3d_editor_plugin.cpp:617-624`):
  ```cpp
  Node3DEditorViewport *viewport = nullptr;
  for (uint32_t i = 0; i < Node3DEditor::VIEWPORTS_COUNT; i++) {
  	Node3DEditorViewport *vp = Node3DEditor::get_singleton()->get_editor_viewport(i);
  	if (vp->get_camera_3d() == p_camera) { viewport = vp; break; }
  }
  ```
- Viewport state keys accepted by `set_state` include `"gizmos"`, `"transform_gizmo"`, `"grid"`,
  `"information"`, `"listener"`, `"doppler"` (`.cpp:5222-5262`); each toggles the corresponding View-menu check item.
- Private but relevant to understand behaviour: `_build_screen_frustum(p_min, p_max)` builds the
  4 side planes + near + far used for region select (`.cpp:1230-1257`): for perspective each side plane is
  `Plane(a, b, cam_pos)` through two unprojected rectangle corners and the eye; for orthogonal
  `Plane((a - b).normalized(), a)`. A point is **inside** when `plane.distance_to(p) <= 0` for all planes
  (Path3D test, `path_3d_editor_plugin.cpp:1223-1229`).

### 1.7 Snap settings

- Toolbar toggle "Use Snap" (shortcut `spatial_editor/snap`, default **Y**) plus a temporary snap key; read
  with `Node3DEditor::get_singleton()->is_snap_enabled()`.
- Values come from the Transform ▸ Configure Snap… dialog: `get_translate_snap()`, `get_rotate_snap()`
  (degrees), `get_scale_snap()` (percent). Holding Shift refines them (÷10, ÷3, ÷2).
- `snap_point()` snaps absolute coordinates with `Vector3::snapf`. For relative (delta) snapping do
  `delta.snapf(get_translate_snap())` yourself.

### 1.8 The built-in transform gizmo: visibility rules and how to disable or coexist

Visibility is computed per viewport in `update_transform_gizmo_view()` (`.cpp:5027-5140`):

```cpp
bool show_gizmo = spatial_editor->is_gizmo_visible() && !_edit.instant && transform_gizmo_visible
		&& !collision_reposition && !hide_gizmo_during_rotation && !hide_gizmo_during_trackball;
// move arrows/planes: tool mode TRANSFORM or MOVE; rotate rings: TRANSFORM or ROTATE; scale: SCALE only
```

with `Node3DEditor::is_gizmo_visible()` = `gizmo.visible && selected->is_transform_gizmo_visible()`
(`.cpp:11225-11230`) and `gizmo.visible = count > 0` from `update_transform_gizmo()` (`.cpp:7401-7458`).

The gizmo's **position** is: if the single selected node has a "current gizmo" with selected subgizmos
(`Node3DEditorSelectedItem::gizmo` valid), the average of
`node->get_global_transform() * gizmo->get_subgizmo_transform(id)` over selected ids, with the basis of
the last one when local coords are on; otherwise the average origin of the selected nodes.

Three supported ways to control it:

| Technique | Call | Used by | Effect |
|---|---|---|---|
| Per-node switch | `node->set_transform_gizmo_visible(false)` (`scene/3d/node_3d.h:289`, virtual; `Node3D::data.transform_gizmo_visible` bitfield) then `Node3DEditor::get_singleton()->update_transform_gizmo()` | `Skeleton3DEditor::_update_gizmo_visible` (`skeleton_3d_editor_plugin.cpp:1589-1609`): hidden in bone edit mode when no bone is selected, shown when one is | Hides and disables picking of the transform gizmo for that node only. Restore to `true` on exit (Skeleton does at `:1275`). Not serialised. |
| Per-viewport View-menu toggle | `viewport->set_state({ "transform_gizmo": false })` for all 4 viewports | `GridMapEditor::_show_viewports_transform_gizmo` (`grid_map_editor_plugin.cpp:799-806`), restored in `make_visible(false)` (`:2028`) | Unchecks "View Transform Gizmo" for the user — a visible global side-effect. |
| Re-target it with subgizmos | implement the subgizmo API (1.12) | Path3D, Skeleton3D | The gizmo moves to the selected elements and edits them; Move/Rotate/Scale tool modes, axis locks (X/Y/Z, Shift+X/Y/Z), numeric input, snapping and local/global all work for free. |

Blocking it by input: returning `AFTER_GUI_INPUT_STOP` for an LMB press prevents `_transform_gizmo_select`
from running at all. Returning `CUSTOM` does **not** block the gizmo.

Also note the "instant transform" shortcuts `spatial_editor/instant_translate|instant_rotate|instant_scale`
exist but have **no default key** (`.cpp:6918-6920`); if the user binds G/R/S to them, they operate on
subgizmos too (they go through `begin_transform(mode, true)` → `apply_transform`).

### 1.9 Adding toolbar buttons, menus, side panels and docks to the 3D editor

`EditorPlugin::CustomControlContainer` (`editor_plugin.h:76-89`) and what each maps to (`editor_plugin.cpp:140-190`):

| Enum | Destination |
|---|---|
| `CONTAINER_TOOLBAR` | `EditorNode::get_title_bar()->add_child` (main title bar) |
| `CONTAINER_SPATIAL_EDITOR_MENU` | `Node3DEditor::add_control_to_menu_panel` — the *context toolbar* strip of the 3D editor |
| `CONTAINER_SPATIAL_EDITOR_SIDE_LEFT` | `Node3DEditor::add_control_to_left_panel` (HSplit left of viewports) |
| `CONTAINER_SPATIAL_EDITOR_SIDE_RIGHT` | `Node3DEditor::add_control_to_right_panel` |
| `CONTAINER_SPATIAL_EDITOR_BOTTOM` | `Node3DEditor::get_shader_split()->add_child` (VSplit under viewports) |
| `CONTAINER_CANVAS_EDITOR_MENU / _SIDE_LEFT / _SIDE_RIGHT / _BOTTOM` | 2D editor equivalents |
| `CONTAINER_INSPECTOR_BOTTOM` | `InspectorDock::get_singleton()->get_addon_area()` |
| `CONTAINER_PROJECT_SETTING_TAB_LEFT / _RIGHT` | Project Settings tabs |

`Node3DEditor::add_control_to_menu_panel` (`.cpp:9799-9811`) requires the control to have **no parent**,
inserts a `VSeparator` before it, and connects `visibility_changed` to `_update_context_toolbar`, which
hides the whole context panel when no child is visible. Therefore: add your `HBoxContainer` once in the
plugin constructor, `hide()` it, and `show()`/`hide()` it from `make_visible()` — exactly what Path3D
(`topmenu_bar`, `path_3d_editor_plugin.cpp:1025-1026,1118`, `make_visible` at `:808-816`), Polygon3D
(`:613-617`) and MeshInstance3D (`options` MenuButton, `mesh_instance_3d_editor_plugin.cpp:803-814`) do.
In-tree C++ calls `Node3DEditor::get_singleton()->add_control_to_menu_panel(ctrl)` directly rather than
going through `add_control_to_container`.

Toolbar button conventions (Path3D constructor, `:1031-1038`):

```cpp
curve_edit = memnew(Button);
curve_edit->set_theme_type_variation(SceneStringName(FlatButton));
curve_edit->set_toggle_mode(true);
curve_edit->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
curve_edit->set_tooltip_text(TTR("Select Points") + "\n" + ...);
curve_edit->set_accessibility_name(TTRC("Select Points"));
toolbar->add_child(curve_edit);
curve_edit->connect(SceneStringName(pressed), callable_mp(this, &Path3DEditorPlugin::_mode_changed).bind(MODE_EDIT));
```

Icons are assigned on theme change: `button->set_button_icon(get_editor_theme_icon(SNAME("Edit")))`
(Control method) or `EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Name"), EditorStringName(EditorIcons))`.
A menu: `MenuButton *m = memnew(MenuButton); m->set_flat(false); m->set_theme_type_variation("FlatMenuButton");
m->get_popup()->add_item(TTR("…"), ID); m->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &X::_menu_option));`.

Docks: GridMap's editor is an `EditorDock` subclass registered with
`EditorDockManager::get_singleton()->add_dock(grid_map_editor)` and opened/closed with
`make_visible()` / `close()` (`grid_map_editor_plugin.cpp:1965-1972, 2016-2031`). `EditorDock` public API
(`editor/docks/editor_dock.h`): `open()`, `make_visible()`, `close()`, `set_title`, `set_layout_key`,
`set_global`, `set_transient`, `set_closable`, `set_icon_name`, `set_dock_icon`, `set_dock_shortcut`,
`set_default_slot(DockSlot)`, `set_available_layouts(BitField<DockLayout>)`; slots
`DOCK_SLOT_LEFT_UL … DOCK_SLOT_RIGHT_BR, DOCK_SLOT_BOTTOM, DOCK_SLOT_BOTTOM_L, DOCK_SLOT_BOTTOM_R`;
layouts `DOCK_LAYOUT_VERTICAL=1, HORIZONTAL=2, FLOATING=4`.

Main-screen workspaces ("Image", "Model", "Video" tabs): override `has_main_screen()` to return true,
`get_plugin_name()`, `get_plugin_icon()`, and in the constructor add your root control like the 3D editor
does (`node_3d_editor_plugin.cpp:11336-11342`):

```cpp
root = memnew(MyWorkspace);
root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(root);
root->hide();
// make_visible(bool) shows/hides root.
```

`EditorMainScreen` (`editor/editor_main_screen.h`): `select_by_name(const String&)`, `select(int)`,
`get_selected_plugin()`, `get_plugin_by_name()`, `VBoxContainer *get_control() const`; built-in indices
`EDITOR_2D=0, EDITOR_3D, EDITOR_SCRIPT, EDITOR_GAME, EDITOR_ASSETLIB`.

### 1.10 Built-in 3D editor shortcuts that an Edit Mode will collide with

Declared with `ED_SHORTCUT(path, name, key, physical)`; tested with `ED_IS_SHORTCUT(path, event)`
(`editor/settings/editor_settings.h:234-239`). Tool-button shortcuts are *button shortcuts* handled by the
GUI shortcut system (they fire regardless of plugins unless the event is accepted first); viewport
shortcuts are tested inside `_sinput` **after** plugins, so returning `STOP` pre-empts them.

| Path | Default | Handled | Blender meaning it collides with |
|---|---|---|---|
| `spatial_editor/tool_transform` | Q (physical) | toolbar button | — |
| `spatial_editor/tool_move` | W (physical) | toolbar button | — |
| `spatial_editor/tool_rotate` | E (physical) | toolbar button | **E = extrude** |
| `spatial_editor/tool_scale` | R (physical) | toolbar button | **R = rotate** |
| `spatial_editor/tool_select` | V (physical) | toolbar button | V = rip |
| `spatial_editor/measure` | M | toolbar button | M = merge |
| `spatial_editor/local_coords` | T | toolbar button | — |
| `spatial_editor/snap` | Y | toolbar button | Y axis lock is separate (below) |
| `spatial_editor/trackball` | U | toolbar button | U = UV unwrap menu |
| `spatial_editor/preserve_children_transform` | P | toolbar button | P = separate |
| `spatial_editor/vertex_snap` | B | `_sinput` **before plugins** | **B = box select** |
| `spatial_editor/focus_selection` | F | `_sinput` after plugins | **F = fill/make face** |
| `spatial_editor/focus_origin` | O | after plugins | O = proportional editing |
| `spatial_editor/insert_anim_key` | K | after plugins | **K = knife** |
| `spatial_editor/freelook_toggle` | Shift+F | after plugins | — |
| `spatial_editor/lock_transform_x/y/z` | X / Y / Z | after plugins, only while transforming | same meaning |
| `spatial_editor/lock_transform_yz/xz/xy` | Shift+X / Shift+Y / Shift+Z | same | same meaning |
| `spatial_editor/cancel_transform` | Escape | after plugins | same |
| `spatial_editor/instant_translate/rotate/scale` | *(unbound)* | after plugins | G / R / S |
| `spatial_editor/collision_reposition` | Shift+G | after plugins | Shift+G = select similar |
| `spatial_editor/reset_transform_position/rotation/scale` | Alt+W / Alt+E / Alt+R | after plugins | — |
| `spatial_editor/top_view` etc. | KP7, KP1, KP3, Alt+KP7/1/3, KP2/4/6/8, KP9, KP5 | after plugins | same as Blender |
| `spatial_editor/snap_to_floor` | PageDown | menu shortcut | — |
| `spatial_editor/view_grid` | `#` | menu shortcut | — |
| `spatial_editor/1_viewport … 4_viewports` | Ctrl+1 … Ctrl+4 (physical) | menu shortcut | — |
| `spatial_editor/freelook_left/right/forward/backwards/up/down` | A / D / W / S / E / Q (physical) | only in freelook | — |
| Escape with no transform | — | `_clear_selected()` (`.cpp:2824-2827`) | deselects the node! |

Consequences for the design:

- Toolbar-button shortcuts (Q/W/E/R/V/M/T/Y/U/P) are `Button::set_shortcut` shortcuts
  (`.cpp:10503-10637`). GridMap coexists by registering its own buttons with *physical* Q/W/E/R/T/Z/X/C/V/A/S/D
  shortcuts and matching them manually in `forward_spatial_input_event` with
  `b->get_shortcut()->matches_event(p_event)` then `accept_event(); return STOP;`
  (`grid_map_editor_plugin.cpp:829-844`). Because plugin forwarding happens inside the surface's `gui_input`,
  which precedes unhandled-shortcut processing, `accept_event()` on the event wins over the 3D toolbar
  buttons. The Tetra edit mode must do the same for E/R/etc.
- Escape must be consumed (`STOP`) while in edit mode, or the node gets deselected and the plugin is torn down.
- `B` (vertex snap) is checked before plugins; it cannot be intercepted from `forward_3d_gui_input`.
  Options: keep Tetra's box-select on another key; or patch the default in the Tetra branding patch set
  (`patches/godot/`); or rebind at editor init. `ED_SHORTCUT_OVERRIDE(path, feature, key, physical)`
  (`editor_settings.cpp:2173-2200`) only applies when `OS::has_feature(p_feature)` is true (it exists for
  per-OS defaults such as `"macos"`), so a module could call
  `ED_SHORTCUT_OVERRIDE("spatial_editor/vertex_snap", "linuxbsd", Key::…)` per platform after the 3D editor
  has registered the shortcut — this was **not** tested here and user-customised shortcuts still win.
- Plugin input is delivered from the surface's `gui_input`, which runs before the shortcut phase:
  `Viewport::push_input` order is `_input` group → `_gui_input_event` → `_push_shortcut_input_internal` →
  unhandled key → unhandled input (`scene/main/viewport.cpp:3535-3549, 3620-3633`), each stage skipped once
  the event is marked handled. `Control::accept_event()` = `get_viewport()->_gui_accept_event()`
  (`scene/gui/control.cpp:2543-2548`). An `EditorPlugin` is a `Node`, not a `Control`: call
  `accept_event()` on one of your Controls that lives in the same window as the 3D viewport (GridMap calls
  it on its dock), or `Node3DEditor::get_singleton()->get_viewport()->set_input_as_handled()`.

### 1.11 `EditorNode3DGizmoPlugin` and `EditorNode3DGizmo` reference

Header: `editor/scene/3d/node_3d_editor_gizmos.h`.

`class EditorNode3DGizmo : public Node3DGizmo` — one instance per (node, plugin). Building API:

```cpp
void add_lines(const Vector<Vector3> &p_lines, const Ref<Material> &p_material, bool p_billboard = false, const Color &p_modulate = Color(1, 1, 1));
void add_vertices(const Vector<Vector3> &p_vertices, const Ref<Material> &p_material, Mesh::PrimitiveType p_primitive_type, bool p_billboard = false, const Color &p_modulate = Color(1, 1, 1));
void add_mesh(const Ref<Mesh> &p_mesh, const Ref<Material> &p_material = Ref<Material>(), const Transform3D &p_xform = Transform3D(), const Ref<SkinReference> &p_skin_reference = Ref<SkinReference>());
void add_collision_segments(const Vector<Vector3> &p_lines);
void add_collision_triangles(const Ref<TriangleMesh> &p_tmesh);
void set_collision_meshes_are_snap_source(bool p_enable);
void add_unscaled_billboard(const Ref<Material> &p_material, real_t p_scale = 1, const Color &p_modulate = Color(1, 1, 1));
void add_handles(const Vector<Vector3> &p_handles, const Ref<Material> &p_material, const Vector<int> &p_ids = Vector<int>(), bool p_billboard = false, bool p_secondary = false);
void add_solid_box(const Ref<Material> &p_material, Vector3 p_size, Vector3 p_position = Vector3(), const Transform3D &p_xform = Transform3D());
Node3D *get_node_3d() const;  Ref<EditorNode3DGizmoPlugin> get_plugin() const;
bool is_selected() const;  bool is_editable() const;
bool is_subgizmo_selected(int p_id) const;  Vector<int> get_subgizmo_selection() const;
virtual void clear();  virtual void redraw();   // + create()/transform()/free() driven by Node3D
void set_hidden(bool p_hidden);
```

Virtuals a gizmo may override (default forwards to the plugin, `.cpp:95-209`):
`is_handle_highlighted`, `get_handle_name`, `get_handle_value`, `begin_handle_action`,
`set_handle(int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point)`,
`commit_handle(int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel = false)`,
`subgizmos_intersect_ray`, `subgizmos_intersect_frustum`, `get_subgizmo_transform`, `set_subgizmo_transform`,
`commit_subgizmos`.

What the `add_*` calls actually do (`.cpp:216-480`):

- Each call creates **one `ArrayMesh` and one RS instance** (`RS::instance_create2(mesh->get_rid(), scenario)`),
  attached with `instance_attach_object_instance_id`, shadows off, layer mask `1 << GIZMO_EDIT_LAYER`,
  `INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING`, transform = node global transform (× `p_xform` for `add_mesh`).
  `clear()` frees them all. So a gizmo `redraw()` **reallocates every mesh** — fine for dozens of handles,
  far too slow for per-frame updates of a 100k-vertex edit mesh. Use dedicated instances for the edit cage (§2.6).
- `add_vertices` fills `ARRAY_COLOR` with white × `p_modulate` at alpha 0.8 when the gizmo is selected,
  0.2 otherwise; it is the caller's material that decides if vertex colour is used.
- `add_handles` early-outs unless `is_selected() && is_editable()`; draws `PRIMITIVE_POINTS` with per-vertex
  colours: white, or `Color(0, 0, 1, 0.9)` when `is_handle_highlighted(id, secondary)`; alpha 0.8 unless hovered.
  Handles are positions in the node's **local space**. If ids are omitted the index is the id; mixing
  id-less and id-carrying calls is an error.
- `handles_intersect_ray` (`.cpp:587-647`): a handle is hit when its unprojected position is within
  `HANDLE_HALF_SIZE = 9.5` px of the mouse; nearest to camera wins; secondary handles are tested first
  and win when Shift is held.
- `intersect_ray` (node picking, `.cpp:649-773`): icon billboard rect, then `collision_segments` (< 8 px in
  screen space), then `collision_meshes` via `TriangleMesh::intersect_ray` in local space. **A node is only
  clickable in the editor if one of its gizmos registers collision geometry** — `MeshInstance3DGizmoPlugin::redraw`
  does `p_gizmo->add_collision_triangles(m->generate_triangle_mesh())` (`mesh_instance_3d_gizmo_plugin.cpp:178-183`).
  After editing a mesh, the node's `update_gizmos()` must run so picking matches the new shape
  (`MeshInstance3D` does this when its mesh emits `changed`).

`class EditorNode3DGizmoPlugin : public Resource`:

```cpp
static const int VISIBLE = 0, HIDDEN = 1, ON_TOP = 2;           // View ▸ Gizmos tri-state per plugin
void create_material(const String &p_name, const Color &p_color, bool p_billboard = false, bool p_on_top = false, bool p_use_vertex_color = false);
void create_icon_material(const String &p_name, const Ref<Texture2D> &p_texture, bool p_on_top = false, const Color &p_albedo = Color(1, 1, 1, 1));
void create_handle_material(const String &p_name, bool p_billboard = false, const Ref<Texture2D> &p_texture = nullptr);
void add_material(const String &p_name, Ref<StandardMaterial3D> p_material);
Ref<StandardMaterial3D> get_material(const String &p_name, const Ref<EditorNode3DGizmo> &p_gizmo = Ref<EditorNode3DGizmo>());
virtual bool has_gizmo(Node3D *p_spatial);                       // protected
virtual Ref<EditorNode3DGizmo> create_gizmo(Node3D *p_spatial);  // protected; default instantiates EditorNode3DGizmo if has_gizmo()
virtual String get_gizmo_name() const;  virtual int get_priority() const;
virtual bool can_be_hidden() const;  virtual bool is_selectable_when_hidden() const;  virtual bool can_commit_handle_on_click() const;
virtual void redraw(EditorNode3DGizmo *p_gizmo);
virtual bool is_handle_highlighted(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const;
virtual String get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const;
virtual Variant get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const;
virtual void begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary);
virtual void set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point);
virtual void commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel = false);
virtual int subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const;
virtual Vector<int> subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const;
virtual Transform3D get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const;
virtual void set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform);
virtual void commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel = false);
Ref<EditorNode3DGizmo> get_gizmo(Node3D *p_spatial);
void set_state(int p_state);  int get_state() const;
```

- Registration: `Node3DEditor::get_singleton()->add_gizmo_plugin(ref)` (or `EditorPlugin::add_node_3d_gizmo_plugin`).
  Plugins are sorted by `get_priority()` descending then name (`.cpp:11260-11285`). Several plugins can each
  attach a gizmo to the same node (`_request_gizmo` loops over all, `.cpp:9907-9932`), so a Tetra gizmo
  plugin **coexists** with the stock `MeshInstance3DGizmoPlugin` (priority −1); it does not replace it.
- `create_material(name, color, billboard, on_top, use_vertex_color)` creates **4 variants** indexed
  `(selected ? 1 : 0) + (editable ? 2 : 0)`; unselected variants get alpha × 0.3; non-editable ("instantiated")
  variants use the editor colour `editors/3d_gizmos/gizmo_colors/instantiated`. All are unshaded,
  `TRANSPARENCY_ALPHA`, `render_priority = RENDER_PRIORITY_MIN + 1`, `CULL_DISABLED`, `FLAG_DISABLE_FOG`
  (`.cpp:902-943`). `get_material(name, gizmo)` picks the variant, and when the plugin state is `ON_TOP` and
  the gizmo is selected returns a *duplicate* with `FLAG_DISABLE_DEPTH_TEST` (`.cpp:1015-1035`).
- `create_handle_material(name, billboard, texture)` (`.cpp:987-1008`): unshaded, `FLAG_USE_POINT_SIZE`,
  `point_size = icon width`, albedo texture = editor icon `Editor3DHandle` by default,
  `FLAG_ALBEDO_FROM_VERTEX_COLOR`, `FLAG_SRGB_VERTEX_COLOR`, `FLAG_DISABLE_FOG`, `set_on_top_of_alpha()`,
  then `TRANSPARENCY_ALPHA`. This is the canonical "screen-space dot" material.
- `Node3D` side (`scene/3d/node_3d.h:289-300`): `update_gizmos()` (deferred redraw of all gizmos),
  `set_subgizmo_selection(Ref<Node3DGizmo> p_gizmo, int p_id, Transform3D p_transform = Transform3D())`,
  `clear_subgizmo_selection()`, `Vector<Ref<Node3DGizmo>> get_gizmos() const`, `add_gizmo`, `set_disable_gizmos`,
  `set_transform_gizmo_visible(bool)`.

### 1.12 The subgizmo protocol (how Path3D lets the standard gizmo move points)

A *subgizmo* is an integer id naming an element of a node (curve point, bone, — for Tetra: a vertex or a
selection island). The editor owns the selection set, stored in
`Node3DEditorSelectedItem::subgizmos` (`HashMap<int, Transform3D>`: id → transform at selection/transform
start) and `Node3DEditorSelectedItem::gizmo` (which gizmo owns them) (`node_3d_editor_plugin.h:544-566`).

| Step | Who calls | What | Source |
|---|---|---|---|
| Click-pick | viewport, LMB press, *after* handle test and transform-gizmo test | `int id = seg->subgizmos_intersect_ray(camera, mouse_pos)`; `-1` = none. Shift toggles membership, otherwise the set is replaced by `{id}`; `se->gizmo = seg`; `seg->redraw()`; `update_transform_gizmo()` | `.cpp:2412-2455` |
| Box-select | viewport, LMB release after drag, **before** node region selection | `Vector<int> ids = seg->subgizmos_intersect_frustum(camera, frustum)`; non-empty ⇒ they are added (Shift) or replace the set, and node selection is skipped | `.cpp:1273-1326` |
| Read transform | viewport/Node3DEditor | `get_subgizmo_transform(id)` → transform **in the node's local space**; cached as the "original" | `.cpp:1303, 1440, 2431, 7412` |
| Live transform | `Node3DEditorViewport::apply_transform` | For each selected id: `xform = original (local)`; `new = _compute_transform(mode, se->original * xform, xform, motion, snap, local_coords, …)`; if not local coords `new = se->original.affine_inverse() * new`; then `gizmo->set_subgizmo_transform(id, new)` — i.e. you always receive a **node-local** transform | `.cpp:6268-6276` |
| Commit | viewport on LMB release | `gizmo->commit_subgizmos(ids, restore, false)` where `restore[i]` are the cached originals — create the undo action here | `.cpp:2597-2609` |
| Cancel | viewport (`cancel_transform`, RMB/Escape) | `gizmo->commit_subgizmos(ids, restore, true)` — write the originals back, no undo action | `.cpp:625` |
| Deselect | Escape / empty click | `_clear_selected()`: `se->subgizmos.clear(); se->gizmo->redraw(); se->gizmo.unref(); update_transform_gizmo()` — and only if there was *no* subgizmo selection does it clear the node selection | `.cpp:682-704` |
| Query | gizmo plugin | `p_gizmo->get_subgizmo_selection()` / `is_subgizmo_selected(id)` to colour selected elements in `redraw` | Path3D `redraw`, `:1157` |
| Programmatic set | plugin | `node->set_subgizmo_selection(gizmo, id, transform)` (replaces the set with **one** id) / `node->clear_subgizmo_selection()`; `Node3DEditor::clear_subgizmo_selection()` | `.cpp:9941-9987`; Skeleton `:1509` |

Limits that matter for a mesh editor:

- Ids are `int`; the transform gizmo sits at the **average** of the selected subgizmo origins; with local
  coords the basis is the last selected one's.
- The viewport applies the *same world-space motion* to each subgizmo independently around the shared gizmo
  centre (`_compute_transform` with `_edit.center`), which gives correct translate / rotate-about-centre /
  scale-about-centre behaviour for points — the same as Blender's "Median Point" pivot.
- There is no public API to set a multi-id selection programmatically
  (`_set_subgizmo_selection` clears then inserts one id; the map itself is reachable only through
  `EditorNode::get_singleton()->get_editor_selection()->get_node_editor_data<Node3DEditorSelectedItem>(node)`,
  which *is* callable from module C++ because the struct is public — in-tree code outside the 3D editor does
  not do this, so treat it as fragile).
- For large selections the per-id virtual calls each mouse-move are O(selected) — acceptable (it is just
  `set_subgizmo_transform` per id), but the plugin must batch its GPU update, not rebuild the mesh per call.
  A robust alternative for Tetra: expose **one** subgizmo id (e.g. id 0 = "current selection pivot") whose
  transform is the selection pivot frame; in `set_subgizmo_transform` compute the delta from the original
  and apply it to all selected elements in the native core. This keeps the built-in gizmo, snapping, axis
  locks and numeric entry while the element selection stays in Tetra's own data structure.

Path3D's implementation as the template (`path_3d_editor_plugin.cpp:1188-1300`):

```cpp
int Path3DGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	Path3D *path = Object::cast_to<Path3D>(p_gizmo->get_node_3d());
	...
	for (int idx = 0; idx < curve->get_point_count(); ++idx) {
		Vector3 pos = path->get_global_transform().xform(curve->get_point_position(idx));
		if (p_camera->unproject_position(pos).distance_to(p_point) < 20) { ret = idx; break; }
	}
	return ret;
}
Vector<int> Path3DGizmoPlugin::subgizmos_intersect_frustum(..., const Vector<Plane> &p_frustum) const {
	// inside when p_frustum[i].distance_to(world_pos) <= 0 for every plane
}
Transform3D Path3DGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	Basis basis = transformation_locked_basis.has(p_id) ? transformation_locked_basis[p_id] : curve->get_point_baked_posture(p_id, true);
	return Transform3D(basis, curve->get_point_position(p_id));           // local space
}
void Path3DGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	if (!transformation_locked_basis.has(p_id)) { transformation_locked_basis[p_id] = Basis(curve->get_point_baked_posture(p_id, true)); }
	curve->set_point_position(p_id, p_transform.origin);
}
void Path3DGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	transformation_locked_basis.clear();
	if (p_cancel) { for (...) curve->set_point_position(p_ids[i], p_restore[i].origin); return; }
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Curve Point Position"));
	for (...) {
		undo_redo->add_do_method(curve.ptr(), "set_point_position", idx, curve->get_point_position(idx));
		undo_redo->add_undo_method(curve.ptr(), "set_point_position", idx, p_restore[i].origin);
	}
	undo_redo->commit_action();
}
int Path3DGizmoPlugin::get_priority() const { return -1; }
```

Notes: Path3D "locks" the basis during a drag so that local-space dragging is stable (comment at
`path_3d_editor_plugin.h:85-87`); `Path3DGizmo` connects the node's `curve_changed` signal to
`Node3DEditor::update_transform_gizmo` so the gizmo follows edits (`:571-583`); the custom gizmo's `redraw()`
calls `gizmo_plugin->redraw(this)` manually at the end because the base `EditorNode3DGizmo::redraw` is
bypassed by its override (`:566-567`).

### 1.13 Worked examples

**Path3D** (`editor/scene/3d/path_3d_editor_plugin.*`) — three cooperating classes:
`Path3DEditorPlugin` (toolbar with mode toggle buttons added via `add_control_to_menu_panel`; `handles` =
`p_object->is_class("Path3D")`; `edit` stores the node and calls `update_overlays()`; `make_visible` shows the
toolbar and toggles `set_physics_process`), `Path3DGizmoPlugin` (materials + subgizmos for curve points)
and `Path3DGizmo` (custom `EditorNode3DGizmo` with *handles* for in/out/tilt control points:
`get_handle_name/value`, `set_handle`, `commit_handle`). `forward_3d_gui_input` (`:592-781`) handles only
create/split/delete clicks: it projects curve samples with `viewport->point_to_screen`, uses
`Geometry2D::get_closest_point_to_segment(mbpos, a, b)` for a 10 px hit test, then
`Geometry3D::get_closest_points_between_segments(ray_from, ray_from + ray_dir * 4096, from, to, ra, rb)` to
get the 3D split point, or `Plane(p_camera->get_transform().basis.get_column(2), origin).intersects_ray(...)`
to place a new point on the view plane through the last point. Every edit is an `EditorUndoRedoManager`
action using string method names on the resource (`"add_point"`, `"remove_point"`), and returns `STOP`.

**GridMap** (`modules/gridmap/editor/grid_map_editor_plugin.*`) — the in-module model:
- Plugin (`GridMapEditorPlugin`) is thin; the dock (`GridMapEditor : EditorDock`) owns state.
- Overlay geometry is raw RenderingServer: `grid[i] = RS::mesh_create(); grid_instance[i] = RS::instance_create2(grid[i], scenario);
  RS::instance_set_layer_mask(grid_instance[i], 1 << Node3DEditorViewport::MISC_TOOL_LAYER);`
  with `scenario = get_tree()->get_root()->get_world_3d()->get_scenario()` (`:1336-1352`); rebuilt with
  `RS::mesh_clear` + `RS::mesh_add_surface_from_arrays(grid[i], RSE::PRIMITIVE_LINES, d)` +
  `RS::mesh_surface_set_material(grid[i], 0, indicator_mat->get_rid())` (`:1263-1311`); moved with
  `RS::instance_set_transform`; freed with `RS::free_rid` in the destructor.
- Picking (`do_input_action`, `:375-430`): ray from `camera->project_ray_origin/normal`, transformed into node
  space with `node->get_global_transform().affine_inverse()`, intersected with the edit plane via
  `Plane::intersects_segment`, then rejected if outside `camera->get_frustum()` planes
  (`local_xform.xform(planes[i]).is_point_over(inters)`).
- Input: mode by toggle buttons in a `ButtonGroup`; keyboard shortcuts matched manually (see 1.10);
  paint strokes accumulate `SetItem`s and are committed as **one undo action on mouse release** (`:946-961`).
- Hides the transform gizmo with `set_state({"transform_gizmo": false})` on all viewports.
- Emits its own `overlay_update_requested` signal connected to `EditorPlugin::update_overlays`.

**Polygon3D** (`polygon_3d_editor_plugin.cpp`) — overlay via helper nodes: a `MeshInstance3D *imgeom` with an
`ImmediateMesh` (lines) plus a child `MeshInstance3D *pointsm` with an `ArrayMesh` of `PRIMITIVE_POINTS`
(handles) are **added as children of the edited node** in `edit()` (`p_node->add_child(imgeom)`, `:510-514`) and
removed in `edit(nullptr)`; no owner is set, so they are not saved with the scene. Materials: unshaded,
`TRANSPARENCY_ALPHA`, `FLAG_ALBEDO_FROM_VERTEX_COLOR`, `FLAG_SRGB_VERTEX_COLOR`, `FLAG_DISABLE_FOG`,
`FLAG_DISABLE_DEPTH_TEST`; the handle material adds `FLAG_USE_POINT_SIZE`, `set_point_size(handle->get_width())`
and the `Editor3DHandle` icon as albedo (`:562-581`). `_polygon_draw()` rebuilds both meshes on every change
(`imesh->clear_surfaces(); imesh->surface_begin(Mesh::PRIMITIVE_LINES); … surface_end();`). `handles()` is
duck-typed: `Object::cast_to<Node3D>(p_object) && bool(p_object->call("_is_editable_3d_polygon"))`.
Caveat: child helper nodes appear in `get_children()` of the edited node and inherit its visibility/transform.

**Skeleton3D** (`skeleton_3d_editor_plugin.cpp`) — bone edit mode:
`forward_3d_gui_input` returns `AFTER_GUI_INPUT_CUSTOM` while `is_edit_mode()` (`:1569-1579`); bones are
subgizmos (`subgizmos_intersect_ray` picks the joint within `8 * EDSCALE` px closest to the camera and also
drives the inspector selection, `:1667-1707`); `set_transform_gizmo_visible(false)` when no bone is selected;
joints are drawn with an `ImmediateMesh` of `PRIMITIVE_POINTS` on a child `MeshInstance3D` using a **custom
spatial shader** that forces points on top by rewriting clip-space depth (`:1351-1383`):

```glsl
shader_type spatial;
render_mode unshaded, shadows_disabled, depth_draw_always, fog_disabled;
uniform sampler2D texture_albedo : source_color;
uniform float point_size : hint_range(0, 128) = 32;
void vertex() {
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), COLOR.rgb * (1.0 / 12.92), lessThan(COLOR.rgb, vec3(0.04045)));
	}
	VERTEX = VERTEX;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * MODEL_MATRIX * vec4(VERTEX.xyz, 1.0);
	POSITION.z = mix(POSITION.z, POSITION.w, 0.999);
	POINT_SIZE = point_size;
}
void fragment() {
	vec4 albedo_tex = texture(texture_albedo, POINT_COORD);
	vec3 col = albedo_tex.rgb + COLOR.rgb;
	col = vec3(min(col.r, 1.0), min(col.g, 1.0), min(col.b, 1.0));
	ALBEDO = col;
	if (albedo_tex.a < 0.5) { discard; }
	ALPHA = albedo_tex.a;
}
```

(`POSITION.z = mix(POSITION.z, POSITION.w, k)` with reversed-Z pulls the fragment towards the near plane:
"draw on top but still depth-sorted among themselves". The selected-bone line shader uses 0.998.)

**MeshInstance3D editor** (`mesh_instance_3d_editor_plugin.cpp`) — a `MenuButton` "Mesh" in the context
toolbar with: Create Collision Shape…, Create Navigation Mesh, Create Outline Mesh…
(`mesh->create_outline(margin)`), Create Debug Tangents, View UV1, View UV2, Unwrap UV2 for Lightmap/AO.
The UV2 path (`:466-559`) is the reference for "modify a mesh safely": refuse meshes that live inside another
scene or have a `.import` sidecar ("Make it unique first"), handle `PrimitiveMesh` via `set_add_uv2`,
otherwise `Ref<ArrayMesh> unwrapped = array_mesh->duplicate(false); unwrapped->lightmap_unwrap(node->get_global_transform());`
then undo action `add_do_method(node, "set_mesh", unwrapped); add_do_reference(unwrapped.ptr()); add_undo_method(node, "set_mesh", array_mesh);`.
Tetra's "enter edit mode" needs the same guards: **imported meshes (`.import` next to the path) and
sub-resources of other scenes must be made unique / saved as `.mesh`/`.res` before being edited.**
UV debug drawing uses `debug_uv->draw_multiline(...)` on a Control inside an `AspectRatioContainer` (`:664-705`).

**MeshInstance3D gizmo** (`gizmos/mesh_instance_3d_gizmo_plugin.cpp`) — `has_gizmo` = MeshInstance3D and not
SoftBody3D; `can_be_hidden() == false`; `redraw` registers `generate_triangle_mesh()` as collision triangles
(with a 2-triangle shortcut for subdivided `PlaneMesh` because "TriangleMesh BVH calculation for every face"
is slow) and adds size handles for Box/Capsule/Cylinder primitive meshes through `Gizmo3DHelper`.

**CSG** (`modules/csg/editor/csg_gizmos.cpp`) — module-side gizmo plugin: `create_material("shape_union_material", gizmo_color)`
etc. in the constructor, `create_handle_material("handles")`; `redraw` turns `cs->get_brush_faces()` into an
edge list for `add_lines`, builds a temporary `ArrayMesh` to get a `TriangleMesh` for `add_collision_triangles`,
adds `add_mesh(mesh, solid_material)` for the translucent solid when selected, and `add_handles(helper->box_get_handles(size), handles_material)`.
Handle dragging: `helper->initialize_handle_action(get_handle_value(...), node->get_global_transform())` in
`begin_handle_action`; `helper->get_segment(p_camera, p_point, sg)` + `helper->box_set_handle(sg, p_id, size, position)` in
`set_handle`; `helper->box_commit_handle(TTR("Change CSG Box Size"), p_cancel, cs)` in `commit_handle`.

### 1.14 Picking maths

`Camera3D` (`scene/3d/camera_3d.h`, impl `camera_3d.cpp:406-529`):

| Function | Definition |
|---|---|
| `virtual Vector3 project_ray_origin(const Point2 &p_pos) const` | Perspective/frustum: `get_camera_transform().origin`. Orthogonal: `pos = cpos / viewport_size; ray = Vector3(pos.x * hsize - hsize/2, (1 - pos.y) * vsize - vsize/2, -near)` transformed to world, where for `KEEP_HEIGHT` (default) `vsize = size, hsize = size * aspect`. |
| `virtual Vector3 project_ray_normal(const Point2 &p_pos) const` | `get_camera_transform().basis.xform(project_local_ray_normal(p_pos)).normalized()`; local: orthogonal ⇒ `(0, 0, -1)`; else `Vector3(((cpos.x / w) * 2 - 1) * he.x, ((1 - cpos.y / h) * 2 - 1) * he.y, -near).normalized()` with `he = projection.get_viewport_half_extents()`. |
| `virtual Point2 unproject_position(const Vector3 &p_pos) const` | `p = cm.xform4(Plane(camera_transform.xform_inv(p_pos), 1.0)); p.normal /= p.d; res = ((p.normal.x * 0.5 + 0.5) * w, (-p.normal.y * 0.5 + 0.5) * h)`. Fails (returns `Point2()` with an error print) when `w == 0`. **Not clipped**: points behind the camera give mirrored garbage. |
| `bool is_position_behind(const Vector3 &p_pos) const` | `(-global_basis.z.normalized()).dot(p_pos - origin) < near`. Always test before `unproject_position`. |
| `virtual Vector3 project_position(const Point2 &p_point, real_t p_z_depth) const` | point at view depth `p_z_depth` under the pixel. |
| `virtual Vector<Plane> get_frustum() const` | 6 world-space planes; outside = `plane.is_point_over(p)`. |
| `bool is_position_in_frustum(const Vector3 &p_position) const` | |
| `virtual Transform3D get_camera_transform() const`, `virtual Projection get_camera_projection() const`, `get_near()`, `get_far()`, `get_fov()`, `get_size()`, `get_projection()` → `PROJECTION_PERSPECTIVE / PROJECTION_ORTHOGONAL / PROJECTION_FRUSTUM` | For a fast batched projection of 100k vertices, build `Projection P = camera->get_camera_projection(); Transform3D V = camera->get_camera_transform().affine_inverse();` once and do the `xform4` maths yourself in the native core rather than calling `unproject_position` per vertex (each call re-derives the projection). |

All require the camera to be inside the tree. Screen coordinates are y-down pixels of the viewport.

`Geometry3D` (namespace, `core/math/geometry_3d.h`) — complete list of what exists:

| Function | Notes |
|---|---|
| `void get_closest_points_between_segments(p0, p1, q0, q1, Vector3 &r_ps, Vector3 &r_qt)` | edge picking in 3D |
| `real_t get_closest_distance_between_segments(p0, p1, q0, q1)` | |
| `inline bool ray_intersects_triangle(const Vector3 &p_from, const Vector3 &p_dir, const Vector3 &p_v0, const Vector3 &p_v1, const Vector3 &p_v2, Vector3 *r_res = nullptr)` | Möller–Trumbore, **two-sided**, accepts `t > 0.00001` |
| `inline bool segment_intersects_triangle(from, to, v0, v1, v2, Vector3 *r_res = nullptr)` | |
| `inline bool segment_intersects_sphere(from, to, sphere_pos, radius, Vector3 *r_res = nullptr, Vector3 *r_norm = nullptr)` | |
| `inline bool segment_intersects_cylinder(from, to, height, radius, r_res, r_norm, int p_cylinder_axis = 2)` | |
| `inline bool segment_intersects_convex(from, to, const Plane *p_planes, int p_plane_count, Vector3 *r_res, Vector3 *r_norm)` | |
| `inline Vector3 get_closest_point_to_segment(point, a, b)` / `(point, const Vector3 *segment)` | clamped |
| `inline Vector3 get_closest_point_to_segment_uncapped(point, a, b)` | infinite line |
| `inline bool point_in_projected_triangle(point, v1, v2, v3)` | |
| `inline bool triangle_sphere_intersection_test(a, b, c, normal, sphere_pos, radius, Vector3 &r_triangle_contact, Vector3 &r_sphere_contact)` | brush-vs-face tests |
| `inline Vector<Vector3> clip_polygon(const Vector<Vector3> &polygon, const Plane &p_plane)` | knife/bisect building block |
| `inline Vector<int32_t> tetrahedralize_delaunay(const Vector<Vector3> &p_points)` | |
| `Vector<Face3> wrap_geometry(const Vector<Face3> &p_array, real_t *p_error = nullptr)` | |
| `struct MeshData` + `MeshData build_convex_mesh(const Vector<Plane>&)`, `build_sphere_planes`, `build_box_planes`, `build_cylinder_planes`, `build_capsule_planes`, `Vector<Vector3> compute_convex_mesh_points(const Plane *p_planes, int p_plane_count)` | frustum → convex points |
| `_FORCE_INLINE_ bool triangle_box_overlap(const Vector3 &boxcenter, const Vector3 boxhalfsize, const Vector3 *triverts)` | |
| `inline Vector3 triangle_get_barycentric_coords(a, b, c, pos)`, `inline Color tetrahedron_get_barycentric_coords(a, b, c, d, pos)` | |
| `Vector<uint32_t> generate_edf(...)`, `Vector<int8_t> generate_sdf8(...)`, `octahedron_map_decode` | |

2D helpers used for screen-space edge picking live in `core/math/geometry_2d.h`
(`Geometry2D::get_closest_point_to_segment(point, a, b)` — three-argument form in this version).

`Plane` (`core/math/plane.h`): `bool intersects_ray(const Vector3 &p_from, const Vector3 &p_dir, Vector3 *p_intersection) const`,
`bool intersects_segment(const Vector3 &p_begin, const Vector3 &p_end, Vector3 *p_intersection) const`,
`is_point_over`, `distance_to`, `project`.

`TriangleMesh` (`core/math/triangle_mesh.h`) — static BVH over a triangle soup:

```cpp
void create(const Vector<Vector3> &p_faces, const Vector<int32_t> &p_surface_indices = Vector<int32_t>());   // 3 vertices per face
bool intersect_ray(const Vector3 &p_begin, const Vector3 &p_dir, Vector3 &r_point, Vector3 &r_normal, int32_t *r_surf_index = nullptr, int32_t *r_face_index = nullptr) const;
bool intersect_segment(const Vector3 &p_begin, const Vector3 &p_end, Vector3 &r_point, Vector3 &r_normal, int32_t *r_surf_index = nullptr, int32_t *r_face_index = nullptr) const;
bool inside_convex_shape(const Plane *p_planes, int p_plane_count, const Vector3 *p_points, int p_point_count, Vector3 p_scale = Vector3(1, 1, 1)) const;
const Vector<Triangle> &get_triangles() const;  const Vector<Vector3> &get_vertices() const;  const Vector<BVH> &get_bvh() const;
Vector<Face3> get_faces() const;  void get_indices(Vector<int> *r_triangles_indices) const;
```

- `intersect_ray` returns the **nearest** hit along the ray, two-sided, with the face plane normal;
  `r_face_index` is the index of the input triangle (`p_faces[3*i..3*i+2]`), so it maps straight back to a
  triangulated-face table.
- Building is O(n log n) and there is **no refit/update** — rebuild after topology or position edits.
  `Mesh::generate_triangle_mesh()` caches it per `Mesh` (`mesh.cpp:290-408`) and the cache is dropped by
  `clear_cache()` (called from `add_surface`/`surface_remove`; **not** from `surface_update_vertex_region`).
  Note `Mesh::generate_triangle_mesh()` computes a `surface_indices` array but calls `triangle_mesh->create(faces)`
  without it (`mesh.cpp:404-405`), so `r_surf_index` is always 0 on that path; use
  `generate_surface_triangle_mesh(int p_surface)` or build your own `TriangleMesh` with surface indices.
- `DynamicBVH` (`core/math/dynamic_bvh.h`, used for the gizmo BVH) supports insert/update/remove with
  `ray_query` / `convex_query` taking a callable; it is the option if an incrementally updatable BVH is needed
  on the Godot side. The Tetra core will more likely carry its own BVH (ported from Blender's BVHTree).

### 1.15 Recommended shape of the Tetra mesh Edit Mode plugin (derived from the above)

```cpp
// modules/tetra_model/editor/tetra_mesh_edit_plugin.h   (sketch; every call below is verified to exist)
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/mesh_instance_3d.h"

class TetraMeshEditPlugin : public EditorPlugin {
	GDCLASS(TetraMeshEditPlugin, EditorPlugin);
	MeshInstance3D *target = nullptr;
	bool edit_mode = false;
	HBoxContainer *toolbar = nullptr;
	RID cage_mesh, cage_instance;            // RS-level overlay on MISC_TOOL_LAYER / GIZMO_EDIT_LAYER
public:
	virtual String get_plugin_name() const override { return "TetraMeshEdit"; }
	virtual bool handles(Object *p_object) const override { return Object::cast_to<MeshInstance3D>(p_object) != nullptr; }
	virtual void edit(Object *p_object) override;          // may be nullptr -> leave edit mode
	virtual void make_visible(bool p_visible) override;    // toolbar->set_visible(p_visible); leave edit mode when false
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual void forward_3d_draw_over_viewport(Control *p_overlay) override;
	TetraMeshEditPlugin() {
		toolbar = memnew(HBoxContainer);
		toolbar->hide();
		Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);
	}
};
```

- Enter edit mode: `target->set_transform_gizmo_visible(false)` **or** register a gizmo plugin exposing the
  selection pivot as subgizmo(s); build the overlay; `update_overlays()`.
- Leave edit mode (also from `edit(nullptr)`, `make_visible(false)`, node removal — connect to the node's
  `tree_exiting`): restore `set_transform_gizmo_visible(true)`, free RS RIDs, `Node3DEditor::get_singleton()->update_transform_gizmo()`.
- Input: `STOP` what you consume, `CUSTOM` for other LMB events while in edit mode, `PASS` for navigation.
- Undo: one `EditorUndoRedoManager` action per operator; `create_action(name, UndoRedo::MERGE_DISABLE, target)` —
  the third parameter `Object *p_custom_context` selects which scene's history the action goes to
  (`editor_undo_redo_manager.h:92`). Methods are invoked by `StringName`, so the object receiving
  `add_do_method(obj, "apply_snapshot", …)` must have that method bound in `_bind_methods`
  (GridMap binds `_set_selection` for this; Path3D binds `_update_toolbar`, `_clear_curve_points`).
  Keep big mesh snapshots in a `RefCounted` held alive with `add_do_reference(obj)` / `add_undo_reference(obj)`.

---

## 2. Mesh data APIs

### 2.1 Surface array layout (`Mesh::ARRAY_*` == `RSE::ARRAY_*`)

`Array arrays; arrays.resize(Mesh::ARRAY_MAX);` (13 slots; the size is checked:
`ERR_FAIL_COND(p_arrays.size() != ARRAY_MAX)`, `scene/resources/mesh.cpp:1822`). Accepted Variant types are
enforced in `RenderingServer::_surface_set_data` / `mesh_create_surface_data_from_arrays`
(`servers/rendering/rendering_server.cpp:426-920, 1179-1240`):

| Idx | Name | Variant type required | Element count | GPU encoding (uncompressed) |
|---|---|---|---|---|
| 0 | `ARRAY_VERTEX` | `PackedVector3Array` (3D) or `PackedVector2Array` (sets `ARRAY_FLAG_USE_2D_VERTICES`) | N (must be > 0 unless `ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY`) | 3 × float32 (12 bytes); 2D: 2 × float32 |
| 1 | `ARRAY_NORMAL` | `PackedVector3Array` | N | octahedral, 2 × uint16 (4 bytes) |
| 2 | `ARRAY_TANGENT` | `PackedFloat32Array` or `PackedFloat64Array` | 4N (x, y, z, w = binormal sign ±1) | octahedral+sign, 2 × uint16 (4 bytes). If normals exist but tangents are nil, an arbitrary tangent is synthesised (`rendering_server.cpp:680-705`). |
| 3 | `ARRAY_COLOR` | `PackedColorArray` | N | RGBA8 (clamped 0..1 → 0..255; **linear vs sRGB is the material's business**, see `FLAG_SRGB_VERTEX_COLOR`) |
| 4 | `ARRAY_TEX_UV` | `PackedVector2Array` (or `PackedVector3Array`) | N | 2 × float32 (8 bytes) |
| 5 | `ARRAY_TEX_UV2` | same | N | 2 × float32 |
| 6-9 | `ARRAY_CUSTOM0..3` | `PackedByteArray` for `RGBA8_UNORM`/`RGBA8_SNORM`/`RG_HALF`/`RGBA_HALF`; `PackedFloat32Array` for `R/RG/RGB/RGBA_FLOAT` | per format | 4/4/4/8/4/8/12/16 bytes; format is packed into the flags at `ARRAY_FORMAT_CUSTOMn_SHIFT` (3 bits each, base bit 13) |
| 10 | `ARRAY_BONES` | `PackedInt32Array` (or `PackedFloat32Array`) | 4N (8N with `ARRAY_FLAG_USE_8_BONE_WEIGHTS`) | uint16 each |
| 11 | `ARRAY_WEIGHTS` | `PackedFloat32Array` / `PackedFloat64Array` | 4N / 8N | unorm16 each |
| 12 | `ARRAY_INDEX` | `PackedInt32Array` | any (must be > 0 if present) | uint16 when `vertex_count <= 65536`, else uint32 |

`RSE::MAX_MESH_SURFACES = 256` per mesh (`rendering_server_enums.h:47`).

### 2.2 Format flags (`Mesh::ArrayFormat : uint64_t`, mirrors `RSE::ArrayFormat`)

| Flag | Value | Meaning |
|---|---|---|
| `ARRAY_FORMAT_VERTEX … ARRAY_FORMAT_INDEX` | `1 << ARRAY_x` | which arrays are present (derived automatically from non-nil slots) |
| `ARRAY_FORMAT_BLEND_SHAPE_MASK` | `VERTEX \| NORMAL \| TANGENT` | arrays a blend shape may carry |
| `ARRAY_FORMAT_CUSTOM_BASE` | 13 | first custom-format bit; `ARRAY_FORMAT_CUSTOM_BITS = 3`, `ARRAY_FORMAT_CUSTOM_MASK = 0x7`; shifts 13/16/19/22 |
| `ARRAY_COMPRESS_FLAGS_BASE` | 25 | |
| `ARRAY_FLAG_USE_2D_VERTICES` | `1 << 25` | |
| `ARRAY_FLAG_USE_DYNAMIC_UPDATE` | `1 << 26` | hint; only the GLES3 backend reads it (`GL_DYNAMIC_DRAW`, `drivers/gles3/storage/mesh_storage.cpp:235`); the RD backend ignores it. Harmless to pass for edit-mode meshes. |
| `ARRAY_FLAG_USE_8_BONE_WEIGHTS` | `1 << 27` | |
| `ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY` | `1 << 28` | |
| `ARRAY_FLAG_COMPRESS_ATTRIBUTES` | `1 << 29` | positions → 4 × unorm16 relative to AABB, normal/tangent packed into the vertex stream, UVs → unorm16. **Do not use for meshes being edited** (changes the binary layout of §2.5 and quantises positions). Requires normals when tangents are present. |
| `ARRAY_FLAG_FORMAT_VERSION_*` | shift 35, 8 bits | `ARRAY_FLAG_FORMAT_CURRENT_VERSION = ARRAY_FLAG_FORMAT_VERSION_2`; added automatically by `mesh_create_surface_data_from_arrays` |

`Mesh::PrimitiveType`: `PRIMITIVE_POINTS, PRIMITIVE_LINES, PRIMITIVE_LINE_STRIP, PRIMITIVE_TRIANGLES,
PRIMITIVE_TRIANGLE_STRIP` (`PRIMITIVE_MAX`). Lines are always 1 px wide; points need a material with
point size (§2.11). There is no quad/n-gon primitive — the edit core must triangulate for display and keep
a tri→face map for picking.

### 2.3 `ArrayMesh` (`scene/resources/mesh.h:300-405`)

```cpp
void add_surface_from_arrays(PrimitiveType p_primitive, const Array &p_arrays,
		const TypedArray<Array> &p_blend_shapes = TypedArray<Array>(), const Dictionary &p_lods = Dictionary(),
		BitField<ArrayFormat> p_flags = 0);
void add_surface(BitField<ArrayFormat> p_format, PrimitiveType p_primitive, const Vector<uint8_t> &p_array,
		const Vector<uint8_t> &p_attribute_array, const Vector<uint8_t> &p_skin_array, int p_vertex_count,
		const Vector<uint8_t> &p_index_array, int p_index_count, const AABB &p_aabb,
		const Vector<uint8_t> &p_blend_shape_data = Vector<uint8_t>(), const Vector<AABB> &p_bone_aabbs = Vector<AABB>(),
		const Vector<RenderingServerTypes::SurfaceData::LOD> &p_lods = Vector<RenderingServerTypes::SurfaceData::LOD>(),
		const Vector4 p_uv_scale = Vector4());
Array surface_get_arrays(int p_surface) const override;            // round-trips through the GPU-format buffers (normals are octahedral-decoded: lossy)
TypedArray<Array> surface_get_blend_shape_arrays(int p_surface) const override;
Dictionary surface_get_lods(int p_surface) const override;
void add_blend_shape(const StringName &p_name);  void clear_blend_shapes();     // only before any surface exists
void set_blend_shape_mode(BlendShapeMode p_mode);                               // default BLEND_SHAPE_MODE_RELATIVE
void surface_update_vertex_region(int p_surface, int p_offset, const Vector<uint8_t> &p_data);
void surface_update_attribute_region(int p_surface, int p_offset, const Vector<uint8_t> &p_data);
void surface_update_skin_region(int p_surface, int p_offset, const Vector<uint8_t> &p_data);
int get_surface_count() const override;
void surface_remove(int p_surface);
void clear_surfaces();
void surface_set_custom_aabb(int p_idx, const AABB &p_aabb);
int surface_get_array_len(int p_idx) const override;  int surface_get_array_index_len(int p_idx) const override;
BitField<ArrayFormat> surface_get_format(int p_idx) const override;  PrimitiveType surface_get_primitive_type(int p_idx) const override;
virtual void surface_set_material(int p_idx, const Ref<Material> &p_material) override;
virtual Ref<Material> surface_get_material(int p_idx) const override;
int surface_find_by_name(const String &p_name) const;  void surface_set_name(int p_idx, const String &p_name);  String surface_get_name(int p_idx) const;
void set_custom_aabb(const AABB &p_custom);  AABB get_aabb() const override;  virtual RID get_rid() const override;
void regen_normal_maps();
Error lightmap_unwrap(const Transform3D &p_base_transform = Transform3D(), float p_texel_size = 0.05);
Error lightmap_unwrap_cached(const Transform3D &p_base_transform, float p_texel_size, const Vector<uint8_t> &p_src_cache, Vector<uint8_t> &r_dst_cache, bool p_generate_cache = true);
void set_shadow_mesh(const Ref<ArrayMesh> &p_mesh);
```

Behavioural facts (from `mesh.cpp:1783-2036`):

- `add_surface*` → `RS::mesh_add_surface`, then `clear_cache()` (drops the cached `TriangleMesh`),
  `notify_property_list_changed()`, `emit_changed()`.
- `surface_update_*_region` → RS call + `emit_changed()`; **AABB is not recomputed and the TriangleMesh cache
  is not cleared**. After a vertex-region update that moves geometry outside the old bounds, call
  `set_custom_aabb()` (or rebuild) or the instance will be frustum-culled incorrectly.
- `clear_surfaces()` → `RS::mesh_clear(mesh); surfaces.clear(); aabb = AABB();` and **does not emit `changed`**.
- `surface_remove()` → RS remove, `clear_cache()`, `_recompute_aabb()`, `emit_changed()`.
- Every `emit_changed()` on a mesh used by a `MeshInstance3D` runs `MeshInstance3D::_mesh_changed`
  (`scene/3d/mesh_instance_3d.cpp:412-439`), which ends with `update_gizmos()`; the deferred gizmo redraw
  (coalesced to once per frame by `Node3D::update_gizmos`, `scene/3d/node_3d.cpp:865-885`) calls
  `MeshInstance3DGizmoPlugin::redraw` → `m->generate_triangle_mesh()`. With `add_surface` in the loop this is a
  **full BVH rebuild of the whole mesh per frame**. For interactive editing either (a) show the edit mesh
  through your own RS mesh/instance and only write the `ArrayMesh` on commit, or (b) keep topology fixed during
  a drag and use `surface_update_vertex_region` (cache survives), or (c) accept the cost for small meshes.
- `ArrayMesh` is `RES_BASE_EXTENSION("mesh")`: `ResourceSaver::save(mesh, "res://x.mesh")` (binary) or
  `.res`/`.tres`.

Minimal correct creation, lines + points (pattern of `EditorNode3DGizmo::add_vertices`, `node_3d_editor_gizmos.cpp:287-333`):

```cpp
Ref<ArrayMesh> mesh;
mesh.instantiate();
Array a;
a.resize(Mesh::ARRAY_MAX);
a[Mesh::ARRAY_VERTEX] = points;      // Vector<Vector3>
a[Mesh::ARRAY_COLOR] = colors;       // Vector<Color>, same size
mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, a);     // or PRIMITIVE_POINTS / PRIMITIVE_TRIANGLES (+ a[Mesh::ARRAY_INDEX])
mesh->surface_set_material(0, material);
```

### 2.4 Binary layout of the surface buffers (needed for `surface_update_*_region`)

From `RenderingServer::mesh_surface_make_offsets_from_format` (`rendering_server.cpp:1055-1177`) for a
non-compressed 3D surface with N vertices:

- **`vertex_data`** = *two consecutive blocks*:
  1. positions: N × 12 bytes (3 × float32), stride 12, starting at byte 0;
  2. normal+tangent block starting at byte `12 * N`: stride = 4 (normal only) or 8 (normal + tangent);
     normal at offset 0 (2 × uint16 octahedral), tangent at offset 4 (2 × uint16, sign folded in).
     (`r_offsets[NORMAL/TANGENT] += r_vertex_element_size * p_vertex_len`.)
- **`attribute_data`** = interleaved per vertex in this order: color (4 bytes RGBA8), UV (8), UV2 (8), custom0..3.
- **`skin_data`** = interleaved bones (4 or 8 × uint16) then weights (4 or 8 × uint16).
- **`index_data`** = uint16 or uint32.

Helpers to query at runtime instead of hard-coding (`rendering_server.h:198-206`):

```cpp
virtual uint32_t mesh_surface_get_format_offset(BitField<RSE::ArrayFormat> p_format, int p_vertex_len, int p_array_index) const;
virtual uint32_t mesh_surface_get_format_vertex_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
virtual uint32_t mesh_surface_get_format_normal_tangent_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
virtual uint32_t mesh_surface_get_format_attribute_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
virtual uint32_t mesh_surface_get_format_skin_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
virtual uint32_t mesh_surface_get_format_index_stride(BitField<RSE::ArrayFormat> p_format, int p_vertex_len) const;
```

Moving vertices cheaply during a drag (positions only): build a `Vector<uint8_t>` of `12 * count` bytes of
float32 xyz for a contiguous vertex range `[first, first+count)` and call
`mesh->surface_update_vertex_region(surface, first * 12, bytes)`. The RD implementation is a plain
`RD::buffer_update(vertex_buffer, p_offset, size, data)` (`servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp:571-582`)
— no validation beyond non-empty data and a valid buffer, so offsets must be right. To also refresh shading,
write the normal block the same way using `Vector3::octahedron_encode()` → `uint16(CLAMP(res.x * 65535, 0, 65535))`
(`rendering_server.cpp:620-627`); tangents use `octahedron_tangent_encode(sign)` with the
`(0, 65535) → (65535, 65535)` fix-up (`:645-657`). Simpler alternative with identical cost class: regenerate the
whole surface with `RS::mesh_clear` + `mesh_add_surface_from_arrays` on a private RS mesh (GridMap does this for
its overlays).

`RenderingServerTypes::SurfaceData` (`servers/rendering/rendering_server_types.h:83-112`):
`primitive`, `format`, `vertex_data` ("Vertex, Normal, Tangent"), `attribute_data` ("Color, UV, UV2, Custom0-3"),
`skin_data`, `vertex_count`, `index_data`, `index_count`, `aabb`, `lods` (`edge_length`, `index_data`),
`bone_aabbs`, `mesh_to_skeleton_xform`, `blend_shape_data`, `uv_scale`, `material`.

### 2.5 RenderingServer mesh/instance calls for cheap dynamic overlays

```cpp
virtual RID mesh_create() = 0;
virtual RID mesh_create_from_surfaces(const Vector<RenderingServerTypes::SurfaceData> &p_surfaces, int p_blend_shape_count = 0) = 0;
virtual Error mesh_create_surface_data_from_arrays(RenderingServerTypes::SurfaceData *r_surface_data, RSE::PrimitiveType p_primitive, const Array &p_arrays, const Array &p_blend_shapes = Array(), const Dictionary &p_lods = Dictionary(), uint64_t p_compress_format = 0);
virtual void mesh_add_surface_from_arrays(RID p_mesh, RSE::PrimitiveType p_primitive, const Array &p_arrays, const Array &p_blend_shapes = Array(), const Dictionary &p_lods = Dictionary(), BitField<RSE::ArrayFormat> p_compress_format = 0);
virtual void mesh_add_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface) = 0;
virtual void mesh_surface_update_vertex_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
virtual void mesh_surface_update_attribute_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
virtual void mesh_surface_update_skin_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;
virtual void mesh_surface_update_index_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) = 0;   // RS-only; ArrayMesh has no wrapper
virtual void mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) = 0;
virtual RenderingServerTypes::SurfaceData mesh_get_surface(RID p_mesh, int p_surface) const = 0;
Array mesh_surface_get_arrays(RID p_mesh, int p_surface) const;
virtual int mesh_get_surface_count(RID p_mesh) const = 0;
virtual void mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) = 0;
virtual void mesh_surface_remove(RID p_mesh, int p_surface) = 0;
virtual void mesh_clear(RID p_mesh) = 0;
virtual RID instance_create2(RID p_base, RID p_scenario);
virtual void instance_set_base(RID p_instance, RID p_base) = 0;
virtual void instance_set_scenario(RID p_instance, RID p_scenario) = 0;
virtual void instance_set_layer_mask(RID p_instance, uint32_t p_mask) = 0;
virtual void instance_set_transform(RID p_instance, const Transform3D &p_transform) = 0;
virtual void instance_attach_object_instance_id(RID p_instance, ObjectID p_id) = 0;
virtual void instance_set_surface_override_material(RID p_instance, int p_surface, RID p_material) = 0;
virtual void instance_set_visible(RID p_instance, bool p_visible) = 0;
virtual void instance_set_custom_aabb(RID p_instance, AABB aabb) = 0;
virtual void instance_set_extra_visibility_margin(RID p_instance, real_t p_margin) = 0;
virtual void instance_set_ignore_culling(RID p_instance, bool p_enabled) = 0;
virtual void instance_set_pivot_data(RID p_instance, float p_sorting_offset, bool p_use_aabb_center) = 0;
virtual void instance_geometry_set_flag(RID p_instance, RSE::InstanceFlags p_flags, bool p_enabled) = 0;   // INSTANCE_FLAG_USE_BAKED_LIGHT, _USE_DYNAMIC_GI, _DRAW_NEXT_FRAME_IF_VISIBLE, _IGNORE_OCCLUSION_CULLING
virtual void instance_geometry_set_cast_shadows_setting(RID p_instance, RSE::ShadowCastingSetting p_shadow_casting_setting) = 0;   // SHADOW_CASTING_SETTING_OFF/ON/DOUBLE_SIDED/SHADOWS_ONLY
virtual void instance_geometry_set_material_override(RID p_instance, RID p_material) = 0;
virtual void instance_geometry_set_material_overlay(RID p_instance, RID p_material) = 0;
virtual void instance_geometry_set_transparency(RID p_instance, float p_transparency) = 0;
virtual void free_rid(RID p_rid) = 0;
```

The gizmo recipe for an overlay instance (`node_3d_editor_gizmos.cpp:216-230`), to copy verbatim:

```cpp
instance = RS::get_singleton()->instance_create2(mesh_rid, node->get_world_3d()->get_scenario());
RS::get_singleton()->instance_attach_object_instance_id(instance, node->get_instance_id());
RS::get_singleton()->instance_set_extra_visibility_margin(instance, 1);                       // for point handles
RS::get_singleton()->instance_geometry_set_cast_shadows_setting(instance, RSE::SHADOW_CASTING_SETTING_OFF);
RS::get_singleton()->instance_set_layer_mask(instance, 1 << Node3DEditorViewport::GIZMO_EDIT_LAYER);
RS::get_singleton()->instance_geometry_set_flag(instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
RS::get_singleton()->instance_geometry_set_flag(instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
RS::get_singleton()->instance_set_transform(instance, node->get_global_transform());
```

RS instances do not follow the node: update the transform yourself (connect to nothing — poll in `_process`
or listen for `NOTIFICATION_TRANSFORM_CHANGED` by `set_notify_transform(true)` on a helper Node3D child). The
helper-node alternative (Polygon3D/Skeleton3D) gets transform/visibility inheritance for free.

To temporarily replace how the edited node itself is drawn (e.g. show the cage's display mesh instead of the
saved mesh) use `RS::instance_set_base(node->get_instance(), my_mesh_rid)` — not seen in-tree for this purpose;
simpler and proven: `node->set_mesh(edit_display_mesh)` inside the edit session and restore on exit, or hide the
node's own instance with `RS::instance_set_visible(node->get_instance(), false)` and draw your own.

### 2.6 `SurfaceTool` (`scene/resources/surface_tool.h`)

```cpp
void begin(Mesh::PrimitiveType p_primitive);
void set_color(Color p_color);  void set_normal(const Vector3 &p_normal);  void set_tangent(const Plane &p_tangent);
void set_uv(const Vector2 &p_uv);  void set_uv2(const Vector2 &p_uv2);  void set_custom(int p_channel_index, const Color &p_custom);
void set_bones(const Vector<int> &p_bones);  void set_weights(const Vector<float> &p_weights);  void set_smooth_group(uint32_t p_group);
void set_skin_weight_count(SkinWeightCount p_weights);  void set_custom_format(int p_channel_index, CustomFormat p_format);
void add_vertex(const Vector3 &p_vertex);          // commits the "current" attributes
void add_triangle_fan(const Vector<Vector3> &p_vertices, const Vector<Vector2> &p_uvs = …, const Vector<Color> &p_colors = …, const Vector<Vector2> &p_uv2s = …, const Vector<Vector3> &p_normals = …, const Vector<Plane> &p_tangents = …);
void add_index(int p_index);
void index();  void deindex();
void generate_normals(bool p_flip = false);
void generate_tangents();
void optimize_indices_for_cache();
AABB get_aabb() const;
Vector<int> generate_lod(float p_threshold, int p_target_index_count = 3);
void set_material(const Ref<Material> &p_material);
void clear();
LocalVector<Vertex> &get_vertex_array();
void create_from_triangle_arrays(const Array &p_arrays);
void create_from_arrays(const Array &p_arrays, Mesh::PrimitiveType p_primitive_type = Mesh::PRIMITIVE_TRIANGLES);
static void create_vertex_array_from_arrays(const Array &p_arrays, LocalVector<Vertex> &ret, uint64_t *r_format = nullptr);
Array commit_to_arrays();
void create_from(const Ref<Mesh> &p_existing, int p_surface);
void create_from_blend_shape(const Ref<Mesh> &p_existing, int p_surface, const String &p_blend_shape_name);
void append_from(const Ref<Mesh> &p_existing, int p_surface, const Transform3D &p_xform);
Ref<ArrayMesh> commit(const Ref<ArrayMesh> &p_existing = Ref<ArrayMesh>(), uint64_t p_compress_flags = 0);   // appends a surface to p_existing
```

- `generate_normals`: `ERR_FAIL_COND(primitive != Mesh::PRIMITIVE_TRIANGLES)`; deindexes, computes face
  normals, merges per `(position, smooth_group)` (smooth group `UINT32_MAX` = flat), re-indexes if it was indexed
  (`surface_tool.cpp:1188-1194`).
- `generate_tangents`: requires UVs (`"UVs are required to generate tangents."`) and normals; runs **mikktspace**
  (`thirdparty/misc/mikktspace.h`, the same library Blender uses) (`surface_tool.cpp:1157-1158`).
- Static function pointers filled by the `meshoptimizer` module:
  `SurfaceTool::optimize_vertex_cache_func`, `optimize_vertex_fetch_remap_func`, `simplify_func`,
  `simplify_with_attrib_func`, `simplify_scale_func`, `generate_remap_func`, `remap_vertex_func`, `remap_index_func`
  (decimation/LOD building blocks callable from Tetra; null if the module is disabled).
- `SurfaceTool::Vertex` fields: `smooth_group, color, normal, binormal, tangent, uv, uv2, custom[4], vertex, bones, weights`.

### 2.7 `MeshDataTool` (`scene/resources/mesh_data_tool.h`)

```cpp
Error create_from_surface(const Ref<ArrayMesh> &p_mesh, int p_surface);   // PRIMITIVE_TRIANGLES only
Error commit_to_surface(const Ref<ArrayMesh> &p_mesh, uint64_t p_compression_flags = 0);   // ADDS a new surface
int get_vertex_count() const;  int get_edge_count() const;  int get_face_count() const;
Vector3 get_vertex(int) const;  void set_vertex(int, const Vector3 &);  (same pattern: normal, tangent(Plane), uv, uv2, color, bones, weights, meta)
Vector<int> get_vertex_edges(int) const;  Vector<int> get_vertex_faces(int) const;
int get_edge_vertex(int p_edge, int p_vertex) const;  Vector<int> get_edge_faces(int p_edge) const;
int get_face_vertex(int p_face, int p_vertex) const;  int get_face_edge(int p_face, int p_vertex) const;  Vector3 get_face_normal(int p_face) const;
```

Triangle-only vertex/edge/face adjacency over the *render* vertices (split at UV/normal seams), storing
`Vector<int>` per element — fine for small tools, not a substitute for a BMesh-style editable mesh. Tetra's
`tetra_model` core should own topology and only *emit* arrays.

### 2.8 `ImmediateMesh` (`scene/resources/immediate_mesh.h`)

```cpp
void surface_begin(PrimitiveType p_primitive, const Ref<Material> &p_material = Ref<Material>());
void surface_set_color(const Color &p_color);  void surface_set_normal(const Vector3 &);  void surface_set_tangent(const Plane &);
void surface_set_uv(const Vector2 &);  void surface_set_uv2(const Vector2 &);
void surface_add_vertex(const Vector3 &p_vertex);  void surface_add_vertex_2d(const Vector2 &p_vertex);
void surface_end();
void clear_surfaces();
```

Non-indexed; rebuilt from scratch each time; `surface_end` errors on zero vertices
(`"No vertices were added, surface can't be created."`). Used for the editor ruler, Polygon3D lines, Skeleton
handles. Good for ≤ a few thousand vertices; for the edit cage prefer arrays + region updates.

### 2.9 `ImporterMesh` (`scene/resources/3d/importer_mesh.h`)

Keeps the *un-quantised* `Array`s per surface (importers use it "so the data is not registered (hence, quality
loss)"): `add_surface(Mesh::PrimitiveType, const Array &p_arrays, const TypedArray<Array> &p_blend_shapes = …, const Dictionary &p_lods = …, const Ref<Material> &p_material = …, const String &p_surface_name = String(), const uint64_t p_flags = 0)`,
`get_surface_arrays(int)`, `get_surface_blend_shape_arrays(int, int)`, `set_surface_material`, `set_surface_name`,
`optimize_indices()`, `generate_lods(float p_normal_merge_angle, Array p_skin_pose_transform_array)`,
`create_shadow_mesh()`, `Ref<ArrayMesh> get_mesh(const Ref<ArrayMesh> &p_base = Ref<ArrayMesh>())`,
`static Ref<ImporterMesh> from_mesh(const Ref<Mesh> &p_mesh)`,
`static Ref<ImporterMesh> merge_importer_meshes(...)`, `lightmap_unwrap_cached(...)`,
`create_trimesh_shape()`, `create_convex_shape()`, `create_navigation_mesh()`.
Useful as the exact-data handoff type between the Tetra core and Godot (LOD + shadow mesh generation for free
via `generate_lods` + `get_mesh`).

### 2.10 Normals, tangents, UV unwrap, collision

- Normals/tangents: `SurfaceTool::generate_normals` / `generate_tangents` (above). A native core will normally
  compute its own (angle-weighted, custom split normals) and pass `ARRAY_NORMAL`/`ARRAY_TANGENT` directly.
- UV unwrap hook (xatlas): a global function pointer defined in `scene/resources/mesh.cpp:2071` and set by
  `modules/xatlas_unwrap/register_types.cpp:236`:
  ```cpp
  extern bool (*array_mesh_lightmap_unwrap_callback)(float p_texel_size, const float *p_vertices, const float *p_normals,
  		int p_vertex_count, const int *p_indices, int p_index_count, const uint8_t *p_cache_data, bool *r_use_cache,
  		uint8_t **r_mesh_cache, int *r_mesh_cache_size, float **r_uv, int **r_vertex, int *r_vertex_count,
  		int **r_index, int *r_index_count, int *r_size_hint_x, int *r_size_hint_y);
  ```
  Inputs: flat xyz positions/normals (already transformed by `p_base_transform` in the callers) and triangle
  indices; outputs: new UVs (2 floats per output vertex), `r_vertex` = for each output vertex the index of the
  source vertex it was split from, new index buffer, atlas size hint. `ArrayMesh::lightmap_unwrap` writes the
  result to **UV2** and rebuilds all surfaces; it fails with `ERR_UNCONFIGURED` when the pointer is null and
  rejects blend shapes / non-triangle surfaces / missing normals (checks mirrored in
  `mesh_instance_3d_editor_plugin.cpp:515-539`). Tetra can call the pointer directly (declare it `extern`
  exactly as `importer_mesh.cpp:1308` does) to get an automatic *UV1* unwrap ("Smart UV Project"-like) without
  Blender; the returned memory is `memalloc`-style and must be freed the way `mesh.cpp:2194-2275` does (read that
  block before use — it was not transcribed here).
- Collision / picking structures on `Mesh` (`mesh.h:186-206`):
  `Vector<Face3> get_faces() const`, `Vector<Face3> get_surface_faces(int)`, `Ref<TriangleMesh> generate_triangle_mesh() const`
  (cached), `Ref<TriangleMesh> generate_surface_triangle_mesh(int p_surface) const`,
  `void generate_debug_mesh_lines(Vector<Vector3> &r_lines)` (wireframe edge list),
  `Ref<Mesh> create_outline(float p_margin) const`,
  `Ref<ConcavePolygonShape3D> create_trimesh_shape() const`,
  `Ref<ConvexPolygonShape3D> create_convex_shape(bool p_clean = true, bool p_simplify = false) const`,
  `Vector<Ref<Shape3D>> convex_decompose(const Ref<MeshConvexDecompositionSettings> &) const` (V-HACD via
  `Mesh::convex_decomposition_function`), `void clear_cache() const`.
- `MeshInstance3D` (`scene/3d/mesh_instance_3d.h`): `set_mesh(const Ref<Mesh>&)`,
  `set_surface_override_material(int, const Ref<Material>&)`, `get_active_material(int)`,
  `create_trimesh_collision()`, `create_debug_tangents()`,
  `bake_mesh_from_current_blend_shape_mix(...)`, `bake_mesh_from_current_skeleton_pose(...)`;
  from `GeometryInstance3D`/`VisualInstance3D` (`scene/3d/visual_instance_3d.h`): `set_material_override`,
  `set_material_overlay`, `set_cast_shadows_setting`, `set_layer_mask(uint32_t)`, `set_extra_cull_margin`,
  `set_custom_aabb(AABB)`, `set_ignore_occlusion_culling(bool)`, `set_sorting_offset(float)`,
  `RID get_instance() const`, `RID get_base() const`.

### 2.11 Materials for editor overlays (`BaseMaterial3D`, `scene/resources/material.h`)

| Need | Call | Notes |
|---|---|---|
| Unshaded | `set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED)` | others: `SHADING_MODE_PER_PIXEL`, `SHADING_MODE_PER_VERTEX` |
| Alpha blending | `set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA)` | enum: `TRANSPARENCY_DISABLED, _ALPHA, _ALPHA_SCISSOR, _ALPHA_HASH, _ALPHA_DEPTH_PRE_PASS` |
| No depth test (x-ray) | `set_flag(StandardMaterial3D::FLAG_DISABLE_DEPTH_TEST, true)` | |
| "On top" preset | `set_on_top_of_alpha()` = `set_transparency(TRANSPARENCY_DISABLED); set_render_priority(RENDER_PRIORITY_MAX); set_flag(FLAG_DISABLE_DEPTH_TEST, true);` (`material.cpp:3041-3045`) | **Call it before `set_transparency(TRANSPARENCY_ALPHA)`** if you also want blending — in-tree code does exactly that order (`grid_map_editor_plugin.cpp:1863-1865`, `node_3d_editor_gizmos.cpp:999-1004`) |
| Vertex colours | `set_flag(FLAG_ALBEDO_FROM_VERTEX_COLOR, true)` + `set_flag(FLAG_SRGB_VERTEX_COLOR, true)` | the second converts sRGB vertex colours to linear in the shader; all editor overlays set both |
| Point size | `set_flag(FLAG_USE_POINT_SIZE, true); set_point_size(px);` | for `PRIMITIVE_POINTS`; albedo texture is sampled with `POINT_COORD` (handle icon) |
| Billboard | `set_billboard_mode(BILLBOARD_ENABLED)` (also `BILLBOARD_FIXED_Y`, `BILLBOARD_PARTICLES`); `set_flag(FLAG_BILLBOARD_KEEP_SCALE, true)` | |
| Constant screen size | `set_flag(FLAG_FIXED_SIZE, true)` | used for icon gizmos |
| Draw order among transparents | `set_render_priority(int)`; range `Material::RENDER_PRIORITY_MIN = -128 … RENDER_PRIORITY_MAX = 127` | gizmo line materials use `RENDER_PRIORITY_MIN + 1` |
| Two-sided | `set_cull_mode(StandardMaterial3D::CULL_DISABLED)` (`CULL_BACK`, `CULL_FRONT`) | |
| No fog | `set_flag(FLAG_DISABLE_FOG, true)` | set on every editor overlay material |
| Depth write | `set_depth_draw_mode(DEPTH_DRAW_OPAQUE_ONLY / DEPTH_DRAW_ALWAYS / DEPTH_DRAW_DISABLED)`; `set_depth_test(DEPTH_TEST_DEFAULT / DEPTH_TEST_INVERTED)` | `DEPTH_TEST_INVERTED` = draw only where occluded (x-ray "hidden" pass) |
| Blend mode | `set_blend_mode(BLEND_MODE_MIX / ADD / SUB / MUL / PREMULT_ALPHA)` | |
| Stencil outline / x-ray | `set_stencil_mode(STENCIL_MODE_OUTLINE / STENCIL_MODE_XRAY / STENCIL_MODE_CUSTOM)`, `set_stencil_effect_color`, `set_stencil_effect_outline_thickness` | new in 4.5+; a ready-made selection outline for faces/objects |
| Polygon offset substitute | There is no depth-bias/polygon-offset property in `BaseMaterial3D`. Proven in-tree technique: a `ShaderMaterial` writing `POSITION.z = mix(POSITION.z, POSITION.w, k)` (Skeleton3D shaders, k = 0.998/0.999). `FLAG_USE_Z_CLIP_SCALE` + `set_z_clip_scale(float)` also exists (clip-space z scaling) but its suitability for wire-over-face drawing was not tested. | needed to draw wire/vertices over coplanar faces without z-fighting |
| Second pass | `Material::set_next_pass(const Ref<Material>&)` | e.g. solid pass + x-ray pass |
| Custom shader | `ShaderMaterial::set_shader(const Ref<Shader>&)`, `set_shader_parameter(const StringName&, const Variant&)`; `Shader::set_code(String)` | |

Complete flag list (`BaseMaterial3D::Flags`): `FLAG_DISABLE_DEPTH_TEST, FLAG_ALBEDO_FROM_VERTEX_COLOR,
FLAG_SRGB_VERTEX_COLOR, FLAG_USE_POINT_SIZE, FLAG_FIXED_SIZE, FLAG_BILLBOARD_KEEP_SCALE,
FLAG_UV1_USE_TRIPLANAR, FLAG_UV2_USE_TRIPLANAR, FLAG_UV1_USE_WORLD_TRIPLANAR, FLAG_UV2_USE_WORLD_TRIPLANAR,
FLAG_AO_ON_UV2, FLAG_EMISSION_ON_UV2, FLAG_ALBEDO_TEXTURE_FORCE_SRGB, FLAG_DONT_RECEIVE_SHADOWS,
FLAG_DISABLE_AMBIENT_LIGHT, FLAG_USE_SHADOW_TO_OPACITY, FLAG_USE_TEXTURE_REPEAT, FLAG_INVERT_HEIGHTMAP,
FLAG_SUBSURFACE_MODE_SKIN, FLAG_PARTICLE_TRAILS_MODE, FLAG_ALBEDO_TEXTURE_MSDF, FLAG_DISABLE_FOG,
FLAG_DISABLE_SPECULAR_OCCLUSION, FLAG_USE_Z_CLIP_SCALE, FLAG_USE_FOV_OVERRIDE`.

How in-tree code draws the three element kinds:

| Element | In-tree technique | Source |
|---|---|---|
| Vertices | `PRIMITIVE_POINTS` mesh + handle material (`FLAG_USE_POINT_SIZE`, icon albedo, vertex colour for state, on-top); selected ones drawn as a **separate surface/mesh with a different material/colour** | `EditorNode3DGizmo::add_handles`; Path3D `redraw` splits points into first/last/common/selected arrays with 5 handle materials (`path_3d_editor_plugin.cpp:1157-1185`) |
| Edges / wire | `PRIMITIVE_LINES` mesh, unshaded + vertex colour; edge list from triangle faces (`CSGShape3DGizmoPlugin::redraw`) or `Mesh::generate_debug_mesh_lines` | `csg_gizmos.cpp:385-421` |
| Faces highlight | translucent triangle mesh with an alpha material (`add_mesh(mesh, solid_material)`), or the selection-box approach: two instances, one normal + one "xray" material | `csg_gizmos.cpp:440-456`; `node_3d_editor_plugin.cpp:7492-7527` |

Per-element state should be encoded in **vertex colours** (one draw call per element kind) and updated with
`surface_update_attribute_region` (colour is the first attribute: 4 bytes RGBA8 at offset 0 of each attribute
stride) rather than by splitting meshes per state.

### 2.12 Screen-space overlays through `forward_3d_draw_over_viewport`

See §1.4. Checklist: project with `p_camera->unproject_position(world)` after `!is_position_behind(world)`;
clip to `Rect2(Point2(), p_overlay->get_size())` yourself for large point sets; batch with
`draw_multiline(points, color, width)` / `draw_polyline` / `draw_multiline_colors` /
`draw_primitive` / `draw_colored_polygon` (one canvas command each) rather than thousands of `draw_line`
calls; dashed selection outlines: `draw_dashed_line(from, to, color, width, dash, aligned, antialiased)`.
Remember the redraw caveat (camera motion does not redraw).

---

## 3. Image and texture APIs (GIMP-style layered painter)

### 3.1 `Image` formats (`core/io/image.h:66-116`) and pixel sizes (`image.cpp:134-230`)

| Format | Bytes/px | Channel type | Notes for the painter |
|---|---|---|---|
| `FORMAT_L8` | 1 | u8 | grayscale mask |
| `FORMAT_LA8` | 2 | u8 | |
| `FORMAT_R8` | 1 | u8 | selection mask / layer mask upload |
| `FORMAT_RG8` | 2 | u8 | |
| `FORMAT_RGB8` | 3 | u8 | **converted to RGBA8 on upload by the RD renderer** (`texture_storage.cpp:2303-2319`, the direct path is disabled with `if (false && …)`) — avoid for per-frame updates |
| `FORMAT_RGBA8` | 4 | u8 | default 8-bit working/display format; uploads without conversion |
| `FORMAT_RGBA4444`, `FORMAT_RGB565` | 2 | packed | |
| `FORMAT_RF` / `RGF` / `RGBF` / `RGBAF` | 4 / 8 / 12 / 16 | float32 | `RGBAF` = GEGL/babl `"RGBA float"` layout; `RGBF` may be reconverted on upload |
| `FORMAT_RH` / `RGH` / `RGBH` / `RGBAH` | 2 / 4 / 6 / 8 | float16 | good GPU display format for float canvases |
| `FORMAT_RGBE9995` | 4 | shared exponent | |
| `FORMAT_R16` / `RG16` / `RGB16` / `RGBA16` | 2 / 4 / 6 / 8 | unorm16 | new integer-16 formats; `RGBA16` is reconverted to `RGBAF` if unsupported (`texture_storage.cpp:2799-2805`) |
| `FORMAT_R16I` / `RG16I` / `RGB16I` / `RGBA16I` | 2 / 4 / 6 / 8 | uint16 (integer) | |
| `FORMAT_DXT1/3/5`, `RGTC_R/RG`, `BPTC_RGBA/RGBF/RGBFU`, `ETC`, `ETC2_*`, `ASTC_4x4/8x8(_HDR)`, `*_RA_AS_RG` | block | compressed | not editable (`convert`, `blit_rect`, `resize` reject compressed) |

Limits: `MAX_WIDTH = MAX_HEIGHT = 1 << 24`, `MAX_PIXELS = 268435456` (16384²).
Data is row-major, top-left origin, no row padding, channels interleaved in the order of the format name;
mipmaps (if any) follow the base level in the same `Vector<uint8_t>`.

### 3.2 Creating and accessing pixels

```cpp
static Ref<Image> create_empty(int p_width, int p_height, bool p_use_mipmaps, Format p_format);        // zero-filled
static Ref<Image> create_from_data(int p_width, int p_height, bool p_use_mipmaps, Format p_format, const Vector<uint8_t> &p_data);
void set_data(int p_width, int p_height, bool p_use_mipmaps, Format p_format, const Vector<uint8_t> &p_data);
void initialize_data(int p_width, int p_height, bool p_use_mipmaps, Format p_format);
void initialize_data(int p_width, int p_height, bool p_use_mipmaps, Format p_format, const Vector<uint8_t> &p_data);
Image(int p_width, int p_height, bool p_mipmaps, Format p_format, const Vector<uint8_t> &p_data);
Vector<uint8_t> get_data() const;        // returns the CoW Vector (no copy until someone writes)
const uint8_t *ptr() const;  uint8_t *ptrw();  int64_t get_data_size() const;
int get_width() const;  int get_height() const;  Size2i get_size() const;  Format get_format() const;
Color get_pixel(int p_x, int p_y) const;  void set_pixel(int p_x, int p_y, const Color &p_color);   // slow per-pixel path
static int get_format_pixel_size(Format p_format);
static int64_t get_image_data_size(int p_width, int p_height, Format p_format, bool p_mipmaps = false);
void copy_internals_from(const Ref<Image> &p_image);
Ref<Image> get_region(const Rect2i &p_area) const;  Rect2i get_used_rect() const;
```

`Vector<uint8_t>` is copy-on-write, so `create_from_data(w, h, false, fmt, buffer)` **shares** `buffer`
(this is what Theora calls "zero copy image creation", `modules/theora/video_stream_theora.cpp:252-255`); a
later `ptrw()` on either side triggers a copy if the refcount is > 1. For a tile/layer store owned by a
`std::vector<float>` in the engine-independent core, one memcpy into a persistent `Vector<uint8_t>` per upload
is unavoidable; keep one `Vector<uint8_t>`/`Image` per display texture and reuse it (`ptrw()` on a uniquely
referenced vector does not reallocate).

### 3.3 Pixel operations available on `Image`

| Function | Constraints / behaviour |
|---|---|
| `void convert(Format p_new_format)` | not to/from compressed (`image.cpp:572-574`); generic per-pixel conversion for non-"compatible" pairs — treat as slow for large canvases |
| `void blit_rect(const Ref<Image> &p_src, const Rect2i &p_src_rect, const Point2i &p_dest)` | formats must match, uncompressed; clipped; **memcpy per row** (single memcpy when full-width) — fast region copy |
| `void blit_rect_mask(src, mask, src_rect, dest)` | copies where mask alpha ≠ 0; per-pixel |
| `void blend_rect(src, src_rect, dest)` | formats must match; **per-pixel `get_pixel`/`Color::blend`/`set_pixel`** (`image.cpp:3212-3245`) — "over" on straight alpha, skips `a == 0`; far too slow for brush dabs or layer compositing. Compositing must live in the Tetra core or on the GPU. |
| `void blend_rect_mask(src, mask, src_rect, dest)` | same, masked |
| `void fill(const Color &)`, `void fill_rect(const Rect2i &, const Color &)` | |
| `void resize(int w, int h, Interpolation = INTERPOLATE_BILINEAR)` | `INTERPOLATE_NEAREST, _BILINEAR, _CUBIC, _TRILINEAR, _LANCZOS`; not for compressed; limits above |
| `void resize_to_po2(bool p_square = false, Interpolation = INTERPOLATE_BILINEAR)`, `shrink_x2()`, `crop(w, h)`, `crop_from_point(x, y, w, h)` | |
| `void rotate_90(ClockDirection)`, `rotate_180()`, `flip_x()`, `flip_y()` | |
| `Error generate_mipmaps(bool p_renormalize = false)`, `clear_mipmaps()`, `Ref<Image> get_image_from_mipmap(int) const` | |
| `void srgb_to_linear()`, `void linear_to_srgb()` | **only `FORMAT_RGB8` / `FORMAT_RGBA8`** (256-entry LUT, alpha untouched; `ERR_FAIL_COND` otherwise, `image.cpp:4175-4213`) |
| `void premultiply_alpha()` | RGBA8 only (`image.cpp:4242`) |
| `void fix_alpha_edges()`, `normalize()`, `bump_map_to_normal_map(float)`, `adjust_bcs(b, c, s)` | |
| `AlphaMode detect_alpha() const` → `ALPHA_NONE / ALPHA_BIT / ALPHA_BLEND`; `bool is_invisible() const` | |
| `UsedChannels detect_used_channels(...)`, `optimize_channels()` | |
| `Dictionary compute_image_metrics(const Ref<Image> p_compared_image, bool p_luma_metric = true)` | PSNR etc. — handy for tests |

Per-colour helpers (`core/math/color.h`): `Color::srgb_to_linear() const`, `Color::linear_to_srgb() const`
(exact piecewise sRGB), `Color::blend(const Color &p_over) const`, `to_rgba32()`, `to_abgr32()`,
`get_luminance()`, `Color::from_rgba8(r, g, b, a)`, `Color::hex(uint32_t)`.

### 3.4 Loading and saving — complete matrix for this build

Loaders are `ImageFormatLoader`s registered with `ImageLoader::add_image_format_loader`, plus per-format
"mem loader" function pointers on `Image`; savers are static function pointers on `Image`.

| Format | Extensions | File load (`Image::load` / `Image::load_from_file` / `ImageLoader::load_image`) | Buffer load | Save to file | Save to buffer | Module |
|---|---|---|---|---|---|---|
| PNG | `png` | yes | `load_png_from_buffer` | `save_png(path)` | `save_png_to_buffer()` | `drivers/png` |
| JPEG | `jpg`, `jpeg` | yes | `load_jpg_from_buffer` | `save_jpg(path, float quality = 0.75)` | `save_jpg_to_buffer(quality)` | `modules/jpg` (libjpeg-turbo) |
| WebP | `webp` | yes | `load_webp_from_buffer` | `save_webp(path, bool lossy = false, float quality = 0.75f)` | `save_webp_to_buffer(lossy, quality)` | `modules/webp` |
| OpenEXR | `exr` | yes | `load_exr_from_buffer` | `save_exr(path, bool grayscale = false, bool color_image = false, float max_value = -1.0f)` | `save_exr_to_buffer(...)` | `modules/tinyexr` |
| TGA | `tga` | yes | `load_tga_from_buffer` | — | — | `modules/tga` |
| BMP | `bmp` | yes | `load_bmp_from_buffer` | — | — | `modules/bmp` |
| Radiance HDR | `hdr` | yes | — (no mem loader pointer) | — | — | `modules/hdr` |
| SVG | `svg` | yes (rasterised; `p_scale`) | `load_svg_from_buffer(buf, float scale = 1.0)`, `load_svg_from_string(str, scale)` | — | — | `modules/svg` (ThorVG) |
| KTX/KTX2 | `ktx`, `ktx2` | via `ResourceLoader` (`ResourceFormatLoader`, not an `ImageFormatLoader`) | `load_ktx_from_buffer` | — | — | `modules/ktx` |
| DDS | `dds` | via `ResourceLoader` | `load_dds_from_buffer` | `save_dds(path)` | `save_dds_to_buffer()` | `modules/dds` |

```cpp
Error Image::load(const String &p_path);                       // = ImageLoader::load_image(path, this); in DEBUG_ENABLED builds (the editor) it first
                                                               // prints "Loaded resource as image file, this will not work on export: …" when the path
                                                               // is res:// and a resource exists there (image.cpp:2766-2774). To open a project image
                                                               // for editing without that warning call ImageLoader::load_image() directly.
static Ref<Image> Image::load_from_file(const String &p_path);
static Error ImageLoader::load_image(const String &p_file, Ref<Image> p_image, Ref<FileAccess> p_custom = Ref<FileAccess>(),
		BitField<ImageFormatLoader::LoaderFlags> p_flags = ImageFormatLoader::FLAG_NONE, float p_scale = 1.0);
static void ImageLoader::get_recognized_extensions(List<String> *p_extensions);
// ImageFormatLoader::LoaderFlags: FLAG_NONE = 0, FLAG_FORCE_LINEAR = 1, FLAG_CONVERT_COLORS = 2
```

A save function returns `ERR_UNAVAILABLE` if its pointer is null (module disabled). There is no built-in
TIFF/PSD/XCF/ORA/GIF/JPEG-XL I/O — those go through the GEGL bridge (or a Tetra `ImageFormatLoader` subclass
registered with `ImageLoader::add_image_format_loader`, which automatically makes the format importable as a
texture). Writing a custom loader: derive `ImageFormatLoader`, implement
`virtual Error load_image(Ref<Image> p_image, Ref<FileAccess> p_fileaccess, BitField<LoaderFlags> p_flags = FLAG_NONE, float p_scale = 1.0)`
and `virtual void get_recognized_extensions(List<String> *p_extensions) const`.

`ResourceSaver::save(image_texture, "res://x.tres")` embeds pixel data in a Godot resource; for project assets
that the game should load, write a real `.png`/`.exr`/`.webp` into `res://` and let the import system produce
the texture (§8).

### 3.5 `ImageTexture` (`scene/resources/image_texture.h`) — what each call costs

```cpp
static Ref<ImageTexture> create_from_image(const Ref<Image> &p_image);
void set_image(const Ref<Image> &p_image);     // (re)allocates: texture_2d_create (+ texture_replace if one existed), notify_property_list_changed, emit_changed
void update(const Ref<Image> &p_image);        // same size + format + mipmap config required; RS::texture_2d_update; emit_changed
Ref<Image> get_image() const override;         // downloads from the GPU (RS::texture_2d_get) — slow, avoid in loops
void set_size_override(const Size2i &p_size);
virtual RID get_rid() const override;
```

- `update()` validates dimensions, format and mipmap flag (three `ERR_FAIL_COND_MSG`s, `image_texture.cpp:97-105`).
- RD backend: `TextureStorage::_texture_2d_update` → `_validate_texture_format` → `RD::texture_update(rd_texture, layer, data)`
  (`texture_storage.cpp:1616-1636`): **the whole image is uploaded every time**. Cost ≈ one memcpy into a staging
  buffer + GPU copy: fine for a 1080p RGBA8 video frame (8 MB) at 30-60 fps, wasteful for a 8k×8k canvas when a
  64 px dab changed.
- Both `set_image` and `update` emit `changed`; anything connected (inspectors, previews) reacts — do not attach
  editor previews to a texture that updates per frame.

### 3.6 Is there a partial (region) texture update? — No for CPU uploads; yes on the GPU

- `RenderingServer::texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer = 0)` is the only
  CPU→GPU update entry (`rendering_server.h:119`); no rect parameter, and the image must match the texture size.
- `RenderingDevice::texture_update(RID p_texture, uint32_t p_layer, const Vector<uint8_t> &p_data)`
  (`servers/rendering/rendering_device.h:467`) also takes a full layer.
- Region-capable GPU-side operations:
  `RenderingDevice::texture_copy(RID p_from_texture, RID p_to_texture, const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_size, uint32_t p_src_mipmap, uint32_t p_dst_mipmap, uint32_t p_src_layer, uint32_t p_dst_layer)`
  (`rendering_device.h:480`) and the new **drawable textures**.

Strategies for the painter, in order of preference:

1. **Tile the display**: split the canvas into fixed tiles (e.g. 256² or 512², matching GEGL's tile size keeps
   the bridge simple), one `ImageTexture` per tile, and `update()` only dirty tiles. Draw them with
   `draw_texture_rect` in `_draw` under a view transform. This needs nothing exotic and bounds upload cost by the
   dirty area.
2. **`DrawableTexture2D` + `texture_blit` shaders** (GPU compositing/stamping without a SubViewport):

   ```cpp
   // scene/resources/drawable_texture_2d.h
   enum DrawableFormat { DRAWABLE_FORMAT_RGBA8, DRAWABLE_FORMAT_RGBA8_SRGB, DRAWABLE_FORMAT_RGBAH, DRAWABLE_FORMAT_RGBAF };
   void setup(int p_width, int p_height, DrawableFormat p_format, const Color &p_modulate = Color(1, 1, 1, 1), bool p_use_mipmaps = false);  // 1..16384 per side; fills with the colour
   void blit_rect(const Rect2i p_rect, const Ref<Texture2D> &p_source, const Color &p_modulate = Color(1, 1, 1, 1), int p_mipmap = 0, const Ref<Material> &p_material = Ref<Material>());
   void blit_rect_multi(const Rect2i p_rect, const TypedArray<Texture2D> &p_sources, const TypedArray<DrawableTexture2D> &p_extra_targets, const Color &p_modulate = Color(1, 1, 1, 1), int p_mipmap = 0, const Ref<Material> &p_material = Ref<Material>());
   virtual Ref<Image> get_image() const override;   // GPU readback
   void generate_mipmaps();
   ```

   RS level: `texture_drawable_create(int w, int h, RSE::TextureDrawableFormat fmt, const Color &p_color = Color(1,1,1,1), bool p_with_mipmaps = false)`,
   `texture_drawable_blit_rect(const TypedArray<RID> &p_textures, const Rect2i &p_rect, RID p_material, const Color &p_modulate, const TypedArray<RID> &p_source_textures, int p_to_mipmap = 0)`,
   `texture_drawable_generate_mipmaps(RID)`, `texture_drawable_get_default_material()`
   (`rendering_server.h:115-142`).

   Semantics from the RD implementation (`texture_storage.cpp:1727-1863`): draws one quad covering `p_rect`
   (in destination pixels) into up to **4 render targets** (all the same size) using a `shader_type texture_blit`
   material with up to **4 source textures**; `UV` runs 0..1 **across the rect** (offset/size are passed as push
   constants), `MODULATE` is `p_modulate`; a source equal to a target is an error ("Cannot use self as a
   source"); `AtlasTexture` sources are unsupported. Shader interface (`servers/rendering/shader_types.cpp:527-549`):
   built-ins `TIME`, `PI`, `TAU`, `E`, `FRAGCOORD` (vec4, const), `UV` (vec2, const), `MODULATE` (vec4, const),
   outputs `COLOR0..COLOR3` (vec4); entry point `void blit()`; `render_mode blend_mix | blend_add | blend_sub |
   blend_mul | blend_disabled`. Default shader (`texture_storage.cpp:748-761`):

   ```glsl
   shader_type texture_blit;
   uniform sampler2D source_texture0 : hint_blit_source0;
   uniform sampler2D source_texture1 : hint_blit_source1;
   uniform sampler2D source_texture2 : hint_blit_source2;
   uniform sampler2D source_texture3 : hint_blit_source3;
   void blit() {
   	COLOR0 = texture(source_texture0, UV) * MODULATE;
   	COLOR1 = texture(source_texture1, UV) * MODULATE;
   	COLOR2 = texture(source_texture2, UV) * MODULATE;
   	COLOR3 = texture(source_texture3, UV) * MODULATE;
   }
   ```

   `BlitMaterial` (`scene/resources/blit_material.h`) is the stock material with
   `BLEND_MODE_MIX / ADD / SUB / MUL / DISABLED`. Uses: upload a small dirty-rect `ImageTexture` and blit it
   into the big canvas texture at `p_rect` with `blend_disabled` (= **a region update**), GPU layer compositing
   with custom blend-mode shaders (ping-pong between two drawables because self-sampling is forbidden), GPU brush
   stamping. This class is new and has **no in-tree users at all** (a grep finds only its definition, its
   registration in `scene/register_scene_types.cpp` and `doc/classes/DrawableTexture2D.xml`); behaviour on the
   Compatibility (GLES3) renderer exists (`drivers/gles3/shaders/tex_blit.glsl`) but was not exercised here —
   prototype before committing the architecture to it.
3. `Texture2DRD` (`scene/resources/texture_rd.h`): wraps a `RenderingDevice` texture RID as a `Texture2D`
   (`set_texture_rd_rid(RID)`, `get_texture_rd_rid()`), for content produced by compute shaders / custom RD
   passes. `RS::texture_rd_create(const RID &p_rd_texture, RSE::TextureLayeredType = TEXTURE_LAYERED_2D_ARRAY)` and
   `RS::texture_get_rd_texture(RID p_texture, bool p_srgb = false)` convert between the two worlds. RD calls must
   happen on the render thread (`RS::call_on_render_thread(const Callable &)`); Forward+/Mobile only.
4. `SubViewport` render-to-texture: `SubViewport` (`scene/main/viewport.h:900+`) with
   `set_size(Size2i)`, `set_update_mode(UPDATE_DISABLED / UPDATE_ONCE / UPDATE_WHEN_VISIBLE / UPDATE_WHEN_PARENT_VISIBLE / UPDATE_ALWAYS)`,
   `set_clear_mode(CLEAR_MODE_ALWAYS / CLEAR_MODE_NEVER / CLEAR_MODE_ONCE)`, `set_transparent_background(bool)`,
   `set_use_hdr_2d(bool)`, `set_disable_3d(bool)`, `Ref<ViewportTexture> get_texture() const`. With
   `CLEAR_MODE_NEVER` + `UPDATE_ONCE` it is the classic accumulate-strokes-on-GPU canvas; CanvasItem children with
   `CanvasItemMaterial`/`ShaderMaterial` do the blending. One frame of latency per update; readback via
   `get_texture()->get_image()`.

### 3.7 Colour management reminders

- 2D canvas rendering treats 8-bit textures as display-referred sRGB and blends in that space unless the
  viewport has `use_hdr_2d` enabled. A linear-light float canvas (GEGL default `"RGBA float"` is linear) must be
  converted for display: either in the core before upload to RGBA8, or by a canvas shader on the display Control.
- `DRAWABLE_FORMAT_RGBA8_SRGB` exists for textures "read from both 2D (non-hdr) and 3D"
  (`rendering_server_enums.h:75`).
- When painting textures that are simultaneously shown on a 3D model, give the material the *same* texture
  object (`ImageTexture` / `DrawableTexture2D`): 3D albedo samplers (`source_color`) use the sRGB view of the same GPU
  texture, so no extra copy is needed.

### 3.8 `CanvasItem` drawing API (`scene/main/canvas_item.h:333-369`) — all draw calls, exact signatures

```cpp
void draw_dashed_line(const Point2 &p_from, const Point2 &p_to, const Color &p_color, real_t p_width = -1.0, real_t p_dash = 2.0, bool p_aligned = true, bool p_antialiased = false);
void draw_line(const Point2 &p_from, const Point2 &p_to, const Color &p_color, real_t p_width = -1.0, bool p_antialiased = false);
void draw_polyline(const Vector<Point2> &p_points, const Color &p_color, real_t p_width = -1.0, bool p_antialiased = false);
void draw_polyline_colors(const Vector<Point2> &p_points, const Vector<Color> &p_colors, real_t p_width = -1.0, bool p_antialiased = false);
void draw_ellipse_arc(const Vector2 &p_center, real_t p_major, real_t p_minor, real_t p_start_angle, real_t p_end_angle, int p_point_count, const Color &p_color, real_t p_width = -1.0, bool p_antialiased = false);
void draw_arc(const Vector2 &p_center, real_t p_radius, real_t p_start_angle, real_t p_end_angle, int p_point_count, const Color &p_color, real_t p_width = -1.0, bool p_antialiased = false);
void draw_multiline(const Vector<Point2> &p_points, const Color &p_color, real_t p_width = -1.0, bool p_antialiased = false);
void draw_multiline_colors(const Vector<Point2> &p_points, const Vector<Color> &p_colors, real_t p_width = -1.0, bool p_antialiased = false);
void draw_rect(const Rect2 &p_rect, const Color &p_color, bool p_filled = true, real_t p_width = -1.0, bool p_antialiased = false);
void draw_ellipse(const Point2 &p_pos, real_t p_major, real_t p_minor, const Color &p_color, bool p_filled = true, real_t p_width = -1.0, bool p_antialiased = false);
void draw_circle(const Point2 &p_pos, real_t p_radius, const Color &p_color, bool p_filled = true, real_t p_width = -1.0, bool p_antialiased = false);
void draw_texture(RequiredParam<Texture2D> rp_texture, const Point2 &p_pos, const Color &p_modulate = Color(1, 1, 1, 1));
void draw_texture_rect(RequiredParam<Texture2D> rp_texture, const Rect2 &p_rect, bool p_tile = false, const Color &p_modulate = Color(1, 1, 1), bool p_transpose = false);
void draw_texture_rect_region(RequiredParam<Texture2D> rp_texture, const Rect2 &p_rect, const Rect2 &p_src_rect, const Color &p_modulate = Color(1, 1, 1), bool p_transpose = false, bool p_clip_uv = false);
void draw_msdf_texture_rect_region(...);  void draw_lcd_texture_rect_region(...);
void draw_style_box(RequiredParam<StyleBox> rp_style_box, const Rect2 &p_rect);
void draw_primitive(const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs, Ref<Texture2D> p_texture = Ref<Texture2D>());   // 1-4 points
void draw_polygon(const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs = Vector<Point2>(), Ref<Texture2D> p_texture = Ref<Texture2D>());
void draw_colored_polygon(const Vector<Point2> &p_points, const Color &p_color, const Vector<Point2> &p_uvs = Vector<Point2>(), Ref<Texture2D> p_texture = Ref<Texture2D>());
void draw_mesh(RequiredParam<Mesh> rp_mesh, const Ref<Texture2D> &p_texture, const Transform2D &p_transform = Transform2D(), const Color &p_modulate = Color(1, 1, 1));
void draw_multimesh(RequiredParam<MultiMesh> rp_multimesh, const Ref<Texture2D> &p_texture);
void draw_string(RequiredParam<Font> rp_font, const Point2 &p_pos, const String &p_text, HorizontalAlignment p_alignment = HORIZONTAL_ALIGNMENT_LEFT, float p_width = -1, int p_font_size = DEFAULT_FONT_SIZE, const Color &p_modulate = Color(1.0, 1.0, 1.0), …) const;
void draw_multiline_string(...) const;  void draw_string_outline(...) const;  void draw_multiline_string_outline(...) const;
void draw_char(RequiredParam<Font> rp_font, const Point2 &p_pos, const String &p_char, int p_font_size = DEFAULT_FONT_SIZE, const Color &p_modulate = Color(1.0, 1.0, 1.0), float p_oversampling = 0.0) const;
void draw_char_outline(...) const;
void draw_set_transform(const Point2 &p_offset, real_t p_rot = 0.0, const Size2 &p_scale = Size2(1.0, 1.0));
void draw_set_transform_matrix(const Transform2D &p_matrix);
void draw_animation_slice(double p_animation_length, double p_slice_begin, double p_slice_end, double p_offset = 0);
void draw_end_animation();
void queue_redraw();
```

- Draw calls are only legal during `NOTIFICATION_DRAW` (= 30) / the `draw` signal. `RequiredParam<T>` accepts a
  `Ref<T>`; passing null is a hard error, check first.
- `draw_set_transform*` affects all *subsequent* commands of that item in the same draw pass — the standard way
  to implement pan/zoom of a canvas: `TextureRegionEditor::_get_offset_transform()` builds
  `Transform2D mtx; mtx.columns[2] = -draw_ofs * draw_zoom; mtx.scale_basis(Vector2(draw_zoom, draw_zoom));`
  then `draw_set_transform_matrix(mtx); draw_texture(texture, Point2());` and resets with `Transform2D()` before
  drawing zoom-independent overlays on a second Control (`editor/scene/texture/texture_region_editor_plugin.cpp:56-75`).
- Line `p_width = -1` means a 1-pixel primitive line regardless of zoom (good for selection outlines).
  "Marching ants" need a shader or animated `draw_dashed_line` offsets — nothing built in.
- Lower level for many items: `RS::canvas_item_add_*` on `get_canvas_item()` (`rendering_server.h:821-840`),
  e.g. `canvas_item_add_triangle_array(RID p_item, const Vector<int> &p_indices, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs = …, const Vector<int> &p_bones = …, const Vector<float> &p_weights = …, RID p_texture = RID(), int p_count = -1)`.

### 3.9 Texture filtering, materials on Controls, clipping

```cpp
virtual void set_texture_filter(TextureFilter p_texture_filter);   // CanvasItem; inherited by children that use TEXTURE_FILTER_PARENT_NODE
virtual void set_texture_repeat(TextureRepeat p_texture_repeat);
virtual void set_material(const Ref<Material> &p_material);        // CanvasItemMaterial or ShaderMaterial (shader_type canvas_item)
virtual void set_use_parent_material(bool p_use_parent_material);
void set_instance_shader_parameter(const StringName &p_name, const Variant &p_value);   // per-item 'instance uniform'
void set_clip_children_mode(ClipChildrenMode p_clip_mode);          // CLIP_CHILDREN_DISABLED / _ONLY / _AND_DRAW
void set_draw_behind_parent(bool p_enable);  void set_modulate(const Color &);  void set_self_modulate(const Color &);
void Control::set_clip_contents(bool p_clip);
```

`CanvasItem::TextureFilter`: `TEXTURE_FILTER_PARENT_NODE, TEXTURE_FILTER_NEAREST, TEXTURE_FILTER_LINEAR,
TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC`.
`TextureRepeat`: `TEXTURE_REPEAT_PARENT_NODE, _DISABLED, _ENABLED, _MIRROR`.
Painter rule of thumb: `TEXTURE_FILTER_NEAREST` when zoom ≥ 100 % (pixel grid visible), linear-with-mipmaps
when zoomed out (requires mipmapped textures, i.e. regenerate mips for dirty tiles) or a custom downsampling
shader.

GPU compositing on a Control: put a `ShaderMaterial` on the canvas Control and bind layer textures as uniforms
(`material->set_shader_parameter("layer0", tex)`); in `_draw` issue one `draw_texture_rect(base_tex, rect)` —
the fragment shader composites. The editor's own texture inspector does precisely this
(`editor/scene/texture/texture_editor_plugin.cpp:52-80`):

```glsl
shader_type canvas_item;
render_mode blend_mix;
instance uniform vec4 u_channel_factors = vec4(1.0);
instance uniform float lod = 0.0;
void fragment() {
	COLOR = filter_preview_colors(textureLod(TEXTURE, UV, lod), u_channel_factors);
}
```

with `texture_display->set_material(texture_material); texture_display->set_instance_shader_parameter("u_channel_factors", Vector4(1, 1, 1, 1));`
(one shared material, per-Control parameters through *instance uniforms*).

### 3.10 Checkerboard and how the existing editors display images

- There is no C++ "draw checkerboard" function. The editor uses **theme icons** tiled:
  `get_editor_theme_icon(SNAME("Checkerboard"))` (large, translucent) and `SNAME("GuiMiniCheckerboard")`.
  Two idioms:
  - immediate: `draw_texture_rect(get_editor_theme_icon(SNAME("Checkerboard")), rect, true /* tile */);`
    (`editor/scene/texture/texture_3d_editor_plugin.cpp:86-87`, `tile_atlas_view.cpp:452`). The material
    preview first draws opaque black because "checkerboard texture is transluscent"
    (`editor/scene/material_editor_plugin.cpp:143-144`).
  - retained: a `TextureRect` with `set_stretch_mode(TextureRect::STRETCH_TILE)`,
    `set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE)`, `set_texture_repeat(CanvasItem::TEXTURE_REPEAT_ENABLED)`
    placed under the image (`texture_editor_plugin.cpp:264-268`).
  Tiling with `p_tile = true` relies on texture repeat; when the canvas is zoomed the checker should stay
  screen-constant, so draw it *before* applying the view transform.
- `TexturePreview` (`editor/scene/texture/texture_editor_plugin.{h,cpp}`): `MarginContainer` ▸ `ColorRect` bg ▸
  `AspectRatioContainer` ▸ { checkerboard `TextureRect`, `TextureRect *texture_display` with
  `set_texture_filter(TEXTURE_FILTER_NEAREST_WITH_MIPMAPS)` + the channel-filter `ShaderMaterial`, an outline
  overlay `Control` drawn via the `draw` signal so the outline "is not affected by the filtering shader" }.
  Metadata label uses `Image::get_format_name(format)` and
  `memory = w * h * Image::get_format_pixel_size(format) >> Image::get_format_pixel_rshift(format)`.
- `TextureRegionEditor` (`texture_region_editor_plugin.cpp`): two stacked Controls — `texture_preview` draws the
  texture under `draw_set_transform_matrix(zoom/pan)`, `texture_overlay` draws grid lines, region rectangle and
  handles in screen space (`draw_line(..., Color(0.3, 0.7, 1, 1), 2)`, handle icons with `draw_texture`),
  scrollbars are driven from `draw_ofs`; zoom-at-cursor is `_zoom_on_position(draw_zoom * factor, position)`.
  This two-layer split (zoomed content / unzoomed chrome) is the structure to copy for the Tetra canvas view.
- Thumbnails for `Image` resources: `EditorImagePreviewPlugin::generate`
  (`editor/inspector/editor_preview_plugins.cpp:193-222`) duplicates, decompresses or converts to RGBA8,
  resizes with `INTERPOLATE_CUBIC` and returns `ImageTexture::create_from_image(img)`.

---

## 4. Audio / video (Kdenlive-style NLE monitor)

### 4.1 `AudioStreamGenerator` + `AudioStreamGeneratorPlayback` (`servers/audio/effects/audio_stream_generator.{h,cpp}`)

```cpp
class AudioStreamGenerator : public AudioStream {
	enum AudioStreamGeneratorMixRate { MIX_RATE_OUTPUT, MIX_RATE_INPUT, MIX_RATE_CUSTOM, MIX_RATE_MAX };
	// defaults: mix_rate_mode = MIX_RATE_CUSTOM, mix_rate = 44100, buffer_len = 0.5 (seconds)
	void set_mix_rate(float p_mix_rate);  float get_mix_rate() const;                 // property range 20..192000
	void set_mix_rate_mode(AudioStreamGeneratorMixRate p_mix_rate_mode);
	void set_buffer_length(float p_seconds);  float get_buffer_length() const;        // property range 0.01..10
	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
};
class AudioStreamGeneratorPlayback : public AudioStreamPlaybackResampled {
	bool push_frame(const Vector2 &p_frame);                // x = left, y = right; false if the ring is full
	bool can_push_buffer(int p_frames) const;               // buffer.space_left() >= p_frames
	bool push_buffer(const PackedVector2Array &p_frames);   // all-or-nothing: false (nothing written) if it does not fit
	int get_frames_available() const;                       // FREE space in frames (space_left), not queued frames
	int get_skips() const;                                  // number of mix callbacks that underran (zero-filled)
	void clear_buffer();                                    // ERR_FAIL_COND(active): only while stopped
	virtual void start(double p_from_pos = 0.0) override;  virtual void stop() override;  virtual bool is_playing() const override;
};
```

Facts from the implementation:

- The ring buffer is allocated in `instantiate_playback()` with
  `buffer.resize(Math::nearest_shift(uint32_t(_get_target_rate() * buffer_len)))`, i.e. a **power-of-two** number
  of frames ≥ rate × length — set `mix_rate`/`buffer_length` *before* the player instantiates the playback
  (before `AudioStreamPlayer::play()`).
- `_get_target_rate()`: `MIX_RATE_OUTPUT` → `AudioServer::get_mix_rate()`, `MIX_RATE_INPUT` →
  `get_input_mix_rate()`, else `mix_rate`. With `MIX_RATE_OUTPUT` no resampling happens — feed MLT audio at the
  server rate (configure the MLT consumer/profile frequency to `AudioServer::get_singleton()->get_mix_rate()`).
- `_mix_internal` runs on the **audio thread**: reads what is available, zero-fills the rest and increments
  `skips` on underrun. `push_*` run on the caller's thread; the `RingBuffer<AudioFrame>` is used single-producer /
  single-consumer without a lock, so push from **one** thread only.
- With `sizeof(real_t) == 4` (this build: `precision` not set to double) `push_buffer` writes the
  `PackedVector2Array` memory straight into the ring (`AudioFrame` is `{ float left; float right; }`,
  `core/math/audio_frame.h`).
- Typical use: `Ref<AudioStreamGenerator> gen; gen.instantiate(); gen->set_mix_rate_mode(AudioStreamGenerator::MIX_RATE_OUTPUT);
  gen->set_buffer_length(0.2); player->set_stream(gen); player->play();
  Ref<AudioStreamGeneratorPlayback> pb = player->get_stream_playback();` then each frame
  `while (pb->can_push_buffer(n)) pb->push_buffer(chunk);`.
- `AudioStreamPlayer` (`scene/audio/audio_stream_player.h`): `set_stream(Ref<AudioStream>)`, `play(float p_from_pos = 0.0)`,
  `seek(float)`, `stop()`, `set_bus(const StringName&)`, `set_volume_db(float)`, `set_stream_paused(bool)`,
  `bool has_stream_playback()`, `Ref<AudioStreamPlayback> get_stream_playback()`. It works inside editor UI
  (it is how the editor previews audio).

Custom streams: subclass `AudioStream` (`instantiate_playback`, `get_stream_name`, `get_length`,
`is_monophonic`) and `AudioStreamPlaybackResampled` (override `virtual int _mix_internal(AudioFrame *p_buffer, int p_frames)`
and `virtual float get_stream_sampling_rate()`, call `begin_resample()` in `start`) — headers in
`servers/audio/audio_stream.h`. That is the route for a pull-model MLT audio source (the audio thread asks for
frames) instead of the push-model generator.

### 4.2 `AudioServer` (`servers/audio/audio_server.h`)

| Purpose | API |
|---|---|
| Output rate | `virtual float get_mix_rate() const` (`:468`); project setting `audio/driver/mix_rate` (default `DEFAULT_MIX_RATE = 44100`); `virtual float get_input_mix_rate() const` |
| Latency / clock | `virtual double get_output_latency() const`; `virtual double get_time_to_next_mix() const`; `virtual double get_time_since_last_mix() const`; `uint64_t get_mixed_frames() const`. Audio clock for A/V sync: `playback_pos + get_time_since_last_mix() - get_output_latency()`. Project setting `audio/video/video_delay_compensation_ms` exists for video. |
| Channels | `virtual SpeakerMode get_speaker_mode() const`; `int get_channel_count() const` (stereo pairs: 1..4) |
| Buses | `set_bus_count(int)`, `get_bus_count()`, `add_bus(int p_at_pos = -1)`, `remove_bus(int)`, `move_bus(int, int)`, `set_bus_name(int, const String&)`, `get_bus_name(int)`, `int get_bus_index(const StringName&) const`, `get_bus_channels(int)`, `set_bus_volume_db(int, float)` / `get_bus_volume_db`, `set_bus_volume_linear` / `get_bus_volume_linear`, `set_bus_send(int, const StringName&)`, `set_bus_solo`, `set_bus_mute`, `set_bus_bypass_effects`, `float get_bus_peak_volume_left_db(int p_bus, int p_channel) const`, `get_bus_peak_volume_right_db` (VU meters), `set_bus_layout(const Ref<AudioBusLayout>&)` |
| Effects | `add_bus_effect(int p_bus, const Ref<AudioEffect>&, int p_at_pos = -1)`, `remove_bus_effect(int, int)`, `get_bus_effect_count(int)`, `Ref<AudioEffect> get_bus_effect(int, int)`, `Ref<AudioEffectInstance> get_bus_effect_instance(int p_bus, int p_effect, int p_channel = 0)`, `set_bus_effect_enabled` |
| Direct playback (no node) | `start_playback_stream(Ref<AudioStreamPlayback> p_playback, const StringName &p_bus, Vector<AudioFrame> p_volume_db_vector, float p_start_time = 0, float p_pitch_scale = 1)`, `stop_playback_stream(...)`, `set_playback_paused`, `set_playback_pitch_scale`, `bool is_playback_active(...)`, `float get_playback_position(...)` |
| Mix-thread hooks | `typedef void (*AudioCallback)(void *p_userdata)`; `add_mix_callback(AudioCallback, void *)` / `remove_mix_callback` (called on the audio thread each mix), `add_update_callback` / `remove_update_callback` (main thread); inside a mix callback: `AudioFrame *thread_get_channel_mix_buffer(int p_bus, int p_buffer)`, `int thread_get_mix_buffer_size() const`, `int thread_find_bus_index(const StringName &p_name)` — this is how `VideoStreamPlayer` injects audio (`scene/gui/video_stream_player.cpp:87-130`) |
| Locking | `virtual void lock()` / `virtual void unlock()` — take it when mutating state the audio thread reads (VideoStreamPlayer wraps `resampler.setup/clear` in it) |
| Devices | `get_output_device_list()`, `get_output_device()`, `set_output_device(const String&)`; input: `get_input_device_list()`, `set_input_device`, `Error set_input_device_active(bool)`, `int get_input_frames_available()`, `PackedVector2Array get_input_frames(int p_frames)` (needs project setting `audio/driver/enable_input`) |
| Editor preview speed | `set_playback_speed_scale(float)` |

`AudioRBResampler` (`servers/audio/audio_rb_resampler.h`) — reusable ring-buffer resampler for
interleaved float input of 1/2/4/6 channels:
`Error setup(int p_channels, int p_src_mix_rate, int p_target_mix_rate, int p_buffer_msec, int p_minbuff_needed = -1)`,
`int get_writer_space() const`, `float *get_write_buffer()`, `void write(uint32_t p_frames)`,
`bool mix(AudioFrame *p_dest, int p_frames)`, `int get_num_of_ready_frames()`, `void set_playback_speed(double)`,
`void flush()`, `void clear()`, `int get_channel_count() const`, `bool has_data() const`.

### 4.3 `AudioStreamWAV`, capture and record

`AudioStreamWAV` (`scene/resources/audio_stream_wav.h`), `RES_BASE_EXTENSION("sample")`:
`enum Format { FORMAT_8_BITS, FORMAT_16_BITS, FORMAT_IMA_ADPCM, FORMAT_QOA }`,
`enum LoopMode { LOOP_DISABLED, LOOP_FORWARD, LOOP_PINGPONG, LOOP_BACKWARD }`;
`set_format`, `set_mix_rate(int p_hz)`, `set_stereo(bool)`, `set_loop_mode/begin/end`,
`void set_data(const Vector<uint8_t> &p_data)` (PCM little-endian, interleaved L/R), `Vector<uint8_t> get_data() const`,
`Error save_to_wav(const String &p_path)`,
`static Ref<AudioStreamWAV> load_from_file(const String &p_path, const Dictionary &p_options)`,
`static Ref<AudioStreamWAV> load_from_buffer(const Vector<uint8_t> &p_stream_data, const Dictionary &p_options)`.
Use for clip-audio caches / exporting a rendered mix; there is no float32 format (16-bit max).

`AudioEffectCapture` (`servers/audio/effects/audio_effect_capture.h`) — tap a bus:
`set_buffer_length(float seconds)` (default 0.1), `bool can_get_buffer(int p_frames) const`,
`PackedVector2Array get_buffer(int p_len)`, `clear_buffer()`, `int get_frames_available() const`,
`int64_t get_discarded_frames() const`, `int get_buffer_length_frames() const`, `int64_t get_pushed_frames() const`.
`AudioEffectRecord` (`audio_effect_record.h`): `set_recording_active(bool)`, `is_recording_active()`,
`set_format(AudioStreamWAV::Format)`, `Ref<AudioStreamWAV> get_recording() const`. Both are added with
`AudioServer::add_bus_effect`. Useful for scopes/meters on the monitor bus and voice-over recording.

### 4.4 `VideoStream` / `VideoStreamPlayback` (`scene/resources/video_stream.h`) and the Theora reference

```cpp
class VideoStreamPlayback : public Resource {
public:
	typedef int (*AudioMixCallback)(void *p_udata, const float *p_data, int p_frames);   // interleaved floats, returns frames consumed
protected:
	AudioMixCallback mix_callback = nullptr;  void *mix_udata = nullptr;
public:
	virtual void stop();  virtual void play();  virtual bool is_playing() const;
	virtual void set_paused(bool p_paused);  virtual bool is_paused() const;
	virtual double get_length() const;  virtual double get_playback_position() const;  virtual void seek(double p_time);
	virtual void set_audio_track(int p_idx);
	virtual Ref<Texture2D> get_texture() const;
	virtual void update(double p_delta);                    // GDVIRTUAL1_REQUIRED(_update, double)
	virtual void set_mix_callback(AudioMixCallback p_callback, void *p_userdata);
	virtual int get_channels() const;  virtual int get_mix_rate() const;
};
class VideoStream : public Resource {      // OBJ_SAVE_TYPE(VideoStream)
protected: String file; int audio_track = 0;
public:
	void set_file(const String &p_file);  String get_file();
	virtual void set_audio_track(int p_track);
	virtual Ref<VideoStreamPlayback> instantiate_playback();   // GDVIRTUAL0R_REQUIRED(_instantiate_playback)
};
```

How `VideoStreamPlayer` (`scene/gui/video_stream_player.{h,cpp}`) drives a playback:

1. `set_stream`: under `AudioServer::lock()` `playback = stream->instantiate_playback()`; then
   `texture = playback->get_texture()` (connects to its `changed` signal to track size and redraw);
   `resampler.setup(playback->get_channels(), playback->get_mix_rate(), AudioServer::get_mix_rate(), buffering_ms /*500*/, 0)`;
   `playback->set_mix_callback(_audio_mix_callback, this)` if `channels > 0` (`:280-316`).
2. Every frame in `NOTIFICATION_INTERNAL_PROCESS`: `playback->update(delta * speed_scale)` on the **main thread**;
   when `!playback->is_playing()` afterwards → loop or emit `finished` (`:154-176`).
3. The playback decodes inside `update()` and pushes audio through `mix_callback(mix_udata, interleaved_floats, frames)`;
   the callback copies into the player's `AudioRBResampler` and returns how many frames fit (`:62-79`) — the
   playback must keep the remainder and retry (Theora's `send_audio()`).
4. On the audio thread the registered mix callback resamples and adds into the bus buffers.
5. Drawing: `draw_texture_rect(texture, Rect2(Point2(), expand ? get_size() : texture_size), false)`.

Theora implementation (`modules/theora/video_stream_theora.{h,cpp}`), the template for an MLT/FFmpeg-backed
stream usable by games:

- `VideoStreamTheora::instantiate_playback()` creates `VideoStreamPlaybackTheora`, sets audio track and file.
- Frame output (`video_write`, `:235-256`): YUV→RGBA8 into a persistent `Vector<uint8_t> frame_data`, then
  ```cpp
  Ref<Image> img;
  img.instantiate(region.size.x, region.size.y, false, Image::FORMAT_RGBA8, frame_data); // zero copy image creation
  texture->update(img); // Zero-copy send to rendering server.
  ```
  The texture is created once at open time with
  `texture->set_image(Image::create_empty(w, h, false, Image::FORMAT_RGBA8))` (`:461-463`).
- Timing (`update`, `:508-623`): `time += p_delta`; decode until a frame with `next_frame_time > time` is ready and
  the audio buffer is full; upload only when `time >= current_frame_time`. Audio/video sync is driven by the
  accumulated process delta, *not* by the audio clock (the latency compensation is commented out with a FIXME at
  `:497-501`).
- Loader: `ResourceFormatLoaderTheora` (extension `ogv`, `handles_type` = any `VideoStream` parent, resource type
  `"VideoStreamTheora"`); registered at `MODULE_INITIALIZATION_LEVEL_SCENE` with
  `ResourceLoader::add_resource_format_loader(loader, true)` + `GDREGISTER_CLASS(VideoStreamTheora)`
  (`modules/theora/register_types.cpp`).

For Tetra: a `VideoStreamTetra`/`VideoStreamPlaybackTetra` pair in `tetra_video` following this shape gives
runtime playback of MLT-readable media (if the exported game links MLT — otherwise editor-only); the editor
monitor itself does not need the `VideoStream` abstraction and can drive `ImageTexture::update` directly.

### 4.5 Presenting externally decoded RGBA frames

- Allocate once: `Ref<ImageTexture> tex = ImageTexture::create_from_image(Image::create_empty(w, h, false, Image::FORMAT_RGBA8));`
  — **RGBA8, no mipmaps**; RGB24 from MLT (`mlt_image_rgb`) would be expanded to RGBA on every upload inside the
  renderer, so request `mlt_image_rgba` from MLT.
- Per frame (main thread): copy the frame into a reusable `Vector<uint8_t>` (`memcpy` to `ptrw()`), wrap with
  `Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, buf)` (shares the buffer) and call `tex->update(img)`.
  Size or format change ⇒ `set_image()` instead (reallocates).
- A `TextureRect` showing `tex` redraws itself on the texture's `changed` signal; a custom Control should
  connect `tex->connect_changed(callable_mp((CanvasItem *)this, &CanvasItem::queue_redraw))`.
- Decoding belongs on a worker thread (MLT consumer thread / `WorkerThreadPool`); hand finished frames to the main
  thread through a mutex-protected slot or `callable_mp(this, &X::_frame_ready).call_deferred(...)` and upload at
  most one frame per process tick (drop late frames). `RS::texture_2d_update` itself may be called from any
  thread (it is queued, §5.7), but `ImageTexture::update` also emits `changed`, which must stay on the main thread.
- Float/HDR preview: `Image::FORMAT_RGBAH` (8 bytes/px) uploads without conversion.
- YUV→RGB on the GPU is possible with three `FORMAT_R8` textures and a `canvas_item` shader on the monitor
  Control (not done in-tree; Theora converts on the CPU).

### 4.6 `MovieWriter` (`servers/movie_writer/movie_writer.h`)

```cpp
class MovieWriter : public Object {
protected:
	virtual uint32_t get_audio_mix_rate() const;
	virtual AudioServer::SpeakerMode get_audio_speaker_mode() const;
	virtual Error write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path);
	virtual Error write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data);
	virtual void write_end();
public:
	virtual bool handles_file(const String &p_path) const;
	virtual void get_supported_extensions(List<String> *r_extensions) const;
	static void add_writer(MovieWriter *p_writer);              // MAX_WRITERS = 8
	static MovieWriter *find_writer_for_file(const String &p_file);
	void begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path);
	void add_frame();
	void end();
};
```

- Activated by `--write-movie <file>` (`main/main.cpp:1961`); the writer is chosen with
  `MovieWriter::find_writer_for_file(Engine::get_singleton()->get_write_movie_path())` (`:3800`); `begin` at
  `:4855`, `add_frame()` once per rendered frame (`:5151`), `end()` at exit. The engine runs with a fixed FPS and
  the dummy audio driver (non-real-time).
- `add_frame()` (`movie_writer.cpp:186-250`): reads the **main window** viewport texture with
  `RS::viewport_get_texture` + `RS::texture_2d_get` (a GPU readback), crops/resizes to the movie size, converts to
  RGBA8 (HDR: `linear_to_srgb`), mixes `mix_rate / fps` audio frames via `AudioDriverDummy::mix_audio` into an
  `int32_t` interleaved buffer (`audio_channels` × frames) and calls `write_frame(image, audio)`.
- Built-in writers: `MovieWriterPNGWAV` (`servers/movie_writer/movie_writer_pngwav.h`, PNG sequence + WAV),
  AVI/MJPEG (`modules/jpg`), and in the editor build `MovieWriterOGV` (`modules/theora/editor/movie_writer_ogv.h`,
  registered at `MODULE_INITIALIZATION_LEVEL_SERVERS`). Project settings: `editor/movie_writer/mix_rate` (48000),
  `speaker_mode`, `video_quality` (0.75), `audio_bit_depth`, `ogv/audio_quality`, `ogv/encoding_speed`,
  `ogv/keyframe_interval`, `movie_file`, `disable_vsync`, `fps` (60).
- Relevance: a Tetra `MovieWriter` subclass that pipes frames into an MLT consumer would give "render the *game*
  to any MLT/FFmpeg format". It is **not** an API for exporting the NLE timeline (that is MLT's own render path).

### 4.7 Timing

- `OS::get_singleton()->get_ticks_usec()` (`uint64_t`, monotonic, `core/os/os.h:281`), `get_ticks_msec()`,
  `delay_usec(uint32_t)`, `get_unix_time()`.
- Per-frame: `Node::set_process(true)` + `NOTIFICATION_PROCESS` (17) with `get_process_delta_time()`;
  `set_process_internal(true)` + `NOTIFICATION_INTERNAL_PROCESS` (25) for engine-style nodes;
  `NOTIFICATION_PHYSICS_PROCESS` (16) for fixed-step. The editor redraws only when something changed
  ("update continuously" off) but `_process` still runs every iteration, with low-processor-mode sleeps — a
  playing monitor should keep requesting redraws (texture `changed` does).
- `Timer` (`scene/main/timer.h`): `set_wait_time(double)`, `set_one_shot(bool)`, `start(double p_time = -1)`,
  `stop()`, `is_stopped()`, `get_time_left()`, `set_timer_process_callback(TIMER_PROCESS_PHYSICS | TIMER_PROCESS_IDLE)`,
  `set_ignore_time_scale(bool)`; signal `"timeout"` (used by the Blender runner's kill timer).
  `SceneTree::create_timer(double p_delay_sec, bool p_process_always = true, bool p_process_in_physics = false, bool p_ignore_time_scale = false)`.
- Timers and process callbacks are quantised to frames; frame-accurate A/V pacing must use `get_ticks_usec()` or the
  audio clock (§4.2) inside `_process`.

---

## 5. Processes, threads, IPC

### 5.1 Process control (`core/os/os.h:214-223`; Unix implementation `drivers/unix/os_unix.cpp:715-1029`)

```cpp
virtual String get_executable_path() const;
virtual Error execute(const String &p_path, const List<String> &p_arguments, String *r_pipe = nullptr, int *r_exitcode = nullptr,
		bool read_stderr = false, Mutex *p_pipe_mutex = nullptr, bool p_open_console = false) = 0;      // BLOCKS until exit
virtual Dictionary execute_with_pipe(const String &p_path, const List<String> &p_arguments, bool p_blocking = true);
virtual Error create_process(const String &p_path, const List<String> &p_arguments, ProcessID *r_child_id = nullptr, bool p_open_console = false) = 0;
virtual Error create_instance(const List<String> &p_arguments, ProcessID *r_child_id = nullptr);         // another copy of this binary
virtual Error kill(const ProcessID &p_pid) = 0;
virtual int get_process_id() const;
virtual bool is_process_running(const ProcessID &p_pid) const = 0;
virtual int get_process_exit_code(const ProcessID &p_pid) const = 0;                                    // -1 while running
virtual bool has_environment(const String &) const = 0;  virtual String get_environment(const String &) const = 0;
virtual void set_environment(const String &p_var, const String &p_value) const = 0;  virtual void unset_environment(const String &) const = 0;
// typedef int64_t ProcessID;   (core/os/process_id.h)
```

Unix behaviour to rely on / watch out for:

| Call | Behaviour |
|---|---|
| `execute(..., r_pipe != nullptr)` | Builds **a shell command string**: `"path" "arg1" "arg2" …` + ` 2>&1` (if `read_stderr`) or ` 2>/dev/null`, runs it with `popen(…, "r")`, appends output line by line (64 KiB `fgets` buffer, UTF-8 with Latin-1 fallback) to `*r_pipe` — locking `p_pipe_mutex` around each append if given, so another thread can read partial output — then `pclose`; exit code = `WEXITSTATUS`. **Arguments are only wrapped in double quotes, not escaped**: `"`, `$`, `` ` `` and `\` inside an argument are interpreted by `/bin/sh`. Godot's own Blender runner passes a whole Python script this way after `c_escape()`-ing string values only. For untrusted/arbitrary strings prefer the non-pipe form or `execute_with_pipe`. |
| `execute(..., r_pipe == nullptr)` | `fork` + `execvp(path, argv)` (no shell, PATH lookup), waits; `*r_exitcode` = exit status. Child inherits stdio. |
| `create_process` | `fork` + `setsid()` + `execvp`; returns immediately; pid recorded in `process_map` so `is_process_running` / `get_process_exit_code` work (`waitpid(WNOHANG)`; result cached once exited). Child inherits the editor's stdout/stderr. |
| `execute_with_pipe(path, args, p_blocking)` | Creates 3 pipes, `fork` + `setsid` + `dup2` + `execvp`. Returns `Dictionary{ "stdio": Ref<FileAccess> (read = child's stdout, write = child's stdin), "stderr": Ref<FileAccess> (read-only), "pid": int }`; empty dictionary on failure. With `p_blocking == false` both fds get `O_NONBLOCK`. |
| `kill(pid)` | `::kill(pid, SIGKILL)` then reaps; no graceful SIGTERM variant. |

Pipe `FileAccess` semantics (`drivers/unix/file_access_unix_pipe.cpp`):

- `uint64_t get_length() const` → `ioctl(fd, FIONREAD)` = **bytes available to read right now**. This is the
  non-blocking poll primitive.
- `uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const` → one `::read`; returns bytes read (0 on error /
  nothing available in non-blocking mode); `get_error()` is `ERR_FILE_CANT_READ` for short reads. In blocking mode
  it blocks until *some* data arrives.
- `bool store_buffer(const uint8_t *p_src, uint64_t p_length)` → one `::write` with SIGPIPE ignored; returns false
  on a short write (non-blocking + full pipe ⇒ partial write reported as failure: write requests in blocking
  mode or keep them smaller than the pipe buffer).
- `eof_reached()` always returns false; `flush()` is a no-op; `seek` no-op. Detect child exit with
  `OS::is_process_running(pid)`; drain remaining bytes afterwards.
- `get_line()`/`get_as_text()` on a pipe are byte-at-a-time and block/spin — read raw chunks and split lines yourself.

Non-blocking read loop (safe to run from `_process`):

```cpp
Dictionary info = OS::get_singleton()->execute_with_pipe(blender_path, args, false);
ERR_FAIL_COND_V(info.is_empty(), FAILED);
Ref<FileAccess> io = info["stdio"];  Ref<FileAccess> err = info["stderr"];  ProcessID pid = info["pid"];
// each frame:
uint64_t n = io->get_length();
if (n > 0) { PackedByteArray chunk; chunk.resize(n); uint64_t got = io->get_buffer(chunk.ptrw(), n); chunk.resize(got); /* append + parse */ }
// write a request:  io->store_buffer(bytes.ptr(), bytes.size());
// teardown:         if (OS::get_singleton()->is_process_running(pid)) { OS::get_singleton()->kill(pid); }  io.unref(); err.unref();
```

Always drain `stderr` as well, or the child blocks once the 64 KiB pipe buffer fills.

Named pipes also exist: `FileAccess::open("pipe://name", FileAccess::READ_WRITE)` on Unix maps to a FIFO
`/tmp/<name>` created with `mkfifo(…, 0600)`, opened `O_RDWR | O_NONBLOCK`, unlinked on close when it created it
(`file_access_unix_pipe.cpp:open_internal`).

### 5.2 Paths, temp files, directories

| API | Value on Linux in this build |
|---|---|
| `OS::get_user_data_dir()` | `user://` root for the *project* (per project name / custom user dir) |
| `OS::get_data_path()`, `get_config_path()`, `get_cache_path()`, `get_temp_path()` | XDG dirs (`get_cache_path`: `$XDG_CACHE_HOME` if absolute, else `$HOME/.cache`, else config path — `platform/linuxbsd/os_linuxbsd.cpp:926-937`) |
| `OS::get_godot_dir_name()` | `String(GODOT_VERSION_SHORT_NAME).to_lower()` = `"godot"` (`core/os/os.cpp:294-297`; `version.py` keeps `short_name = "godot"` even though `name = "Tetra Studio"`) — so Tetra shares `~/.local/share/godot`, `~/.config/godot`, `~/.cache/godot` with stock Godot unless the branding patch changes `short_name` |
| `EditorPaths::get_singleton()` (`editor/file_system/editor_paths.h`) | `get_data_dir()` = data/`godot`; `get_config_dir()`; `get_cache_dir()` = cache/`godot` ("thumbnails, tmp generated files"); `get_temp_dir()` = `OS::get_temp_path()`; `get_project_data_dir()` = `res://.godot`; `get_project_settings_dir()`; `is_self_contained()` (a `._sc_`/`_sc_` file next to the binary redirects everything to `editor_data/`) |
| `ProjectSettings::get_singleton()` | `String globalize_path(const String &) const` (`res://`/`user://` → absolute OS path), `localize_path`, `get_resource_path()`, `get_project_data_path()`, `get_imported_files_path()` (= `res://.godot/imported`) |

```cpp
// core/io/file_access.h
enum ModeFlags : int32_t { READ = 1, WRITE = 2, READ_WRITE = 3, WRITE_READ = 7, SKIP_PACK = 16 };
static Ref<FileAccess> open(const String &p_path, int p_mode_flags, Error *r_error = nullptr);
static Ref<FileAccess> create_temp(ModeFlags p_mode_flags, const String &p_prefix = "", const String &p_extension = "", bool p_keep = false, Error *r_error = nullptr);
static bool exists(const String &p_name);
static Vector<uint8_t> get_file_as_bytes(const String &p_path, Error *r_error = nullptr);
static String get_file_as_string(const String &p_path, Error *r_error = nullptr);
static uint64_t get_modified_time(const String &p_file);
static String get_md5(const String &p_file);  static String get_sha256(const String &p_file);
static Error set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions);
virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const = 0;  Vector<uint8_t> get_buffer(int64_t p_length) const;
virtual bool store_buffer(const uint8_t *p_src, uint64_t p_length) = 0;  bool store_buffer(const Vector<uint8_t> &p_buffer);
virtual bool store_string(const String &p_string);  virtual bool store_line(const String &p_line);  String get_as_text() const;
virtual void flush() = 0;  virtual void close() = 0;  virtual String get_path_absolute() const;
// core/io/dir_access.h
static Ref<DirAccess> open(const String &p_path, Error *r_error = nullptr);
static Ref<DirAccess> create_temp(const String &p_prefix = "", bool p_keep = false, Error *r_error = nullptr);
static Error make_dir_absolute(const String &p_dir);  static Error make_dir_recursive_absolute(const String &p_dir);
static bool dir_exists_absolute(const String &p_dir);  static bool exists(const String &p_dir);
static Error copy_absolute(const String &p_from, const String &p_to, int p_chmod_flags = -1);
static Error rename_absolute(const String &p_from, const String &p_to);  static Error remove_absolute(const String &p_path);
static PackedStringArray get_files_at(const String &p_path);  static PackedStringArray get_directories_at(const String &p_path);
virtual Error list_dir_begin() = 0;  virtual String get_next() = 0;  virtual Error make_dir_recursive(const String &p_dir);
virtual Error erase_contents_recursive();   // "super dangerous, use with care!"
```

`create_temp(..., p_keep = false)` deletes the file/dir when the returned object is freed — keep the `Ref` alive
for as long as a child process needs the path (`get_path_absolute()`), or pass `p_keep = true` and clean up
yourself. Scratch data that must survive between sessions: `EditorPaths::get_singleton()->get_cache_dir().path_join("tetra")`;
per-project derived data (proxy clips, thumbnails): under `res://.godot/` (ignored by VCS and by the importer).

### 5.3 JSON, JSON-RPC, base64

```cpp
// core/io/json.h
static String JSON::stringify(const Variant &p_var, const String &p_indent = "", bool p_sort_keys = true, bool p_full_precision = false);
static Variant JSON::parse_string(const String &p_json_string);               // Variant() on error
Error JSON::parse(const String &p_json_string, bool p_keep_text = false);     // instance: get_data(), get_error_line(), get_error_message()
static Variant JSON::from_native(const Variant &p_variant, bool p_full_objects = false);   // Variant <-> JSON-safe encoding of Godot types
static Variant JSON::to_native(const Variant &p_json, bool p_allow_objects = false);
// modules/jsonrpc/jsonrpc.h   (module "jsonrpc"; class JSONRPC : public Object)
enum ErrorCode { PARSE_ERROR = -32700, INVALID_REQUEST = -32600, METHOD_NOT_FOUND = -32601, INVALID_PARAMS = -32602, INTERNAL_ERROR = -32603 };
Dictionary make_request(const String &p_method, const Variant &p_params, const Variant &p_id);
Dictionary make_notification(const String &p_method, const Variant &p_params);
Dictionary make_response(const Variant &p_value, const Variant &p_id);
Dictionary make_response_error(int p_code, const String &p_message, const Variant &p_id = Variant()) const;
Variant process_action(const Variant &p_action, bool p_process_arr_elements = false);
String process_string(const String &p_input);
void set_method(const String &p_name, const Callable &p_callback);
// core/core_bind.h  (namespace CoreBind)
CoreBind::Marshalls::get_singleton()->raw_to_base64(const Vector<uint8_t> &p_arr) -> String;
CoreBind::Marshalls::get_singleton()->base64_to_raw(const String &p_str) -> Vector<uint8_t>;
// also variant_to_base64 / base64_to_variant / utf8_to_base64 / base64_to_utf8
// core/crypto/crypto_core.h
static String CryptoCore::b64_encode_str(const uint8_t *p_src, size_t p_src_len);
static Error CryptoCore::b64_encode(uint8_t *r_dst, size_t p_dst_len, size_t *r_len, const uint8_t *p_src, size_t p_src_len);
static Error CryptoCore::b64_decode(uint8_t *r_dst, size_t p_dst_len, size_t *r_len, const uint8_t *p_src, size_t p_src_len);
```

**Every** JSON number parses to a `FLOAT` Variant (the tokenizer uses `String::to_float` → `double`,
`core/io/json.cpp:390-396`), including integers — convert explicitly (`int64_t(v)`) on the receiving side; ids
above 2^53 lose precision. `JSONRPC` is only the message builder/dispatcher (JSON-RPC 2.0 dictionaries); transport is yours.
A Tetra module that uses it must declare the dependency in `config.py`
(`env.module_add_dependencies("tetra", ["jsonrpc"])`) and include `modules/jsonrpc/jsonrpc.h`. For bulk mesh/pixel
payloads prefer raw binary framing over base64-in-JSON (33 % overhead + UTF-32 `String` conversion).

### 5.4 Sockets and HTTP

```cpp
// core/io/stream_peer.h — base: put_data/put_partial_data/get_data/get_partial_data/get_available_bytes,
//   put_u8..put_u64, put_float, put_double, put_string, put_utf8_string, put_var, get_* counterparts, set_big_endian
// core/io/stream_peer_socket.h
class StreamPeerSocket : public StreamPeer {
	enum Status { STATUS_NONE, STATUS_CONNECTING, STATUS_CONNECTED, STATUS_ERROR };
	void disconnect_from_host();  int get_available_bytes() const override;  Status get_status() const;
	Error poll();                                                   // must be called to progress STATUS_CONNECTING and detect disconnects
	Error wait(NetSocket::PollType p_type, int p_timeout = 0);
	Error put_data(const uint8_t *p_data, int p_bytes) override;                       // blocks until everything is sent
	Error put_partial_data(const uint8_t *p_data, int p_bytes, int &r_sent) override;   // non-blocking
	Error get_data(uint8_t *p_buffer, int p_bytes) override;                           // BLOCKS until p_bytes arrived
	Error get_partial_data(uint8_t *p_buffer, int p_bytes, int &r_received) override;   // non-blocking
};
// core/io/stream_peer_tcp.h
class StreamPeerTCP : public StreamPeerSocket {
	Error bind(int p_port, const IPAddress &p_host);  Error connect_to_host(const IPAddress &p_host, int p_port);
	IPAddress get_connected_host() const;  int get_connected_port() const;  int get_local_port() const;  void set_no_delay(bool p_enabled);
};
// core/io/stream_peer_uds.h  (Unix domain sockets)
class StreamPeerUDS : public StreamPeerSocket { Error bind(const String &p_path);  Error connect_to_host(const String &p_path);  const String get_connected_path() const; };
// core/io/socket_server.h / tcp_server.h / uds_server.h
class SocketServer : public RefCounted { bool is_listening() const;  bool is_connection_available() const;  void stop(); /* MAX_PENDING_CONNECTIONS = 8 */ };
class TCPServer : public SocketServer { Error listen(uint16_t p_port, const IPAddress &p_bind_address = IPAddress("*"));  int get_local_port() const;  Ref<StreamPeerTCP> take_connection(); };
class UDSServer : public SocketServer { Error listen(const String &p_path);  Ref<StreamPeerUDS> take_connection(); };
```

Bridge-friendly pattern: the editor listens on `TCPServer::listen(0, IPAddress("127.0.0.1"))` (port 0 = ephemeral,
read it back with `get_local_port()`), passes the port to the child on its command line, the child connects back;
poll `is_connection_available()`/`take_connection()` then `poll()` + `get_available_bytes()` +
`get_partial_data()` from `_process` or a dedicated `Thread`. This avoids the fixed-port collision problem of the
stock Blender runner (§6). `UDSServer`/`StreamPeerUDS` is new API with no in-tree consumer found in this pass —
verify on the target platforms before depending on it.

`HTTPClient` (`core/io/http_client.h`; create with `HTTPClient::create()`): `connect_to_host(const String &p_host, int p_port = -1, Ref<TLSOptions> = …)`,
`request(Method p_method, const String &p_url, const Vector<String> &p_headers, const uint8_t *p_body, int p_body_size)`,
`poll()`, `get_status()` (`STATUS_DISCONNECTED, RESOLVING, CANT_RESOLVE, CONNECTING, CANT_CONNECT, CONNECTED,
REQUESTING, BODY, CONNECTION_ERROR, TLS_HANDSHAKE_ERROR`), `has_response()`, `get_response_code()`,
`get_response_headers(List<String> *)`, `get_response_body_length()`, `PackedByteArray read_response_body_chunk()`,
`set_blocking_mode(bool)`, `set_read_chunk_size(int)`, `close()`. Methods: `METHOD_GET, HEAD, POST, PUT, DELETE,
OPTIONS, TRACE, CONNECT, PATCH`. A full blocking request loop is in §6.3. `XMLParser` (`core/io/xml_parser.h`) is
available for XML-RPC responses and for reading Kdenlive/MLT XML.

### 5.5 Threads and synchronisation primitives

```cpp
// core/os/thread.h
class Thread {
	typedef void (*Callback)(void *p_userdata);  typedef uint64_t ID;  enum : ID { UNASSIGNED_ID = 0, MAIN_ID = 1 };
	enum Priority { PRIORITY_LOW, PRIORITY_NORMAL, PRIORITY_HIGH };  struct Settings { Priority priority; };
	ID start(Thread::Callback p_callback, void *p_user, const Settings &p_settings = Settings());
	bool is_started() const;  void wait_to_finish();          // must be called before destruction of a started thread
	static ID get_caller_id();  static bool is_main_thread();  static Error set_name(const String &p_name);  static void yield();
};
// core/os/mutex.h
using Mutex = MutexImpl<std::recursive_mutex>;   // recursive
using BinaryMutex = MutexImpl<std::mutex>;       // non-recursive
//   lock() const, unlock() const, try_lock() const
MutexLock lock(my_mutex);                         // RAII guard (CTAD), as in `MutexLock lock(process_map_mutex);`
// core/os/semaphore.h
class Semaphore { void post(uint32_t p_count = 1) const;  void wait() const;  bool try_wait() const; };
// core/os/condition_variable.h — ConditionVariable (wait(MutexLock<BinaryMutex>&), notify_one(), notify_all())
// core/templates/safe_refcount.h
template <typename T> class SafeNumeric { set, get, increment, postincrement, decrement, postdecrement, add, sub, bit_or, bit_and, exchange_if_greater, conditional_increment };
class SafeFlag { bool is_set() const;  void set();  void clear();  void set_to(bool);  bool set_if_clear();  bool clear_if_set(); };
```

`WorkerThreadPool` (`core/object/worker_thread_pool.h`, `WorkerThreadPool::get_singleton()`):

```cpp
typedef int64_t TaskID;  typedef int64_t GroupID;  enum { INVALID_TASK_ID = -1 };
template <typename C, typename M, typename U>
TaskID add_template_task(C *p_instance, M p_method, U p_userdata, bool p_high_priority = false, const String &p_description = String());
TaskID add_native_task(void (*p_func)(void *), void *p_userdata, bool p_high_priority = false, const String &p_description = String());
TaskID add_task(const Callable &p_action, bool p_high_priority = false, const String &p_description = String(), bool p_pump_task = false);
bool is_task_completed(TaskID p_task_id) const;
Error wait_for_task_completion(TaskID p_task_id);
template <typename C, typename M, typename U>
GroupID add_template_group_task(C *p_instance, M p_method, U p_userdata, int p_elements, int p_tasks = -1, bool p_high_priority = false, const String &p_description = String());
GroupID add_native_group_task(void (*p_func)(void *, uint32_t), void *p_userdata, int p_elements, int p_tasks = -1, bool p_high_priority = false, const String &p_description = String());
GroupID add_group_task(const Callable &p_action, int p_elements, int p_tasks = -1, bool p_high_priority = false, const String &p_description = String());
uint32_t get_group_processed_element_count(GroupID p_group) const;
bool is_group_task_completed(GroupID p_group) const;
void wait_for_group_task_completion(GroupID p_group);
int get_thread_count() const;  int get_thread_index() const;  TaskID get_caller_task_id() const;
```

- Template task method shape: `void C::method(U p_userdata)`; group task: `void C::method(uint32_t p_index, U p_userdata)`
  (see `EditorFileSystem::_reimport_thread` used with `add_template_group_task(this, &EditorFileSystem::_reimport_thread, &tdata, item_count, -1, false, description)`,
  `editor_file_system.cpp`, reimport loop).
- Every task **must** be waited for (`wait_for_task_completion` / `wait_for_group_task_completion`) — that call is
  also what releases the task record (`tasks.erase` + `task_allocator.free`, `worker_thread_pool.cpp:413-415, 449-451`);
  fire-and-forget tasks leak their slot until waited. A non-pool caller (e.g. the main thread) simply blocks on
  the task's semaphore; a pool thread that waits keeps running other tasks meanwhile (`_wait_collaboratively`).
  For non-blocking UIs poll `is_task_completed(id)` from `_process` and call `wait_for_task_completion(id)` once
  it reports true.
- `p_high_priority = false` tasks are limited by the "low priority thread ratio" so they cannot starve the pool;
  long blocking jobs (a whole MLT render, a Blender round trip) should use a dedicated `Thread`, not a pool worker.
- Tiles/rows for filters and compositing: `add_native_group_task(fn, ctx, tile_count)` then wait — the idiom the
  engine itself uses for image/lightmap work.

### 5.6 Getting back to the main thread

```cpp
// core/variant/callable.h
template <typename... VarArgs> void Callable::call_deferred(VarArgs... p_args) const;
// core/object/callable_mp.h
callable_mp(object_ptr, &Class::method)            // Object-derived instance; .bind(args…) to pre-bind
callable_mp_static(&free_function)
// core/object/object.h
template <typename... VarArgs> void Object::call_deferred(const StringName &p_name, VarArgs... p_args);   // needs a bound method name
// core/object/message_queue.h
MessageQueue::get_singleton()->push_callable(callable, args…);     // what call_deferred uses
MessageQueue::get_main_singleton()                                 // always the main-thread queue
```

- `callable_mp(this, &X::_done).call_deferred(result)` from a worker thread is the standard, thread-safe way to
  marshal a result to the main thread (in-tree: `callable_mp(_edit.gizmo.ptr(), &Path3DGizmo::redraw).call_deferred()`,
  `callable_mp(EditorFileSystem::get_singleton(), &EditorFileSystem::scan).call_deferred()`). The queue is flushed
  by the main loop every frame. Arguments are copied as `Variant`s: pass `Ref<>`s/Packed arrays (refcounted, cheap),
  not raw pointers to stack data.
- `MessageQueue::get_singleton()` returns a *thread-local override* when one is set (process thread groups);
  `get_main_singleton()` is unambiguous if that ever matters.
- The target object must still be alive when the queue flushes: a `callable_mp` callable stores the instance
  pointer plus its ObjectID and, if the object has been freed, the call fails safely with the error
  *"Invalid Object id '…', can't call method."* (`core/object/callable_mp.h:101`) instead of crashing. Cancel
  or join workers before freeing their owner to avoid error spam; for `RefCounted` targets keep a `Ref` alive.

### 5.7 Thread-safety rules

- **Scene tree / Nodes / Controls**: main thread only. Node methods are guarded in debug builds by
  `ERR_THREAD_GUARD` / `ERR_MAIN_THREAD_GUARD` / `ERR_READ_THREAD_GUARD` (`scene/main/node.h:935-940`), which print
  e.g. *"The caller thread can't call the function `%s()` on this node. Use `call_deferred()` or
  `call_deferred_thread_group()` instead."* and return. A node *outside* the tree may be built on another thread
  (`is_accessible_from_caller_thread()` is true when `!data.tree`), then added to the tree via `call_deferred`.
  `Control::accept_event`, `queue_redraw`, `EditorUndoRedoManager`, `EditorFileSystem`, `EditorNode` — all main thread.
- **Resources** (`Image`, `ArrayMesh`, `ImageTexture`, …): no internal locking of their data; create and fill them
  on a worker, publish to the main thread, and do not touch concurrently. `Resource::emit_changed()` from a
  non-main thread is only special-cased during `ResourceLoader` loads (`core/io/resource.cpp:49-60`) — otherwise it
  emits on the calling thread, which will run main-thread-only slots on the wrong thread. Rule: **anything that
  emits `changed` (e.g. `ImageTexture::update`, `ArrayMesh::add_surface`) is called on the main thread.**
- **RenderingServer**: every call is safe from any thread. `RenderingServerDefault` wraps each method in a macro
  that pushes to a `CommandQueueMT` when `Thread::get_caller_id() != server_thread`
  (`servers/rendering/rendering_server_default.h:117-119`, `servers/server_wrap_mt_common.h:192-201`); calls that
  return a value synchronise (block until the render thread ran them); RID-creating calls return immediately with
  the RID allocated on the caller thread. So a worker may call `RS::texture_2d_update`, `RS::mesh_add_surface`,
  `RS::instance_set_transform` directly; ordering is preserved per calling thread. Direct `RenderingDevice` use
  must be on the render thread: `RS::call_on_render_thread(const Callable &)`, `RS::is_on_render_thread()`.
- **AudioServer**: bus/effect mutations are internally locked; the mix callbacks and `_mix_internal` run on the
  audio thread — no allocation, no locks held long, no Variant/String work there.
- **Variant containers** (`Array`, `Dictionary`, Packed arrays, `String`, `Vector`): refcount is atomic, contents are
  not — never mutate a shared instance from two threads; copy-on-write protects `Vector`/Packed arrays only when
  each thread holds its own handle and writes through `ptrw()`.
- **Engine-independent cores** use `std::thread`/`std::mutex` freely; they must simply never call into Godot from
  their own threads except through the binding layer's deferred-call shim.
- Editor progress UI: `EditorProgress ep("task_id", TTR("Label"), steps, /*can_cancel*/ false);
  ep.step(TTR("state"), i);` (modal, main thread, pumps the event loop — `editor/editor_node.h:111-118`);
  `EditorProgressBG` (`:1054-1062`) for the background-task indicator; toasts:
  `EditorToaster::get_singleton()->popup_str(msg, EditorToaster::SEVERITY_WARNING)` (`editor/gui/editor_toaster.h:117`).

---

## 6. The existing Blender integration (seed of the Tetra Blender bridge)

Files: `modules/gltf/editor/editor_scene_importer_blend.{h,cpp}` (importer + setup dialog),
`modules/gltf/editor/editor_import_blend_runner.{h,cpp}` (process + RPC), `modules/gltf/register_types.cpp`
(wiring). Pipeline: **.blend → (Blender headless, `bpy.ops.export_scene.gltf`) → `.gltf` + `.bin` + textures in
`res://.godot/imported/` → `GLTFDocument` → Godot scene**.

### 6.1 Settings and wiring

| Setting | Kind | Default | Defined at |
|---|---|---|---|
| `filesystem/import/blender/blender_path` | **EditorSettings**, `STRING`, `PROPERTY_HINT_GLOBAL_FILE`, restart-if-changed | `""` | `editor/settings/editor_settings.cpp:696` |
| `filesystem/import/blender/rpc_port` | EditorSettings, `INT` 0..65535, restart-if-changed; **0 disables RPC** | `6011` | `:697` |
| `filesystem/import/blender/rpc_server_uptime` | EditorSettings, `FLOAT` seconds (0..300, or greater) | `5` | `:698` |
| `filesystem/import/blender/enabled` | **ProjectSettings** (restart), `.android` / `.web` overrides false | `true` | `modules/gltf/register_types.cpp:154-158` |
| `filesystem/import/blender/blender3_path` | legacy EditorSetting (a *directory*); migrated to `blender_path` by appending `/blender`, `\blender.exe` or `/Contents/MacOS/Blender`, then erased | — | `register_types.cpp:67-89` |

`_editor_init()` (run through `EditorNode::add_init_callback`, `register_types.cpp:60-103, 160`):
registers `EditorSceneFormatImporterGLTF`; if `filesystem/import/blender/enabled`:
`ResourceImporterScene::add_scene_importer(Ref<EditorSceneFormatImporterBlend>)` and
`EditorFileSystem::get_singleton()->add_import_format_support_query(Ref<EditorFileSystemImportFormatSupportQueryBlend>)`;
always: `memnew(EditorImportBlendRunner)` added as a child of `EditorNode` (singleton
`EditorImportBlendRunner::get_singleton()`).

### 6.2 Locating and validating the executable

`static bool _get_blender_version(const String &p_path, int &r_major, int &r_minor, String *r_err = nullptr)`
(`editor_scene_importer_blend.cpp:54-100`):

1. `FileAccess::exists(p_path)` else "Path does not point to a valid executable."
2. `OS::execute(p_path, {"--version"}, &pipe)` (blocking, shell-quoted) else "Couldn't run Blender executable."
3. Find `"Blender "` in the output ("Unexpected --version output from Blender executable at: %s."), parse
   `major.minor` after it; reject `major < 3` ("Found Blender version %d.x, which is too old for this importer
   (3.0+ is required).").

Results are cached per importer instance (`blender_major_version`, `blender_minor_version`,
`last_tested_blender_path`) and re-probed only when the path setting changes (`:114-120`).

First-use dialog: `EditorFileSystemImportFormatSupportQueryBlend` — `is_active()` returns true when the project
has blend import enabled but `_test_blender_path(EDITOR_GET("filesystem/import/blender/blender_path"))` fails;
`get_file_extensions()` = `{"blend"}`; `query()` (`:551-649`) shows a borderless `ConfirmationDialog` "Configure
Blender Importer" with a path `LineEdit`, Browse (`EditorFileDialog`, `ACCESS_FILESYSTEM`, `FILE_MODE_OPEN_FILE`), live
validation, OK = "Confirm Path", Cancel = "Disable '.blend' Import"; it **spins a nested main loop**
(`DisplayServer::process_events(); Main::iteration();`) until closed; on confirm writes the EditorSetting and saves;
on cancel sets the ProjectSetting to false, saves and restarts the editor. In headless mode it errors out.
Autodetection (`_autodetect_path`, `:475-529`) tries, validating each with `_test_blender_path`:

| Platform | Candidates |
|---|---|
| Linux/BSD | `/usr/bin/blender`, `/usr/local/bin/blender`, `/opt/blender/bin/blender` |
| macOS | `/opt/homebrew/bin/blender`, `/opt/local/bin/blender`, `/usr/local/bin/blender`, `/usr/local/opt/blender`, `/Applications/Blender.app/Contents/MacOS/Blender`, plus `mdfind kMDItemCFBundleIdentifier=org.blenderfoundation.blender` results + `/Contents/MacOS/Blender` |
| Windows | `C:\Program Files\Blender Foundation\blender.exe`, `C:\Program Files (x86)\Blender Foundation\blender.exe`, plus the `.blend` "open" association (`AssocQueryString`) directory + `blender.exe` |

(No `PATH` search and no Flatpak/Snap/Steam locations — Tetra should add `which blender`-style lookup and its own
bundled/venv location, e.g. the repo's `build/blender-venv` `bpy` module.)

### 6.3 Launch, command line, the two embedded Python scripts

`Error EditorImportBlendRunner::start_blender(const String &p_python_script, bool p_blocking)`
(`editor_import_blend_runner.cpp:160-183`):

```
<blender_path> --background --python-exit-code 1 --python-expr <script>
```

- blocking: `OS::execute(blender_path, args, &str, &exitcode, true /*read_stderr*/)`; non-zero exit ⇒
  `print_error("Blender import failed: <captured output>.")`, `FAILED`.
- non-blocking: `OS::create_process(blender_path, args, &blender_pid)`.

Entry point `Error do_import(const Dictionary &p_options)` (`:189-208`): if `is_using_rpc()` (`rpc_port != 0`) try
`do_import_rpc`; on `ERR_CONNECTION_ERROR` print an explanatory error, **persistently set `rpc_port` to 0**
(`EditorSettings::set_manually`) and fall back; any error other than `ERR_QUERY_FAILED` falls back to
`do_import_direct`. Without RPC: `do_import_direct`.

**Direct mode** (`PYTHON_SCRIPT_DIRECT`, one Blender start-up per file, blocking the editor):

```python
import bpy, sys
opts = %s                                  # dict_to_python(p_options): nested dict literal, bool -> True/False, strings c_escape()d in single quotes
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
bpy.ops.wm.open_mainfile(filepath=opts['path'])
if opts['unpack_all']:
  bpy.ops.file.unpack_all(method='USE_LOCAL')
bpy.ops.export_scene.gltf(**opts['gltf_options'])
```

**RPC mode** (`PYTHON_SCRIPT_RPC`, `%d` = port; Blender stays alive between imports):

```python
import bpy, sys, threading
from xmlrpc.server import SimpleXMLRPCServer
req = threading.Condition()
res = threading.Condition()
info = None
export_err = None
def xmlrpc_server():
  server = SimpleXMLRPCServer(('127.0.0.1', %d))
  server.register_function(export_gltf)
  server.serve_forever()
def export_gltf(opts):
  with req:
    global info
    info = ('export_gltf', opts)
    req.notify()
  with res:
    res.wait()
  if export_err:
    raise export_err
  # Important to return a value to prevent the error 'cannot marshal None unless allow_none is enabled'.
  return 'BLENDER_GODOT_EXPORT_SUCCESSFUL'
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
threading.Thread(target=xmlrpc_server).start()
while True:
  with req:
    while info is None:
      req.wait()
  method, opts = info
  if method == 'export_gltf':
    try:
      export_err = None
      bpy.ops.wm.open_mainfile(filepath=opts['path'])
      if opts['unpack_all']:
        bpy.ops.file.unpack_all(method='USE_LOCAL')
      bpy.ops.export_scene.gltf(**opts['gltf_options'])
    except Exception as e:
      export_err = e
  info = None
  with res:
    res.notify()
```

Design points worth keeping: the XML-RPC server runs on a *secondary* thread and only enqueues; **all `bpy` work
happens on Blender's main thread** in the `while True` loop (bpy is not thread-safe); one request at a time;
errors are transported as XML-RPC faults by re-raising in the handler. Limitations: a single fixed loopback port
(two editor instances collide — the second one talks to the first one's Blender, which is tolerated because the
script is identical), no authentication, a single `export_gltf` method, no progress or cancellation, and a race in
the script (`export_err` is read by the handler thread after `res.wait()` without re-checking a predicate).

`Error do_import_rpc(const Dictionary &p_options)` (`:245-326`):

1. `kill_timer->stop()`; if `!is_running()` (`blender_pid != 0 && OS::is_process_running(blender_pid)`), start
   Blender non-blocking with the RPC script.
2. Body: `<?xml version="1.0"?><methodCall><methodName>export_gltf</methodName><params><param><value>` +
   `dict_to_xmlrpc(p_options)` + `</value></param></params></methodCall>`; `dict_to_xmlrpc` supports only `BOOL`
   (`<boolean>0|1</boolean>`), `STRING`/`STRING_NAME` (`<string>…xml_escape()</string>`) and nested `DICTIONARY`
   (`<struct><member><name>k</name><value>…</value></member>…</struct>`); any other Variant type is an error.
3. `connect_blender_rpc(client, 1000000)` (`:210-243`): `client->connect_to_host("127.0.0.1", rpc_port)` then a loop
   of `OS::delay_usec(1000)` + `get_status()`; while `RESOLVING`/`CONNECTING` → `poll()`; on any failure status it
   re-issues `connect_to_host` until `attempts * 1000 µs` exceeds the 1 s timeout (this is what absorbs Blender's
   start-up time — 1 s total, after which the import falls back to direct mode and **RPC gets disabled for good**).
4. `client->request(HTTPClient::METHOD_POST, "/", Vector<String>(), xml_buffer.ptr(), xml_buffer.size())`.
5. Busy loop: `STATUS_REQUESTING` → `poll()`; `STATUS_BODY` → `poll()` + append `read_response_body_chunk()`;
   `STATUS_CONNECTED` → done; anything else → `ERR_CONNECTION_ERROR`. (Blocks the editor main thread for the whole
   export; no sleep in the loop.)
6. Response code ≠ 200 → `ERR_QUERY_FAILED` ("Error received from Blender - status code: %s, error: %s"). If the
   body lacks `BLENDER_GODOT_EXPORT_SUCCESSFUL` and is not the legacy "cannot marshal None unless allow_none is
   enabled" message → `ERR_QUERY_FAILED` with the concatenated text nodes of the XML fault
   (`_extract_error_message_xml`, via `XMLParser::open_buffer` / `read()` / `NODE_TEXT`).

Lifetime: the runner connects to `EditorFileSystem`'s `resources_reimported` signal and (re)starts a one-shot
`Timer` of `rpc_server_uptime` seconds; on timeout `_kill_blender()` → `OS::kill(blender_pid)` (SIGKILL) and
`blender_pid = 0`; also killed on `NOTIFICATION_PREDELETE`. Blender's stdout/stderr in RPC mode go to the editor's
own stdio (inherited), not to the Output dock.

### 6.4 What the importer sends (`EditorSceneFormatImporterBlend::import_scene`, `:106-361`)

Request dictionary: `{ "path": <absolute .blend path>, "unpack_all": <bool>, "gltf_options": { … } }` where
`path = ProjectSettings::globalize_path(p_path)` (Windows network shares get their leading `//` rewritten to `\\`)
and the sink is
`ProjectSettings::get_imported_files_path().path_join(vformat("%s-%s.gltf", blend_basename, p_path.md5_text()))`,
globalised.

Fixed glTF exporter arguments: `filepath = sink_global`, `export_format = "GLTF_SEPARATE"`, `export_yup = True`,
`export_import_convert_lighting_mode = "COMPAT"`, `export_keep_originals = unpack_original_images`.

| Import option (default) | Enum / type | glTF exporter kwargs |
|---|---|---|
| `blender/nodes/visible` (`BLEND_VISIBLE_ALL`) | "All,Visible Only,Renderable" | Visible Only → `use_visible=True`; Renderable → `use_renderable=True`; All → both False |
| `blender/nodes/active_collection_only` (false) | bool | `use_active_collection=True` when set |
| `blender/nodes/punctual_lights` (true) | bool | `export_lights` |
| `blender/nodes/cameras` (true) | bool | `export_cameras` |
| `blender/nodes/custom_properties` (true) | bool | `export_extras` |
| `blender/nodes/modifiers` (`BLEND_MODIFIERS_ALL`) | "No Modifiers,All Modifiers" | `export_apply` |
| `blender/meshes/vertex_colors` (`BLEND_VERTEX_COLOR_ACTIVE`) | "Material,Active,None" | Blender ≥ 4.2: `export_vertex_color = "MATERIAL" / "ACTIVE" / "NONE"`; older: `export_colors = (option != None)` |
| `blender/meshes/uvs` (true) | bool | `export_texcoords` |
| `blender/meshes/normals` (true) | bool | `export_normals` |
| `blender/meshes/export_geometry_nodes_instances` (false) | bool | Blender ≥ 4.1: `export_gn_mesh`; on exactly 4.1 also forces `export_lights=False` (Blender bug) |
| `blender/meshes/gpu_instances` (false) | bool | Blender ≥ 4.0: `export_gpu_instances` |
| `blender/meshes/tangents` (true) | bool | `export_tangents` |
| `blender/meshes/skins` (`BLEND_BONE_INFLUENCES_ALL`) | "None,4 Influences (Compatible),All Influences" | None → `export_skins=False`; Compatible → `export_skins=True, export_all_influences=False`; All → both True |
| `blender/meshes/export_bones_deforming_mesh_only` (false) | bool | `export_def_bones` |
| `blender/materials/unpack_enabled` (true) | bool | `unpack_all` in the request **and** `export_keep_originals` |
| `blender/materials/export_materials` (`BLEND_MATERIAL_EXPORT_EXPORT`) | "Placeholder,Export,Named Placeholder" | `export_materials = "PLACEHOLDER"` / `"EXPORT"` / (`"EXPORT"` + `export_image_format = "NONE"`) |
| `blender/animation/limit_playback` (true) | bool | `export_frame_range` |
| `blender/animation/always_sample` (true) | bool | `export_force_sampling` |
| `blender/animation/group_tracks` (true) | bool | Blender ≥ 3.6: `export_animation_mode = "ACTIONS"` / `"ACTIVE_ACTIONS"`; older: `export_nla_strips = True/False` |
| `gltf/naming_version` (2) | "Godot 4.0 or 4.1,Godot 4.2 to 4.4,Godot 4.5 or later" | `GLTFDocument::set_naming_version` |
| `gltf/texture_map_mode` (`TEXTURE_MAP_MODE_REMAP_TO_STANDARD_MATERIAL`) | "Do Not Remap,Remap to StandardMaterial3D" | `GLTFDocument::set_texture_map_mode` |

Enum values (`editor_scene_importer_blend.h`): `BLEND_VISIBLE_ALL, _VISIBLE_ONLY, _RENDERABLE`;
`BLEND_BONE_INFLUENCES_NONE, _COMPATIBLE, _ALL`; `BLEND_MATERIAL_EXPORT_PLACEHOLDER, _EXPORT, _NAMED_PLACEHOLDER`;
`BLEND_MODIFIERS_NONE, _ALL`; `BLEND_VERTEX_COLOR_MATERIAL, _ACTIVE, _NONE`. `handle_compatibility_options` maps the
legacy boolean `blender/meshes/colors` and defaults a missing `gltf/texture_map_mode` to "Do Not Remap".

After Blender returns OK the glTF is read back with (`:327-360`):

```cpp
Ref<GLTFDocument> gltf;  gltf.instantiate();
Ref<GLTFState> state;    state.instantiate();
state->set_scene_name(blend_basename);
state->set_extract_path(p_path.get_base_dir());
state->set_extract_prefix(blend_basename);
err = gltf->append_from_file(sink.get_basename() + ".gltf", state, p_flags, sink.get_base_dir());
return gltf->generate_scene(state, (float)p_options["animation/fps"], trimming, false);
```

Errors: Blender failure → `*r_err = ERR_SCRIPT_FAILED`, null scene; glTF read failure → `FAILED`. Empty or
non-existent `blender_path` → `ERR_FAIL_COND_V_MSG` ("Blender path is empty, check your Editor Settings." /
"Invalid Blender path: %s, check your Editor Settings.").

### 6.5 What to reuse / change for the Tetra bridge

Reuse: the editor setting `filesystem/import/blender/blender_path` (so one configured Blender serves both the
stock importer and Tetra), `_get_blender_version`-style validation (copy; it is a file-static function), the
"network thread enqueues, main thread executes `bpy`" server skeleton, `GLTFDocument::append_from_file` +
`generate_scene` for getting results back as Godot scenes, the `.godot/imported` sink convention, the uptime
kill-timer idea.

Change: (1) **do not share port 6011** — pick an ephemeral port (listen on port 0 in the editor, or let Python
bind port 0 and print the port on stdout read through `execute_with_pipe`); (2) launch with `execute_with_pipe` to
capture stdout/stderr for the Output dock and to detect start-up failures; (3) pass the bootstrap script as a
**file** (`--python /path/to/tetra_bridge.py -- --port N`) instead of `--python-expr` to avoid shell-quoting and
argv-length limits (the stock approach only works because its script contains no `"`/`$`/backticks and options
are booleans/strings); (4) use a real protocol (JSON-RPC over TCP with length-prefixed frames, or XML-RPC with a
method registry) with request ids, progress notifications and cancellation; (5) run the client side on a `Thread`
or poll from `_process` rather than busy-looping the editor; (6) add a graceful shutdown RPC before `OS::kill`;
(7) wait for the server far longer than 1 s (Blender 5.x start-up with add-ons is typically several seconds — not
measured here) and never silently disable the bridge.

---

## 7. Linking system libraries from a custom module

### 7.1 How module SCsubs are executed (the facts that determine the incantation)

`modules/SCsub` (whole file is 60 lines):

```python
Import("env")
env_modules = env.Clone()
env_modules.Append(CPPDEFINES=["GODOT_MODULE"])
Export("env_modules")
...
for name, path in env.module_list.items():
    env.modules_sources = []
    base_path = path if os.path.isabs(path) else name      # custom modules: absolute path
    SConscript(base_path + "/SCsub")
    if env.modules_sources:
        lib = env_modules.add_library("module_%s" % name, env.modules_sources)
        env.Prepend(LIBS=[lib])
```

- `env` is **the** global construction environment (exported by `SConstruct:1227`); the final executable is linked
  from it in `platform/linuxbsd/SCsub` (`prog = env.add_program("#bin/godot", common_linuxbsd)`), which runs
  *after* `modules/SCsub` (`SConstruct:1236-1242`: `platform/SCsub`, `modules/SCsub`, `tests`, `main`, then
  `platform/<platform>/SCsub`). Therefore anything a module SCsub appends to **`env`** (`LIBS`, `LIBPATH`,
  `LINKFLAGS`, `RPATH`) reaches the final link, while anything appended to a **clone of `env_modules`** affects only
  objects compiled through that clone.
- Each module is archived as `libmodule_<name>.a` and *prepended* to `env["LIBS"]`; string entries appended to
  `LIBS` (system libs) therefore come after all Godot static libs on the link line — the correct order.
- Custom modules: `SConstruct:446-468` prepends both the `custom_modules` directory and its parent to the global
  `CPPPATH`, so `#include "tetra/foo.h"` resolves from any module, and module sources include engine headers
  root-relative (`#include "core/io/image.h"`).
- SCU build (`scu_build=yes` in `scripts/build.sh`) only merges folders listed in `scu_builders.py`
  (`process_folder([...])` for engine paths); custom module folders are not listed, so
  `add_source_files(..., "*.cpp")` compiles each file separately — no cross-file symbol-clash concerns, and no
  unity-build speed-up either.

In-tree precedents: `modules/camera/SCsub:20` `env.Append(LIBS=["camera2ndk", "mediandk"])` (system libs on the
global env from a module); `modules/raycast/SCsub:73`; `modules/freetype/SCsub:92-108` (inserts a built-in static
lib into `env["LIBS"]` *before the first string entry* so it precedes system libs). System-library switches
(`builtin_freetype`, `builtin_libtheora`, `builtin_mbedtls`, …) are all resolved in
`platform/linuxbsd/detect.py:247-338` with `env.ParseConfig("pkg-config <pkgs> --cflags --libs")` **on the global
env** — which is why those cflags leak to every translation unit; a custom module can do better.

### 7.2 The recommended SCsub (cflags scoped to the module, libs on the final link)

```python
#!/usr/bin/env python
from misc.utility.scons_hints import *

Import("env")
Import("env_modules")

env_tetra_paint = env_modules.Clone()

# 1) Compile flags: only for this module's objects.
#    ParseConfig on the clone would also put -l flags into the clone's LIBS (harmless: clones only build objects),
#    but asking for --cflags only keeps intent explicit.
env_tetra_paint.ParseConfig("pkg-config gegl-0.4 --cflags")

# 2) Link flags: must be on the *global* env to reach the final executable.
#    Use ParseFlags so that -L/-l/-Wl,... are sorted into LIBPATH/LIBS/LINKFLAGS, then append only link parts.
link = env.ParseFlags("!pkg-config gegl-0.4 --libs")
env.AppendUnique(LIBPATH=link["LIBPATH"])
env.Append(LIBS=link["LIBS"])            # strings -> placed after Godot's static libs
env.AppendUnique(LINKFLAGS=link["LINKFLAGS"])

# 3) Sources. Engine-independent core (warning-clean C++17) and Godot bindings.
module_obj = []
env_tetra_paint.add_source_files(module_obj, "*.cpp")
env_tetra_paint.add_source_files(module_obj, "core/*.cpp")
if env.editor_build:
    env_tetra_paint.add_source_files(module_obj, "editor/*.cpp")
env.modules_sources += module_obj
```

Notes:

- `env.ParseFlags("!cmd")` runs the command and returns a dict (`CPPPATH`, `CPPDEFINES`, `CCFLAGS`, `LIBPATH`,
  `LIBS`, `LINKFLAGS`, …) without modifying the environment; `env.ParseConfig(cmd)` = ParseFlags + MergeFlags into
  that env. Neither is used in exactly this split form in-tree (the engine always merges into the global env), so
  this recipe is derived from SCons semantics + the `modules/SCsub` structure above, **not copied from an
  in-tree module** — the first Tetra module that links GEGL/MLT is the test of it. Fallback that is known to work
  (it is what `detect.py` does): `env.ParseConfig("pkg-config gegl-0.4 --cflags --libs")` on the global env, at
  the price of leaking `-I/usr/include/glib-2.0 …` to all of Godot.
- Observed pkg-config output on the build host:
  `gegl-0.4` → `-I/usr/include/gegl-0.4 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include … -I/usr/include/json-glib-1.0 -pthread -I/usr/include/babl-0.1`
  and `-lgegl-0.4 -lgegl-npd-0.4 -Wl,--export-dynamic -lgmodule-2.0 -pthread -lglib-2.0 -ljson-glib-1.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lbabl-0.1`;
  `mlt++-7` → `-I/usr/include/mlt-7/mlt++ -I/usr/include/mlt-7` and `-lmlt++-7 -lmlt-7`.
  `-Wl,--export-dynamic` lands in `LINKFLAGS` (exports the executable's symbols to GEGL's dlopen'ed operation
  plug-ins); `-pthread` appears in both cflags and libs.
- `config.py`'s `can_build(env, platform)` is the place to probe availability
  (`os.system("pkg-config --exists gegl-0.4") == 0`, same idiom as `detect.py:314`) and to define an opt-out
  option via `get_opts(platform)`; `configure(env)` runs with the *global* env before any SCsub
  (`SConstruct:1124`) and is where a module-wide define can be added if other modules need to know
  (`env.Append(CPPDEFINES=["TETRA_HAVE_GEGL"])`).
- Module dependencies: `env.module_add_dependencies("tetra_paint", ["tetra"])` in `can_build` (already used by the
  scaffolding); optional ones take a third argument `True` in upstream usage (see `modules/*/config.py`).
- Third-party headers that are not warning-clean (glib macros, MLT++): compile the files that include them through
  a further clone with `env_thirdparty.disable_warnings()` (sets `WARNLEVEL` to `-w`, `methods.py:114-119`), exactly
  as built-in modules do for vendored code, or wrap the includes in
  `GODOT_GCC_WARNING_PUSH_AND_IGNORE("-Wshadow")` … `GODOT_GCC_WARNING_POP` (`core/typedefs.h:286-289`; clang
  variants `GODOT_CLANG_WARNING_*` at `:272-275`).

### 7.3 Exceptions, RTTI, language level

- `disable_exceptions` (BoolVariable, default **True**, `SConstruct:263`): adds `-fno-exceptions` to `CXXFLAGS`
  (MSVC: `_HAS_EXCEPTIONS=0`) for **all** C++ including modules (`SConstruct:931-937`). Consequences for module
  code: `try`/`catch`/`throw` do not compile; `new` failure and `std::` container errors (`.at()` out of range,
  `std::stoi` on bad input, `std::vector` allocation failure) call `std::terminate`/abort instead of throwing.
  Engine-independent cores must therefore be written exception-free (error codes / optional-style returns), as the
  project rules already state. If a wrapper around a throwing C++ library is ever needed (MLT++ itself does not
  throw; GEGL is C), compile just those files in a clone with `env_x.Append(CXXFLAGS=["-fexceptions"])` and catch
  everything at the boundary — do not let exceptions propagate into `-fno-exceptions` frames.
- RTTI is enabled (no `-fno-rtti`), so `dynamic_cast`/`typeid` work, but Godot code uses `Object::cast_to<T>()`.
- Standard: `-std=gnu17` for C and `-std=gnu++17` for C++ (prepended, `SConstruct:915-916`). Linux adds
  `-ffp-contract=off` and `-pipe` (`platform/linuxbsd/detect.py:204,232`).
- Defines visible to module code: `TOOLS_ENABLED` (editor builds), `DEBUG_ENABLED` (editor/debug targets),
  `GODOT_MODULE`, `MODULE_<NAME>_ENABLED` via the generated `modules/modules_enabled.gen.h`.

### 7.4 Warnings that are active (so the cores compile clean)

Options: `warnings` ∈ `extra | all | moderate | no` (default **`all`**), `werror` (default **False**);
`dev_mode=yes` is an alias for `verbose=yes warnings=extra werror=yes tests=yes strict_checks=yes`
(`SConstruct:237-257, 668-671`). `scripts/build.sh` sets none of them ⇒ `-Wall` + the GCC "common" set, warnings
not fatal. CI-style strictness for Tetra code should be checked with `warnings=extra werror=yes`.

| Level | GCC 13 flags (CCFLAGS unless noted) |
|---|---|
| always | `$WARNLEVEL` = `-Wall` |
| `all` (default) | `-Wshadow`, `-Wno-misleading-indentation`, `-Wenum-conversion` (GCC ≥ 11) |
| `extra` | all of the above + `-Wextra`, `-Wwrite-strings`, `-Wno-unused-parameter`, `-Walloc-zero`, `-Wduplicated-branches`, `-Wduplicated-cond`, `-Wstringop-overflow=4`, `-Wattribute-alias=2` (GCC ≥ 9), `-Wlogical-op` (GCC ≥ 11); CXXFLAGS: `-Wctor-dtor-privacy`, `-Wnon-virtual-dtor`, `-Wplacement-new=1`, `-Wvirtual-inheritance` |
| `moderate` | `-Wall -Wno-unused` + common |
| `no` | `-w` |
| `werror=yes` | `-Werror` |

Clang instead of GCC: common = `-Wshadow-field-in-constructor`, `-Wshadow-uncaptured-local`,
`-Wno-ordered-compare-function-pointers`, `-Wenum-conversion`; `extra` adds `-Wimplicit-fallthrough`
(`SConstruct:976-1032`).

Practical rules for warning-clean cores under `extra`:

- **`-Wshadow`** is the one that bites ported code: no local/parameter may shadow another local, a parameter, a
  member or a global (constructor parameters named like members included, under GCC). Godot's own convention
  (`p_` prefix for parameters, `r_` for outputs) exists largely for this reason.
- `-Wvirtual-inheritance` warns on *any* virtual base class; `-Wnon-virtual-dtor` requires a virtual destructor
  in every polymorphic base; `-Wduplicated-branches` flags identical `if`/`else` bodies (common in mechanically
  ported code); `-Wlogical-op` flags `a && a`-style duplicates; `-Wimplicit-fallthrough` (via `-Wextra` on GCC) needs
  `[[fallthrough]];`; `-Wenum-conversion` flags implicit conversions between different enum types.
- Unused *parameters* are allowed (`-Wno-unused-parameter`); unused variables/functions are not (`-Wall`).
- Sign-compare warnings (`-Wsign-compare`, part of `-Wall` for C++) appear when mixing `size_t` with `int` —
  frequent when bridging `std::vector::size()` and Godot's `int`-sized `Vector::size()`.

`scripts/check-module.sh <file.cpp…>` in this repository type-checks module sources using the flags recorded in
`upstream/godot/compile_commands.json` (written because the build uses `compiledb=yes`) without starting a second
SCons in the same tree.

---

## 8. Scene-integration hooks (live refresh after a workspace writes an asset)

### 8.1 `EditorFileSystem` (`editor/file_system/editor_file_system.h`, singleton `EditorFileSystem::get_singleton()`)

```cpp
EditorFileSystemDirectory *get_filesystem();
bool is_scanning() const;  bool is_importing() const;  bool doing_first_scan() const;  float get_scanning_progress() const;
void scan();                    // full rescan (threaded after the first scan); no-op if already scanning
void scan_changes();            // look for modified/added/removed files, then reimport what changed
void update_file(const String &p_file);
void update_files(const Vector<String> &p_script_paths);
void reimport_files(const Vector<String> &p_files);
Error reimport_append(const String &p_file, const HashMap<StringName, Variant> &p_custom_options, const String &p_custom_importer, Variant p_generator_parameters);
void reimport_file_with_custom_parameters(const String &p_file, const String &p_importer, const HashMap<StringName, Variant> &p_custom_params);
String get_file_type(const String &p_file) const;
EditorFileSystemDirectory *get_filesystem_path(const String &p_path);
EditorFileSystemDirectory *find_file(const String &p_file, int *r_index) const;
Error make_dir_recursive(const String &p_path, const String &p_base_path = String());
Error copy_file(const String &p_from, const String &p_to);  Error copy_directory(const String &p_from, const String &p_to);
void add_import_format_support_query(Ref<EditorFileSystemImportFormatSupportQuery> p_query);
```

Signals (`editor_file_system.cpp:3735-3740`): `filesystem_changed` ("May only be emitted on the main thread"),
`script_classes_updated`, `sources_changed(exist: bool)`, `resources_reimporting(resources: PackedStringArray)`,
`resources_reimported(resources: PackedStringArray)`, `resources_reload(resources: PackedStringArray)`.

Which call for which situation:

| You wrote… | Call | What happens |
|---|---|---|
| A **native Godot resource** (`.tres`, `.res`, `.mesh`, `.tscn`, `.material`) via `ResourceSaver::save` | `update_file(path)` | Registers/refreshes the entry: type from `ResourceLoader::get_resource_type`, UID (creates a `.uid` sidecar where applicable), modified time, dependencies; invalidates the thumbnail (`EditorResourcePreview::check_for_invalidation`); queues `filesystem_changed` so the FileSystem dock updates (`editor_file_system.cpp:2395-2560`). If the file no longer exists the entry is removed. Does **not** import anything. `EditorNode::save_resource_in_path` does this for you. |
| A **source asset that needs importing** (`.png`, `.exr`, `.wav`, `.glb`, `.blend`, …) that is *new* | `scan_changes()` (or `scan()`) | The scanner discovers the file, creates `<file>.import`, runs the importer, emits `resources_reimported`. `update_file` alone would list it without importing. Asynchronous: wait for `resources_reimported` / `filesystem_changed` before loading it. |
| An importable asset that **already exists** and was overwritten | `reimport_files({ path })` | Synchronous on the main thread with a progress dialog (temporarily disables VSync / low-processor mode); `ERR_FAIL` if called re-entrantly while `importing`; importers may fan out on `WorkerThreadPool`. Emits `resources_reimporting` before and `resources_reimported` after; loaded resources of those paths are reloaded in place so **scenes using them refresh live**. |
| Many files at once | one `reimport_files(vector)` / one `scan_changes()` | Batching matters: each call has fixed overhead and a modal progress step. |

Guards: do not call `scan()` during the first scan (`EditorPlugin::add_import_plugin` checks
`doing_first_scan()` and otherwise defers with `callable_mp(EditorFileSystem::get_singleton(), &EditorFileSystem::scan).call_deferred()`);
check `is_scanning()` / `is_importing()` before `reimport_files`. Files and folders under `res://.godot/` and any
folder containing a `.gdignore` file are skipped by the scanner (`_should_skip_directory`) — put caches there.

### 8.2 Saving resources and live in-memory refresh

```cpp
// core/io/resource_saver.h
static Error ResourceSaver::save(RequiredParam<Resource> rp_resource, const String &p_path = "", uint32_t p_flags = (uint32_t)FLAG_NONE);
enum SaverFlags { FLAG_NONE = 0, FLAG_RELATIVE_PATHS = 1, FLAG_BUNDLE_RESOURCES = 2, FLAG_CHANGE_PATH = 4, FLAG_OMIT_EDITOR_PROPERTIES = 8,
		FLAG_SAVE_BIG_ENDIAN = 16, FLAG_COMPRESS = 32, FLAG_REPLACE_SUBRESOURCE_PATHS = 64 };
static void ResourceSaver::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions);
// core/io/resource_loader.h
static Ref<Resource> ResourceLoader::load(const String &p_path, const String &p_type_hint = "", CacheMode p_cache_mode = CACHE_MODE_REUSE, Error *r_error = nullptr);
//   CACHE_MODE_IGNORE, CACHE_MODE_REUSE, CACHE_MODE_REPLACE, CACHE_MODE_IGNORE_DEEP, CACHE_MODE_REPLACE_DEEP
static bool ResourceLoader::exists(const String &p_path, const String &p_type_hint = "");
// core/io/resource.h
virtual void set_path(const String &p_path, bool p_take_over = false);     // registers in ResourceCache
//   "take_over_path" is only the *bound* (script) name of the protected `void _take_over_path(const String &p_path)`;
//   from C++ call set_path(p_path, true).
String get_path() const;  virtual void set_path_cache(const String &p_path);
void emit_changed();  void connect_changed(const Callable &p_callable, uint32_t p_flags = 0);  void disconnect_changed(const Callable &p_callable);
virtual void reload_from_file();  virtual Error copy_from(const Ref<Resource> &p_resource);
Ref<Resource> duplicate(bool p_deep = false) const;
```

- `ResourceSaver::save(res, path)` with an empty `p_path` saves to `res->get_path()`. `FLAG_CHANGE_PATH` makes the
  saver set the resource's path to the destination; `FLAG_COMPRESS` for binary `.res`/`.mesh`; `FLAG_BUNDLE_RESOURCES`
  embeds external dependencies.
- `set_path(p, false)` **fails** with *"Another resource is loaded from path '%s' (possible cyclic resource
  inclusion)."* if a different live resource is cached under `p`; `set_path(p, true)` (take over) evicts the old
  one from the cache (its `path_cache` becomes empty) and registers this one (`core/io/resource.cpp:79-116`). Use
  take-over when a workspace replaces an asset object wholesale (e.g. writes a *new* `ArrayMesh` for
  `res://models/x.mesh`): subsequent `ResourceLoader::load` calls return the new object. Nodes that already hold a
  `Ref` to the **old** object keep it — to refresh them without re-assigning, mutate the existing object in place
  (clear surfaces + add surfaces, `ImageTexture::update`, …) and let its `changed` signal propagate.
- `emit_changed()` is what dependants listen to: `MeshInstance3D` (`_mesh_changed`), materials holding textures,
  `TextureRect`, inspector previews. It is blocked/coalesced while `_block_emit_changed()` is active and is
  redirected through `ResourceLoader` when called from a loading thread.
- The canonical live-edit loop for Tetra ("edit in workspace, see it in the game scene"):
  1. hold the *same* resource instance the scene uses (obtained with `ResourceLoader::load(path)` → cache hit);
  2. mutate it in place on the main thread → viewport updates immediately;
  3. on Save: `ResourceSaver::save(res, path)` then `EditorFileSystem::get_singleton()->update_file(path)`;
  4. for *imported* formats (PNG under `res://`): write the source file, then `reimport_files({path})` — the
     importer regenerates `.godot/imported/*.ctex`, and the engine reloads the cached `CompressedTexture2D` in
     place, so the scene refreshes; during interactive strokes show the live `ImageTexture` via a temporary
     `set_surface_override_material`/override rather than reimporting per stroke.
- `EditorNode` helpers (main thread): `save_resource_in_path(const Ref<Resource> &, const String &)`,
  `save_resource(const Ref<Resource> &)`, `save_resource_as(...)`, `edit_resource(const Ref<Resource> &)`,
  `edit_node(Node *)`, `push_item(Object *, const String &p_property = "", bool p_inspector_only = false)`,
  `load_scene(...)`, `reload_scene(const String &)`, `get_edited_scene()`, `get_scene_root()`,
  `get_editor_selection()`, `show_warning(const String &p_text, const String &p_title = TTR("Warning!"))`,
  `show_accept(text, title)`.

### 8.3 Custom thumbnails: `EditorResourcePreviewGenerator` (`editor/inspector/editor_resource_preview.h`)

```cpp
class EditorResourcePreviewGenerator : public RefCounted {
public:
	virtual bool handles(const String &p_type) const;                                                     // resource class name
	virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const;
	virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const;   // default: load + generate()
	virtual void abort() {}
	virtual bool generate_small_preview_automatically() const;   // true => engine downsizes the big preview for the 16px icon
	virtual bool can_generate_small_preview() const;
	void request_draw_and_wait(RID viewport) const;              // for generators that render a viewport
};
// registration (main thread, after the editor exists):
EditorResourcePreview::get_singleton()->add_preview_generator(Ref<MyPreviewGenerator>(memnew(MyPreviewGenerator)));
EditorResourcePreview::get_singleton()->remove_preview_generator(gen);
// consumers:
void queue_resource_preview(const String &p_path, const Callable &p_callback);
void queue_edited_resource_preview(const Ref<Resource> &p_res, const Callable &p_callback);
void check_for_invalidation(const String &p_path);
```

- Generators run on the **preview thread** when `EditorResourcePreview::is_threaded()` (rendering drivers with async
  texture creation) and from an idle callback otherwise (`_thread` vs `_idle_callback` comments in the header):
  `generate()` must be thread-safe and must not touch the scene tree. Returning a null `Ref` means "no preview".
- Results are cached on disk in the editor cache dir keyed by path + modified time + a metadata version
  (`CURRENT_METADATA_VERSION = 1`); `update_file()` invalidates.
- Built-ins registered in `editor/editor_node.cpp:9434-9438…`: `EditorTexturePreviewPlugin`,
  `EditorImagePreviewPlugin`, `EditorPackedScenePreviewPlugin`, `EditorMaterialPreviewPlugin`,
  `EditorScriptPreviewPlugin`, … (`editor/inspector/editor_preview_plugins.{h,cpp}`). Image template:
  `EditorImagePreviewPlugin::generate` (`editor_preview_plugins.cpp:193-222`, summarised in §3.10).
- A Tetra project-file resource type (layered image document, timeline) gets FileSystem-dock thumbnails by
  registering a generator whose `handles()` matches its class name; the type itself needs a
  `ResourceFormatLoader`/`Saver` (or to be a plain `Resource` saved as `.tres`/`.res`).

### 8.4 `EditorExportPlugin` basics (`editor/export/editor_export_plugin.h`)

Registered with `EditorPlugin::add_export_plugin(const Ref<EditorExportPlugin> &)` (→ `EditorExport::get_singleton()->add_export_plugin`).
C++ virtuals (underscore-prefixed, protected):

```cpp
virtual String get_name() const;                                                                   // required
virtual bool supports_platform(const Ref<EditorExportPlatform> &p_export_platform) const;
virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags);
virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features);   // once per exported file
virtual void _export_end();
virtual bool _begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features);   // return true to take part
virtual Ref<Resource> _customize_resource(const Ref<Resource> &p_resource, const String &p_path);    // null = unchanged
virtual bool _begin_customize_scenes(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features);
virtual Node *_customize_scene(Node *p_root, const String &p_path);
virtual uint64_t _get_customization_configuration_hash() const;                                    // cache key for customised output
virtual void _end_customize_scenes();  virtual void _end_customize_resources();
virtual PackedStringArray _get_export_features(const Ref<EditorExportPlatform> &p_export_platform, bool p_debug) const;
virtual void _get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const;
// helpers callable from the hooks:
void add_file(const String &p_path, const Vector<uint8_t> &p_file, bool p_remap);   // inject/replace a file in the PCK
void add_shared_object(const String &p_path, const Vector<String> &tags, const String &p_target = String());
void skip();                                                                         // drop the current file (inside _export_file)
```

Tetra uses: strip editor-only source documents (layered `.xcf`-like projects, `.kdenlive`/MLT XML, `.blend`) from
exports with `skip()`, bake them to runtime formats via `add_file`, and ship required shared objects with
`add_shared_object` if a runtime module ever needs MLT. In-tree examples to read first:
`editor/export/dedicated_server_export_plugin.cpp`, `editor/export/shader_baker_export_plugin.cpp`,
`editor/export/gdextension_export_plugin.h`.

---

## 9. Things that could not be determined in this pass (verify before relying on them)

| Item | Status |
|---|---|
| Scoped pkg-config SCsub recipe (§7.2) | Derived from SCons semantics and `modules/SCsub`; not compiled here (the tree was busy with the baseline build). The global-env `ParseConfig` fallback is proven by `detect.py`. |
| `DrawableTexture2D` / `texture_blit` behaviour and performance (§3.6) | API and RD implementation read; class has no in-tree users (only its definition, registration and docs). GLES3 path exists but unverified. |
| `StreamPeerUDS` / `UDSServer` | API read; no in-tree consumer looked at; platform coverage unknown. |
| Overriding `spatial_editor/*` default shortcuts from a module | Mechanism described (§1.10); untested. |
| Programmatic multi-subgizmo selection | No public API; reachable only via `Node3DEditorSelectedItem` through `EditorSelection::get_node_editor_data` (public struct, not used that way outside the 3D editor). |
| Freeing convention of `array_mesh_lightmap_unwrap_callback` outputs | Not transcribed; read `scene/resources/mesh.cpp:2194-2275` before calling it directly. |
| Blender 5.3 start-up time vs. the stock 1 s RPC connect timeout | Not measured. |
| `ARRAY_FLAG_USE_DYNAMIC_UPDATE` on RD | Only GLES3 reads it (grep); treated as a harmless hint. |
| Windows/macOS variants of `OS::execute*` and pipes | Only the Unix implementation (`drivers/unix/os_unix.cpp`) was read. |
