extends "res://tetra_test.gd"
## TetraBlenderScene: the modifier stack kept in node metadata and the scene
## description handed to the Cycles render (materials, lights in Blender's
## units, world, camera). The last part renders through Blender and checks
## that the picture is framed and lit as Godot would.


func run() -> void:
	_test_modifier_metadata()
	_test_materials()
	_test_lights()
	_test_world()
	_test_camera()
	_test_gather()
	await _test_render_matches_godot()


func _test_modifier_metadata() -> void:
	var node := MeshInstance3D.new()
	var box := BoxMesh.new()
	node.mesh = box
	check(not TetraBlenderScene.has_modifier_stack(node), "a fresh node has no stack")
	check_eq(TetraBlenderScene.get_modifier_stack(node), [], "empty stack")
	check(TetraBlenderScene.get_base_mesh(node) == box, "without modifiers the base mesh is the node's mesh")

	var modifier := TetraBlenderScene.make_modifier("blender:modifier.SUBSURF", {"levels": 2}, "Subdivision Surface")
	check_eq(modifier, {"type": "SUBSURF", "label": "Subdivision Surface", "values": {"levels": 2}, "enabled": true, "object_paths": {}}, "make_modifier")
	check_eq(TetraBlenderScene.make_modifier("BEVEL")["label"], "Bevel", "label falls back to the type")

	TetraBlenderScene.set_modifier_stack(node, box, [modifier])
	check(node.has_meta("tetra_blender_modifiers") and node.has_meta("tetra_blender_source_mesh"), "both metadata entries are written")
	check_eq(String(TetraBlenderScene.get_modifiers_meta()), "tetra_blender_modifiers", "metadata name")
	check_eq(String(TetraBlenderScene.get_source_mesh_meta()), "tetra_blender_source_mesh", "metadata name")
	check(TetraBlenderScene.has_modifier_stack(node), "has a stack")
	check(TetraBlenderScene.get_source_mesh(node) == box, "source mesh")

	# The evaluated mesh replaces the node's mesh; the base stays the source.
	node.mesh = SphereMesh.new()
	check(TetraBlenderScene.get_base_mesh(node) == box, "with modifiers the base mesh is the source")

	var copy := TetraBlenderScene.get_modifier_stack(node)
	copy[0]["values"]["levels"] = 5
	check_eq(TetraBlenderScene.get_modifier_stack(node)[0]["values"]["levels"], 2, "get_modifier_stack returns a deep copy")

	TetraBlenderScene.set_modifier_stack(node, null, [])
	check(not node.has_meta("tetra_blender_modifiers") and not node.has_meta("tetra_blender_source_mesh"), "an empty stack removes the metadata")
	node.free()

	# Object parameters are resolved to the other node's mesh and relative transform.
	var parent := Node3D.new()
	root.add_child(parent)
	var a := MeshInstance3D.new()
	a.name = "A"
	a.mesh = BoxMesh.new()
	a.position = Vector3(1, 0, 0)
	parent.add_child(a)
	var b := MeshInstance3D.new()
	b.name = "B"
	b.mesh = SphereMesh.new()
	b.position = Vector3(1.5, 2, 0)
	parent.add_child(b)
	var boolean := TetraBlenderScene.make_modifier("BOOLEAN", {"operation": "DIFFERENCE"})
	boolean["object_paths"] = {"object": NodePath("../B"), "missing": NodePath("../Nowhere")}
	var disabled := TetraBlenderScene.make_modifier("BEVEL")
	disabled["enabled"] = false
	var bridge_stack := TetraBlenderScene.to_bridge_stack(a, [boolean, disabled])
	check_eq(bridge_stack.size(), 2, "disabled modifiers stay in the stack")
	check_eq(bridge_stack[0]["type"], "BOOLEAN", "type")
	check_eq(bridge_stack[0]["values"], {"operation": "DIFFERENCE"}, "values")
	check_eq(bridge_stack[0]["objects"].keys(), ["object"], "only paths that lead to a mesh become objects")
	check(bridge_stack[0]["objects"]["object"]["mesh"] == b.mesh, "the object's mesh")
	check(bridge_stack[0]["objects"]["object"]["transform"].is_equal_approx(Transform3D(Basis(), Vector3(0.5, 2, 0))), "the object's transform is relative to the modified node")
	check_eq(bridge_stack[1], {"type": "BEVEL", "values": {}, "enabled": false}, "a plain entry has no objects")
	parent.free()


func _test_materials() -> void:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.1, 0.2)
	material.metallic = 0.25
	material.roughness = 0.4
	var plain := TetraBlenderScene.material_to_dictionary(material)
	check_eq(plain.keys(), ["base_color", "metallic", "roughness", "emission", "emission_strength"], "material keys")
	check_eq(plain["base_color"], Color(0.8, 0.1, 0.2), "base colour")
	check_near(plain["metallic"], 0.25, 1e-6, "metallic")
	check_near(plain["roughness"], 0.4, 1e-6, "roughness")
	check_eq(plain["emission"], Color(0, 0, 0), "no emission colour unless emission is enabled")
	check_eq(plain["emission_strength"], 0.0, "no emission strength unless emission is enabled")
	material.emission_enabled = true
	material.emission = Color(1, 0.5, 0)
	material.emission_energy_multiplier = 3.0
	var emissive := TetraBlenderScene.material_to_dictionary(material)
	check_eq(emissive["emission"], Color(1, 0.5, 0), "emission colour")
	check_eq(emissive["emission_strength"], 3.0, "emission strength")
	check_eq(TetraBlenderScene.material_to_dictionary(null), {"base_color": Color(1, 1, 1), "metallic": 0.0, "roughness": 1.0, "emission": Color(0, 0, 0), "emission_strength": 0.0}, "no material is Godot's white fallback")
	check_eq(TetraBlenderScene.material_to_dictionary(ShaderMaterial.new())["base_color"], Color(1, 1, 1), "a shader material cannot be described")

	var default_material := TetraBlenderScene.make_default_material()
	check_eq(default_material.albedo_color, Color(0.8, 0.8, 0.8), "Blender's default base colour")
	check_eq(default_material.roughness, 0.5, "Blender's default roughness")


func _test_lights() -> void:
	var sun := DirectionalLight3D.new()
	sun.light_energy = 2.0
	sun.light_color = Color(1, 0.9, 0.8)
	sun.light_angular_distance = 1.5
	var sun_light := TetraBlenderScene.light_to_dictionary(sun)
	check_eq(sun_light["type"], "sun", "DirectionalLight3D is a sun")
	check_near(sun_light["energy"], 2.0 * PI, 1e-5, "sun strength is pi times the energy")
	check_eq(sun_light["color"], Color(1, 0.9, 0.8), "light colour")
	check_near(sun_light["angle_degrees"], 1.5, 1e-5, "angular size")
	sun.free()

	var omni := OmniLight3D.new()
	omni.light_energy = 1.5
	omni.omni_range = 8.0
	omni.omni_attenuation = 2.0
	omni.light_size = 0.2
	var point := TetraBlenderScene.light_to_dictionary(omni)
	check_eq(point["type"], "point", "OmniLight3D is a point light")
	check_near(point["energy"], 1.5 * 4.0 * PI * PI, 1e-3, "inverse-square attenuation: watts = 4 pi^2 energy")
	check_near(point["radius"], 0.2, 1e-5, "light size is the radius")
	# Godot's default falls off with 1/d; matched halfway to the range (4 m).
	omni.omni_attenuation = 1.0
	check_near(TetraBlenderScene.light_to_dictionary(omni)["energy"], 1.5 * 4.0 * PI * PI * 4.0, 1e-2, "1/d attenuation is matched at half the range")
	omni.free()

	var spot := SpotLight3D.new()
	spot.light_energy = 1.0
	spot.spot_range = 2.0
	spot.spot_attenuation = 2.0
	spot.spot_angle = 30.0
	spot.spot_angle_attenuation = 1.0
	spot.position = Vector3(1, 2, 3)
	var spot_light := TetraBlenderScene.light_to_dictionary(spot)
	check_eq(spot_light["type"], "spot", "SpotLight3D is a spot light")
	check_near(spot_light["energy"], 4.0 * PI * PI, 1e-3, "spot watts")
	check_near(spot_light["spot_angle_degrees"], 30.0, 1e-5, "spot angle")
	check_near(spot_light["spot_blend"], 0.5, 1e-5, "spot blend")
	check_eq(spot_light["transform"].origin, Vector3(1, 2, 3), "outside the tree the local transform is used")
	spot.free()


func _test_world() -> void:
	var clear_color: Color = ProjectSettings.get_setting("rendering/environment/defaults/default_clear_color")
	clear_color.a = 1.0
	check_eq(TetraBlenderScene.environment_to_world(null), {"color": clear_color, "strength": 1.0}, "no environment: the default clear colour")

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.1, 0.2, 0.3)
	environment.background_energy_multiplier = 1.5
	check_eq(TetraBlenderScene.environment_to_world(environment), {"color": Color(0.1, 0.2, 0.3), "strength": 1.5}, "background colour")

	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.2, 0.4, 0.8)
	sky_material.sky_horizon_color = Color(0.6, 0.6, 0.6)
	var sky := Sky.new()
	sky.sky_material = sky_material
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.background_energy_multiplier = 1.0
	var world := TetraBlenderScene.environment_to_world(environment)
	check(world["color"].is_equal_approx(Color(0.4, 0.5, 0.7)), "a procedural sky is averaged: %s" % world["color"])


func _test_camera() -> void:
	var camera := Camera3D.new()
	camera.fov = 60.0
	camera.near = 0.1
	camera.far = 200.0
	camera.position = Vector3(0, 1, 5)
	var description := TetraBlenderScene.camera_to_dictionary(camera, 16.0 / 9.0)
	check_near(description["fov_y_degrees"], 60.0, 1e-5, "the vertical field of view is passed on")
	check_near(description["near"], 0.1, 1e-6, "near")
	check_near(description["far"], 200.0, 1e-4, "far")
	check_eq(description["transform"].origin, Vector3(0, 1, 5), "camera transform")
	check(not description.has("ortho_size"), "a perspective camera has no ortho size")

	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = 90.0
	check_near(TetraBlenderScene.camera_to_dictionary(camera, 2.0)["fov_y_degrees"], rad_to_deg(2.0 * atan(0.5)), 1e-4, "a horizontal field of view is converted")

	camera.keep_aspect = Camera3D.KEEP_HEIGHT
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 4.0
	var ortho := TetraBlenderScene.camera_to_dictionary(camera, 2.0)
	check_near(ortho["ortho_size"], 4.0, 1e-5, "ortho size is the visible height")
	check(not ortho.has("fov_y_degrees"), "an orthogonal camera has no field of view")
	camera.free()

	check_eq(TetraBlenderScene.fit_resolution(960, 540, 2.0, true), Vector2i(960, 480), "the height follows the aspect ratio")
	check_eq(TetraBlenderScene.fit_resolution(960, 540, 2.0, false), Vector2i(960, 540), "or stays as given")
	check_eq(TetraBlenderScene.fit_resolution(1, 1, 1.0, false), Vector2i(4, 4), "a picture has at least 4 pixels a side")


func _test_gather() -> void:
	var scene := _make_scene()
	var gathered := TetraBlenderScene.gather_scene(scene)
	check_eq(gathered.keys(), ["meshes", "lights", "world"], "gather_scene keys")
	check_eq(gathered["meshes"].size(), 2, "the hidden mesh and the one below a hidden node are left out")
	var floor_entry: Dictionary = gathered["meshes"][0]
	check_eq(floor_entry["name"], "Floor", "tree order")
	check_eq(floor_entry["materials"].size(), 1, "one material per surface")
	check_eq(floor_entry["materials"][0]["base_color"], Color(1, 1, 1), "the floor's material")
	var cube_entry: Dictionary = gathered["meshes"][1]
	check_eq(cube_entry["transform"].origin, Vector3(0, 0.5, 0), "world transform of a nested mesh")
	check_eq(cube_entry["materials"][0]["base_color"], Color(1, 0, 0), "the override material wins over the mesh's own")
	check_eq(gathered["lights"].size(), 1, "the invisible light is left out")
	check_eq(gathered["lights"][0]["type"], "sun", "the sun")
	check_eq(gathered["world"], {"color": Color(0, 0, 0), "strength": 1.0}, "the WorldEnvironment's background")

	check_eq(TetraBlenderScene.find_cameras(scene).size(), 2, "both cameras are found")
	check_eq(TetraBlenderScene.find_scene_camera(scene).name, &"Second", "the current camera is preferred")
	scene.free()
	var empty := Node3D.new()
	check(TetraBlenderScene.find_scene_camera(empty) == null, "no camera")
	empty.free()


## A white floor, a red cube, a sun straight down, black world, and things
## that must be ignored.
func _make_scene() -> Node3D:
	var scene := Node3D.new()
	root.add_child(scene)

	var white := StandardMaterial3D.new()
	white.roughness = 1.0
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	plane.material = white
	var floor_node := MeshInstance3D.new()
	floor_node.name = "Floor"
	floor_node.mesh = plane
	scene.add_child(floor_node)

	var group := Node3D.new()
	group.position = Vector3(0, 0.5, 0)
	scene.add_child(group)
	var blue := StandardMaterial3D.new()
	blue.albedo_color = Color(0, 0, 1)
	var box := BoxMesh.new()
	box.material = blue
	var red := StandardMaterial3D.new()
	red.albedo_color = Color(1, 0, 0)
	red.roughness = 1.0
	var cube := MeshInstance3D.new()
	cube.name = "Cube"
	cube.mesh = box
	cube.set_surface_override_material(0, red)
	group.add_child(cube)

	var hidden := MeshInstance3D.new()
	hidden.mesh = BoxMesh.new()
	hidden.visible = false
	scene.add_child(hidden)
	var hidden_group := Node3D.new()
	hidden_group.visible = false
	scene.add_child(hidden_group)
	var below_hidden := MeshInstance3D.new()
	below_hidden.mesh = BoxMesh.new()
	hidden_group.add_child(below_hidden)

	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-PI / 2, 0, 0)
	sun.light_energy = 0.5
	scene.add_child(sun)
	var dark := OmniLight3D.new()
	dark.visible = false
	scene.add_child(dark)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0, 0, 0)
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	scene.add_child(world_environment)

	var first := Camera3D.new()
	first.name = "First"
	scene.add_child(first)
	var second := Camera3D.new()
	second.name = "Second"
	second.position = Vector3(0, 10, 0)
	second.rotation = Vector3(-PI / 2, 0, 0)
	second.fov = 30.0
	second.current = true
	scene.add_child(second)
	return scene


func _test_render_matches_godot() -> void:
	if TetraBlenderBridge.find_launcher()["kind"] == "":
		print("SKIPPED the render part of tetra_model/test_scene_gather.gd: no Blender")
		return
	var scene := _make_scene()
	var camera := TetraBlenderScene.find_scene_camera(scene)
	var description := TetraBlenderScene.gather_scene(scene)
	description["camera"] = TetraBlenderScene.camera_to_dictionary(camera, 2.0)
	var bridge := TetraBlenderBridge.new()
	var image := bridge.render_scene(description, {"samples": 16, "width": 200, "height": 100})
	if not check(image != null, "render: " + bridge.get_last_error()):
		scene.free()
		return
	image.save_png(ProjectSettings.globalize_path("res://").path_join("../../../build/screens/blender-scene-gather.png"))
	check_eq(image.get_size(), Vector2i(200, 100), "render size")

	# A sun of energy 0.5 straight down on a white floor is 0.5 in Godot
	# (sRGB 188) and the red cube's top is 0.5 red.
	var floor_pixel := image.get_pixel(20, 50)
	check_near(floor_pixel.srgb_to_linear().r, 0.5, 0.03, "the floor is as bright as in Godot: %s" % floor_pixel)
	check_near(floor_pixel.g, floor_pixel.r, 0.02, "the floor is white")
	var cube_pixel := image.get_pixel(100, 50)
	check_near(cube_pixel.srgb_to_linear().r, 0.5, 0.03, "the cube's top is lit like the floor: %s" % cube_pixel)
	# The Principled BSDF adds a faint white specular reflection to the red.
	check(cube_pixel.g < 0.25 and cube_pixel.b < 0.25, "the cube is red (the override material): %s" % cube_pixel)

	# Framing: the camera looks down from 10 m with a vertical field of view
	# of 30 degrees; the cube's top (1 m wide, 9 m away) must cover what
	# Godot's own projection says, to the pixel.
	root.size = Vector2i(200, 100)
	var right_edge := _unproject(camera, Vector3(0.5, 1.0, 0.0), Vector2(200, 100))
	var expected_half_width := right_edge.x - 100.0
	check_near(expected_half_width, 100.0 * 0.5 / (9.0 * tan(deg_to_rad(15.0))) / 2.0, 0.01, "Godot's projection of the cube edge")
	# A pixel counts as cube when it is nearer to the cube's green than to the floor's.
	var threshold := (cube_pixel.g + floor_pixel.g) * 0.5
	var red_columns := 0
	for x in 200:
		if image.get_pixel(x, 50).g < threshold:
			red_columns += 1
	check_near(red_columns, 2.0 * expected_half_width, 1.5, "the cube is as wide in the render as in Godot's projection")
	var red_rows := 0
	for y in 100:
		if image.get_pixel(100, y).g < threshold:
			red_rows += 1
	check_near(red_rows, 2.0 * expected_half_width, 1.5, "and as high")
	# Orientation: +X of the scene is to the right, -Z is up in this view.
	check_near(right_edge.x, 100.0 + expected_half_width, 0.01, "+X projects to the right")
	check(image.get_pixel(int(right_edge.x) - 2, 50).g < threshold and image.get_pixel(int(right_edge.x) + 2, 50).g > threshold, "the cube's right edge is where Godot projects it")
	bridge.stop()
	scene.free()


## Camera3D.unproject_position needs a viewport; this is the same projection
## for a camera with a vertical field of view.
func _unproject(p_camera: Camera3D, p_point: Vector3, p_size: Vector2) -> Vector2:
	var local := p_camera.global_transform.affine_inverse() * p_point
	var half_height := tan(deg_to_rad(p_camera.fov) * 0.5) * -local.z
	var half_width := half_height * p_size.x / p_size.y
	return Vector2((local.x / half_width * 0.5 + 0.5) * p_size.x, (0.5 - local.y / half_height * 0.5) * p_size.y)
