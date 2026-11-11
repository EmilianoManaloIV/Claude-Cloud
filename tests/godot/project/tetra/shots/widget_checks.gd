extends RefCounted
## Checks of the shared widgets that need the running editor: the classes are
## part of the editor API and cannot be created by a headless test script.
## Run through scripts/screenshot.sh; results are printed as "screenshot ..." lines.
##
##   scripts/screenshot.sh build/screens/tetra-widget-checks.png 3D res://tetra/shots/widget_checks.gd

var _tree: SceneTree
var root: Window
var _checks := 0
var _failures := 0


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	root = p_tree.root
	root.gui_embed_subwindows = true
	await _test_canvas_view()
	await _test_param_panel()
	await _test_browser()
	await _test_dialog()
	print("screenshot widget checks: %d checks, %d failed" % [_checks, _failures])


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func check_near(p_actual: float, p_expected: float, p_epsilon: float = 1e-4, p_what: String = "") -> bool:
	return check(absf(p_actual - p_expected) <= p_epsilon, "%s: expected %f, got %f" % [p_what, p_expected, p_actual])


func _check_vector(p_actual: Vector2, p_expected: Vector2, p_what: String) -> void:
	check(p_actual.distance_to(p_expected) < 0.01, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func _wheel(p_viewport: Viewport, p_button: MouseButton, p_position: Vector2, p_ctrl := false, p_shift := false) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = p_button
	event.pressed = true
	event.position = p_position
	event.global_position = p_position
	event.factor = 1.0
	event.ctrl_pressed = p_ctrl
	event.shift_pressed = p_shift
	p_viewport.push_input(event)


func _test_canvas_view() -> void:
	var canvas := TetraCanvasView.new()
	canvas.position = Vector2.ZERO
	root.add_child(canvas)
	canvas.set_content_size(Vector2(800, 400))
	check_eq(canvas.get_content_size(), Vector2(800, 400), "content size")
	var changes := [0]
	canvas.view_changed.connect(func() -> void: changes[0] += 1)

	# A view that gets its size for the first time shows the whole content, centred.
	canvas.size = Vector2(432, 232)
	await _tree.process_frame
	check_near(canvas.get_zoom(), 0.5, 1e-5, "zoom to fit leaves a 16 px margin: (432 - 32) / 800")
	_check_vector(canvas.content_to_view(Vector2(400, 200)), Vector2(216, 116), "the content centre is at the view centre")
	_check_vector(canvas.content_to_view(Vector2(0, 0)), Vector2(16, 16), "the content corner sits at the margin")
	check_eq(canvas.get_visible_content_rect(), Rect2(0, 0, 800, 400), "the whole content is visible")
	check(changes[0] > 0, "view_changed was emitted")
	var h_scroll: HScrollBar = canvas.get_child(0)
	var v_scroll: VScrollBar = canvas.get_child(1)
	check(not h_scroll.visible and not v_scroll.visible, "no scroll bars while the content fits")

	# The two conversions are inverses of each other.
	_check_vector(canvas.view_to_content(canvas.content_to_view(Vector2(123, 45))), Vector2(123, 45), "round trip")
	_check_vector(canvas.view_to_content(Vector2(16, 16)), Vector2(0, 0), "view_to_content")

	# Zooming keeps the content under the pivot in place.
	var pivot := Vector2(100, 60)
	var under_pivot := canvas.view_to_content(pivot)
	canvas.set_zoom(2.0, pivot)
	check_near(canvas.get_zoom(), 2.0, 1e-5, "set_zoom")
	_check_vector(canvas.view_to_content(pivot), under_pivot, "the pivot stays over the same content")
	check(h_scroll.visible and v_scroll.visible, "scroll bars appear when the zoomed content exceeds the view")
	check_near(h_scroll.page, 216.0, 1e-3, "the scroll bar page is the view width in content units")
	check_near(h_scroll.value, canvas.get_view_offset().x, 1e-3, "the scroll bar shows the offset")
	check_eq(canvas.get_visible_content_rect(), Rect2(canvas.get_view_offset(), Vector2(216, 116)), "visible rect when zoomed in")

	canvas.set_zoom_centered(4.0)
	check_near(canvas.get_zoom(), 4.0, 1e-5, "set_zoom_centered")
	canvas.set_zoom(100000.0, pivot)
	check_near(canvas.get_zoom(), 256.0, 1e-5, "the zoom stops at its maximum")
	canvas.set_zoom(0.00001, pivot)
	check_near(canvas.get_zoom(), 1.0 / 64.0, 1e-6, "the zoom stops at its minimum")
	canvas.set_zoom_limits(0.25, 8.0)
	check_near(canvas.get_zoom(), 0.25, 1e-6, "new limits apply to the current zoom")
	check_eq([canvas.get_zoom_min(), canvas.get_zoom_max()], [0.25, 8.0], "zoom limits")

	# The view centre cannot leave the content.
	canvas.set_zoom(1.0, Vector2.ZERO)
	canvas.set_view_offset(Vector2(-5000, -5000))
	_check_vector(canvas.get_view_offset(), Vector2(-216, -116), "lowest offset: the content corner at the view centre")
	canvas.set_view_offset(Vector2(5000, 5000))
	_check_vector(canvas.get_view_offset(), Vector2(800 - 216, 400 - 116), "highest offset")
	canvas.set_view_offset(Vector2(10.4, 20.6))
	_check_vector(canvas.get_view_offset(), Vector2(10, 21), "the offset is snapped to whole view pixels")
	canvas.set_zoom(4.0, Vector2.ZERO)
	canvas.set_view_offset(Vector2(10.3, 20.1))
	_check_vector(canvas.get_view_offset(), Vector2(10.25, 20.0), "at zoom 4 a view pixel is a quarter of a unit")
	canvas.center_content()
	_check_vector(canvas.view_to_content(Vector2(216, 116)), Vector2(400, 200), "center_content")
	canvas.zoom_to_fit()
	check_near(canvas.get_zoom(), 0.5, 1e-5, "zoom_to_fit again")

	# Dragging a scroll bar moves the view.
	canvas.set_zoom(2.0, Vector2.ZERO)
	h_scroll.value = 300
	check_near(canvas.get_view_offset().x, 300.0, 1e-3, "the horizontal scroll bar sets the offset")
	v_scroll.value = 150
	check_near(canvas.get_view_offset().y, 150.0, 1e-3, "the vertical scroll bar sets the offset")

	# The wheel zooms about the cursor, with or without Ctrl.
	var cursor := Vector2(200, 100)
	under_pivot = canvas.view_to_content(cursor)
	_wheel(root, MOUSE_BUTTON_WHEEL_UP, cursor)
	check_near(canvas.get_zoom(), 2.4, 1e-4, "one wheel notch zooms in by 1.2")
	check(canvas.view_to_content(cursor).distance_to(under_pivot) < 0.5, "the wheel zooms about the cursor")
	_wheel(root, MOUSE_BUTTON_WHEEL_DOWN, cursor, true)
	check_near(canvas.get_zoom(), 2.0, 1e-4, "Ctrl+wheel zooms too")
	var before := canvas.get_view_offset()
	_wheel(root, MOUSE_BUTTON_WHEEL_DOWN, cursor, false, true)
	_check_vector(canvas.get_view_offset(), before + Vector2(24, 0), "Shift+wheel scrolls sideways by 48 px")

	# A subclass can ask for a scrolling wheel; zooming is then Ctrl+wheel only.
	canvas.set_wheel_scrolls(true)
	check(canvas.is_wheel_scrolling(), "set_wheel_scrolls")
	before = canvas.get_view_offset()
	_wheel(root, MOUSE_BUTTON_WHEEL_DOWN, cursor)
	_check_vector(canvas.get_view_offset(), before + Vector2(0, 24), "the plain wheel scrolls down")
	check_near(canvas.get_zoom(), 2.0, 1e-4, "the plain wheel no longer zooms")
	_wheel(root, MOUSE_BUTTON_WHEEL_UP, cursor, true)
	check_near(canvas.get_zoom(), 2.4, 1e-4, "Ctrl+wheel still zooms")

	# Middle-drag pans: the content follows the mouse.
	canvas.set_zoom(2.0, Vector2.ZERO)
	canvas.set_view_offset(Vector2(100, 100))
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_MIDDLE
	press.pressed = true
	press.position = cursor
	press.global_position = cursor
	root.push_input(press)
	check(canvas.is_panning(), "a middle press starts a pan")
	var motion := InputEventMouseMotion.new()
	motion.position = cursor + Vector2(40, -20)
	motion.global_position = motion.position
	motion.relative = Vector2(40, -20)
	motion.button_mask = MOUSE_BUTTON_MASK_MIDDLE
	root.push_input(motion)
	_check_vector(canvas.get_view_offset(), Vector2(80, 110), "dragging 40 px right at zoom 2 moves the offset 20 units left")
	var release := InputEventMouseButton.new()
	release.button_index = MOUSE_BUTTON_MIDDLE
	release.pressed = false
	release.position = motion.position
	release.global_position = motion.position
	root.push_input(release)
	check(not canvas.is_panning(), "releasing ends the pan")

	canvas.queue_free()


func _operation() -> Dictionary:
	var op := TetraSchema.make_operation("native:test", "Test Operation", "Test", "native", "For the widget tests.")
	TetraSchema.add_param(op, TetraSchema.set_ui_range(TetraSchema.param_float("radius", "Radius", 1.5, 0.0, 1500.0, 0.1, "px"), 0.0, 100.0))
	TetraSchema.add_param(op, TetraSchema.param_int("count", "Count", 3, 1, 10))
	TetraSchema.add_param(op, TetraSchema.param_bool("clip", "Clip", true))
	TetraSchema.add_param(op, TetraSchema.param_enum("mode", "Mode", "a", TetraSchema.make_options(["a", "b", "c"])))
	TetraSchema.add_param(op, TetraSchema.param_string("name", "Name", "x"))
	TetraSchema.add_param(op, TetraSchema.param_vec2("offset", "Offset", Vector2(1, 2), -10.0, 10.0))
	TetraSchema.add_param(op, TetraSchema.set_advanced(TetraSchema.param_seed("seed", "Seed", 7)))
	return op


func _test_param_panel() -> void:
	var op := _operation()
	var panel := TetraParamPanel.new()
	root.add_child(panel)
	var changes: Array = []
	panel.value_changed.connect(func(name: String, value: Variant) -> void: changes.append([name, value]))

	panel.set_operation(op)
	check_eq(panel.get_operation(), op, "get_operation")
	check_eq(panel.get_values(), TetraSchema.get_defaults(op), "a new panel holds the defaults")
	check(panel.has_param("radius") and not panel.has_param("nope"), "has_param")

	panel.set_values({"radius": 5000, "count": "4", "mode": "zzz", "unknown": 1})
	check_eq(panel.get_value("radius"), 1500.0, "set_values clamps to the hard maximum")
	check_eq(typeof(panel.get_value("radius")), TYPE_FLOAT, "set_values converts to the parameter type")
	check_eq(panel.get_value("count"), 4, "set_values converts a string")
	check_eq(panel.get_value("mode"), "a", "a bad enum value becomes the default")
	check_eq(panel.get_value("clip"), true, "values that are not mentioned are kept")
	check_eq(panel.get_values().size(), 7, "unknown names are ignored")
	panel.set_value("offset", Vector2(50, -50))
	check_eq(panel.get_value("offset"), Vector2(10, -10), "set_value clamps a vector")
	check_eq(changes, [], "programmatic changes do not emit value_changed")

	# The controls, in display order: radius, count, offset x, offset y, seed.
	var spins := panel.find_children("*", "EditorSpinSlider", true, false)
	check_eq(spins.size(), 5, "one spin slider per number and vector component")
	var radius: Range = spins[0]
	check_eq([radius.min_value, radius.max_value], [0.0, 100.0], "the slider covers the ui range")
	check(radius.allow_greater and not radius.allow_lesser, "typing may exceed the slider towards the hard maximum only")
	check_near(radius.value, 1500.0, 1e-6, "the slider shows the value")
	check_eq(radius.get("suffix"), " px", "the unit is the suffix")

	# A change of the control is a user edit.
	radius.value = 42.5
	check_eq(changes, [["radius", 42.5]], "editing the control emits value_changed")
	check_eq(panel.get_value("radius"), 42.5, "and stores the value")
	radius.value = 99999.0
	check_eq(changes[-1], ["radius", 1500.0], "a typed value beyond the hard limit is clamped")
	check_near(radius.value, 1500.0, 1e-6, "and the control shows the clamped value")
	radius.value = 99999.0
	check_eq(changes.size(), 2, "an edit that changes nothing emits nothing")
	var count: Range = spins[1]
	count.value = 8
	check_eq(changes[-1], ["count", 8], "an int parameter reports an int")
	check_eq(typeof(changes[-1][1]), TYPE_INT, "of type int")
	var offset_y: Range = spins[3]
	offset_y.value = 3.0
	check_eq(changes[-1], ["offset", Vector2(10, 3)], "a vector component reports the whole vector")

	var check_box: CheckBox = panel.find_children("*", "CheckBox", true, false)[0]
	check_box.button_pressed = false
	check_eq(changes[-1], ["clip", false], "the check box")
	var options: OptionButton = panel.find_children("*", "OptionButton", true, false)[0]
	check_eq(options.item_count, 3, "one item per option")
	check_eq(options.selected, 0, "the option button shows the value")
	options.select(2)
	options.item_selected.emit(2)
	check_eq(changes[-1], ["mode", "c"], "the option button")
	var line: LineEdit = panel.find_children("*", "LineEdit", true, false).filter(
			func(node: LineEdit) -> bool: return node.text == "x")[0]
	line.text = "hello"
	line.text_changed.emit("hello")
	check_eq(changes[-1], ["name", "hello"], "the line edit")
	var randomize_button: Button = panel.find_children("*", "Button", true, false).filter(
			func(node: Node) -> bool: return node.get_class() == "Button")[0]
	var seeds := {}
	for i in 8:
		randomize_button.pressed.emit()
		seeds[panel.get_value("seed")] = true
		check(panel.get_value("seed") >= 0 and panel.get_value("seed") <= 4294967295, "a random seed is inside the limits")
	check(seeds.size() > 1, "the randomize button changes the seed")
	check_eq(changes[-1][0], "seed", "and reports it")

	var emitted := changes.size()
	panel.reset_to_defaults()
	check_eq(panel.get_values(), TetraSchema.get_defaults(op), "reset_to_defaults")
	check_near(radius.value, 1.5, 1e-6, "reset updates the controls")
	check_eq(changes.size(), emitted, "reset does not emit value_changed")

	panel.set_read_only(true)
	check(panel.is_read_only(), "is_read_only")
	check(radius.get("read_only") and check_box.disabled and options.disabled and not line.editable and randomize_button.disabled,
			"read-only disables every control")
	panel.set_read_only(false)
	check(not radius.get("read_only") and not check_box.disabled and line.editable, "and read-write enables them again")

	check(panel.is_advanced_folded(), "the advanced section starts folded")
	panel.set_advanced_folded(false)
	check(not panel.is_advanced_folded(), "set_advanced_folded")
	check(panel.is_scrollable(), "a panel scrolls by default")
	await _tree.process_frame
	var form_height := panel.get_form_minimum_size().y
	check(form_height > 100, "the form has a height")
	check(panel.get_combined_minimum_size().y < form_height, "a scrolling panel is smaller than its form")
	panel.set_scrollable(false)
	check(panel.get_combined_minimum_size().y >= form_height, "a panel that does not scroll asks for the whole form")

	# Another operation replaces the form.
	var other := TetraSchema.make_operation("native:other", "Other", "Test", "native")
	TetraSchema.add_param(other, TetraSchema.param_color("tint", "Tint", Color.RED))
	panel.set_operation(other)
	check_eq(panel.get_values(), {"tint": Color.RED}, "set_operation replaces the values")
	panel.set_operation({})
	check_eq(panel.get_values(), {}, "an operation without parameters")
	panel.queue_free()


func _test_browser() -> void:
	var ops: Array = []
	for name: String in ["Gaussian Blur", "Motion Blur", "Sharpen", "Curves", "Levels", "Subdivide"]:
		var category := "Blur" if name.ends_with("Blur") else ("Mesh/Edges" if name == "Subdivide" else "Colors")
		ops.append(TetraSchema.make_operation("t:" + name.to_lower().replace(" ", "-"), name, category, "native", name + " description."))
	var browser := TetraOperationBrowser.new()
	root.add_child(browser)
	var selected: Array = []
	var activated: Array = []
	browser.operation_selected.connect(func(id: String) -> void: selected.append(id))
	browser.operation_activated.connect(func(id: String) -> void: activated.append(id))

	browser.set_operations(ops)
	check_eq(browser.get_operations(), ops, "get_operations")
	check_eq(browser.get_selected_id(), "", "nothing is selected at first")
	check_eq(browser.get_selected_operation(), {}, "no selected operation")
	var tree: Tree = browser.find_children("*", "Tree", true, false)[0]
	var folders: Array = tree.get_root().get_children().map(func(item: TreeItem) -> String: return item.get_text(0))
	check_eq(folders, ["Blur", "Colors", "Mesh"], "top-level folders in alphabetical order")
	check_eq(tree.get_root().get_child(0).get_children().map(func(item: TreeItem) -> String: return item.get_text(0)),
			["Gaussian Blur", "Motion Blur"], "operations inside their folder, by label")
	check_eq(tree.get_root().get_child(2).get_child(0).get_text(0), "Edges", "nested categories")

	browser.select_operation("t:subdivide")
	check_eq(browser.get_selected_id(), "t:subdivide", "select_operation")
	check_eq(browser.get_selected_operation()["label"], "Subdivide", "get_selected_operation")
	check_eq(tree.get_selected().get_text(0), "Subdivide", "the tree shows the selection")
	check_eq(selected, [], "select_operation does not emit operation_selected")
	browser.select_operation("t:nope")
	check_eq(browser.get_selected_id(), "", "an unknown id clears the selection")

	browser.set_filter_text("blur")
	check_eq(browser.get_filter_text(), "blur", "get_filter_text")
	check_eq(tree.get_root().get_children().map(func(item: TreeItem) -> String: return item.get_text(0)),
			["Motion Blur", "Gaussian Blur"], "a search lists the matches flat, best first")
	check_eq(tree.get_root().get_child(0).get_text(1), "Blur", "with the category beside them")
	check_eq(browser.get_selected_id(), "t:motion-blur", "a search selects its best match")
	browser.select_operation("t:curves")
	check_eq(browser.get_filter_text(), "", "selecting an operation the search hides ends the search")
	check_eq(tree.get_selected().get_text(0), "Curves", "and reveals it")

	# The user typing in the search field.
	var search: LineEdit = browser.find_children("*", "LineEdit", true, false)[0]
	search.text = "sharp"
	search.text_changed.emit("sharp")
	check_eq(selected, ["t:sharpen"], "typing selects the best match and emits operation_selected")
	search.text_submitted.emit("sharp")
	check_eq(activated, ["t:sharpen"], "Enter in the search field activates the selection")
	search.text = "zzz"
	search.text_changed.emit("zzz")
	check_eq(browser.get_selected_id(), "", "a search without matches selects nothing")
	check_eq(tree.get_root().get_child_count(), 0, "and lists nothing")

	# The user clicking and double-clicking in the tree.
	search.text = ""
	search.text_changed.emit("")
	var levels: TreeItem = tree.get_root().get_child(1).get_child(1)
	levels.select(0)
	check_eq(selected[-1], "t:levels", "selecting a row emits operation_selected")
	tree.item_activated.emit()
	check_eq(activated[-1], "t:levels", "activating a row emits operation_activated")

	# The selection survives a new list that still has the operation.
	browser.set_operations(ops.slice(2))
	check_eq(browser.get_selected_id(), "t:levels", "the selection is kept across set_operations")
	browser.set_operations(ops.slice(0, 2))
	check_eq(browser.get_selected_id(), "", "and dropped when the operation is gone")
	browser.queue_free()


func _test_dialog() -> void:
	var op := _operation()
	var dialog := TetraOperationDialog.new()
	root.add_child(dialog)
	var changed: Array = []
	var confirmed: Array = []
	var cancelled: Array = []
	dialog.values_changed.connect(func(values: Dictionary) -> void: changed.append(values))
	dialog.operation_confirmed.connect(func(id: String, values: Dictionary) -> void: confirmed.append([id, values]))
	dialog.operation_cancelled.connect(func(id: String) -> void: cancelled.append(id))

	check(not dialog.is_preview_enabled() and not dialog.is_preview_checked(), "no preview check box by default")
	dialog.set_preview_enabled(true)
	check(dialog.is_preview_enabled() and dialog.is_preview_checked(), "the preview check box starts checked")
	dialog.set_preview_checked(false)
	check(not dialog.is_preview_checked(), "set_preview_checked")

	dialog.popup_operation(op, {"count": 9})
	check(dialog.visible, "popup_operation shows the dialog")
	check_eq(dialog.title, "Test Operation", "the title is the operation's label")
	check_eq(dialog.get_operation(), op, "get_operation")
	var expected := TetraSchema.get_defaults(op)
	expected["count"] = 9
	check_eq(dialog.get_values(), expected, "initial values override the defaults")
	check_eq(dialog.get_param_panel().get_values(), expected, "get_param_panel")

	# Edits are reported once, 150 ms after the last one.
	var spins := dialog.get_param_panel().find_children("*", "EditorSpinSlider", true, false)
	spins[0].value = 10.0
	spins[0].value = 20.0
	check_eq(changed, [], "values_changed waits")
	await _tree.create_timer(0.4).timeout
	expected["radius"] = 20.0
	check_eq(changed, [expected], "values_changed arrives once with every value")

	# Reset restores the defaults and reports them.
	for button: Button in dialog.find_children("*", "Button", true, false):
		if button.text == "Reset":
			button.pressed.emit()
	await _tree.create_timer(0.4).timeout
	check_eq(dialog.get_values(), TetraSchema.get_defaults(op), "Reset restores the defaults")
	check_eq(changed.size(), 2, "Reset emits values_changed")
	check_eq(changed[-1], TetraSchema.get_defaults(op), "with the defaults")

	# OK confirms with the values and drops a pending change report.
	spins[0].value = 30.0
	dialog.get_ok_button().pressed.emit()
	expected = TetraSchema.get_defaults(op)
	expected["radius"] = 30.0
	check_eq(confirmed, [["native:test", expected]], "OK emits operation_confirmed")
	check(not dialog.visible, "OK closes the dialog")
	await _tree.create_timer(0.4).timeout
	check_eq(changed.size(), 2, "no values_changed after OK")

	dialog.popup_operation(op)
	check_eq(dialog.get_values(), TetraSchema.get_defaults(op), "a second popup starts from the defaults")
	dialog.get_cancel_button().pressed.emit()
	check_eq(cancelled, ["native:test"], "Cancel emits operation_cancelled")
	check_eq(confirmed.size(), 1, "and does not confirm")
	await _tree.process_frame
	dialog.queue_free()
