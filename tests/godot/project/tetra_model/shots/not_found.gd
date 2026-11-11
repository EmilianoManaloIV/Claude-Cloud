extends RefCounted
## Screenshot driver: the dock when no Blender can be found.


func drive(p_tree: SceneTree) -> void:
	var blender := TetraBlenderEditorPlugin.get_singleton()
	# Without the variable the test machine has neither a bpy Python nor a
	# Blender executable configured.
	OS.unset_environment("TETRA_BLENDER_PYTHON")
	blender.restart_bridge()
	blender.show_tab("modifiers")
	await p_tree.create_timer(0.3).timeout
	print("screenshot not found: status %d '%s' hint '%s'" % [blender.get_status(), blender.get_status_text(), blender.get_status_hint()])
