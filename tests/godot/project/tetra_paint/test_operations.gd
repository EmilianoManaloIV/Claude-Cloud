extends "res://tetra_test.gd"
## Native colour operations and filters of TetraImageDocument.


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 2e-5) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func flat(p_width: int, p_height: int, p_color: Color) -> Image:
	var image := Image.create_empty(p_width, p_height, false, Image.FORMAT_RGBAF)
	image.fill(p_color)
	return image


## Applies an operation to one colour and returns the result.
func one(p_id: String, p_values: Dictionary, p_color: Color) -> Color:
	return TetraImageDocument.filter_image(flat(1, 1, p_color), "native:" + p_id, p_values).get_pixel(0, 0)


func enc(p_value: float) -> float:
	return Color(p_value, 0, 0).linear_to_srgb().r


func dec(p_value: float) -> float:
	return Color(p_value, 0, 0).srgb_to_linear().r


func run() -> void:
	test_schema()
	test_point_operations()
	test_filters()
	test_document_integration()


func test_schema() -> void:
	var operations := TetraImageDocument.get_native_operations()
	var expected := ["invert", "desaturate", "brightness-contrast", "levels", "hue-saturation", "color-balance", "colorize",
			"threshold", "posterize", "gaussian-blur", "unsharp-mask", "pixelize", "edge-sobel", "emboss", "noise-rgb"]
	check_eq(operations.size(), expected.size(), "native operation count")
	for i in operations.size():
		var operation: Dictionary = operations[i]
		check_eq(operation["id"], "native:" + expected[i], "operation id")
		check_eq(operation["source"], "native", "operation source")
		check(String(operation["category"]).begins_with("Colors") or String(operation["category"]).begins_with("Filters/"), "category follows GIMP's menus: %s" % operation["category"])
		check_eq(TetraSchema.validate_operation(operation), "", "schema of %s" % operation["id"])
		# Running with the defaults must work and keep the size.
		var result := TetraImageDocument.filter_image(flat(5, 4, Color(0.3, 0.5, 0.7, 1.0)), operation["id"], TetraSchema.get_defaults(operation))
		check(result != null and result.get_size() == Vector2i(5, 4), "%s runs with its defaults" % operation["id"])
	var blur: Dictionary = operations[9]
	check_eq(blur["category"], "Filters/Blur", "blur category")
	check_eq(blur["params"][0]["name"], "std-dev-x", "GEGL parameter name")
	check_eq(blur["params"][0]["default"], 1.5, "GEGL default")
	check_eq(blur["params"][0]["max"], 1500.0, "GEGL range")
	check(TetraImageDocument.get_operations().size() >= operations.size(), "get_operations includes the native ones")
	check(TetraImageDocument.filter_image(flat(2, 2, Color.WHITE), "native:does-not-exist", {}) == null, "unknown operation returns null")


func test_point_operations() -> void:
	var c := Color(0.2, 0.5, 0.8, 0.6)

	check_color(one("invert", {"linear": true}, c), Color(0.8, 0.5, 0.2, 0.6), "linear invert")
	check_color(one("invert", {}, c), Color(dec(1 - enc(0.2)), dec(1 - enc(0.5)), dec(1 - enc(0.8)), 0.6), "perceptual invert")
	check_color(one("invert", {}, Color(0.2140411, 0.2140411, 0.2140411, 1)), Color(0.2140411, 0.2140411, 0.2140411, 1), "perceptual invert keeps middle grey", 1e-5)

	# Luminance: 0.2224879 * 0.2 + 0.7169038 * 0.5 + 0.0606083 * 0.8 = 0.4514361.
	var y := 0.4514361
	check_color(one("desaturate", {}, c), Color(y, y, y, 0.6), "desaturate luminance")
	var luma := dec(0.2224879 * enc(0.2) + 0.7169038 * enc(0.5) + 0.0606083 * enc(0.8))
	check_color(one("desaturate", {"mode": "luma"}, c), Color(luma, luma, luma, 0.6), "desaturate luma")
	var lightness := dec((enc(0.2) + enc(0.8)) / 2.0)
	check_color(one("desaturate", {"mode": "lightness"}, c), Color(lightness, lightness, lightness, 0.6), "desaturate lightness")
	var average := dec((enc(0.2) + enc(0.5) + enc(0.8)) / 3.0)
	check_color(one("desaturate", {"mode": "average"}, c), Color(average, average, average, 0.6), "desaturate average")
	check_color(one("desaturate", {"mode": "value"}, c), Color(0.8, 0.8, 0.8, 0.6), "desaturate value")

	# Brightness 0.5 lifts the encoded value a quarter of the way to white.
	var bright := func(p_v: float) -> float: return dec(enc(p_v) + (1.0 - enc(p_v)) * 0.25)
	check_color(one("brightness-contrast", {"brightness": 0.5}, c), Color(bright.call(0.2), bright.call(0.5), bright.call(0.8), 0.6), "brightness up")
	var dark := func(p_v: float) -> float: return dec(enc(p_v) * 0.75)
	check_color(one("brightness-contrast", {"brightness": -0.5}, c), Color(dark.call(0.2), dark.call(0.5), dark.call(0.8), 0.6), "brightness down")
	# Contrast 0.5 has slope tan(67.5 deg) = 2.414214 around 0.5.
	var contrast := func(p_v: float) -> float: return dec((enc(p_v) - 0.5) * 2.414214 + 0.5)
	check_color(one("brightness-contrast", {"contrast": 0.5}, c), Color(contrast.call(0.2), contrast.call(0.5), contrast.call(0.8), 0.6), "contrast", 1e-4)
	check_color(one("brightness-contrast", {}, c), c, "brightness-contrast defaults change nothing", 1e-6)

	var stretch := func(p_v: float) -> float: return dec((enc(p_v) - 0.2) / 0.6)
	check_color(one("levels", {"low-input": 0.2, "high-input": 0.8}, c), Color(stretch.call(0.2), stretch.call(0.5), stretch.call(0.8), 0.6), "levels input range")
	check_color(one("levels", {"gamma": 2.0}, c), Color(dec(sqrt(enc(0.2))), dec(sqrt(enc(0.5))), dec(sqrt(enc(0.8))), 0.6), "levels gamma")
	check_color(one("levels", {"low-output": 0.25, "high-output": 0.75}, c), Color(dec(enc(0.2) * 0.5 + 0.25), dec(enc(0.5) * 0.5 + 0.25), dec(enc(0.8) * 0.5 + 0.25), 0.6), "levels output range")
	check_color(one("levels", {"channel": "red", "low-input": 0.2, "high-input": 0.8}, c), Color(stretch.call(0.2), 0.5, 0.8, 0.6), "levels on one channel")
	check_color(one("levels", {"channel": "alpha", "high-output": 0.5}, c), Color(0.2, 0.5, 0.8, 0.3), "levels on alpha")
	check_color(one("levels", {"low-input": 0.2, "high-input": 0.8, "trc": "linear"}, c), Color(0.0, 0.5, 1.0, 0.6), "levels in linear light")
	check_color(one("levels", {"low-input": 0.5, "clamp-input": true}, c), Color(0, dec((enc(0.5) - 0.5) / 0.5), dec((enc(0.8) - 0.5) / 0.5), 0.6), "levels clamp input")

	var red := Color(1, 0, 0, 1)
	check_color(one("hue-saturation", {"hue": 1.0}, red), Color(0, 1, 1, 1), "hue half turn: red to cyan")
	check_color(one("hue-saturation", {"hue": 1.0 / 3.0}, red), Color(1, 1, 0, 1), "hue sixth turn: red to yellow")
	# Lightness +0.5 moves red (l 0.5) to l 0.75: encoded (1, 0.5, 0.5).
	check_color(one("hue-saturation", {"lightness": 0.5}, red), Color(1, 0.2140411, 0.2140411, 1), "lightness up")
	check_color(one("hue-saturation", {"lightness": -0.5}, red), Color(dec(0.5), 0, 0, 1), "lightness down")
	# Saturation -1 leaves the HSL lightness of the encoded colour.
	check_color(one("hue-saturation", {"saturation": -1.0}, c), Color(lightness, lightness, lightness, 0.6), "saturation removed")
	check_color(one("hue-saturation", {}, c), c, "hue-saturation defaults change nothing", 1e-5)

	# Mid grey (encoded 0.5): a full midtone red shift adds 0.7 and clamps at 1.
	var grey := Color(0.2140411, 0.2140411, 0.2140411, 1)
	check_color(one("color-balance", {"midtones-cyan-red": 1.0, "preserve-luminosity": false}, grey), Color(1, 0.2140411, 0.2140411, 1), "color balance midtones")
	# Preserving the lightness (0.5) turns encoded (1, 0.5, 0.5) into pure red.
	check_color(one("color-balance", {"midtones-cyan-red": 1.0}, grey), Color(1, 0, 0, 1), "color balance preserving luminosity")
	check_color(one("color-balance", {"shadows-cyan-red": 1.0, "highlights-yellow-blue": 1.0, "preserve-luminosity": false}, grey), grey, "shadows and highlights do not touch mid grey", 1e-5)
	check_color(one("color-balance", {"shadows-yellow-blue": 1.0, "preserve-luminosity": false}, Color(0, 0, 0, 1)), Color(0, 0, dec(0.7), 1), "color balance shadows on black")

	# Colorize: luminance 0.4, hue 0.5, saturation 0.5 -> (0.2, 0.6, 0.6).
	check_color(one("colorize", {}, Color(0.4, 0.4, 0.4, 0.6)), Color(0.2, 0.6, 0.6, 0.6), "colorize")
	check_color(one("colorize", {"lightness": 0.5}, Color(0.4, 0.4, 0.4, 1)), Color(0.55, 0.85, 0.85, 1), "colorize lighter")
	check_color(one("colorize", {"hue": 0.0, "saturation": 1.0}, Color(0.5, 0.5, 0.5, 1)), Color(1, 0, 0, 1), "colorize red")

	check_color(one("threshold", {}, Color(0.3, 0.1, 0.1, 0.6)), Color(1, 1, 1, 0.6), "threshold above (encoded 0.58)")
	check_color(one("threshold", {}, Color(0.1, 0.1, 0.1, 0.6)), Color(0, 0, 0, 0.6), "threshold below (encoded 0.35)")
	check_color(one("threshold", {"channel": "green"}, Color(0.3, 0.1, 0.1, 1)), Color(0, 0, 0, 1), "threshold on one channel")
	check_color(one("threshold", {"low": 0.0, "high": 0.4}, Color(0.1, 0.1, 0.1, 1)), Color(1, 1, 1, 1), "threshold window")

	# Three levels: encoded 0.4845 -> 0.5, 0.7354 -> 0.5, 0.9063 -> 1.
	check_color(one("posterize", {}, Color(0.2, 0.5, 0.8, 1)), Color(dec(0.5), dec(0.5), 1.0, 1), "posterize")
	check_color(one("posterize", {"levels": 2}, Color(0.2, 0.5, 0.8, 1)), Color(0, 1, 1, 1), "posterize two levels")


func test_filters() -> void:
	# Gaussian blur of one white pixel, sigma 1: kernel 0.39905, 0.24203, 0.054005, 0.004433.
	var impulse := flat(9, 9, Color(0, 0, 0, 1))
	impulse.set_pixel(4, 4, Color(1, 1, 1, 1))
	var blurred := TetraImageDocument.filter_image(impulse, "native:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 1.0})
	check_near(blurred.get_pixel(4, 4).r, 0.39905 * 0.39905, 1e-5, "blur centre")
	check_near(blurred.get_pixel(5, 4).r, 0.39905 * 0.24203, 1e-5, "blur neighbour")
	check_near(blurred.get_pixel(6, 3).r, 0.054005 * 0.24203, 1e-5, "blur further out")
	check_near(blurred.get_pixel(8, 4).r, 0.0, 0.0, "blur ends at three sigma")
	var total := 0.0
	for py in 9:
		for px in 9:
			total += blurred.get_pixel(px, py).r
	check_near(total, 1.0, 1e-5, "blur preserves the total")
	var horizontal := TetraImageDocument.filter_image(impulse, "native:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 0.0})
	check_near(horizontal.get_pixel(5, 4).r, 0.24203, 1e-5, "horizontal-only blur")
	check_near(horizontal.get_pixel(4, 5).r, 0.0, 0.0, "horizontal-only blur leaves other rows")
	# Alpha-weighted: a red dot on nothing spreads as red, never as dark fringes.
	var dot := flat(9, 9, Color(0, 0, 0, 0))
	dot.set_pixel(4, 4, Color(1, 0, 0, 1))
	var soft := TetraImageDocument.filter_image(dot, "native:gaussian-blur", {"std-dev-x": 1.0, "std-dev-y": 1.0})
	check_color(soft.get_pixel(5, 5), Color(1, 0, 0, 0.24203 * 0.24203), "blur is alpha-weighted", 1e-5)

	# Unsharp mask on a step from 0.2 to 0.8: the blurred value next to the
	# edge is 0.8 - 0.6 * 0.300468, so amount 1 overshoots to 0.980281.
	var step := flat(12, 3, Color(0.2, 0.2, 0.2, 1))
	step.fill_rect(Rect2i(6, 0, 6, 3), Color(0.8, 0.8, 0.8, 1))
	var sharp := TetraImageDocument.filter_image(step, "native:unsharp-mask", {"std-dev": 1.0, "scale": 1.0})
	check_near(sharp.get_pixel(6, 1).r, 0.980281, 1e-5, "unsharp overshoot")
	check_near(sharp.get_pixel(5, 1).r, 0.019719, 1e-5, "unsharp undershoot")
	check_near(sharp.get_pixel(1, 1).r, 0.2, 1e-6, "unsharp leaves flat areas")
	var gated := TetraImageDocument.filter_image(step, "native:unsharp-mask", {"std-dev": 1.0, "scale": 1.0, "threshold": 0.5})
	check_near(gated.get_pixel(6, 1).r, 0.8, 1e-6, "unsharp threshold")

	var blocks := flat(4, 2, Color(0, 0, 0, 1))
	blocks.set_pixel(0, 0, Color(1, 1, 1, 1))
	blocks.set_pixel(3, 1, Color(1, 0, 0, 0))
	var pixelized := TetraImageDocument.filter_image(blocks, "native:pixelize", {"size-x": 2, "size-y": 2})
	check_color(pixelized.get_pixel(1, 1), Color(0.25, 0.25, 0.25, 1), "pixelize averages a block")
	check_color(pixelized.get_pixel(2, 0), Color(0, 0, 0, 0.75), "pixelize averages alpha and weights colour by it")
	var whole := TetraImageDocument.filter_image(blocks, "native:pixelize", {"size-x": 16, "size-y": 16})
	check_near(whole.get_pixel(3, 0).a, 7.0 / 8.0, 1e-6, "a block larger than the image averages everything")

	# Sobel on a vertical edge between columns 1 and 2: gradient 4 -> 4 / 5.656854.
	var edge := flat(4, 3, Color(0, 0, 0, 1))
	edge.fill_rect(Rect2i(2, 0, 2, 3), Color(1, 1, 1, 1))
	var sobel := TetraImageDocument.filter_image(edge, "native:edge-sobel", {})
	check_near(sobel.get_pixel(1, 1).r, 0.707107, 1e-5, "sobel magnitude left of the edge")
	check_near(sobel.get_pixel(2, 1).r, 0.707107, 1e-5, "sobel magnitude right of the edge")
	check_near(sobel.get_pixel(0, 1).r, 0.0, 1e-6, "sobel flat area")
	check_near(sobel.get_pixel(1, 1).a, 1.0, 0.0, "sobel keeps alpha")
	var signed := TetraImageDocument.filter_image(edge, "native:edge-sobel", {"vertical": false})
	check_near(signed.get_pixel(1, 1).r, 1.0, 1e-6, "sobel horizontal with sign")
	check_near(signed.get_pixel(3, 1).r, 0.5, 1e-6, "sobel signed zero is mid grey")
	var vertical := TetraImageDocument.filter_image(edge, "native:edge-sobel", {"horizontal": false, "keep-sign": false})
	check_near(vertical.get_pixel(1, 1).r, 0.0, 1e-6, "no vertical gradient on a vertical edge")

	# A flat surface is lit by sin(elevation).
	var surface := flat(5, 5, Color(0.3, 0.6, 0.9, 0.8))
	var embossed := TetraImageDocument.filter_image(surface, "native:emboss", {})
	check_color(embossed.get_pixel(2, 2), Color(0.707107, 0.707107, 0.707107, 0.8), "emboss of a flat area")
	var bump := TetraImageDocument.filter_image(surface, "native:emboss", {"type": "bumpmap", "elevation": 30.0})
	check_color(bump.get_pixel(2, 2), Color(0.15, 0.3, 0.45, 0.8), "bumpmap keeps the colour times the shade")
	# A step lit from the right is bright on the side facing the light.
	var relief := flat(6, 5, Color(0, 0, 0, 1))
	relief.fill_rect(Rect2i(3, 0, 3, 5), Color(1, 1, 1, 1))
	var lit := TetraImageDocument.filter_image(relief, "native:emboss", {"azimuth": 0.0, "depth": 10})
	var facing := TetraImageDocument.filter_image(relief, "native:emboss", {"azimuth": 180.0, "depth": 10})
	check(lit.get_pixel(2, 2).r < 0.1 and facing.get_pixel(2, 2).r > 0.7, "emboss shades an edge by the light direction: %f / %f" % [lit.get_pixel(2, 2).r, facing.get_pixel(2, 2).r])

	var canvas := flat(64, 64, Color(0.5, 0.5, 0.5, 0.5))
	var noise_a := TetraImageDocument.filter_image(canvas, "native:noise-rgb", {"seed": 7})
	var noise_b := TetraImageDocument.filter_image(canvas, "native:noise-rgb", {"seed": 7})
	var noise_c := TetraImageDocument.filter_image(canvas, "native:noise-rgb", {"seed": 8})
	check_eq(noise_a.get_data(), noise_b.get_data(), "noise is repeatable for a seed")
	check(noise_a.get_data() != noise_c.get_data(), "another seed gives other noise")
	var mean := 0.0
	var changed := 0
	var in_range := true
	for py in 64:
		for px in 64:
			var p := noise_a.get_pixel(px, py)
			mean += p.r
			changed += 1 if p.r != 0.5 else 0
			in_range = in_range and p.r >= 0.0 and p.r <= 1.0 and p.a == 0.5
	check_near(mean / 4096.0, 0.5, 0.01, "noise is centred on the original value")
	check(changed > 4000, "noise touches almost every pixel")
	check(in_range, "noise stays in range and leaves alpha alone")
	var none := TetraImageDocument.filter_image(canvas, "native:noise-rgb", {"red": 0.0, "green": 0.0, "blue": 0.0})
	check_eq(none.get_data(), canvas.get_data(), "zero amounts add no noise")
	var grey_noise := TetraImageDocument.filter_image(canvas, "native:noise-rgb", {"independent": false, "seed": 3})
	var p0 := grey_noise.get_pixel(10, 10)
	check(p0.r == p0.g and p0.g == p0.b and p0.r != 0.5, "non-independent noise is the same on all channels")


func test_document_integration() -> void:
	var doc := TetraImageDocument.new()
	doc.create(4, 4, Color(0.2, 0.5, 0.8, 1))
	var layer := doc.get_active_layer()
	doc.select_rect(Rect2i(0, 0, 2, 4))
	doc.clear_undo_history()
	doc.get_and_clear_dirty_rect()

	# compute_operation previews without touching the document.
	var preview := doc.compute_operation(layer, "native:invert", {"linear": true})
	check_color(preview.get_pixel(1, 1), Color(0.8, 0.5, 0.2, 1), "preview inside the selection")
	check_color(preview.get_pixel(2, 1), Color(0.2, 0.5, 0.8, 1), "preview outside the selection")
	check_color(doc.layer_get_image(layer).get_pixel(1, 1), Color(0.2, 0.5, 0.8, 1), "the document is unchanged by a preview")
	check(not doc.can_undo(), "a preview records nothing")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(), "a preview dirties nothing")

	check_eq(doc.apply_operation(layer, "native:invert", {"linear": true}), OK, "apply")
	check_eq(doc.layer_get_image(layer).get_data(), preview.get_data(), "apply gives exactly the preview")
	check_eq(doc.get_undo_names(), PackedStringArray(["Invert"]), "apply is one named undo step")
	check_eq(doc.get_and_clear_dirty_rect(), Rect2i(0, 0, 2, 4), "apply dirties the selection")
	doc.undo()
	check_color(doc.layer_get_image(layer).get_pixel(1, 1), Color(0.2, 0.5, 0.8, 1), "undo apply")

	# Committing a preview with layer_set_image inside a group is equivalent.
	doc.begin_undo_group("Invert (preview)")
	doc.layer_set_image(layer, preview)
	doc.end_undo_group()
	check_color(doc.layer_get_image(layer).get_pixel(0, 3), Color(0.8, 0.5, 0.2, 1), "committed preview")
	check_eq(doc.get_undo_memory_usage(), 2 * 4 * 16, "the commit stores only the changed half")
	doc.undo()

	# A half selection gives half of the effect; lock alpha keeps the alpha.
	var half := Image.create_empty(4, 4, false, Image.FORMAT_RF)
	half.fill(Color(0.5, 0, 0))
	doc.select_mask(half)
	doc.apply_operation(layer, "native:invert", {"linear": true})
	check_color(doc.layer_get_image(layer).get_pixel(3, 3), Color(0.5, 0.5, 0.5, 1), "half-selected pixels get half of the effect")
	doc.undo()
	doc.select_none()
	doc.layer_set_lock_alpha(layer, true)
	doc.apply_operation(layer, "native:levels", {"channel": "alpha", "high-output": 0.5})
	check_near(doc.layer_get_image(layer).get_pixel(0, 0).a, 1.0, 0.0, "locked alpha survives an operation")
	doc.layer_set_lock_alpha(layer, false)
	doc.apply_operation(layer, "native:levels", {"channel": "alpha", "high-output": 0.5})
	check_near(doc.layer_get_image(layer).get_pixel(0, 0).a, 0.5, 1e-6, "unlocked alpha changes")

	# A filter can run on a worker thread on a snapshot while the document stays usable.
	var snapshot := doc.layer_get_image(layer)
	var thread := Thread.new()
	thread.start(func() -> Image: return TetraImageDocument.filter_image(snapshot, "native:gaussian-blur", {"std-dev-x": 2.0, "std-dev-y": 2.0}))
	doc.layer_set_opacity(layer, 0.5)
	var threaded: Image = thread.wait_to_finish()
	check_eq(threaded.get_size(), Vector2i(4, 4), "filter_image works from a thread")
	check_color(threaded.get_pixel(2, 2), Color(0.2, 0.5, 0.8, 0.5), "blur of a flat layer is flat", 1e-5)
