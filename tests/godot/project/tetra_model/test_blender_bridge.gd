extends "res://tetra_test.gd"
## TetraBlenderBridge against a real Blender: protocol, schemas, mesh round
## trips, rendering, error handling and process lifetime. Skipped when no
## Blender can be launched (see TetraBlenderBridge.find_launcher()).

var _bridge: TetraBlenderBridge
var _progress: Array = []
var _log_lines := 0
var _started := 0
var _stopped := 0


func run() -> void:
	var launcher := TetraBlenderBridge.find_launcher()
	if launcher["kind"] == "":
		print("SKIPPED tetra_model/test_blender_bridge.gd: ", launcher["description"])
		return
	check(TetraBlenderBridge.is_available(), "a launcher means the bridge is available")

	_bridge = TetraBlenderBridge.new()
	_bridge.progress.connect(func(event: Dictionary) -> void: _progress.append(event))
	_bridge.log_line.connect(func(_line: String) -> void: _log_lines += 1)
	_bridge.started.connect(func() -> void: _started += 1)
	_bridge.stopped.connect(func() -> void: _stopped += 1)

	await _test_session()
	_test_schemas()
	_test_modifiers()
	_test_operators_and_primitives()
	await _test_async()
	await _test_render()
	_test_files()
	_test_errors()
	await _test_lifetime()


func _test_session() -> void:
	check(not _bridge.is_running(), "Blender is not started before the first request")
	check_eq(_bridge.get_blender_version(), "", "no version before the first start")
	check(_bridge.get_launch_description().contains("bpy module") or _bridge.get_launch_description().contains("Blender executable"), "launch description: " + _bridge.get_launch_description())

	var hello := _bridge.request({"cmd": "hello"})
	check(hello["ok"], "hello: %s" % hello.get("error", ""))
	check(_bridge.is_running(), "the first request starts Blender")
	check(_bridge.get_blender_version().begins_with("5."), "Blender 5.x, got '%s'" % _bridge.get_blender_version())
	check_eq(hello.get("blender_version"), _bridge.get_blender_version(), "hello reports the same version")
	check(str(hello.get("python_version")).begins_with("3."), "Python version reported")
	check_eq(hello.get("temp_dir"), _bridge.get_exchange_dir(), "the server uses the bridge's exchange directory")
	check(hello.get("cycles") == true, "Cycles is available")
	check_eq(_bridge.start(), OK, "start() on a running bridge")
	await process_frame
	await process_frame
	check_eq(_started, 1, "started signal")
	if ClassDB.class_exists("TetraEngineRegistry"):
		var engine: Dictionary = ClassDB.class_call_static("TetraEngineRegistry", "get_engine", "Blender")
		check(engine.get("available") == true and engine.get("version") == _bridge.get_blender_version(), "engine registry entry: %s" % engine)


func _validate(p_operations: Array, p_what: String) -> void:
	if not ClassDB.class_exists("TetraSchema"):
		print("TetraSchema is not part of this build; %s schemas were not validated" % p_what)
		return
	var problems := 0
	for operation: Dictionary in p_operations:
		var report: String = ClassDB.class_call_static("TetraSchema", "validate_operation", operation)
		if report != "":
			problems += 1
			if problems <= 3:
				printerr("%s: %s" % [operation.get("id"), report])
	check_eq(problems, 0, "every %s schema validates" % p_what)


func _by_id(p_operations: Array) -> Dictionary:
	var result := {}
	for operation: Dictionary in p_operations:
		result[operation["id"]] = operation
	return result


func _param(p_operation: Dictionary, p_name: String) -> Dictionary:
	for param: Dictionary in p_operation.get("params", []):
		if param["name"] == p_name:
			return param
	return {}


func _test_schemas() -> void:
	var listed := _bridge.list_operators()
	check(listed.size() > 100, "more than 100 operators, got %d" % listed.size())
	_validate(listed, "operator")
	var operators := _by_id(listed)
	check(operators.has("blender:mesh.subdivide") and operators.has("blender:mesh.bevel"), "subdivide and bevel are listed")
	check(not operators.has("blender:mesh.loopcut") and not operators.has("blender:mesh.knife_tool"), "viewport-only tools are not listed")
	if operators.has("blender:mesh.subdivide"):
		var subdivide: Dictionary = operators["blender:mesh.subdivide"]
		check_eq(subdivide["label"], "Subdivide", "label")
		check_eq(subdivide["category"], "Mesh", "category")
		check_eq(subdivide["source"], "blender", "source")
		check_eq(subdivide["mode"], "EDIT", "mode")
		var cuts := _param(subdivide, "number_cuts")
		check(cuts.get("type") == "int" and typeof(cuts.get("default")) == TYPE_INT and cuts.get("default") == 1, "number_cuts: %s" % cuts)
		check(cuts.get("min") == 1.0 and cuts.get("max") == 100.0 and cuts.get("ui_max") == 10.0, "number_cuts limits")
		var corner := _param(subdivide, "quadcorner")
		check(corner.get("type") == "enum" and corner.get("default") == "STRAIGHT_CUT" and (corner.get("options", []) as Array).size() == 4, "quadcorner: %s" % corner)
	var plane_co := _param(operators.get("blender:mesh.bisect", {}), "plane_co")
	check(plane_co.get("type") == "vec3" and typeof(plane_co.get("default")) == TYPE_VECTOR3, "vectors become Vector3: %s" % plane_co)
	var angle := _param(operators.get("blender:mesh.spin", {}), "angle")
	check(angle.get("unit") == "°" and is_equal_approx(angle.get("default", 0.0), 90.0), "angles are in degrees: %s" % angle)
	check_eq(operators.get("blender:object.shade_smooth", {}).get("mode"), "OBJECT", "object-mode operators are marked")

	listed = _bridge.list_modifiers()
	_validate(listed, "modifier")
	var modifiers := _by_id(listed)
	for type: String in ["SUBSURF", "BEVEL", "SOLIDIFY", "ARRAY", "MIRROR", "DECIMATE"]:
		check(modifiers.has("blender:modifier." + type), "modifier %s is listed" % type)
	var levels := _param(modifiers.get("blender:modifier.SUBSURF", {}), "levels")
	check(levels.get("type") == "int" and levels.get("default") == 1 and levels.get("min") == 0.0, "SUBSURF levels: %s" % levels)
	check_eq(modifiers.get("blender:modifier.SUBSURF", {}).get("category"), "Generate", "SUBSURF category")
	check_eq(modifiers.get("blender:modifier.SUBSURF", {}).get("label"), "Subdivision Surface", "SUBSURF label")
	var width := _param(modifiers.get("blender:modifier.BEVEL", {}), "width")
	check(width.get("type") == "float" and is_equal_approx(width.get("default", 0.0), 0.1) and width.get("unit") == "m", "BEVEL width: %s" % width)
	check_eq(_param(modifiers.get("blender:modifier.BEVEL", {}), "segments").get("type"), "int", "BEVEL segments")
	var thickness := _param(modifiers.get("blender:modifier.SOLIDIFY", {}), "thickness")
	check(thickness.get("type") == "float" and is_equal_approx(thickness.get("default", 0.0), 0.01), "SOLIDIFY thickness: %s" % thickness)
	check_eq(_param(modifiers.get("blender:modifier.ARRAY", {}), "count").get("default"), 2, "ARRAY count")
	check_eq(_param(modifiers.get("blender:modifier.ARRAY", {}), "relative_offset_displace").get("default"), Vector3(1, 0, 0), "ARRAY relative offset")
	check_eq(_param(modifiers.get("blender:modifier.MIRROR", {}), "use_axis").get("default"), "1,0,0", "MIRROR axes are a number list")
	var ratio := _param(modifiers.get("blender:modifier.DECIMATE", {}), "ratio")
	check(ratio.get("type") == "float" and ratio.get("default") == 1.0 and ratio.get("min") == 0.0 and ratio.get("max") == 1.0, "DECIMATE ratio: %s" % ratio)
	check_eq(modifiers.get("blender:modifier.SIMPLE_DEFORM", {}).get("category"), "Deform", "SIMPLE_DEFORM category")
	var boolean_objects: Array = modifiers.get("blender:modifier.BOOLEAN", {}).get("object_params", [])
	check(boolean_objects.size() == 1 and boolean_objects[0].get("name") == "object", "BOOLEAN names its object parameter")

	listed = _bridge.list_primitives()
	_validate(listed, "primitive")
	var primitives := _by_id(listed)
	check(primitives.has("blender:mesh.primitive_monkey_add") and primitives.has("blender:mesh.primitive_torus_add"), "primitives are listed")
	check_eq(primitives.get("blender:mesh.primitive_monkey_add", {}).get("label"), "Monkey", "primitive label")

	var again := _bridge.list_operators()
	check_eq(again.size(), operators.size(), "the cached list is the same")
	again.clear()
	check_eq(_bridge.list_operators().size(), operators.size(), "callers cannot damage the cache")


func _triangle_count(p_mesh: Mesh) -> int:
	var total := 0
	for surface in p_mesh.get_surface_count():
		total += (p_mesh.surface_get_arrays(surface)[Mesh.ARRAY_INDEX] as PackedInt32Array).size() / 3
	return total


func _unique_positions(p_mesh: Mesh) -> int:
	var seen := {}
	for surface in p_mesh.get_surface_count():
		for vertex: Vector3 in p_mesh.surface_get_arrays(surface)[Mesh.ARRAY_VERTEX]:
			seen[vertex] = true
	return seen.size()


## Triangles whose clockwise front side disagrees with their vertex normals.
func _misoriented_triangles(p_mesh: Mesh) -> int:
	var wrong := 0
	for surface in p_mesh.get_surface_count():
		var arrays := p_mesh.surface_get_arrays(surface)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		for i in range(0, indices.size(), 3):
			var a := vertices[indices[i]]
			if (vertices[indices[i + 2]] - a).cross(vertices[indices[i + 1]] - a).dot(normals[indices[i]]) <= 0.0:
				wrong += 1
	return wrong


func _test_modifiers() -> void:
	var material := StandardMaterial3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(1, 2, 3)
	box.material = material

	var same := _bridge.apply_modifier_stack(box, [])
	check(same != null, "empty stack: " + _bridge.get_last_error())
	if same != null:
		check(same.get_aabb().is_equal_approx(box.get_aabb()), "an empty stack keeps the bounding box and the axes: %s" % same.get_aabb())
		check_eq(_triangle_count(same), 12, "an empty stack keeps the 12 triangles")
		check_eq(_unique_positions(same), 8, "and the 8 corners")
		check_eq(same.surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size(), 24, "flat shading survives: 4 vertices per face")
		check_eq(_misoriented_triangles(same), 0, "faces still point outwards")
		check(same.surface_get_material(0) == material, "the surface material is carried over")
		check(same.surface_get_arrays(0)[Mesh.ARRAY_TEX_UV] != null and same.surface_get_arrays(0)[Mesh.ARRAY_TANGENT] != null, "UVs and tangents come back")

	# One asymmetric triangle: any mirrored or swapped axis would move a corner.
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(0, 0, 0), Vector3(0, 2, 0), Vector3(1, 0, 0.5)])
	var normal := (Vector3(1, 0, 0.5) - Vector3(0, 0, 0)).cross(Vector3(0, 2, 0) - Vector3(0, 0, 0)).normalized()
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([normal, normal, normal])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([Vector2(0, 0), Vector2(0, 1), Vector2(0.25, 0.75)])
	var triangle := ArrayMesh.new()
	triangle.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	same = _bridge.apply_modifier_stack(triangle, [])
	check(same != null and same.get_surface_count() == 1, "a single triangle round-trips")
	if same != null and same.get_surface_count() == 1:
		var back := same.surface_get_arrays(0)
		var uv_of := {}
		for i in (back[Mesh.ARRAY_VERTEX] as PackedVector3Array).size():
			uv_of[back[Mesh.ARRAY_VERTEX][i]] = back[Mesh.ARRAY_TEX_UV][i]
			# Godot stores normals octahedron-encoded, so not bit for bit.
			check((back[Mesh.ARRAY_NORMAL][i] as Vector3).dot(normal) > 0.9999, "the normal keeps its direction: %s" % back[Mesh.ARRAY_NORMAL][i])
		check(uv_of.size() == 3 and uv_of.get(Vector3(0, 2, 0)) == Vector2(0, 1) and uv_of.get(Vector3(1, 0, 0.5)) == Vector2(0.25, 0.75), "positions and UVs are unchanged: %s" % uv_of)
		check_eq(_misoriented_triangles(same), 0, "the winding is unchanged")

	var cube := BoxMesh.new()
	var smooth := _bridge.apply_modifier_stack(cube, [{"type": "SUBSURF", "values": {"levels": 2}}])
	check(smooth != null, "SUBSURF: " + _bridge.get_last_error())
	if smooth != null:
		check(_unique_positions(smooth) > 8, "SUBSURF adds vertices: %d" % _unique_positions(smooth))
		check(_triangle_count(smooth) > 100, "SUBSURF adds faces: %d" % _triangle_count(smooth))
		var size := smooth.get_aabb().size
		check(size.x < 0.95 and size.y < 0.95 and size.z < 0.95 and size.x > 0.5, "SUBSURF rounds the box off: %s" % size)
		check_eq(_misoriented_triangles(smooth), 0, "the subdivided faces point outwards")

	_bridge.join_triangles = true
	smooth = _bridge.apply_modifier_stack(cube, [{"id": "blender:modifier.SUBSURF", "values": {"levels": 2.0}}])
	_bridge.join_triangles = false
	check(smooth != null and _triangle_count(smooth) == 192, "with joined triangles the box subdivides as 6 quads into 96: %d" % (_triangle_count(smooth) if smooth != null else -1))

	var row := _bridge.apply_modifier_stack(cube, [{"type": "ARRAY", "values": {"count": 3}}, {"type": "SUBSURF", "values": {"levels": 3}, "enabled": false}])
	check(row != null, "ARRAY: " + _bridge.get_last_error())
	if row != null:
		check_eq(_triangle_count(row), 36, "ARRAY count 3 triples the triangles; the disabled modifier is skipped")
		check(row.get_aabb().is_equal_approx(AABB(Vector3(-0.5, -0.5, -0.5), Vector3(3, 1, 1))), "three boxes side by side along X: %s" % row.get_aabb())

	# Vector parameters are in Blender's axes: its Z is Godot's Y.
	row = _bridge.apply_modifier_stack(cube, [{"type": "ARRAY", "values": {"count": 2, "relative_offset_displace": Vector3(0, 0, 2)}}])
	check(row != null and row.get_aabb().is_equal_approx(AABB(Vector3(-0.5, -0.5, -0.5), Vector3(1, 3, 1))), "a Blender Z offset stacks upwards: %s" % (row.get_aabb() if row != null else AABB()))

	var bent := _bridge.apply_modifier_stack(cube, [{"type": "SIMPLE_DEFORM", "values": {"deform_method": "TWIST", "angle": 90.0, "deform_axis": "Z"}}])
	check(bent != null and is_equal_approx(bent.get_aabb().size.x, sqrt(2.0) * 1.0), "angles are given in degrees: %s" % (bent.get_aabb().size if bent != null else Vector3()))

	var small := BoxMesh.new()
	small.size = Vector3(0.5, 0.5, 0.5)
	var cut := _bridge.apply_modifier_stack(cube, [{"type": "BOOLEAN", "values": {"operation": "DIFFERENCE", "solver": "EXACT"}, "objects": {"object": {"mesh": small, "transform": Transform3D(Basis(), Vector3(0, 0.5, 0))}}}])
	check(cut != null, "BOOLEAN: " + _bridge.get_last_error())
	if cut != null:
		var positions := {}
		for vertex: Vector3 in cut.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]:
			positions[vertex.snappedf(0.001)] = true
		var floor_corners := 0
		for corner: Vector3 in [Vector3(0.25, 0.25, 0.25), Vector3(-0.25, 0.25, 0.25), Vector3(0.25, 0.25, -0.25), Vector3(-0.25, 0.25, -0.25)]:
			floor_corners += 1 if positions.has(corner) else 0
		check_eq(floor_corners, 4, "the second mesh cuts a notch into the top, where its transform puts it")
		check(cut.get_aabb().is_equal_approx(cube.get_aabb()), "the outline is unchanged")
		check_eq(_misoriented_triangles(cut), 0, "the notch faces outwards")


func _test_operators_and_primitives() -> void:
	var plane := PlaneMesh.new()
	var divided := _bridge.run_operators(plane, [{"id": "blender:mesh.subdivide", "values": {"number_cuts": 2}}], {"join_triangles": true})
	check(divided != null, "subdivide: " + _bridge.get_last_error())
	if divided != null:
		check_eq(_triangle_count(divided), 18, "a quad plane cut twice is 9 quads, 18 triangles")
		check_eq(_unique_positions(divided), 16, "on a 4 x 4 grid of vertices")
		# An ArrayMesh gives a flat bounding box a hair of thickness.
		check(divided.get_aabb().position.is_equal_approx(plane.get_aabb().position) and divided.get_aabb().size.distance_to(plane.get_aabb().size) < 0.001, "in the same place: %s" % divided.get_aabb())

	var cube := BoxMesh.new()
	var edited := _bridge.run_operators(cube, [
		{"id": "blender:mesh.inset", "values": {"thickness": 0.1, "use_individual": true}},
		{"id": "blender:object.shade_smooth", "values": {}},
		{"id": "blender:transform.translate", "values": {"value": Vector3(0, 0, 2)}, "select": "ALL"},
	], {"join_triangles": true})
	check(edited != null, "operator sequence: " + _bridge.get_last_error())
	if edited != null:
		check_eq(_triangle_count(edited), 60, "6 quads inset individually are 30 quads")
		check(edited.get_aabb().is_equal_approx(AABB(Vector3(-0.5, 1.5, -0.5), Vector3(1, 1, 1))), "translate along Blender Z lifts the mesh: %s" % edited.get_aabb())

	var monkey := _bridge.create_primitive("blender:mesh.primitive_monkey_add", {})
	check(monkey != null, "monkey: " + _bridge.get_last_error())
	if monkey != null:
		check_eq(_triangle_count(monkey), 968, "Suzanne has 968 triangles")
		check_eq(_misoriented_triangles(monkey), 0, "all facing outwards")
		var size := monkey.get_aabb().size
		check(size.x > size.y and size.y > size.z * 0.9, "upright and facing the viewer: wider than tall, %s" % size)
	# The same through the raw protocol, which also reports Blender's own counts.
	var file := _bridge.get_exchange_dir().path_join("monkey.tmsh")
	var answer := _bridge.request({"cmd": "create_primitive", "primitive": "blender:mesh.primitive_monkey_add", "values": {}, "output": file})
	check(answer.get("ok") == true and answer.get("vertex_count") == 507.0 and answer.get("polygon_count") == 500.0, "Suzanne has 507 vertices and 500 faces: %s" % answer)
	check_eq(int(TetraBlenderMesh.read_header(file).get("vertex_count", 0)), 507, "as the exchange file says")
	var from_file := TetraBlenderMesh.read_file(file)
	check(from_file != null and _triangle_count(from_file) == 968, "and the file reads back as the same mesh")
	DirAccess.remove_absolute(file)

	var cone := _bridge.create_primitive("blender:mesh.primitive_cone_add", {"vertices": 8, "radius1": 1.0, "depth": 3.0, "location": Vector3(0, 0, 10)})
	check(cone != null and cone.get_aabb().is_equal_approx(AABB(Vector3(-1, 8.5, -1), Vector3(2, 3, 2))), "primitive parameters apply; Blender's Z axis is Godot's Y: %s" % (cone.get_aabb() if cone != null else AABB()))


func _until(p_condition: Callable, p_timeout_sec: float = 120.0) -> bool:
	var deadline := Time.get_ticks_msec() + int(p_timeout_sec * 1000.0)
	while not p_condition.call():
		if Time.get_ticks_msec() > deadline:
			return false
		await process_frame
	return true


func _test_async() -> void:
	var answers := []
	_bridge.request_async({"cmd": "hello"}, func(answer: Dictionary) -> void: answers.append(answer))
	check(answers.is_empty(), "request_async returns before the answer")
	check(await _until(func() -> bool: return not answers.is_empty()), "the callback runs")
	check(not answers.is_empty() and answers[0].get("ok") == true and answers[0].get("blender_version") == _bridge.get_blender_version(), "with the answer")

	var order := []
	var plane := PlaneMesh.new()
	_bridge.create_primitive_async("blender:mesh.primitive_cube_add", {"size": 4.0}, func(result: Dictionary) -> void: order.append(["cube", result]))
	_bridge.list_modifiers_async(func(result: Dictionary) -> void: order.append(["modifiers", result]))
	_bridge.run_operators_async(plane, [{"id": "blender:mesh.subdivide", "values": {"number_cuts": 2}}], func(result: Dictionary) -> void: order.append(["subdivide", result]), {"join_triangles": true})
	_bridge.apply_modifier_stack_async(plane, [{"type": "NO_SUCH_MODIFIER"}], func(result: Dictionary) -> void: order.append(["broken", result]))
	check(_bridge.is_busy(), "queued jobs make the bridge busy")
	# A blocking request from the main thread queues behind the job in flight.
	check(_bridge.request({"cmd": "hello"}).get("ok") == true, "blocking and asynchronous requests mix")
	check(await _until(func() -> bool: return order.size() == 4), "all queued jobs finish")
	if order.size() == 4:
		check_eq(order.map(func(item: Array) -> String: return item[0]), ["cube", "modifiers", "subdivide", "broken"], "jobs run in the order they were queued")
		var cube: Dictionary = order[0][1]
		check(cube.get("ok") == true and cube.get("mesh") is ArrayMesh and (cube["mesh"] as ArrayMesh).get_aabb().size.is_equal_approx(Vector3(4, 4, 4)), "asynchronous primitive: %s" % cube.get("error"))
		check(cube.get("vertex_count") == 8 and typeof(cube.get("vertex_count")) == TYPE_INT and cube.get("polygon_count") == 6, "with the statistics Blender reported")
		check(order[1][1].get("ok") == true and (order[1][1].get("operations", []) as Array).size() > 40, "asynchronous modifier list")
		var subdivide: Dictionary = order[2][1]
		check(subdivide.get("ok") == true and subdivide.get("polygon_count") == 9 and _triangle_count(subdivide["mesh"]) == 18, "asynchronous subdivide gives 9 quads")
		check_eq(subdivide.get("results"), [{"id": "blender:mesh.subdivide", "result": "FINISHED"}], "operator results are reported")
		check(order[3][1].get("ok") == false and str(order[3][1].get("error")).contains("NO_SUCH_MODIFIER") and not order[3][1].has("mesh"), "failures arrive as ok = false")
	check(not _bridge.is_busy(), "idle again")


func _test_render() -> void:
	var grey := StandardMaterial3D.new()
	grey.albedo_color = Color(0.8, 0.8, 0.8)
	var centre := BoxMesh.new()
	centre.size = Vector3(1.2, 1.2, 1.2)
	centre.material = grey
	var marker := BoxMesh.new()
	marker.size = Vector3(0.7, 0.7, 0.7)
	# Seen from +Z looking at the origin: red is to the right, blue above.
	var scene := {
		"meshes": [
			{"mesh": centre, "transform": Transform3D(Basis(Vector3(0, 1, 0), 0.5), Vector3.ZERO)},
			{"mesh": marker, "transform": Transform3D(Basis(), Vector3(1.5, 0, 0)), "material": {"base_color": Color(1, 0, 0)}},
			{"mesh": marker, "transform": Transform3D(Basis(), Vector3(0, 1.5, 0)), "material": {"base_color": Color(0, 0, 1)}},
		],
		"camera": {"transform": Transform3D(Basis(), Vector3(0, 0, 6)), "fov_y_degrees": 40.0, "near": 0.1, "far": 100.0},
		"lights": [{"type": "sun", "transform": Transform3D(Basis.from_euler(Vector3(-0.5, 0.4, 0)), Vector3.ZERO), "color": Color(1, 1, 1), "energy": 4.0}],
		"world": {"color": Color(0.05, 0.05, 0.08), "strength": 1.0},
	}
	var settings := {"engine": "CYCLES", "samples": 8, "width": 96, "height": 96, "denoise": false}

	_progress.clear()
	_log_lines = 0
	var results := []
	_bridge.render_scene_async(scene, settings, func(result: Dictionary) -> void: results.append(result))
	check(await _until(func() -> bool: return not results.is_empty()), "the render finishes")
	if results.is_empty():
		return
	var result: Dictionary = results[0]
	check(result.get("ok") == true and result.get("image") is Image, "render: %s" % result.get("error"))
	if not result.get("image") is Image:
		return
	var image: Image = result["image"]
	check_eq(image.get_size(), Vector2i(96, 96), "image size")
	check(result.get("render_time", 0.0) > 0.0, "render time is reported")
	var screens := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens")
	if OS.get_environment("TETRA_REPO_ROOT") != "":
		DirAccess.make_dir_recursive_absolute(screens)
		image.save_png(screens.path_join("blender-render.png"))

	var front := image.get_pixel(48, 48)
	check(front.r > 0.25 and absf(front.r - front.g) < 0.1 and absf(front.r - front.b) < 0.1, "the grey cube is in the middle: %s" % front)
	# The centre cube is turned about Y so that its -X side, which the sun
	# barely reaches, shows as a darker strip on the left.
	var side := image.get_pixel(35, 48)
	check(side.r > 0.03 and side.r < front.r * 0.6, "the cube's rotation is the right way round: side %s, front %s" % [side, front])
	var right := image.get_pixel(81, 48)
	check(right.r > 0.3 and right.r > right.g * 3.0 and right.r > right.b * 3.0, "the red marker is on the right: %s" % right)
	var top := image.get_pixel(48, 15)
	check(top.b > 0.3 and top.b > top.r * 3.0 and top.b > top.g * 3.0, "the blue marker is above: %s" % top)
	for empty: Vector2i in [Vector2i(15, 48), Vector2i(48, 84), Vector2i(3, 3)]:
		var pixel := image.get_pixel(empty.x, empty.y)
		check(pixel.r < 0.4 and pixel.g < 0.4 and pixel.b > pixel.r, "only the bluish world at %s: %s" % [empty, pixel])

	await process_frame
	var stages := _progress.map(func(event: Dictionary) -> String: return event.get("stage", ""))
	check(stages.size() >= 3 and stages.front() == "start" and stages.back() == "finish" and stages.has("sample"), "progress events: %s" % [stages])
	var last_sample: Dictionary = _progress.filter(func(event: Dictionary) -> bool: return event.get("stage") == "sample").back()
	check(last_sample.get("sample") == 8.0 and last_sample.get("samples") == 8.0 and last_sample.get("fraction") == 1.0 and last_sample.get("cmd") == "render", "the last sample event: %s" % last_sample)
	check(_log_lines > 0, "Blender's own output arrives as log lines")

	var blocking := _bridge.render_scene(scene, {"samples": 1, "width": 32, "height": 24})
	check(blocking != null and blocking.get_size() == Vector2i(32, 24), "blocking render: " + _bridge.get_last_error())


func _test_files() -> void:
	var paint := StandardMaterial3D.new()
	paint.resource_name = "Paint"
	paint.albedo_color = Color(0.2, 0.6, 1.0)
	paint.metallic = 0.25
	paint.roughness = 0.75
	var box := BoxMesh.new()
	box.size = Vector3(1, 2, 3)
	box.material = paint
	var path := tmp_dir().path_join("bridge-export.glb")
	DirAccess.remove_absolute(path)
	var placed := Transform3D(Basis(Vector3(0, 1, 0), PI / 2.0), Vector3(1, 2, 3))
	check_eq(_bridge.export_meshes(path, [{"mesh": box, "name": "Crate", "transform": placed}, {"mesh": SphereMesh.new(), "name": "Ball"}]), OK, "export to glb: " + _bridge.get_last_error())
	check(FileAccess.file_exists(path), "the glb exists")

	# Godot's own glTF importer is an independent judge of the axis conversion.
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	if document.append_from_file(path, state) == OK:
		var root := document.generate_scene(state)
		var crate := root.find_child("Crate", true, false) as Node3D
		check(crate != null and crate.transform.is_equal_approx(placed), "Godot reads the exported object where it was: %s" % (crate.transform if crate != null else Transform3D()))
		root.free()
	else:
		check(false, "Godot could not read the exported glb")

	var imported := _bridge.import_meshes(path)
	check_eq(imported.size(), 2, "both objects are imported: " + _bridge.get_last_error())
	for item: Dictionary in imported:
		if item["name"] != "Crate":
			continue
		check((item["transform"] as Transform3D).is_equal_approx(placed), "the object transform survives: %s" % item["transform"])
		var mesh: ArrayMesh = item["mesh"]
		check(mesh.get_aabb().is_equal_approx(box.get_aabb()), "the mesh survives: %s" % mesh.get_aabb())
		check_eq(_triangle_count(mesh), 12, "12 triangles")
		var material := mesh.surface_get_material(0) as StandardMaterial3D
		check(material != null and material.resource_name == "Paint", "the material is recreated by name")
		if material != null:
			var albedo := material.albedo_color
			check(absf(albedo.r - 0.2) < 0.01 and absf(albedo.g - 0.6) < 0.01 and absf(albedo.b - 1.0) < 0.01, "with its colour: %s" % albedo)
			check(is_equal_approx(material.metallic, 0.25) and is_equal_approx(material.roughness, 0.75), "metallic and roughness")
	DirAccess.remove_absolute(path)


func _test_errors() -> void:
	var pid := int(_bridge.get_server_info().get("pid", 0))
	var box := BoxMesh.new()

	var answer := _bridge.request({"cmd": "no_such_command"})
	check(answer.get("ok") == false and str(answer.get("error")).contains("Unknown command"), "unknown command: %s" % answer)
	answer = _bridge.request({})
	check(answer.get("ok") == false and str(answer.get("error")).contains("cmd"), "a request without a command: %s" % answer)
	answer = _bridge.request({"cmd": "apply_modifiers", "mesh": "/no/such/file.tmsh", "output": "/tmp/x.tmsh"})
	check(answer.get("ok") == false and str(answer.get("error")).contains("FileNotFoundError") and answer.has("traceback"), "Blender-side exceptions come back with a traceback: %s" % answer.get("error"))

	check(_bridge.apply_modifier_stack(box, [{"type": "NOT_A_MODIFIER"}]) == null and _bridge.get_last_error().contains("NOT_A_MODIFIER"), "unknown modifier: " + _bridge.get_last_error())
	check(_bridge.apply_modifier_stack(null, []) == null and _bridge.get_last_error() != "", "no mesh: " + _bridge.get_last_error())
	check(_bridge.apply_modifier_stack(box, ["SUBSURF"]) == null, "a malformed stack entry")
	check(_bridge.run_operators(box, [{"id": "blender:mesh.subdivide", "values": {"quadcorner": "WRONG"}}]) == null and _bridge.get_last_error().contains("TypeError"), "bad enum value: " + _bridge.get_last_error())
	check(_bridge.run_operators(box, [{"id": "blender:mesh.no_such_operator"}]) == null and _bridge.get_last_error().contains("Unknown Blender operator"), "unknown operator: " + _bridge.get_last_error())
	check(_bridge.create_primitive("blender:mesh.subdivide", {}) == null and _bridge.get_last_error().contains("not a mesh primitive"), "not a primitive: " + _bridge.get_last_error())
	check(_bridge.import_meshes("/no/such/model.obj").is_empty() and _bridge.get_last_error().contains("File not found"), "missing import: " + _bridge.get_last_error())
	check(_bridge.render_scene({"meshes": [{"mesh": box}]}, {"engine": "EEVEE"}) == null and _bridge.get_last_error().contains("CYCLES"), "unsupported engine: " + _bridge.get_last_error())
	check_eq(_bridge.export_meshes(tmp_dir().path_join("bridge.unknown"), [{"mesh": box}]), FAILED, "unknown export format")

	var again := _bridge.apply_modifier_stack(box, [])
	check(again != null and _triangle_count(again) == 12 and _bridge.get_last_error() == "", "the bridge keeps working after errors")
	check_eq(int(_bridge.get_server_info().get("pid", 0)), pid, "in the same Blender process")
	check(DirAccess.get_files_at(_bridge.get_exchange_dir()).is_empty(), "no exchange files are left behind: %s" % [DirAccess.get_files_at(_bridge.get_exchange_dir())])


func _process_exists(p_pid: int) -> bool:
	return DirAccess.dir_exists_absolute("/proc/%d" % p_pid)


func _test_lifetime() -> void:
	# A request that outlives its timeout costs the Blender process.
	var pid := int(_bridge.get_server_info().get("pid", 0))
	check(pid > 0 and _process_exists(pid), "Blender runs as process %d" % pid)
	var slow := {"cmd": "render", "output": tmp_dir().path_join("never.png"), "settings": {"samples": 4096, "width": 2048, "height": 2048}, "scene": {"world": {"strength": 1.0}}}
	var answer := _bridge.request(slow, 0.3)
	check(answer.get("ok") == false and str(answer.get("error")).contains("did not answer"), "timeout: %s" % answer)
	check(not _process_exists(pid), "the unresponsive Blender is gone")
	check(not _bridge.is_running(), "and the bridge knows")

	_started = 0
	answer = _bridge.request({"cmd": "hello"})
	var second_pid := int(answer.get("pid", 0))
	check(answer.get("ok") == true and second_pid != pid and _process_exists(second_pid), "the next request starts a new Blender")
	await process_frame
	check_eq(_started, 1, "started is emitted again")

	_stopped = 0
	_bridge.stop()
	check(not _bridge.is_running(), "stopped")
	check(not _process_exists(second_pid), "stop() leaves no process behind")
	await process_frame
	await process_frame
	check_eq(_stopped, 1, "stopped signal")

	# stop() during a request in flight interrupts it.
	var interrupted := []
	_bridge.request_async(slow, func(result: Dictionary) -> void: interrupted.append(result))
	check(await _until(func() -> bool: return _bridge.is_running() and _bridge.is_busy()), "the slow render is running")
	var third_pid := int(_bridge.get_server_info().get("pid", 0))
	await create_timer(0.2).timeout
	_bridge.stop()
	check(not _process_exists(third_pid), "stop() ends a busy Blender")
	check(await _until(func() -> bool: return not interrupted.is_empty()), "the interrupted job reports back")
	check(not interrupted.is_empty() and interrupted[0].get("ok") == false, "as a failure: %s" % [interrupted])

	# Dropping the last reference ends Blender and removes the exchange directory.
	check(_bridge.request({"cmd": "hello"}).get("ok") == true, "restarted once more")
	var last_pid := int(_bridge.get_server_info().get("pid", 0))
	var exchange := _bridge.get_exchange_dir()
	check(DirAccess.dir_exists_absolute(exchange), "the exchange directory exists while the bridge lives")
	_bridge = null
	check(not _process_exists(last_pid), "freeing the bridge ends its Blender")
	check(not DirAccess.dir_exists_absolute(exchange), "and removes its exchange directory")
