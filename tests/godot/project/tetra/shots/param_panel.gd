extends RefCounted
## Screenshot driver: TetraParamPanel when narrow, with 40 parameters, and read-only.

const ShotData := preload("res://tetra/shots/shot_data.gd")


func _column(p_parent: Control, p_title: String, p_width: int) -> TetraParamPanel:
	var column := VBoxContainer.new()
	column.custom_minimum_size.x = p_width
	p_parent.add_child(column)
	var title := Label.new()
	title.text = p_title
	title.theme_type_variation = "HeaderSmall"
	title.modulate = Color(1, 1, 1, 0.6)
	column.add_child(title)
	var panel := TetraParamPanel.new()
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(panel)
	return panel


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var window := Window.new()
	window.title = "Parameter panels"
	window.size = Vector2i(1010, 620)
	var background := PanelContainer.new()
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	window.add_child(background)
	var margin := MarginContainer.new()
	for side: String in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 8)
	background.add_child(margin)
	var columns := HBoxContainer.new()
	columns.add_theme_constant_override("separation", 16)
	margin.add_child(columns)
	EditorInterface.get_base_control().add_child(window)
	window.popup_centered()

	var narrow := _column(columns, "200 px wide", 200)
	narrow.set_operation(ShotData.kitchen_sink())

	var many := TetraSchema.make_operation("native:forty", "Forty", "Test", "native")
	for i in 40:
		var param: Dictionary
		match i % 4:
			0: param = TetraSchema.param_float("p%d" % i, "Parameter %d" % (i + 1), i * 2.5, 0.0, 100.0)
			1: param = TetraSchema.param_int("p%d" % i, "Parameter %d" % (i + 1), i, 0, 64)
			2: param = TetraSchema.param_bool("p%d" % i, "Parameter %d" % (i + 1), i % 8 == 2)
			3: param = TetraSchema.param_enum("p%d" % i, "Parameter %d" % (i + 1), "b", TetraSchema.make_options(["a", "b", "c"]))
		TetraSchema.add_param(many, param)
	var forty := _column(columns, "40 parameters", 340)
	forty.set_operation(many)

	var read_only := _column(columns, "Read-only, advanced open", 400)
	read_only.set_operation(ShotData.kitchen_sink())
	read_only.set_advanced_folded(false)
	read_only.set_read_only(true)
	await p_tree.create_timer(0.5).timeout
	print("screenshot panel form heights: narrow %d, forty %d (panel is %d tall)" % [
			narrow.get_form_minimum_size().y, forty.get_form_minimum_size().y, forty.size.y])
