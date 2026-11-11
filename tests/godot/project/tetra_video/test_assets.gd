extends "res://tetra_test.gd"
## Kdenlive's effect catalog as operation schemas, and the conversion of
## panel values to MLT properties.

const V := TetraVideoProject.TRACK_VIDEO


func find(operations: Array, id: String) -> Dictionary:
	for op in operations:
		if op.id == id:
			return op
	return {}


func param(op: Dictionary, name: String) -> Dictionary:
	for p in op.get("params", []):
		if p.name == name:
			return p
	return {}


func run() -> void:
	check(TetraVideoAssets.get_data_dir() != "", "Kdenlive's data directory was found")
	var all := TetraVideoAssets.get_effects(false)
	var available := TetraVideoAssets.get_effects(true)
	var kdenlive_all := all.filter(func(op): return op.source == "kdenlive")
	var kdenlive_available := available.filter(func(op): return op.source == "kdenlive")
	var mlt_only := available.filter(func(op): return op.source == "mlt")
	print("catalog: %d Kdenlive effects parsed, %d available here, %d more from MLT metadata, %d transitions" % [
			kdenlive_all.size(), kdenlive_available.size(), mlt_only.size(), TetraVideoAssets.get_transitions(false).size()])
	check(kdenlive_all.size() > 150, "more than 150 Kdenlive effects parsed: %d" % kdenlive_all.size())
	check(kdenlive_available.size() > 50, "more than 50 of them available on this machine: %d" % kdenlive_available.size())
	check(TetraVideoAssets.get_transitions(false).size() >= 8, "transitions parsed")
	check(TetraVideoAssets.get_transitions(true).size() >= 4, "transitions available")

	# Every operation follows the shared schema.
	var ids := {}
	var types := {}
	var problems := 0
	var has_validator := ClassDB.class_exists("TetraSchema")
	for op in all + TetraVideoAssets.get_transitions(false):
		var bad := ""
		for key in ["id", "label", "category", "source", "params", "service", "kind"]:
			if not op.has(key):
				bad = "missing " + key
		if ids.has(op.id):
			bad = "duplicate id"
		ids[op.id] = true
		if not (op.kind in ["video_effect", "audio_effect", "transition"]):
			bad = "kind " + str(op.kind)
		if op.label == "" or op.category == "":
			bad = "empty label or category"
		for p in op.params:
			types[p.type] = types.get(p.type, 0) + 1
			if not (p.type in ["bool", "int", "float", "string", "text", "enum", "color", "file"]):
				bad = "parameter type " + p.type
			if not p.has("default") or p.name == "" or p.label == "":
				bad = "parameter " + p.name + " incomplete"
			if p.type == "enum" and not p.options.any(func(o): return o.value == p.default):
				bad = "enum default not among the options of " + p.name
			if (p.has("min") and p.default < p.min) or (p.has("max") and p.default > p.max):
				bad = "default of %s outside %s..%s: %s" % [p.name, p.get("min"), p.get("max"), p.default]
		if bad == "" and has_validator:
			bad = ClassDB.class_call_static("TetraSchema", "validate_operation", op)
		if bad != "":
			problems += 1
			if problems <= 8:
				printerr("%s: %s" % [op.id, bad])
	check_eq(problems, 0, "operations that break the schema")
	check(has_validator, "validated with TetraSchema.validate_operation")
	for type in ["bool", "int", "float", "string", "enum", "color", "file"]:
		check(types.get(type, 0) > 0, "catalog has %s parameters" % type)

	# Brightness: default 1 with factor 100 is shown as 100 in 0..400.
	var brightness := TetraVideoAssets.get_operation("kdenlive:brightness")
	check_eq([brightness.label, brightness.service, brightness.kind, brightness.source], ["Intensity (Brightness)", "brightness", "video_effect", "kdenlive"], "brightness header")
	check_eq(brightness.category, "Color and Image Correction", "category from kdenliveeffectscategory.rc")
	var level := param(brightness, "level")
	check_eq([level.type, level.default, level.min, level.max, level.factor, level.get("animated")], ["int", 100, 0.0, 400.0, 100.0, true], "level in display units")
	check_eq(TetraVideoAssets.get_default_values(brightness), {"level": 100}, "default values")
	check_eq(TetraVideoAssets.values_to_mlt_properties(brightness, {"level": 50}), {"level": "0.5"}, "display 50 is MLT 0.5")
	check_eq(TetraVideoAssets.values_to_mlt_properties(brightness, {}), {"level": "1"}, "defaults are sent")

	# Kinds, categories, fixed parameters and lists.
	check_eq(TetraVideoAssets.get_operation("kdenlive:volume").kind, "audio_effect", "volume is an audio effect")
	var wipe := find(TetraVideoAssets.get_transitions(false), "kdenlive:wipe")
	check_eq([wipe.get("service"), wipe.get("kind"), wipe.get("category")], ["composite", "transition", "Transitions"], "wipe transition")
	check_eq(wipe.get("fixed"), {"aligned": "0", "fill": "1"}, "fixed parameters are kept aside")
	check_eq(param(wipe, "softness").get("factor"), 100.0, "softness factor")
	var revert := param(wipe, "geometry")
	check_eq([revert.get("type"), revert.get("default")], ["bool", false], "a switch is a bool")
	var wipe_props := TetraVideoAssets.values_to_mlt_properties(wipe, {"softness": 25, "luma_invert": true, "geometry": true})
	check_eq(wipe_props.get("softness"), "0.25", "softness converted")
	check_eq(wipe_props.get("luma_invert"), "1", "bool converted")
	check_eq(wipe_props.get("geometry"), "0=0% 0% 100% 100% 100%;-1=0% 0% 100% 100% 0%", "switch sends its 'on' string")
	check_eq([wipe_props.get("aligned"), wipe_props.get("fill")], ["0", "1"], "fixed values are always sent")
	check(not wipe_props.has("luma"), "an empty file parameter is not sent")

	var with_list := {}
	var with_color := {}
	var with_raw := {}
	for op in kdenlive_all:
		for p in op.params:
			if p.type == "enum" and with_list.is_empty() and p.options.size() > 1 and p.options[0].label != p.options[0].value:
				with_list = {"op": op, "param": p}
			if p.type == "color" and with_color.is_empty():
				with_color = {"op": op, "param": p}
			if p.get("raw", false) and with_raw.is_empty() and p.default != "":
				with_raw = {"op": op, "param": p}
	if check(not with_list.is_empty(), "a list parameter with display names exists"):
		var p: Dictionary = with_list.param
		var props := TetraVideoAssets.values_to_mlt_properties(with_list.op, {p.name: p.options[1].value})
		check_eq(props[p.name], str(p.options[1].value), "an enum sends the option's value")
	if check(not with_color.is_empty(), "a colour parameter exists"):
		var p: Dictionary = with_color.param
		check(p.default is Color, "colour default is a Color")
		var sent: String = TetraVideoAssets.values_to_mlt_properties(with_color.op, {p.name: Color(1, 0, 0, 1)})[p.name]
		check(sent in ["0xff0000ff", "#ff0000", "#ffff0000"], "colour sent in MLT notation: " + sent)
	if check(not with_raw.is_empty(), "a complex parameter is kept as a raw string"):
		var p: Dictionary = with_raw.param
		check_eq(p.type, "string", "raw parameters are strings")
		check_eq(TetraVideoAssets.values_to_mlt_properties(with_raw.op, {})[p.name], p.default, "the raw default is sent")

	# Placeholders are resolved against the profile.
	var resolved := 0
	for op in TetraVideoAssets.get_effects(false, 1280, 720):
		for p in op.params:
			if str(p.default).contains("%width") or str(p.default).contains("%height") or str(p.get("max", "")).contains("%"):
				resolved -= 1000
			if p.get("max", 0.0) == 1280.0 or str(p.default).contains("1280"):
				resolved += 1
	check(resolved > 0, "profile placeholders became numbers (%d uses of the width)" % resolved)

	# MLT filters Kdenlive does not describe come from MLT's metadata.
	check(mlt_only.size() > 10, "MLT-only filters are offered: %d" % mlt_only.size())
	var extra: Dictionary = mlt_only[0]
	check(extra.id.begins_with("mlt:") and TetraMltEngine.has_service("filter", extra.service), "MLT-only entry: " + extra.id)
	for op in available:
		if not TetraMltEngine.has_service("filter", op.service):
			check(false, "listed as available but MLT has no filter " + op.service)
			break

	# The catalog drives the XML writer: a Kdenlive effect with display values renders correctly.
	var project := TetraVideoProject.new()
	project.set_profile(320, 180, 25, 1, 48000)
	project.add_track(V)
	var red := project.add_bin_color("Red", Color(1, 0, 0), 25)
	var clip := project.insert_clip(red, 0, 0)
	project.add_clip_effect(clip, "kdenlive:brightness", "brightness", {"level": 50})
	var xml := project.to_mlt_xml()
	check(xml.contains("<property name=\"level\">0.5</property>"), "the XML carries the MLT value")
	check(xml.contains("<property name=\"tetra:operation\">kdenlive:brightness</property>"), "the XML remembers the operation")
	var engine := TetraMltEngine.new()
	engine.set_project(project)
	var half := engine.render_frame(10).get_pixel(160, 90)
	check(half.r > 0.4 and half.r < 0.56 and half.g < 0.03, "display value 50 halves the red: %s" % half)
	var invert := TetraVideoAssets.get_operation("kdenlive:invert")
	if check(not invert.is_empty(), "invert effect in the catalog"):
		project.remove_clip_effect(clip, 0)
		project.add_clip_effect(clip, invert.id, invert.service, TetraVideoAssets.get_default_values(invert))
		engine.set_project(project)
		var cyan := engine.render_frame(10).get_pixel(160, 90)
		check(cyan.r < 0.05 and cyan.g > 0.95 and cyan.b > 0.95, "Kdenlive's invert with default values: %s" % cyan)
	var reloaded := TetraVideoProject.new()
	check_eq(reloaded.load_from_xml(project.to_mlt_xml()), OK, "reload")
	check_eq(reloaded.get_state(), project.get_state(), "effect values survive the file as display values")

	if ClassDB.class_exists("TetraEngineRegistry"):
		var entry: Dictionary = ClassDB.class_call_static("TetraEngineRegistry", "get_engine", "MLT")
		print("engine registry: ", entry)
		check(entry.get("available", false) and str(entry.get("details", "")).contains("Kdenlive effects"), "MLT is announced to the engine registry")
