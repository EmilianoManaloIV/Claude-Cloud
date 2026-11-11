extends SceneTree
## Captures the editor window to a PNG. Run by scripts/screenshot.sh.
##
## User arguments: --out=<png> [--screen=<main screen name>] [--wait=<seconds>]
## [--driver=<res:// path of a script>]. A driver script extends RefCounted and
## implements `func drive(p_tree: SceneTree) -> void` (it may await); it runs
## after the main screen has been switched and before the capture.


func _initialize() -> void:
	_main()


func _main() -> void:
	var options := {"out": "user://editor.png", "screen": "", "wait": "2", "driver": ""}
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--") and argument.contains("="):
			options[argument.get_slice("=", 0).trim_prefix("--")] = argument.get_slice("=", 1)

	# Let the editor finish its first scan and layout.
	await create_timer(3.0).timeout
	if options["screen"] != "":
		EditorInterface.set_main_screen_editor(options["screen"])
		await create_timer(0.5).timeout
	if options["driver"] != "":
		var driver: Variant = load(options["driver"]).new()
		await driver.drive(self)
	await create_timer(float(options["wait"])).timeout

	var image := root.get_texture().get_image()
	var error := image.save_png(options["out"])
	print("screenshot %s (%dx%d): %s" % [options["out"], image.get_width(), image.get_height(), error_string(error)])
	quit(0 if error == OK else 1)
