extends RefCounted
## Picture (c): a selection with its marching ants and the Filters menu open.
##
##   scripts/screenshot.sh build/screens/image-selection-menu.png Image res://tetra_paint/shots/workspace_selection_menu.gd


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ws: Object = TetraImageWorkspace.get_singleton()
	var doc: TetraImageDocument = load("res://tetra_paint/shots/demo_scene.gd").build(ws)
	while not ws.are_operations_loaded():
		await p_tree.process_frame
	var canvas: Control = ws.get_image_canvas()
	ws.set_tool("ellipse-select")
	canvas.tool_press(Vector2(520, 90))
	canvas.tool_drag(Vector2(880, 420))
	canvas.tool_release()
	ws.set_tool("rect-select")
	canvas.tool_press(Vector2(120, 480), 1.0, true)
	canvas.tool_drag(Vector2(420, 700))
	canvas.tool_release()
	await p_tree.process_frame

	var bar: MenuBar = ws.get_menu_bar()
	var filters: PopupMenu = ws.get_menu("Filters")
	# Under its title, the last one of the menu bar.
	filters.popup(Rect2i(int(bar.global_position.x + bar.size.x - 60), int(bar.global_position.y + bar.size.y), 0, 0))
	await p_tree.create_timer(0.3).timeout
	for i in filters.item_count:
		if filters.get_item_text(i) == "Blur":
			filters.set_focused_item(i)
			var submenu := filters.get_item_submenu_node(i)
			submenu.popup(Rect2i(filters.position.x + filters.size.x, filters.position.y + 80, 0, 0))
	await p_tree.create_timer(0.3).timeout
	print("screenshot selection: bounds %s, %d outline points, %d filter menu items" % [doc.selection_bounds(), canvas.get_outline_point_count(), filters.item_count])
