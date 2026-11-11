extends "res://tetra_test.gd"
## TetraSchema: building operations, defaults, sanitising values, validation,
## search ranking and the category tree.


func run() -> void:
	_test_builders()
	_test_defaults()
	_test_sanitize_tables()
	_test_sanitize_values()
	_test_validation()
	_test_find_param()
	_test_search()
	_test_category_tree()


## An operation that uses every parameter type.
func _kitchen_sink() -> Dictionary:
	var op := TetraSchema.make_operation("native:kitchen-sink", "Kitchen Sink", "Test/Everything", "native", "Uses every parameter type.")
	TetraSchema.add_param(op, TetraSchema.param_bool("clip", "Clip", true))
	TetraSchema.add_param(op, TetraSchema.param_int("iterations", "Iterations", 3, 1, 10))
	TetraSchema.add_param(op, TetraSchema.param_float("radius", "Radius", 1.5, 0.0, 100.0, 0.1, "px"))
	TetraSchema.add_param(op, TetraSchema.param_string("name", "Name", "abc"))
	TetraSchema.add_param(op, TetraSchema.param_text("script", "Script", "one\ntwo"))
	TetraSchema.add_param(op, TetraSchema.param_enum("mode", "Mode", "normal", TetraSchema.make_options(["normal", "multiply", "screen"])))
	TetraSchema.add_param(op, TetraSchema.param_enum("quality", "Quality", 1, [
		TetraSchema.make_option(0, "Draft"), TetraSchema.make_option(1, "Good"), TetraSchema.make_option(2, "Best")]))
	TetraSchema.add_param(op, TetraSchema.param_color("tint", "Tint", Color(1, 0.5, 0, 1)))
	TetraSchema.add_param(op, TetraSchema.param_vec2("offset", "Offset", Vector2(2, 3), -10.0, 10.0))
	TetraSchema.add_param(op, TetraSchema.param_vec3("axis", "Axis", Vector3(0, 0, 1)))
	TetraSchema.add_param(op, TetraSchema.param_file("path", "Path", "res://a.png"))
	TetraSchema.add_param(op, TetraSchema.param_seed("seed", "Seed", 42))
	return op


func _check_value(p_actual: Variant, p_expected: Variant, p_what: String) -> void:
	check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func _test_builders() -> void:
	var op := TetraSchema.make_operation("gegl:gaussian-blur", "Gaussian Blur", "Blur", "gegl", "Averages neighbouring pixels.")
	check_eq(op, {"id": "gegl:gaussian-blur", "label": "Gaussian Blur", "description": "Averages neighbouring pixels.",
			"category": "Blur", "source": "gegl", "params": []}, "make_operation")
	check(not TetraSchema.make_operation("a:b", "B", "", "native").has("description"), "an empty description is left out")

	var size := TetraSchema.param_float("std-dev-x", "Size X", 1.5, 0.0, 1500.0, 0.01, "px")
	check_eq(size, {"name": "std-dev-x", "label": "Size X", "type": "float", "default": 1.5,
			"min": 0.0, "max": 1500.0, "step": 0.01, "unit": "px"}, "param_float with every optional key")
	check_eq(TetraSchema.param_float("x", "X"), {"name": "x", "label": "X", "type": "float", "default": 0.0},
			"param_float leaves unset limits out")
	check_eq(TetraSchema.param_int("n", "N", 4, 1), {"name": "n", "label": "N", "type": "int", "default": 4, "min": 1.0},
			"param_int with only a minimum")
	check_eq(typeof(TetraSchema.param_int("n", "N", 4)["default"]), TYPE_INT, "param_int default is an int")
	check_eq(TetraSchema.param_bool("b", "B", true), {"name": "b", "label": "B", "type": "bool", "default": true}, "param_bool")
	check_eq(TetraSchema.param_string("s", "S", "x"), {"name": "s", "label": "S", "type": "string", "default": "x"}, "param_string")
	check_eq(TetraSchema.param_text("t", "T", "x"), {"name": "t", "label": "T", "type": "text", "default": "x"}, "param_text")
	check_eq(TetraSchema.param_file("f", "F", "x"), {"name": "f", "label": "F", "type": "file", "default": "x"}, "param_file")
	check_eq(TetraSchema.param_seed("r", "R", 7), {"name": "r", "label": "R", "type": "seed", "default": 7}, "param_seed")
	check_eq(TetraSchema.param_color("c", "C", Color.RED), {"name": "c", "label": "C", "type": "color", "default": Color.RED}, "param_color")
	check_eq(TetraSchema.param_vec2("v", "V", Vector2(1, 2)), {"name": "v", "label": "V", "type": "vec2", "default": Vector2(1, 2)}, "param_vec2")
	check_eq(TetraSchema.param_vec3("v", "V", Vector3(1, 2, 3)), {"name": "v", "label": "V", "type": "vec3", "default": Vector3(1, 2, 3)}, "param_vec3")
	check_eq(TetraSchema.param_enum("e", "E", "a", TetraSchema.make_options(["a", "b"])),
			{"name": "e", "label": "E", "type": "enum", "default": "a",
			"options": [{"value": "a", "label": "a"}, {"value": "b", "label": "b"}]}, "param_enum")

	# The setters change the parameter in place and return it.
	var returned := TetraSchema.set_advanced(TetraSchema.set_group(TetraSchema.set_description(
			TetraSchema.set_ui_range(size, 0.0, 100.0), "Standard deviation"), "Size"))
	check_eq(size["ui_min"], 0.0, "set_ui_range ui_min")
	check_eq(size["ui_max"], 100.0, "set_ui_range ui_max")
	check_eq(size["description"], "Standard deviation", "set_description")
	check_eq(size["group"], "Size", "set_group")
	check_eq(size["advanced"], true, "set_advanced")
	check(is_same(returned, size), "setters return the same Dictionary")
	TetraSchema.set_range(size, 1.0, 2.0)
	TetraSchema.set_step(size, 0.5)
	TetraSchema.set_unit(size, "mm")
	check_eq([size["min"], size["max"], size["step"], size["unit"]], [1.0, 2.0, 0.5, "mm"], "set_range, set_step, set_unit")

	check(is_same(TetraSchema.add_param(op, size), op), "add_param returns the operation")
	check_eq(op["params"].size(), 1, "add_param appends")
	check_eq(TetraSchema.get_param_types(), PackedStringArray(["bool", "int", "float", "string", "text", "enum", "color", "vec2", "vec3", "file", "seed"]), "parameter types")
	check_eq(TetraSchema.get_sources(), PackedStringArray(["native", "gegl", "mlt", "kdenlive", "blender"]), "sources")
	check_eq(TetraSchema.validate_operation(_kitchen_sink()), "", "the kitchen sink operation is valid")


func _test_defaults() -> void:
	var defaults := TetraSchema.get_defaults(_kitchen_sink())
	check_eq(defaults.keys(), ["clip", "iterations", "radius", "name", "script", "mode", "quality", "tint", "offset", "axis", "path", "seed"],
			"defaults keep the display order")
	_check_value(defaults["clip"], true, "default clip")
	_check_value(defaults["iterations"], 3, "default iterations")
	_check_value(defaults["radius"], 1.5, "default radius")
	_check_value(defaults["name"], "abc", "default name")
	_check_value(defaults["script"], "one\ntwo", "default script")
	_check_value(defaults["mode"], "normal", "default mode")
	_check_value(defaults["quality"], 1, "default quality")
	_check_value(defaults["tint"], Color(1, 0.5, 0, 1), "default tint")
	_check_value(defaults["offset"], Vector2(2, 3), "default offset")
	_check_value(defaults["axis"], Vector3(0, 0, 1), "default axis")
	_check_value(defaults["path"], "res://a.png", "default path")
	_check_value(defaults["seed"], 42, "default seed")

	# Without a usable default every type has a zero value inside its limits.
	var zeroes := {
		"bool": false, "int": 0, "float": 0.0, "string": "", "text": "", "file": "", "color": Color(0, 0, 0, 1),
		"vec2": Vector2(), "vec3": Vector3(), "seed": 0,
	}
	for type: String in zeroes:
		_check_value(TetraSchema.get_param_default({"name": "p", "label": "P", "type": type}), zeroes[type], "zero value of " + type)
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "int", "min": 2, "max": 9}), 2, "zero value of an int with a minimum")
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "float", "min": -5.0, "max": -1.0}), -1.0, "zero value of a float below zero")
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "enum", "options": TetraSchema.make_options(["x", "y"])}), "x", "an enum falls back to its first option")
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "enum", "default": "nope", "options": TetraSchema.make_options(["x", "y"])}), "x", "an enum with a bad default uses its first option")
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "int", "default": 7.0}), 7, "a float default of an int is converted")
	_check_value(TetraSchema.get_param_default({"name": "p", "type": "float", "default": 50, "max": 10}), 10.0, "a default is clamped")
	check_eq(TetraSchema.get_defaults({}), {}, "an operation without params has no defaults")


func _test_sanitize_tables() -> void:
	var op := _kitchen_sink()
	# Each row: parameter name, input, expected output.
	var table: Array = [
		["clip", false, false], ["clip", 1, true], ["clip", 0.0, false], ["clip", "true", true], ["clip", " No ", false],
		["clip", "on", true], ["clip", "maybe", true], ["clip", null, true], ["clip", [], true],

		["iterations", 5, 5], ["iterations", 0, 1], ["iterations", 99, 10], ["iterations", -3, 1], ["iterations", 4.4, 4],
		["iterations", 4.5, 5], ["iterations", "7", 7], ["iterations", "7.6", 8], ["iterations", "x", 3], ["iterations", true, 1],
		["iterations", NAN, 3], ["iterations", INF, 3], ["iterations", null, 3], ["iterations", Vector2(1, 1), 3],

		["radius", 2, 2.0], ["radius", 2.25, 2.25], ["radius", -1.0, 0.0], ["radius", 250, 100.0], ["radius", "3.5", 3.5],
		["radius", " 12 ", 12.0], ["radius", "wide", 1.5], ["radius", NAN, 1.5], ["radius", -INF, 1.5], ["radius", null, 1.5],
		["radius", false, 0.0],

		["name", "hello", "hello"], ["name", &"sn", "sn"], ["name", 12, "12"], ["name", 1.5, "1.5"], ["name", true, "true"],
		["name", null, "abc"], ["name", [1], "abc"], ["name", "", ""],
		["script", "a\nb\nc", "a\nb\nc"], ["script", null, "one\ntwo"],
		["path", "res://b.png", "res://b.png"], ["path", {}, "res://a.png"],

		["mode", "screen", "screen"], ["mode", &"multiply", "multiply"], ["mode", "Screen", "normal"], ["mode", "burn", "normal"],
		["mode", 1, "normal"], ["mode", null, "normal"],
		["quality", 2, 2], ["quality", 2.0, 2], ["quality", "0", 0], ["quality", 3, 1], ["quality", "Best", 1], ["quality", null, 1],

		["tint", Color(0, 1, 0, 0.5), Color(0, 1, 0, 0.5)], ["tint", "#ff0000", Color(1, 0, 0, 1)], ["tint", "blue", Color(0, 0, 1, 1)],
		["tint", [0.25, 0.5, 0.75], Color(0.25, 0.5, 0.75, 1)], ["tint", [0.25, 0.5, 0.75, 0.5], Color(0.25, 0.5, 0.75, 0.5)],
		["tint", Vector3(1, 1, 0), Color(1, 1, 0, 1)], ["tint", "not a colour", Color(1, 0.5, 0, 1)], ["tint", 5, Color(1, 0.5, 0, 1)],
		["tint", [1, 2], Color(1, 0.5, 0, 1)], ["tint", null, Color(1, 0.5, 0, 1)],

		["offset", Vector2(4, -4), Vector2(4, -4)], ["offset", Vector2(40, -40), Vector2(10, -10)], ["offset", Vector2i(1, 2), Vector2(1, 2)],
		["offset", [5, 6.5], Vector2(5, 6.5)], ["offset", 3, Vector2(3, 3)], ["offset", [1, 2, 3], Vector2(2, 3)],
		["offset", "1,2", Vector2(2, 3)], ["offset", Vector2(NAN, 0), Vector2(2, 3)], ["offset", null, Vector2(2, 3)],

		["axis", Vector3(1, 2, 3), Vector3(1, 2, 3)], ["axis", Vector3i(1, 0, 0), Vector3(1, 0, 0)], ["axis", [0.5, 0.5, 0], Vector3(0.5, 0.5, 0)],
		["axis", PackedFloat32Array([1, 0, 0]), Vector3(1, 0, 0)], ["axis", 2.0, Vector3(2, 2, 2)], ["axis", Vector2(1, 1), Vector3(0, 0, 1)],
		["axis", Color(1, 0, 0), Vector3(0, 0, 1)], ["axis", null, Vector3(0, 0, 1)],

		["seed", 7, 7], ["seed", -1, 0], ["seed", 4294967295, 4294967295], ["seed", 4294967296, 4294967295], ["seed", 12.6, 13],
		["seed", "99", 99], ["seed", "abc", 42], ["seed", null, 42],
	]
	for row: Array in table:
		var param := TetraSchema.find_param(op, row[0])
		_check_value(TetraSchema.sanitize_value(param, row[1]), row[2], "sanitize %s <- %s" % [row[0], var_to_str(row[1])])

	var limited_seed := TetraSchema.param_seed("s", "S", 5, 1, 100)
	_check_value(TetraSchema.sanitize_value(limited_seed, 0), 1, "a seed honours an explicit minimum")
	_check_value(TetraSchema.sanitize_value(limited_seed, 1000), 100, "a seed honours an explicit maximum")
	var signed_seed := TetraSchema.param_seed("s", "S", 0, -2147483648, 2147483647)
	_check_value(TetraSchema.sanitize_value(signed_seed, -5), -5, "a seed with a negative minimum accepts negatives")
	var big := TetraSchema.param_int("n", "N", 0)
	_check_value(TetraSchema.sanitize_value(big, 9007199254740993), 9007199254740993, "an unlimited int keeps 64-bit precision")
	var fractional := TetraSchema.param_int("n", "N", 2, 1.5, 4.5)
	_check_value(TetraSchema.sanitize_value(fractional, 0), 2, "an int is clamped to the integers inside fractional limits (low)")
	_check_value(TetraSchema.sanitize_value(fractional, 9), 4, "an int is clamped to the integers inside fractional limits (high)")
	_check_value(TetraSchema.sanitize_value({"name": "u", "type": "matrix", "default": 1}, "kept"), "kept", "a value of an unknown type passes through")


func _test_sanitize_values() -> void:
	var op := _kitchen_sink()
	var values := TetraSchema.sanitize_values(op, {
		"iterations": 50, "radius": "7.5", "mode": "nonsense", "unknown": 1, "tint": "#00ff00", "extra": "dropped",
	})
	check_eq(values.keys(), ["clip", "iterations", "radius", "name", "script", "mode", "quality", "tint", "offset", "axis", "path", "seed"],
			"sanitize_values returns exactly the operation's parameters, in order")
	_check_value(values["iterations"], 10, "sanitize_values clamps")
	_check_value(values["radius"], 7.5, "sanitize_values converts")
	_check_value(values["mode"], "normal", "sanitize_values replaces a bad enum value by the default")
	_check_value(values["tint"], Color(0, 1, 0, 1), "sanitize_values parses a colour")
	_check_value(values["clip"], true, "sanitize_values fills a missing bool")
	_check_value(values["seed"], 42, "sanitize_values fills a missing seed")
	check_eq(TetraSchema.sanitize_values(op, {}), TetraSchema.get_defaults(op), "sanitising nothing gives the defaults")
	check_eq(TetraSchema.sanitize_values(op, values), values, "sanitising is idempotent")


func _test_validation() -> void:
	check_eq(TetraSchema.validate_operation({}),
			"'id' must be a non-empty String\n'label' must be a non-empty String\n'category' must be a String\n" +
			"'source' must be one of native, gegl, mlt, kdenlive, blender\n'params' must be an Array", "an empty operation")
	check_eq(TetraSchema.validate_operation(TetraSchema.make_operation("x:y", "Y", "", "native")), "", "an empty category and no params are valid")
	check_eq(TetraSchema.validate_operation(TetraSchema.make_operation("x:y", "Y", "A/B", "photoshop")),
			"'source' must be one of native, gegl, mlt, kdenlive, blender", "an unknown source")

	# Each row: a broken parameter and the exact complaint.
	var broken: Array = [
		[42, "param 0: must be a Dictionary"],
		[{"label": "L", "type": "bool", "default": true}, "param 0: 'name' must be a non-empty String"],
		[{"name": "p", "type": "bool", "default": true}, "param 'p': 'label' must be a non-empty String"],
		[{"name": "p", "label": "P", "type": "matrix", "default": 1},
				"param 'p': 'type' must be one of bool, int, float, string, text, enum, color, vec2, vec3, file, seed"],
		[{"name": "p", "label": "P", "type": "float"}, "param 'p': 'default' is missing"],
		[{"name": "p", "label": "P", "type": "bool", "default": 1}, "param 'p': 'default' must be a bool"],
		[{"name": "p", "label": "P", "type": "int", "default": 1.5}, "param 'p': 'default' must be an int"],
		[{"name": "p", "label": "P", "type": "float", "default": "1"}, "param 'p': 'default' must be a float"],
		[{"name": "p", "label": "P", "type": "string", "default": 1}, "param 'p': 'default' must be a String"],
		[{"name": "p", "label": "P", "type": "color", "default": "#fff"}, "param 'p': 'default' must be a Color"],
		[{"name": "p", "label": "P", "type": "vec2", "default": Vector3()}, "param 'p': 'default' must be a Vector2"],
		[{"name": "p", "label": "P", "type": "vec3", "default": Vector2()}, "param 'p': 'default' must be a Vector3"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "min": "0"}, "param 'p': 'min' must be a finite number"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "min": 5.0, "max": 0.0},
				"param 'p': 'min' is greater than 'max'\nparam 'p': 'default' is outside 'min'/'max'"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "ui_min": 5.0, "ui_max": 0.0}, "param 'p': 'ui_min' is greater than 'ui_max'"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "step": 0.0}, "param 'p': 'step' must be greater than 0"],
		[{"name": "p", "label": "P", "type": "float", "default": 11.0, "min": 0.0, "max": 10.0}, "param 'p': 'default' is outside 'min'/'max'"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "unit": 3}, "param 'p': 'unit' must be a String"],
		[{"name": "p", "label": "P", "type": "float", "default": 1.0, "advanced": "yes"}, "param 'p': 'advanced' must be a bool"],
		[{"name": "p", "label": "P", "type": "enum", "default": "a"}, "param 'p': 'options' must be a non-empty Array of {value, label}"],
		[{"name": "p", "label": "P", "type": "enum", "default": "a", "options": ["a"]}, "param 'p': option 0 must be a Dictionary"],
		[{"name": "p", "label": "P", "type": "enum", "default": "a", "options": [{"label": "A"}]}, "param 'p': option 0 needs a String or int 'value'"],
		[{"name": "p", "label": "P", "type": "enum", "default": "a", "options": [{"value": "a"}]}, "param 'p': option 0 needs a String 'label'"],
		[{"name": "p", "label": "P", "type": "enum", "default": "c", "options": TetraSchema.make_options(["a", "b"])},
				"param 'p': 'default' must be the 'value' of one of the options"],
		[{"name": "p", "label": "P", "type": "enum", "default": "a", "options": TetraSchema.make_options(["a", "a"])},
				"param 'p': option 1 repeats the value of option 0"],
	]
	for row: Array in broken:
		var op := TetraSchema.make_operation("x:y", "Y", "Test", "native")
		op["params"] = [row[0]]
		check_eq(TetraSchema.validate_operation(op), row[1], "validation of %s" % var_to_str(row[0]).replace("\n", " "))

	var duplicated := TetraSchema.make_operation("x:y", "Y", "Test", "native")
	TetraSchema.add_param(duplicated, TetraSchema.param_bool("same", "First"))
	TetraSchema.add_param(duplicated, TetraSchema.param_bool("same", "Second"))
	check_eq(TetraSchema.validate_operation(duplicated), "param 'same': duplicate name", "duplicate parameter names")
	var int_from_json := TetraSchema.make_operation("x:y", "Y", "Test", "native")
	int_from_json["params"] = [{"name": "p", "label": "P", "type": "int", "default": 3.0}]
	check_eq(TetraSchema.validate_operation(int_from_json), "", "an int default may arrive as a whole float")


func _test_find_param() -> void:
	var op := _kitchen_sink()
	check_eq(TetraSchema.find_param(op, "radius")["label"], "Radius", "find_param finds by name")
	check(is_same(TetraSchema.find_param(op, "radius"), op["params"][2]), "find_param returns the parameter itself")
	check_eq(TetraSchema.find_param(op, "Radius"), {}, "find_param is case-sensitive")
	check_eq(TetraSchema.find_param({}, "radius"), {}, "find_param on an operation without params")


func _search_ops() -> Array:
	return [
		TetraSchema.make_operation("gegl:gaussian-blur", "Gaussian Blur", "Blur", "gegl", "Averages neighbouring pixels."),
		TetraSchema.make_operation("gegl:unsharp-mask", "Sharpen (Unsharp Mask)", "Enhance", "gegl", "Subtracts a blur to sharpen."),
		TetraSchema.make_operation("native:blur", "Blur", "Blur", "native", "Simple box filter."),
		TetraSchema.make_operation("gegl:motion-blur-linear", "Linear Motion Blur", "Blur", "gegl", "Blurs along a line."),
		TetraSchema.make_operation("gegl:box", "Box", "Blur", "gegl", "Averages a square."),
		TetraSchema.make_operation("mlt:blurry", "Soften", "Stylize", "mlt", "Softens the picture."),
		TetraSchema.make_operation("gegl:noise-hurl", "Hurl", "Noise", "gegl", "Randomises pixels."),
		TetraSchema.make_operation("gegl:preblurred", "Deblurred Glow", "Light", "gegl", "A glow."),
		"not an operation",
	]


func _ids(p_operations: Array) -> Array:
	return p_operations.map(func(operation: Dictionary) -> String: return operation["id"])


func _test_search() -> void:
	var ops := _search_ops()
	# Exact label, then word starts in the label (shorter labels first), a match
	# inside a word, the id, the category and finally the description.
	check_eq(_ids(TetraSchema.search(ops, "blur")),
			["native:blur", "gegl:gaussian-blur", "gegl:motion-blur-linear", "gegl:preblurred", "mlt:blurry", "gegl:box", "gegl:unsharp-mask"],
			"ranking for 'blur'")
	check_eq(_ids(TetraSchema.search(ops, "  BLUR ")), _ids(TetraSchema.search(ops, "blur")), "search ignores case and surrounding spaces")
	check_eq(_ids(TetraSchema.search(ops, "motion blur")), ["gegl:motion-blur-linear"], "every word must match")
	check_eq(_ids(TetraSchema.search(ops, "blur gauss")), ["gegl:gaussian-blur"], "words match in any order")
	check_eq(_ids(TetraSchema.search(ops, "gegl")),
			["gegl:box", "gegl:noise-hurl", "gegl:gaussian-blur", "gegl:preblurred", "gegl:motion-blur-linear", "gegl:unsharp-mask"],
			"matches in the id only are ordered by label length, then label")
	check_eq(_ids(TetraSchema.search(ops, "sharp")), ["gegl:unsharp-mask"], "a word start in the label")
	check_eq(_ids(TetraSchema.search(ops, "stylize")), ["mlt:blurry"], "search looks at the category")
	check_eq(_ids(TetraSchema.search(ops, "randomises")), ["gegl:noise-hurl"], "search looks at the description")
	check_eq(TetraSchema.search(ops, "zzz"), [], "no match")
	check_eq(TetraSchema.search(ops, "").size(), 8, "an empty text returns every operation")
	check_eq(_ids(TetraSchema.search(ops, "")), _ids(ops.slice(0, 8)), "an empty text keeps the given order")
	check_eq(TetraSchema.search_indices(ops, "blur"), PackedInt32Array([2, 0, 3, 7, 5, 4, 1]), "search_indices gives positions in the input")
	check(is_same(TetraSchema.search(ops, "hurl")[0], ops[6]), "search returns the operations themselves")


func _test_category_tree() -> void:
	var ops: Array = [
		TetraSchema.make_operation("b:subdivide", "Subdivide", "Mesh/Edges", "blender"),
		TetraSchema.make_operation("b:bevel", "Bevel", "Mesh / Edges", "blender"),
		TetraSchema.make_operation("b:extrude", "Extrude", "Mesh/Faces/", "blender"),
		TetraSchema.make_operation("b:join", "Join", "Mesh", "blender"),
		TetraSchema.make_operation("g:blur10", "Blur 10", "blur", "gegl"),
		TetraSchema.make_operation("g:blur2", "Blur 2", "blur", "gegl"),
		TetraSchema.make_operation("n:top", "Top Level", "", "native"),
		TetraSchema.make_operation("g:curves", "Curves", "Colors", "gegl"),
	]
	var tree := TetraSchema.category_tree(ops)
	check_eq(tree, {
		"name": "", "path": "", "count": 8, "operations": ["n:top"],
		"categories": [
			{"name": "blur", "path": "blur", "count": 2, "categories": [], "operations": ["g:blur2", "g:blur10"]},
			{"name": "Colors", "path": "Colors", "count": 1, "categories": [], "operations": ["g:curves"]},
			{"name": "Mesh", "path": "Mesh", "count": 4, "operations": ["b:join"], "categories": [
				{"name": "Edges", "path": "Mesh/Edges", "count": 2, "categories": [], "operations": ["b:bevel", "b:subdivide"]},
				{"name": "Faces", "path": "Mesh/Faces", "count": 1, "categories": [], "operations": ["b:extrude"]},
			]},
		],
	}, "category tree")
	check_eq(TetraSchema.category_tree([]), {"name": "", "path": "", "count": 0, "categories": [], "operations": []}, "empty category tree")
