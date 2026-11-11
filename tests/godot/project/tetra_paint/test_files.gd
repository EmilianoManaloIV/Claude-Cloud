extends "res://tetra_test.gd"
## Loading, OpenRaster round trips and export of TetraImageDocument.


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 1e-6) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


## A linear colour that an 8-bit sRGB file stores exactly.
func c8(p_r: int, p_g: int, p_b: int, p_a: int = 255) -> Color:
	var c := Color8(p_r, p_g, p_b, 255).srgb_to_linear()
	c.a = p_a / 255.0
	return c


func images_close(p_a: Image, p_b: Image, p_epsilon: float) -> bool:
	if p_a.get_size() != p_b.get_size():
		return false
	for y in p_a.get_height():
		for x in p_a.get_width():
			var a := p_a.get_pixel(x, y)
			var b := p_b.get_pixel(x, y)
			if absf(a.r - b.r) > p_epsilon or absf(a.g - b.g) > p_epsilon or absf(a.b - b.b) > p_epsilon or absf(a.a - b.a) > p_epsilon:
				return false
	return true


func build_document() -> TetraImageDocument:
	var doc := TetraImageDocument.new()
	doc.create(16, 12, c8(200, 180, 40))
	var background := doc.get_layer_ids()[0]
	doc.layer_set_name(background, "Paper")

	var shade := doc.add_layer("Shade <&\"'> é")
	doc.select_rect(Rect2i(3, 2, 5, 4))
	doc.fill(shade, c8(30, 90, 220, 128))
	doc.select_none()
	doc.layer_set_mode(shade, TetraImageDocument.LAYER_MODE_MULTIPLY)
	doc.layer_set_opacity(shade, 0.5)
	doc.layer_set_lock_alpha(shade, true)

	var hidden := doc.add_layer("Hidden")
	doc.fill(hidden, c8(255, 0, 0))
	doc.layer_set_visible(hidden, false)
	doc.layer_set_mode(hidden, TetraImageDocument.LAYER_MODE_SCREEN)

	var grain := doc.add_layer("Grain")
	doc.select_rect(Rect2i(10, 7, 6, 5))
	doc.fill(grain, c8(120, 130, 140))
	doc.select_none()
	doc.layer_set_mode(grain, TetraImageDocument.LAYER_MODE_GRAIN_MERGE)
	doc.layer_set_opacity(grain, 0.25)

	doc.add_layer("Empty")
	doc.set_active_layer(shade)
	return doc


func run() -> void:
	test_ora_round_trip()
	test_ora_archive()
	test_foreign_ora()
	test_ora_as_image()
	test_load_raster()
	test_export()


func test_ora_round_trip() -> void:
	var doc := build_document()
	var path := tmp_dir().path_join("roundtrip.ora")
	check_eq(doc.save_ora(path), OK, "save_ora")

	var loaded := TetraImageDocument.new()
	loaded.create(3, 3)
	loaded.select_all()
	var sizes := [0]
	loaded.size_changed.connect(func() -> void: sizes[0] += 1)
	check_eq(loaded.load(path), OK, "load ora")
	check_eq(sizes[0], 1, "load reports the new size")
	check_eq(loaded.get_size(), Vector2i(16, 12), "size")
	check(not loaded.can_undo(), "loading starts a fresh history")
	check(loaded.is_selection_empty(), "loading clears the selection")
	check_eq(loaded.get_layer_count(), 5, "layer count")

	var original_ids := doc.get_layer_ids()
	var loaded_ids := loaded.get_layer_ids()
	for i in original_ids.size():
		var a := original_ids[i]
		var b := loaded_ids[i]
		var what := "layer %d (%s)" % [i, doc.layer_get_name(a)]
		check_eq(loaded.layer_get_name(b), doc.layer_get_name(a), what + " name")
		check_eq(loaded.layer_get_mode(b), doc.layer_get_mode(a), what + " mode")
		check_near(loaded.layer_get_opacity(b), doc.layer_get_opacity(a), 1e-6, what + " opacity")
		check_eq(loaded.layer_is_visible(b), doc.layer_is_visible(a), what + " visibility")
		check_eq(loaded.layer_get_lock_alpha(b), doc.layer_get_lock_alpha(a), what + " lock alpha")
		check(images_close(loaded.layer_get_image(b), doc.layer_get_image(a), 1e-6), what + " pixels")
	check_eq(loaded.get_layer_index(loaded.get_active_layer()), 1, "the active layer is restored")
	check_eq(loaded.composite().get_data(), doc.composite().get_data(), "the composite is identical after the round trip")

	# Every mode survives, also those OpenRaster has no name for.
	var modes := TetraImageDocument.new()
	modes.create(2, 2)
	for mode in TetraImageDocument.LAYER_MODE_MAX:
		modes.layer_set_mode(modes.add_layer("m%d" % mode), mode)
	var modes_path := tmp_dir().path_join("modes.ora")
	check_eq(modes.save_ora(modes_path), OK, "save all modes")
	var modes_loaded := TetraImageDocument.new()
	check_eq(modes_loaded.load(modes_path), OK, "load all modes")
	var all_modes := true
	var ids := modes_loaded.get_layer_ids()
	for mode in TetraImageDocument.LAYER_MODE_MAX:
		all_modes = all_modes and modes_loaded.layer_get_mode(ids[mode + 1]) == mode
	check(all_modes, "all %d layer modes round-trip through composite-op" % TetraImageDocument.LAYER_MODE_MAX)

	# A file that cannot be read leaves the document alone.
	check(loaded.load(tmp_dir().path_join("missing.ora")) != OK, "a missing file fails to load")
	var broken := FileAccess.open(tmp_dir().path_join("broken.ora"), FileAccess.WRITE)
	broken.store_string("this is not a zip archive")
	broken.close()
	check(loaded.load(tmp_dir().path_join("broken.ora")) != OK, "a broken file fails to load")
	check_eq(loaded.get_layer_count(), 5, "a failed load keeps the document")


func test_ora_archive() -> void:
	var doc := build_document()
	var path := tmp_dir().path_join("archive.ora")
	doc.save_ora(path)

	# The specification wants "mimetype" as the first member, stored, without extra field.
	var bytes := FileAccess.get_file_as_bytes(path)
	check_eq(bytes.slice(0, 4), PackedByteArray([0x50, 0x4b, 0x03, 0x04]), "zip local header")
	check_eq(bytes.decode_u16(8), 0, "mimetype is stored uncompressed")
	check_eq(bytes.decode_u16(26), 8, "first member name length")
	check_eq(bytes.decode_u16(28), 0, "no extra field before the mimetype")
	check_eq(bytes.slice(30, 38).get_string_from_ascii(), "mimetype", "first member is the mimetype")
	check_eq(bytes.slice(38, 54).get_string_from_ascii(), "image/openraster", "mimetype content")

	var zip := ZIPReader.new()
	check_eq(zip.open(path), OK, "the archive opens with a zip reader")
	var files := zip.get_files()
	check_eq(files[0], "mimetype", "member order")
	for member: String in ["stack.xml", "mergedimage.png", "Thumbnails/thumbnail.png", "data/layer000.png", "data/layer004.png"]:
		check(files.has(member), "archive has %s" % member)
	var xml := zip.read_file("stack.xml").get_string_from_utf8()
	check(xml.contains("<image version=\"0.0.3\" w=\"16\" h=\"12\""), "stack.xml image element")
	check(xml.contains("composite-op=\"svg:multiply\""), "stack.xml composite-op")
	check(xml.contains("visibility=\"hidden\""), "stack.xml visibility")
	check(xml.contains("name=\"Shade &lt;&amp;&quot;&apos;&gt; é\""), "stack.xml escapes names")
	check(xml.find("name=\"Empty\"") < xml.find("name=\"Paper\""), "stack.xml lists the top layer first")
	# Layers are stored cropped to their content with their position.
	check(xml.contains("src=\"data/layer001.png\" x=\"3\" y=\"2\""), "stack.xml layer offset")
	var layer_png := Image.new()
	check_eq(layer_png.load_png_from_buffer(zip.read_file("data/layer001.png")), OK, "layer PNG decodes")
	check_eq(layer_png.get_size(), Vector2i(5, 4), "layer PNG is cropped to its content")
	check_eq(layer_png.get_pixel(0, 0), Color8(30, 90, 220, 128), "layer PNG holds straight-alpha sRGB")
	var merged := Image.new()
	check_eq(merged.load_png_from_buffer(zip.read_file("mergedimage.png")), OK, "merged image decodes")
	check_eq(merged.get_data(), doc.composite().get_data(), "merged image is the composite")
	var thumbnail := Image.new()
	thumbnail.load_png_from_buffer(zip.read_file("Thumbnails/thumbnail.png"))
	check_eq(thumbnail.get_size(), Vector2i(16, 12), "a small image is its own thumbnail")
	zip.close()

	var wide := TetraImageDocument.new()
	wide.create(600, 300, c8(10, 200, 30))
	var wide_path := tmp_dir().path_join("wide.ora")
	wide.save_ora(wide_path)
	zip.open(wide_path)
	thumbnail.load_png_from_buffer(zip.read_file("Thumbnails/thumbnail.png"))
	check_eq(thumbnail.get_size(), Vector2i(256, 128), "thumbnails are at most 256 pixels")
	check_eq(thumbnail.get_pixel(100, 50), Color8(10, 200, 30, 255), "thumbnail content")
	zip.close()


func png_bytes(p_width: int, p_height: int, p_color: Color) -> PackedByteArray:
	var image := Image.create_empty(p_width, p_height, false, Image.FORMAT_RGBA8)
	image.fill(p_color)
	return image.save_png_to_buffer()


func test_foreign_ora() -> void:
	# An archive the way other programs write it: compressed members, a
	# group, offsets, an unknown composite-op, no merged image.
	var path := tmp_dir().path_join("foreign.ora")
	var zip := ZIPPacker.new()
	zip.open(path)
	zip.start_file("mimetype")
	zip.write_file("image/openraster".to_utf8_buffer())
	zip.close_file()
	zip.start_file("stack.xml")
	zip.write_file("""<?xml version='1.0' encoding='UTF-8'?>
<image w="8" h="6" xres="300" yres="300">
  <stack>
    <layer name="top" src="data/top.png" x="5" y="4" opacity="0.5" visibility="visible" composite-op="svg:screen" />
    <stack name="group" opacity="0.5" visibility="visible">
      <layer name="in group" src="data/dot.png" x="-1" y="-1" opacity="0.5" composite-op="krita:unknown"/>
      <layer name="hidden" src="data/dot.png" x="0" y="0" visibility="hidden" composite-op="svg:multiply"/>
    </stack>
    <layer name="background" src="data/bg.png"/>
  </stack>
</image>
""".to_utf8_buffer())
	zip.close_file()
	for entry: Array in [["data/bg.png", png_bytes(8, 6, Color8(255, 255, 255, 255))], ["data/top.png", png_bytes(4, 4, Color8(128, 0, 0, 255))], ["data/dot.png", png_bytes(2, 2, Color8(0, 0, 255, 255))]]:
		zip.start_file(entry[0])
		zip.write_file(entry[1])
		zip.close_file()
	zip.close()

	var doc := TetraImageDocument.new()
	check_eq(doc.load(path), OK, "load a foreign archive")
	check_eq(doc.get_size(), Vector2i(8, 6), "foreign size")
	var ids := doc.get_layer_ids()
	check_eq(ids.size(), 4, "groups are flattened into their layers")
	var names: Array[String] = []
	for id in ids:
		names.append(doc.layer_get_name(id))
	check_eq(names, ["background", "hidden", "in group", "top"] as Array[String], "bottom to top order")
	check_eq(doc.layer_get_mode(ids[3]), TetraImageDocument.LAYER_MODE_SCREEN, "svg:screen")
	check_eq(doc.layer_get_mode(ids[2]), TetraImageDocument.LAYER_MODE_NORMAL, "an unknown composite-op is Normal")
	check_eq(doc.layer_get_mode(ids[1]), TetraImageDocument.LAYER_MODE_MULTIPLY, "svg:multiply")
	check_near(doc.layer_get_opacity(ids[3]), 0.5, 1e-6, "layer opacity")
	check_near(doc.layer_get_opacity(ids[2]), 0.25, 1e-6, "group opacity is folded into the layer")
	check_near(doc.layer_get_opacity(ids[0]), 1.0, 1e-6, "missing opacity is 1")
	check_eq(doc.layer_is_visible(ids[1]), false, "hidden layer")
	check_eq(doc.layer_is_visible(ids[0]), true, "missing visibility is visible")
	# Offsets: "top" covers (5..7, 4..5) after clipping, the dot only (0, 0).
	var top := doc.layer_get_image(ids[3])
	check_color(top.get_pixel(5, 4), Color(0.2158605, 0, 0, 1), "offset layer content is linearised", 1e-5)
	check_color(top.get_pixel(7, 5), Color(0.2158605, 0, 0, 1), "offset layer is clipped to the canvas", 1e-5)
	check_color(top.get_pixel(4, 4), Color(0, 0, 0, 0), "left of the offset layer")
	var dot := doc.layer_get_image(ids[2])
	check_color(dot.get_pixel(0, 0), Color(0, 0, 1, 1), "negative offsets are clipped")
	check_color(dot.get_pixel(1, 0), Color(0, 0, 0, 0), "only one pixel of the dot is on the canvas")
	check_eq(doc.get_active_layer(), ids[3], "without a selected layer the top one is active")

	# Without a merged image the loader composites the layers itself.
	var image := Image.load_from_file(path)
	check(image != null, "Image.load_from_file reads an archive without mergedimage.png")
	check_eq(image.get_data(), doc.composite().get_data(), "the image is the composite")


func test_ora_as_image() -> void:
	var doc := build_document()
	var path := tmp_dir().path_join("texture.ora")
	doc.save_ora(path)
	var image := Image.load_from_file(path)
	check(image != null, "Image.load_from_file handles .ora")
	check_eq(image.get_size(), Vector2i(16, 12), "image size")
	check_eq(image.get_data(), doc.composite().get_data(), "the image is the merged picture")
	var texture := ImageTexture.create_from_image(image)
	check_eq(texture.get_size(), Vector2(16, 12), "the image makes a texture")


func test_load_raster() -> void:
	var png_path := tmp_dir().path_join("Photo One.png")
	var png := Image.create_empty(5, 3, false, Image.FORMAT_RGBA8)
	png.fill(Color8(128, 255, 0, 255))
	png.set_pixel(4, 2, Color8(0, 0, 0, 0))
	png.save_png(png_path)
	var doc := TetraImageDocument.new()
	check_eq(doc.load(png_path), OK, "load png")
	check_eq(doc.get_size(), Vector2i(5, 3), "png size")
	check_eq(doc.get_layer_count(), 1, "one layer")
	var layer := doc.get_active_layer()
	check_eq(doc.layer_get_name(layer), "Photo One", "the layer is named after the file")
	check_color(doc.layer_get_image(layer).get_pixel(1, 1), Color(0.2158605, 1, 0, 1), "8-bit files are linearised", 1e-5)
	check_near(doc.layer_get_image(layer).get_pixel(4, 2).a, 0.0, 0.0, "alpha is kept")
	check_eq(doc.composite().get_data(), png.get_data(), "displaying the document gives the file's pixels back")

	# Float files stay linear, including values above 1.
	var exr_path := tmp_dir().path_join("light.exr")
	var exr := Image.create_empty(4, 4, false, Image.FORMAT_RGBAF)
	exr.fill(Color(0.5, 2.0, 0.125, 1.0))
	check_eq(exr.save_exr(exr_path), OK, "write a test EXR")
	check_eq(doc.load(exr_path), OK, "load exr")
	check_color(doc.layer_get_image(doc.get_active_layer()).get_pixel(2, 2), Color(0.5, 2.0, 0.125, 1.0), "EXR values are kept linear", 1e-3)
	check_eq(doc.layer_get_name(doc.get_active_layer()), "light", "exr layer name")

	var jpg_path := tmp_dir().path_join("photo.jpg")
	var jpg := Image.create_empty(8, 8, false, Image.FORMAT_RGB8)
	jpg.fill(Color8(128, 128, 128))
	jpg.save_jpg(jpg_path, 0.95)
	check_eq(doc.load(jpg_path), OK, "load jpg")
	check_color(doc.layer_get_image(doc.get_active_layer()).get_pixel(3, 3), Color(0.2158605, 0.2158605, 0.2158605, 1), "jpg is linearised and opaque", 0.01)
	check(doc.load(tmp_dir().path_join("missing.png")) != OK, "a missing image fails to load")
	check_eq(doc.get_size(), Vector2i(8, 8), "a failed load keeps the document")


func test_export() -> void:
	var doc := build_document()
	var composite := doc.composite()

	var png_path := tmp_dir().path_join("export.png")
	check_eq(doc.export_image(png_path), OK, "export png")
	check_eq(Image.load_from_file(png_path).get_data(), composite.get_data(), "the PNG is the composite")

	var webp_path := tmp_dir().path_join("export.webp")
	check_eq(doc.export_image(webp_path, 1.0), OK, "export lossless webp")
	var webp := Image.load_from_file(webp_path)
	webp.convert(Image.FORMAT_RGBA8)
	check_eq(webp.get_data(), composite.get_data(), "lossless WebP is the composite")
	check_eq(doc.export_image(tmp_dir().path_join("lossy.webp"), 0.5), OK, "export lossy webp")
	check_eq(Image.load_from_file(tmp_dir().path_join("lossy.webp")).get_size(), Vector2i(16, 12), "lossy WebP size")

	# JPEG is flattened onto white.
	var clear := TetraImageDocument.new()
	clear.create(16, 16, Color(0, 0, 0, 0))
	clear.select_rect(Rect2i(0, 0, 8, 16))
	clear.fill(clear.get_active_layer(), c8(200, 30, 30))
	var jpg_path := tmp_dir().path_join("export.jpg")
	check_eq(clear.export_image(jpg_path, 0.95), OK, "export jpg")
	var jpg := Image.load_from_file(jpg_path)
	check_eq(jpg.get_size(), Vector2i(16, 16), "JPEG size")
	var solid := jpg.get_pixel(2, 8)
	var empty := jpg.get_pixel(13, 8)
	check(absf(solid.r - 200 / 255.0) < 0.04 and absf(solid.g - 30 / 255.0) < 0.04, "JPEG colour: %s" % solid)
	check(empty.r > 0.96 and empty.g > 0.96 and empty.b > 0.96, "transparent areas export as white: %s" % empty)

	# EXR keeps linear floats, also above 1.
	var hdr := TetraImageDocument.new()
	hdr.create(4, 4, Color(0.25, 3.0, 0.5, 1.0))
	var exr_path := tmp_dir().path_join("export.exr")
	check_eq(hdr.export_image(exr_path), OK, "export exr")
	var exr := Image.load_from_file(exr_path)
	exr.convert(Image.FORMAT_RGBAF)
	check_color(exr.get_pixel(1, 1), Color(0.25, 3.0, 0.5, 1.0), "EXR keeps linear values", 1e-3)

	check_eq(doc.export_image(tmp_dir().path_join("export.bmp")), ERR_FILE_UNRECOGNIZED, "unknown export format")
