extends RefCounted
## Screenshot driver: the Operators tab with a search for "bevel", the
## operator's parameters, and its result on a cube.

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var blender := TetraBlenderEditorPlugin.get_singleton()

	var cube := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_cube_add", {"size": 1.6})
	if cube == null:
		return
	cube.position = Vector3(0, 0.8, 0)
	cube.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.35, 0.6, 0.9)))

	blender.show_tab("operators")
	var loaded := await ShotScene.wait_until(p_tree, func() -> bool:
		return blender.get_schema_state(TetraBlenderEditorPlugin.SCHEMA_OPERATORS) == TetraBlenderEditorPlugin.SCHEMA_LOADED)
	var tab := blender.get_dock().get_operators_tab()
	tab.get_browser().set_filter_text("bevel")
	tab.select_operator(tab.get_browser().get_selected_id())
	tab.get_param_panel().set_values({"offset": 0.25, "segments": 4})

	var finished: Array = []
	blender.operator_finished.connect(func(_node: Node, ok: bool, message: String) -> void: finished.append([ok, message]), CONNECT_ONE_SHOT)
	tab.run()
	await ShotScene.wait_until(p_tree, func() -> bool: return not finished.is_empty())
	await p_tree.create_timer(0.5).timeout
	print("screenshot operators: loaded %s (%d operators), selected '%s', result %s" % [
		loaded, tab.get_browser().get_operations().size(), tab.get_selected_operator(), finished])
