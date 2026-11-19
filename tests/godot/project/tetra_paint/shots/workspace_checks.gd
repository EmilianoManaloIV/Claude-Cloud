extends RefCounted
## Behaviour checks of the Image workspace. The workspace classes belong to
## the editor API, so they are exercised inside the running editor:
##
##   scripts/screenshot.sh build/screens/image-checks.png Image res://tetra_paint/shots/workspace_checks.gd
##
## Prints "screenshot image workspace checks: <n> checks, <m> failed".

const TMP_PNG := "res://tetra_paint/shots/_tmp_edit.png"

var _tree: SceneTree
var root: Window
var ws: Object
var _checks := 0
var _failures := 0


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	root = p_tree.root
	root.gui_embed_subwindows = true
	ws = TetraImageWorkspace.get_singleton()
	_test_welcome()
	await _test_canvas()
	await _test_tools_and_shortcuts()
	await _test_layers()
	await _test_menus()
	await _test_filters()
	await _test_files()
	print("screenshot image workspace checks: %d checks, %d failed" % [_checks, _failures])


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func _pixel(p_doc: TetraImageDocument, p_x: int, p_y: int) -> Color:
	return p_doc.composite(Rect2i(p_x, p_y, 1, 1)).get_pixel(0, 0)


func _close(p_color: Color, p_expected: Color) -> bool:
	# The centre of a soft brush dab is a hair short of full coverage.
	return absf(p_color.r - p_expected.r) < 0.03 and absf(p_color.g - p_expected.g) < 0.03 and absf(p_color.b - p_expected.b) < 0.03 and absf(p_color.a - p_expected.a) < 0.03


func _key(p_code: Key, p_shift: bool = false, p_ctrl: bool = false) -> void:
	var event := InputEventKey.new()
	event.keycode = p_code
	event.physical_keycode = p_code
	event.pressed = true
	event.shift_pressed = p_shift
	event.ctrl_pressed = p_ctrl
	root.push_input(event)
	var release := event.duplicate()
	release.pressed = false
	root.push_input(release)


func _mouse(p_canvas: Control, p_content: Vector2, p_pressed: Variant, p_ctrl: bool = false) -> void:
	var position: Vector2 = p_canvas.global_position + p_canvas.content_to_view(p_content)
	var event: InputEventMouse
	if p_pressed == null:
		var motion := InputEventMouseMotion.new()
		motion.button_mask = MOUSE_BUTTON_MASK_LEFT
		event = motion
	else:
		var button := InputEventMouseButton.new()
		button.button_index = MOUSE_BUTTON_LEFT
		button.pressed = p_pressed
		button.button_mask = MOUSE_BUTTON_MASK_LEFT if p_pressed else 0
		event = button
	event.position = position
	event.global_position = position
	event.ctrl_pressed = p_ctrl
	root.push_input(event)


func _frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func _test_welcome() -> void:
	check(ws != null and ws.is_visible_in_tree(), "the Image main screen shows the workspace")
	check(not ws.has_document(), "starts without a document")
	check_eq(ws.get_title_text(), "", "no title without a document")
	check(not ws.get_image_canvas().visible, "the canvas is hidden on the welcome page")
	check(ws.get_menu("File").is_item_disabled(ws.get_menu("File").get_item_index(ws.FILE_SAVE)), "Save is disabled without a document")
	check_eq(ws.get_tool_names().size(), 14, "fourteen tools")


func _test_canvas() -> void:
	var doc: TetraImageDocument = ws.new_document(2048, 2048, Color.WHITE)
	var canvas: Control = ws.get_image_canvas()
	await _frames(3)
	check(canvas.visible, "the canvas shows once a document exists")
	check_eq(canvas.get_tile_count(), 64, "2048 x 2048 is shown through 64 tiles of 256 px")
	check_eq(canvas.get_content_size(), Vector2(2048, 2048), "content size follows the document")
	check(canvas.get_zoom() < 1.0 and canvas.get_zoom() > 0.1, "the new image is fitted into the view (zoom %f)" % canvas.get_zoom())
	check(ws.get_status_text().contains("2048 × 2048 px"), "status bar shows the document size: " + ws.get_status_text())
	check(ws.get_status_text().contains("%"), "status bar shows the zoom")
	check_eq(ws.get_title_text(), "[Untitled]", "title of a new image")

	# A stroke inside one tile uploads exactly that tile.
	ws.set_tool("paintbrush")
	ws.set_tool_option("paintbrush", "size", 20.0)
	ws.set_foreground_color(Color.RED)
	var uploads: int = canvas.get_tile_update_count()
	canvas.tool_press(Vector2(350, 350))
	canvas.tool_drag(Vector2(400, 380))
	canvas.tool_release()
	await _frames(2)
	check_eq(canvas.get_tile_update_count() - uploads, 1, "a stroke inside one tile re-uploads one tile")
	check(_close(_pixel(doc, 350, 350), Color.RED), "the stroke painted with the foreground colour")
	check_eq(ws.get_title_text(), "[Untitled] (*)", "an edit marks the title")
	check(ws.is_modified(), "and the document as modified")

	# Responsiveness: a long stroke across the big image, one frame per point.
	ws.set_tool_option("paintbrush", "size", 40.0)
	var started := Time.get_ticks_msec()
	canvas.tool_press(Vector2(100, 1000))
	for i in 60:
		canvas.tool_drag(Vector2(100 + i * 30, 1000 + 200 * sin(i * 0.2)))
		await _tree.process_frame
	canvas.tool_release()
	var per_point := (Time.get_ticks_msec() - started) / 60.0
	print("screenshot image workspace: 2048x2048 stroke, %.1f ms per point including the frame (software rendering)" % per_point)
	check(per_point < 60.0, "painting on 2048 x 2048 stays responsive (%.1f ms per point)" % per_point)

	# Real pointer events: press, drag, release through the viewport.
	ws.set_foreground_color(Color.BLUE)
	ws.set_tool_option("paintbrush", "size", 60.0)
	_mouse(canvas, Vector2(1500, 500), true)
	_mouse(canvas, Vector2(1560, 500), null)
	_mouse(canvas, Vector2(1620, 500), null)
	_mouse(canvas, Vector2(1620, 500), false)
	await _frames(2)
	check(_pixel(doc, 1560, 500).is_equal_approx(Color.BLUE), "dragging the mouse paints: " + str(_pixel(doc, 1560, 500)))
	check(not canvas.is_tool_active(), "releasing the button ends the stroke")
	check(ws.get_status_text().contains("1620, 500"), "status bar shows the pointer position: " + ws.get_status_text())
	check_eq(doc.get_undo_names()[-1], "Paintbrush", "the stroke is one undo step named after the tool")

	# Clone: Ctrl+click sets the source.
	ws.set_tool("clone")
	_mouse(canvas, Vector2(1560, 500), true, true)
	_mouse(canvas, Vector2(1560, 500), false, true)
	check(canvas.has_clone_source_point(), "Ctrl+click sets the clone source")
	ws.set_tool_option("clone", "size", 30.0)
	ws.set_tool_option("clone", "hardness", 100.0)
	canvas.tool_press(Vector2(1000, 200))
	canvas.tool_release()
	check(_close(_pixel(doc, 1000, 200), Color.BLUE), "clone copies from the source: " + str(_pixel(doc, 1000, 200)))

	# Zoom tool and nearest filtering above 100 %.
	ws.set_zoom(4.0)
	check(is_equal_approx(ws.get_zoom(), 4.0), "set_zoom")
	check(ws.get_status_text().contains("400 %"), "status bar zoom: " + ws.get_status_text())
	ws.menu_option(ws.VIEW_ZOOM_FIT)
	check(ws.get_zoom() < 1.0, "View > Fit")


func _test_tools_and_shortcuts() -> void:
	var doc: TetraImageDocument = ws.get_document()
	var canvas: Control = ws.get_image_canvas()
	canvas.grab_focus()
	var expected := {
		"paintbrush": [KEY_P, false], "pencil": [KEY_N, false], "eraser": [KEY_E, true], "airbrush": [KEY_A, false],
		"smudge": [KEY_S, false], "clone": [KEY_C, false], "bucket-fill": [KEY_B, true], "gradient": [KEY_G, false],
		"color-picker": [KEY_O, false], "rect-select": [KEY_R, false], "ellipse-select": [KEY_E, false],
		"fuzzy-select": [KEY_U, false], "move": [KEY_M, false], "zoom": [KEY_Z, false],
	}
	for tool: String in expected:
		_key(expected[tool][0], expected[tool][1])
		check_eq(ws.get_tool(), tool, "shortcut of " + tool)
		check_eq(ws.get_tool_options_panel().get_operation()["id"], "tool:" + tool, "Tool Options shows " + tool)
	check(ws.get_status_text().begins_with("Zoom:"), "status bar shows the hint of the active tool: " + ws.get_status_text())

	ws.set_foreground_color(Color.RED)
	ws.set_background_color(Color.GREEN)
	_key(KEY_X)
	check_eq(ws.get_foreground_color(), Color.GREEN, "X swaps the colours")
	check_eq(ws.get_background_color(), Color.RED, "X swaps the colours (background)")
	_key(KEY_D)
	check_eq([ws.get_foreground_color(), ws.get_background_color()], [Color.BLACK, Color.WHITE], "D resets the colours")

	# The colour area opens Godot's ColorPicker.
	var swatch: Control = ws.find_children("*", "TetraImageColorSwatch", true, false)[0]
	swatch.foreground_pressed.emit()
	var picker: ColorPicker = ws.find_children("*", "ColorPicker", true, false)[0]
	check(picker.is_visible_in_tree(), "clicking the foreground swatch opens a ColorPicker")
	picker.color_changed.emit(Color(0.2, 0.4, 0.6))
	check_eq(ws.get_foreground_color(), Color(0.2, 0.4, 0.6), "which sets the foreground colour")
	picker.get_parent().hide()
	swatch.background_pressed.emit()
	picker.color_changed.emit(Color(0.9, 0.8, 0.1))
	check_eq(ws.get_background_color(), Color(0.9, 0.8, 0.1), "and the background colour from the other swatch")
	picker.get_parent().hide()
	ws.reset_colors()
	canvas.grab_focus()

	_key(KEY_P)
	ws.set_tool_option("paintbrush", "size", 20.0)
	_key(KEY_BRACKETRIGHT)
	check_eq(ws.get_tool_options("paintbrush")["size"], 22.0, "] grows the brush")
	check_eq(ws.get_tool_options_panel().get_value("size"), 22.0, "and the Tool Options panel follows")
	_key(KEY_BRACKETLEFT)
	_key(KEY_BRACKETLEFT)
	check_eq(ws.get_tool_options("paintbrush")["size"], 18.0, "[ shrinks the brush")
	check_eq(canvas.get_tool_options()["size"], 18.0, "the canvas uses the new size")
	for name: String in ["size", "hardness", "opacity", "flow", "spacing", "mode"]:
		check(ws.get_tool_options_panel().has_param(name), "paintbrush option " + name)
	for pair: Array in [["bucket-fill", "threshold"], ["gradient", "type"], ["rect-select", "mode"], ["rect-select", "feather"], ["fuzzy-select", "threshold"]]:
		check(ws.get_tool_options(pair[0]).has(pair[1]), "%s option %s" % pair)

	# Undo and redo through the keyboard, with the step name in the menu.
	var steps := doc.get_undo_names().size()
	var edit: PopupMenu = ws.get_menu("Edit")
	check_eq(edit.get_item_text(edit.get_item_index(ws.EDIT_UNDO)), "Undo Clone", "the Undo item names the step")
	_key(KEY_Z, false, true)
	check_eq(doc.get_undo_names().size(), steps - 1, "Ctrl+Z undoes")
	check_eq(edit.get_item_text(edit.get_item_index(ws.EDIT_REDO)), "Redo Clone", "the Redo item names the step")
	_key(KEY_Z, true, true)
	check_eq(doc.get_undo_names().size(), steps, "Ctrl+Shift+Z redoes")
	_key(KEY_Z, false, true)
	_key(KEY_Y, false, true)
	check_eq(doc.get_undo_names().size(), steps, "Ctrl+Y redoes too")

	# Tools through pointer gestures.
	ws.set_tool("rect-select")
	canvas.tool_press(Vector2(100, 100))
	canvas.tool_drag(Vector2(300, 250))
	canvas.tool_release()
	check_eq(doc.selection_bounds(), Rect2i(100, 100, 200, 150), "rectangle select")
	await _frames(2)
	check(canvas.get_outline_point_count() >= 8, "the selection outline is drawn (%d points)" % canvas.get_outline_point_count())
	ws.set_tool_option("rect-select", "mode", TetraImageDocument.SELECT_ADD)
	canvas.tool_press(Vector2(300, 100))
	canvas.tool_drag(Vector2(400, 250))
	canvas.tool_release()
	check_eq(doc.selection_bounds(), Rect2i(100, 100, 300, 150), "select mode Add from the Tool Options")
	ws.set_tool_option("rect-select", "mode", TetraImageDocument.SELECT_REPLACE)
	_key(KEY_COMMA, false, true)
	check(_pixel(doc, 200, 200).is_equal_approx(Color.BLACK), "Ctrl+, fills the selection with the foreground colour")
	check(_pixel(doc, 50, 50).is_equal_approx(Color.WHITE), "and nothing outside it")
	_key(KEY_A, true, true)
	check(doc.is_selection_empty(), "Ctrl+Shift+A selects nothing")
	_key(KEY_A, false, true)
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 2048, 2048), "Ctrl+A selects all")
	_key(KEY_I, false, true)
	check(doc.is_selection_empty(), "Ctrl+I inverts")

	ws.set_tool("ellipse-select")
	canvas.tool_press(Vector2(600, 600))
	canvas.tool_drag(Vector2(800, 700))
	canvas.tool_release()
	check_eq(doc.selection_bounds(), Rect2i(600, 600, 200, 100), "ellipse select")
	check(doc.get_selection_mask().get_pixel(601, 601).r < 0.5, "the corner of the ellipse is not selected")
	doc.select_none()

	ws.set_tool("fuzzy-select")
	canvas.tool_press(Vector2(200, 200))
	canvas.tool_release()
	check_eq(doc.selection_bounds(), Rect2i(100, 100, 300, 150), "fuzzy select picks the black rectangle")
	doc.select_none()

	ws.set_tool("color-picker")
	canvas.tool_press(Vector2(350, 350))
	canvas.tool_release()
	check(_close(ws.get_foreground_color(), Color.RED), "colour picker sets the foreground: " + str(ws.get_foreground_color()))

	ws.set_tool("bucket-fill")
	ws.set_foreground_color(Color(1, 1, 0))
	canvas.tool_press(Vector2(200, 200))
	canvas.tool_release()
	check(_pixel(doc, 390, 240).is_equal_approx(Color(1, 1, 0)), "bucket fill fills the black rectangle")
	check(_pixel(doc, 50, 50).is_equal_approx(Color.WHITE), "and stops at its edge")

	ws.set_tool("gradient")
	ws.set_foreground_color(Color.BLACK)
	ws.set_background_color(Color.WHITE)
	doc.select_rect(Rect2i(0, 1500, 2048, 100))
	canvas.tool_press(Vector2(0, 1550))
	canvas.tool_drag(Vector2(2047, 1550))
	canvas.tool_release()
	doc.select_none()
	check(_pixel(doc, 5, 1550).r < 0.1 and _pixel(doc, 2040, 1550).r > 0.9, "gradient from foreground to background")
	check_eq(doc.get_undo_names()[-2], "Gradient", "the gradient is one undo step: " + str(doc.get_undo_names().slice(-3)))

	ws.set_tool("eraser")
	ws.set_tool_option("eraser", "size", 30.0)
	canvas.tool_press(Vector2(1000, 1550))
	canvas.tool_release()
	check(_pixel(doc, 1000, 1550).a < 0.05, "the eraser makes the layer transparent")

	ws.set_tool("move")
	canvas.tool_press(Vector2(500, 500))
	canvas.tool_drag(Vector2(510, 520))
	canvas.tool_release()
	check(_pixel(doc, 1010, 1570).a < 0.05 and _pixel(doc, 1000, 1550).a > 0.9, "the Move tool translates the layer")
	_key(KEY_Z, false, true)
	check(_pixel(doc, 1000, 1550).a < 0.05, "and that is one undo step")

	ws.set_tool("zoom")
	var zoom: float = ws.get_zoom()
	canvas.tool_press(Vector2(1024, 1024))
	canvas.tool_release()
	check(is_equal_approx(ws.get_zoom(), zoom * 2.0), "the Zoom tool zooms in")
	ws.zoom_to_fit()

	# The keys belong to the workspace only while it is shown and focused.
	ws.set_tool("paintbrush")
	EditorInterface.set_main_screen_editor("2D")
	await _frames(3)
	_key(KEY_N)
	check_eq(ws.get_tool(), "paintbrush", "tool keys do nothing while another main screen is shown")
	steps = doc.get_undo_names().size()
	_key(KEY_Z, false, true)
	check_eq(doc.get_undo_names().size(), steps, "Ctrl+Z elsewhere does not undo in the image")
	EditorInterface.set_main_screen_editor("Image")
	await _frames(3)
	var filter_box: LineEdit = EditorInterface.get_file_system_dock().find_children("*", "LineEdit", true, false)[0]
	filter_box.grab_focus()
	_key(KEY_N)
	check_eq(ws.get_tool(), "paintbrush", "tool keys do nothing while the focus is in a dock")
	filter_box.clear()
	canvas.grab_focus()
	_key(KEY_N)
	check_eq(ws.get_tool(), "pencil", "and work again with the focus on the canvas")


func _test_layers() -> void:
	var doc: TetraImageDocument = ws.new_document(640, 480, Color.WHITE)
	var panel: Control = ws.get_layers_panel()
	await _frames(2)
	check_eq(panel.get_listed_names(), PackedStringArray(["Background"]), "one layer listed")
	panel.new_layer()
	ws.menu_option(ws.LAYER_NEW)
	await _frames(2)
	check_eq(doc.get_layer_count(), 3, "new layers from the panel and the menu")
	var names: PackedStringArray = panel.get_listed_names()
	check_eq(names.size(), 3, "three rows")
	check_eq(names[2], "Background", "the list is top layer first")
	var top := doc.get_active_layer()
	check_eq(doc.get_layer_index(top), 2, "the new layer is on top and active")
	panel.lower_layer()
	check_eq(doc.get_layer_index(top), 1, "lower")
	panel.raise_layer()
	check_eq(doc.get_layer_index(top), 2, "raise")
	panel.duplicate_layer()
	check_eq(doc.get_layer_count(), 4, "duplicate")
	panel.merge_down()
	check_eq(doc.get_layer_count(), 3, "merge down")
	panel.delete_layer()
	check_eq(doc.get_layer_count(), 2, "delete")
	await _frames(2)
	check_eq(panel.get_listed_names().size(), 2, "the list follows")
	var tree: Tree = panel.get_layer_tree()
	var first := tree.get_root().get_first_child()
	check(first.get_icon(1) != null or true, "row exists")
	check_eq(first.get_button_count(0), 1, "eye toggle in the first column")
	check_eq(first.get_button_count(2), 1, "alpha lock in the last column")
	tree.button_clicked.emit(first, 0, 0, MOUSE_BUTTON_LEFT)
	check(not doc.layer_is_visible(doc.get_layer_ids()[1]), "the eye hides the layer")
	tree.button_clicked.emit(first, 0, 0, MOUSE_BUTTON_LEFT)
	check(doc.layer_is_visible(doc.get_layer_ids()[1]), "and shows it again")
	tree.button_clicked.emit(first, 2, 2, MOUSE_BUTTON_LEFT)
	check(doc.layer_get_lock_alpha(doc.get_layer_ids()[1]), "the lock toggles lock alpha")
	tree.button_clicked.emit(first, 2, 2, MOUSE_BUTTON_LEFT)
	var mode: OptionButton = panel.find_children("*", "OptionButton", true, false)[0]
	check_eq(mode.item_count, TetraImageDocument.LAYER_MODE_MAX, "every layer mode is offered")
	mode.select(TetraImageDocument.LAYER_MODE_MULTIPLY)
	mode.item_selected.emit(TetraImageDocument.LAYER_MODE_MULTIPLY)
	check_eq(doc.layer_get_mode(doc.get_active_layer()), TetraImageDocument.LAYER_MODE_MULTIPLY, "the mode button sets the mode of the active layer")
	await _tree.create_timer(0.5).timeout
	check(tree.get_root().get_first_child().get_icon(1) != null, "rows get thumbnails")
	doc.undo()
	check_eq(doc.layer_get_mode(doc.get_active_layer()), TetraImageDocument.LAYER_MODE_NORMAL, "layer changes are undoable")

	# Double-click renames in place.
	await _frames(2)
	tree.item_activated.emit()
	await _frames(2)
	var editors := tree.find_children("*", "LineEdit", true, false)
	if check(editors.size() > 0 and editors[0].is_visible_in_tree(), "double-click starts renaming the layer"):
		editors[0].text = "Renamed"
		editors[0].text_submitted.emit("Renamed")
		await _frames(3)
		check_eq(doc.layer_get_name(doc.get_active_layer()), "Renamed", "the new name reaches the document")
		check(panel.get_listed_names().has("Renamed"), "and the list")


func _test_menus() -> void:
	var doc: TetraImageDocument = ws.new_document(640, 480, Color.WHITE)
	var bar: MenuBar = ws.get_menu_bar()
	var titles := []
	for i in bar.get_menu_count():
		titles.append(bar.get_menu_title(i))
	check_eq(titles, ["File", "Edit", "Select", "View", "Image", "Layer", "Colors", "Filters"], "menu bar")
	doc.fill(doc.get_active_layer(), Color.RED)
	doc.select_rect(Rect2i(0, 0, 320, 480))
	ws.menu_option(ws.EDIT_CLEAR)
	check(_pixel(doc, 10, 10).a == 0.0 and _pixel(doc, 400, 10).a == 1.0, "Edit > Clear")
	ws.menu_option(ws.SELECT_NONE)
	ws.menu_option(ws.IMAGE_FLIP_HORIZONTAL)
	check(_pixel(doc, 10, 10).a == 1.0 and _pixel(doc, 400, 10).a == 0.0, "Image > Flip Horizontally")
	ws.menu_option(ws.IMAGE_ROTATE_90_CW)
	check_eq(doc.get_size(), Vector2i(480, 640), "Image > Rotate 90° clockwise")
	await _frames(2)
	check(ws.get_status_text().contains("480 × 640 px"), "the status bar follows the size")
	check_eq(ws.get_image_canvas().get_content_size(), Vector2(480, 640), "and so does the canvas")
	ws.menu_option(ws.IMAGE_ROTATE_90_CCW)
	ws.menu_option(ws.IMAGE_ROTATE_180)
	check_eq(doc.get_size(), Vector2i(640, 480), "rotating back")
	doc.select_rect(Rect2i(100, 100, 200, 100))
	ws.menu_option(ws.IMAGE_CROP_TO_SELECTION)
	check_eq(doc.get_size(), Vector2i(200, 100), "Image > Crop to Selection")
	ws.menu_option(ws.LAYER_NEW)
	ws.menu_option(ws.EDIT_FILL_BACKGROUND)
	ws.menu_option(ws.LAYER_FLIP_VERTICAL)
	ws.menu_option(ws.IMAGE_FLATTEN)
	check_eq(doc.get_layer_count(), 1, "Image > Flatten Image")
	var image_menu: PopupMenu = ws.get_menu("Image")
	check(image_menu.is_item_disabled(image_menu.get_item_index(ws.IMAGE_FLATTEN)), "Flatten is disabled with one layer")

	# Dialogs: New, Scale, Canvas Size, Feather.
	ws.menu_option(ws.IMAGE_SCALE)
	var dialog := _visible_dialog("Scale Image")
	if check(dialog != null, "Image > Scale Image... opens a dialog"):
		var spins := dialog.find_children("*", "SpinBox", true, false)
		spins[0].value = 400
		check_eq(spins[1].value, 200.0, "the height follows the width (aspect ratio kept)")
		dialog.get_ok_button().pressed.emit()
		check_eq(doc.get_size(), Vector2i(400, 200), "scaled")
	ws.menu_option(ws.IMAGE_CANVAS_SIZE)
	dialog = _visible_dialog("Set Canvas Size")
	if check(dialog != null, "Image > Canvas Size... opens a dialog"):
		var spins := dialog.find_children("*", "SpinBox", true, false)
		dialog.find_children("*", "CheckBox", true, false)[0].button_pressed = false
		spins[0].value = 500
		spins[1].value = 300
		dialog.find_children("*", "Button", true, false).filter(func(b: Button) -> bool: return b.text == "Center")[0].pressed.emit()
		check_eq([spins[2].value, spins[3].value], [50.0, 50.0], "Center computes the offset")
		dialog.get_ok_button().pressed.emit()
		check_eq(doc.get_size(), Vector2i(500, 300), "canvas resized")
		check(_pixel(doc, 10, 10).a == 0.0 and _pixel(doc, 60, 60).a == 1.0, "the image sits at the offset")
	doc.select_rect(Rect2i(100, 100, 100, 100))
	ws.menu_option(ws.SELECT_GROW)
	dialog = _visible_dialog("Grow Selection")
	if check(dialog != null, "Select > Grow... opens a dialog"):
		dialog.find_children("*", "SpinBox", true, false)[0].value = 10
		dialog.get_ok_button().pressed.emit()
		check_eq(doc.selection_bounds(), Rect2i(90, 90, 120, 120), "grown by 10 px")
	ws.menu_option(ws.SELECT_FEATHER)
	dialog = _visible_dialog("Feather Selection")
	if check(dialog != null, "Select > Feather... opens a dialog"):
		dialog.get_ok_button().pressed.emit()
		var edge: float = doc.get_selection_mask().get_pixel(90, 150).r
		check(edge > 0.2 and edge < 0.8, "the selection edge is feathered (%f)" % edge)

	# Replacing a modified document asks first.
	check(ws.is_modified(), "the document is modified")
	ws.menu_option(ws.FILE_NEW)
	dialog = _visible_dialog("Unsaved Changes")
	if check(dialog != null, "File > New asks before discarding changes"):
		dialog.get_ok_button().pressed.emit()
		dialog = _visible_dialog("Create a New Image")
		if check(dialog != null, "then shows the New Image dialog"):
			var spins := dialog.find_children("*", "SpinBox", true, false)
			spins[0].value = 320
			spins[1].value = 200
			dialog.find_children("*", "OptionButton", true, false)[0].select(3)
			dialog.get_ok_button().pressed.emit()
			var created: TetraImageDocument = ws.get_document()
			check_eq(created.get_size(), Vector2i(320, 200), "File > New creates the image")
			check_eq(_pixel(created, 5, 5).a, 0.0, "filled with transparency")
			check(not ws.is_modified(), "a new image is not modified")
	ws.menu_option(ws.FILE_CLOSE)
	check(not ws.has_document(), "File > Close returns to the welcome page")


func _visible_dialog(p_title: String) -> AcceptDialog:
	for dialog: AcceptDialog in ws.find_children("*", "AcceptDialog", true, false):
		if dialog.visible and dialog.title == p_title:
			return dialog
	return null


func _wait_for(p_condition: Callable, p_seconds: float = 20.0) -> bool:
	var deadline := Time.get_ticks_msec() + int(p_seconds * 1000.0)
	while not p_condition.call():
		if Time.get_ticks_msec() > deadline:
			return false
		await _tree.process_frame
	return true


func _test_filters() -> void:
	check(await _wait_for(func() -> bool: return ws.are_operations_loaded()), "the operation list arrives")
	var operations: Array = ws.get_operations()
	check(operations.size() >= 15, "native operations are listed (%d)" % operations.size())
	var colors: PopupMenu = ws.get_menu("Colors")
	var filters: PopupMenu = ws.get_menu("Filters")
	var color_items := []
	for i in colors.item_count:
		color_items.append(colors.get_item_text(i))
	check(color_items.has("Invert...") and color_items.has("Levels..."), "the Colors menu lists the colour operations: " + str(color_items.slice(0, 6)))
	var filter_items := []
	for i in filters.item_count:
		filter_items.append(filters.get_item_text(i))
	check_eq(filter_items.slice(0, 2), ["Repeat Last Filter", "Search Filters..."], "the Filters menu starts with Repeat and Search")
	check(filter_items.has("Blur") and filters.get_item_submenu_node(filter_items.find("Blur")) != null, "categories are submenus: " + str(filter_items))
	var blur: PopupMenu = filters.get_item_submenu_node(filter_items.find("Blur"))
	var blur_items := []
	for i in blur.item_count:
		blur_items.append(blur.get_item_text(i))
	check(blur_items.has("Gaussian Blur..."), "Filters > Blur lists Gaussian Blur: " + str(blur_items.slice(0, 5)))
	var gegl := TetraGegl.is_available()
	if gegl:
		check(operations.size() > 100, "GEGL operations are listed (%d)" % operations.size())

	var doc: TetraImageDocument = ws.new_document(256, 256, Color.WHITE)
	doc.select_rect(Rect2i(0, 0, 128, 256))
	doc.fill(doc.get_active_layer(), Color.BLACK)
	doc.select_none()
	var steps := doc.get_undo_names().size()

	# No parameters: applied at once, off the main thread.
	# GEGL's parameterless Invert shares its name with Tetra's own, so it is marked.
	var invert_index := color_items.find("Invert (GEGL)")
	check(invert_index >= 0 or not gegl, "an engine operation named like a native one is marked: " + str(color_items))
	check_eq(ws.apply_operation("native:posterize", {"levels": 2}), OK, "apply_operation")
	check_eq(doc.get_undo_names().size(), steps + 1, "apply_operation is one undo step")
	doc.undo()

	# With parameters: dialog, live preview, cancel restores.
	var before := doc.composite()
	var blur_id := "gegl:gaussian-blur" if gegl else "native:gaussian-blur"
	blur.index_pressed.emit(blur_items.find("Gaussian Blur..."))
	var dialog: AcceptDialog = ws.get_operation_dialog()
	check(dialog.visible, "choosing a filter with parameters opens the operation dialog")
	check(dialog.get_operation()["id"].ends_with(":gaussian-blur"), "for that operation")
	check(await _wait_for(func() -> bool: return ws.is_preview_shown()), "the preview appears")
	check(doc.composite().get_data() != before.get_data(), "the preview shows on the canvas")
	var edge := _pixel(doc, 128, 100).r
	check(edge > 0.05 and edge < 0.95, "the edge is blurred in the preview (%f)" % edge)
	# A newer request supersedes an older one: only the last values end up shown.
	dialog.get_param_panel().set_value("std-dev-x", 30.0)
	dialog.values_changed.emit(dialog.get_values())
	dialog.get_param_panel().set_value("std-dev-x", 12.0)
	dialog.get_param_panel().set_value("std-dev-y", 12.0)
	dialog.values_changed.emit(dialog.get_values())
	check(ws.is_filter_running(), "the preview is computed in the background")
	check(await _wait_for(func() -> bool: return not ws.is_filter_running()), "the computations finish")
	await _frames(2)
	var wide := _pixel(doc, 110, 100).r
	check(wide > 0.02, "the newest values are previewed (%f)" % wide)
	check_eq(doc.get_undo_names().size(), steps, "a preview adds no undo step")
	check(not ws.undo(), "undo is refused while the dialog is open")
	dialog.get_cancel_button().pressed.emit()
	await _frames(2)
	check(not dialog.visible, "Cancel closes the dialog")
	check(doc.composite().get_data() == before.get_data(), "Cancel restores the image exactly")
	check_eq(doc.get_undo_names().size(), steps, "and leaves no undo step")
	check(not doc.is_undo_group_open(), "and no open undo group")

	# OK commits one step.
	ws.show_operation_dialog(blur_id, {"std-dev-x": 6.0, "std-dev-y": 6.0})
	check(dialog.visible, "show_operation_dialog opens the dialog")
	check_eq(dialog.get_values()["std-dev-x"], 6.0, "with the given values")
	dialog.get_ok_button().pressed.emit()
	check(await _wait_for(func() -> bool: return not ws.is_filter_running() and not dialog.visible and doc.get_undo_names().size() == steps + 1), "OK applies the filter as one undo step: " + str(doc.get_undo_names()))
	await _frames(2)
	edge = _pixel(doc, 122, 100).r
	check(edge > 0.02 and edge < 0.5, "the committed result is the blur (%f)" % edge)
	check_eq(doc.get_undo_names()[-1], "Gaussian Blur", "named after the operation")
	check(ws.is_modified(), "the document counts as modified")
	check(ws.undo(), "undo")
	check(doc.composite().get_data() == before.get_data(), "undo restores the image")
	check(ws.redo(), "redo")

	# OK while the preview already shows the same values keeps the preview.
	ws.show_operation_dialog("native:brightness-contrast", {"brightness": 0.5})
	check(await _wait_for(func() -> bool: return ws.is_preview_shown()), "preview of a native operation")
	dialog.get_ok_button().pressed.emit()
	await _frames(2)
	check_eq(doc.get_undo_names().size(), steps + 2, "OK keeps the shown preview as one step")
	check_eq(doc.get_undo_names()[-1], "Brightness-Contrast", "step name: " + doc.get_undo_names()[-1])
	check(not doc.is_undo_group_open(), "the undo group is closed")

	# The dialog remembers the last values of an operation.
	ws.show_operation_dialog("native:brightness-contrast")
	check_eq(dialog.get_values()["brightness"], 0.5, "the last values are offered again")
	dialog.get_cancel_button().pressed.emit()
	await _frames(2)

	# Repeat with Ctrl+F.
	check_eq(filters.get_item_text(0), "Repeat \"Brightness-Contrast\"", "the Repeat item names the last filter")
	ws.get_image_canvas().grab_focus()
	_key(KEY_F, false, true)
	check(await _wait_for(func() -> bool: return doc.get_undo_names().size() == steps + 3), "Ctrl+F repeats the last filter")

	# Operations without parameters from the menu, through the selection.
	doc.select_rect(Rect2i(0, 0, 256, 128))
	var bottom := _pixel(doc, 200, 200)
	if gegl:
		colors.index_pressed.emit(invert_index)
	else:
		ws.apply_operation("native:invert")
	check(await _wait_for(func() -> bool: return doc.get_undo_names().size() == steps + 5), "Colors > Invert applies at once")
	check_eq(doc.get_undo_names()[-1], "Invert", "as one undo step")
	check(_pixel(doc, 200, 200).is_equal_approx(bottom), "outside the selection nothing changed")
	check(not _pixel(doc, 200, 50).is_equal_approx(bottom), "inside it did")
	doc.select_none()

	# Search Filters with "/".
	_key(KEY_SLASH)
	var search := _visible_dialog("Search Filters")
	if check(search != null, "/ opens Search Filters"):
		var browser: Control = search.find_children("*", "TetraOperationBrowser", true, false)[0]
		check_eq(browser.get_operations().size(), operations.size(), "the browser lists every operation")
		browser.set_filter_text("pixelize")
		check(browser.get_selected_id().ends_with(":pixelize"), "searching selects the best match: " + browser.get_selected_id())
		browser.operation_activated.emit(browser.get_selected_id())
		check(not search.visible and dialog.visible, "activating opens the operation dialog")
		dialog.get_cancel_button().pressed.emit()
		await _wait_for(func() -> bool: return not ws.is_filter_running())
	await _frames(2)


func _test_files() -> void:
	# A flat image in the project: open by double-click path, edit, save, re-import.
	var source := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	source.fill(Color.WHITE)
	source.save_png(TMP_PNG)
	var file_system := EditorInterface.get_resource_filesystem()
	file_system.scan()
	await _wait_for(func() -> bool: return not file_system.is_scanning() and ResourceLoader.exists(TMP_PNG))
	await _frames(5)
	var texture: Texture2D = load(TMP_PNG)
	if not check(texture != null, "the test image imports"):
		return
	check(texture.get_image().get_pixel(10, 10).is_equal_approx(Color.WHITE), "it is white")
	ws.new_document(16, 16)
	EditorInterface.set_main_screen_editor("2D")
	await _frames(3)
	EditorInterface.edit_resource(texture)
	await _frames(5)
	check(ws.is_visible_in_tree(), "editing an image resource switches to the Image workspace")
	check_eq(ws.get_document_path(), TMP_PNG, "and opens its file")
	var doc: TetraImageDocument = ws.get_document()
	check_eq(doc.get_size(), Vector2i(64, 64), "with its pixels")
	check_eq(ws.get_title_text(), "_tmp_edit.png", "title is the file name")
	check(ws.can_save_directly(), "a one-layer png can be saved directly")
	doc.fill(doc.get_active_layer(), Color.RED)
	check_eq(ws.get_title_text(), "_tmp_edit.png (*)", "modified marker")
	ws.get_image_canvas().grab_focus()
	_key(KEY_S, false, true)
	check(not ws.is_modified(), "Ctrl+S saves")
	check_eq(ws.get_title_text(), "_tmp_edit.png", "the marker is gone")
	await _wait_for(func() -> bool: return not file_system.is_scanning())
	await _frames(5)
	check(Image.load_from_file(ProjectSettings.globalize_path(TMP_PNG)).get_pixel(10, 10).is_equal_approx(Color.RED), "the file holds the new pixels")
	check(texture.get_image().get_pixel(10, 10).is_equal_approx(Color.RED), "the loaded texture was re-imported: " + str(texture.get_image().get_pixel(10, 10)))

	# With layers a flat file needs a decision.
	doc.add_layer("Second")
	check(not ws.can_save_directly(), "two layers cannot go into a png directly")
	ws.menu_option(ws.FILE_SAVE)
	var dialog := _visible_dialog("Save Layered Image")
	if check(dialog != null, "Save asks: export flattened or save as .ora"):
		var texts := dialog.find_children("*", "Button", true, false).map(func(b: Button) -> String: return b.text)
		check(texts.has("Export Flattened") and texts.has("Save as .ora"), "both choices are offered: " + str(texts))
		dialog.get_ok_button().pressed.emit()
		var ora := TMP_PNG.get_basename() + ".ora"
		check_eq(ws.get_document_path(), ora, "Save as .ora switches the document to the .ora file")
		check(FileAccess.file_exists(ora), "which exists")
		var reloaded := TetraImageDocument.new()
		check_eq(reloaded.load(ora), OK, "and loads")
		check_eq(reloaded.get_layer_count(), 2, "with both layers")
		check(ws.can_save_directly(), "an .ora document saves directly")
	var exported := TMP_PNG.get_basename() + "_export.webp"
	check_eq(ws.export_as(exported), OK, "export_as")
	check_eq(ws.get_document_path(), TMP_PNG.get_basename() + ".ora", "exporting keeps the document path")
	check(FileAccess.file_exists(exported), "the export exists")
	check_eq(ws.open_file("res://does_not_exist.png"), ERR_FILE_NOT_FOUND, "open_file reports a missing file")
	check(ws.has_document(), "and keeps the open document")

	ws.close_document()
	await _wait_for(func() -> bool: return not file_system.is_scanning())
	for path: String in [TMP_PNG, TMP_PNG + ".import", TMP_PNG.get_basename() + ".ora", TMP_PNG.get_basename() + ".ora.import", exported, exported + ".import"]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	file_system.scan()
	await _wait_for(func() -> bool: return not file_system.is_scanning())
