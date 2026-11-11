extends RefCounted
## Screenshot driver: the "About Tetra Studio" dialog.


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	# The engine modules announce themselves at start-up. Where one is not part
	# of this build yet, an example entry shows how its row looks.
	var examples := [
		["GEGL", "0.4.48", true, "example entry added by the screenshot driver"],
		["MLT", "7.22.0", true, "example entry added by the screenshot driver"],
		["Blender", "", false, "example entry added by the screenshot driver: no bpy module or blender executable was found"],
	]
	for example: Array in examples:
		if TetraEngineRegistry.get_engine(example[0]).is_empty():
			TetraEngineRegistry.add_engine(example[0], example[1], example[2], example[3])
	TetraEditorPlugin.get_singleton().show_about()
	await p_tree.create_timer(0.5).timeout
	print("screenshot about dialog size %s, engines %s" % [TetraEditorPlugin.get_singleton().get_about_dialog().size, TetraEngineRegistry.get_engines()])
