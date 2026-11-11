extends "res://tetra_test.gd"
## The GEGL filter bridge.


func flat(p_width: int, p_height: int, p_color: Color) -> Image:
	var image := Image.create_empty(p_width, p_height, false, Image.FORMAT_RGBAF)
	image.fill(p_color)
	return image


func max_difference(p_a: Image, p_b: Image, p_margin: int = 0) -> float:
	var worst := 0.0
	for y in range(p_margin, p_a.get_height() - p_margin):
		for x in range(p_margin, p_a.get_width() - p_margin):
			var a := p_a.get_pixel(x, y)
			var b := p_b.get_pixel(x, y)
			worst = maxf(worst, maxf(maxf(absf(a.r - b.r), absf(a.g - b.g)), maxf(absf(a.b - b.b), absf(a.a - b.a))))
	return worst


## A small picture with edges, colour and partial alpha.
func picture() -> Image:
	var image := Image.create_empty(24, 16, false, Image.FORMAT_RGBAF)
	for y in 16:
		for x in 24:
			var c := Color(x / 23.0, y / 15.0, 0.5 + 0.5 * sin(x * 0.9) * cos(y * 0.7), 1.0)
			if Vector2(x - 12, y - 8).length() < 5.0:
				c = Color(0.9, 0.2, 0.1, 1.0)
			image.set_pixel(x, y, c)
	return image


func run() -> void:
	if not TetraGegl.is_available():
		check_eq(TetraGegl.list_operations().size(), 0, "without GEGL the list is empty")
		check_eq(TetraGegl.get_version(), "", "without GEGL there is no version")
		check(TetraGegl.apply("gegl:gaussian-blur", {}, flat(4, 4, Color.WHITE)) == null, "without GEGL apply fails")
		check_eq(TetraEngineRegistry.is_engine_available("GEGL"), false, "the registry reports GEGL as unavailable")
		return
	test_listing()
	test_filters()
	test_parameters()
	test_document()
	await test_threads()
	save_picture()


func test_listing() -> void:
	check(TetraGegl.get_version().begins_with("0.4."), "version: %s" % TetraGegl.get_version())
	var operations := TetraGegl.list_operations()
	check(operations.size() > 100, "more than 100 operations: %d" % operations.size())
	var ids := {}
	var invalid := 0
	for operation: Dictionary in operations:
		var problems := TetraSchema.validate_operation(operation)
		if problems != "":
			invalid += 1
			printerr("%s: %s" % [operation["id"], problems])
		if not String(operation["id"]).begins_with("gegl:") or operation["source"] != "gegl":
			invalid += 1
		ids[operation["id"]] = operation
	check_eq(invalid, 0, "every operation follows the schema")
	check_eq(ids.size(), operations.size(), "ids are unique")
	for id: String in ["gegl:gaussian-blur", "gegl:unsharp-mask", "gegl:emboss", "gegl:cartoon", "gegl:noise-rgb", "gegl:checkerboard", "gegl:waves", "gegl:edge-sobel"]:
		check(ids.has(id), "%s is listed" % id)
	# Infrastructure, file, multi-input and object-driven operations stay out.
	for id: String in ["gegl:nop", "gegl:load", "gegl:png-save", "gegl:buffer-source", "gegl:over", "gegl:add", "gegl:crop", "gegl:display", "gegl:fill-path", "gegl:warp"]:
		check(not ids.has(id), "%s is not listed" % id)

	var blur: Dictionary = ids["gegl:gaussian-blur"]
	check_eq(blur["label"], "Gaussian Blur", "title")
	check_eq(blur["category"], "Filters/Blur", "category")
	var by_name := {}
	for param: Dictionary in blur["params"]:
		by_name[param["name"]] = param
	check_eq(by_name["std-dev-x"]["type"], "float", "double property")
	check_eq(by_name["std-dev-x"]["default"], 1.5, "default")
	check_eq(by_name["std-dev-x"]["min"], 0.0, "minimum")
	check_eq(by_name["std-dev-x"]["max"], 1500.0, "maximum")
	check_eq(by_name["std-dev-x"]["ui_max"], 100.0, "ui range")
	check_eq(by_name["std-dev-x"]["unit"], "px", "unit from ui-meta")
	check_eq(by_name["abyss-policy"]["type"], "enum", "enum property")
	check_eq(by_name["abyss-policy"]["default"], "clamp", "enum default")
	check_eq(by_name["abyss-policy"]["options"].size(), 4, "enum options")
	check_eq(by_name["clip-extent"]["type"], "bool", "boolean property")
	check_eq(ids["gegl:emboss"]["params"][3]["type"], "int", "int property")
	check_eq(ids["gegl:emboss"]["params"][1]["unit"], "°", "degree unit")
	check_eq(ids["gegl:checkerboard"]["category"], "Filters/Render", "render category")
	check_eq(ids["gegl:checkerboard"]["params"][4]["type"], "color", "colour property")
	check_eq(ids["gegl:checkerboard"]["params"][4]["default"], Color(0, 0, 0, 1), "colour default")
	check_eq(ids["gegl:exposure"]["category"], "Colors", "colour category")
	check_eq(ids["gegl:mantiuk06"]["category"], "Colors/Tone Mapping", "tone mapping category")
	var noise_types := {}
	for param: Dictionary in ids["gegl:noise-rgb"]["params"]:
		noise_types[param["name"]] = param["type"]
	check_eq(noise_types["seed"], "seed", "seed property")
	check_eq(TetraImageDocument.get_operations().size(), TetraImageDocument.get_native_operations().size() + operations.size(), "the document lists native and GEGL operations")
	var engine := TetraEngineRegistry.get_engine("GEGL")
	check_eq(engine["available"], true, "the registry lists GEGL")
	check_eq(engine["details"], "%d operations" % operations.size(), "the registry tells how many operations there are")


func test_filters() -> void:
	# Gaussian blur of one white pixel with sigma 1: centre 0.39905^2, neighbour 0.39905 * 0.24203.
	var impulse := flat(9, 9, Color(0, 0, 0, 1))
	impulse.set_pixel(4, 4, Color(1, 1, 1, 1))
	# The default "auto" filter is GEGL's recursive approximation at this size.
	var blurred := TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 1.0}, impulse)
	check_eq(blurred.get_format(), Image.FORMAT_RGBAF, "result format")
	check_eq(blurred.get_size(), Vector2i(9, 9), "result has the input size")
	check_near(blurred.get_pixel(4, 4).r, 0.159241, 0.004, "blurred centre")
	check_near(blurred.get_pixel(5, 4).r, 0.096582, 0.02, "blurred neighbour")
	check_near(blurred.get_pixel(3, 4).r, blurred.get_pixel(4, 5).r, 1e-3, "blur is symmetric")
	check(blurred.get_pixel(4, 4).r > blurred.get_pixel(5, 4).r and blurred.get_pixel(5, 4).r > blurred.get_pixel(6, 4).r, "blur falls off from the centre")
	var total := 0.0
	for y in 9:
		for x in 9:
			total += blurred.get_pixel(x, y).r
	check_near(total, 1.0, 1e-3, "blur preserves the total")
	# With the exact (FIR) kernel GEGL and the native blur are the same.
	var exact := TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 1.0, "filter": "fir"}, impulse)
	check_near(exact.get_pixel(4, 4).r, 0.159241, 1e-5, "FIR blurred centre")
	check_near(exact.get_pixel(5, 4).r, 0.096582, 1e-5, "FIR blurred neighbour")
	var native := TetraImageDocument.filter_image(impulse, "native:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 1.0})
	check(max_difference(exact, native) < 1e-5, "GEGL FIR and native blur agree: %f" % max_difference(exact, native))

	# Operations ported natively must match GEGL's own implementation.
	var image := picture()
	var pairs := {
		"invert-linear": ["native:invert", {"linear": true}, {}],
		"invert-gamma": ["native:invert", {}, {}],
		"emboss": ["native:emboss", {"azimuth": 70.0, "depth": 8}, {"azimuth": 70.0, "depth": 8}],
		"pixelize": ["native:pixelize", {"size-x": 4, "size-y": 4}, {"size-x": 4, "size-y": 4}],
	}
	for gegl_name: String in pairs:
		var native_result := TetraImageDocument.filter_image(image, pairs[gegl_name][0], pairs[gegl_name][1])
		var gegl_result := TetraGegl.apply("gegl:" + gegl_name, pairs[gegl_name][2], image)
		var difference := max_difference(native_result, gegl_result, 1)
		check(difference < 2e-4, "native %s matches gegl:%s: %f" % [pairs[gegl_name][0], gegl_name, difference])
	# GEGL's edge-sobel reads its padded source with destination coordinates,
	# which moves its result one pixel down and right; the native port does
	# not copy that shift, so the comparison undoes it.
	var native_sobel := TetraImageDocument.filter_image(image, "native:edge-sobel", {})
	var gegl_sobel := TetraGegl.apply("gegl:edge-sobel", {}, image)
	var sobel_difference := 0.0
	for y in range(1, 14):
		for x in range(1, 22):
			sobel_difference = maxf(sobel_difference, absf(native_sobel.get_pixel(x, y).g - gegl_sobel.get_pixel(x + 1, y + 1).g))
	check(sobel_difference < 2e-4, "native sobel matches gegl:edge-sobel: %f" % sobel_difference)

	# gegl:emboss lives in GEGL's GPL3+ set, so this also shows those are enabled;
	# a flat surface must come out as sin(45 deg).
	var embossed := TetraGegl.apply("gegl:emboss", {}, flat(6, 6, Color(0.3, 0.6, 0.9, 1)))
	check_near(embossed.get_pixel(3, 3).r, 0.707107, 1e-4, "GPL3+ operation gegl:emboss works")

	# 8-bit input is sRGB: inverting linear 0.5 must still give 0.5.
	var eight_bit := Image.create_empty(4, 4, false, Image.FORMAT_RGBA8)
	eight_bit.fill(Color8(188, 188, 188, 255))
	var inverted := TetraGegl.apply("gegl:invert-linear", {}, eight_bit)
	check_near(inverted.get_pixel(1, 1).r, 1.0 - 0.5028865, 1e-4, "8-bit input is linearised")
	check(TetraGegl.apply("gegl:no-such-operation", {}, impulse) == null, "unknown operation fails")
	check(TetraGegl.apply("gegl:load", {"path": "/etc/hostname"}, impulse) == null, "unlisted operations cannot be applied")


func test_parameters() -> void:
	# A generator ignores its input and fills the canvas; colour parameters are linear.
	var board := TetraGegl.apply("gegl:checkerboard", {"x": 4, "y": 4, "color1": Color(1, 0, 0, 1), "color2": Color(0, 0, 1, 0.5)}, flat(8, 8, Color(0, 0, 0, 0)))
	check(board.get_pixel(0, 0).is_equal_approx(Color(1, 0, 0, 1)), "generator with a colour parameter: %s" % board.get_pixel(0, 0))
	check(board.get_pixel(4, 0).is_equal_approx(Color(0, 0, 1, 0.5)), "second colour with alpha: %s" % board.get_pixel(4, 0))
	check(board.get_pixel(5, 5).is_equal_approx(Color(1, 0, 0, 1)), "the pattern repeats")

	# Enum by nick: the edge policy changes how a blur treats the border.
	var bright := flat(8, 8, Color(1, 1, 1, 1))
	var clamped := TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 2.0, "std-dev-y": 2.0, "abyss-policy": "clamp"}, bright)
	var open := TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 2.0, "std-dev-y": 2.0, "abyss-policy": "none"}, bright)
	check_near(clamped.get_pixel(0, 0).a, 1.0, 1e-4, "clamp policy keeps the border opaque")
	check(open.get_pixel(0, 0).a < 0.5, "none policy fades the border: %f" % open.get_pixel(0, 0).a)

	# Bool and seed parameters.
	var grey := flat(16, 16, Color(0.5, 0.5, 0.5, 1))
	var noise_a := TetraGegl.apply("gegl:noise-rgb", {"seed": 11, "independent": false}, grey)
	var noise_b := TetraGegl.apply("gegl:noise-rgb", {"seed": 11, "independent": false}, grey)
	var noise_c := TetraGegl.apply("gegl:noise-rgb", {"seed": 12, "independent": false}, grey)
	check_eq(noise_a.get_data(), noise_b.get_data(), "a seed repeats its noise")
	check(noise_a.get_data() != noise_c.get_data(), "another seed changes the noise")
	var sample := noise_a.get_pixel(3, 3)
	check(is_equal_approx(sample.r, sample.g) and is_equal_approx(sample.g, sample.b), "bool parameter: non-independent noise is grey")

	# A string parameter: the text generator draws something.
	var text := TetraGegl.apply("gegl:text", {"string": "Tetra", "size": 12.0, "color": Color(1, 1, 1, 1)}, flat(64, 20, Color(0, 0, 0, 0)))
	var ink := 0.0
	for y in 20:
		for x in 64:
			ink += text.get_pixel(x, y).a
	check(ink > 20.0, "string parameter: gegl:text renders glyphs (coverage %f)" % ink)


func test_document() -> void:
	var doc := TetraImageDocument.new()
	doc.create(8, 8, Color(0.2, 0.5, 0.8, 1))
	var layer := doc.get_active_layer()
	doc.select_rect(Rect2i(0, 0, 4, 8))
	var preview := doc.compute_operation(layer, "gegl:invert-linear", {})
	check(preview.get_pixel(1, 1).is_equal_approx(Color(0.8, 0.5, 0.2, 1)), "GEGL preview inside the selection")
	check(preview.get_pixel(5, 1).is_equal_approx(Color(0.2, 0.5, 0.8, 1)), "GEGL preview outside the selection")
	check_eq(doc.apply_operation(layer, "gegl:invert-linear", {}), OK, "apply a GEGL operation")
	check_eq(doc.layer_get_image(layer).get_data(), preview.get_data(), "apply equals the preview")
	check_eq(doc.get_undo_names()[-1], "Invert Linear", "undo name")
	doc.undo()
	check(doc.layer_get_image(layer).get_pixel(1, 1).is_equal_approx(Color(0.2, 0.5, 0.8, 1)), "undo a GEGL operation")
	check_eq(doc.apply_operation(layer, "gegl:no-such-operation", {}), ERR_DOES_NOT_EXIST, "unknown GEGL operation")


func test_threads() -> void:
	# Calls from several threads are serialised and all succeed.
	var image := picture()
	var expected := TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 2.0, "std-dev-y": 2.0}, image).get_data()
	var threads: Array[Thread] = []
	for i in 4:
		var thread := Thread.new()
		thread.start(func() -> PackedByteArray: return TetraGegl.apply("gegl:gaussian-blur", {"std-dev-x": 2.0, "std-dev-y": 2.0}, image).get_data())
		threads.append(thread)
	var all_equal := true
	for thread in threads:
		all_equal = all_equal and thread.wait_to_finish() == expected
	check(all_equal, "concurrent calls give the same result")
	await process_frame


func save_picture() -> void:
	# A painted document sent through a few GEGL filters, side by side.
	var doc := TetraImageDocument.new()
	doc.create(160, 120, Color(1, 1, 1, 1))
	var layer := doc.get_active_layer()
	doc.gradient_fill(layer, Vector2(0, 0), Vector2(160, 120), Color(0.9, 0.6, 0.1, 1).srgb_to_linear(), Color(0.1, 0.3, 0.7, 1).srgb_to_linear())
	doc.select_ellipse(Rect2i(40, 25, 80, 70))
	doc.fill(layer, Color(0.8, 0.1, 0.1, 1).srgb_to_linear())
	doc.select_none()
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH, {"color": Color(1, 1, 1, 1), "size": 14.0, "hardness": 0.4}, Vector2(20, 100), Vector2(140, 20))
	var filters := [["gegl:gaussian-blur", {"std-dev-x": 4.0, "std-dev-y": 4.0}], ["gegl:cartoon", {}], ["gegl:waves", {"amplitude": 8.0, "period": 60.0}],
			["gegl:emboss", {"type": "bumpmap", "depth": 6}], ["gegl:mosaic", {"tile-size": 12.0}]]
	var sheet := Image.create_empty(160 * (filters.size() + 1), 120, false, Image.FORMAT_RGBA8)
	sheet.blit_rect(doc.composite(), Rect2i(0, 0, 160, 120), Vector2i(0, 0))
	var original := doc.layer_get_image(layer).get_data()
	for i in filters.size():
		check_eq(doc.apply_operation(layer, filters[i][0], filters[i][1]), OK, "apply %s" % filters[i][0])
		check(doc.layer_get_image(layer).get_data() != original, "%s changes the picture" % filters[i][0])
		sheet.blit_rect(doc.composite(), Rect2i(0, 0, 160, 120), Vector2i(160 * (i + 1), 0))
		doc.undo()
	var path := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens/paint-gegl.png")
	check_eq(sheet.save_png(path), OK, "save the GEGL sheet")
