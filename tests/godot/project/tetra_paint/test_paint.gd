extends "res://tetra_test.gd"
## Paint strokes of TetraImageDocument.

const WHITE := Color(1, 1, 1, 1)
const BLACK := Color(0, 0, 0, 1)
const RED := Color(1, 0, 0, 1)
const BLUE := Color(0, 0, 1, 1)


func check_color(p_actual: Color, p_expected: Color, p_what: String, p_epsilon: float = 1e-5) -> void:
	var close := absf(p_actual.r - p_expected.r) <= p_epsilon and absf(p_actual.g - p_expected.g) <= p_epsilon \
			and absf(p_actual.b - p_expected.b) <= p_epsilon and absf(p_actual.a - p_expected.a) <= p_epsilon
	check(close, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func px(p_doc: TetraImageDocument, p_x: int, p_y: int) -> Color:
	return p_doc.layer_get_image(p_doc.get_active_layer()).get_pixel(p_x, p_y)


func new_doc(p_width: int, p_height: int, p_fill: Color = WHITE) -> TetraImageDocument:
	var doc := TetraImageDocument.new()
	doc.create(p_width, p_height, p_fill)
	return doc


## GIMP's generated-brush falloff table (gimp_brush_generated_calc_lut),
## written out again here so the engine is checked against a second copy.
func gimp_profile(p_radius: float, p_hardness: float) -> PackedFloat32Array:
	var gauss := func(p_f: float) -> float:
		if p_f < -0.5:
			p_f = -1.0 - p_f
			return 2.0 * p_f * p_f
		if p_f < 0.5:
			return 1.0 - 2.0 * p_f * p_f
		p_f = 1.0 - p_f
		return 2.0 * p_f * p_f
	var length := 4 * ceili(1.0 + sqrt(2.0 * pow(ceilf(p_radius + 1.0), 2.0)))
	var exponent := 1000000.0 if (1.0 - p_hardness) < 0.0000004 else 0.4 / (1.0 - p_hardness)
	var buffer := [0.0, 0.0, 0.0, 0.0]
	var sum := 0.0
	var d := 0.0
	for i in 4:
		d = absf((i + 0.5) / 4.0 - 0.5)
		buffer[i] = 0.0 if d > p_radius else gauss.call(pow(d / p_radius, exponent))
		sum += buffer[i]
	var table := PackedFloat32Array()
	table.resize(length)
	var x := 0
	while (d < p_radius or sum > 0.00001) and x < length:
		sum -= buffer[x % 4]
		buffer[x % 4] = 0.0 if d > p_radius else gauss.call(pow(d / p_radius, exponent))
		sum += buffer[x % 4]
		table[x] = sum / 4.0
		x += 1
		d += 0.25
	return table


func profile_at(p_table: PackedFloat32Array, p_radius: float, p_distance: float) -> float:
	if p_distance >= p_radius + 1.0:
		return 0.0
	return p_table[roundi(p_distance * 4.0)]


func run() -> void:
	test_pencil()
	test_constant_application()
	test_soft_brush()
	test_airbrush()
	test_eraser_and_locks()
	test_modes_and_pressure()
	test_clone()
	test_smudge()
	test_spacing_and_cancel()
	save_stroke_sheet()


func test_pencil() -> void:
	var doc := new_doc(10, 6)
	var layer := doc.get_active_layer()
	var rects: Array[Rect2i] = []
	doc.changed.connect(func(p_rect: Rect2i) -> void: rects.append(p_rect))
	var settings := {"color": BLACK, "size": 1.0}

	check_eq(doc.stroke_begin(layer, TetraImageDocument.TOOL_PENCIL, settings), OK, "stroke begin")
	check(doc.is_stroke_active(), "stroke is active")
	doc.stroke_add_point(Vector2(2.5, 2.5))
	check_eq(rects, [Rect2i(1, 1, 3, 3)] as Array[Rect2i], "the first point reports its dab")
	check_color(px(doc, 2, 2), BLACK, "the first dab is visible before the stroke ends")
	doc.stroke_add_point(Vector2(7.5, 2.5))
	check_eq(rects.back(), Rect2i(2, 1, 7, 3), "the second point reports only the new dabs")
	check(not doc.can_undo(), "no undo step while the stroke is open")
	check_eq(doc.fill(layer, RED), ERR_BUSY, "other edits are refused during a stroke")
	doc.stroke_end()
	check(not doc.is_stroke_active(), "stroke ended")

	for x in range(2, 8):
		check_color(px(doc, x, 2), BLACK, "pencil pixel %d" % x)
	for point: Vector2i in [Vector2i(1, 2), Vector2i(8, 2), Vector2i(4, 1), Vector2i(4, 3)]:
		check_color(px(doc, point.x, point.y), WHITE, "pencil leaves %s" % point)
	check_eq(doc.get_undo_names(), PackedStringArray(["Pencil"]), "one undo step per stroke")
	check_eq(doc.get_undo_memory_usage(), 8 * 3 * 16, "the stroke stores only its bounding box")
	doc.undo()
	var all_white := true
	for y in 6:
		for x in 10:
			all_white = all_white and px(doc, x, y) == WHITE
	check(all_white, "undo restores the layer exactly")
	doc.redo()
	check_color(px(doc, 5, 2), BLACK, "redo repaints the stroke")

	# A diagonal one-pixel line is connected: one pixel per column.
	var diagonal := new_doc(10, 10)
	diagonal.stroke_line(diagonal.get_active_layer(), TetraImageDocument.TOOL_PENCIL, settings, Vector2(1.5, 1.5), Vector2(8.5, 8.5))
	for i in range(1, 9):
		check_color(px(diagonal, i, i), BLACK, "diagonal pixel %d" % i)
	check_color(px(diagonal, 2, 1), WHITE, "the diagonal is one pixel wide")


func test_constant_application() -> void:
	# Overlapping dabs of one stroke do not build up: a hard brush at 50 %
	# opacity leaves exactly 50 % however many dabs cover a pixel.
	var doc := new_doc(30, 20)
	var layer := doc.get_active_layer()
	var settings := {"color": BLACK, "size": 6.0, "hardness": 1.0, "opacity": 0.5}
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH, settings, Vector2(5, 10), Vector2(25, 10))
	check_color(px(doc, 15, 10), Color(0.5, 0.5, 0.5, 1), "stroke centre at half opacity")
	check_color(px(doc, 10, 9), Color(0.5, 0.5, 0.5, 1), "stroke body at half opacity")
	check_color(px(doc, 15, 14), WHITE, "outside the stroke")
	# A stroke that crosses itself is still flat.
	doc.undo()
	doc.stroke_begin(layer, TetraImageDocument.TOOL_PAINTBRUSH, settings)
	doc.stroke_add_point(Vector2(5, 10))
	doc.stroke_add_point(Vector2(25, 10))
	doc.stroke_add_point(Vector2(15, 4))
	doc.stroke_add_point(Vector2(15, 16))
	doc.stroke_end()
	check_color(px(doc, 15, 10), Color(0.5, 0.5, 0.5, 1), "self-crossing stroke does not darken")
	check_color(px(doc, 15, 6), Color(0.5, 0.5, 0.5, 1), "second arm of the stroke")
	# A second stroke does add to the first.
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH, settings, Vector2(5, 10), Vector2(25, 10))
	check_color(px(doc, 15, 10), Color(0.25, 0.25, 0.25, 1), "a new stroke paints over the previous one")
	check_color(px(doc, 15, 6), Color(0.5, 0.5, 0.5, 1), "untouched part of the first stroke")

	# The colour's alpha scales the stroke opacity.
	var tinted := new_doc(12, 12)
	tinted.stroke_line(tinted.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH,
			{"color": Color(1, 0, 0, 0.5), "size": 6.0, "hardness": 1.0, "opacity": 0.5}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(tinted, 6, 6), Color(1, 0.75, 0.75, 1), "colour alpha times opacity")

	# The selection limits the stroke.
	var limited := new_doc(30, 20)
	limited.select_rect(Rect2i(0, 0, 15, 20))
	limited.stroke_line(limited.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH,
			{"color": BLACK, "size": 6.0, "hardness": 1.0}, Vector2(5, 10), Vector2(25, 10))
	check_color(px(limited, 14, 10), BLACK, "painted inside the selection")
	check_color(px(limited, 15, 10), WHITE, "not painted outside the selection")
	# The selection step stores 15 x 20 mask values, the stroke its 14 x 8 pixels inside the selection.
	check_eq(limited.get_undo_memory_usage(), 15 * 20 * 4 + 14 * 8 * 16, "the stroke's undo region stays inside the selection bounds")


func test_soft_brush() -> void:
	var radius := 10.0
	var hardness := 0.5
	var table := gimp_profile(radius, hardness)
	var doc := new_doc(32, 32)
	var layer := doc.get_active_layer()
	var settings := {"color": BLACK, "size": 20.0, "hardness": hardness}
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH, settings, Vector2(16, 16), Vector2(16, 16))
	# One dab: every pixel is white faded to black by the brush profile at its distance.
	for point: Vector2i in [Vector2i(16, 16), Vector2i(20, 16), Vector2i(24, 16), Vector2i(26, 16), Vector2i(12, 9), Vector2i(8, 20), Vector2i(27, 16), Vector2i(3, 3)]:
		var distance := Vector2(point.x + 0.5 - 16.0, point.y + 0.5 - 16.0).length()
		var expected := 1.0 - profile_at(table, radius, distance)
		check_near(px(doc, point.x, point.y).r, expected, 1e-5, "soft dab at %s (distance %.2f)" % [point, distance])
	check_near(px(doc, 16, 16).r, 1.0 - table[3], 1e-5, "the centre uses the table near zero")
	check(px(doc, 16, 16).r < 0.05, "soft dab centre is nearly solid: %f" % px(doc, 16, 16).r)
	check(px(doc, 20, 16).r < px(doc, 24, 16).r and px(doc, 24, 16).r < px(doc, 25, 16).r, "soft dab fades outwards")
	check_near(px(doc, 15, 15).r, px(doc, 16, 16).r, 1e-6, "a dab on a pixel corner is symmetric")
	check_color(px(doc, 28, 16), WHITE, "beyond the radius nothing is painted")

	# Sub-pixel placement: a quarter-pixel shift changes the edge pixels.
	var shifted := new_doc(32, 32)
	shifted.stroke_line(shifted.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH, settings, Vector2(16.25, 16), Vector2(16.25, 16))
	var right := Vector2(24 + 0.5 - 16.25, 0.5).length()
	var left := Vector2(7 + 0.5 - 16.25, 0.5).length()
	check_near(px(shifted, 24, 16).r, 1.0 - profile_at(table, radius, right), 1e-5, "shifted dab, right edge")
	check_near(px(shifted, 7, 16).r, 1.0 - profile_at(table, radius, left), 1e-5, "shifted dab, left edge")
	check(px(shifted, 24, 16).r < px(shifted, 7, 16).r, "the shifted dab leans right")

	# A hard brush has an antialiased rim of about one pixel.
	var hard := new_doc(32, 32)
	hard.stroke_line(hard.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH, {"color": BLACK, "size": 20.0, "hardness": 1.0}, Vector2(16.5, 16.5), Vector2(16.5, 16.5))
	check_color(px(hard, 25, 16), BLACK, "hard brush is solid inside")
	var rim := px(hard, 26, 16).r
	check(rim > 0.0 and rim < 1.0, "hard brush rim is antialiased: %f" % rim)
	check_color(px(hard, 27, 16), WHITE, "hard brush ends at its radius")


func test_airbrush() -> void:
	var doc := new_doc(12, 12)
	var layer := doc.get_active_layer()
	var settings := {"color": BLACK, "size": 6.0, "hardness": 1.0, "flow": 0.1}
	doc.stroke_begin(layer, TetraImageDocument.TOOL_AIRBRUSH, settings)
	doc.stroke_add_point(Vector2(6, 6))
	check_near(px(doc, 6, 6).r, 0.9, 1e-5, "one airbrush dab")
	doc.stroke_add_point(Vector2(6, 6))
	check_near(px(doc, 6, 6).r, 0.81, 1e-5, "a resting airbrush keeps spraying")
	doc.stroke_add_point(Vector2(6, 6))
	check_near(px(doc, 6, 6).r, 0.729, 1e-5, "third dab")
	doc.stroke_end()
	check_eq(doc.get_undo_names(), PackedStringArray(["Airbrush"]), "airbrush undo name")
	check_eq(TetraImageDocument.get_default_stroke_settings(TetraImageDocument.TOOL_AIRBRUSH)["flow"], 0.1, "airbrush default flow")
	check_eq(TetraImageDocument.get_default_stroke_settings(TetraImageDocument.TOOL_PAINTBRUSH)["spacing"], 10.0, "default spacing is 10 %")

	# The paintbrush does not build up when it rests.
	var brush := new_doc(12, 12)
	brush.stroke_begin(brush.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH, {"color": BLACK, "size": 6.0, "hardness": 1.0, "opacity": 0.5})
	for i in 3:
		brush.stroke_add_point(Vector2(6, 6))
	brush.stroke_end()
	check_near(px(brush, 6, 6).r, 0.5, 1e-6, "a resting paintbrush paints once")


func test_eraser_and_locks() -> void:
	var doc := new_doc(12, 12, RED)
	var layer := doc.get_active_layer()
	var eraser := {"size": 6.0, "hardness": 1.0}
	doc.stroke_line(layer, TetraImageDocument.TOOL_ERASER, eraser, Vector2(6, 6), Vector2(6, 6))
	check_color(px(doc, 6, 6), Color(1, 0, 0, 0), "the eraser removes alpha and keeps the colour")
	check_color(px(doc, 11, 11), RED, "the eraser leaves the rest")
	doc.undo()
	eraser["opacity"] = 0.25
	doc.stroke_line(layer, TetraImageDocument.TOOL_ERASER, eraser, Vector2(6, 6), Vector2(6, 6))
	check_color(px(doc, 6, 6), Color(1, 0, 0, 0.75), "eraser opacity")
	doc.undo()

	doc.layer_set_lock_alpha(layer, true)
	doc.stroke_line(layer, TetraImageDocument.TOOL_ERASER, {"size": 6.0, "hardness": 1.0}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(doc, 6, 6), RED, "the eraser cannot change a layer with locked alpha")

	# Painting with locked alpha colours only what is there.
	var sparse := new_doc(12, 12, Color(0, 0, 0, 0))
	var sparse_layer := sparse.get_active_layer()
	sparse.select_rect(Rect2i(6, 0, 6, 12))
	sparse.fill(sparse_layer, Color(1, 0, 0, 0.5))
	sparse.select_none()
	sparse.layer_set_lock_alpha(sparse_layer, true)
	sparse.stroke_line(sparse_layer, TetraImageDocument.TOOL_PAINTBRUSH, {"color": BLUE, "size": 8.0, "hardness": 1.0}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(sparse, 7, 6), Color(0, 0, 1, 0.5), "locked alpha: colour replaced, alpha kept")
	check_color(px(sparse, 4, 6), Color(0, 0, 0, 0), "locked alpha: empty pixels stay empty")

	# Without the lock the same stroke also covers the empty half.
	sparse.undo()
	sparse.layer_set_lock_alpha(sparse_layer, false)
	sparse.stroke_line(sparse_layer, TetraImageDocument.TOOL_PAINTBRUSH, {"color": BLUE, "size": 8.0, "hardness": 1.0}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(sparse, 4, 6), BLUE, "unlocked: empty pixels are painted")
	check_color(px(sparse, 7, 6), BLUE, "unlocked: translucent pixels become opaque")


func test_modes_and_pressure() -> void:
	var backdrop := Color(0.2, 0.5, 0.8, 1)
	var paint := Color(0.6, 0.4, 0.1, 1)
	var doc := new_doc(12, 12, backdrop)
	var layer := doc.get_active_layer()
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH,
			{"color": paint, "size": 6.0, "hardness": 1.0, "mode": TetraImageDocument.LAYER_MODE_MULTIPLY}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(doc, 6, 6), Color(0.12, 0.2, 0.08, 1), "multiply paint mode")
	doc.undo()
	doc.stroke_line(layer, TetraImageDocument.TOOL_PAINTBRUSH,
			{"color": paint, "size": 6.0, "hardness": 1.0, "opacity": 0.5, "mode": TetraImageDocument.LAYER_MODE_ADDITION}, Vector2(6, 6), Vector2(6, 6))
	check_color(px(doc, 6, 6), Color(0.5, 0.7, 0.85, 1), "addition paint mode at half opacity")

	# Pressure scales the size: at half pressure a 10 px brush covers radius 2.5.
	var sized := new_doc(16, 16)
	var sized_layer := sized.get_active_layer()
	var settings := {"color": BLACK, "size": 10.0, "hardness": 1.0, "pressure_size": true}
	sized.stroke_begin(sized_layer, TetraImageDocument.TOOL_PENCIL, settings)
	sized.stroke_add_point(Vector2(8.5, 8.5), 0.5)
	sized.stroke_end()
	check_color(px(sized, 10, 8), BLACK, "half pressure: inside radius 2.5")
	check_color(px(sized, 11, 8), WHITE, "half pressure: outside radius 2.5")
	sized.undo()
	sized.stroke_begin(sized_layer, TetraImageDocument.TOOL_PENCIL, settings)
	sized.stroke_add_point(Vector2(8.5, 8.5), 1.0)
	sized.stroke_end()
	check_color(px(sized, 13, 8), BLACK, "full pressure: inside radius 5")
	check_color(px(sized, 14, 8), WHITE, "full pressure: outside radius 5")

	# Pressure scales the dab opacity; GIMP's coverage rule gives opacity squared for one dab.
	var faded := new_doc(12, 12)
	faded.stroke_begin(faded.get_active_layer(), TetraImageDocument.TOOL_PAINTBRUSH, {"color": BLACK, "size": 6.0, "hardness": 1.0, "pressure_opacity": true})
	faded.stroke_add_point(Vector2(6, 6), 0.5)
	faded.stroke_end()
	check_near(px(faded, 6, 6).r, 0.75, 1e-5, "pressure opacity")


func test_clone() -> void:
	var doc := new_doc(24, 10, Color(0, 0, 0, 0))
	var layer := doc.get_active_layer()
	doc.select_rect(Rect2i(2, 2, 4, 4))
	doc.fill(layer, RED)
	doc.select_none()
	var settings := {"size": 4.0, "hardness": 1.0, "source_point": Vector2(4, 4)}
	doc.stroke_line(layer, TetraImageDocument.TOOL_CLONE, settings, Vector2(14, 4), Vector2(14, 4))
	check_color(px(doc, 13, 3), RED, "clone copies the source")
	check_color(px(doc, 14, 4), RED, "clone copies the source (2)")
	check_color(px(doc, 18, 4), Color(0, 0, 0, 0), "clone is limited to the brush")
	check_color(px(doc, 3, 3), RED, "the source is untouched")

	# An explicit offset, and cloning transparency paints nothing.
	doc.stroke_line(layer, TetraImageDocument.TOOL_CLONE, {"size": 4.0, "hardness": 1.0, "source_offset": Vector2(-17, 3)}, Vector2(21, 1), Vector2(21, 1))
	check_color(px(doc, 21, 1), RED, "clone with a source offset")
	doc.stroke_line(layer, TetraImageDocument.TOOL_CLONE, {"size": 4.0, "hardness": 1.0, "source_offset": Vector2(0, 5)}, Vector2(4, 3), Vector2(4, 3))
	check_color(px(doc, 4, 3), RED, "cloning empty pixels over content changes nothing")


func test_smudge() -> void:
	var doc := new_doc(32, 16, BLUE)
	var layer := doc.get_active_layer()
	doc.select_rect(Rect2i(0, 0, 16, 16))
	doc.fill(layer, RED)
	doc.select_none()
	var settings := {"size": 8.0, "hardness": 1.0, "rate": 0.5}
	doc.stroke_line(layer, TetraImageDocument.TOOL_SMUDGE, settings, Vector2(8, 8), Vector2(26, 8))
	var dragged := px(doc, 18, 8)
	check(dragged.r > 0.2 and dragged.b > 0.0 and dragged.b < 1.0, "red is dragged into the blue half: %s" % dragged)
	check(px(doc, 18, 8).r > px(doc, 22, 8).r, "the smear fades along the stroke")
	check_near(dragged.a, 1.0, 1e-5, "smudging opaque pixels stays opaque")
	check_color(px(doc, 18, 14), BLUE, "pixels away from the stroke are untouched")
	check_color(px(doc, 10, 8), RED, "smudging a flat colour leaves it flat")
	check_eq(doc.get_undo_names()[-1], "Smudge", "smudge undo name")

	# With rate 0 nothing is carried.
	doc.undo()
	doc.stroke_line(layer, TetraImageDocument.TOOL_SMUDGE, {"size": 8.0, "hardness": 1.0, "rate": 0.0}, Vector2(8, 8), Vector2(26, 8))
	check_color(px(doc, 18, 8), BLUE, "rate 0 does not smear")


func test_spacing_and_cancel() -> void:
	# Spacing 300 % of a 4 px brush: one dab every 12 px.
	var doc := new_doc(48, 8)
	var layer := doc.get_active_layer()
	doc.stroke_line(layer, TetraImageDocument.TOOL_PENCIL, {"color": BLACK, "size": 4.0, "spacing": 300.0}, Vector2(4.5, 4.5), Vector2(40.5, 4.5))
	for x: int in [4, 16, 28, 40]:
		check_color(px(doc, x, 4), BLACK, "dab at %d" % x)
	for x: int in [10, 22, 34]:
		check_color(px(doc, x, 4), WHITE, "gap at %d" % x)
	# The spacing carries across the points of a stroke.
	doc.undo()
	doc.stroke_begin(layer, TetraImageDocument.TOOL_PENCIL, {"color": BLACK, "size": 4.0, "spacing": 300.0})
	for x: float in [4.5, 9.5, 14.5, 19.5, 24.5, 29.5]:
		doc.stroke_add_point(Vector2(x, 4.5))
	check_color(px(doc, 16, 4), BLACK, "dab placed when the distance is reached")
	check_color(px(doc, 28, 4), BLACK, "next dab after another 12 px")
	check_color(px(doc, 22, 4), WHITE, "no dab in between")

	# Cancelling restores the layer and leaves no undo step.
	var steps := doc.get_undo_names().size()
	doc.stroke_cancel()
	check(not doc.is_stroke_active(), "cancel ends the stroke")
	check_color(px(doc, 16, 4), WHITE, "cancel restores the pixels")
	check_eq(doc.get_undo_names().size(), steps, "cancel records nothing")
	check_eq(doc.stroke_begin(999, TetraImageDocument.TOOL_PENCIL, {}), ERR_DOES_NOT_EXIST, "a stroke needs a layer")


func save_stroke_sheet() -> void:
	var doc := new_doc(360, 240)
	var layer := doc.add_layer("paint")
	var wave := func(p_tool: int, p_settings: Dictionary, p_y: float, p_pressure: bool) -> void:
		doc.stroke_begin(layer, p_tool, p_settings)
		for i in 41:
			var t := i / 40.0
			doc.stroke_add_point(Vector2(20 + t * 320, p_y + sin(t * TAU * 1.5) * 14), sin(t * PI) if p_pressure else 1.0)
		doc.stroke_end()
	wave.call(TetraImageDocument.TOOL_PAINTBRUSH, {"color": Color(0.8, 0.05, 0.05, 1), "size": 24.0, "hardness": 0.3}, 30.0, false)
	wave.call(TetraImageDocument.TOOL_PAINTBRUSH, {"color": Color(0.02, 0.2, 0.7, 1), "size": 24.0, "hardness": 1.0, "opacity": 0.6}, 70.0, false)
	wave.call(TetraImageDocument.TOOL_PAINTBRUSH, {"color": Color(0.0, 0.3, 0.05, 1), "size": 28.0, "hardness": 0.6, "pressure_size": true}, 110.0, true)
	wave.call(TetraImageDocument.TOOL_AIRBRUSH, {"color": Color(0.3, 0.0, 0.4, 1), "size": 30.0, "hardness": 0.2, "flow": 0.15}, 150.0, false)
	wave.call(TetraImageDocument.TOOL_PENCIL, {"color": Color(0, 0, 0, 1), "size": 3.0}, 185.0, false)
	wave.call(TetraImageDocument.TOOL_ERASER, {"size": 10.0, "hardness": 0.8}, 60.0, false)
	doc.stroke_line(layer, TetraImageDocument.TOOL_SMUDGE, {"size": 26.0, "hardness": 0.5, "rate": 0.8}, Vector2(180, 10), Vector2(180, 200))
	doc.stroke_line(layer, TetraImageDocument.TOOL_CLONE, {"size": 40.0, "hardness": 0.7, "source_point": Vector2(60, 30)}, Vector2(60, 215), Vector2(300, 215))
	var path := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens/paint-strokes.png")
	check_eq(doc.composite().save_png(path), OK, "save the stroke sheet")
