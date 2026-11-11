extends "res://tetra_test.gd"
## Fills, gradients, colour picking and transforms of TetraImageDocument.

const WHITE := Color(1, 1, 1, 1)
const RED := Color(1, 0, 0, 1)
const BLUE := Color(0, 0, 1, 1)
const GREEN := Color(0, 1, 0, 1)
const CLEAR := Color(0, 0, 0, 0)


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 1e-5) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func px(p_doc: TetraImageDocument, p_layer: int, p_x: int, p_y: int) -> Color:
	return p_doc.layer_get_image(p_layer).get_pixel(p_x, p_y)


## A document whose single layer holds a distinct colour per pixel.
func numbered(p_width: int, p_height: int) -> TetraImageDocument:
	var doc := TetraImageDocument.new()
	doc.create(p_width, p_height)
	var image := Image.create_empty(p_width, p_height, false, Image.FORMAT_RGBAF)
	for y in p_height:
		for x in p_width:
			image.set_pixel(x, y, tag(x, y))
	doc.layer_set_image(doc.get_active_layer(), image)
	doc.clear_undo_history()
	return doc


func tag(p_x: int, p_y: int) -> Color:
	return Color(p_x / 10.0, p_y / 10.0, 0.5, 1.0)


func run() -> void:
	test_fill_and_clear()
	test_bucket_fill()
	test_gradients()
	test_pick_color()
	test_layer_transforms()
	test_image_transforms()
	test_scale()
	test_canvas_size()
	save_gradient_sheet()


func test_fill_and_clear() -> void:
	var doc := TetraImageDocument.new()
	doc.create(6, 6, WHITE)
	var layer := doc.get_active_layer()
	doc.get_and_clear_dirty_rect()
	doc.select_rect(Rect2i(1, 1, 2, 2))
	check_eq(doc.fill(layer, RED), OK, "fill")
	check_color(px(doc, layer, 1, 1), RED, "fill inside the selection")
	check_color(px(doc, layer, 2, 2), RED, "fill inside the selection (corner)")
	check_color(px(doc, layer, 0, 0), WHITE, "fill leaves the outside")
	check_color(px(doc, layer, 3, 3), WHITE, "fill leaves the outside (after)")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(1, 1, 2, 2), "fill dirties only the selection")
	check_eq(doc.get_undo_names()[-1], "Fill", "fill undo name")
	doc.undo()
	check_color(px(doc, layer, 1, 1), WHITE, "undo fill")
	doc.redo()
	check_color(px(doc, layer, 1, 1), RED, "redo fill")

	# A half-selected pixel receives half of the fill.
	var half := Image.create_empty(6, 6, false, Image.FORMAT_RF)
	half.fill(Color(0.5, 0, 0))
	doc.select_mask(half)
	doc.fill(layer, BLUE)
	check_color(px(doc, layer, 0, 0), Color(0.5, 0.5, 1.0, 1.0), "half selection blends white and blue")
	check_color(px(doc, layer, 1, 1), Color(0.5, 0.0, 0.5, 1.0), "half selection blends red and blue")

	# Clear makes the selection transparent; with no selection, everything.
	doc.select_rect(Rect2i(0, 0, 3, 6))
	doc.clear(layer)
	check_near(px(doc, layer, 2, 5).a, 0.0, 0.0, "clear inside the selection")
	check_near(px(doc, layer, 3, 5).a, 1.0, 0.0, "clear leaves the outside")

	# Lock alpha: colour changes only where there is something, alpha never changes.
	doc.select_none()
	doc.layer_set_lock_alpha(layer, true)
	doc.fill(layer, GREEN)
	check_color(px(doc, layer, 4, 4), GREEN, "locked alpha: opaque pixels take the colour")
	check_near(px(doc, layer, 1, 4).a, 0.0, 0.0, "locked alpha: transparent pixels stay transparent")
	doc.clear(layer)
	check_color(px(doc, layer, 4, 4), GREEN, "locked alpha: clear does nothing")
	check_eq(doc.fill(999, RED), ERR_DOES_NOT_EXIST, "fill on a missing layer fails")


func test_bucket_fill() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 8, BLUE)
	var layer := doc.get_active_layer()
	var image := doc.layer_get_image(layer)
	image.fill_rect(Rect2i(0, 0, 3, 8), RED)
	image.fill_rect(Rect2i(7, 2, 2, 2), RED)
	doc.layer_set_image(layer, image)
	doc.get_and_clear_dirty_rect()

	doc.bucket_fill(layer, Vector2i(1, 1), GREEN, 0.1)
	check_color(px(doc, layer, 0, 0), GREEN, "contiguous fill takes the region")
	check_color(px(doc, layer, 2, 7), GREEN, "contiguous fill takes the whole region")
	check_color(px(doc, layer, 7, 2), RED, "contiguous fill leaves the island")
	check_color(px(doc, layer, 3, 0), BLUE, "contiguous fill stops at the edge")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(0, 0, 3, 8), "bucket fill dirties the filled region")
	doc.undo()
	doc.bucket_fill(layer, Vector2i(1, 1), GREEN, 0.1, false)
	check_color(px(doc, layer, 8, 3), GREEN, "non-contiguous fill takes the island")
	check_color(px(doc, layer, 5, 5), BLUE, "non-contiguous fill leaves other colours")
	doc.undo()

	# The selection limits the fill; a translucent colour is painted over.
	doc.select_rect(Rect2i(0, 0, 2, 8))
	doc.bucket_fill(layer, Vector2i(1, 1), Color(0, 1, 0, 0.5), 0.1)
	check_color(px(doc, layer, 1, 4), Color(0.5, 0.5, 0, 1), "translucent fill inside the selection")
	check_color(px(doc, layer, 2, 4), RED, "fill limited by the selection")

	# Sample merged: the region comes from the composite, the paint goes to the layer.
	doc.select_none()
	var top := doc.add_layer("top")
	doc.bucket_fill(top, Vector2i(8, 3), WHITE, 0.1, true, true)
	check_color(px(doc, top, 7, 2), WHITE, "sample merged fills the island on the empty layer")
	check_near(px(doc, top, 5, 5).a, 0.0, 0.0, "sample merged leaves the rest of the layer empty")


func test_gradients() -> void:
	var doc := TetraImageDocument.new()
	doc.create(10, 10, WHITE)
	var layer := doc.get_active_layer()
	var black := Color(0, 0, 0, 1)
	var center := Vector2(5, 5)
	var right := Vector2(10, 5)

	# The two colours are mixed in sRGB-encoded space: black to white at t is decode(t).
	var grey := func(p_t: float) -> Color: return Color(p_t, p_t, p_t, 1.0).srgb_to_linear()

	doc.gradient_fill(layer, Vector2(0, 0), Vector2(10, 0), black, WHITE)
	check_color(px(doc, layer, 0, 0), grey.call(0.05), "linear start")
	check_color(px(doc, layer, 4, 7), grey.call(0.45), "linear middle")
	check_color(px(doc, layer, 9, 9), grey.call(0.95), "linear end")

	doc.gradient_fill(layer, center, right, black, WHITE, TetraImageDocument.GRADIENT_BILINEAR)
	check_color(px(doc, layer, 0, 3), grey.call(0.9), "bilinear left")
	check_color(px(doc, layer, 7, 3), grey.call(0.5), "bilinear right")

	doc.gradient_fill(layer, center, right, black, WHITE, TetraImageDocument.GRADIENT_RADIAL)
	check_color(px(doc, layer, 5, 5), grey.call(0.141421), "radial centre")
	check_color(px(doc, layer, 8, 5), grey.call(0.707107), "radial outer")
	check_color(px(doc, layer, 0, 0), grey.call(1.0), "radial beyond the end is clamped")

	doc.gradient_fill(layer, center, right, black, WHITE, TetraImageDocument.GRADIENT_SQUARE)
	check_color(px(doc, layer, 8, 6), grey.call(0.7), "square")
	check_color(px(doc, layer, 6, 1), grey.call(0.7), "square, other axis")

	# Conical: the angle to the axis over pi. (3.5, 0.5) -> 0.045167, (0.5, 3.5) -> 0.454833.
	doc.gradient_fill(layer, center, right, black, WHITE, TetraImageDocument.GRADIENT_CONICAL)
	check_color(px(doc, layer, 8, 5), grey.call(0.045167), "conical near the axis")
	check_color(px(doc, layer, 5, 8), grey.call(0.454833), "conical perpendicular")
	check_color(px(doc, layer, 5, 1), grey.call(0.454833), "conical is symmetric")
	check_color(px(doc, layer, 1, 5), grey.call(0.954833), "conical opposite")

	doc.gradient_fill(layer, center, right, black, WHITE, TetraImageDocument.GRADIENT_CONICAL_ASYMMETRIC)
	check_color(px(doc, layer, 5, 8), grey.call(0.772583), "asymmetric conical below")
	check_color(px(doc, layer, 5, 1), grey.call(0.227417), "asymmetric conical above")

	# Repeat modes: at x = 5 a 4 px gradient has run 1.375 times.
	var short_end := Vector2(4, 0)
	doc.gradient_fill(layer, Vector2(0, 0), short_end, black, WHITE)
	check_color(px(doc, layer, 5, 0), grey.call(1.0), "no repeat clamps")
	doc.gradient_fill(layer, Vector2(0, 0), short_end, black, WHITE, TetraImageDocument.GRADIENT_LINEAR, TetraImageDocument.REPEAT_SAWTOOTH)
	check_color(px(doc, layer, 5, 0), grey.call(0.375), "sawtooth repeat")
	doc.gradient_fill(layer, Vector2(0, 0), short_end, black, WHITE, TetraImageDocument.GRADIENT_LINEAR, TetraImageDocument.REPEAT_TRIANGULAR)
	check_color(px(doc, layer, 5, 0), grey.call(0.625), "triangular repeat")
	check_color(px(doc, layer, 1, 0), grey.call(0.375), "triangular first run")

	# A gradient to transparent is painted over what is there, inside the selection only.
	doc.fill(layer, WHITE)
	doc.select_rect(Rect2i(0, 0, 10, 5))
	doc.gradient_fill(layer, Vector2(0, 0), Vector2(10, 0), RED, Color(1, 0, 0, 0))
	check_color(px(doc, layer, 2, 2), Color(1, 0.25, 0.25, 1), "fade to transparent over white")
	check_color(px(doc, layer, 2, 7), WHITE, "gradient limited by the selection")
	check_eq(doc.get_undo_names()[-1], "Gradient", "gradient undo name")


func test_pick_color() -> void:
	var doc := TetraImageDocument.new()
	doc.create(6, 6, BLUE)
	var layer := doc.get_active_layer()
	var image := doc.layer_get_image(layer)
	image.fill_rect(Rect2i(0, 0, 2, 6), RED)
	doc.layer_set_image(layer, image)
	check_color(doc.pick_color(Vector2i(1, 3)), RED, "pick a pixel")
	check_color(doc.pick_color(Vector2i(2, 3)), BLUE, "pick the next pixel")
	# 3x3 around (2, 3): one red column, two blue.
	check_color(doc.pick_color(Vector2i(2, 3), false, 1), Color(1.0 / 3.0, 0, 2.0 / 3.0, 1), "pick an average")
	check_color(doc.pick_color(Vector2i(0, 0), false, 1), Color(1, 0, 0, 1), "the average is clipped at the border")

	var top := doc.add_layer("top")
	check_near(doc.pick_color(Vector2i(1, 3)).a, 0.0, 0.0, "the active layer is empty")
	check_color(doc.pick_color(Vector2i(1, 3), true), RED, "sample merged sees the composite")
	var half := Image.create_empty(6, 6, false, Image.FORMAT_RGBAF)
	half.fill(Color(0, 1, 0, 0.5))
	doc.layer_set_image(top, half)
	check_color(doc.pick_color(Vector2i(1, 3), true), Color(0.5, 0.5, 0, 1), "sample merged blends the layers")


func test_layer_transforms() -> void:
	var doc := numbered(3, 2)
	var layer := doc.get_active_layer()
	doc.flip_layer(layer)
	check_color(px(doc, layer, 0, 0), tag(2, 0), "horizontal flip")
	check_color(px(doc, layer, 2, 1), tag(0, 1), "horizontal flip, other corner")
	doc.undo()
	doc.flip_layer(layer, true)
	check_color(px(doc, layer, 0, 0), tag(0, 1), "vertical flip")
	doc.undo()
	doc.rotate_layer(layer, 180)
	check_color(px(doc, layer, 0, 0), tag(2, 1), "rotate layer 180")
	doc.undo()
	doc.translate_layer(layer, Vector2i(1, 0))
	check_color(px(doc, layer, 1, 0), tag(0, 0), "translate moves the content")
	check_color(px(doc, layer, 2, 1), tag(1, 1), "translate moves the content (2)")
	check_color(px(doc, layer, 0, 1), CLEAR, "translate leaves transparency behind")
	doc.undo()
	doc.translate_layer(layer, Vector2i(-1, 1))
	check_color(px(doc, layer, 0, 1), tag(1, 0), "translate up-left")
	check_color(px(doc, layer, 0, 0), CLEAR, "translate vacates the first row")
	doc.undo()
	check_eq(doc.layer_get_image(layer).get_data(), numbered(3, 2).layer_get_image(numbered(3, 2).get_active_layer()).get_data(), "undo restores the layer exactly")

	# Quarter turns of a layer rotate about the canvas centre.
	var square := numbered(4, 4)
	var square_layer := square.get_active_layer()
	square.rotate_layer(square_layer, 90)
	check_color(px(square, square_layer, 3, 0), tag(0, 0), "rotate 90 clockwise: top-left goes top-right")
	check_color(px(square, square_layer, 0, 0), tag(0, 3), "rotate 90 clockwise: bottom-left goes top-left")
	square.undo()
	square.rotate_layer(square_layer, 270)
	check_color(px(square, square_layer, 0, 3), tag(0, 0), "rotate 270: top-left goes bottom-left")
	check_eq(square.rotate_layer(square_layer, 45), ERR_INVALID_PARAMETER, "only quarter turns")


func test_image_transforms() -> void:
	var doc := numbered(3, 2)
	var layer := doc.get_active_layer()
	var original := doc.layer_get_image(layer).get_data()
	var sizes := [0]
	doc.size_changed.connect(func() -> void: sizes[0] += 1)
	doc.select_rect(Rect2i(0, 0, 1, 1))

	doc.rotate_image(90)
	check_eq(doc.get_size(), Vector2i(2, 3), "rotate 90 swaps the size")
	check_eq(sizes[0], 1, "size_changed emitted")
	check_color(px(doc, layer, 1, 0), tag(0, 0), "rotate 90: top-left goes top-right")
	check_color(px(doc, layer, 0, 0), tag(0, 1), "rotate 90: bottom-left goes top-left")
	check_color(px(doc, layer, 1, 2), tag(2, 0), "rotate 90: top-right goes bottom-right")
	check_eq(doc.selection_bounds(), Rect2i(1, 0, 1, 1), "the selection rotates with the image")
	doc.undo()
	check_eq(doc.get_size(), Vector2i(3, 2), "undo restores the size")
	check_eq(doc.layer_get_image(layer).get_data(), original, "undo restores the pixels exactly")
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 1, 1), "undo restores the selection")

	doc.rotate_image(270)
	check_color(px(doc, layer, 0, 0), tag(2, 0), "rotate 270: top-right goes top-left")
	check_eq(doc.selection_bounds(), Rect2i(0, 2, 1, 1), "rotate 270 selection")
	doc.undo()
	doc.rotate_image(180)
	check_eq(doc.get_size(), Vector2i(3, 2), "rotate 180 keeps the size")
	check_color(px(doc, layer, 0, 0), tag(2, 1), "rotate 180")
	doc.undo()

	var second := doc.add_layer("second")
	doc.fill(second, RED)
	doc.select_rect(Rect2i(0, 0, 1, 1))
	doc.clear(second)
	doc.flip_image()
	check_color(px(doc, layer, 0, 0), tag(2, 0), "flip image: first layer")
	check_near(px(doc, second, 2, 0).a, 0.0, 0.0, "flip image: second layer")
	check_eq(doc.selection_bounds(), Rect2i(2, 0, 1, 1), "flip image: selection")
	doc.flip_image(true)
	check_color(px(doc, layer, 0, 0), tag(2, 1), "flip image vertically")
	check_eq(doc.selection_bounds(), Rect2i(2, 1, 1, 1), "flip image vertically: selection")


func test_scale() -> void:
	var doc := numbered(3, 2)
	var layer := doc.get_active_layer()
	doc.scale_image(Vector2i(6, 4), TetraImageDocument.INTERPOLATION_NEAREST)
	check_eq(doc.get_size(), Vector2i(6, 4), "scaled size")
	check_color(px(doc, layer, 0, 0), tag(0, 0), "nearest (0, 0)")
	check_color(px(doc, layer, 1, 1), tag(0, 0), "nearest repeats pixels")
	check_color(px(doc, layer, 5, 3), tag(2, 1), "nearest last pixel")
	check_color(px(doc, layer, 2, 2), tag(1, 1), "nearest middle")
	doc.undo()
	check_eq(doc.get_size(), Vector2i(3, 2), "undo scale")

	# Shrinking by two with the linear filter weighs the four nearest source
	# pixels 0.5, 0.375 and 0.125 (the edge is repeated).
	var halves := TetraImageDocument.new()
	halves.create(4, 4, Color(0, 0, 0, 1))
	var halves_layer := halves.get_active_layer()
	halves.select_rect(Rect2i(2, 0, 2, 4))
	halves.fill(halves_layer, WHITE)
	halves.scale_image(Vector2i(2, 2), TetraImageDocument.INTERPOLATION_LINEAR)
	check_color(px(halves, halves_layer, 0, 0), Color(0.125, 0.125, 0.125, 1), "linear downscale left")
	check_color(px(halves, halves_layer, 1, 1), Color(0.875, 0.875, 0.875, 1), "linear downscale right")
	check_eq(halves.selection_bounds(), Rect2i(0, 0, 2, 2), "the selection is scaled too")
	check_near(halves.get_selection_mask().get_pixel(1, 0).r, 0.875, 1e-5, "scaled selection value")

	# Transparent pixels must not tint their neighbours.
	var edge := TetraImageDocument.new()
	edge.create(4, 4, CLEAR)
	var edge_layer := edge.get_active_layer()
	edge.select_rect(Rect2i(2, 0, 2, 4))
	edge.fill(edge_layer, RED)
	edge.scale_image(Vector2i(2, 2), TetraImageDocument.INTERPOLATION_LINEAR)
	check_color(px(edge, edge_layer, 0, 0), Color(1, 0, 0, 0.125), "resampling is alpha-weighted")

	# Flat colour stays flat with every filter, up and down.
	for interpolation: int in [TetraImageDocument.INTERPOLATION_NEAREST, TetraImageDocument.INTERPOLATION_LINEAR, TetraImageDocument.INTERPOLATION_CUBIC]:
		var flat := TetraImageDocument.new()
		flat.create(5, 7, Color(0.3, 0.6, 0.9, 1))
		flat.scale_image(Vector2i(13, 4), interpolation)
		check_color(px(flat, flat.get_active_layer(), 12, 3), Color(0.3, 0.6, 0.9, 1), "flat colour after scaling (%d)" % interpolation)
		check_color(px(flat, flat.get_active_layer(), 6, 0), Color(0.3, 0.6, 0.9, 1), "flat colour after scaling, middle (%d)" % interpolation)

	# Cubic keeps a linear ramp linear away from the borders (Catmull-Rom).
	var ramp := TetraImageDocument.new()
	ramp.create(8, 1)
	var ramp_image := Image.create_empty(8, 1, false, Image.FORMAT_RGBAF)
	for x in 8:
		ramp_image.set_pixel(x, 0, Color(x / 10.0, 0, 0, 1))
	ramp.layer_set_image(ramp.get_active_layer(), ramp_image)
	ramp.scale_image(Vector2i(16, 1), TetraImageDocument.INTERPOLATION_CUBIC)
	# Destination pixel 8 samples source position (8.5 / 2) - 0.5 = 3.75.
	check_near(px(ramp, ramp.get_active_layer(), 8, 0).r, 0.375, 1e-5, "cubic upscale of a ramp")


func test_canvas_size() -> void:
	var doc := numbered(3, 2)
	var layer := doc.get_active_layer()
	var original := doc.layer_get_image(layer).get_data()
	doc.resize_canvas(Vector2i(5, 4), Vector2i(1, 1))
	check_eq(doc.get_size(), Vector2i(5, 4), "canvas grows")
	check_color(px(doc, layer, 1, 1), tag(0, 0), "content moved by the offset")
	check_color(px(doc, layer, 3, 2), tag(2, 1), "content moved by the offset (2)")
	check_color(px(doc, layer, 0, 0), CLEAR, "new area is transparent")
	check_eq(doc.composite().get_size(), Vector2i(5, 4), "composite follows the canvas")
	doc.undo()
	check_eq(doc.layer_get_image(layer).get_data(), original, "undo canvas size")

	doc.crop(Rect2i(1, 0, 2, 2))
	check_eq(doc.get_size(), Vector2i(2, 2), "crop size")
	check_color(px(doc, layer, 0, 0), tag(1, 0), "crop content")
	check_color(px(doc, layer, 1, 1), tag(2, 1), "crop content (2)")
	doc.undo()

	doc.select_rect(Rect2i(1, 1, 2, 1))
	check_eq(doc.crop_to_selection(), OK, "crop to selection")
	check_eq(doc.get_size(), Vector2i(2, 1), "crop to selection size")
	check_color(px(doc, layer, 0, 0), tag(1, 1), "crop to selection content")
	check_eq(doc.selection_bounds(), Rect2i(0, 0, 2, 1), "the selection moves with the crop")
	doc.undo()
	check_eq(doc.get_size(), Vector2i(3, 2), "undo crop")
	check_eq(doc.layer_get_image(layer).get_data(), original, "undo crop restores the pixels")
	check_eq(doc.selection_bounds(), Rect2i(1, 1, 2, 1), "undo crop restores the selection")


func save_gradient_sheet() -> void:
	var cell := 96
	var types := [TetraImageDocument.GRADIENT_LINEAR, TetraImageDocument.GRADIENT_BILINEAR, TetraImageDocument.GRADIENT_RADIAL,
			TetraImageDocument.GRADIENT_SQUARE, TetraImageDocument.GRADIENT_CONICAL, TetraImageDocument.GRADIENT_CONICAL_ASYMMETRIC,
			TetraImageDocument.GRADIENT_SPIRAL_CLOCKWISE, TetraImageDocument.GRADIENT_SPIRAL_ANTICLOCKWISE]
	var sheet := Image.create_empty(cell * types.size(), cell * 2, false, Image.FORMAT_RGBA8)
	for i in types.size():
		for row in 2:
			var doc := TetraImageDocument.new()
			doc.create(cell, cell, WHITE)
			var repeat := TetraImageDocument.REPEAT_NONE if row == 0 else TetraImageDocument.REPEAT_TRIANGULAR
			var end := Vector2(cell * 0.9, cell * 0.5) if row == 0 else Vector2(cell * 0.7, cell * 0.6)
			doc.gradient_fill(doc.get_active_layer(), Vector2(cell * 0.5, cell * 0.5), end,
					Color(0.9, 0.1, 0.05, 1).srgb_to_linear(), Color(0.05, 0.2, 0.9, 1).srgb_to_linear(), types[i], repeat)
			sheet.blit_rect(doc.composite(), Rect2i(0, 0, cell, cell), Vector2i(i * cell, row * cell))
	var path := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens/paint-gradients.png")
	check_eq(sheet.save_png(path), OK, "save the gradient sheet")
