# tetra_model — the Blender bridge

This module reaches Blender through a long-lived Blender process. Every
operator, modifier and primitive is discovered from Blender's RNA and
described with the operation schema of `docs/ARCHITECTURE.md`, so the generic
operation UI can show them without knowing any of them.

| File | Contents |
| --- | --- |
| `bridge/tetra_blender_server.py` | The server that runs inside Blender's Python. Embedded in the editor binary at build time. |
| `bridge/test_server.py` | Pure-Python client that checks the server on its own. |
| `tetra_blender_bridge.{h,cpp}` | `TetraBlenderBridge`: process, protocol and the high-level API. |
| `tetra_blender_mesh.{h,cpp}` | `TetraBlenderMesh`: mesh exchange files and the Godot/Blender conventions. |
| `tetra_edit_mesh*.{h,cpp}` | `TetraEditMesh`: the polygon mesh and the modelling operations of the native Edit Mode (no Blender needed). |
| `editor/tetra_mesh_edit_plugin.{h,cpp}` | `TetraMeshEditPlugin`: Edit Mode in the 3D viewport. |

Everything below is in ClassDB. The native mesh Edit Mode, which does not use
Blender, is described under "Mesh Edit Mode (native)".

## Finding Blender

`TetraBlenderBridge.find_launcher()` uses the first of these that names an
existing file:

1. editor setting `tetra/blender/python_path` — a Python that has the `bpy` module
2. project setting `tetra/blender/python_path`
3. environment variable `TETRA_BLENDER_PYTHON` (`scripts/run.sh` and `scripts/test.sh` set it to `build/blender-venv/bin/python`)
4. editor setting `tetra/blender/executable_path` — a Blender executable
5. editor setting `filesystem/import/blender/blender_path` — the one Godot's `.blend` importer uses
6. `blender` on `PATH`

A Python is run as `python tetra_blender_server.py --temp-dir DIR`, a Blender
as `blender --background --factory-startup --python-exit-code 1 --python
tetra_blender_server.py -- --temp-dir DIR`. The script is written to
`<cache>/tetra/blender/tetra_blender_server.py`; set
`TETRA_BLENDER_SERVER_SCRIPT` to run a script from disk instead (useful while
editing the server: no rebuild needed).

Nothing is started until the first request. `is_available()` only says that a
launcher exists; whether that Python really has `bpy` is found out at start.
The module announces `Blender` in `TetraEngineRegistry` at start-up
(available = a launcher was found, no version), again once editor settings
exist, and with the version after the first successful start.

## Threading rules

- Every method of `TetraBlenderBridge` may be called from any thread.
- One command is in flight at a time. `request()` and the object-returning
  calls block their caller until Blender answers; other callers queue.
- Starting Blender takes from half a second to more than ten (cold disk
  cache). **Editor UI must only use the `*_async` calls.** They return at
  once, run on the bridge's worker thread in the order they were queued, and
  call their `Callable` on the main thread.
- Signals are emitted on the main thread, when its message queue is flushed.
  While the main thread is blocked in a synchronous call nothing is emitted.
- The asynchronous calls read their `Mesh` arguments and write the exchange
  files on the calling thread before they return, so the mesh may change
  afterwards. The resulting `ArrayMesh` is created on the main thread.
- `stop()` interrupts a request in flight by ending the process.

## TetraBlenderBridge (RefCounted)

```
static get_shared() -> TetraBlenderBridge      # the instance the editor shares; TetraBlenderBridge.new() gives a private one
static find_launcher() -> Dictionary           # {kind: "python" | "blender" | "", path, description}
static is_available() -> bool

start() -> Error                               # blocking; OK, ERR_FILE_NOT_FOUND (no launcher), ERR_CANT_CONNECT, ERR_TIMEOUT, ...
stop()                                         # ends Blender, cancels queued jobs; the next request starts it again
                                               # the jobs it ends answer {ok: false, stopped: true, error}
is_running() -> bool
is_busy() -> bool                              # a request is in flight or jobs are queued
get_launch_description() -> String             # e.g. "bpy module of /…/python (environment variable TETRA_BLENDER_PYTHON)"
get_blender_version() -> String                # "5.3.0"; empty until the first start
get_server_info() -> Dictionary                # the server's hello answer, see the protocol
get_exchange_dir() -> String                   # where exchange files live; removed when the bridge is freed
get_last_error() -> String                     # message of the last failed call, empty after a success
start_timeout: float = 180.0                   # seconds to wait for Blender to come up
join_triangles: bool = false                   # see "Triangles and quads"

request(command: Dictionary, timeout_sec := 0.0) -> Dictionary
request_async(command: Dictionary, done: Callable, timeout_sec := 0.0)

list_operators() -> Array[Dictionary]          # schemas, cached after the first call
list_modifiers() -> Array[Dictionary]
list_primitives() -> Array[Dictionary]
apply_modifier_stack(mesh: Mesh, stack: Array) -> ArrayMesh
run_operators(mesh: Mesh, operators: Array, options := {}) -> ArrayMesh
create_primitive(id: String, values := {}) -> ArrayMesh
import_meshes(path: String) -> Array[Dictionary]
export_meshes(path: String, meshes: Array) -> Error
render_scene(scene: Dictionary, settings := {}) -> Image

list_operators_async(done)   list_modifiers_async(done)   list_primitives_async(done)
apply_modifier_stack_async(mesh, stack, done)
run_operators_async(mesh, operators, done, options := {})
create_primitive_async(id, values, done)
import_meshes_async(path, done)
export_meshes_async(path, meshes, done)
render_scene_async(scene, settings, done)

signal started()                               # a Blender process is up and answered hello
signal stopped()                               # the process ended, for whatever reason
signal progress(event: Dictionary)             # see "Progress"
signal log_line(line: String)                  # everything Blender prints
```

Failures never raise. `request()` answers `{"ok": false, "error": "…"}`
(plus `"traceback"` for an exception inside Blender). The object-returning
calls return `null` / an empty Array / `FAILED` and leave the message in
`get_last_error()`. A crash of Blender fails the request in flight and the
next request starts a new process.

`request()` with a positive `timeout_sec` kills Blender when the answer does
not arrive in time: a running command cannot be interrupted any other way.

### What `done` receives

One `Dictionary`: always `ok: bool` and `error: String`, and on success

| Call | Keys |
| --- | --- |
| `request_async` | the raw answer |
| `list_*_async` | `operations: Array[Dictionary]` |
| `apply_modifier_stack_async`, `run_operators_async`, `create_primitive_async` | `mesh: ArrayMesh`, `vertex_count`, `polygon_count`, `triangle_count` (Blender's own counts, ints), `skipped_values: Array[String]` (value names Blender does not know); `run_operators` also `results: [{id, result: "FINISHED" | "CANCELLED"}]` |
| `import_meshes_async` | `meshes: Array[Dictionary]` as returned by `import_meshes` |
| `export_meshes_async` | `path`, `object_count` |
| `render_scene_async` | `image: Image`, `render_time: float` (seconds), `width`, `height`, `samples`, `denoised` |

### Schemas

`list_operators()`, `list_modifiers()` and `list_primitives()` return
operation schemas that pass `TetraSchema.validate_operation`, with defaults
already of the right Variant type (`int`, `Vector2`, `Vector3`, `Color`).

- Operators: `id` is `blender:<module>.<name>` (`blender:mesh.subdivide`),
  `category` is `Mesh`, `UV`, `Object` or `Transform`. The extra key `mode` is
  `"EDIT"` or `"OBJECT"`: the mode the server switches to before running it.
  Only operators whose poll succeeds on a mesh without a 3D viewport are
  listed (about 180 with Blender 5.3); modal tools, viewport tools and
  operators that manage other objects are left out.
- Modifiers: `id` is `blender:modifier.<TYPE>` (`blender:modifier.SUBSURF`),
  `category` is the heading of Blender's Add Modifier menu (`Edit`,
  `Generate`, `Deform`, `Normals`, `Physics`), `mode` is `"MODIFIER"`.
  Defaults are read from a freshly added modifier. Modifiers that refer to
  another object list those properties in the extra key `object_params:
  [{name, label, description}]` (for example `object` of `BOOLEAN`, `target`
  of `SHRINKWRAP`); they are filled through `objects` in a stack entry.
- Primitives: `id` is `blender:mesh.primitive_<name>_add`, `category` is `Primitive`.

How RNA properties map to parameter types:

| RNA | Schema |
| --- | --- |
| BOOLEAN | `bool` |
| INT | `int`; `seed` when the property is called `seed` or `*_seed` |
| FLOAT | `float`, with `min`/`max` (hard limits), `ui_min`/`ui_max` (soft limits), `step`, `unit` |
| FLOAT with an angle unit | `float` in **degrees**, `unit` `°`; the server converts to radians |
| ENUM | `enum` with `options` |
| ENUM flags | `string` of comma-separated identifiers; extra key `flags: [{value, label}]` lists the choices |
| ENUM whose items depend on the context | `string` |
| STRING | `string`; `file` for file and directory paths |
| FLOAT array of 2 / 3 | `vec2` / `vec3` (angles in degrees) |
| colour array of 3 / 4 | `color` |
| any other array (bool, int, matrix) | `string` of comma-separated numbers (`"1,0,0"`), extra key `array_length` |
| POINTER, COLLECTION | not listed (object pointers of modifiers: see `object_params`) |

Vector parameters are in **Blender's axes** (Z up): a `value` of
`Vector3(0, 0, 2)` for `blender:transform.translate` moves the mesh up, which
is Godot's +Y. Values may be given loosely typed (a float for an int, an
Array for a vector); the server coerces them by the RNA type.

### Modifier stacks

```gdscript
var stack := [
    {"type": "SUBSURF", "values": {"levels": 2}},                  # "id": "blender:modifier.SUBSURF" works too
    {"type": "BEVEL", "values": {"width": 0.02}, "enabled": false}, # kept in the stack, not evaluated
    {"type": "BOOLEAN", "values": {"operation": "DIFFERENCE"},
     "objects": {"object": {"mesh": other_mesh, "transform": relative_transform}}},
]
bridge.apply_modifier_stack_async(mesh, stack, func(result: Dictionary) -> void:
    if result.ok:
        mesh_instance.mesh = result.mesh)
```

The stack is evaluated through Blender's dependency graph, top to bottom. An
`objects` entry is a `Mesh`, or `{mesh, transform}` with the other object's
transform relative to the mesh being modified
(`node.global_transform.affine_inverse() * other.global_transform`).
Surface materials of the input are put back on the result by slot.

### Operators

```gdscript
var operators := [
    {"id": "blender:mesh.inset", "values": {"thickness": 0.1, "use_individual": true}},
    {"id": "blender:mesh.subdivide", "values": {"number_cuts": 2}, "select": "ALL"},
]
bridge.run_operators_async(mesh, operators, done, {"join_triangles": true})
```

The mesh is loaded, edit mode is entered and everything is selected
(`options.select`: `"ALL"` (default) or `"NONE"`; `options.select_mode`:
`"VERT"`, `"EDGE"` or `"FACE"`). The operators then run in order, each in its
own mode, with `EXEC_DEFAULT`. The selection is whatever the previous
operator left — after an inset only the inner faces are selected — unless an
entry carries its own `select` (`"ALL"`, `"NONE"`, `"INVERT"`). An operator
that raises an error fails the whole call; one that cancels is reported in
`results` and the call succeeds.

### Triangles and quads

Godot meshes are triangles, and Blender's tools work best on quads: a
`BoxMesh` arrives as 12 triangles and subdivides unevenly. With
`join_triangles` (the bridge property, or the `run_operators` option) pairs of
triangles are merged back into quads before anything else runs, wherever no
material border, sharp edge or UV seam separates them. Results always come
back triangulated by Blender, n-gons included.

Shading: vertices at the same position are welded on the way to Blender and
an edge is marked sharp where the normals on its two sides differ, so flat
and smooth shading survive the round trip and behave in modifiers as they
would in Blender.

### Import and export

`import_meshes(path)` reads anything this Blender can import — `.blend`,
`.fbx`, `.obj`, `.ply`, `.stl`, `.gltf`, `.glb`, `.abc`, `.usd*`, and `.dae`
where the build has it (`get_server_info().import_extensions`) — and returns
one entry per mesh-like object, with its modifiers applied:

```
{name: String, parent: String, transform: Transform3D (world), mesh: ArrayMesh}
```

Surfaces carry a `StandardMaterial3D` rebuilt from the Principled BSDF's
plain values (base colour, metallic, roughness, emission). Textures and node
setups are not transferred.

`export_meshes(path, meshes)` writes `[{mesh: Mesh, name: String, transform:
Transform3D}]` in the format of the extension. `res://` paths are accepted.
The raw `convert_file` command converts between two formats without passing
through Godot.

### Rendering

```gdscript
var scene := {
    "meshes": [{"mesh": mesh, "transform": node.global_transform,
                "material": {"base_color": Color.RED, "metallic": 0.0, "roughness": 0.5,
                             "emission": Color.BLACK, "emission_strength": 1.0}}],
    "camera": {"transform": camera.global_transform, "fov_y_degrees": camera.fov,   # or "ortho_size": camera.size
               "near": camera.near, "far": camera.far},
    "lights": [{"type": "sun", "transform": light.global_transform, "color": Color.WHITE, "energy": 3.0}],
    "world": {"color": Color(0.05, 0.05, 0.05), "strength": 1.0},
}
var settings := {"engine": "CYCLES", "samples": 64, "width": 1280, "height": 720, "denoise": true}
bridge.render_scene_async(scene, settings, func(result: Dictionary) -> void:
    if result.ok:
        texture_rect.texture = ImageTexture.create_from_image(result.image))
```

- Transforms are Godot world transforms; cameras and lights point down their
  local -Z in both programs, so `global_transform` is passed as it is.
- `Color` values are converted from sRGB to the linear values Blender expects.
- A mesh entry without `material` uses, per surface, the surface's own
  `BaseMaterial3D` (albedo, metallic, roughness, emission); `materials:
  [Dictionary]` overrides single surfaces.
- Lights: `type` is `sun`, `point`, `spot` or `area`; `energy` is in Blender's
  units (W/m² for a sun, W for the others — a point light needs hundreds).
  Optional: `spot_angle_degrees` (half angle, like `SpotLight3D`),
  `spot_blend`, `radius`, `angle_degrees` (sun), `size` (area).
- Settings: `engine` must be `CYCLES` (rendering runs on the CPU, without a
  display); `samples` (32), `width` (640), `height` (360), `denoise` (false;
  ignored when the build has no OpenImageDenoise), `transparent` (false),
  `view_transform` (`"Standard"`), `exposure`, `max_bounces`, `seed`,
  `threads`, `adaptive_sampling` (false).
- The image is an 8-bit RGBA PNG read back as an `Image`.

### Progress

`progress` events are Dictionaries with `event`, `cmd` and `id` (the request).
Rendering emits

```
{event: "progress", cmd: "render", stage: "start",  sample: 0, samples: 64, fraction: 0.0}
{event: "progress", cmd: "render", stage: "sample", sample: 17, samples: 64, fraction: 0.265}
{event: "progress", cmd: "render", stage: "finish", sample: 64, samples: 64, fraction: 1.0}
```

one `sample` event per sample Cycles reports. Numbers arrive as floats (JSON).
Other commands emit no progress; use the callback.

## TetraBlenderMesh (static)

```
position_to_blender(position: Vector3) -> Vector3            # (x, y, z) -> (x, -z, y)
position_from_blender(position: Vector3) -> Vector3
transform_to_matrix(transform: Transform3D, convert_local_axes := true) -> PackedFloat64Array   # 16 numbers, row-major
matrix_to_transform(matrix: PackedFloat64Array, convert_local_axes := true) -> Transform3D
write_file(mesh: Mesh, path: String) -> Error                # every triangle surface; surface i = material slot i
read_surfaces(path: String) -> Array                         # [{material_index, material_name, arrays}], plain data, any thread
surfaces_to_mesh(surfaces: Array, material_source: Mesh = null) -> ArrayMesh
read_file(path: String, material_source: Mesh = null) -> ArrayMesh   # null when unreadable
read_header(path: String) -> Dictionary                      # counts and material names
```

`convert_local_axes` is true for objects whose data is converted as well
(meshes) and false for cameras and lights.

## Conventions

The conversion between the two programs happens on the Godot side, in
`TetraBlenderMesh`. Exchange files and the protocol are in Blender's
conventions:

| | Godot | Blender / protocol |
| --- | --- | --- |
| Up, forward | +Y, -Z | +Z, +Y: `(x, y, z)` ↔ `(x, -z, y)` |
| Front faces | clockwise | counter-clockwise |
| UV origin | top left | bottom left: `v` ↔ `1 - v` |
| Colours | sRGB `Color` | linear |
| Transforms | `Transform3D` | 16 floats, row-major 4×4 |

## The protocol

JSON Lines over the child's stdin and stdout. A request is one line
`{"id": n, "cmd": "…", …}`; the answer is one line `{"id": n, "ok": true, …}`
or `{"id": n, "ok": false, "error": "…", "traceback": "…"}`. Lines with
`"event"` instead of `"ok"` are unsolicited (`ready` once at start-up,
`progress`). The server duplicates the real stdout for the protocol and
points file descriptor 1 at stderr before importing Blender, so Blender's own
prints cannot corrupt it; everything else that arrives on either pipe is a
log line. `id` and `cmd` are reserved keys. `TetraBlenderBridge.request()`
fills in `id` and returns the answer without it.

| Command | Request | Answer |
| --- | --- | --- |
| `hello` | | `protocol`, `blender_version`, `blender_version_tuple`, `python_version`, `temp_dir`, `cycles`, `denoise`, `background`, `pid`, `import_extensions`, `export_extensions` |
| `list_operators` | `prefixes: ["mesh.", "uv.", "object.", "transform."]` (the default) | `operators: [schema]` |
| `list_modifiers` | | `modifiers: [schema]` |
| `list_primitives` | | `primitives: [schema]` |
| `apply_modifiers` | `mesh: file`, `output: file`, `modifiers: [{type, values, enabled, objects: {name: file \| {mesh: file, transform}}}]`, `join_triangles` | `output`, `vertex_count`, `polygon_count`, `triangle_count`, `skipped_values` |
| `run_operators` | `mesh`, `output`, `operators: [{id, values, select}]`, `select`, `select_mode`, `join_triangles` | as above plus `results` |
| `create_primitive` | `primitive: "blender:mesh.primitive_…_add"`, `values`, `output` | as `apply_modifiers` |
| `import_meshes` | `path`, `output_dir` (optional) | `meshes: [{name, parent, file, transform, materials: [{name, base_color, metallic, roughness, emission}], vertex_count, …}]`, `directory` |
| `export_meshes` | `path`, `meshes: [{file, name, transform, material \| materials}]` | `path`, `object_count` |
| `convert_file` | `input`, `output` | `output`, `object_count` |
| `render` | `scene: {meshes: [{file, name, transform, material \| materials}], camera, lights, world}`, `settings`, `output: png path` | `output`, `render_time`, `width`, `height`, `samples`, `denoised` |
| `shutdown` | | (the process exits after answering) |

The scene is reset to an empty file before every command that builds one.
Mesh files use the `.tmsh` format documented at the top of
`bridge/tetra_blender_server.py`: a JSON header followed by little-endian
arrays (positions, polygon corner starts, corner vertices, material index per
polygon, optional corner UVs and normals, and — in files Blender writes — its
triangulation, so n-gons survive and come back triangulated the way Blender
would draw them).

## Workspace

In the editor the bridge appears inside the 3D workspace (`editor/`): a
**Blender** dock (right side, next to the Inspector) and a **Blender** menu in
the 3D toolbar. Blender is started by the first action that needs it; nothing
blocks the editor, and the dock's first line shows the state: not started,
"Starting Blender...", "Blender 5.3.0 ready", "Blender 5.3.0 working...", or
"Blender not found" with a hint naming the two settings and
`scripts/setup-blender-bridge.sh`. Every change to the scene is one step in
the editor's undo history. The workspace defines no keyboard shortcuts.

| Where | What the user can do |
| --- | --- |
| Dock > **Modifiers** | For the selected MeshInstance3D: **Add Modifier** (popup grouped Edit / Generate / Deform / Normals / Physics / Other, from `list_modifiers`), the stack with an enable checkbox, move up/down and remove per entry, and the selected modifier's parameters. Blender evaluates source mesh + stack after every change (debounced; the newest request wins) and the result replaces the node's mesh. **Apply All** bakes the result and clears the stack; **Remove All** restores the original mesh. Dragging a parameter is one undo step. |
| Dock > **Operators** | Search or browse the mesh and UV operators of `list_operators`, edit the parameters, **Run** on the selected node's mesh (whole mesh selected). Afterwards the panel stays editable and **Re-run with New Values** replaces the result by running the operator again on the mesh it started from (Blender's "Adjust Last Operation"), as long as that operation is still the newest undo step. |
| Dock > **Render** | Cycles render of the edited scene: every visible MeshInstance3D with its StandardMaterial3D values, the Directional/Omni/Spot lights, the WorldEnvironment background, seen through the 3D viewport's camera or the scene's current Camera3D. Settings: camera, width, height, viewport aspect, samples, denoise, transparent background (advanced). Progress while rendering, Cancel, the picture in a pan/zoom view, **Save to Project...** (PNG; the file system dock is rescanned). |
| Menu > **Add Mesh** | The primitives of `list_primitives` (Cube, UV Sphere, Ico Sphere, Cylinder, Cone, Torus, Grid, Monkey, ...). Choosing one opens its parameter dialog; OK adds a MeshInstance3D named after the primitive, with a default material, under the selected node or the scene root. |
| Menu > **Import Model...** | Imports fbx, obj, ply, stl, usd, abc, blend and the other formats Blender reads as MeshInstance3D nodes under a new Node3D. |
| Menu > Modifiers / Operators / Render, Join Triangles into Quads, Restart Blender | Open the dock on a tab; toggle the editor setting `tetra/blender/join_triangles` (meshes go to Blender as quads where triangles pair up); end the Blender process (the next request starts a new one). |

The stack and the original mesh are stored on the node as metadata
(`tetra_blender_modifiers`: Array of the entries `apply_modifiers` takes,
`tetra_blender_source_mesh`: Mesh), so they are saved with the scene.

Settings (Editor Settings): `tetra/blender/python_path` (a Python with the
`bpy` module) and `tetra/blender/executable_path` (a Blender executable); the
environment variable `TETRA_BLENDER_PYTHON` works too. See "Finding Blender".

### Scripting surface

The editor classes exist only inside the running editor (tool scripts, editor
plugins, screenshot drivers), not in headless tests.

```gdscript
var blender := TetraBlenderEditorPlugin.get_singleton()
blender.add_primitive("blender:mesh.primitive_cube_add", {"size": 1.0})
var cube: MeshInstance3D = await blender.primitive_added
blender.add_modifier(cube, "SUBSURF", {"levels": 2})
await blender.modifiers_evaluated
blender.run_operator(cube, "blender:mesh.bevel", {"offset": 0.1})
await blender.operator_finished
blender.render_current_view({"width": 800, "samples": 64})
await blender.render_finished
blender.save_render("res://render.png")
```

`TetraBlenderEditorPlugin` (EditorPlugin). All calls return at once; results
come through the signals.

| Method | |
| --- | --- |
| `static get_singleton() -> TetraBlenderEditorPlugin` | |
| `get_bridge() -> TetraBlenderBridge`, `get_dock() -> TetraBlenderDock`, `show_tab(tab: String)` | `tab` is `"modifiers"`, `"operators"` or `"render"`. |
| `get_status() -> Status`, `get_status_text()`, `get_status_hint()`, `get_last_error() -> String`, `restart_bridge()` | `Status`: `STATUS_NOT_FOUND`, `STATUS_NOT_STARTED`, `STATUS_STARTING`, `STATUS_READY`, `STATUS_WORKING`, `STATUS_FAILED`. |
| `load_schemas(kind: SchemaKind)`, `get_schema_state(kind) -> SchemaState`, `get_schemas(kind) -> Array`, `find_schema(kind, id: String) -> Dictionary` | `SchemaKind`: `SCHEMA_MODIFIERS`, `SCHEMA_OPERATORS`, `SCHEMA_PRIMITIVES`. `SchemaState`: `SCHEMA_NOT_LOADED`, `SCHEMA_LOADING`, `SCHEMA_LOADED`, `SCHEMA_FAILED`. |
| `static get_changed_values(schema: Dictionary, values: Dictionary) -> Dictionary` | The values that differ from the schema's defaults. |
| `get_target() -> MeshInstance3D` | The single selected MeshInstance3D, or null. |
| `get_modifier_stack(node) -> Array`, `get_modifier_label(modifier: Dictionary) -> String` | |
| `add_modifier(node, type: String, values := {}) -> int` | `type` as Blender names it (`"BEVEL"`); returns the index. |
| `set_modifier_values(node, index: int, values: Dictionary)`, `set_modifier_enabled(node, index, enabled: bool)`, `set_modifier_object(node, index, param: String, path: NodePath)`, `move_modifier(node, index, to_index)`, `remove_modifier(node, index)` | |
| `apply_modifiers(node) -> bool`, `clear_modifiers(node)`, `evaluate_modifiers(node)`, `is_evaluating(node) -> bool`, `get_evaluation_info(node) -> Dictionary` | Info: `{evaluating, error, vertex_count, polygon_count, triangle_count}`. |
| `run_operator(node, id: String, values := {}) -> bool`, `rerun_last_operator(values: Dictionary) -> bool`, `can_rerun_last_operator() -> bool`, `is_operator_running() -> bool`, `get_last_operation() -> Dictionary` | Last operation: `{node, id, values}`. |
| `add_primitive(id: String, values := {}, parent: Node = null)`, `popup_add_primitive(id)`, `import_model(path: String, parent: Node = null)`, `popup_import_model()` | |
| `get_render_schema() -> Dictionary`, `render_current_view(settings := {}) -> bool`, `cancel_render()`, `is_rendering() -> bool`, `get_last_render() -> Image`, `get_last_render_info() -> Dictionary`, `save_render(path: String) -> Error` | Settings: `camera` (`"viewport"` or `"scene"`), `width`, `height`, `match_viewport`, `samples`, `denoise`, `transparent`. Info: `{width, height, samples, render_time, denoised}`. |

Signals: `status_changed()`, `schemas_changed(kind: int)`, `target_changed()`,
`modifiers_changed(node)`, `modifiers_evaluated(node, ok: bool)`,
`operator_started()`, `operator_finished(node, ok: bool, message: String)`,
`primitive_added(node: MeshInstance3D)`, `model_imported(node: Node3D)`,
`request_failed(action: String, message: String)`, `render_started()`,
`render_progress(fraction: float, sample: int, samples: int)`,
`render_finished(ok: bool, message: String)`.

The dock's parts, for drivers: `TetraBlenderDock.show_tab(tab)`,
`get_current_tab()`, `get_modifiers_tab()`, `get_operators_tab()`,
`get_render_tab()`; `TetraBlenderModifiersTab.select_modifier(index)`,
`get_selected_modifier()`, `popup_add_menu()`, `get_param_panel()`;
`TetraBlenderOperatorsTab.select_operator(id)`, `get_selected_operator()`,
`run()`, `get_browser()`, `get_param_panel()`;
`TetraBlenderRenderTab.render()`, `get_param_panel()`, `get_view()`.

Screenshot drivers and the UI checks live in
`tests/godot/project/tetra_model/shots/`:

```sh
scripts/screenshot.sh build/screens/model_checks.png 3D res://tetra_model/shots/checks.gd   # prints "N checks, M failed"
```

## Mesh Edit Mode (native)

Interactive polygon editing in Godot's 3D viewport, without Blender. Two
classes: `TetraEditMesh` holds the mesh and every operation (headless,
scriptable); `TetraMeshEditPlugin` is the viewport tool.

### TetraEditMesh : Resource

A polygon mesh with real topology: vertices, explicit edges (`x < y`; edges
without faces and vertices without edges are allowed) and faces of three or
more vertices with one UV per corner, a material index and a smooth flag.
Face loops are counter-clockwise seen from outside, as in Blender;
`to_array_mesh()` writes Godot's clockwise triangles. Indices are stable
until an operation changes topology.

```gdscript
var mesh := TetraEditMesh.new()
mesh.add_cube()                                   # appended and selected
mesh.set_select_mode(TetraEditMesh.SELECT_FACE)
mesh.select_none()
mesh.select_face(5)                               # Blender's cube: face 5 is the top
mesh.extrude_region()
mesh.translate_selection(mesh.get_selection_normal())
assert(mesh.validate() == "")
$MeshInstance3D.mesh = mesh.to_array_mesh()
```

| Group | Methods |
| --- | --- |
| Elements | `clear`, `get_vertex_count/get_edge_count/get_face_count`, `add_vertex(position)`, `add_edge(a, b)`, `add_face(vertices, uvs = [], material = 0, smooth = false)`, `get_vertex/set_vertex`, `get_vertex_positions/set_vertex_positions`, `get_edge_vertices(edge) -> Vector2i`, `find_edge(a, b)`, `get_edge_faces(edge)`, `get_face_vertices`, `get_face_uvs/set_face_uvs`, `get_face_material/set_face_material`, `is_face_smooth/set_face_smooth`, `get_face_normal`, `get_face_center`, `find_face(vertices)`, `materials` (Array of Material, by material index) |
| Selection | `select_mode` (`SELECT_VERTEX/EDGE/FACE`), `select_vertex/edge/face(index, selected = true)`, `is_*_selected`, `get_selected_vertices/edges/faces`, `select_all`, `select_none`, `select_invert`, `select_linked`, `select_linked_from_vertex(vertex, selected = true)`, `select_more`, `select_less`, `get_edge_loop(edge)`, `get_edge_ring(edge)`, `select_edge_loop(edge, extend = false)`, `select_edge_ring(edge, extend = false)` |
| Consistency | `validate() -> String`: empty when indices are in range, every face has at least three distinct vertices and all its edges, there are no duplicate edges or faces, and the selection is what the select mode's flushing produces |
| Conversion | `to_array_mesh() -> ArrayMesh`, `from_mesh(mesh, merge_quads = false, weld_distance = 0.00001)` |
| Storage / undo | `get_state() -> PackedByteArray`, `set_state(bytes) -> bool` (geometry, selection, select mode); the stored property `data` is the same bytes |
| Primitives | `add_plane(size)`, `add_cube(size)`, `add_grid(x_subdivisions, y_subdivisions, size)`, `add_circle(vertices, radius, fill)`, `add_cylinder(vertices, radius, depth, fill)`, `add_cone(vertices, radius1, radius2, depth, fill)`, `add_uv_sphere(segments, rings, radius)`, `add_ico_sphere(subdivisions, radius)`; `get_primitive_operations()` (static, operation schema) and `add_primitive(id, values)`. `fill` is `FILL_NOTHING/NGON/TRIANGLE_FAN` |
| Transform | `get_selection_pivot(PIVOT_MEDIAN or PIVOT_BOUNDING_BOX)`, `get_selection_normal()`, `transform_selection(transform, pivot, proportional_radius = 0)`, `translate_selection(offset, proportional_radius = 0)`, `rotate_selection(axis, angle, pivot, proportional_radius)`, `scale_selection(scale, pivot, proportional_radius)` |
| Operations | `extrude_region`, `extrude_faces_individual(offset)`, `inset_faces(thickness, depth)`, `subdivide(cuts)`, `loop_cut(edge, cuts)`, `delete_selection(DELETE_VERTICES/EDGES/FACES/ONLY_FACES/ONLY_EDGES_AND_FACES)`, `dissolve_vertices/edges/faces`, `merge_at_center`, `merge_collapse`, `merge_by_distance(distance) -> removed`, `fill`, `bridge_edge_loops`, `flip_normals`, `recalculate_normals(inside = false)`, `shade_smooth(smooth = true)`, `duplicate_selection`, `separate_selection() -> TetraEditMesh`, `triangulate`, `tris_to_quads(face_angle, shape_angle) -> joined` |

Operations act on the selection and return `false` (or `0`, or `null`) when
there was nothing to do; they leave the result selected the way Blender does.

Behaviour worth knowing:

- **Selection flushing** follows Blender. Vertex mode: an edge or face is
  selected when all its vertices are. Edge mode: vertices follow edges, a face
  needs all its edges. Face mode: vertices and edges follow faces. Switching
  vertex → edge → face keeps only what is entirely selected.
- **Primitives** use Blender's parameters and are built in Blender's Z-up
  layout, then rotated to Y-up: Blender's `(x, y, z)` is Godot's `(x, z, -y)`.
  The plane, cube (vertex and face order included), grid and circle match
  Blender index for index; the cone, cylinder and spheres have Blender's
  topology and counts, with the poles/apex placed first or last rather than
  where Blender's remove-doubles pass leaves them. Cube faces each get the
  whole 0..1 UV square, not Blender's cross layout.
- **Extrude region**: side faces along the region's border, wound outwards.
  The original faces are removed when the region borders other faces; a lone
  face keeps its original as the flipped bottom (a plane becomes a closed
  box); a lone region with interior edges loses them (a grid becomes an open
  box). Selected edges without selected faces become faces, lone vertices
  become edges. A region without a border (a whole closed mesh) does nothing.
- **Inset** is the region inset with even offset; `depth` moves the inner
  faces along the region's vertex normals.
- **Subdivide**: a quad with all four edges cut becomes a grid, a triangle
  with all three a triangle grid, a quad with two opposite edges a ladder
  (this is what `loop_cut` uses). Any other pattern inserts the new vertices
  into the face, which becomes an n-gon — Blender would add corner and fan
  patterns there. There is no smoothness parameter.
- **Dissolve**: faces are joined when their outline is one simple loop
  (a ring of faces around a hole is left alone). Dissolving edges also removes
  end vertices left between two edges (Blender's default).
- **Fill** makes a face along a closed selected edge loop, wound against its
  neighbours; without a loop, one face through the selected vertices ordered
  by angle; with two vertices, an edge. It is not Blender's grid or beauty fill.
- **Bridge edge loops** needs exactly two closed loops of equal length.
- **Recalculate normals** makes each island's winding consistent and then
  outward by the sign of its volume about its centre, so it suits closed or
  nearly closed islands; flat open sheets keep the orientation of their first
  face.
- `from_mesh` welds vertices by position, marks faces smooth when their vertex
  normals differ from the face normal, and with `merge_quads` joins coplanar
  triangle pairs that share UVs across the diagonal (best-shaped pairs first).
  Tangents, vertex colours, UV2, bones and blend shapes are not carried.
- `to_array_mesh` writes one surface per used material index (so surface
  index and material index differ when an index is unused), splits vertices
  of flat faces and shares those of smooth faces that have the same UV.
- Bevel, knife, rip, vertex/edge slide, proportional editing in the viewport
  tool and individual-origins pivots are not implemented here; use the Blender
  operators tab for bevel.

### Edit Mode in the 3D viewport

Select a `MeshInstance3D` and press **Tab** in the 3D viewport, or use the
**Edit Mode** button in the 3D toolbar. The first time, the node's mesh is
converted (welded, coplanar triangle pairs joined into quads) into a
`TetraEditMesh` stored in the node's metadata `tetra_edit_mesh` — this
conversion is an undo step. From then on the node's `mesh` is the `ArrayMesh`
generated from it; both are saved with the scene, and entering again is
lossless. Leaving (Tab, the button, or selecting something else) keeps the
result.

| Input | Action |
| --- | --- |
| Tab | Enter / leave Edit Mode |
| 1 / 2 / 3, or the Vertex / Edge / Face buttons | Select mode |
| Click, Shift+click | Select; extend or toggle |
| Drag from empty space | Box select (Shift extends); a click on empty space deselects |
| Alt+click | Edge loop |
| A / Alt+A / Ctrl+I | All / none / invert |
| L / Shift+L | Select / deselect what is linked to the element under the cursor |
| Ctrl+Numpad + / − (also Ctrl+= / Ctrl+-) | More / less |
| G / R / S | Move / rotate / scale. X, Y, Z lock to an axis, Shift+X/Y/Z to the plane across it (the same key again unlocks); digits, `.`, `-` and Backspace type an exact value; Ctrl snaps to the 3D editor's snap steps; Enter or left click confirms, Esc or right click cancels |
| E | Extrude region, then move along the normal |
| I | Inset faces: drag towards the centre for the thickness, or type it |
| Ctrl+R | Loop cut: a preview follows the edge under the cursor; the wheel or digits set the number of cuts; click confirms |
| X or Delete | Delete menu: Vertices, Edges, Faces, Only Faces, Only Edges & Faces, Dissolve Vertices/Edges/Faces |
| M | Merge menu: at Center, Collapse, by Distance |
| F | New edge/face from the selection |
| Shift+D | Duplicate, then move |
| Shift+N | Recalculate normals outside |

Ctrl+B is not bound (bevel is a Blender operator). The **Select** and **Mesh**
menus in the 3D toolbar list every operation; entries ending in "..." open a
`TetraOperationDialog` with a live preview. **Add > Mesh** (always in the 3D
toolbar) creates a `MeshInstance3D` with an editable primitive, or adds the
primitive to the mesh being edited.

How it sits in Godot's 3D editor:

- While in Edit Mode the plugin consumes the keys and mouse buttons above
  (`AFTER_GUI_INPUT_STOP` plus `set_input_as_handled`), so Godot's own
  E / R / F / M / Delete / Escape shortcuts do not fire; middle button, wheel
  and right-button freelook still navigate. Godot's transform gizmo and the
  node's selection box are hidden for the node being edited.
- With "Emulate Numpad" on, the viewport rewrites the digit keys to numpad
  keys before plugins see them; the plugin reads the physical key instead.
- The overlay is real 3D geometry on the editor's gizmo layer (black wire and
  points, orange selection, as in Blender): edge lines, vertex points in
  vertex mode, face-centre dots in face mode and a translucent fill on
  selected faces, drawn with a small view-space pull towards the camera. While
  a G/R/S drag runs only its position buffers are rewritten
  (`RenderingServer.mesh_surface_update_vertex_region`). Box select, the loop
  cut preview and the readout are drawn on the 2D overlay.
- Only elements that are not hidden behind the mesh can be picked or box
  selected. Picking is a linear search with a ray test per candidate: fine for
  meshes of a few thousand faces, slow beyond that.
- Every operation, and every confirmed modal tool (E and Shift+D together
  with their move), is one `EditorUndoRedoManager` action that swaps snapshots
  (`get_state()`), also when the node is no longer in Edit Mode. Selection
  changes alone are not undo steps.
- **With the Blender dock**: operators and modifiers keep working on such a
  node — they replace `mesh` as before. The edit mesh is then out of date, and
  the plugin refreshes it from what the node shows: at once when the node is
  in Edit Mode, otherwise the next time Edit Mode is entered (it compares
  surface sizes and bounds). The refresh is a new conversion, so n-gons come
  back as the triangles/quads Blender produced. With a live modifier stack,
  Edit Mode edits the evaluated result, not the source mesh: apply the
  modifiers first.

```gdscript
var edit := TetraMeshEditPlugin.get_singleton()
var node := edit.add_mesh("cube", {"size": 2.0})        # Add > Mesh
edit.enter_edit_mode(node)
edit.set_select_mode(TetraEditMesh.SELECT_FACE)
edit.get_edit_mesh().select_face(5)
edit.refresh()                                          # after changing the mesh or selection directly
edit.run_operation("extrude")                           # one undo action each
edit.run_operation("translate", {"offset": Vector3(0, 1, 0)})
edit.exit_edit_mode()
```

`TetraMeshEditPlugin`: `get_singleton()` (static), `enter_edit_mode(node) -> bool`,
`exit_edit_mode()`, `is_edit_mode()`, `get_edited_node()`, `get_edit_mesh()`,
`set_select_mode(mode)` / `get_select_mode()`, `get_operations()` (operation
schema; ids such as `extrude`, `inset_faces`, `loop_cut` with `{edge, cuts}`,
`subdivide`, `delete_faces`, `merge_center`, `add:<primitive>`),
`run_operation(id, values = {}) -> bool`, `request_operation(id)` (what the
menu does: run, or open the dialog), `get_operation_dialog()`,
`start_modal(MODAL_GRAB/ROTATE/SCALE/INSET/LOOP_CUT)`, `get_modal()`,
`get_modal_readout()`, `add_mesh(primitive, values = {}) -> MeshInstance3D`,
`pick(camera, position) -> {vertex, edge, face}`, `get_overlay_counts()`,
`refresh()`.

## Testing

```sh
build/blender-venv/bin/python modules/tetra_model/bridge/test_server.py   # the server alone
scripts/test.sh tetra_model                                               # the C++ classes, headless
```

`test_edit_mesh.gd` and `test_edit_mesh_ops.gd` cover `TetraEditMesh` (exact
counts and positions, `validate()` after every operation). Edit Mode itself
needs the running editor:

```sh
scripts/screenshot.sh build/screens/editmode-extruded.png 3D res://tetra_model/shots/edit_mode.gd
```

prints `screenshot edit mode checks: <n> checks, <m> failed`; it drives the
plugin through its scripting surface and through simulated mouse and key
events, checks undo/redo in the editor's history, and also writes
`build/screens/editmode-vertex.png`, `editmode-face.png`, `editmode-loopcut.png`
and `editmode-dialog.png`.

`test_blender_bridge.gd` is skipped with a message when no launcher is found.
It writes `build/screens/blender-render.png`. `test_server.py
--dry-run-operators` runs every listed operator once with default values and
prints the ones that fail; that is how the deny lists in the server are kept
honest when Blender is updated.

## Known limits

- Launching a real Blender executable is implemented and covered by a test
  with a stand-in executable, but has not been run against an actual
  `blender` binary: none is installed on the development machine.
- Process handling is written against the POSIX behaviour of
  `OS::execute_with_pipe` (non-blocking pipes); only Linux has been tested.
- One Blender process serves one command at a time; a long render blocks
  other bridge calls until it finishes or `stop()` is called.
- Only the active UV layer and the plain values of the Principled BSDF travel
  with a mesh. Vertex colours, vertex groups, shape keys and textures do not.
- `run_operators` writes back the object it loaded; operators that create
  other objects are not listed.

## Recovery and stale results

- A job ended by `stop()` (a cancelled render, "Restart Blender") carries
  `stopped: true`. The workspace does not treat it as a failed start: a
  cancelled render reports "The render was cancelled.", and a modifier
  evaluation or schema list that waited behind it is sent again.
- After a failed start the workspace retries the failed schema lists and
  evaluations when the editor settings name another launcher, or through
  "Restart Blender", which stays enabled while a start error is shown.
- `apply_modifiers()` (Apply All) refuses a stack whose last evaluation failed.
- An object parameter of a modifier is watched (twice a second) for the stacks
  evaluated in this editor session: when the object moves or gets another
  mesh, the stack is evaluated again. A path that no longer leads to another
  MeshInstance3D with a mesh is an evaluation error
  (`TetraBlenderScene.find_object_problem(node, stack) -> String`). Stacks of a
  freshly opened scene are not watched until they are evaluated once, and a
  mesh resource edited in place is not noticed.
- Exchange directories (`exchange-<pid>-<id>` in the cache) that have not
  changed for a day are removed when a bridge creates its own: they belong to
  editors that did not exit normally.
