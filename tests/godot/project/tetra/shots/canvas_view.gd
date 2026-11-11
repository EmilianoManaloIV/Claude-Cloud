extends RefCounted
## Screenshot driver: a TetraCanvasView showing a 640x400 document zoomed in,
## so that both scroll bars and the checkerboard are visible.


class DemoCanvas extends TetraCanvasView:
	func _draw() -> void:
		draw_rect(Rect2(Vector2.ZERO, size), Color(0.13, 0.13, 0.13))
		draw_checkerboard(Rect2(Vector2.ZERO, get_content_size()))
		draw_set_transform_to_content()
		draw_rect(Rect2(40, 40, 240, 160), Color(0.9, 0.3, 0.25, 0.7))
		draw_circle(Vector2(400, 220), 120, Color(0.2, 0.5, 0.95, 0.8))
		draw_rect(Rect2(Vector2.ZERO, get_content_size()), Color.WHITE, false, 2.0)
		draw_reset_transform()


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var window := Window.new()
	window.title = "Canvas view"
	window.size = Vector2i(900, 560)
	var split := HBoxContainer.new()
	split.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	window.add_child(split)
	var fitted := DemoCanvas.new()
	fitted.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	fitted.set_content_size(Vector2(640, 400))
	split.add_child(fitted)
	var zoomed := DemoCanvas.new()
	zoomed.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	zoomed.set_content_size(Vector2(640, 400))
	split.add_child(zoomed)
	EditorInterface.get_base_control().add_child(window)
	window.popup_centered()
	await p_tree.create_timer(0.3).timeout
	zoomed.set_zoom(2.0, zoomed.content_to_view(Vector2(400, 220)))
	await p_tree.create_timer(0.3).timeout
	print("screenshot canvas: fitted zoom %.3f offset %s; zoomed zoom %.3f offset %s visible %s" % [
			fitted.get_zoom(), fitted.get_view_offset(), zoomed.get_zoom(), zoomed.get_view_offset(), zoomed.get_visible_content_rect()])
