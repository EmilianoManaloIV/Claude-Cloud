extends RefCounted
## Screenshot driver: a cube carrying Bevel, Subdivision Surface and Array
## modifiers evaluated by Blender, with the dock on the Modifiers tab.

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var blender := TetraBlenderEditorPlugin.get_singleton()
	blender.show_tab("modifiers")

	var cube := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_cube_add", {"size": 1.0})
	if cube == null:
		return
	cube.position = Vector3(-1.3, 0.5, 0)
	cube.mesh.surface_set_material(0, ShotScene.colored_material(Color(0.9, 0.55, 0.2)))

	blender.add_modifier(cube, "BEVEL", {"width": 0.12, "segments": 2})
	blender.add_modifier(cube, "SUBSURF", {"levels": 2})
	blender.add_modifier(cube, "ARRAY", {"count": 3, "relative_offset_displace": Vector3(1.3, 0, 0)})
	var evaluated := await ShotScene.wait_for_modifiers(p_tree, cube)
	blender.get_dock().get_modifiers_tab().select_modifier(0)
	await p_tree.create_timer(0.5).timeout

	var info := blender.get_evaluation_info(cube)
	print("screenshot modifiers: evaluated %s, stack %s, result %s, status '%s'" % [
		evaluated, blender.get_modifier_stack(cube).map(func(m: Dictionary) -> String: return m["type"]), info, blender.get_status_text()])
