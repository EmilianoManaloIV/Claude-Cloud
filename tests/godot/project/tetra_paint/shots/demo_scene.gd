extends RefCounted
## Builds the demonstration picture of the Image workspace drivers through the
## workspace's scripting surface: a 1024 x 768 image with three layers in
## different modes, a gradient and brush strokes.


static func build(p_workspace: Object) -> TetraImageDocument:
	var ws := p_workspace
	var doc: TetraImageDocument = ws.new_document(1024, 768, Color.WHITE)
	var canvas: Object = ws.get_image_canvas()

	# Background: a sky gradient, drawn with the Gradient tool.
	doc.layer_set_name(doc.get_active_layer(), "Sky")
	ws.set_foreground_color(Color("2b4c9b"))
	ws.set_background_color(Color("f6c98a"))
	ws.set_tool("gradient")
	canvas.tool_press(Vector2(512, 0))
	canvas.tool_drag(Vector2(512, 768))
	canvas.tool_release()

	# Hills painted with the paintbrush on a Multiply layer.
	var hills := doc.add_layer("Hills")
	doc.layer_set_mode(hills, TetraImageDocument.LAYER_MODE_MULTIPLY)
	ws.set_tool("paintbrush")
	ws.set_tool_option("paintbrush", "size", 150.0)
	ws.set_tool_option("paintbrush", "hardness", 80.0)
	ws.set_foreground_color(Color("3f8f5a"))
	_stroke(canvas, func(t: float) -> Vector2: return Vector2(t * 1024.0, 640.0 - 90.0 * sin(t * PI * 2.2)), 40)
	ws.set_foreground_color(Color("2f6f8f"))
	_stroke(canvas, func(t: float) -> Vector2: return Vector2(t * 1024.0, 730.0 - 40.0 * cos(t * PI * 3.0)), 40)

	# A sun and its glow on a Screen layer, with the airbrush and the brush.
	var light := doc.add_layer("Light")
	doc.layer_set_mode(light, TetraImageDocument.LAYER_MODE_SCREEN)
	doc.layer_set_opacity(light, 0.85)
	ws.set_tool("airbrush")
	ws.set_tool_option("airbrush", "size", 320.0)
	ws.set_tool_option("airbrush", "flow", 30.0)
	ws.set_foreground_color(Color("ffb347"))
	_stroke(canvas, func(t: float) -> Vector2: return Vector2(700.0, 250.0) + Vector2(30.0, 0.0).rotated(t * TAU), 24)
	ws.set_tool("paintbrush")
	ws.set_tool_option("paintbrush", "size", 110.0)
	ws.set_tool_option("paintbrush", "hardness", 95.0)
	ws.set_foreground_color(Color("fff2c0"))
	canvas.tool_press(Vector2(700, 250))
	canvas.tool_release()
	# Birds with the pencil.
	ws.set_tool("pencil")
	ws.set_tool_option("pencil", "size", 5.0)
	ws.set_foreground_color(Color.WHITE)
	for origin: Vector2 in [Vector2(250, 180), Vector2(340, 130), Vector2(420, 210)]:
		_stroke(canvas, func(t: float) -> Vector2: return origin + Vector2(t * 60.0, -18.0 * absf(sin(t * TAU))), 16)

	ws.set_tool("paintbrush")
	ws.set_tool_option("paintbrush", "size", 24.0)
	ws.set_tool_option("paintbrush", "hardness", 50.0)
	ws.set_foreground_color(Color("d94f3d"))
	return doc


static func _stroke(p_canvas: Object, p_path: Callable, p_steps: int) -> void:
	p_canvas.tool_press(p_path.call(0.0))
	for i in range(1, p_steps + 1):
		p_canvas.tool_drag(p_path.call(float(i) / p_steps))
	p_canvas.tool_release()
