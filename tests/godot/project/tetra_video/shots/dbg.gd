extends RefCounted
func drive(p_tree: SceneTree) -> void:
	var ws: Object = ClassDB.class_call_static("TetraVideoWorkspace", "get_singleton")
	var c: RefCounted = load("res://tetra_video/shots/video_common.gd").new()
	await c.setup(p_tree)
	ws.focus_timeline()
	await p_tree.process_frame
	print("screenshot focus: ", p_tree.root.gui_get_focus_owner())
	ws.seek(20)
	for k in [KEY_RIGHT, KEY_M, KEY_SPACE]:
		for pressed in [true, false]:
			var e := InputEventKey.new()
			e.keycode = k
			e.physical_keycode = k
			e.pressed = pressed
			p_tree.root.push_input(e)
			await p_tree.process_frame
		print("screenshot after key ", k, " playhead ", ws.get_playhead(), " markers ", ws.get_project().get_markers().size(), " playing ", ws.is_playing(), " focus ", p_tree.root.gui_get_focus_owner())
