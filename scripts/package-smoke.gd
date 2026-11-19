# Smoke test for a packaged Tetra Studio: run through the archive's launcher,
#   tetra-studio --headless --script scripts/package-smoke.gd
# It checks that the bundled engines are found and prints where from. The
# last line is "SMOKE OK" or "SMOKE FAILED: <what>".
extends SceneTree


func _init() -> void:
	var problems: PackedStringArray = []

	var gegl_operations: int = TetraGegl.list_operations().size() if TetraGegl.is_available() else 0
	print("gegl: available=%s version=%s operations=%d" % [TetraGegl.is_available(), TetraGegl.get_version(), gegl_operations])
	if gegl_operations < 100:
		problems.append("GEGL filters missing")

	var producers: PackedStringArray = TetraMltEngine.list_services("producer") if TetraMltEngine.is_available() else PackedStringArray()
	var filters: PackedStringArray = TetraMltEngine.list_services("filter") if TetraMltEngine.is_available() else PackedStringArray()
	var frei0r := 0
	for filter in filters:
		if filter.begins_with("frei0r."):
			frei0r += 1
	print("mlt: available=%s version=%s producers=%d filters=%d frei0r=%d" % [TetraMltEngine.is_available(), TetraMltEngine.get_version(), producers.size(), filters.size(), frei0r])
	if not TetraMltEngine.is_available():
		problems.append("MLT engine unavailable")
	if not producers.has("avformat"):
		problems.append("MLT avformat (ffmpeg) module missing")
	if frei0r == 0:
		problems.append("frei0r plug-ins missing")

	var melt: String = TetraVideoRenderer.find_melt()
	print("melt: ", melt)
	if melt.is_empty():
		problems.append("melt not found")

	var data_dir: String = TetraVideoAssets.get_data_dir()
	print("kdenlive data: ", data_dir, " effects=", TetraVideoAssets.get_effects(true).size())
	if data_dir.is_empty():
		problems.append("Kdenlive catalogs not found")

	var launcher: Dictionary = TetraBlenderBridge.find_launcher()
	print("blender: ", launcher.get("description", ""))
	if OS.get_environment("TETRA_SMOKE_SKIP_BLENDER") != "1":
		if String(launcher.get("kind", "")).is_empty():
			problems.append("Blender not found")
		else:
			var bridge: TetraBlenderBridge = TetraBlenderBridge.new()
			if bridge.start() == OK:
				print("blender: started, version ", bridge.get_blender_version())
				bridge.stop()
			else:
				problems.append("Blender did not start: " + bridge.get_last_error())

	if problems.is_empty():
		print("SMOKE OK")
		quit(0)
	else:
		print("SMOKE FAILED: ", ", ".join(problems))
		quit(1)
