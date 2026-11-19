extends RefCounted
## Screenshot driver: the parameter dialog of "Blender > Add Mesh > Torus".

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var blender := TetraBlenderEditorPlugin.get_singleton()
	blender.load_schemas(TetraBlenderEditorPlugin.SCHEMA_PRIMITIVES)
	var loaded := await ShotScene.wait_until(p_tree, func() -> bool:
		return blender.get_schema_state(TetraBlenderEditorPlugin.SCHEMA_PRIMITIVES) == TetraBlenderEditorPlugin.SCHEMA_LOADED)
	blender.popup_add_primitive("blender:mesh.primitive_torus_add")
	var labels := blender.get_schemas(TetraBlenderEditorPlugin.SCHEMA_PRIMITIVES).map(func(s: Dictionary) -> String: return s["label"])
	print("screenshot add primitive dialog: loaded %s, primitives %s" % [loaded, labels])
