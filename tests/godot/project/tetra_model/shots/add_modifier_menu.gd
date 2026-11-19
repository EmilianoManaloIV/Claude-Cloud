extends RefCounted
## Screenshot driver: the "Add Modifier" popup of the Modifiers tab, grouped
## as Blender groups its modifiers.

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var blender := TetraBlenderEditorPlugin.get_singleton()
	blender.show_tab("modifiers")
	var cube := await ShotScene.add_primitive(p_tree, "blender:mesh.primitive_cube_add")
	if cube == null:
		return
	blender.load_schemas(TetraBlenderEditorPlugin.SCHEMA_MODIFIERS)
	var loaded := await ShotScene.wait_until(p_tree, func() -> bool:
		return blender.get_schema_state(TetraBlenderEditorPlugin.SCHEMA_MODIFIERS) == TetraBlenderEditorPlugin.SCHEMA_LOADED)
	await p_tree.create_timer(0.3).timeout
	blender.get_dock().get_modifiers_tab().popup_add_menu()
	print("screenshot add modifier menu: loaded %s, %d modifiers" % [loaded, blender.get_schemas(TetraBlenderEditorPlugin.SCHEMA_MODIFIERS).size()])
