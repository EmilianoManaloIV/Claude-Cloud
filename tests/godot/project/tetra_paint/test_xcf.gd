extends "res://tetra_test.gd"
## XCF import: the real files of the checkout, files written here by an
## independent minimal XCF writer (exact pixels), and damaged files.

const MODE_NORMAL := TetraImageDocument.LAYER_MODE_NORMAL

# Property numbers (app/xcf/xcf-private.h).
const PROP_COLORMAP := 1
const PROP_ACTIVE_LAYER := 2
const PROP_SELECTION := 4
const PROP_FLOATING_SELECTION := 5
const PROP_OPACITY := 6
const PROP_MODE := 7
const PROP_VISIBLE := 8
const PROP_LOCK_ALPHA := 10
const PROP_APPLY_MASK := 11
const PROP_OFFSETS := 15
const PROP_COMPRESSION := 17
const PROP_GROUP_ITEM := 29
const PROP_ITEM_PATH := 30
const PROP_FLOAT_OPACITY := 33

const RGB := 0
const RGBA := 1
const GRAY := 2
const GRAYA := 3
const INDEXED := 4
const INDEXEDA := 5

const NONE := 0
const RLE := 1
const ZLIB := 2


# --- A minimal XCF writer -------------------------------------------------

## Writes one XCF file. `image` keys: version, width, height, base (0 RGB,
## 1 grey, 2 indexed), precision (GimpPrecision), compression, colormap
## (PackedByteArray, optional), layers (Array, top first), channels (Array),
## extra_props (PackedByteArray appended to the image properties).
## A layer: name, width, height, type, props (PackedByteArray), data (the
## pixel bytes in file order, or null for an empty level), mask (a channel),
## floating_on (index of the layer it floats on), raw_tiles (Array of
## ready-made tile payloads, instead of data).
## A channel: name, width, height, props, data (one sample per pixel).
class XcfWriter:
	var out := StreamPeerBuffer.new()
	var version := 0
	var wide := false
	var compression := 0
	var sample_bytes := 1
	var layer_positions: Array[int] = []

	func offset(p_value: int) -> void:
		if wide:
			out.put_u64(p_value)
		else:
			out.put_u32(p_value)

	func offset_size() -> int:
		return 8 if wide else 4

	func patch(p_at: int, p_value: int) -> void:
		var here := out.get_position()
		out.seek(p_at)
		offset(p_value)
		out.seek(here)

	func string(p_text: String) -> void:
		var bytes := p_text.to_utf8_buffer()
		out.put_u32(bytes.size() + 1)
		out.put_data(bytes)
		out.put_u8(0)

	func write(p_image: Dictionary) -> PackedByteArray:
		out.big_endian = true
		version = p_image.version
		wide = version >= 11
		compression = p_image.get("compression", 0)
		var precision: int = p_image.get("precision", 150)
		sample_bytes = 1 if precision < 200 else (2 if precision < 300 else (4 if precision < 500 else (2 if precision < 600 else (4 if precision < 700 else 8))))
		if version == 0:
			out.put_data("gimp xcf file".to_ascii_buffer())
		else:
			out.put_data(("gimp xcf v%03d" % version).to_ascii_buffer())
		out.put_u8(0)
		out.put_u32(p_image.width)
		out.put_u32(p_image.height)
		out.put_u32(p_image.get("base", 0))
		if version >= 4:
			out.put_u32(p_image.get("stored_precision", precision))
		if p_image.has("colormap"):
			var colors: PackedByteArray = p_image.colormap
			out.put_u32(PROP_COLORMAP)
			if version == 0:
				# Version 0 wrote the count and then only one byte per colour.
				out.put_u32(4 + colors.size())
				out.put_u32(colors.size())
			else:
				out.put_u32(4 + colors.size())
				out.put_u32(colors.size() / 3)
			out.put_data(colors)
		if compression != 0 or p_image.get("write_compression", false):
			out.put_u32(PROP_COMPRESSION)
			out.put_u32(1)
			out.put_u8(compression)
		if p_image.has("extra_props"):
			out.put_data(p_image.extra_props)
		out.put_u32(0)
		out.put_u32(0)

		var layers: Array = p_image.get("layers", [])
		var channels: Array = p_image.get("channels", [])
		var layer_table := out.get_position()
		for i in layers.size() + 1:
			offset(0)
		var channel_table := out.get_position()
		for i in channels.size() + 1:
			offset(0)
		if version >= 18:
			offset(0)

		var floating_patches := {}
		for i in layers.size():
			var layer: Dictionary = layers[i]
			layer_positions.append(out.get_position())
			patch(layer_table + i * offset_size(), out.get_position())
			out.put_u32(layer.width)
			out.put_u32(layer.height)
			out.put_u32(layer.type)
			string(layer.name)
			if layer.has("floating_on"):
				out.put_u32(PROP_FLOATING_SELECTION)
				out.put_u32(offset_size())
				floating_patches[out.get_position()] = layer.floating_on
				offset(0)
			out.put_data(layer.get("props", PackedByteArray()))
			out.put_u32(0)
			out.put_u32(0)
			var pointers := out.get_position()
			offset(0)
			offset(0)
			if version >= 20:
				offset(0)
			var components: int = [3, 4, 1, 2, 1, 2][mini(layer.type, 5)]
			patch(pointers, out.get_position())
			hierarchy(layer.width, layer.height, components * sample_bytes, layer.get("data"), layer.get("raw_tiles"))
			if layer.has("mask"):
				patch(pointers + offset_size(), out.get_position())
				channel(layer.mask)
		for at: int in floating_patches:
			patch(at, layer_positions[floating_patches[at]])
		for i in channels.size():
			patch(channel_table + i * offset_size(), out.get_position())
			channel(channels[i])
		return out.data_array

	func channel(p_channel: Dictionary) -> void:
		out.put_u32(p_channel.width)
		out.put_u32(p_channel.height)
		string(p_channel.get("name", "channel"))
		out.put_data(p_channel.get("props", PackedByteArray()))
		out.put_u32(0)
		out.put_u32(0)
		var pointer := out.get_position()
		offset(0)
		if version >= 26:
			offset(0)
		patch(pointer, out.get_position())
		hierarchy(p_channel.width, p_channel.height, sample_bytes, p_channel.get("data"), null)

	func hierarchy(p_width: int, p_height: int, p_bpp: int, p_data: Variant, p_raw_tiles: Variant) -> void:
		out.put_u32(p_width)
		out.put_u32(p_height)
		out.put_u32(p_bpp)
		var level_pointer := out.get_position()
		offset(0)
		# One dummy level, as GIMP writes for anything larger than a tile.
		var dummy_pointer := out.get_position()
		offset(0)
		offset(0)
		patch(level_pointer, out.get_position())
		out.put_u32(p_width)
		out.put_u32(p_height)
		var columns := (p_width + 63) / 64
		var rows := (p_height + 63) / 64
		if p_data == null and p_raw_tiles == null:
			offset(0)
		else:
			var table := out.get_position()
			for i in columns * rows + 1:
				offset(0)
			for ty in rows:
				for tx in columns:
					var index := ty * columns + tx
					patch(table + index * offset_size(), out.get_position())
					if p_raw_tiles != null:
						out.put_data(p_raw_tiles[index])
						continue
					var tile_width := mini(64, p_width - tx * 64)
					var tile_height := mini(64, p_height - ty * 64)
					var tile := PackedByteArray()
					for y in tile_height:
						var start := ((ty * 64 + y) * p_width + tx * 64) * p_bpp
						tile.append_array((p_data as PackedByteArray).slice(start, start + tile_width * p_bpp))
					out.put_data(encode_tile(tile, p_bpp))
		patch(dummy_pointer, out.get_position())
		out.put_u32(p_width / 2)
		out.put_u32(p_height / 2)
		out.put_u32(0)

	func encode_tile(p_tile: PackedByteArray, p_bpp: int) -> PackedByteArray:
		match compression:
			1:
				return rle(p_tile, p_bpp)
			2:
				return p_tile.compress(FileAccess.COMPRESSION_DEFLATE)
		return p_tile

	## Byte planes as packets. Runs of 3 or more become repeat packets, the
	## rest literals; both use the 16-bit length form from 128 bytes on.
	static func rle(p_tile: PackedByteArray, p_bpp: int) -> PackedByteArray:
		var packed := PackedByteArray()
		var count := p_tile.size() / p_bpp
		for plane in p_bpp:
			var bytes := PackedByteArray()
			for i in count:
				bytes.append(p_tile[i * p_bpp + plane])
			var i := 0
			var literal := PackedByteArray()
			while i < count:
				var run := 1
				while i + run < count and bytes[i + run] == bytes[i] and run < 60000:
					run += 1
				if run >= 3:
					flush_literal(packed, literal)
					literal = PackedByteArray()
					if run >= 128:
						packed.append_array([127, run >> 8, run & 255, bytes[i]])
					else:
						packed.append_array([run - 1, bytes[i]])
					i += run
				else:
					literal.append(bytes[i])
					i += 1
			flush_literal(packed, literal)
		return packed

	static func flush_literal(p_packed: PackedByteArray, p_literal: PackedByteArray) -> void:
		if p_literal.is_empty():
			return
		if p_literal.size() >= 128:
			p_packed.append_array([128, p_literal.size() >> 8, p_literal.size() & 255])
		else:
			p_packed.append(256 - p_literal.size())
		p_packed.append_array(p_literal)


func prop_u32(p_id: int, p_value: int) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = true
	buffer.put_u32(p_id)
	buffer.put_u32(4)
	buffer.put_32(p_value)
	return buffer.data_array


func prop_float(p_id: int, p_value: float) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = true
	buffer.put_u32(p_id)
	buffer.put_u32(4)
	buffer.put_float(p_value)
	return buffer.data_array


func prop_ints(p_id: int, p_values: Array) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = true
	buffer.put_u32(p_id)
	buffer.put_u32(4 * p_values.size())
	for value: int in p_values:
		buffer.put_32(value)
	return buffer.data_array


func prop_raw(p_id: int, p_payload: PackedByteArray) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = true
	buffer.put_u32(p_id)
	buffer.put_u32(p_payload.size())
	buffer.put_data(p_payload)
	return buffer.data_array


## Samples (0..1 floats, or raw ints for 8-bit) to file bytes of a precision.
func samples(p_values: Array, p_precision: int, p_big_endian: bool = true) -> PackedByteArray:
	var buffer := StreamPeerBuffer.new()
	buffer.big_endian = p_big_endian
	for value: Variant in p_values:
		if p_precision < 200:
			buffer.put_u8(value)
		elif p_precision < 300:
			buffer.put_u16(roundi(value * 65535.0))
		elif p_precision < 500:
			buffer.put_u32(roundi(value * 4294967295.0))
		elif p_precision < 600:
			buffer.put_half(value)
		elif p_precision < 700:
			buffer.put_float(value)
		else:
			buffer.put_double(value)
	return buffer.data_array


func load_bytes(p_bytes: PackedByteArray) -> TetraImageDocument:
	var doc := TetraImageDocument.new()
	var err := doc.load_xcf_buffer(p_bytes)
	check_eq(err, OK, "load_xcf_buffer")
	return doc if err == OK else null


func layer_pixel(p_doc: TetraImageDocument, p_index: int, p_x: int, p_y: int) -> Color:
	return p_doc.layer_get_image(p_doc.get_layer_ids()[p_index]).get_pixel(p_x, p_y)


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 2e-6) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


## The linear colour of an 8-bit sRGB pixel.
func c8(p_r: int, p_g: int, p_b: int, p_a: int = 255) -> Color:
	var c := Color(p_r / 255.0, p_g / 255.0, p_b / 255.0).srgb_to_linear()
	c.a = p_a / 255.0
	return c


func has_warning(p_doc: TetraImageDocument, p_part: String) -> bool:
	for warning in p_doc.get_load_warnings():
		if warning.contains(p_part):
			return true
	return false


## A deterministic RGBA 8-bit test pattern with flat areas, gradients and noise.
func pattern_pixel(p_x: int, p_y: int) -> Array:
	if p_x < 20:
		return [200, 30, 40, 255] # A long run in every plane.
	if p_y < 10:
		return [p_x, 255 - p_x, (p_x * 7) & 255, 128 + (p_x & 1)] # Literals.
	if (p_x / 4 + p_y / 4) % 2 == 0:
		return [10, 20, 30, 0] # Short runs.
	return [(p_x * 37 + p_y * 101) & 255, (p_x * 3) & 255, p_y & 255, (p_x + p_y) & 255]


func pattern(p_width: int, p_height: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	for y in p_height:
		for x in p_width:
			bytes.append_array(pattern_pixel(x, y))
	return bytes


func run() -> void:
	test_real_files()
	test_8bit_offsets_and_compressions()
	test_rle_packets()
	test_precisions()
	test_old_precision_numbers()
	test_gray_and_indexed()
	test_properties_and_modes()
	test_groups()
	test_masks()
	test_selection_and_floating()
	test_load_paths()
	test_rejects()
	test_fuzz()


# --- Real files -----------------------------------------------------------

func test_real_files() -> void:
	var root := OS.get_environment("TETRA_REPO_ROOT")
	if root.is_empty():
		root = ProjectSettings.globalize_path("res://").path_join("../../..").simplify_path()
	var files := find_xcf(root.path_join("upstream/gimp"))
	files.append_array(find_xcf(root.path_join("upstream/blender/release/datafiles")))
	check(files.size() >= 1, "at least one real .xcf file in the checkout")
	DirAccess.make_dir_recursive_absolute(root.path_join("build/screens"))
	for path in files:
		var doc := TetraImageDocument.new()
		var err := doc.load(path)
		check_eq(err, OK, "load " + path)
		if err != OK:
			continue
		check(doc.get_layer_count() >= 1 and doc.get_layer_count() < 200, "sane layer count in " + path)
		check(doc.get_width() >= 1 and doc.get_height() >= 1, "size of " + path)
		print("xcf %s: %d x %d, %d layers" % [path.get_file(), doc.get_width(), doc.get_height(), doc.get_layer_count()])
		for id in doc.get_layer_ids():
			print("  layer \"%s\" visible=%s opacity=%.2f mode=%s" % [doc.layer_get_name(id), doc.layer_is_visible(id), doc.layer_get_opacity(id), TetraImageDocument.get_layer_mode_names()[doc.layer_get_mode(id)]])
		for warning in doc.get_load_warnings():
			print("  warning: " + warning)
		var image := doc.composite()
		check_eq(image.get_size(), doc.get_size(), "composite size of " + path)
		check_eq(image.save_png(root.path_join("build/screens/xcf-%s.png" % path.get_file().get_basename())), OK, "save composite")
		# The image loader gives the same picture.
		var loaded := Image.load_from_file(path)
		check(loaded != null and loaded.get_size() == doc.get_size() and loaded.get_data() == image.get_data(), "Image.load_from_file " + path)

		if path.get_file() == "gimp-2-6-file.xcf":
			# What app/tests/test-xcf.c puts into this file.
			check_eq(doc.get_size(), Vector2i(100, 90), "2.6 file size")
			check_eq(doc.get_layer_count(), 2, "2.6 file layers")
			var ids := doc.get_layer_ids()
			check_eq(doc.layer_get_name(ids[0]), "layer1", "bottom layer")
			check_eq(doc.layer_get_name(ids[1]), "layer2", "top layer")
			check_near(doc.layer_get_opacity(ids[0]), 1.0, 1e-6, "layer1 opacity")
			check_near(doc.layer_get_opacity(ids[1]), 0.0, 1e-6, "layer2 opacity")
			check_eq(doc.layer_get_mode(ids[0]), TetraImageDocument.LAYER_MODE_NORMAL, "layer1 mode")
			check_eq(doc.layer_get_mode(ids[1]), TetraImageDocument.LAYER_MODE_MULTIPLY, "layer2 mode (Multiply legacy)")
			# layer1 is 50 x 51 RGBA at the origin; layer2 is 25 x 251 RGB with a mask.
			check_eq(layer_pixel(doc, 0, 60, 60).a, 0.0, "layer1 is transparent outside its 50 x 51")
			check_eq(layer_pixel(doc, 1, 30, 80).a, 0.0, "layer2 is 25 wide")
			check(has_warning(doc, "masks were applied to the transparency of \"layer2\""), "the mask of layer2 is reported")
			check(has_warning(doc, "cut off"), "layer2 (251 high) is reported as clipped")
			check(has_warning(doc, "channel"), "channel1 is reported")
			check(has_warning(doc, "Paths"), "the paths are reported")
			# The selection is the rectangle 5,6 7x8.
			check(not doc.is_selection_empty(), "selection imported")
			var mask := doc.get_selection_mask()
			check_eq(mask.get_pixel(5, 6).r, 1.0, "selection inside (corner)")
			check_eq(mask.get_pixel(11, 13).r, 1.0, "selection inside (far corner)")
			check_eq(mask.get_pixel(12, 13).r, 0.0, "selection outside right")
			check_eq(mask.get_pixel(5, 14).r, 0.0, "selection outside below")
			check_eq(mask.get_pixel(4, 6).r, 0.0, "selection outside left")


func find_xcf(p_dir: String) -> PackedStringArray:
	var found := PackedStringArray()
	var dir := DirAccess.open(p_dir)
	if dir == null:
		return found
	for file in dir.get_files():
		if file.get_extension().to_lower() == "xcf":
			found.append(p_dir.path_join(file))
	for sub in dir.get_directories():
		if not sub.begins_with("."):
			found.append_array(find_xcf(p_dir.path_join(sub)))
	return found


# --- Crafted files --------------------------------------------------------

func test_8bit_offsets_and_compressions() -> void:
	# 150 x 70: three tile columns, two rows, with partial tiles at the edges.
	var width := 150
	var height := 70
	var data := pattern(width, height)
	for case: Array in [[0, NONE], [3, RLE], [8, ZLIB], [11, RLE], [12, ZLIB], [26, RLE]]:
		var what := "v%d compression %d" % case
		var bytes := XcfWriter.new().write({
			version = case[0], width = 200, height = 100, compression = case[1],
			layers = [{ name = "pattern", width = width, height = height, type = RGBA, data = data,
					props = prop_ints(PROP_OFFSETS, [30, 20]) }],
		})
		var doc := load_bytes(bytes)
		if doc == null:
			continue
		check_eq(doc.get_size(), Vector2i(200, 100), what + " canvas")
		check_eq(doc.get_layer_count(), 1, what + " layers")
		check_eq(doc.layer_get_name(doc.get_layer_ids()[0]), "pattern", what + " name")
		var image := doc.layer_get_image(doc.get_layer_ids()[0])
		var wrong := 0
		for y in 100:
			for x in 200:
				var expected := Color(0, 0, 0, 0)
				if x >= 30 and x < 30 + width and y >= 20 and y < 20 + height:
					var p := pattern_pixel(x - 30, y - 20)
					expected = c8(p[0], p[1], p[2], p[3])
				var actual := image.get_pixel(x, y)
				if absf(actual.r - expected.r) > 2e-6 or absf(actual.g - expected.g) > 2e-6 or absf(actual.b - expected.b) > 2e-6 or absf(actual.a - expected.a) > 2e-6:
					wrong += 1
		check_eq(wrong, 0, what + ": wrong pixels")
		check_eq(doc.get_load_warnings().size(), 0, what + ": nothing to warn about")

	# Negative offsets and a layer larger than the canvas: clipped.
	var clipped := load_bytes(XcfWriter.new().write({
		version = 3, width = 40, height = 30, compression = RLE,
		layers = [{ name = "big", width = width, height = height, type = RGBA, data = data,
				props = prop_ints(PROP_OFFSETS, [-70, -25]) }],
	}))
	if clipped:
		var p := pattern_pixel(70, 25)
		check_color(layer_pixel(clipped, 0, 0, 0), c8(p[0], p[1], p[2], p[3]), "clipped layer, canvas corner")
		p = pattern_pixel(70 + 39, 25 + 29)
		check_color(layer_pixel(clipped, 0, 39, 29), c8(p[0], p[1], p[2], p[3]), "clipped layer, far corner")
		check(has_warning(clipped, "cut off"), "clipping is reported")

	# A layer entirely off the canvas and one with an empty level.
	var empty := load_bytes(XcfWriter.new().write({
		version = 3, width = 40, height = 30,
		layers = [
			{ name = "away", width = 10, height = 10, type = RGBA, data = pattern(10, 10), props = prop_ints(PROP_OFFSETS, [500, 0]) },
			{ name = "empty", width = 40, height = 30, type = RGBA },
			{ name = "empty opaque", width = 20, height = 30, type = RGB },
		],
	}))
	if empty:
		check_eq(empty.get_layer_count(), 3, "three layers")
		check_eq(empty.layer_get_name(empty.get_layer_ids()[2]), "away", "the first layer of the file is on top")
		check_color(layer_pixel(empty, 2, 5, 5), Color(0, 0, 0, 0), "off-canvas layer is empty")
		check_color(layer_pixel(empty, 1, 5, 5), Color(0, 0, 0, 0), "empty level is transparent")
		check_color(layer_pixel(empty, 0, 5, 5), Color(0, 0, 0, 1), "empty level without alpha is opaque black")
		check_color(layer_pixel(empty, 0, 25, 5), Color(0, 0, 0, 0), "outside the layer it is transparent")


func test_rle_packets() -> void:
	# One 5 x 2 RGBA tile written packet by packet.
	var planes := [
		[9, 77], # A run of 10 (short form: n - 1).
		[127, 0, 10, 200], # A run of 10 in the 16-bit form.
		[246, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9], # 10 literals (256 - n).
		[128, 0, 4, 250, 251, 252, 253, 2, 255, 253, 99, 100, 101], # Long-form literals, a run of 3, literals.
	]
	var payload := PackedByteArray()
	for plane: Array in planes:
		payload.append_array(plane)
	var doc := load_bytes(XcfWriter.new().write({
		version = 3, width = 5, height = 2, compression = RLE,
		layers = [{ name = "packets", width = 5, height = 2, type = RGBA, raw_tiles = [payload] }],
	}))
	if doc:
		var alpha := [250, 251, 252, 253, 255, 255, 255, 99, 100, 101]
		for i in 10:
			check_color(layer_pixel(doc, 0, i % 5, i / 5), c8(77, 200, i, alpha[i]), "hand-written RLE pixel %d" % i)

	# Packets that overrun the plane or the data: the layer is unreadable.
	for bad: Array in [[10, 77], [9], [127, 0], [246, 1, 2, 3], [9, 77, 9, 78, 128, 0, 200, 1, 2]]:
		var broken := TetraImageDocument.new()
		var bytes := XcfWriter.new().write({
			version = 3, width = 5, height = 2, compression = RLE,
			layers = [{ name = "bad", width = 5, height = 2, type = RGBA, raw_tiles = [PackedByteArray(bad)] }],
		})
		check_eq(broken.load_xcf_buffer(bytes), ERR_FILE_CORRUPT, "bogus RLE %s" % [bad])
		check_eq(broken.get_layer_count(), 0, "a failed load leaves the document untouched")


func test_precisions() -> void:
	# 3 x 2 RGBA; components in 0..1 that every precision stores exactly enough.
	var values := [
		0.0, 0.25, 0.5, 1.0,   1.0, 0.75, 0.125, 0.5,   0.5, 0.5, 0.5, 0.25,
		0.875, 0.0, 1.0, 0.0,   0.0625, 0.375, 0.625, 1.0,   1.0, 1.0, 1.0, 0.75,
	]
	var index := 0
	for precision: int in [100, 150, 175, 200, 250, 275, 300, 350, 375, 500, 550, 575, 600, 650, 675, 700, 750, 775]:
		index += 1
		var version := 12 + index % 3 * 7
		var compression := index % 3
		var what := "precision %d (v%d, compression %d)" % [precision, version, compression]
		var stored := values
		if precision < 200:
			stored = values.map(func(v: float) -> int: return roundi(v * 255.0))
		var doc := load_bytes(XcfWriter.new().write({
			version = version, width = 3, height = 2, precision = precision, compression = compression,
			layers = [{ name = "p", width = 3, height = 2, type = RGBA, data = samples(stored, precision) }],
		}))
		if doc == null:
			continue
		var linear := precision % 100 == 0
		var epsilon := 2e-6 if precision != 200 and precision != 250 and precision != 275 else 2e-5
		for i in 6:
			var c := Color(values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3])
			if precision < 200:
				c = Color(roundi(c.r * 255.0) / 255.0, roundi(c.g * 255.0) / 255.0, roundi(c.b * 255.0) / 255.0, roundi(c.a * 255.0) / 255.0)
			elif precision < 300:
				c = Color(roundi(c.r * 65535.0) / 65535.0, roundi(c.g * 65535.0) / 65535.0, roundi(c.b * 65535.0) / 65535.0, roundi(c.a * 65535.0) / 65535.0)
			var expected := c if linear else c.srgb_to_linear()
			expected.a = c.a
			check_color(layer_pixel(doc, 0, i % 3, i / 3), expected, "%s pixel %d" % [what, i], epsilon)
		check_eq(doc.get_load_warnings().size(), 0, what + ": no warnings")

	# Float files keep values outside 0..1; NaN and infinity become 0.
	var hdr := load_bytes(XcfWriter.new().write({
		version = 12, width = 2, height = 1, precision = 600, compression = ZLIB,
		layers = [{ name = "hdr", width = 2, height = 1, type = RGBA, data = samples([4.5, -0.25, 1000.0, 1.0, NAN, INF, 0.5, 2.0], 600) }],
	}))
	if hdr:
		check_color(layer_pixel(hdr, 0, 0, 0), Color(4.5, -0.25, 1000.0, 1.0), "values above 1 and below 0 are kept")
		check_color(layer_pixel(hdr, 0, 1, 0), Color(0.0, 0.0, 0.5, 1.0), "NaN and infinity become 0, alpha is clamped")

	# Before version 12 wide samples were stored as the writing machine had them.
	var old := load_bytes(XcfWriter.new().write({
		version = 7, width = 1, height = 1, precision = 200,
		layers = [{ name = "le", width = 1, height = 1, type = RGBA, data = samples([0.25, 0.5, 1.0, 0.5], 200, false) }],
	}))
	if old:
		check_color(layer_pixel(old, 0, 0, 0), Color(roundi(0.25 * 65535) / 65535.0, roundi(0.5 * 65535) / 65535.0, 1.0, roundi(0.5 * 65535) / 65535.0), "v7 16-bit is little-endian", 2e-6)
		check(has_warning(old, "little-endian"), "the assumption is reported")


func test_old_precision_numbers() -> void:
	# Version 4 stored 0..4, versions 5 and 6 an older numbering.
	for case: Array in [[4, 0, 150], [4, 1, 250], [4, 2, 300], [4, 3, 500], [4, 4, 600], [5, 100, 100], [6, 250, 250], [5, 400, 500], [6, 450, 550], [5, 500, 600], [6, 550, 650]]:
		var precision: int = case[2]
		var stored: Array = [1.0, 0.5, 0.0, 1.0]
		if precision < 200:
			stored = [255, 128, 0, 255]
		var doc := load_bytes(XcfWriter.new().write({
			version = case[0], width = 1, height = 1, precision = precision, stored_precision = case[1],
			layers = [{ name = "p", width = 1, height = 1, type = RGBA, data = samples(stored, precision, false) }],
		}))
		if doc:
			var half := 128 / 255.0 if precision < 200 else (roundi(0.5 * 65535.0) / 65535.0 if precision < 300 else 0.5)
			var expected := Color(1.0, half, 0.0, 1.0)
			if precision % 100 != 0:
				expected = expected.srgb_to_linear()
			check_color(layer_pixel(doc, 0, 0, 0), expected, "v%d precision field %d" % [case[0], case[1]], 2e-5)
	for case: Array in [[4, 5], [5, 600], [7, 123], [12, 0]]:
		var doc := TetraImageDocument.new()
		var bytes := XcfWriter.new().write({ version = case[0], width = 1, height = 1, precision = 150, stored_precision = case[1],
				layers = [{ name = "p", width = 1, height = 1, type = RGBA, data = PackedByteArray([1, 2, 3, 4]) }] })
		check(doc.load_xcf_buffer(bytes) != OK, "v%d precision field %d is invalid" % case)


func test_gray_and_indexed() -> void:
	var gray := load_bytes(XcfWriter.new().write({
		version = 3, width = 4, height = 1, base = 1, compression = RLE,
		layers = [
			{ name = "graya", width = 2, height = 1, type = GRAYA, data = PackedByteArray([128, 255, 64, 100]), props = prop_ints(PROP_OFFSETS, [2, 0]) },
			{ name = "gray", width = 4, height = 1, type = GRAY, data = PackedByteArray([0, 51, 128, 255]) },
		],
	}))
	if gray:
		check_color(layer_pixel(gray, 0, 1, 0), c8(51, 51, 51), "grey without alpha")
		check_color(layer_pixel(gray, 0, 3, 0), c8(255, 255, 255), "white")
		check_color(layer_pixel(gray, 1, 2, 0), c8(128, 128, 128), "grey with alpha")
		check_color(layer_pixel(gray, 1, 3, 0), c8(64, 64, 64, 100), "grey with partial alpha")
		check_color(layer_pixel(gray, 1, 0, 0), Color(0, 0, 0, 0), "outside the grey layer")

	# Linear 16-bit grey.
	var gray16 := load_bytes(XcfWriter.new().write({
		version = 12, width = 2, height = 1, base = 1, precision = 200, compression = ZLIB,
		layers = [{ name = "g", width = 2, height = 1, type = GRAYA, data = samples([0.5, 1.0, 1.0, 0.25], 200) }],
	}))
	if gray16:
		var half := roundi(0.5 * 65535.0) / 65535.0
		check_color(layer_pixel(gray16, 0, 0, 0), Color(half, half, half, 1.0), "linear 16-bit grey")
		check_color(layer_pixel(gray16, 0, 1, 0), Color(1, 1, 1, roundi(0.25 * 65535.0) / 65535.0), "linear 16-bit grey alpha")

	var colormap := PackedByteArray([255, 0, 0, 0, 128, 0, 10, 20, 250])
	for compression in [NONE, RLE, ZLIB]:
		var indexed := load_bytes(XcfWriter.new().write({
			version = 8 if compression == ZLIB else 1, width = 3, height = 2, base = 2, colormap = colormap, compression = compression,
			layers = [
				{ name = "indexed alpha", width = 3, height = 1, type = INDEXEDA, data = PackedByteArray([2, 255, 1, 0, 0, 77]) },
				{ name = "indexed", width = 3, height = 2, type = INDEXED, data = PackedByteArray([0, 1, 2, 2, 1, 7]) },
			],
		}))
		if indexed:
			var what := "indexed, compression %d: " % compression
			check_color(layer_pixel(indexed, 0, 0, 0), c8(255, 0, 0), what + "colour 0")
			check_color(layer_pixel(indexed, 0, 1, 0), c8(0, 128, 0), what + "colour 1")
			check_color(layer_pixel(indexed, 0, 0, 1), c8(10, 20, 250), what + "colour 2")
			check_color(layer_pixel(indexed, 0, 2, 1), Color(0, 0, 0, 1), what + "an index beyond the colormap is black")
			check_color(layer_pixel(indexed, 1, 0, 0), c8(10, 20, 250), what + "with alpha, opaque")
			check_color(layer_pixel(indexed, 1, 1, 0), c8(0, 128, 0, 0), what + "with alpha, transparent")
			check_color(layer_pixel(indexed, 1, 2, 0), c8(255, 0, 0, 77), what + "with alpha, partial")
			check_color(layer_pixel(indexed, 1, 0, 1), Color(0, 0, 0, 0), what + "outside the layer")

	# Version 0 colormaps were stored wrongly: GIMP substitutes a grey ramp.
	var ramp := load_bytes(XcfWriter.new().write({
		version = 0, width = 2, height = 1, base = 2, colormap = PackedByteArray([9, 9, 9, 9, 9, 9, 9, 9, 9, 9]),
		layers = [{ name = "v0", width = 2, height = 1, type = INDEXED, data = PackedByteArray([7, 0]) }],
	}))
	if ramp:
		check_color(layer_pixel(ramp, 0, 0, 0), c8(7, 7, 7), "version 0 colormap is a grey ramp")
		check_color(layer_pixel(ramp, 0, 1, 0), c8(0, 0, 0), "version 0 colormap, index 0")


func test_properties_and_modes() -> void:
	var one := PackedByteArray([255, 255, 255, 255])
	var unknown := prop_raw(9999, PackedByteArray([1, 2, 3, 4, 5, 6, 7]))
	var doc := load_bytes(XcfWriter.new().write({
		version = 12, width = 1, height = 1, compression = RLE, extra_props = unknown + prop_raw(19, PackedByteArray([0x43, 0x96, 0, 0, 0x43, 0x96, 0, 0])),
		layers = [
			{ name = "float opacity", width = 1, height = 1, type = RGBA, data = one,
					props = prop_u32(PROP_OPACITY, 255) + prop_float(PROP_FLOAT_OPACITY, 0.3) + prop_u32(PROP_MODE, 30) + prop_u32(PROP_ACTIVE_LAYER, 0).slice(0, 4) + PackedByteArray([0, 0, 0, 0]) },
			{ name = "int opacity", width = 1, height = 1, type = RGBA, data = one,
					props = unknown + prop_u32(PROP_OPACITY, 128) + prop_u32(PROP_VISIBLE, 0) + prop_u32(PROP_LOCK_ALPHA, 1) + prop_u32(PROP_MODE, 3) },
			{ name = "old overlay", width = 1, height = 1, type = RGBA, data = one, props = prop_u32(PROP_MODE, 5) },
			{ name = "pass through", width = 1, height = 1, type = RGBA, data = one, props = prop_u32(PROP_MODE, 61) },
			{ name = "garbage mode", width = 1, height = 1, type = RGBA, data = one, props = prop_u32(PROP_MODE, 4000) },
			{ name = "naïve ünïcode", width = 1, height = 1, type = RGBA, data = one, props = prop_u32(PROP_MODE, 28) + prop_u32(37, 2) },
		],
	}))
	if doc == null:
		return
	var ids := doc.get_layer_ids()
	check_eq(ids.size(), 6, "six layers")
	check_near(doc.layer_get_opacity(ids[5]), 0.3, 1e-6, "PROP_FLOAT_OPACITY overrides PROP_OPACITY")
	check_eq(doc.layer_get_mode(ids[5]), TetraImageDocument.LAYER_MODE_MULTIPLY, "mode 30 is Multiply")
	check_eq(doc.get_active_layer(), ids[5], "PROP_ACTIVE_LAYER")
	check_near(doc.layer_get_opacity(ids[4]), 128 / 255.0, 1e-6, "PROP_OPACITY")
	check_eq(doc.layer_is_visible(ids[4]), false, "PROP_VISIBLE")
	check_eq(doc.layer_get_lock_alpha(ids[4]), true, "PROP_LOCK_ALPHA")
	check_eq(doc.layer_get_lock_alpha(ids[5]), false, "lock alpha defaults to off")
	check_eq(doc.layer_get_mode(ids[4]), TetraImageDocument.LAYER_MODE_MULTIPLY, "mode 3 (Multiply legacy) is Multiply")
	check_eq(doc.layer_get_mode(ids[3]), TetraImageDocument.LAYER_MODE_SOFT_LIGHT, "mode 5 (old Overlay) is Soft light, as in GIMP")
	check_eq(doc.layer_get_mode(ids[2]), MODE_NORMAL, "Pass through on a layer is Normal")
	check_eq(doc.layer_get_mode(ids[1]), MODE_NORMAL, "an unknown mode is Normal")
	check_eq(doc.layer_get_name(ids[0]), "naïve ünïcode", "UTF-8 names")
	check(has_warning(doc, "pass through (Pass through)"), "unavailable mode is reported with its name")
	check(has_warning(doc, "garbage mode (mode 4000)"), "unknown mode is reported")
	check(has_warning(doc, "Legacy layer modes") and has_warning(doc, "\"int opacity\", \"old overlay\""), "legacy modes are reported")
	check(has_warning(doc, "Custom blend space"), "an explicit blend space is reported")

	# Every mode number GIMP 3.2.8 knows, against GIMP's names.
	var gimp_names := ["Normal", "Dissolve", "Behind", "Multiply", "Screen", "Soft light", "Difference", "Addition", "Subtract",
		"Darken only", "Lighten only", "HSV Hue", "HSV Saturation", "HSL Color", "HSV Value", "Divide", "Dodge", "Burn", "Hard light",
		"Soft light", "Grain extract", "Grain merge", "Color erase", "Overlay", "LCh Hue", "LCh Chroma", "LCh Color", "LCh Lightness",
		"Normal", "Behind", "Multiply", "Screen", "Difference", "Addition", "Subtract", "Darken only", "Lighten only", "HSV Hue",
		"HSV Saturation", "HSL Color", "HSV Value", "Divide", "Dodge", "Burn", "Hard light", "Soft light", "Grain extract",
		"Grain merge", "Vivid light", "Pin light", "Linear light", "Hard mix", "Exclusion", "Linear burn", "Luma/Luminance darken only",
		"Luma/Luminance lighten only", "Luminance", "Color erase", "Erase", "Merge", "Split"]
	var layers := []
	for mode in gimp_names.size():
		layers.append({ name = "m%d" % mode, width = 1, height = 1, type = RGBA, data = one, props = prop_u32(PROP_MODE, mode) })
	var modes := load_bytes(XcfWriter.new().write({ version = 10, width = 1, height = 1, layers = layers }))
	if modes:
		var names := TetraImageDocument.get_layer_mode_names()
		var mode_ids := modes.get_layer_ids()
		var mismatches := PackedStringArray()
		for mode in gimp_names.size():
			var got := names[modes.layer_get_mode(mode_ids[gimp_names.size() - 1 - mode])]
			if got.to_lower() != gimp_names[mode].to_lower():
				mismatches.append("%d: %s != %s" % [mode, got, gimp_names[mode]])
		check_eq(mismatches, PackedStringArray(), "GimpLayerMode numbers map to the modes of the same name")
		check(not has_warning(modes, "replaced by Normal"), "all of them exist in Tetra")


func test_groups() -> void:
	var px := func(p_value: int) -> PackedByteArray: return PackedByteArray([p_value, p_value, p_value, 255])
	var group := prop_raw(PROP_GROUP_ITEM, PackedByteArray())
	# top / group (50 %, Multiply) [ a (50 %) / inner (hidden) [ hidden child ] / b ] / bottom
	var doc := load_bytes(XcfWriter.new().write({
		version = 12, width = 1, height = 1, compression = RLE,
		layers = [
			{ name = "top", width = 1, height = 1, type = RGBA, data = px.call(10) },
			{ name = "group", width = 1, height = 1, type = RGBA, props = group + prop_float(PROP_FLOAT_OPACITY, 0.5) + prop_u32(PROP_MODE, 30) },
			{ name = "a", width = 1, height = 1, type = RGBA, data = px.call(20), props = prop_ints(PROP_ITEM_PATH, [1, 0]) + prop_float(PROP_FLOAT_OPACITY, 0.5) },
			{ name = "inner", width = 1, height = 1, type = RGBA, props = group + prop_ints(PROP_ITEM_PATH, [1, 1]) + prop_u32(PROP_VISIBLE, 0) + prop_u32(PROP_MODE, 61) },
			{ name = "hidden child", width = 1, height = 1, type = RGBA, data = px.call(30), props = prop_ints(PROP_ITEM_PATH, [1, 1, 0]) + prop_u32(PROP_ACTIVE_LAYER, 0).slice(0, 4) + PackedByteArray([0, 0, 0, 0]) },
			{ name = "b", width = 1, height = 1, type = RGBA, data = px.call(40), props = prop_ints(PROP_ITEM_PATH, [1, 2]) },
			{ name = "bottom", width = 1, height = 1, type = RGBA, data = px.call(50) },
		],
	}))
	if doc == null:
		return
	var ids := doc.get_layer_ids()
	var names := []
	for id in ids:
		names.append(doc.layer_get_name(id))
	check_eq(names, ["bottom", "b", "hidden child", "a", "top"], "groups are flattened in stacking order")
	if ids.size() == 5:
		check_near(doc.layer_get_opacity(ids[3]), 0.25, 1e-6, "group opacity times layer opacity")
		check_near(doc.layer_get_opacity(ids[1]), 0.5, 1e-6, "group opacity on a child")
		check_near(doc.layer_get_opacity(ids[2]), 0.5, 1e-6, "outer group opacity through an inner group")
		check_eq(doc.layer_is_visible(ids[2]), false, "a hidden group hides its children")
		check_eq(doc.layer_is_visible(ids[1]), true, "children of a visible group stay visible")
		check_near(doc.layer_get_opacity(ids[4]), 1.0, 1e-6, "layers outside the group are untouched")
		check_color(layer_pixel(doc, 2, 0, 0), c8(30, 30, 30), "pixels of a nested child")
		check_eq(doc.get_active_layer(), ids[2], "active layer inside a group")
	check(has_warning(doc, "Layer groups were dissolved") and has_warning(doc, "\"group\", \"inner\""), "groups are reported")
	check(has_warning(doc, "layer mode of these groups"), "group modes are reported")

	# A damaged item path: the layer lands at the top level.
	var damaged := load_bytes(XcfWriter.new().write({
		version = 3, width = 1, height = 1,
		layers = [
			{ name = "lost", width = 1, height = 1, type = RGBA, data = px.call(1), props = prop_ints(PROP_ITEM_PATH, [7, 3]) },
			{ name = "not a group", width = 1, height = 1, type = RGBA, data = px.call(2) },
			{ name = "child of a layer", width = 1, height = 1, type = RGBA, data = px.call(3), props = prop_ints(PROP_ITEM_PATH, [1, 0]) },
		],
	}))
	if damaged:
		check_eq(damaged.get_layer_count(), 3, "layers with damaged paths are kept")
		check(has_warning(damaged, "\"lost\", \"child of a layer\""), "damaged paths are reported")

	# Deeply nested groups do not recurse without limit.
	var deep := []
	var path := []
	for depth in 300:
		var props := group
		if depth > 0:
			props = group + prop_ints(PROP_ITEM_PATH, path)
		deep.append({ name = "g%d" % depth, width = 1, height = 1, type = RGBA, props = props })
		path = path.duplicate()
		path.append(0)
	deep.append({ name = "leaf", width = 1, height = 1, type = RGBA, data = px.call(9), props = prop_ints(PROP_ITEM_PATH, path) })
	var nested := TetraImageDocument.new()
	var err := nested.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1, layers = deep }))
	check(err == OK or err == ERR_FILE_CORRUPT, "300 nested groups load or fail cleanly")


func test_masks() -> void:
	var white := PackedByteArray()
	for i in 4:
		white.append_array([255, 255, 255, 200])
	var mask := { width = 2, height = 2, data = PackedByteArray([255, 128, 0, 51]) }
	var doc := load_bytes(XcfWriter.new().write({
		version = 3, width = 4, height = 4, compression = RLE,
		layers = [
			{ name = "masked", width = 2, height = 2, type = RGBA, data = white, mask = mask, props = prop_u32(PROP_APPLY_MASK, 1) + prop_ints(PROP_OFFSETS, [1, 2]) },
			{ name = "mask off", width = 2, height = 2, type = RGBA, data = white, mask = mask, props = prop_u32(PROP_APPLY_MASK, 0) },
			{ name = "opaque masked", width = 2, height = 2, type = RGB, data = PackedByteArray([255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255]), mask = mask },
		],
	}))
	if doc:
		var a := 200 / 255.0
		check_color(layer_pixel(doc, 2, 1, 2), Color(1, 1, 1, a), "mask 255 keeps alpha")
		check_color(layer_pixel(doc, 2, 2, 2), Color(1, 1, 1, a * 128 / 255.0), "mask 128 scales alpha")
		check_color(layer_pixel(doc, 2, 1, 3), Color(1, 1, 1, 0), "mask 0 makes transparent")
		check_color(layer_pixel(doc, 2, 2, 3), Color(1, 1, 1, a * 51 / 255.0), "mask 51")
		check_color(layer_pixel(doc, 1, 0, 1), Color(1, 1, 1, a), "a disabled mask is not applied")
		check_color(layer_pixel(doc, 0, 1, 0), Color(1, 1, 1, 128 / 255.0), "a mask gives a layer without alpha its transparency")
		check(has_warning(doc, "masks were applied") and has_warning(doc, "\"masked\", \"opaque masked\""), "applied masks are reported")
		check(has_warning(doc, "Disabled layer masks were left out: \"mask off\""), "the dropped mask is reported")

	# 16-bit float image: masks have one sample of the image's precision.
	var float_mask := load_bytes(XcfWriter.new().write({
		version = 12, width = 2, height = 1, precision = 600, compression = ZLIB,
		layers = [{ name = "f", width = 2, height = 1, type = RGBA, data = samples([1, 1, 1, 1, 1, 1, 1, 1], 600),
				mask = { width = 2, height = 1, data = samples([0.25, 1.0], 600) } }],
	}))
	if float_mask:
		check_color(layer_pixel(float_mask, 0, 0, 0), Color(1, 1, 1, 0.25), "float mask")

	# A mask on a group (version 13) cuts all its children.
	var px := PackedByteArray([255, 255, 255, 255, 255, 255, 255, 255])
	var grouped := load_bytes(XcfWriter.new().write({
		version = 13, width = 2, height = 1,
		layers = [
			{ name = "g", width = 2, height = 1, type = RGBA, props = prop_raw(PROP_GROUP_ITEM, PackedByteArray()), mask = { width = 2, height = 1, data = PackedByteArray([255, 51]) } },
			{ name = "child", width = 2, height = 1, type = RGBA, data = px, props = prop_ints(PROP_ITEM_PATH, [0, 0]) },
			{ name = "outside", width = 2, height = 1, type = RGBA, data = px },
		],
	}))
	if grouped:
		check_color(layer_pixel(grouped, 1, 0, 0), Color(1, 1, 1, 1), "group mask, white part")
		check_color(layer_pixel(grouped, 1, 1, 0), Color(1, 1, 1, 0.2), "group mask, grey part")
		check_color(layer_pixel(grouped, 0, 1, 0), Color(1, 1, 1, 1), "a layer outside the group is not masked")


func test_selection_and_floating() -> void:
	var px := func(p_value: int) -> PackedByteArray: return PackedByteArray([p_value, p_value, p_value, 255, p_value, p_value, p_value, 255])
	var selection := { name = "Selection Mask", width = 2, height = 1, data = PackedByteArray([0, 153]), props = prop_raw(PROP_SELECTION, PackedByteArray()) }
	var saved := { name = "saved", width = 2, height = 1, data = PackedByteArray([255, 255]) }
	var doc := load_bytes(XcfWriter.new().write({
		version = 12, width = 2, height = 1, compression = RLE,
		layers = [
			{ name = "floating", width = 1, height = 1, type = RGBA, data = PackedByteArray([1, 2, 3, 255]), floating_on = 2, props = prop_ints(PROP_OFFSETS, [1, 0]) + prop_float(PROP_FLOAT_OPACITY, 0.5) },
			{ name = "top", width = 2, height = 1, type = RGBA, data = px.call(10) },
			{ name = "host", width = 2, height = 1, type = RGBA, data = px.call(20) },
			{ name = "bottom", width = 2, height = 1, type = RGBA, data = px.call(30) },
		],
		channels = [saved, selection],
	}))
	if doc:
		var names := []
		for id in doc.get_layer_ids():
			names.append(doc.layer_get_name(id))
		check_eq(names, ["bottom", "host", "floating", "top"], "the floating selection sits above its host")
		if names.size() == 4:
			check_color(layer_pixel(doc, 2, 1, 0), c8(1, 2, 3), "floating selection pixels at its offset")
			check_color(layer_pixel(doc, 2, 0, 0), Color(0, 0, 0, 0), "floating selection is 1 x 1")
			check_near(doc.layer_get_opacity(doc.get_layer_ids()[2]), 0.5, 1e-6, "floating selection opacity")
		check(has_warning(doc, "floating selection became a layer"), "floating selection is reported")
		check(not doc.is_selection_empty(), "the selection channel becomes the selection")
		check_near(doc.get_selection_mask().get_pixel(1, 0).r, 0.6, 1e-6, "selection value")
		check_near(doc.get_selection_mask().get_pixel(0, 0).r, 0.0, 1e-6, "unselected pixel")
		check(has_warning(doc, "1 saved channels"), "saved channels are reported")

	# An all-zero selection channel is no selection; a floating selection of a
	# channel is dropped.
	selection.data = PackedByteArray([0, 0])
	var none := load_bytes(XcfWriter.new().write({
		version = 3, width = 2, height = 1,
		layers = [{ name = "only", width = 2, height = 1, type = RGBA, data = px.call(10) }],
		channels = [selection],
	}))
	if none:
		check(none.is_selection_empty(), "an empty selection channel selects nothing")
		check_eq(none.get_load_warnings().size(), 0, "and is not worth a warning")


func test_load_paths() -> void:
	var bytes := XcfWriter.new().write({
		version = 12, width = 70, height = 3, compression = ZLIB,
		layers = [
			{ name = "half", width = 70, height = 3, type = RGBA, data = pattern(70, 3), props = prop_float(PROP_FLOAT_OPACITY, 0.5) },
			{ name = "base", width = 70, height = 3, type = RGB, data = pattern(70, 4).slice(0, 70 * 3 * 3) },
		],
	})
	var path := tmp_dir().path_join("crafted.xcf")
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()

	var doc := TetraImageDocument.new()
	doc.create(5, 5)
	doc.add_layer("extra")
	check(doc.can_undo(), "there is history before the load")
	check_eq(doc.load(path), OK, "load() takes .xcf")
	check_eq(doc.get_size(), Vector2i(70, 3), "size after load")
	check_eq(doc.get_layer_count(), 2, "layers after load")
	check(not doc.can_undo(), "loading clears the history")
	check_eq(doc.get_load_warnings().size(), 0, "no warnings")

	var image := Image.load_from_file(path)
	check(image != null and image.get_size() == Vector2i(70, 3), "Image.load_from_file takes .xcf")
	if image:
		check_eq(image.get_data(), doc.composite().get_data(), "the image loader returns the composite")

	# Warnings belong to the last load.
	doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1,
			layers = [{ name = "x", width = 9, height = 9, type = RGBA }] }))
	check_eq(doc.get_load_warnings().size(), 1, "one warning (clipped)")
	check_eq(doc.load(path), OK, "load again")
	check_eq(doc.get_load_warnings().size(), 0, "warnings are replaced by the next load")

	# A missing file and a file that is not XCF.
	print("(two error messages about unreadable files are expected here)")
	check(doc.load(tmp_dir().path_join("missing.xcf")) != OK, "missing file")
	var wrong := FileAccess.open(tmp_dir().path_join("wrong.xcf"), FileAccess.WRITE)
	wrong.store_string("this is not an xcf file at all")
	wrong.close()
	check_eq(doc.load(tmp_dir().path_join("wrong.xcf")), ERR_FILE_UNRECOGNIZED, "not an XCF file")
	check_eq(doc.get_layer_count(), 2, "a failed load keeps the document")


func test_rejects() -> void:
	var doc := TetraImageDocument.new()
	var layer := { name = "l", width = 1, height = 1, type = RGBA, data = PackedByteArray([1, 2, 3, 4]) }
	check_eq(doc.load_xcf_buffer(PackedByteArray()), ERR_FILE_UNRECOGNIZED, "empty data")
	check_eq(doc.load_xcf_buffer("gimp xcf v999".to_ascii_buffer() + PackedByteArray([0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0])), ERR_FILE_UNRECOGNIZED, "a version from the future")
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 27, width = 1, height = 1, layers = [layer] })), ERR_FILE_UNRECOGNIZED, "version 27")
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1, base = 3, layers = [layer] })), ERR_FILE_UNRECOGNIZED, "invalid base type")
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1, layers = [] })), ERR_FILE_CORRUPT, "no layers")
	# Too large for a document: refused before anything is allocated.
	var start := Time.get_ticks_msec()
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 20000, height = 20000, layers = [layer] })), ERR_OUT_OF_MEMORY, "20000 x 20000")
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 9000, height = 9000, layers = [layer] })), ERR_OUT_OF_MEMORY, "9000 x 9000")
	check(Time.get_ticks_msec() - start < 2000, "oversized images are refused at once")
	# GIMP takes a nonsensical image size as 1 x 1 and goes on.
	var lenient := load_bytes(XcfWriter.new().write({ version = 3, width = 0x7fffffff, height = 0, layers = [layer] }))
	if lenient:
		check_eq(lenient.get_size(), Vector2i(1, 1), "invalid image size becomes 1 x 1")
	# A layer that claims to be enormous costs nothing: only the canvas part is read.
	start = Time.get_ticks_msec()
	var huge := doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 8, height = 8,
			layers = [{ name = "huge", width = 500000, height = 500000, type = RGBA }, layer] }))
	check_eq(huge, OK, "a 500000 x 500000 empty layer")
	check(Time.get_ticks_msec() - start < 2000, "and it takes no time")
	check_eq(doc.get_layer_count(), 2, "both layers are there")
	# One damaged layer among good ones is left out, as in GIMP.
	var damaged := load_bytes(XcfWriter.new().write({ version = 3, width = 1, height = 1, compression = RLE,
			layers = [layer, { name = "bad", width = 1, height = 1, type = RGBA, raw_tiles = [PackedByteArray([200])] }, layer] }))
	if damaged:
		check_eq(damaged.get_layer_count(), 2, "the damaged layer is skipped")
		check(has_warning(damaged, "1 damaged layers"), "and reported")
	# An unreadable group does not take its layers with it.
	var orphan := load_bytes(XcfWriter.new().write({ version = 3, width = 1, height = 1,
			layers = [{ name = "broken group", width = 1, height = 1, type = 9, props = prop_raw(PROP_GROUP_ITEM, PackedByteArray()) },
				{ name = "child", width = 1, height = 1, type = RGBA, data = PackedByteArray([1, 2, 3, 4]), props = prop_ints(PROP_ITEM_PATH, [0, 0]) }, layer] }))
	if orphan:
		check_eq(orphan.get_layer_count(), 2, "the child of an unreadable group is kept")
	# "Fractal" compression never existed.
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1, compression = 3, layers = [layer] })), ERR_FILE_CORRUPT, "fractal compression")
	check_eq(doc.load_xcf_buffer(XcfWriter.new().write({ version = 3, width = 1, height = 1, compression = 9, layers = [layer] })), ERR_FILE_CORRUPT, "unknown compression")
	# The hierarchy must match the layer.
	var mismatch := XcfWriter.new().write({ version = 3, width = 2, height = 1, layers = [{ name = "rgb as rgba", width = 2, height = 1, type = RGB, data = PackedByteArray([1, 2, 3, 4, 5, 6]) }] })
	check_eq(doc.load_xcf_buffer(mismatch), OK, "a sound file")
	var at := mismatch.size() - 1
	while at > 0 and not (mismatch[at] == 3 and mismatch[at - 4] == 1 and mismatch[at - 8] == 2):
		at -= 1
	mismatch[at] = 4 # The hierarchy's bytes per pixel.
	check_eq(doc.load_xcf_buffer(mismatch), ERR_FILE_CORRUPT, "bytes per pixel that do not match the layer type")


# --- Damaged files --------------------------------------------------------

func fuzz_seeds() -> Array[PackedByteArray]:
	var small := pattern(70, 9)
	var mask := { width = 70, height = 9, data = small.slice(0, 630) }
	var group := prop_raw(PROP_GROUP_ITEM, PackedByteArray())
	var seeds: Array[PackedByteArray] = []
	for case: Array in [[3, NONE], [3, RLE], [12, ZLIB], [20, RLE]]:
		seeds.append(XcfWriter.new().write({
			version = case[0], width = 80, height = 12, compression = case[1],
			layers = [
				{ name = "floating", width = 3, height = 3, type = RGBA, data = small.slice(0, 36), floating_on = 3 },
				{ name = "group", width = 70, height = 9, type = RGBA, props = group + prop_float(PROP_FLOAT_OPACITY, 0.5), mask = mask },
				{ name = "child", width = 70, height = 9, type = RGBA, data = small, mask = mask, props = prop_ints(PROP_ITEM_PATH, [1, 0]) + prop_ints(PROP_OFFSETS, [5, 2]) },
				{ name = "base", width = 70, height = 9, type = RGB, data = small.slice(0, 70 * 9 * 3), props = prop_u32(PROP_MODE, 30) },
			],
			channels = [{ name = "sel", width = 80, height = 12, data = pattern(80, 3), props = prop_raw(PROP_SELECTION, PackedByteArray()) }],
		}))
	seeds.append(XcfWriter.new().write({
		version = 12, width = 9, height = 5, precision = 700, compression = RLE,
		layers = [{ name = "double", width = 9, height = 5, type = RGBA, data = samples(range(180).map(func(i: int) -> float: return i / 180.0), 700) }],
	}))
	seeds.append(XcfWriter.new().write({
		version = 2, width = 9, height = 5, base = 2, colormap = PackedByteArray([1, 2, 3, 4, 5, 6]), compression = RLE,
		layers = [{ name = "indexed", width = 9, height = 5, type = INDEXEDA, data = small.slice(0, 90) }],
	}))
	var root := OS.get_environment("TETRA_REPO_ROOT")
	var real := FileAccess.get_file_as_bytes(root.path_join("upstream/gimp/app/tests/files/gimp-2-6-file.xcf"))
	if not real.is_empty():
		seeds.append(real)
	return seeds


var _fuzz_doc := TetraImageDocument.new()
var _fuzz_loads := 0
var _fuzz_loaded := 0
var _fuzz_sound := true


func fuzz_try(p_bytes: PackedByteArray) -> void:
	_fuzz_loads += 1
	if _fuzz_doc.load_xcf_buffer(p_bytes) != OK:
		return
	_fuzz_loaded += 1
	# Whatever came out must be a usable document.
	if not TetraImageDocument.is_size_supported(_fuzz_doc.get_width(), _fuzz_doc.get_height()) or _fuzz_doc.get_layer_count() < 1:
		_fuzz_sound = false
	elif _fuzz_doc.get_width() * _fuzz_doc.get_height() <= 1 << 20:
		if _fuzz_doc.composite().get_size() != _fuzz_doc.get_size():
			_fuzz_sound = false


func test_fuzz() -> void:
	var random := RandomNumberGenerator.new()
	random.seed = 20261119
	var started := Time.get_ticks_msec()
	for bytes in fuzz_seeds():
		check_eq(_fuzz_doc.load_xcf_buffer(bytes), OK, "fuzz seed loads")
		# Every truncation.
		for length in bytes.size():
			fuzz_try(bytes.slice(0, length))
		# Single byte changes: every byte once with a random value, the first
		# 400 bytes (the structure) also with the extreme values.
		for at in bytes.size():
			var changed := bytes.duplicate()
			changed[at] = random.randi_range(0, 255)
			fuzz_try(changed)
			if at < 400:
				for value: int in [0, 1, 0x7f, 0x80, 0xff]:
					changed = bytes.duplicate()
					changed[at] = value
					fuzz_try(changed)
		# Several changes at once, with and without a truncation.
		for round in 1500:
			var changed := bytes.duplicate()
			for flip in random.randi_range(2, 8):
				changed[random.randi_range(0, changed.size() - 1)] = random.randi_range(0, 255)
			if round % 3 == 0:
				changed = changed.slice(0, random.randi_range(14, changed.size()))
			fuzz_try(changed)
	print("xcf fuzz: %d damaged files, %d still loaded, %d ms" % [_fuzz_loads, _fuzz_loaded, Time.get_ticks_msec() - started])
	check(_fuzz_loads > 10000, "many damaged files were tried")
	check(_fuzz_sound, "every damaged file that loads gives a usable document")
	check(_fuzz_loaded > 0 and _fuzz_loaded < _fuzz_loads, "some damaged files load, some are refused")
