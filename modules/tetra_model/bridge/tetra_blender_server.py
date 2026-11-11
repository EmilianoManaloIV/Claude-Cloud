"""Tetra Studio Blender bridge server.

Runs inside Blender's Python, either as ``python tetra_blender_server.py`` with
the ``bpy`` module installed, or as
``blender --background --factory-startup --python tetra_blender_server.py``.
Arguments after ``--`` (or all arguments when run by plain Python):
``--temp-dir DIR`` uses DIR for exchange files instead of a fresh directory.

Protocol
--------
JSON Lines. The client writes one request object per line on stdin::

    {"id": 7, "cmd": "apply_modifiers", ...}

and the server answers every request with exactly one line on the protocol
channel::

    {"id": 7, "ok": true, ...}
    {"id": 7, "ok": false, "error": "...", "traceback": "..."}

Unsolicited event lines carry ``"event"`` instead of ``"ok"``::

    {"event": "ready", ...}                       once, when requests are accepted
    {"event": "progress", "id": 7, "cmd": "render", "stage": "sample", ...}

The protocol channel is a duplicate of the stdout the process was started
with. File descriptor 1 itself is redirected to stderr before Blender is
imported, so nothing Blender or an add-on prints can corrupt the protocol.
A client must still ignore lines that do not start with ``{``: a Blender
executable prints its banner before this script runs.

Bulk data never travels through JSON. Meshes are exchanged as files in the
session directory named by ``hello`` (any other path works too).

Coordinate conventions
----------------------
Everything in the protocol is in Blender's conventions: Z up, counter-clockwise
front faces, UV origin at the bottom left, matrices as 16 floats in row-major
order (``m[row * 4 + column]``), angles in radians inside matrices. The client
converts (Tetra does it in ``TetraBlenderMesh``). The one exception is
operator and modifier parameters whose RNA unit is an angle: the schemas
present them in degrees and this server converts them to radians.

Mesh exchange file (``.tmsh``), little-endian
---------------------------------------------
::

    char[4]  magic "TMSH"
    uint32   version (1)
    uint32   header_length            bytes of JSON that follow, padded with
                                      spaces to a multiple of 4
    char[]   header JSON (UTF-8):
               name            string, optional
               vertex_count    V
               polygon_count   P
               corner_count    C      sum of the polygon sizes
               triangle_count  T      0 when no triangulation is stored
               has_uvs         bool
               has_normals     bool
               materials       [string]   material slot names
    float32  positions[3 * V]
    int32    polygon_loop_start[P]    first corner of each polygon; polygon i
                                      owns corners loop_start[i] up to
                                      loop_start[i + 1] (C for the last one)
    int32    corner_vertices[C]       vertex index of every corner
    int32    polygon_material[P]      material slot index
    float32  corner_uvs[2 * C]        only if has_uvs
    float32  corner_normals[3 * C]    only if has_normals
    int32    triangle_corners[3 * T]  only if T > 0: a triangulation of the
                                      polygons, as corner indices
    int32    triangle_polygons[T]     only if T > 0: polygon of each triangle

Files written by the server always carry normals and a triangulation. In
files read by the server, normals are optional and only decide shading: an
edge is marked sharp where the normals of the faces on its two sides differ,
every face is smooth. A stored triangulation is ignored on input.
"""

import json
import math
import os
import shutil
import struct
import sys
import tempfile
import time
import traceback
import warnings

# The protocol channel must be taken before Blender gets a chance to print.
_protocol_fd = os.dup(1)
os.dup2(2, 1)
_protocol = os.fdopen(_protocol_fd, "w", encoding="utf-8", newline="\n")
sys.stdout = sys.stderr

import bpy  # noqa: E402
import numpy as np  # noqa: E402

PROTOCOL_VERSION = 1
MESH_MAGIC = b"TMSH"
MESH_VERSION = 1

_state = {
    "temp_dir": "",
    "owns_temp_dir": False,
    "request_id": None,
    "command": "",
    "operator_modes": {},
}


def _send(message):
    _protocol.write(json.dumps(message, separators=(",", ":"), allow_nan=False) + "\n")
    _protocol.flush()


def _event(name, **fields):
    message = {"event": name, "id": _state["request_id"], "cmd": _state["command"]}
    message.update(fields)
    try:
        _send(message)
    except (ValueError, OSError):
        pass


def _log(*parts):
    print("[tetra]", *parts, file=sys.stderr, flush=True)


class BridgeError(Exception):
    """An error caused by the request, reported without a traceback."""


# ---------------------------------------------------------------------------
# Mesh exchange files


def _read_mesh_file(path):
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < 12 or blob[:4] != MESH_MAGIC:
        raise BridgeError("Not a Tetra mesh exchange file: %s" % path)
    version, header_length = struct.unpack_from("<II", blob, 4)
    if version != MESH_VERSION:
        raise BridgeError("Unsupported mesh exchange version %d in %s" % (version, path))
    header = json.loads(blob[12:12 + header_length].decode("utf-8"))
    offset = 12 + header_length
    vertex_count = int(header["vertex_count"])
    polygon_count = int(header["polygon_count"])
    corner_count = int(header["corner_count"])

    def take(dtype, count):
        nonlocal offset
        size = count * 4
        if offset + size > len(blob):
            raise BridgeError("Truncated mesh exchange file: %s" % path)
        array = np.frombuffer(blob, dtype=dtype, count=count, offset=offset)
        offset += size
        return array

    data = {
        "header": header,
        "positions": take("<f4", vertex_count * 3),
        "loop_start": take("<i4", polygon_count),
        "corner_vertices": take("<i4", corner_count),
        "polygon_material": take("<i4", polygon_count),
        "uvs": take("<f4", corner_count * 2) if header.get("has_uvs") else None,
        "normals": take("<f4", corner_count * 3) if header.get("has_normals") else None,
    }
    loop_start = data["loop_start"]
    if polygon_count:
        sizes = np.diff(np.append(loop_start, corner_count))
        if loop_start[0] != 0 or (sizes < 3).any():
            raise BridgeError("Invalid polygon table in %s" % path)
        data["loop_total"] = sizes.astype(np.int32)
    else:
        data["loop_total"] = np.zeros(0, dtype=np.int32)
    if corner_count and (data["corner_vertices"].min() < 0 or data["corner_vertices"].max() >= vertex_count):
        raise BridgeError("Corner vertex index out of range in %s" % path)
    return data


def _mark_sharp_edges(mesh, data):
    """Mark the edges across which the stored corner normals differ as sharp."""
    corner_count = len(mesh.loops)
    edge_count = len(mesh.edges)
    if not corner_count or not edge_count:
        return
    normals = data["normals"].reshape(-1, 3).astype(np.float64)
    loop_start = data["loop_start"].astype(np.int64)
    loop_total = data["loop_total"].astype(np.int64)
    corner_vertices = data["corner_vertices"].astype(np.int64)
    corner_edges = np.empty(corner_count, dtype=np.int32)
    mesh.loops.foreach_get("edge_index", corner_edges)
    corner_edges = corner_edges.astype(np.int64)

    following = np.arange(corner_count, dtype=np.int64) + 1
    following[loop_start + loop_total - 1] = loop_start

    # Each corner's edge runs from its own vertex to the following corner's
    # vertex; the face contributes its normal at both ends. Grouping the
    # contributions by (edge, vertex) puts the two sides of an edge next to
    # each other.
    vertex_count = len(mesh.vertices)
    keys = np.concatenate((corner_edges * vertex_count + corner_vertices, corner_edges * vertex_count + corner_vertices[following]))
    sides = np.concatenate((normals, normals[following]))
    order = np.argsort(keys, kind="stable")
    keys = keys[order]
    sides = sides[order]
    same = keys[1:] == keys[:-1]
    differs = np.einsum("ij,ij->i", sides[1:], sides[:-1]) < 0.9999
    sharp = np.zeros(edge_count, dtype=bool)
    sharp[(keys[1:] // vertex_count)[same & differs]] = True
    mesh.edges.foreach_set("use_edge_sharp", sharp)
    if hasattr(mesh, "use_auto_smooth"):
        mesh.use_auto_smooth = True
        mesh.auto_smooth_angle = math.pi


def _material_named(name, cache):
    key = name or "Material"
    material = cache.get(key)
    if material is None:
        material = bpy.data.materials.new(key)
        cache[key] = material
    return material


def _mesh_from_file(path, name=None):
    """Build a Mesh datablock from an exchange file."""
    data = _read_mesh_file(path)
    header = data["header"]
    mesh = bpy.data.meshes.new(name or header.get("name") or "TetraMesh")
    vertex_count = int(header["vertex_count"])
    polygon_count = int(header["polygon_count"])
    corner_count = int(header["corner_count"])
    mesh.vertices.add(vertex_count)
    mesh.vertices.foreach_set("co", data["positions"])
    mesh.loops.add(corner_count)
    mesh.loops.foreach_set("vertex_index", data["corner_vertices"])
    mesh.polygons.add(polygon_count)
    mesh.polygons.foreach_set("loop_start", data["loop_start"])
    try:
        mesh.polygons.foreach_set("loop_total", data["loop_total"])
    except (AttributeError, RuntimeError, TypeError):
        # Derived from loop_start in current Blender versions.
        pass
    mesh.polygons.foreach_set("material_index", data["polygon_material"])
    mesh.polygons.foreach_set("use_smooth", np.full(polygon_count, data["normals"] is not None, dtype=bool))

    cache = {}
    for material_name in header.get("materials", []):
        mesh.materials.append(_material_named(material_name, cache))

    mesh.update(calc_edges=True)
    if data["uvs"] is not None and corner_count:
        layer = mesh.uv_layers.new(name="UVMap")
        if hasattr(layer, "uv"):
            layer.uv.foreach_set("vector", data["uvs"])
        else:
            layer.data.foreach_set("uv", data["uvs"])
    if data["normals"] is not None:
        _mark_sharp_edges(mesh, data)
    mesh.validate(verbose=False)
    mesh.update()
    return mesh


def _corner_normals(mesh, corner_count):
    normals = np.empty(corner_count * 3, dtype=np.float32)
    if hasattr(mesh, "corner_normals"):
        mesh.corner_normals.foreach_get("vector", normals)
    else:
        mesh.calc_normals_split()
        mesh.loops.foreach_get("normal", normals)
    return normals


def _write_mesh_file(path, mesh, name=None, material_names=None):
    """Write a Mesh datablock, with corner normals and a triangulation."""
    vertex_count = len(mesh.vertices)
    polygon_count = len(mesh.polygons)
    corner_count = len(mesh.loops)

    positions = np.empty(vertex_count * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    loop_start = np.empty(polygon_count, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", loop_start)
    corner_vertices = np.empty(corner_count, dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_vertices)
    polygon_material = np.empty(polygon_count, dtype=np.int32)
    mesh.polygons.foreach_get("material_index", polygon_material)

    uvs = None
    layer = mesh.uv_layers.active
    if layer is not None and corner_count:
        uvs = np.empty(corner_count * 2, dtype=np.float32)
        if hasattr(layer, "uv"):
            layer.uv.foreach_get("vector", uvs)
        else:
            layer.data.foreach_get("uv", uvs)

    normals = _corner_normals(mesh, corner_count)

    mesh.calc_loop_triangles()
    triangle_count = len(mesh.loop_triangles)
    triangle_corners = np.empty(triangle_count * 3, dtype=np.int32)
    mesh.loop_triangles.foreach_get("loops", triangle_corners)
    triangle_polygons = np.empty(triangle_count, dtype=np.int32)
    mesh.loop_triangles.foreach_get("polygon_index", triangle_polygons)

    if material_names is None:
        material_names = [m.name if m is not None else "" for m in mesh.materials]
    header = {
        "name": name or mesh.name,
        "vertex_count": vertex_count,
        "polygon_count": polygon_count,
        "corner_count": corner_count,
        "triangle_count": triangle_count,
        "has_uvs": uvs is not None,
        "has_normals": True,
        "materials": material_names,
    }
    header_bytes = json.dumps(header).encode("utf-8")
    header_bytes += b" " * (-len(header_bytes) % 4)
    with open(path, "wb") as handle:
        handle.write(MESH_MAGIC)
        handle.write(struct.pack("<II", MESH_VERSION, len(header_bytes)))
        handle.write(header_bytes)
        handle.write(positions.astype("<f4").tobytes())
        handle.write(loop_start.astype("<i4").tobytes())
        handle.write(corner_vertices.astype("<i4").tobytes())
        handle.write(polygon_material.astype("<i4").tobytes())
        if uvs is not None:
            handle.write(uvs.astype("<f4").tobytes())
        handle.write(normals.astype("<f4").tobytes())
        if triangle_count:
            handle.write(triangle_corners.astype("<i4").tobytes())
            handle.write(triangle_polygons.astype("<i4").tobytes())
    return {
        "vertex_count": vertex_count,
        "polygon_count": polygon_count,
        "triangle_count": triangle_count,
    }


# ---------------------------------------------------------------------------
# Scene helpers


def _reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def _matrix(values):
    from mathutils import Matrix

    if values is None:
        return Matrix.Identity(4)
    values = [float(v) for v in values]
    if len(values) != 16:
        raise BridgeError("A transform needs 16 numbers, got %d" % len(values))
    return Matrix([values[0:4], values[4:8], values[8:12], values[12:16]])


def _matrix_list(matrix):
    return [float(matrix[row][column]) for row in range(4) for column in range(4)]


def _link_object(name, data):
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _activate(obj):
    for other in bpy.context.view_layer.objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def _mesh_object_from_file(path, name="TetraMesh", transform=None):
    obj = _link_object(name, _mesh_from_file(path, name))
    obj.matrix_world = _matrix(transform)
    return obj


def _context_override(obj):
    """Context members operators look for, valid without a 3D viewport."""
    override = {
        "object": obj,
        "active_object": obj,
        "selected_objects": [obj],
        "selected_editable_objects": [obj],
    }
    windows = bpy.context.window_manager.windows
    if len(windows):
        override["window"] = windows[0]
        override["screen"] = windows[0].screen
    return override


def _set_mode(obj, mode):
    if obj.mode != mode:
        with bpy.context.temp_override(**_context_override(obj)):
            bpy.ops.object.mode_set(mode=mode)


def _write_evaluated(obj, output):
    """Write obj with its modifier stack evaluated."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        names = [slot.material.name if slot.material is not None else "" for slot in evaluated.material_slots]
        return _write_mesh_file(output, mesh, name=obj.name, material_names=names)
    finally:
        evaluated.to_mesh_clear()


def _require(request, key):
    if key not in request or request[key] in (None, ""):
        raise BridgeError("Missing '%s' in '%s' request" % (key, request.get("cmd", "")))
    return request[key]


# ---------------------------------------------------------------------------
# RNA introspection -> operation schemas

_INT_LIMIT = 2 ** 31 - 1
_FLOAT_LIMIT = 1e30

_UNIT_SUFFIX = {
    "LENGTH": "m",
    "AREA": "m²",
    "VOLUME": "m³",
    "ROTATION": "°",
    "TIME": "frames",
    "TIME_ABSOLUTE": "s",
    "VELOCITY": "m/s",
    "ACCELERATION": "m/s²",
    "MASS": "kg",
    "CAMERA": "mm",
    "POWER": "W",
    "TEMPERATURE": "K",
    "WAVELENGTH": "nm",
    "FREQUENCY": "Hz",
}

_MODULE_CATEGORY = {
    "mesh": "Mesh",
    "uv": "UV",
    "object": "Object",
    "transform": "Transform",
    "geometry": "Geometry",
    "paint": "Paint",
    "sculpt": "Sculpt",
}


def _is_angle(prop):
    return prop.type == "FLOAT" and getattr(prop, "unit", "NONE") == "ROTATION"


def _finite(value):
    value = float(value)
    return value if math.isfinite(value) else 0.0


def _title(identifier):
    return identifier.replace("_", " ").strip().title()


def _join_numbers(values):
    flat = []

    def walk(item):
        if hasattr(item, "__len__") and not isinstance(item, str):
            for child in item:
                walk(child)
        elif isinstance(item, bool):
            flat.append("1" if item else "0")
        elif isinstance(item, int):
            flat.append(str(item))
        else:
            flat.append(repr(round(_finite(item), 6)))

    walk(values)
    return ",".join(flat)


def _param_schema(prop, current=None):
    """One schema parameter for an RNA property, or None if it has no plain value.

    current, when given, is the property's value on a live instance and is
    used as the default instead of the RNA default (the two can differ).
    """
    kind = prop.type
    if kind in ("POINTER", "COLLECTION"):
        return None
    param = {
        "name": prop.identifier,
        "label": prop.name or _title(prop.identifier),
    }
    if prop.description:
        param["description"] = prop.description
    array_length = getattr(prop, "array_length", 0) if kind in ("BOOLEAN", "INT", "FLOAT") else 0
    angle = _is_angle(prop)
    scale = 180.0 / math.pi if angle else 1.0

    def default_value():
        if current is not None:
            return current
        if array_length:
            return list(prop.default_array)
        return prop.default

    if kind in ("INT", "FLOAT"):
        limit = _INT_LIMIT if kind == "INT" else _FLOAT_LIMIT
        convert = int if kind == "INT" else (lambda v: _finite(v) * scale)
        if abs(prop.hard_min) < limit:
            param["min"] = convert(prop.hard_min)
        if abs(prop.hard_max) < limit:
            param["max"] = convert(prop.hard_max)
        if abs(prop.soft_min) < limit and prop.soft_min > prop.hard_min:
            param["ui_min"] = convert(prop.soft_min)
        if abs(prop.soft_max) < limit and prop.soft_max < prop.hard_max:
            param["ui_max"] = convert(prop.soft_max)
        if kind == "INT":
            param["step"] = max(1, int(prop.step))
        else:
            # RNA stores the float UI step in hundredths; angles step in whole degrees.
            param["step"] = 1.0 if angle else max(_finite(prop.step) / 100.0, 10.0 ** -max(int(prop.precision), 1))
        unit = _UNIT_SUFFIX.get(getattr(prop, "unit", "NONE"))
        if prop.subtype == "PERCENTAGE":
            unit = "%"
        elif prop.subtype == "PIXEL":
            unit = "px"
        if unit:
            param["unit"] = unit

    if kind == "BOOLEAN" and not array_length:
        param["type"] = "bool"
        param["default"] = bool(default_value())
    elif kind == "INT" and not array_length:
        param["type"] = "seed" if prop.identifier == "seed" or prop.identifier.endswith("_seed") else "int"
        param["default"] = int(default_value())
    elif kind == "FLOAT" and not array_length:
        param["type"] = "float"
        param["default"] = _finite(default_value()) * scale
    elif kind == "FLOAT" and array_length in (2, 3, 4) and len(getattr(prop, "array_dimensions", [0, 0, 0])) and prop.array_dimensions[1] == 0:
        values = [_finite(v) * scale for v in default_value()]
        if prop.subtype in ("COLOR", "COLOR_GAMMA") and array_length in (3, 4):
            param["type"] = "color"
            param["default"] = values
            param.pop("step", None)
        elif array_length == 2:
            param["type"] = "vec2"
            param["default"] = values
        elif array_length == 3:
            param["type"] = "vec3"
            param["default"] = values
        else:
            param["type"] = "string"
            param["default"] = _join_numbers(values)
            param["array_length"] = array_length
    elif array_length:
        values = default_value()
        if angle:
            values = [_finite(v) * scale for v in np.array(values, dtype=float).ravel()]
        param["type"] = "string"
        param["default"] = _join_numbers(values)
        param["array_length"] = array_length
        for key in ("min", "max", "ui_min", "ui_max", "step", "unit"):
            param.pop(key, None)
    elif kind == "ENUM":
        items = [item for item in prop.enum_items if item.identifier]
        if prop.is_enum_flag:
            default = current if current is not None else prop.default_flag
            param["type"] = "string"
            param["default"] = ",".join(sorted(default))
            param["flags"] = [{"value": item.identifier, "label": item.name} for item in items]
        elif items:
            param["type"] = "enum"
            param["default"] = current if current is not None else prop.default
            param["options"] = [{"value": item.identifier, "label": item.name or item.identifier} for item in items]
            if param["default"] not in [item.identifier for item in items]:
                param["default"] = items[0].identifier
        else:
            # Items are computed from the context at run time; accept any identifier.
            param["type"] = "string"
            param["default"] = current if isinstance(current, str) else (prop.default or "")
    elif kind == "STRING":
        param["type"] = "file" if prop.subtype in ("FILE_PATH", "DIR_PATH", "FILE_NAME") else "string"
        value = default_value()
        param["default"] = value if isinstance(value, str) else ""
    else:
        return None
    return param


def _coerce(prop, value):
    """Convert a JSON value to what RNA accepts for prop."""
    kind = prop.type
    array_length = getattr(prop, "array_length", 0) if kind in ("BOOLEAN", "INT", "FLOAT") else 0
    scale = math.pi / 180.0 if _is_angle(prop) else 1.0

    def scalar(item):
        if kind == "BOOLEAN":
            if isinstance(item, str):
                return item.strip().lower() in ("1", "true", "yes", "on")
            return bool(item)
        if kind == "INT":
            return int(round(float(item)))
        return float(item) * scale

    if array_length:
        if isinstance(value, str):
            value = [part for part in value.replace(";", ",").split(",") if part.strip()]
        elif isinstance(value, dict):
            value = [value[key] for key in ("x", "y", "z", "w") if key in value] or [value[key] for key in ("r", "g", "b", "a") if key in value]
        values = [scalar(item) for item in value]
        if len(values) > array_length:
            values = values[:array_length]
        if len(values) != array_length:
            raise BridgeError("'%s' needs %d numbers, got %d" % (prop.identifier, array_length, len(values)))
        dimensions = [d for d in getattr(prop, "array_dimensions", []) if d]
        if len(dimensions) == 2:
            rows, columns = dimensions
            return [values[row * columns:(row + 1) * columns] for row in range(rows)]
        return values
    if kind in ("BOOLEAN", "INT", "FLOAT"):
        return scalar(value)
    if kind == "ENUM":
        if prop.is_enum_flag:
            if isinstance(value, str):
                return {part.strip() for part in value.split(",") if part.strip()}
            return {str(part) for part in value}
        return str(value)
    if kind == "STRING":
        return str(value)
    raise BridgeError("'%s' cannot be set from a plain value" % prop.identifier)


def _coerce_values(properties, values, skipped):
    """Map a request's value dictionary onto RNA properties; unknown names go to skipped."""
    result = {}
    for name, value in (values or {}).items():
        prop = properties.get(name)
        if prop is None or prop.type in ("POINTER", "COLLECTION") or name == "rna_type":
            skipped.append(name)
            continue
        result[name] = _coerce(prop, value)
    return result


# Operators that pass their poll without a viewport but still cannot do
# anything useful there: they only start a modal interaction, manage the
# editor's own state, or act on things outside the one mesh being processed.
_OPERATOR_DENY_EXACT = {
    "object.mode_set",
    "object.mode_set_with_submode",
    "object.editmode_toggle",
    "object.posemode_toggle",
    "object.delete",
    "object.join",
    "object.join_shapes",
    "object.join_uvs",
    "object.duplicate",
    "object.duplicate_move",
    "object.duplicate_move_linked",
    "object.duplicates_make_real",
    "object.make_local",
    "object.make_single_user",
    "object.make_override_library",
    "object.unlink_data",
    "object.transfer_mode",
    "object.paths_calculate",
    "object.paths_update",
    "object.paths_update_visible",
    "object.paths_clear",
    "object.bake",
    "object.bake_image",
    "object.ocean_bake",
    "object.multires_external_save",
    "object.multires_external_pack",
    "object.simulation_nodes_cache_bake",
    "object.simulation_nodes_cache_calculate_to_frame",
    "object.simulation_nodes_cache_delete",
    "object.geometry_node_bake_single",
    "object.geometry_node_bake_delete_single",
    "object.geometry_node_bake_pack_single",
    "object.geometry_node_bake_unpack_single",
    "object.quick_explode",
    "object.quick_fur",
    "object.quick_liquid",
    "object.quick_smoke",
    "object.add",
    "object.add_named",
    "object.instance_offset_from_cursor",
    "object.instance_offset_from_object",
    "object.instance_offset_to_cursor",
    "object.isolate_type_render",
    "object.subdivision_set",
    "object.hide_render_clear_all",
    "mesh.separate",
    "mesh.knife_project",
    "mesh.select_mode",
    "mesh.reveal",
    "mesh.hide",
    "uv.reveal",
    "uv.hide",
    "uv.export_layout",
    "transform.create_orientation",
    "transform.delete_orientation",
    "transform.select_orientation",
    "transform.from_gizmo",
    "transform.transform",
}

_OPERATOR_DENY_PARTS = (
    "gizmo",
    "modal",
    "drop_",
    "_drag",
    "cursor",
    "pick",
    "_walk",
    "paste",
    "copy",
    "_menu",
    "clipboard",
    "preview",
)

_OBJECT_DENY_PARTS = (
    "_add",
    "add_",
    "select",
    "link",
    "parent",
    "track",
    "constraint",
    "collection",
    "hide_",
    "hook_",
    "pose",
    "particle",
    "effector",
    "forcefield",
    "collision",
    "rigidbody",
    "light",
    "lineart",
    "grease",
    "gpencil",
    "curves",
    "volume",
    "pointcloud",
    "speaker",
    "camera",
    "empty",
    "armature",
    "metaball",
    "text_",
    "shaderfx",
    "skin_",
    "surfacedeform",
    "meshdeform",
    "laplaciandeform",
    "correctivesmooth",
    "explode_refresh",
    "ocean",
    "multires",
    "modifier",
    "material_slot",
    "geometry_node",
    "bake",
    "anim",
    "nla",
    "driver",
    "keyframe",
    "visual_",
    "lod_",
    "game_",
    "override",
    "asset",
    "datalayout",
    "data_transfer",
    "move_to",
    "location_clear",
    "rotation_clear",
    "scale_clear",
    "origin_clear",
    "visual_transform",
    "shape_key",
    "vertex_weight",
    "vertex_parent",
    "face_map",
    "type_",
    "update_shapes",
    "unlink",
    "reset_override",
    "resolve",
)


def _operator_candidates(prefixes):
    for module_name in dir(bpy.ops):
        module = getattr(bpy.ops, module_name)
        for name in dir(module):
            idname = module_name + "." + name
            if not any(idname.startswith(prefix) for prefix in prefixes):
                continue
            if idname in _OPERATOR_DENY_EXACT or any(part in name for part in _OPERATOR_DENY_PARTS):
                continue
            if module_name == "object" and any(part in name for part in _OBJECT_DENY_PARTS):
                continue
            if module_name == "mesh" and name.startswith("primitive_"):
                continue
            operator = getattr(module, name)
            try:
                options = operator.bl_options
            except Exception:
                continue
            if "INTERNAL" in options:
                continue
            yield idname, module_name, operator


def _classify_operators(candidates):
    """Decide the mode each operator runs in by polling it on a scratch mesh.

    Poll functions are how Blender itself says "this needs a 3D viewport" or
    "this needs edit mode", so an operator that fails in both modes without a
    viewport is left out.
    """
    _reset_scene()
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.view_layer.objects.active
    polled = {"OBJECT": set(), "EDIT": set()}
    for mode in ("OBJECT", "EDIT"):
        _set_mode(obj, mode)
        if mode == "EDIT":
            bpy.ops.mesh.select_all(action="SELECT")
        with bpy.context.temp_override(**_context_override(obj)):
            for idname, _module_name, operator in candidates:
                try:
                    if operator.poll():
                        polled[mode].add(idname)
                except Exception:
                    pass
    _set_mode(obj, "OBJECT")
    modes = {}
    for idname, module_name, _operator in candidates:
        preferred = ("OBJECT", "EDIT") if module_name == "object" else ("EDIT", "OBJECT")
        for mode in preferred:
            if idname in polled[mode]:
                modes[idname] = mode
                break
    return modes


def _operator_schema(idname, module_name, operator, mode):
    rna = operator.get_rna_type()
    params = []
    for prop in rna.properties:
        if prop.identifier == "rna_type" or prop.is_hidden:
            continue
        param = _param_schema(prop)
        if param is not None:
            params.append(param)
    schema = {
        "id": "blender:" + idname,
        "label": rna.name or _title(idname.split(".", 1)[1]),
        "category": _MODULE_CATEGORY.get(module_name, _title(module_name)),
        "source": "blender",
        "mode": mode,
        "params": params,
    }
    if rna.description:
        schema["description"] = rna.description
    return schema


def cmd_list_operators(request):
    prefixes = request.get("prefixes") or ["mesh.", "uv.", "object.", "transform."]
    candidates = list(_operator_candidates(prefixes))
    modes = _classify_operators(candidates)
    _state["operator_modes"].update(modes)
    operators = []
    for idname, module_name, operator in candidates:
        if idname in modes:
            operators.append(_operator_schema(idname, module_name, operator, modes[idname]))
    return {"operators": operators}


def _modifier_categories():
    """Modifier type -> heading of Blender's Add Modifier menu, read from the menu classes."""
    categories = {}
    try:
        import bl_ui.properties_data_modifier as menus
    except Exception:
        return categories

    class Recorder:
        def __init__(self, heading):
            self.heading = heading
            self.layout = self

        def operator_modifier_add(self, _layout, mod_type, *_args, **_kwargs):
            categories.setdefault(mod_type, self.heading)

        def __getattr__(self, _name):
            return lambda *args, **kwargs: self

    class MeshContext:
        class object:
            type = "MESH"

        def __getattr__(self, _name):
            return None

    for heading in ("Edit", "Generate", "Deform", "Normals", "Physics"):
        menu = getattr(menus, "OBJECT_MT_modifier_add_" + heading.lower(), None)
        if menu is None:
            continue
        try:
            menu.draw(Recorder(heading), MeshContext())
        except Exception:
            _log("could not read modifier menu", heading)
    return categories


def _modifier_schema(modifier, item, category, base_names):
    params = []
    object_params = []
    for prop in modifier.bl_rna.properties:
        name = prop.identifier
        if name in base_names or prop.is_hidden:
            continue
        if prop.type == "POINTER":
            if prop.fixed_type.identifier == "Object" and not prop.is_readonly:
                object_params.append({"name": name, "label": prop.name or _title(name), "description": prop.description})
            continue
        if prop.is_readonly:
            continue
        try:
            current = getattr(modifier, name)
        except Exception:
            continue
        if prop.type in ("BOOLEAN", "INT", "FLOAT") and getattr(prop, "array_length", 0):
            current = [list(row) if hasattr(row, "__len__") else row for row in current]
        elif prop.type == "ENUM" and prop.is_enum_flag:
            current = set(current)
        param = _param_schema(prop, current)
        if param is not None:
            params.append(param)
    schema = {
        "id": "blender:modifier." + item.identifier,
        "label": item.name,
        "category": category,
        "source": "blender",
        "mode": "MODIFIER",
        "params": params,
    }
    if item.description:
        schema["description"] = item.description
    if object_params:
        schema["object_params"] = object_params
    return schema


def cmd_list_modifiers(_request):
    _reset_scene()
    obj = _link_object("Probe", bpy.data.meshes.new("Probe"))
    categories = _modifier_categories()
    base_names = set(bpy.types.Modifier.bl_rna.properties.keys())
    order = {"Edit": 0, "Generate": 1, "Deform": 2, "Normals": 3, "Physics": 4}
    modifiers = []
    for item in bpy.types.Modifier.bl_rna.properties["type"].enum_items:
        if not item.identifier:
            continue
        try:
            modifier = obj.modifiers.new(item.name, item.identifier)
        except Exception:
            modifier = None
        if modifier is None:
            continue
        category = categories.get(item.identifier, "Other")
        modifiers.append(_modifier_schema(modifier, item, category, base_names))
        obj.modifiers.remove(modifier)
    modifiers.sort(key=lambda schema: order.get(schema["category"], 9))
    return {"modifiers": modifiers}


_PRIMITIVE_SKIP = {"enter_editmode", "align"}


def cmd_list_primitives(_request):
    primitives = []
    for name in dir(bpy.ops.mesh):
        if not (name.startswith("primitive_") and name.endswith("_add")):
            continue
        operator = getattr(bpy.ops.mesh, name)
        schema = _operator_schema("mesh." + name, "mesh", operator, "OBJECT")
        schema["params"] = [param for param in schema["params"] if param["name"] not in _PRIMITIVE_SKIP]
        schema["category"] = "Primitive"
        label = schema["label"]
        schema["label"] = label[4:] if label.startswith("Add ") else label
        primitives.append(schema)
    return {"primitives": primitives}


# ---------------------------------------------------------------------------
# Mesh processing commands


def _operator_from_id(op_id):
    idname = op_id[len("blender:"):] if op_id.startswith("blender:") else op_id
    module_name, _, name = idname.partition(".")
    module = getattr(bpy.ops, module_name, None) if module_name else None
    if module is None or not name or name not in dir(module):
        raise BridgeError("Unknown Blender operator '%s'" % op_id)
    return idname, module_name, getattr(module, name)


def cmd_apply_modifiers(request):
    source = _require(request, "mesh")
    output = _require(request, "output")
    _reset_scene()
    obj = _mesh_object_from_file(source)
    _activate(obj)
    skipped = []
    for index, entry in enumerate(request.get("modifiers") or []):
        mod_type = str(entry.get("type", ""))
        if mod_type.startswith("blender:modifier."):
            mod_type = mod_type[len("blender:modifier."):]
        try:
            modifier = obj.modifiers.new("%02d_%s" % (index, mod_type), mod_type)
        except Exception:
            modifier = None
        if modifier is None:
            raise BridgeError("Unknown or unsupported modifier type '%s'" % mod_type)
        properties = modifier.bl_rna.properties
        entry_skipped = []
        for name, value in _coerce_values(properties, entry.get("values"), entry_skipped).items():
            if properties[name].is_readonly:
                entry_skipped.append(name)
                continue
            setattr(modifier, name, value)
        for name, target in (entry.get("objects") or {}).items():
            prop = properties.get(name)
            if prop is None or prop.type != "POINTER" or prop.fixed_type.identifier != "Object":
                entry_skipped.append(name)
                continue
            if isinstance(target, str):
                target = {"mesh": target}
            other = _mesh_object_from_file(_require(target, "mesh"), "%s_%s" % (modifier.name, name), target.get("transform"))
            other.hide_render = True
            setattr(modifier, name, other)
        enabled = bool(entry.get("enabled", True))
        modifier.show_viewport = enabled
        modifier.show_render = enabled
        skipped.extend("%s.%s" % (mod_type, name) for name in entry_skipped)
    stats = _write_evaluated(obj, output)
    stats["output"] = output
    stats["skipped_values"] = skipped
    return stats


def _select_all(obj, action):
    if action in ("ALL", "SELECT"):
        bpy.ops.mesh.select_all(action="SELECT")
    elif action in ("NONE", "DESELECT"):
        bpy.ops.mesh.select_all(action="DESELECT")
    elif action == "INVERT":
        bpy.ops.mesh.select_all(action="INVERT")


def _operator_mode(idname, module_name, operator, obj):
    mode = _state["operator_modes"].get(idname)
    if mode is not None:
        return mode
    preferred = ("OBJECT", "EDIT") if module_name == "object" else ("EDIT", "OBJECT")
    for mode in preferred:
        _set_mode(obj, mode)
        with bpy.context.temp_override(**_context_override(obj)):
            if operator.poll():
                _state["operator_modes"][idname] = mode
                return mode
    raise BridgeError("Operator '%s' cannot run on a mesh without a viewport" % idname)


def cmd_run_operators(request):
    source = _require(request, "mesh")
    output = _require(request, "output")
    _reset_scene()
    obj = _mesh_object_from_file(source)
    _activate(obj)
    select = str(request.get("select", "ALL")).upper()
    select_mode = request.get("select_mode")
    _set_mode(obj, "EDIT")
    with bpy.context.temp_override(**_context_override(obj)):
        if select_mode:
            bpy.ops.mesh.select_mode(type=str(select_mode).upper())
        _select_all(obj, select)
    results = []
    skipped = []
    for entry in request.get("operators") or []:
        idname, module_name, operator = _operator_from_id(str(entry.get("id", "")))
        rna = operator.get_rna_type()
        entry_skipped = []
        kwargs = _coerce_values(rna.properties, entry.get("values"), entry_skipped)
        skipped.extend("%s.%s" % (idname, name) for name in entry_skipped)
        mode = _operator_mode(idname, module_name, operator, obj)
        _set_mode(obj, mode)
        with bpy.context.temp_override(**_context_override(obj)):
            if mode == "EDIT" and entry.get("select"):
                _select_all(obj, str(entry["select"]).upper())
            if not operator.poll():
                raise BridgeError("Operator '%s' cannot run here (its poll failed)" % idname)
            outcome = operator("EXEC_DEFAULT", **kwargs)
        results.append({"id": "blender:" + idname, "result": sorted(outcome)[0] if outcome else ""})
        if bpy.context.view_layer.objects.active is not obj and obj.name in bpy.context.view_layer.objects:
            _activate(obj)
    _set_mode(obj, "OBJECT")
    stats = _write_evaluated(obj, output)
    stats["output"] = output
    stats["results"] = results
    stats["skipped_values"] = skipped
    return stats


def cmd_create_primitive(request):
    idname, module_name, operator = _operator_from_id(str(_require(request, "id")))
    output = _require(request, "output")
    if module_name != "mesh" or not idname.startswith("mesh.primitive_"):
        raise BridgeError("'%s' is not a mesh primitive" % idname)
    _reset_scene()
    skipped = []
    kwargs = _coerce_values(operator.get_rna_type().properties, request.get("values"), skipped)
    kwargs.pop("enter_editmode", None)
    operator("EXEC_DEFAULT", **kwargs)
    obj = bpy.context.view_layer.objects.active
    if obj is None or obj.type != "MESH":
        raise BridgeError("'%s' did not create a mesh" % idname)
    # Location, rotation and scale are part of the primitive's parameters, so
    # they are baked into the vertices instead of being lost with the object.
    obj.data.transform(obj.matrix_world)
    obj.matrix_world.identity()
    stats = _write_evaluated(obj, output)
    stats["output"] = output
    stats["skipped_values"] = skipped
    return stats


# ---------------------------------------------------------------------------
# File formats


def _call_first(candidates, **kwargs):
    """Call the first operator that exists; importers were renamed across versions."""
    for path in candidates:
        module_name, name = path.split(".")
        module = getattr(bpy.ops, module_name)
        if name in dir(module):
            return getattr(module, name)(**kwargs)
    raise BridgeError("This Blender build has no operator for that format (%s)" % candidates[0])


_IMPORTERS = {
    ".obj": lambda path: _call_first(["wm.obj_import", "import_scene.obj"], filepath=path),
    ".ply": lambda path: _call_first(["wm.ply_import", "import_mesh.ply"], filepath=path),
    ".stl": lambda path: _call_first(["wm.stl_import", "import_mesh.stl"], filepath=path),
    ".fbx": lambda path: _call_first(["import_scene.fbx"], filepath=path),
    ".gltf": lambda path: _call_first(["import_scene.gltf"], filepath=path),
    ".glb": lambda path: _call_first(["import_scene.gltf"], filepath=path),
    ".abc": lambda path: _call_first(["wm.alembic_import"], filepath=path, as_background_job=False),
    ".usd": lambda path: _call_first(["wm.usd_import"], filepath=path),
    ".usda": lambda path: _call_first(["wm.usd_import"], filepath=path),
    ".usdc": lambda path: _call_first(["wm.usd_import"], filepath=path),
    ".usdz": lambda path: _call_first(["wm.usd_import"], filepath=path),
    ".dae": lambda path: _call_first(["wm.collada_import"], filepath=path),
}

_EXPORTERS = {
    ".obj": lambda path: _call_first(["wm.obj_export", "export_scene.obj"], filepath=path),
    ".ply": lambda path: _call_first(["wm.ply_export", "export_mesh.ply"], filepath=path),
    ".stl": lambda path: _call_first(["wm.stl_export", "export_mesh.stl"], filepath=path),
    ".fbx": lambda path: _call_first(["export_scene.fbx"], filepath=path),
    ".gltf": lambda path: _call_first(["export_scene.gltf"], filepath=path, export_format="GLTF_SEPARATE"),
    ".glb": lambda path: _call_first(["export_scene.gltf"], filepath=path, export_format="GLB"),
    ".abc": lambda path: _call_first(["wm.alembic_export"], filepath=path, as_background_job=False),
    ".usd": lambda path: _call_first(["wm.usd_export"], filepath=path),
    ".usda": lambda path: _call_first(["wm.usd_export"], filepath=path),
    ".usdc": lambda path: _call_first(["wm.usd_export"], filepath=path),
    ".usdz": lambda path: _call_first(["wm.usd_export"], filepath=path),
    ".dae": lambda path: _call_first(["wm.collada_export"], filepath=path),
}


def _format_available(table, extension):
    if extension == ".dae":
        return "collada_import" in dir(bpy.ops.wm)
    return extension in table or extension == ".blend"


def _load_file(path):
    if not os.path.isfile(path):
        raise BridgeError("File not found: %s" % path)
    extension = os.path.splitext(path)[1].lower()
    if extension == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path, load_ui=False, use_scripts=False)
        return
    importer = _IMPORTERS.get(extension)
    if importer is None:
        raise BridgeError("Blender cannot import '%s' files" % extension)
    _reset_scene()
    importer(path)


def _save_file(path):
    extension = os.path.splitext(path)[1].lower()
    if extension == ".blend":
        bpy.ops.wm.save_as_mainfile(filepath=path, copy=True, check_existing=False)
        return
    exporter = _EXPORTERS.get(extension)
    if exporter is None:
        raise BridgeError("Blender cannot export '%s' files" % extension)
    exporter(path)


def _material_description(material):
    description = {"name": material.name if material is not None else ""}
    if material is None:
        return description
    color = list(material.diffuse_color)
    metallic = float(material.metallic)
    roughness = float(material.roughness)
    emission = [0.0, 0.0, 0.0]
    tree = material.node_tree
    if tree is not None:
        for node in tree.nodes:
            if node.bl_idname != "ShaderNodeBsdfPrincipled":
                continue
            inputs = node.inputs
            color = list(inputs["Base Color"].default_value)
            metallic = float(inputs["Metallic"].default_value)
            roughness = float(inputs["Roughness"].default_value)
            if "Emission Color" in inputs and "Emission Strength" in inputs:
                strength = float(inputs["Emission Strength"].default_value)
                emission = [float(c) * strength for c in list(inputs["Emission Color"].default_value)[:3]]
            break
    description["base_color"] = [float(c) for c in color]
    description["metallic"] = metallic
    description["roughness"] = roughness
    description["emission"] = emission
    return description


_MESH_LIKE = {"MESH", "CURVE", "SURFACE", "FONT", "META"}


def cmd_import_meshes(request):
    path = _require(request, "path")
    directory = request.get("output_dir") or os.path.join(_state["temp_dir"], "import-%s" % request.get("id", 0))
    os.makedirs(directory, exist_ok=True)
    _load_file(path)
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    meshes = []
    for obj in bpy.context.scene.objects:
        if obj.type not in _MESH_LIKE:
            continue
        evaluated = obj.evaluated_get(depsgraph)
        try:
            mesh = evaluated.to_mesh()
        except RuntimeError:
            continue
        if mesh is None:
            continue
        try:
            if len(mesh.polygons) == 0:
                continue
            slots = [slot.material for slot in evaluated.material_slots]
            file_path = os.path.join(directory, "mesh-%04d.tmsh" % len(meshes))
            stats = _write_mesh_file(file_path, mesh, name=obj.name, material_names=[m.name if m is not None else "" for m in slots])
            stats.update({
                "name": obj.name,
                "file": file_path,
                "transform": _matrix_list(obj.matrix_world),
                "parent": obj.parent.name if obj.parent is not None else "",
                "materials": [_material_description(m) for m in slots],
            })
            meshes.append(stats)
        finally:
            evaluated.to_mesh_clear()
    return {"meshes": meshes, "directory": directory}


def _principled(material):
    if material.node_tree is None:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            material.use_nodes = True
    for node in material.node_tree.nodes:
        if node.bl_idname == "ShaderNodeBsdfPrincipled":
            return node
    tree = material.node_tree
    tree.nodes.clear()
    shader = tree.nodes.new("ShaderNodeBsdfPrincipled")
    out = tree.nodes.new("ShaderNodeOutputMaterial")
    tree.links.new(shader.outputs["BSDF"], out.inputs["Surface"])
    return shader


def _color4(value, default=(0.8, 0.8, 0.8, 1.0)):
    if value is None:
        return list(default)
    values = [float(v) for v in value]
    if len(values) == 3:
        values.append(1.0)
    if len(values) != 4:
        raise BridgeError("A colour needs 3 or 4 numbers")
    return values


def _apply_material(material, description):
    """Set a Principled BSDF from {base_color, metallic, roughness, emission, emission_strength}."""
    shader = _principled(material)
    inputs = shader.inputs
    base = _color4(description.get("base_color"))
    inputs["Base Color"].default_value = base
    material.diffuse_color = base
    inputs["Metallic"].default_value = float(description.get("metallic", 0.0))
    inputs["Roughness"].default_value = float(description.get("roughness", 0.5))
    emission = description.get("emission")
    if emission is not None and "Emission Color" in inputs:
        inputs["Emission Color"].default_value = _color4(emission, (0.0, 0.0, 0.0, 1.0))[:3] + [1.0]
        inputs["Emission Strength"].default_value = float(description.get("emission_strength", 1.0))
    elif emission is not None and "Emission" in inputs:
        inputs["Emission"].default_value = _color4(emission, (0.0, 0.0, 0.0, 1.0))[:3] + [1.0]


def _build_mesh_objects(entries):
    """Create one object per {file, name, transform, material | materials} entry."""
    objects = []
    for index, entry in enumerate(entries or []):
        name = entry.get("name") or "Mesh%d" % index
        obj = _mesh_object_from_file(_require(entry, "file"), name, entry.get("transform"))
        mesh = obj.data
        per_slot = entry.get("materials")
        shared = entry.get("material")
        if not len(mesh.materials) and (shared is not None or per_slot):
            mesh.materials.append(bpy.data.materials.new(name + "Material"))
        for slot, material in enumerate(mesh.materials):
            description = None
            if per_slot and slot < len(per_slot):
                description = per_slot[slot]
            elif shared is not None:
                description = shared
            if description is not None and material is not None:
                _apply_material(material, description)
        objects.append(obj)
    return objects


def cmd_export_meshes(request):
    path = _require(request, "path")
    _reset_scene()
    objects = _build_mesh_objects(request.get("meshes"))
    if not objects:
        raise BridgeError("Nothing to export")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    _save_file(path)
    return {"path": path, "object_count": len(objects)}


def cmd_convert_file(request):
    source = _require(request, "input")
    output = _require(request, "output")
    _load_file(source)
    _save_file(output)
    return {"output": output, "object_count": len(bpy.context.scene.objects)}


# ---------------------------------------------------------------------------
# Rendering


def _cycles_available():
    try:
        import _cycles  # noqa: F401
    except ImportError:
        return False
    return bool(getattr(bpy.app.build_options, "cycles", True))


def _denoise_available():
    try:
        import _cycles
    except ImportError:
        return False
    return bool(getattr(_cycles, "with_openimagedenoise", False))


def _add_camera(scene, description, aspect):
    data = bpy.data.cameras.new("Camera")
    if "ortho_size" in description and description["ortho_size"] is not None:
        # ortho_size is the visible height; Blender's ortho_scale spans the larger side.
        data.type = "ORTHO"
        data.ortho_scale = float(description["ortho_size"]) * max(aspect, 1.0)
    else:
        data.type = "PERSP"
        data.sensor_fit = "VERTICAL"
        data.lens_unit = "FOV"
        data.angle_y = math.radians(float(description.get("fov_y_degrees", 75.0)))
    data.clip_start = float(description.get("near", 0.05))
    data.clip_end = float(description.get("far", 4000.0))
    obj = _link_object("Camera", data)
    obj.matrix_world = _matrix(description.get("transform"))
    scene.camera = obj
    return obj


_LIGHT_TYPES = {"sun": "SUN", "directional": "SUN", "point": "POINT", "omni": "POINT", "spot": "SPOT", "area": "AREA"}


def _add_light(description, index):
    kind = _LIGHT_TYPES.get(str(description.get("type", "sun")).lower())
    if kind is None:
        raise BridgeError("Unknown light type '%s'" % description.get("type"))
    data = bpy.data.lights.new("Light%d" % index, kind)
    data.color = _color4(description.get("color"), (1.0, 1.0, 1.0, 1.0))[:3]
    data.energy = float(description.get("energy", 3.0 if kind == "SUN" else 1000.0))
    if kind == "SPOT":
        data.spot_size = math.radians(float(description.get("spot_angle_degrees", 45.0))) * 2.0
        data.spot_blend = float(description.get("spot_blend", 0.15))
    if kind == "SUN" and "angle_degrees" in description:
        data.angle = math.radians(float(description["angle_degrees"]))
    if kind in ("POINT", "SPOT") and "radius" in description:
        data.shadow_soft_size = float(description["radius"])
    if kind == "AREA" and "size" in description:
        data.size = float(description["size"])
    obj = _link_object("Light%d" % index, data)
    obj.matrix_world = _matrix(description.get("transform"))
    return obj


def _set_world(scene, description):
    world = bpy.data.worlds.new("World")
    scene.world = world
    if world.node_tree is None:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            world.use_nodes = True
    tree = world.node_tree
    tree.nodes.clear()
    background = tree.nodes.new("ShaderNodeBackground")
    out = tree.nodes.new("ShaderNodeOutputWorld")
    tree.links.new(background.outputs["Background"], out.inputs["Surface"])
    background.inputs["Color"].default_value = _color4(description.get("color"), (0.05, 0.05, 0.05, 1.0))
    background.inputs["Strength"].default_value = float(description.get("strength", 1.0))


def cmd_render(request):
    scene_description = request.get("scene") or {}
    settings = request.get("settings") or {}
    output = _require(request, "output")
    engine = str(settings.get("engine", "CYCLES")).upper()
    if engine != "CYCLES":
        raise BridgeError("Only the CYCLES engine can render without a display, not '%s'" % engine)
    if not _cycles_available():
        raise BridgeError("This Blender build has no Cycles")

    _reset_scene()
    scene = bpy.context.scene
    try:
        scene.render.engine = "CYCLES"
    except TypeError:
        raise BridgeError("The Cycles add-on is not enabled in this Blender")
    width = max(1, int(settings.get("width", 640)))
    height = max(1, int(settings.get("height", 360)))
    samples = max(1, int(settings.get("samples", 32)))
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.cycles.use_adaptive_sampling = bool(settings.get("adaptive_sampling", False))
    denoise = bool(settings.get("denoise", False)) and _denoise_available()
    scene.cycles.use_denoising = denoise
    if "max_bounces" in settings:
        scene.cycles.max_bounces = int(settings["max_bounces"])
    if "seed" in settings:
        scene.cycles.seed = int(settings["seed"])
    if "threads" in settings and int(settings["threads"]) > 0:
        scene.render.threads_mode = "FIXED"
        scene.render.threads = int(settings["threads"])
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = bool(settings.get("transparent", False))
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.filepath = output
    scene.render.use_file_extension = False
    try:
        scene.view_settings.view_transform = str(settings.get("view_transform", "Standard"))
        scene.view_settings.look = "None"
    except TypeError:
        pass
    scene.view_settings.exposure = float(settings.get("exposure", 0.0))

    _build_mesh_objects([dict(entry, file=entry.get("file")) for entry in scene_description.get("meshes") or []])
    _add_camera(scene, scene_description.get("camera") or {}, width / height)
    for index, light in enumerate(scene_description.get("lights") or []):
        _add_light(light, index)
    _set_world(scene, scene_description.get("world") or {})

    import re

    sample_pattern = re.compile(r"Sample (\d+)/(\d+)")
    last = {"sample": -1}

    def on_stats(*args):
        text = args[0] if args and isinstance(args[0], str) else ""
        match = sample_pattern.search(text)
        if match is None:
            return
        done, total = int(match.group(1)), max(int(match.group(2)), 1)
        if done == last["sample"]:
            return
        last["sample"] = done
        _event("progress", stage="sample", sample=done, samples=total, fraction=min(done / total, 1.0))

    stats_handlers = getattr(bpy.app.handlers, "render_stats", None)
    if stats_handlers is not None:
        stats_handlers.append(on_stats)
    _event("progress", stage="start", sample=0, samples=samples, fraction=0.0)
    started = time.monotonic()
    try:
        bpy.ops.render.render(write_still=True)
    finally:
        if stats_handlers is not None and on_stats in stats_handlers:
            stats_handlers.remove(on_stats)
    elapsed = time.monotonic() - started
    if not os.path.isfile(output):
        raise BridgeError("Blender finished rendering but wrote no image")
    _event("progress", stage="finish", sample=samples, samples=samples, fraction=1.0)
    return {"output": output, "render_time": elapsed, "width": width, "height": height, "samples": samples, "denoised": denoise}


# ---------------------------------------------------------------------------
# Session


def cmd_hello(_request):
    extensions = sorted(set(list(_IMPORTERS.keys()) + [".blend"]))
    return {
        "protocol": PROTOCOL_VERSION,
        "blender_version": bpy.app.version_string,
        "blender_version_tuple": list(bpy.app.version),
        "python_version": sys.version.split()[0],
        "temp_dir": _state["temp_dir"],
        "cycles": _cycles_available(),
        "denoise": _denoise_available(),
        "background": bool(bpy.app.background),
        "pid": os.getpid(),
        "import_extensions": [e for e in extensions if _format_available(_IMPORTERS, e)],
        "export_extensions": [e for e in sorted(set(list(_EXPORTERS.keys()) + [".blend"])) if _format_available(_EXPORTERS, e)],
    }


def cmd_shutdown(_request):
    return {}


COMMANDS = {
    "hello": cmd_hello,
    "list_operators": cmd_list_operators,
    "list_modifiers": cmd_list_modifiers,
    "list_primitives": cmd_list_primitives,
    "apply_modifiers": cmd_apply_modifiers,
    "run_operators": cmd_run_operators,
    "create_primitive": cmd_create_primitive,
    "import_meshes": cmd_import_meshes,
    "export_meshes": cmd_export_meshes,
    "convert_file": cmd_convert_file,
    "render": cmd_render,
    "shutdown": cmd_shutdown,
}


def _error_text(error):
    text = str(error).strip()
    # Operator failures arrive as "Error: message" lines from Blender's reports.
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return " ".join(lines) if lines else type(error).__name__


def _handle(line):
    """Answer one request line. Returns False when the server should stop."""
    request_id = None
    try:
        request = json.loads(line)
        if not isinstance(request, dict):
            raise ValueError("a request must be a JSON object")
    except ValueError as error:
        _send({"id": None, "ok": False, "error": "Invalid request: %s" % error})
        return True
    request_id = request.get("id")
    command = request.get("cmd")
    handler = COMMANDS.get(command) if isinstance(command, str) else None
    if handler is None:
        _send({"id": request_id, "ok": False, "error": "Unknown command '%s'" % command})
        return True
    _state["request_id"] = request_id
    _state["command"] = command
    try:
        response = handler(request) or {}
        response["id"] = request_id
        response["ok"] = True
    except BridgeError as error:
        response = {"id": request_id, "ok": False, "error": str(error)}
    except Exception as error:
        tail = traceback.format_exc().strip().splitlines()[-8:]
        response = {"id": request_id, "ok": False, "error": "%s: %s" % (type(error).__name__, _error_text(error)), "traceback": "\n".join(tail)}
        try:
            # A failed operator can leave edit mode or half-built data behind.
            _reset_scene()
        except Exception:
            pass
    finally:
        _state["request_id"] = None
        _state["command"] = ""
    try:
        _send(response)
    except ValueError:
        _send({"id": request_id, "ok": False, "error": "The result of '%s' contained a non-finite number" % command})
    return command != "shutdown"


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    temp_dir = ""
    if "--temp-dir" in arguments and arguments.index("--temp-dir") + 1 < len(arguments):
        temp_dir = arguments[arguments.index("--temp-dir") + 1]
    if temp_dir:
        os.makedirs(temp_dir, exist_ok=True)
    else:
        temp_dir = tempfile.mkdtemp(prefix="tetra-blender-")
        _state["owns_temp_dir"] = True
    _state["temp_dir"] = temp_dir

    _send({"event": "ready", "id": None, "cmd": "", "protocol": PROTOCOL_VERSION, "blender_version": bpy.app.version_string, "pid": os.getpid()})
    stream = sys.stdin.buffer
    while True:
        raw = stream.readline()
        if not raw:
            break
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            if not _handle(line):
                break
        except (BrokenPipeError, OSError):
            break
    if _state["owns_temp_dir"]:
        shutil.rmtree(temp_dir, ignore_errors=True)
    try:
        _protocol.flush()
    except OSError:
        pass
    sys.stderr.flush()
    # Skip interpreter and Blender teardown: the client is gone or told us to stop,
    # and a clean exit code matters more than freeing memory.
    os._exit(0)


if __name__ == "__main__":
    main()
