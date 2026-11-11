extends RefCounted
## Screenshot driver: a small lit scene built from Blender primitives and a
## finished Cycles render of it on the Render tab, next to the viewport.

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var blender := TetraBlenderEditorPlugin.get_singleton()

	var monkey := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_monkey_add", {"size": 1.5})
	if monkey == null:
		return
	var scene := EditorInterface.get_edited_scene_root()
	monkey.position = Vector3(0, 0.75, 0)
	monkey.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.85, 0.5, 0.15), 0.35))
	blender.add_modifier(monkey, "SUBSURF", {"levels": 2})

	var floor_node := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_plane_add", {"size": 14.0}, scene)
	floor_node.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.75, 0.75, 0.72), 0.8))
	var torus := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_torus_add", {"major_radius": 0.55, "minor_radius": 0.2, "major_segments": 64, "minor_segments": 24}, scene)
	torus.position = Vector3(-1.9, 0.2, 0.5)
	torus.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.2, 0.4, 0.9), 0.25, 0.9))
	var sphere := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_uv_sphere_add", {"radius": 0.5, "segments": 48, "ring_count": 24}, scene)
	sphere.position = Vector3(1.8, 0.5, 0.3)
	sphere.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.85, 0.12, 0.12), 0.3))
	var lamp := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_ico_sphere_add", {"radius": 0.22, "subdivisions": 3}, scene)
	lamp.position = Vector3(0.9, 0.22, 1.5)
	var glow := ShotScene.colored_material(Color(1.0, 0.8, 0.3))
	glow.emission_enabled = true
	glow.emission = Color(1.0, 0.7, 0.2)
	glow.emission_energy_multiplier = 4.0
	lamp.mesh.surface_set_material(0, glow)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-50, -35, 0)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	scene.add_child(sun)
	sun.owner = scene
	var fill := OmniLight3D.new()
	fill.name = "Fill"
	fill.position = Vector3(-2.0, 2.0, 2.0)
	fill.light_color = Color(0.6, 0.75, 1.0)
	fill.light_energy = 1.5
	fill.omni_range = 7.0
	fill.shadow_enabled = true
	scene.add_child(fill)
	fill.owner = scene
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.32, 0.4, 0.52)
	var world := WorldEnvironment.new()
	world.name = "World"
	world.environment = environment
	scene.add_child(world)
	world.owner = scene

	await ShotScene.wait_for_modifiers(p_tree, monkey)
	EditorInterface.get_selection().clear()
	blender.show_tab("render")
	await p_tree.create_timer(0.5).timeout

	var finished: Array = []
	var samples_seen: Array[int] = []
	blender.render_progress.connect(func(_fraction: float, sample: int, _samples: int) -> void: samples_seen.append(sample))
	blender.render_finished.connect(func(ok: bool, message: String) -> void: finished.append([ok, message]), CONNECT_ONE_SHOT)
	var tab := blender.get_dock().get_render_tab()
	tab.get_param_panel().set_values({"width": 800, "samples": 96, "denoise": true})
	tab.render()
	await ShotScene.wait_until(p_tree, func() -> bool: return not finished.is_empty(), 240.0)
	await p_tree.create_timer(0.5).timeout
	var image := blender.get_last_render()
	if image != null:
		image.save_png(OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens/blender-cycles-render.png"))
	print("screenshot render: %s, %d progress events, info %s" % [finished, samples_seen.size(), blender.get_last_render_info()])
