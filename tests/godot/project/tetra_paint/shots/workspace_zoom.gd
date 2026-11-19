extends RefCounted
## Picture (d): zoomed in on a small transparent image: nearest filtering,
## the checkerboard, the canvas border, a feathered selection and the brush
## outline under the pointer.
##
##   scripts/screenshot.sh build/screens/image-zoom.png Image res://tetra_paint/shots/workspace_zoom.gd


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ws: Object = TetraImageWorkspace.get_singleton()
	var doc: TetraImageDocument = ws.new_document(96, 64, Color(0, 0, 0, 0))
	var canvas: Control = ws.get_image_canvas()
	ws.set_tool("pencil")
	ws.set_tool_option("pencil", "size", 3.0)
	ws.set_foreground_color(Color("e0503c"))
	canvas.tool_press(Vector2(10, 50))
	for i in 20:
		canvas.tool_drag(Vector2(10 + i * 4, 50 - 30 * sin(i * 0.3)))
	canvas.tool_release()
	ws.set_tool("ellipse-select")
	canvas.tool_press(Vector2(50, 10))
	canvas.tool_drag(Vector2(90, 40))
	canvas.tool_release()
	ws.set_foreground_color(Color("3c7be0"))
	ws.menu_option(ws.EDIT_FILL_FOREGROUND)
	ws.set_tool("paintbrush")
	ws.set_tool_option("paintbrush", "size", 16.0)
	await p_tree.process_frame
	await p_tree.process_frame
	var motion := InputEventMouseMotion.new()
	motion.position = canvas.global_position + canvas.content_to_view(Vector2(30, 24))
	motion.global_position = motion.position
	p_tree.root.push_input(motion)
	await p_tree.create_timer(0.3).timeout
	print("screenshot zoom: %s" % ws.get_status_text())
