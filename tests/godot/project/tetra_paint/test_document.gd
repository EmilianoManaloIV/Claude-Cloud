extends "res://tetra_test.gd"
## Layers, compositing, layer modes and undo of TetraImageDocument.

const B := Color(0.2, 0.5, 0.8, 1.0)
const L := Color(0.6, 0.4, 0.1, 1.0)

var _changed_rects: Array[Rect2i] = []


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 2e-4) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon * maxf(1.0, absf(p_expected.r)) \
			and absf(p_actual.g - p_expected.g) <= p_epsilon * maxf(1.0, absf(p_expected.g)) \
			and absf(p_actual.b - p_expected.b) <= p_epsilon * maxf(1.0, absf(p_expected.b)) \
			and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func pixel(p_doc: TetraImageDocument, p_x: int, p_y: int) -> Color:
	return p_doc.composite_float().get_pixel(p_x, p_y)


func run() -> void:
	test_structure()
	test_layer_images()
	test_modes()
	test_dissolve()
	test_compositing_stack()
	test_undo()
	test_undo_budget()
	save_mode_sheet()


func test_structure() -> void:
	var doc := TetraImageDocument.new()
	doc.changed.connect(func(p_rect: Rect2i) -> void: _changed_rects.append(p_rect))
	check_eq(doc.create(8, 6, Color(1, 1, 1, 1)), OK, "create")
	check_eq(doc.get_size(), Vector2i(8, 6), "size")
	check_eq(doc.get_layer_count(), 1, "one layer after create")
	check_eq(_changed_rects.back(), Rect2i(0, 0, 8, 6), "create reports the whole canvas")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(0, 0, 8, 6), "dirty rect after create")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(), "dirty rect is cleared")

	var background := doc.get_layer_ids()[0]
	check_eq(doc.get_active_layer(), background, "background is active")
	var a := doc.add_layer("A")
	var b := doc.add_layer("B")
	var c := doc.add_layer("C", 0)
	check_eq(doc.get_layer_ids(), PackedInt32Array([c, background, a, b]), "add order and index")
	check_eq(doc.get_active_layer(), c, "new layer becomes active")
	check_eq(doc.layer_get_name(a), "A", "layer name")

	doc.move_layer(c, 3)
	check_eq(doc.get_layer_ids(), PackedInt32Array([background, a, b, c]), "move to top")
	doc.move_layer(c, 1)
	check_eq(doc.get_layer_ids(), PackedInt32Array([background, c, a, b]), "move down")
	check_eq(doc.get_layer_index(a), 2, "layer index")

	var dup := doc.duplicate_layer(a)
	check_eq(doc.get_layer_ids(), PackedInt32Array([background, c, a, dup, b]), "duplicate sits above its source")
	check_eq(doc.layer_get_name(dup), "A copy", "duplicate name")
	check_eq(doc.remove_layer(dup), OK, "remove")
	check_eq(doc.get_layer_count(), 4, "count after remove")
	check_eq(doc.get_active_layer(), a, "active moves to the layer below the removed one")

	doc.layer_set_opacity(a, 0.25)
	doc.layer_set_mode(a, TetraImageDocument.LAYER_MODE_MULTIPLY)
	doc.layer_set_visible(a, false)
	doc.layer_set_lock_alpha(a, true)
	doc.layer_set_name(a, "Renamed")
	check_near(doc.layer_get_opacity(a), 0.25, 1e-6, "opacity")
	check_eq(doc.layer_get_mode(a), TetraImageDocument.LAYER_MODE_MULTIPLY, "mode")
	check_eq(doc.layer_is_visible(a), false, "visible")
	check_eq(doc.layer_get_lock_alpha(a), true, "lock alpha")
	check_eq(doc.layer_get_name(a), "Renamed", "renamed")

	var names := TetraImageDocument.get_layer_mode_names()
	check_eq(names.size(), TetraImageDocument.LAYER_MODE_MAX, "one name per mode")
	check_eq(names[TetraImageDocument.LAYER_MODE_NORMAL], "Normal", "first mode")
	check_eq(names[TetraImageDocument.LAYER_MODE_GRAIN_MERGE], "Grain merge", "mode name by enum value")
	check_eq(names[TetraImageDocument.LAYER_MODE_LUMINANCE], "Luminance", "last mode")


func test_layer_images() -> void:
	var doc := TetraImageDocument.new()
	doc.create(4, 4, Color(0, 0, 0, 0))
	var layer := doc.get_active_layer()

	# An 8-bit image is sRGB: 128 must come back as linear 0.2158605.
	var srgb := Image.create_empty(4, 4, false, Image.FORMAT_RGBA8)
	srgb.fill(Color8(128, 255, 0, 255))
	check_eq(doc.layer_set_image(layer, srgb), OK, "set 8-bit image")
	var stored := doc.layer_get_image(layer)
	check_eq(stored.get_format(), Image.FORMAT_RGBAF, "layer image is float")
	check_color(stored.get_pixel(1, 1), Color(0.2158605, 1.0, 0.0, 1.0), "8-bit input is linearised", 1e-5)
	check_eq(doc.composite().get_pixel(1, 1), Color8(128, 255, 0, 255), "display composite returns the sRGB value")

	# A float image is taken as linear, and a smaller image is placed at the offset.
	var linear := Image.create_empty(2, 2, false, Image.FORMAT_RGBAF)
	linear.fill(Color(0.5, 2.0, 0.25, 0.5))
	doc.layer_set_image(layer, linear, Vector2i(1, 2))
	stored = doc.layer_get_image(layer)
	check_color(stored.get_pixel(1, 2), Color(0.5, 2.0, 0.25, 0.5), "float input kept as it is", 1e-6)
	check_color(stored.get_pixel(2, 3), Color(0.5, 2.0, 0.25, 0.5), "placed image covers 2x2")
	check_color(stored.get_pixel(0, 0), Color(0, 0, 0, 0), "outside the placed image is transparent")
	check_color(stored.get_pixel(3, 3), Color(0, 0, 0, 0), "right of the placed image is transparent")

	# Compositing a sub-rectangle returns just that area.
	var part := doc.composite_float(Rect2i(1, 2, 2, 1))
	check_eq(part.get_size(), Vector2i(2, 1), "sub-rectangle size")
	check_color(part.get_pixel(0, 0), Color(0.5, 2.0, 0.25, 0.5), "sub-rectangle content")
	check_eq(doc.composite(Rect2i(3, 3, 10, 10)).get_size(), Vector2i(1, 1), "rectangle is clipped to the canvas")
	check_eq(doc.composite().get_pixel(1, 2), Color8(188, 255, 137, 128), "8-bit composite clamps and encodes")


func test_modes() -> void:
	# Expected values computed independently (Python) from GIMP's formulas:
	# backdrop B over layer L, both opaque, blended in the mode's blend space.
	var opaque := {
		"LIGHTEN_ONLY": Color(0.600000, 0.500000, 0.800000),
		"SCREEN": Color(0.779008, 0.810147, 0.866888),
		"DODGE": Color(7.558338, 6.166494, 2.136812),
		"ADDITION": Color(0.800000, 0.900000, 0.900000),
		"DARKEN_ONLY": Color(0.200000, 0.400000, 0.100000),
		"MULTIPLY": Color(0.120000, 0.200000, 0.080000),
		"BURN": Color(0.102780, 0.321064, 0.494550),
		"LINEAR_BURN": Color(0.064764, 0.133249, 0.053115),
		"OVERLAY": Color(0.559159, 0.643174, 0.744730),
		"SOFT_LIGHT": Color(0.358751, 0.603233, 0.749809),
		"HARD_LIGHT": Color(0.589486, 0.643174, 0.358387),
		"VIVID_LIGHT": Color(1.000000, 1.000000, 0.721571),
		"PIN_LIGHT": Color(0.313291, 0.500000, 0.445685),
		"LINEAR_LIGHT": Color(1.191759, 1.156096, 0.324075),
		"HARD_MIX": Color(1.000000, 1.000000, 1.000000),
		"DIFFERENCE": Color(0.079950, 0.006001, 0.270794),
		"EXCLUSION": Color(0.222667, 0.148996, 0.345514),
		"SUBTRACT": Color(-0.400000, 0.100000, 0.700000),
		"GRAIN_EXTRACT": Color(0.029138, 0.284835, 1.134954),
		"GRAIN_MERGE": Color(0.574206, 0.788485, 0.531165),
		"DIVIDE": Color(0.333333, 1.250000, 8.000000),
		"HSV_HUE": Color(0.800000, 0.573245, 0.200000),
		"HSV_SATURATION": Color(0.130584, 0.447655, 0.800000),
		"HSL_COLOR": Color(0.702345, 0.539702, 0.248168),
		"HSV_VALUE": Color(0.152184, 0.376506, 0.600000),
		"LCH_HUE": Color(0.582892, 0.432335, 0.194806),
		"LCH_CHROMA": Color(0.067877, 0.523231, 1.010223),
		"LCH_COLOR": Color(0.632612, 0.424051, 0.110273),
		"LCH_LIGHTNESS": Color(0.183975, 0.473063, 0.762971),
		"LUMINANCE": Color(0.188871, 0.472177, 0.755482),
		"LUMA_LIGHTEN_ONLY": Color(0.200000, 0.500000, 0.800000),
		"LUMA_DARKEN_ONLY": Color(0.600000, 0.400000, 0.100000),
	}
	# Backdrop alpha 0.5, layer alpha 0.5, layer opacity 0.8.
	var semi := {
		"LIGHTEN_ONLY": Color(0.360000, 0.500000, 0.800000, 0.5),
		"SCREEN": Color(0.431603, 0.624059, 0.826755, 0.5),
		"DODGE": Color(3.143335, 2.766598, 1.334725, 0.5),
		"ADDITION": Color(0.440000, 0.660000, 0.840000, 0.5),
		"DARKEN_ONLY": Color(0.200000, 0.460000, 0.520000, 0.5),
		"MULTIPLY": Color(0.168000, 0.380000, 0.512000, 0.5),
		"BURN": Color(0.161112, 0.428425, 0.677820, 0.5),
		"LINEAR_BURN": Color(0.145905, 0.353299, 0.501246, 0.5),
		"OVERLAY": Color(0.343664, 0.557269, 0.777892, 0.5),
		"SOFT_LIGHT": Color(0.263501, 0.541293, 0.779924, 0.5),
		"HARD_LIGHT": Color(0.355794, 0.557269, 0.623355, 0.5),
		"VIVID_LIGHT": Color(0.520000, 0.700000, 0.768628, 0.5),
		"PIN_LIGHT": Color(0.245317, 0.500000, 0.658274, 0.5),
		"LINEAR_LIGHT": Color(0.596704, 0.762438, 0.609630, 0.5),
		"HARD_MIX": Color(0.520000, 0.700000, 0.880000, 0.5),
		"DIFFERENCE": Color(0.151980, 0.302400, 0.588317, 0.5),
		"EXCLUSION": Color(0.209067, 0.359598, 0.618205, 0.5),
		"SUBTRACT": Color(-0.040000, 0.340000, 0.760000, 0.5),
		"GRAIN_EXTRACT": Color(0.131655, 0.413934, 0.933982, 0.5),
		"GRAIN_MERGE": Color(0.349682, 0.615394, 0.692466, 0.5),
		"DIVIDE": Color(0.253333, 0.800000, 3.680000, 0.5),
		"HSV_HUE": Color(0.440000, 0.529298, 0.560000, 0.5),
		"HSV_SATURATION": Color(0.172234, 0.479062, 0.800000, 0.5),
		"HSL_COLOR": Color(0.400938, 0.515881, 0.579267, 0.5),
		"HSV_VALUE": Color(0.180874, 0.450603, 0.720000, 0.5),
		"LCH_HUE": Color(0.353157, 0.472934, 0.557923, 0.5),
		"LCH_CHROMA": Color(0.147151, 0.509292, 0.884089, 0.5),
		"LCH_COLOR": Color(0.373045, 0.469621, 0.524109, 0.5),
		"LCH_LIGHTNESS": Color(0.193590, 0.489225, 0.785188, 0.5),
		"LUMINANCE": Color(0.195548, 0.488871, 0.782193, 0.5),
		"LUMA_LIGHTEN_ONLY": Color(0.200000, 0.500000, 0.800000, 0.5),
		"LUMA_DARKEN_ONLY": Color(0.360000, 0.460000, 0.520000, 0.5),
	}
	var backdrop_semi := Color(B.r, B.g, B.b, 0.5)
	var layer_semi := Color(L.r, L.g, L.b, 0.5)
	var backdrop_clear := Color(B.r, B.g, B.b, 0.0)
	var layer_clear := Color(L.r, L.g, L.b, 0.0)
	for mode_name: String in opaque:
		var mode: int = ClassDB.class_get_integer_constant("TetraImageDocument", "LAYER_MODE_" + mode_name)
		check_color(TetraImageDocument.blend_colors(mode, B, L), opaque[mode_name], mode_name + " opaque")
		check_color(TetraImageDocument.blend_colors(mode, backdrop_semi, layer_semi, 0.8), semi[mode_name], mode_name + " semi-transparent")
		# These modes clip to the backdrop: nothing appears where it is transparent,
		# and a transparent layer changes nothing.
		check_color(TetraImageDocument.blend_colors(mode, backdrop_clear, L), backdrop_clear, mode_name + " transparent backdrop")
		check_color(TetraImageDocument.blend_colors(mode, B, layer_clear), B, mode_name + " transparent layer")

	# Hand-computed anchors.
	# Screen of two linear 0.5 greys: encode 0.735357, 1 - 0.264643^2 = 0.929964, decode 0.848014.
	var grey := Color(0.5, 0.5, 0.5, 1.0)
	check_color(TetraImageDocument.blend_colors(TetraImageDocument.LAYER_MODE_SCREEN, grey, grey), Color(0.848014, 0.848014, 0.848014), "screen grey anchor", 1e-5)
	# Multiply is linear: 0.5 * 0.5.
	check_color(TetraImageDocument.blend_colors(TetraImageDocument.LAYER_MODE_MULTIPLY, grey, grey), Color(0.25, 0.25, 0.25), "multiply grey anchor", 1e-6)

	# Normal ("over" on straight alpha, union).
	var normal := TetraImageDocument.LAYER_MODE_NORMAL
	check_color(TetraImageDocument.blend_colors(normal, B, L), L, "normal opaque")
	# la = 0.4, alpha = 0.4 + 0.5 - 0.2 = 0.7, weight = 0.4 / 0.7.
	var w := 0.4 / 0.7
	check_color(TetraImageDocument.blend_colors(normal, backdrop_semi, layer_semi, 0.8),
			Color(L.r * w + B.r * (1 - w), L.g * w + B.g * (1 - w), L.b * w + B.b * (1 - w), 0.7), "normal semi-transparent")
	check_color(TetraImageDocument.blend_colors(normal, backdrop_clear, L), L, "normal on transparent backdrop")
	check_color(TetraImageDocument.blend_colors(normal, B, layer_clear), B, "normal transparent layer")

	# Erase removes backdrop alpha by the layer's coverage: 0.5 * (1 - 0.4).
	var erase := TetraImageDocument.LAYER_MODE_ERASE
	check_color(TetraImageDocument.blend_colors(erase, B, L), Color(B.r, B.g, B.b, 0.0), "erase opaque")
	check_color(TetraImageDocument.blend_colors(erase, backdrop_semi, layer_semi, 0.8), Color(B.r, B.g, B.b, 0.3), "erase semi-transparent")
	check_color(TetraImageDocument.blend_colors(erase, B, layer_clear), B, "erase transparent layer")

	# Behind paints under the backdrop: alpha 0.7, backdrop weight 0.5 / 0.7.
	var behind := TetraImageDocument.LAYER_MODE_BEHIND
	check_color(TetraImageDocument.blend_colors(behind, B, L), B, "behind opaque backdrop")
	var bw := 0.5 / 0.7
	check_color(TetraImageDocument.blend_colors(behind, backdrop_semi, layer_semi, 0.8),
			Color(B.r * bw + L.r * (1 - bw), B.g * bw + L.g * (1 - bw), B.b * bw + L.b * (1 - bw), 0.7), "behind semi-transparent")
	check_color(TetraImageDocument.blend_colors(behind, backdrop_clear, L), L, "behind transparent backdrop")

	# Merge adds coverage: in alpha limited to 1 - la = 0.6 -> stays 0.5, sum 0.9, ratio 0.4 / 0.9.
	var merge := TetraImageDocument.LAYER_MODE_MERGE
	var mw := 0.4 / 0.9
	check_color(TetraImageDocument.blend_colors(merge, backdrop_semi, layer_semi, 0.8),
			Color(B.r + (L.r - B.r) * mw, B.g + (L.g - B.g) * mw, B.b + (L.b - B.b) * mw, 0.9), "merge semi-transparent")
	check_color(TetraImageDocument.blend_colors(merge, B, L), L, "merge opaque")

	# Split subtracts coverage: 0.5 - 0.4.
	var split := TetraImageDocument.LAYER_MODE_SPLIT
	check_color(TetraImageDocument.blend_colors(split, backdrop_semi, layer_semi, 0.8), Color(B.r, B.g, B.b, 0.1), "split semi-transparent")
	check_color(TetraImageDocument.blend_colors(split, B, L), Color(B.r, B.g, B.b, 0.0), "split opaque")

	# Color erase of the backdrop's own colour removes it completely; erasing
	# white from mid grey keeps the grey as half-transparent black.
	var color_erase := TetraImageDocument.LAYER_MODE_COLOR_ERASE
	check_near(TetraImageDocument.blend_colors(color_erase, B, B).a, 0.0, 1e-6, "color erase of the same colour")
	check_color(TetraImageDocument.blend_colors(color_erase, grey, Color(1, 1, 1, 1)), Color(0, 0, 0, 0.5), "color erase white from grey", 1e-5)
	check_color(TetraImageDocument.blend_colors(color_erase, B, layer_clear), B, "color erase transparent layer")


func test_dissolve() -> void:
	# The pattern of GIMP's Dissolve for a layer of alpha 0.5, computed with
	# GLib's GRand (seed table from 314159265, one generator per row).
	var expected := ["1101001100100101", "0110011101101100", "1110000101010110"]
	var doc := TetraImageDocument.new()
	doc.create(16, 3, B)
	var top := doc.add_layer("dissolve")
	var image := Image.create_empty(16, 3, false, Image.FORMAT_RGBAF)
	image.fill(Color(L.r, L.g, L.b, 0.5))
	doc.layer_set_image(top, image)
	doc.layer_set_mode(top, TetraImageDocument.LAYER_MODE_DISSOLVE)
	var result := doc.composite_float()
	for y in 3:
		var row := ""
		for x in 16:
			var px := result.get_pixel(x, y)
			row += "1" if px.is_equal_approx(L) else "0"
			check(px.is_equal_approx(L) or px.is_equal_approx(B), "dissolve pixel is either layer or backdrop")
		check_eq(row, expected[y], "dissolve row %d" % y)
	# A sub-rectangle shows the same pattern: it is anchored to the canvas.
	var part := doc.composite_float(Rect2i(5, 1, 4, 1))
	var part_row := ""
	for x in 4:
		part_row += "1" if part.get_pixel(x, 0).is_equal_approx(L) else "0"
	check_eq(part_row, expected[1].substr(5, 4), "dissolve is anchored to canvas coordinates")


func test_compositing_stack() -> void:
	var doc := TetraImageDocument.new()
	doc.create(2, 2, B)
	var bottom := doc.get_layer_ids()[0]
	var top := doc.add_layer("top")
	var image := Image.create_empty(2, 2, false, Image.FORMAT_RGBAF)
	image.fill(L)
	doc.layer_set_image(top, image)

	doc.layer_set_mode(top, TetraImageDocument.LAYER_MODE_MULTIPLY)
	check_color(pixel(doc, 0, 0), Color(0.12, 0.2, 0.08, 1.0), "multiply layer in the stack", 1e-6)
	doc.layer_set_opacity(top, 0.5)
	check_color(pixel(doc, 1, 1), Color(0.16, 0.35, 0.44, 1.0), "multiply at half opacity", 1e-6)
	doc.layer_set_visible(top, false)
	check_color(pixel(doc, 0, 0), B, "hidden layer is skipped", 1e-6)

	# The bottom visible layer is shown as it is whatever its mode (GIMP's
	# "last node" rule); only its opacity applies.
	doc.layer_set_visible(top, true)
	doc.layer_set_visible(bottom, false)
	check_color(pixel(doc, 0, 0), Color(L.r, L.g, L.b, 0.5), "bottom visible layer ignores its mode", 1e-6)

	# Merge down bakes the result: same composite, one layer less.
	doc.layer_set_visible(bottom, true)
	doc.layer_set_opacity(bottom, 0.5)
	var before := doc.composite_float().get_data()
	check_eq(doc.merge_down(top), OK, "merge down")
	check_eq(doc.get_layer_count(), 1, "merge down removes the upper layer")
	check_near(doc.layer_get_opacity(bottom), 1.0, 1e-6, "merged layer is at full opacity")
	check_eq(doc.composite_float().get_data(), before, "merge down keeps the composite")

	# Flatten keeps the composite too.
	var extra := doc.add_layer("extra")
	var half := Image.create_empty(2, 2, false, Image.FORMAT_RGBAF)
	half.fill(Color(1, 0, 0, 0.25))
	doc.layer_set_image(extra, half)
	before = doc.composite_float().get_data()
	check_eq(doc.flatten(), OK, "flatten")
	check_eq(doc.get_layer_count(), 1, "flatten leaves one layer")
	check_eq(doc.composite_float().get_data(), before, "flatten keeps the composite")

	# A larger canvas goes through the threaded row loop; every pixel must match.
	var big := TetraImageDocument.new()
	big.create(300, 200, B)
	var over := big.add_layer("over")
	var fill := Image.create_empty(300, 200, false, Image.FORMAT_RGBAF)
	fill.fill(L)
	big.layer_set_image(over, fill)
	big.layer_set_mode(over, TetraImageDocument.LAYER_MODE_ADDITION)
	var composite := big.composite_float()
	var uniform := true
	for y in range(0, 200, 7):
		for x in range(0, 300, 11):
			uniform = uniform and composite.get_pixel(x, y).is_equal_approx(Color(0.8, 0.9, 0.9, 1.0))
	check(uniform, "threaded composite is uniform")
	check_eq(big.composite().get_pixel(299, 199), Color8(231, 243, 243, 255), "threaded 8-bit composite")


func test_undo() -> void:
	var doc := TetraImageDocument.new()
	doc.create(4, 4, B)
	var background := doc.get_layer_ids()[0]
	check(not doc.can_undo(), "fresh document has nothing to undo")

	var layer := doc.add_layer("paint")
	var image := Image.create_empty(4, 4, false, Image.FORMAT_RGBAF)
	image.fill(L)
	doc.layer_set_image(layer, image)
	doc.layer_set_opacity(layer, 0.5)
	doc.layer_set_mode(layer, TetraImageDocument.LAYER_MODE_SCREEN)
	check_eq(doc.get_undo_names(), PackedStringArray(["Add Layer", "Set Layer Pixels", "Layer Opacity", "Layer Mode"]), "undo names")
	var final_state := doc.composite_float().get_data()

	check(doc.undo(), "undo mode")
	check_eq(doc.layer_get_mode(layer), TetraImageDocument.LAYER_MODE_NORMAL, "mode restored")
	check(doc.undo(), "undo opacity")
	check_near(doc.layer_get_opacity(layer), 1.0, 1e-6, "opacity restored")
	check(doc.undo(), "undo pixels")
	check_color(doc.layer_get_image(layer).get_pixel(2, 2), Color(0, 0, 0, 0), "pixels restored exactly", 0.0)
	check(doc.undo(), "undo add layer")
	check_eq(doc.get_layer_ids(), PackedInt32Array([background]), "layer removed by undo")
	check(not doc.undo(), "nothing left to undo")
	check_eq(doc.get_redo_names().size(), 4, "four steps to redo")

	while doc.redo():
		pass
	check_eq(doc.get_layer_ids(), PackedInt32Array([background, layer]), "redo restores the layer with its id")
	check_eq(doc.composite_float().get_data(), final_state, "redo restores the final composite exactly")

	# A group is one step; a new edit clears the redo stack.
	doc.undo()
	doc.begin_undo_group("Tweak")
	doc.layer_set_opacity(layer, 0.1)
	doc.layer_set_name(layer, "tweaked")
	doc.layer_set_visible(layer, false)
	doc.end_undo_group()
	check(not doc.can_redo(), "new edit clears redo")
	check_eq(doc.get_undo_names()[-1], "Tweak", "group name")
	doc.undo()
	check_near(doc.layer_get_opacity(layer), 0.5, 1e-6, "group undone: opacity")
	check_eq(doc.layer_get_name(layer), "paint", "group undone: name")
	check_eq(doc.layer_is_visible(layer), true, "group undone: visibility")

	# Structure steps: remove, reorder, merge, flatten all come back exactly.
	var reference := doc.composite_float().get_data()
	var ids := doc.get_layer_ids()
	doc.remove_layer(background)
	doc.undo()
	check_eq(doc.get_layer_ids(), ids, "undo remove")
	doc.move_layer(layer, 0)
	doc.undo()
	check_eq(doc.get_layer_ids(), ids, "undo reorder")
	doc.merge_down(layer)
	doc.undo()
	check_eq(doc.get_layer_ids(), ids, "undo merge down")
	doc.flatten()
	doc.undo()
	check_eq(doc.get_layer_ids(), ids, "undo flatten")
	check_eq(doc.composite_float().get_data(), reference, "composite identical after structure undos")
	check_near(doc.layer_get_opacity(layer), 0.5, 1e-6, "properties survive structure undos")


func test_undo_budget() -> void:
	var doc := TetraImageDocument.new()
	doc.create(64, 64, B)
	var layer := doc.get_active_layer()
	# One full-layer pixel step is 64 * 64 * 16 = 65536 bytes.
	doc.set_undo_memory_limit(65536 * 3)
	var image := Image.create_empty(64, 64, false, Image.FORMAT_RGBAF)
	for i in 6:
		image.fill(Color(i / 10.0, 0, 0, 1))
		doc.layer_set_image(layer, image)
	check_eq(doc.get_undo_names().size(), 3, "the memory budget keeps the newest steps")
	check_eq(doc.get_undo_memory_usage(), 65536 * 3, "memory usage counts the stored regions")
	var undone := 0
	while doc.undo():
		undone += 1
	check_eq(undone, 3, "only the kept steps undo")
	check_color(doc.layer_get_image(layer).get_pixel(0, 0), Color(0.2, 0, 0, 1), "state after undoing the kept steps", 1e-6)

	# A small change stores only the changed region.
	doc.clear_undo_history()
	var patch := doc.layer_get_image(layer)
	patch.set_pixel(10, 20, Color(0, 1, 0, 1))
	patch.set_pixel(12, 21, Color(0, 1, 0, 1))
	doc.get_and_clear_dirty_rect()
	doc.layer_set_image(layer, patch)
	check_eq(doc.get_undo_memory_usage(), 3 * 2 * 16, "undo stores only the changed 3x2 region")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(10, 20, 3, 2), "dirty rect is the changed region")


func save_mode_sheet() -> void:
	# A sheet of all modes: a hue ramp layer over a photo-like backdrop, one cell per mode.
	var cell := 48
	var columns := 8
	var names := TetraImageDocument.get_layer_mode_names()
	var rows := ceili(names.size() / float(columns))
	var sheet := Image.create_empty(columns * cell, rows * cell, false, Image.FORMAT_RGBA8)
	var backdrop := Image.create_empty(cell, cell, false, Image.FORMAT_RGBAF)
	var top := Image.create_empty(cell, cell, false, Image.FORMAT_RGBAF)
	for y in cell:
		for x in cell:
			var t := x / float(cell - 1)
			backdrop.set_pixel(x, y, Color(t, t * 0.6 + 0.2, 1.0 - t, 1.0).srgb_to_linear())
			var disc := Vector2(x - cell / 2.0, y - cell / 2.0).length() < cell * 0.4
			top.set_pixel(x, y, Color.from_hsv(y / float(cell), 0.9, 0.9, 1.0 if disc else 0.0).srgb_to_linear())
	for mode in names.size():
		var doc := TetraImageDocument.new()
		doc.create(cell, cell, Color(0, 0, 0, 0))
		doc.layer_set_image(doc.get_active_layer(), backdrop)
		var layer := doc.add_layer("top")
		doc.layer_set_image(layer, top)
		doc.layer_set_mode(layer, mode)
		sheet.blit_rect(doc.composite(), Rect2i(0, 0, cell, cell), Vector2i((mode % columns) * cell, (mode / columns) * cell))
	var path := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens/paint-layer-modes.png")
	check_eq(sheet.save_png(path), OK, "save the layer-mode sheet")
