extends RefCounted
## Picture (b): a GEGL filter dialog with its live preview on the canvas.
##
##   scripts/screenshot.sh build/screens/image-filter-dialog.png Image res://tetra_paint/shots/workspace_filter_dialog.gd


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ws: Object = TetraImageWorkspace.get_singleton()
	var doc: TetraImageDocument = load("res://tetra_paint/shots/demo_scene.gd").build(ws)
	while not ws.are_operations_loaded():
		await p_tree.process_frame
	doc.set_active_layer(doc.get_layer_ids()[0])
	var id := "gegl:mosaic" if TetraGegl.is_available() else "native:pixelize"
	ws.show_operation_dialog(id)
	var dialog: Window = ws.get_operation_dialog()
	while not ws.is_preview_shown():
		await p_tree.process_frame
	# Beside the image, so the preview stays in view.
	var canvas: Control = ws.get_image_canvas()
	dialog.position = Vector2i(int(canvas.global_position.x + canvas.size.x - dialog.size.x * 0.35), int(canvas.global_position.y + 40))
	await p_tree.create_timer(0.5).timeout
	print("screenshot filter dialog: %s, preview shown %s, undo steps %d" % [dialog.title, ws.is_preview_shown(), doc.get_undo_names().size()])
