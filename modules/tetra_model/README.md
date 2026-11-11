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

There is no editor UI in this module yet; everything below is in ClassDB and
is what a UI codes against.

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

## Testing

```sh
build/blender-venv/bin/python modules/tetra_model/bridge/test_server.py   # the server alone
scripts/test.sh tetra_model                                               # the C++ classes, headless
```

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
