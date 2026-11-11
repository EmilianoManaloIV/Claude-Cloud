"""Stand-alone check of tetra_blender_server.py.

Run with a Python that has bpy installed:

    build/blender-venv/bin/python modules/tetra_model/bridge/test_server.py

``--dry-run-operators`` additionally runs every listed operator with its
default values on a cube and prints the ones that fail, which is how the
server's deny lists are maintained.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile

SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tetra_blender_server.py")


class Client:
    def __init__(self):
        self.process = subprocess.Popen([sys.executable, SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.next_id = 1
        self.events = []
        ready = self._read()
        assert ready.get("event") == "ready", ready

    def _read(self):
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise EOFError("server exited")
            text = line.decode("utf-8").strip()
            if text.startswith("{"):
                return json.loads(text)

    def raw(self, text):
        self.process.stdin.write(text.encode("utf-8") + b"\n")
        self.process.stdin.flush()
        while True:
            message = self._read()
            if "event" in message:
                self.events.append(message)
                continue
            return message

    def call(self, cmd, **fields):
        request = {"id": self.next_id, "cmd": cmd}
        request.update(fields)
        self.next_id += 1
        assert request["cmd"] == cmd and isinstance(request["id"], int), "a field collides with the protocol's own keys"
        response = self.raw(json.dumps(request))
        assert response["id"] == request["id"], response
        return response

    def close(self):
        response = self.call("shutdown")
        assert response["ok"]
        return self.process.wait(timeout=20)


def write_mesh(path, positions, polygons, uvs=None, normals=None, materials=()):
    loop_start = []
    corners = []
    for polygon in polygons:
        loop_start.append(len(corners))
        corners.extend(polygon)
    header = {
        "vertex_count": len(positions),
        "polygon_count": len(polygons),
        "corner_count": len(corners),
        "triangle_count": 0,
        "has_uvs": uvs is not None,
        "has_normals": normals is not None,
        "materials": list(materials),
    }
    blob = json.dumps(header).encode("utf-8")
    blob += b" " * (-len(blob) % 4)
    with open(path, "wb") as handle:
        handle.write(b"TMSH" + struct.pack("<II", 1, len(blob)) + blob)
        handle.write(struct.pack("<%df" % (len(positions) * 3), *[c for p in positions for c in p]))
        handle.write(struct.pack("<%di" % len(loop_start), *loop_start))
        handle.write(struct.pack("<%di" % len(corners), *corners))
        handle.write(struct.pack("<%di" % len(polygons), *([0] * len(polygons))))
        if uvs is not None:
            handle.write(struct.pack("<%df" % (len(uvs) * 2), *[c for uv in uvs for c in uv]))
        if normals is not None:
            handle.write(struct.pack("<%df" % (len(normals) * 3), *[c for n in normals for c in n]))


def read_mesh(path):
    with open(path, "rb") as handle:
        blob = handle.read()
    assert blob[:4] == b"TMSH"
    version, header_length = struct.unpack_from("<II", blob, 4)
    assert version == 1
    header = json.loads(blob[12:12 + header_length])
    offset = 12 + header_length

    def take(fmt, count):
        nonlocal offset
        values = struct.unpack_from("<%d%s" % (count, fmt), blob, offset)
        offset += count * 4
        return values

    mesh = {"header": header}
    mesh["positions"] = take("f", header["vertex_count"] * 3)
    mesh["loop_start"] = take("i", header["polygon_count"])
    mesh["corner_vertices"] = take("i", header["corner_count"])
    mesh["polygon_material"] = take("i", header["polygon_count"])
    if header["has_uvs"]:
        mesh["uvs"] = take("f", header["corner_count"] * 2)
    if header["has_normals"]:
        mesh["normals"] = take("f", header["corner_count"] * 3)
    mesh["triangle_corners"] = take("i", header["triangle_count"] * 3)
    mesh["triangle_polygons"] = take("i", header["triangle_count"])
    assert offset == len(blob), "trailing bytes in %s" % path
    return mesh


def bounds(mesh):
    p = mesh["positions"]
    xs, ys, zs = p[0::3], p[1::3], p[2::3]
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))


CUBE_POSITIONS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
CUBE_POLYGONS = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]

checks = 0
failures = 0


def check(condition, what):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print("FAILED:", what)


def dry_run_operators(client, operators, cube):
    """Run every operator once with defaults; report the ones that cannot work."""
    out = cube + ".dry"
    problems = []
    for schema in operators:
        try:
            response = client.call("run_operators", mesh=cube, output=out, operators=[{"id": schema["id"], "values": {}}])
        except EOFError:
            problems.append((schema["id"], "SERVER DIED"))
            client = Client()
            continue
        if not response["ok"]:
            problems.append((schema["id"], response["error"][:110]))
        elif response["results"][0]["result"] not in ("FINISHED", "CANCELLED"):
            problems.append((schema["id"], response["results"][0]["result"]))
    for problem in problems:
        print("  %-44s %s" % problem)
    print("dry run: %d of %d operators did not finish or cancel cleanly" % (len(problems), len(operators)))
    return client


def main():
    work = tempfile.mkdtemp(prefix="tetra-server-test-")
    client = Client()

    hello = client.call("hello")
    check(hello["ok"] and hello["blender_version_tuple"][0] >= 4, "hello reports a Blender version")
    check(os.path.isdir(hello["temp_dir"]), "hello names an existing temp dir")
    print("Blender %s, Python %s, cycles=%s" % (hello["blender_version"], hello["python_version"], hello["cycles"]))

    listing = client.call("list_operators", prefixes=["mesh.", "uv.", "object.", "transform."])
    operators = {schema["id"]: schema for schema in listing["operators"]}
    check(len(operators) > 100, "more than 100 operators, got %d" % len(operators))
    check("blender:mesh.subdivide" in operators and "blender:mesh.bevel" in operators, "subdivide and bevel listed")
    check("blender:mesh.loopcut" not in operators and "blender:mesh.knife_tool" not in operators, "viewport-only operators left out")
    subdivide = operators["blender:mesh.subdivide"]
    cuts = [p for p in subdivide["params"] if p["name"] == "number_cuts"][0]
    check(cuts["type"] == "int" and cuts["default"] == 1 and cuts["min"] == 1 and cuts["max"] == 100 and cuts["ui_max"] == 10, "number_cuts schema: %s" % cuts)
    check(subdivide["mode"] == "EDIT" and subdivide["category"] == "Mesh" and subdivide["label"] == "Subdivide", "subdivide metadata")
    spin = [p for p in operators["blender:mesh.spin"]["params"] if p["name"] == "angle"][0]
    check(spin["unit"] == "°" and abs(spin["default"] - 90.0) < 1e-3, "angles are presented in degrees: %s" % spin)
    print("%d operators (%d edit mode, %d object mode)" % (len(operators), sum(1 for s in operators.values() if s["mode"] == "EDIT"), sum(1 for s in operators.values() if s["mode"] == "OBJECT")))

    listing = client.call("list_modifiers")
    modifiers = {schema["id"].split(".")[-1]: schema for schema in listing["modifiers"]}
    for name in ("SUBSURF", "BEVEL", "SOLIDIFY", "ARRAY", "MIRROR", "DECIMATE", "BOOLEAN"):
        check(name in modifiers, "modifier %s listed" % name)
    levels = [p for p in modifiers["SUBSURF"]["params"] if p["name"] == "levels"][0]
    check(levels["type"] == "int" and levels["default"] == 1, "SUBSURF levels default: %s" % levels)
    check(modifiers["SUBSURF"]["category"] == "Generate" and modifiers["SIMPLE_DEFORM"]["category"] == "Deform", "modifier categories")
    check(any(p["name"] == "object" for p in modifiers["BOOLEAN"].get("object_params", [])), "Boolean exposes its object")
    print("%d modifiers" % len(modifiers))

    primitives = {schema["id"]: schema for schema in client.call("list_primitives")["primitives"]}
    check("blender:mesh.primitive_monkey_add" in primitives and "blender:mesh.primitive_uv_sphere_add" in primitives, "primitives listed")

    cube = os.path.join(work, "cube.tmsh")
    write_mesh(cube, CUBE_POSITIONS, CUBE_POLYGONS)
    out = os.path.join(work, "out.tmsh")

    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[])
    check(response["ok"], "empty stack: %s" % response.get("error"))
    result = read_mesh(out)
    check(result["header"]["vertex_count"] == 8 and result["header"]["polygon_count"] == 6 and result["header"]["triangle_count"] == 12, "empty stack keeps the cube")
    check(bounds(result) == (-1, -1, -1, 1, 1, 1), "empty stack keeps the bounds")
    check(result["corner_vertices"][:4] == (0, 3, 2, 1), "corner order preserved")
    check(abs(result["normals"][2] + 1.0) < 1e-5, "flat bottom face normal points down: %s" % (result["normals"][:3],))

    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[{"type": "SUBSURF", "values": {"levels": 2.0}}])
    result = read_mesh(out)
    check(response["ok"] and result["header"]["polygon_count"] == 96, "SUBSURF 2 gives 96 quads, got %s" % result["header"]["polygon_count"])
    check(bounds(result)[3] < 0.95, "SUBSURF shrinks the cube")

    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[
        {"type": "SUBSURF", "values": {"levels": 2}, "enabled": False},
        {"type": "blender:modifier.ARRAY", "values": {"count": 3, "relative_offset_displace": [1.5, 0, 0], "bogus": 1}}])
    result = read_mesh(out)
    check(response["ok"] and result["header"]["polygon_count"] == 18, "disabled modifier skipped, ARRAY x3 gives 18 quads")
    check(abs(bounds(result)[3] - 7.0) < 1e-5, "ARRAY offset applied from a vec3")
    check(response["skipped_values"] == ["ARRAY.bogus"], "unknown values are reported")

    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[{"type": "SIMPLE_DEFORM", "values": {"deform_method": "TWIST", "angle": 90.0, "deform_axis": "Z"}}])
    result = read_mesh(out)
    # A 90 degree twist over the cube's height turns the top face 45 degrees one way and the bottom the other.
    check(response["ok"] and abs(bounds(result)[3] - 2 ** 0.5) < 1e-4, "angle given in degrees: max x %s" % bounds(result)[3])

    small = os.path.join(work, "small.tmsh")
    write_mesh(small, [(c[0] * 0.5 + 1, c[1] * 0.5, c[2] * 0.5) for c in CUBE_POSITIONS], CUBE_POLYGONS)
    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[{"type": "BOOLEAN", "values": {"operation": "DIFFERENCE", "solver": "EXACT"}, "objects": {"object": small}}])
    result = read_mesh(out)
    check(response["ok"] and result["header"]["vertex_count"] == 16, "Boolean difference with a second mesh: %s" % (response.get("error") or result["header"]["vertex_count"]))

    plane = os.path.join(work, "plane.tmsh")
    write_mesh(plane, [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [(0, 1, 2, 3)], uvs=[(0, 0), (1, 0), (1, 1), (0, 1)], normals=[(0, 0, 1)] * 4, materials=["Paint"])
    response = client.call("run_operators", mesh=plane, output=out, operators=[{"id": "blender:mesh.subdivide", "values": {"number_cuts": 2}}])
    result = read_mesh(out)
    check(response["ok"] and result["header"]["polygon_count"] == 9 and result["header"]["triangle_count"] == 18, "subdivide with 2 cuts gives 9 quads")
    check(response["results"] == [{"id": "blender:mesh.subdivide", "result": "FINISHED"}], "operator result reported")
    check(result["header"]["materials"] == ["Paint"] and result["header"]["has_uvs"], "materials and UVs survive")
    check(max(result["uvs"]) == 1.0 and min(result["uvs"]) == 0.0, "UV range kept")

    response = client.call("run_operators", mesh=cube, output=out, operators=[
        {"id": "mesh.inset", "values": {"thickness": 0.25, "use_individual": True}},
        {"id": "blender:object.shade_smooth", "values": {}},
        {"id": "blender:transform.translate", "values": {"value": [0, 0, 2]}}])
    result = read_mesh(out)
    check(response["ok"] and result["header"]["polygon_count"] == 30, "inset then object-mode operator then transform: %s" % response.get("error"))
    check(response["ok"] and abs(bounds(result)[5] - 3.0) < 1e-5, "translate moved the mesh")

    triangles = os.path.join(work, "triangles.tmsh")
    write_mesh(triangles, [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [(0, 1, 2), (0, 2, 3)], normals=[(0, 0, 1)] * 6)
    response = client.call("run_operators", mesh=triangles, output=out, join_triangles=True, operators=[{"id": "blender:mesh.subdivide", "values": {"number_cuts": 2}}])
    check(response["ok"] and response["polygon_count"] == 9, "joined triangles subdivide like the quad they came from: %s" % (response.get("error") or response.get("polygon_count")))
    response = client.call("apply_modifiers", mesh=triangles, output=out, join_triangles=True, modifiers=[])
    check(response["ok"] and response["polygon_count"] == 1, "join_triangles also applies to modifier stacks")

    monkey = os.path.join(work, "monkey.tmsh")
    response = client.call("create_primitive", primitive="blender:mesh.primitive_monkey_add", values={}, output=monkey)
    result = read_mesh(monkey)
    check(response["ok"] and result["header"]["vertex_count"] == 507, "Suzanne has 507 vertices")
    response = client.call("create_primitive", primitive="blender:mesh.primitive_cube_add", values={"size": 4, "location": [10, 0, 0]}, output=out)
    check(response["ok"] and bounds(read_mesh(out))[:4] == (8, -2, -2, 12), "primitive parameters and location applied")

    exported = os.path.join(work, "scene.glb")
    response = client.call("export_meshes", path=exported, meshes=[
        {"file": monkey, "name": "Suzanne", "transform": [1, 0, 0, 3, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], "material": {"base_color": [1, 0, 0, 1]}},
        {"file": cube, "name": "Box"}])
    check(response["ok"] and os.path.getsize(exported) > 1000, "export to glb: %s" % response.get("error"))
    response = client.call("import_meshes", path=exported)
    check(response["ok"] and sorted(m["name"] for m in response["meshes"]) == ["Box", "Suzanne"], "import finds both objects: %s" % response.get("error"))
    if response["ok"]:
        suzanne = [m for m in response["meshes"] if m["name"] == "Suzanne"][0]
        check(abs(suzanne["transform"][3] - 3.0) < 1e-5, "object transform survives the round trip")
        check(suzanne["materials"][0]["base_color"][:3] == [1.0, 0.0, 0.0], "material colour survives")
        check(read_mesh(suzanne["file"])["header"]["triangle_count"] == 968, "imported mesh has Suzanne's 968 triangles")
    converted = os.path.join(work, "scene.obj")
    response = client.call("convert_file", input=exported, output=converted)
    check(response["ok"] and os.path.isfile(converted), "convert glb to obj: %s" % response.get("error"))

    image = os.path.join(work, "render.png")
    client.events.clear()
    response = client.call("render", output=image, settings={"engine": "CYCLES", "samples": 4, "width": 64, "height": 48}, scene={
        "meshes": [{"file": cube, "material": {"base_color": [0.8, 0.2, 0.2, 1]}}],
        "camera": {"transform": [1, 0, 0, 0, 0, 0, -1, -6, 0, 1, 0, 0, 0, 0, 0, 1], "fov_y_degrees": 40},
        "lights": [{"type": "sun", "transform": [1, 0, 0, 0, 0, 0.7071, -0.7071, 0, 0, 0.7071, 0.7071, 0, 0, 0, 0, 1], "energy": 4}],
        "world": {"color": [0.1, 0.1, 0.2], "strength": 1.0}})
    check(response["ok"] and os.path.getsize(image) > 200, "render: %s" % response.get("error"))
    stages = [event["stage"] for event in client.events if event["event"] == "progress"]
    check(stages[:1] == ["start"] and stages[-1:] == ["finish"] and "sample" in stages, "progress events: %s" % stages)

    for bad, why in (("not json", "Invalid request"), ("[1]", "Invalid request"), (json.dumps({"id": 900, "cmd": "nope"}), "Unknown command")):
        response = client.raw(bad)
        check(not response["ok"] and why in response["error"], "bad request answered: %s" % response)
    response = client.call("apply_modifiers", mesh=os.path.join(work, "missing.tmsh"), output=out, modifiers=[])
    check(not response["ok"] and "FileNotFoundError" in response["error"] and "traceback" in response, "missing file: %s" % response)
    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[{"type": "NOPE"}])
    check(not response["ok"] and "NOPE" in response["error"], "unknown modifier: %s" % response)
    response = client.call("run_operators", mesh=cube, output=out, operators=[{"id": "blender:mesh.nope"}])
    check(not response["ok"] and "Unknown Blender operator" in response["error"], "unknown operator: %s" % response)
    response = client.call("run_operators", mesh=cube, output=out, operators=[{"id": "blender:mesh.subdivide", "values": {"quadcorner": "WRONG"}}])
    check(not response["ok"] and "TypeError" in response["error"], "bad enum value: %s" % response)
    response = client.call("apply_modifiers", mesh=cube, output=out, modifiers=[])
    check(response["ok"], "server keeps working after errors")

    if "--dry-run-operators" in sys.argv:
        client = dry_run_operators(client, listing_operators(client), cube)

    temp_dir = hello["temp_dir"]
    check(client.close() == 0, "clean exit")
    check(not os.path.exists(temp_dir), "temp dir removed")
    print("%d checks, %d failed" % (checks, failures))
    return 1 if failures else 0


def listing_operators(client):
    return client.call("list_operators")["operators"]


if __name__ == "__main__":
    sys.exit(main())
