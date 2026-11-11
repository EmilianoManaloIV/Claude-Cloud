extends RefCounted
## Made-up operations for the screenshot drivers of the shared widgets.


## An operation that uses every parameter type, groups and advanced parameters.
static func kitchen_sink() -> Dictionary:
	var op := TetraSchema.make_operation("native:kitchen-sink", "Kitchen Sink", "Test/Everything", "native",
			"A made-up operation that uses every parameter type.")
	var add := func(param: Dictionary, description: String = "", group: String = "", advanced: bool = false) -> void:
		if description != "":
			TetraSchema.set_description(param, description)
		if group != "":
			TetraSchema.set_group(param, group)
		if advanced:
			TetraSchema.set_advanced(param)
		TetraSchema.add_param(op, param)

	add.call(TetraSchema.param_bool("clip", "Clip to the input extent", true), "Do not let the result grow beyond the layer.")
	add.call(TetraSchema.param_int("iterations", "Iterations", 3, 1, 10), "How many times the filter runs.")
	add.call(TetraSchema.set_ui_range(TetraSchema.param_float("radius", "Radius", 12.5, 0.0, 1500.0, 0.1, "px"), 0.0, 100.0),
			"Blur radius. The slider stops at 100 px; larger values can be typed.")
	add.call(TetraSchema.param_enum("mode", "Blend mode", "multiply", [
		TetraSchema.make_option("normal", "Normal"), TetraSchema.make_option("multiply", "Multiply"),
		TetraSchema.make_option("screen", "Screen"), TetraSchema.make_option("overlay", "Overlay")]), "How the result is combined.")
	add.call(TetraSchema.param_string("name", "Layer name", "Blurred copy"), "Name of the new layer.")
	add.call(TetraSchema.param_color("tint", "Tint", Color(0.95, 0.55, 0.2, 0.8)), "Colour multiplied into the result.", "Colour")
	add.call(TetraSchema.param_float("angle", "Hue rotation", 45.0, -180.0, 180.0, 1.0, "°"), "", "Colour")
	add.call(TetraSchema.param_float("opacity", "Opacity", 80.0, 0.0, 100.0, 1.0, "%"), "", "Colour")
	add.call(TetraSchema.param_vec2("offset", "Offset", Vector2(4, -2.5), -100.0, 100.0, 0.5, "px"), "Shift of the result.", "Geometry")
	add.call(TetraSchema.param_vec3("axis", "Rotation axis", Vector3(0, 0, 1), -1.0, 1.0, 0.01), "", "Geometry")
	add.call(TetraSchema.param_float("scale", "Scale", 1.0), "A float without limits.", "Geometry")
	add.call(TetraSchema.param_file("mask", "Mask image", "/home/artist/pictures/mask.png"), "A greyscale image.", "Files")
	add.call(TetraSchema.param_text("script", "Expression", "r = r * 0.5\ng = g * 0.5\nb = b"), "One assignment per line.", "Files")
	add.call(TetraSchema.param_seed("seed", "Random seed", 1234567), "Start value of the random sequence.", "", true)
	add.call(TetraSchema.param_int("threads", "Threads", 4, 1), "An integer with only a minimum.", "", true)
	add.call(TetraSchema.param_bool("linear", "Work in linear light", false), "", "", true)
	return op


## p_count operations spread over nested categories.
static func catalog(p_count: int) -> Array:
	var categories := ["Blur", "Blur/Motion", "Colors", "Colors/Map", "Colors/Tone Mapping", "Distort", "Edge Detect", "Enhance",
			"Generic", "Light and Shadow", "Noise", "Render/Fractals", "Render/Noise", "Render/Pattern", "Mesh/Edges", "Mesh/Faces",
			"Mesh/Vertices", "Video/Transitions", "Video/Audio", "Video/Stylize"]
	var first := ["Gaussian", "Linear", "Radial", "Adaptive", "Selective", "Soft", "Hard", "Smooth", "Local", "Global", "Fast",
			"Median", "Bilateral", "Lens", "Zoom", "Tiled", "Cubic", "Spherical", "Random", "Fractal"]
	var second := ["Blur", "Glow", "Sharpen", "Noise", "Shift", "Curve", "Balance", "Contrast", "Warp", "Bevel", "Emboss",
			"Gradient", "Threshold", "Mosaic", "Ripple", "Shadow", "Vignette"]
	var sources := ["gegl", "mlt", "kdenlive", "blender", "native"]
	var ops: Array = []
	for i in p_count:
		var label := "%s %s" % [first[i % first.size()], second[(i * 7 + i / first.size()) % second.size()]]
		if i >= first.size() * second.size():
			label += " %d" % (i / (first.size() * second.size()) + 1)
		var source: String = sources[i % sources.size()]
		var op := TetraSchema.make_operation("%s:%s-%d" % [source, label.to_lower().replace(" ", "-"), i], label,
				categories[(i * 13) % categories.size()], source,
				"%s applies a %s effect. This text is made up for the screenshot and is long enough to wrap onto a second line." % [label, second[(i * 7 + i / first.size()) % second.size()].to_lower()])
		TetraSchema.add_param(op, TetraSchema.param_float("amount", "Amount", 1.0, 0.0, 10.0))
		ops.append(op)
	return ops
