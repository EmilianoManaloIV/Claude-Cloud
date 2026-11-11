extends RefCounted
## Screenshot driver: two TetraOperationBrowsers in a Window over 300 made-up
## operations, the left one browsing by category and the right one searching.

const ShotData := preload("res://tetra/shots/shot_data.gd")


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ops := ShotData.catalog(300)

	var window := Window.new()
	window.title = "Operations"
	window.size = Vector2i(900, 640)
	var background := PanelContainer.new()
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	window.add_child(background)
	var margin := MarginContainer.new()
	for side: String in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 8)
	background.add_child(margin)
	var split := HBoxContainer.new()
	split.add_theme_constant_override("separation", 12)
	margin.add_child(split)

	var by_category := TetraOperationBrowser.new()
	by_category.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(by_category)
	var searching := TetraOperationBrowser.new()
	searching.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(searching)

	EditorInterface.get_base_control().add_child(window)
	window.popup_centered()

	by_category.set_operations(ops)
	by_category.select_operation(ops[27]["id"])
	searching.set_operations(ops)
	searching.set_filter_text("lens bl")

	# 1500 operations must not make the browser lag.
	var many := ShotData.catalog(1500)
	var probe := TetraOperationBrowser.new()
	window.add_child(probe)
	var start := Time.get_ticks_usec()
	probe.set_operations(many)
	var built := Time.get_ticks_usec()
	probe.set_filter_text("a")
	var searched := Time.get_ticks_usec()
	print("screenshot timing: 1500 operations listed in %.1f ms, searched in %.1f ms" % [(built - start) / 1000.0, (searched - built) / 1000.0])
	probe.free()
	await p_tree.create_timer(0.5).timeout
