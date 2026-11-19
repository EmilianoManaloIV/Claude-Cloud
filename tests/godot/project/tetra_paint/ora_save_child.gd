extends SceneTree
## Helper of test_files.gd: saves a 256 x 256 noise image as the .ora file named
## after "--", and prints the result. test_files.gd runs it with a file size
## limit, which is how a full disk looks to the process.


func _init() -> void:
	var noise := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in 256:
		for x in 256:
			noise.set_pixel(x, y, Color8((x * 37 + y * 101) % 256, (x * 59 + y * 13) % 256, (x * y) % 256))
	var doc := TetraImageDocument.new()
	doc.create(256, 256)
	doc.layer_set_image(doc.get_active_layer(), noise)
	print("save_ora=%d" % doc.save_ora(OS.get_cmdline_user_args()[0]))
	quit()
