extends RefCounted
func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ws: Object = TetraImageWorkspace.get_singleton()
	while not ws.are_operations_loaded():
		await p_tree.process_frame
	var doc: TetraImageDocument = ws.new_document(256, 256, Color.WHITE)
	var colors: PopupMenu = ws.get_menu("Colors")
	for i in colors.item_count:
		if colors.get_item_text(i).begins_with("Invert"):
			print("screenshot dbg item ", i, " ", colors.get_item_text(i), " ", colors.get_item_metadata(i))
	doc.select_rect(Rect2i(0, 0, 256, 128))
	for i in colors.item_count:
		if colors.get_item_text(i) == "Invert":
			colors.index_pressed.emit(i)
			break
	for i in 60:
		await p_tree.process_frame
	print("screenshot dbg ", doc.get_undo_names(), doc.composite().get_pixel(200, 50), doc.composite().get_pixel(200, 200), " status ", ws.get_status_text())
	doc.select_none()
	ws.get_image_canvas().grab_focus()
	var e := InputEventKey.new()
	e.keycode = KEY_SLASH
	e.physical_keycode = KEY_SLASH
	e.pressed = true
	p_tree.root.push_input(e)
	await p_tree.process_frame
	var f: PopupMenu = ws.get_menu("Filters")
	print("screenshot dbg focus ", p_tree.root.gui_get_focus_owner(), " sc ", f.get_item_shortcut(1).get_as_text(), " disabled ", f.is_item_disabled(1))
	for d: AcceptDialog in ws.find_children("*", "AcceptDialog", true, false):
		if d.visible:
			print("screenshot dbg visible dialog ", d.title)
