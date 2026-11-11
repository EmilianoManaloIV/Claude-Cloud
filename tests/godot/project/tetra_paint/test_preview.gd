extends "res://tetra_test.gd"
## The two calls the Image workspace builds live previews and interactive drags
## on: cancel_undo_group() and apply_filtered_image().


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 1e-5) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func run() -> void:
	var doc := TetraImageDocument.new()
	doc.create(8, 8, Color(0.2, 0.5, 0.8, 1))
	var layer := doc.get_active_layer()
	doc.fill(layer, Color(0.25, 0.5, 0.75, 1))
	doc.fill(layer, Color(0.2, 0.5, 0.8, 1))
	doc.undo()
	check_eq(doc.get_undo_names(), PackedStringArray(["Fill"]), "history before the preview")
	check_eq(doc.get_redo_names(), PackedStringArray(["Fill"]), "redo before the preview")

	# A cancelled group restores pixels, layers and the selection and leaves both stacks alone.
	check(not doc.is_undo_group_open(), "no group is open at first")
	doc.begin_undo_group("Preview")
	check(doc.is_undo_group_open(), "begin_undo_group opens a group")
	doc.fill(layer, Color(1, 0, 0, 1))
	var extra := doc.add_layer("Extra")
	doc.fill(extra, Color(0, 1, 0, 1))
	doc.select_rect(Rect2i(1, 1, 3, 3))
	doc.translate_layer(layer, Vector2i(3, 0))
	doc.cancel_undo_group()
	check(not doc.is_undo_group_open(), "cancel_undo_group closes the group")
	check_eq(doc.get_layer_count(), 1, "the layer added in the group is gone")
	check_eq(doc.get_active_layer(), layer, "the active layer is restored")
	check(doc.is_selection_empty(), "the selection made in the group is gone")
	check_color(doc.layer_get_image(layer).get_pixel(0, 0), Color(0.25, 0.5, 0.75, 1), "pixels are restored")
	check_color(doc.layer_get_image(layer).get_pixel(7, 7), Color(0.25, 0.5, 0.75, 1), "pixels are restored at the far corner")
	check_eq(doc.get_undo_names(), PackedStringArray(["Fill"]), "a cancelled group records nothing")
	check_eq(doc.get_redo_names(), PackedStringArray(["Fill"]), "a cancelled group keeps the redo stack")
	check(doc.redo(), "redo still works after a cancelled group")
	check_color(doc.layer_get_image(layer).get_pixel(0, 0), Color(0.2, 0.5, 0.8, 1), "redo result")

	# A cancelled nested group ends every level.
	doc.begin_undo_group("Outer")
	doc.begin_undo_group("Inner")
	doc.fill(layer, Color(0, 0, 0, 1))
	doc.cancel_undo_group()
	check(not doc.is_undo_group_open(), "cancelling inside a nested group closes all levels")
	check_color(doc.layer_get_image(layer).get_pixel(4, 4), Color(0.2, 0.5, 0.8, 1), "nested cancel restores the pixels")

	# apply_filtered_image() gives the same pixels and the same single step as apply_operation().
	var mask := Image.create_empty(8, 8, false, Image.FORMAT_RF)
	mask.fill_rect(Rect2i(0, 0, 4, 8), Color(1, 0, 0))
	mask.fill_rect(Rect2i(4, 0, 2, 8), Color(0.5, 0, 0))
	doc.select_mask(mask)
	var steps := doc.get_undo_names().size()
	var filtered := TetraImageDocument.filter_image(doc.layer_get_image(layer), "native:invert", {"linear": true})
	check_eq(doc.apply_filtered_image(layer, filtered, "Invert"), OK, "apply_filtered_image succeeds")
	check_eq(doc.get_undo_names().size(), steps + 1, "apply_filtered_image is one undo step")
	check_eq(doc.get_undo_names()[steps], "Invert", "the step carries the given name")
	var via_image := doc.layer_get_image(layer)
	doc.undo()
	doc.apply_operation(layer, "native:invert", {"linear": true})
	var via_operation := doc.layer_get_image(layer)
	check_eq(via_image.get_data(), via_operation.get_data(), "same pixels as apply_operation")
	check_color(via_image.get_pixel(0, 0), Color(0.8, 0.5, 0.2, 1), "selected pixels are filtered")
	check_color(via_image.get_pixel(4, 0), Color(0.5, 0.5, 0.5, 1), "half-selected pixels get half")
	check_color(via_image.get_pixel(7, 0), Color(0.2, 0.5, 0.8, 1), "unselected pixels are kept")
	doc.undo()
	doc.select_none()

	doc.layer_set_lock_alpha(layer, true)
	var faded := doc.layer_get_image(layer)
	faded.fill(Color(1, 1, 1, 0.25))
	doc.apply_filtered_image(layer, faded, "Fade")
	check_color(doc.layer_get_image(layer).get_pixel(3, 3), Color(1, 1, 1, 1), "the alpha lock holds for apply_filtered_image")
	doc.layer_set_lock_alpha(layer, false)

	check_eq(doc.apply_filtered_image(layer, Image.create_empty(4, 4, false, Image.FORMAT_RGBAF), "Bad"), ERR_INVALID_PARAMETER, "a wrong size is refused")
	check_eq(doc.apply_filtered_image(9999, filtered, "Bad"), ERR_DOES_NOT_EXIST, "an unknown layer is refused")

	# The preview cycle the workspace runs: show, replace, commit as one step.
	steps = doc.get_undo_names().size()
	doc.begin_undo_group("Invert")
	doc.apply_filtered_image(layer, filtered, "Invert")
	doc.cancel_undo_group()
	doc.begin_undo_group("Invert")
	doc.apply_filtered_image(layer, filtered, "Invert")
	doc.end_undo_group()
	check_eq(doc.get_undo_names().size(), steps + 1, "a previewed operation commits as one step")
	check(doc.undo(), "the committed preview can be undone")
	check_color(doc.layer_get_image(layer).get_pixel(3, 3), Color(1, 1, 1, 1), "undo of the committed preview")
