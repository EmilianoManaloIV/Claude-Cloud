extends "res://tetra_test.gd"
## Selection mask operations of TetraImageDocument.


func mask_sum(p_doc: TetraImageDocument) -> float:
	var mask := p_doc.get_selection_mask()
	var total := 0.0
	for y in mask.get_height():
		for x in mask.get_width():
			total += mask.get_pixel(x, y).r
	return total


func mask_at(p_doc: TetraImageDocument, p_x: int, p_y: int) -> float:
	return p_doc.get_selection_mask().get_pixel(p_x, p_y).r


func run() -> void:
	test_rect_ops()
	test_shapes()
	test_by_color()
	test_feather_grow_shrink()
	test_undo()


func test_rect_ops() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 8)
	var events := [0]
	doc.selection_changed.connect(func() -> void: events[0] += 1)
	check(doc.is_selection_empty(), "a new document has no selection")
	check_eq(doc.selection_bounds(), Rect2i(), "empty bounds")
	check_eq(doc.get_selection_mask().get_format(), Image.FORMAT_RF, "mask format")
	check_eq(doc.get_selection_mask().get_size(), Vector2i(10, 8), "mask size")
	check_near(mask_sum(doc), 0.0, 0.0, "empty mask is zero")

	doc.select_rect(Rect2i(2, 1, 4, 3))
	check_eq(events[0], 1, "selection_changed emitted")
	check(not doc.is_selection_empty(), "rect selects")
	check_eq(doc.selection_bounds(), Rect2i(2, 1, 4, 3), "rect bounds")
	check_near(mask_at(doc, 2, 1), 1.0, 0.0, "inside top-left")
	check_near(mask_at(doc, 5, 3), 1.0, 0.0, "inside bottom-right")
	check_near(mask_at(doc, 6, 3), 0.0, 0.0, "outside right")
	check_near(mask_at(doc, 1, 1), 0.0, 0.0, "outside left")

	doc.select_rect(Rect2i(5, 2, 3, 3), TetraImageDocument.SELECT_ADD)
	check_eq(doc.selection_bounds(), Rect2i(2, 1, 6, 4), "add grows the bounds")
	doc.select_rect(Rect2i(2, 1, 2, 3), TetraImageDocument.SELECT_SUBTRACT)
	check_eq(doc.selection_bounds(), Rect2i(4, 1, 4, 4), "subtract")
	check_near(mask_at(doc, 2, 1), 0.0, 0.0, "subtracted pixel")
	doc.select_rect(Rect2i(0, 0, 6, 8), TetraImageDocument.SELECT_INTERSECT)
	check_eq(doc.selection_bounds(), Rect2i(4, 1, 2, 4), "intersect")
	check_near(mask_sum(doc), 7.0, 0.0, "seven pixels remain")

	doc.select_rect(Rect2i(-5, -5, 7, 7))
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 2, 2), "rect is clipped to the canvas")
	doc.select_all()
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 10, 8), "select all")
	doc.select_none()
	check(doc.is_selection_empty(), "select none")
	doc.invert_selection()
	check_near(mask_sum(doc), 80.0, 0.0, "inverting nothing selects everything")
	doc.select_rect(Rect2i(0, 0, 10, 7))
	doc.invert_selection()
	check_eq(doc.selection_bounds(), Rect2i(0, 7, 10, 1), "invert")

	var custom := Image.create_empty(10, 8, false, Image.FORMAT_RF)
	custom.set_pixel(3, 4, Color(0.25, 0, 0))
	doc.select_mask(custom)
	check_near(mask_at(doc, 3, 4), 0.25, 1e-6, "mask values are kept")
	check_eq(doc.selection_bounds(), Rect2i(3, 4, 1, 1), "bounds of a partial selection")


func test_shapes() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 8)
	# A circle of radius 3 centred on (4, 4); rows of 4, 6, 6, 6, 6, 4 pixel centres are inside.
	doc.select_ellipse(Rect2i(1, 1, 6, 6), TetraImageDocument.SELECT_REPLACE, false)
	check_near(mask_sum(doc), 32.0, 0.0, "hard ellipse pixel count")
	check_near(mask_at(doc, 1, 1), 0.0, 0.0, "hard ellipse corner")
	check_near(mask_at(doc, 2, 1), 1.0, 0.0, "hard ellipse first row")
	check_near(mask_at(doc, 1, 2), 1.0, 0.0, "hard ellipse second row")
	check_eq(doc.selection_bounds(), Rect2i(1, 1, 6, 6), "ellipse bounds")

	doc.select_ellipse(Rect2i(1, 1, 6, 6))
	check_near(mask_sum(doc), PI * 9.0, 0.15, "antialiased ellipse area")
	check_near(mask_at(doc, 4, 4), 1.0, 0.0, "antialiased ellipse centre")
	var corner := mask_at(doc, 1, 1)
	check(corner > 0.0 and corner < 0.5, "antialiased ellipse edge is partial: %f" % corner)
	check_near(mask_at(doc, 1, 1), mask_at(doc, 6, 6), 1e-6, "ellipse is symmetric")

	# Right triangle (1,1) (9,1) (1,7): rows of 7, 6, 5, 3, 2, 1 pixel centres; area 24.
	var triangle := PackedVector2Array([Vector2(1, 1), Vector2(9, 1), Vector2(1, 7)])
	doc.select_polygon(triangle, TetraImageDocument.SELECT_REPLACE, false)
	check_near(mask_sum(doc), 24.0, 0.0, "hard polygon pixel count")
	check_near(mask_at(doc, 7, 1), 1.0, 0.0, "hard polygon first row end")
	check_near(mask_at(doc, 8, 1), 0.0, 0.0, "hard polygon beyond the edge")
	doc.select_polygon(triangle)
	check_near(mask_sum(doc), 24.0, 1e-3, "antialiased polygon area")
	var edge := mask_at(doc, 8, 1)
	check(edge > 0.0 and edge < 1.0, "antialiased polygon edge is partial: %f" % edge)
	check_near(mask_at(doc, 2, 2), 1.0, 0.0, "polygon interior")

	# Shapes combine like rectangles.
	doc.select_rect(Rect2i(0, 0, 10, 8))
	doc.select_ellipse(Rect2i(1, 1, 6, 6), TetraImageDocument.SELECT_SUBTRACT, false)
	check_near(mask_sum(doc), 80.0 - 32.0, 0.0, "ellipse subtracted from everything")


func test_by_color() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 8, Color(0, 0, 1, 1))
	var layer := doc.get_active_layer()
	var image := doc.layer_get_image(layer)
	image.fill_rect(Rect2i(0, 0, 3, 8), Color(1, 0, 0, 1))
	image.fill_rect(Rect2i(7, 2, 2, 2), Color(1, 0, 0, 1))
	# One column of a slightly darker red: 0.05 away in sRGB-encoded terms.
	image.fill_rect(Rect2i(3, 0, 1, 8), Color(0.95, 0, 0, 1).srgb_to_linear())
	doc.layer_set_image(layer, image)

	doc.select_fuzzy(Vector2i(1, 1), 0.1)
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 4, 8), "fuzzy select takes the connected region within the threshold")
	check_near(mask_sum(doc), 32.0, 0.0, "fuzzy select count")
	doc.select_fuzzy(Vector2i(1, 1), 0.02)
	check_near(mask_sum(doc), 24.0, 0.0, "a lower threshold excludes the darker column")
	doc.select_by_color(Vector2i(1, 1), 0.02)
	check_near(mask_sum(doc), 28.0, 0.0, "select by colour also takes the island")
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 9, 8), "select by colour bounds")
	doc.select_by_color(Vector2i(8, 3), 0.1, false, false, TetraImageDocument.SELECT_SUBTRACT)
	check(doc.is_selection_empty(), "subtracting the same colour empties the selection")
	doc.select_fuzzy(Vector2i(5, 5), 0.0)
	check_near(mask_sum(doc), 80.0 - 32.0 - 4.0, 0.0, "threshold zero selects the exact colour")

	# On a transparent layer the seed matches by alpha; sample_merged looks at the composite.
	var top := doc.add_layer("top")
	doc.select_fuzzy(Vector2i(1, 1), 0.1)
	check_near(mask_sum(doc), 80.0, 0.0, "a transparent seed selects the transparent area")
	doc.select_fuzzy(Vector2i(1, 1), 0.1, true)
	check_near(mask_sum(doc), 32.0, 0.0, "sample merged sees the layers below")
	check_eq(doc.get_active_layer(), top, "selecting does not change the active layer")


func test_feather_grow_shrink() -> void:
	var doc := TetraImageDocument.new()
	doc.create(20, 20)
	doc.select_rect(Rect2i(8, 8, 4, 4))
	# Feather 3.5 is a Gaussian of sigma 1: kernel 0.39905, 0.24203, 0.054005, 0.004433.
	doc.feather_selection(3.5)
	check_near(mask_sum(doc), 16.0, 1e-3, "feather keeps the selected amount")
	check_near(mask_at(doc, 9, 9), 0.937115 * 0.937115, 1e-4, "feathered centre")
	check_near(mask_at(doc, 7, 9), 0.300468 * 0.937115, 1e-4, "feathered edge")
	check_near(mask_at(doc, 7, 9), mask_at(doc, 12, 10), 1e-6, "feather is symmetric")

	doc.select_rect(Rect2i(8, 8, 4, 4))
	doc.grow_selection(1)
	check_near(mask_sum(doc), 32.0, 0.0, "grow by one adds the four sides")
	check_near(mask_at(doc, 7, 7), 0.0, 0.0, "grow by one leaves the corners")
	check_near(mask_at(doc, 7, 8), 1.0, 0.0, "grow by one side")
	doc.shrink_selection(1)
	check_near(mask_sum(doc), 16.0, 0.0, "shrink undoes the grow")
	check_eq(doc.selection_bounds(), Rect2i(8, 8, 4, 4), "bounds after grow and shrink")
	doc.shrink_selection(1)
	check_eq(doc.selection_bounds(), Rect2i(9, 9, 2, 2), "shrink by one")
	doc.select_rect(Rect2i(8, 8, 4, 4))
	doc.grow_selection(2)
	check_near(mask_sum(doc), 52.0, 0.0, "grow by two uses a disc")
	check_eq(doc.selection_bounds(), Rect2i(6, 6, 8, 8), "grow by two bounds")

	# The outside of the canvas counts as unselected when shrinking.
	doc.select_all()
	doc.shrink_selection(2)
	check_eq(doc.selection_bounds(), Rect2i(2, 2, 16, 16), "shrinking everything retreats from the border")


func test_undo() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 8)
	doc.select_rect(Rect2i(2, 1, 4, 3))
	doc.select_ellipse(Rect2i(0, 0, 10, 8), TetraImageDocument.SELECT_ADD)
	var both := doc.get_selection_mask().get_data()
	check_eq(doc.get_undo_names(), PackedStringArray(["Rectangle Select", "Ellipse Select"]), "selection steps are named")
	doc.undo()
	check_eq(doc.selection_bounds(), Rect2i(2, 1, 4, 3), "undo restores the previous selection")
	check_near(mask_sum(doc), 12.0, 0.0, "undo restores the previous mask")
	doc.undo()
	check(doc.is_selection_empty(), "undo back to no selection")
	doc.redo()
	doc.redo()
	check_eq(doc.get_selection_mask().get_data(), both, "redo restores the mask exactly")
	# A rectangle selection on an empty mask stores just that rectangle (4 bytes per pixel).
	doc.select_none()
	doc.clear_undo_history()
	doc.select_rect(Rect2i(2, 1, 4, 3))
	check_eq(doc.get_undo_memory_usage(), 4 * 3 * 4, "selection undo stores only the changed area")
