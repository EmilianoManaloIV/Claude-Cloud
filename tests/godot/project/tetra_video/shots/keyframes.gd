extends RefCounted
## Keyframed effect parameters in the Effect Stack: the keyframe row of an
## animatable parameter, auto-key, the keyframe strip with real mouse and key
## events, and the monitor showing the interpolated frame.
##
##   scripts/screenshot.sh build/screens/video-keyframes.png Video res://tetra_video/shots/keyframes.gd

var _tree: SceneTree
var root: Window
var _checks := 0
var _failures := 0
var _last_mouse := Vector2()

var ws: Object
var pr: TetraVideoProject
var stack: Control
var strip: Control
var panel: Control
var clip := 0


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func mouse_button(p_control: Control, p_local: Vector2, p_pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = p_pressed
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_pressed else 0
	event.position = p_control.get_global_position() + p_local
	event.global_position = event.position
	root.push_input(event)
	await frames(1)


func mouse_move(p_control: Control, p_local: Vector2, p_held: bool = true) -> void:
	var event := InputEventMouseMotion.new()
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_held else 0
	event.position = p_control.get_global_position() + p_local
	event.global_position = event.position
	event.relative = event.position - _last_mouse
	_last_mouse = event.position
	root.push_input(event)
	await frames(1)


func click(p_control: Control, p_local: Vector2 = Vector2(-1, -1)) -> void:
	var at := p_local if p_local.x >= 0 else p_control.size * 0.5
	await mouse_move(p_control, at, false)
	await mouse_button(p_control, at, true)
	await mouse_button(p_control, at, false)
	await frames(2)


func key(p_key: Key) -> void:
	for pressed in [true, false]:
		var event := InputEventKey.new()
		event.keycode = p_key
		event.physical_keycode = p_key
		event.pressed = pressed
		root.push_input(event)
		await frames(1)
	await frames(1)


func keys() -> Array:
	var list := []
	for k in pr.get_effect_keyframes(clip, 0, "level"):
		list.push_back([k.frame, k.value, k.type])
	return list


func control(p_which: String) -> Control:
	return stack.get_keyframe_control("level", p_which)


## What the user does in the form: the panel reports a value the user set.
func edit_value(p_value: int) -> void:
	panel.set_value("level", p_value)
	panel.value_changed.emit("level", p_value)
	await frames(2)


func seek(p_frame: int) -> void:
	ws.seek(p_frame)
	for i in 60:
		await frames(1)
		if ws.get_playhead() == p_frame and stack.get_playhead() == p_frame:
			break


func monitor_pixel() -> Color:
	var image: Image = ws.get_monitor().get_frame_image()
	return image.get_pixel(image.get_width() / 2, image.get_height() / 8) if image else Color(0, 0, 0, 0)


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	root = p_tree.root
	root.gui_embed_subwindows = true
	var common: RefCounted = load("res://tetra_video/shots/video_common.gd").new()
	await common.setup(p_tree)
	ws = common.workspace
	pr = common.project
	clip = common.clips["blue"]
	stack = ws.get_effect_stack()
	strip = stack.get_keyframe_strip()
	panel = stack.get_param_panel()

	ws.select_clip(clip)
	check(ws.add_effect_to_selection("kdenlive:brightness") >= 0, "brightness added")
	ws.show_effect_stack()
	ws.get_timeline().zoom_to_fit()
	await seek(0)
	await common.settle(p_tree)

	# The row of an animatable parameter: only the toggle until it is switched on.
	check_eq(stack.get_animatable_params(), PackedStringArray(["level"]), "one animatable parameter")
	check(control("toggle") != null and control("toggle").is_visible_in_tree(), "the keyframe toggle is shown")
	check(not control("key").is_visible_in_tree() and not strip.is_visible_in_tree(), "no keyframe controls and no strip while constant")
	check(not stack.is_param_animated("level"), "constant")
	var undo_before := pr.get_undo_count()
	await click(control("toggle"))
	check(stack.is_param_animated("level") and control("toggle").button_pressed, "a click on the toggle animates the parameter")
	check_eq(keys(), [[0, 100.0, 1]], "first keyframe on the first frame of the clip with the constant's value")
	check(control("key").is_visible_in_tree() and control("type").is_visible_in_tree() and strip.is_visible_in_tree(), "keyframe controls and the strip appear")
	check_eq(strip.get_lane_count(), 1, "one lane")
	check_eq(control("key").tooltip_text, "Remove the keyframe at the playhead", "on a keyframe the button removes")
	check(control("previous").disabled and control("next").disabled, "no other keyframe to go to")

	# Auto-key: editing the value away from a keyframe makes one at the playhead.
	await seek(40)
	check_eq(control("key").tooltip_text, "Add a keyframe at the playhead", "between keyframes the button adds")
	undo_before = pr.get_undo_count()
	await edit_value(230)
	await edit_value(250)
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1]], "editing the value made a keyframe at the playhead")
	stack.commit_pending()
	check_eq(pr.get_undo_count(), undo_before + 1, "a run of edits is one undo step")
	# The add button takes the interpolated value; editing on a keyframe changes it.
	await seek(80)
	await click(control("key"))
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1], [80, 250.0, 1]], "the add button made a keyframe with the value held there")
	await edit_value(50)
	stack.commit_pending()
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1], [80, 50.0, 1]], "editing on a keyframe changes that keyframe")

	# The form follows the playhead.
	await seek(60)
	check_eq(panel.get_value("level"), 150, "the control shows the interpolated value at the playhead")
	check_eq(strip.get_playhead(), 60, "the strip's playhead follows")
	await seek(20)
	check_eq(panel.get_value("level"), 175, "and follows the playhead")

	# Previous and next keyframe.
	await click(control("next"))
	await frames(4)
	check_eq(ws.get_playhead(), 40, "next keyframe")
	await click(control("next"))
	await frames(4)
	check_eq(ws.get_playhead(), 80, "next again")
	check(control("next").disabled and not control("previous").disabled, "nothing after the last keyframe")
	await click(control("previous"))
	await click(control("previous"))
	await frames(4)
	check_eq(ws.get_playhead(), 0, "previous twice")

	# Interpolation type of the stretch under the playhead.
	await seek(50)
	var type: OptionButton = control("type")
	check_eq(type.get_item_count(), 35, "every MLT keyframe type is offered")
	check_eq(type.get_selected_id(), TetraVideoProject.KEYFRAME_LINEAR, "linear shown")
	type.select(type.get_item_index(TetraVideoProject.KEYFRAME_SMOOTH))
	type.item_selected.emit(type.get_item_index(TetraVideoProject.KEYFRAME_SMOOTH))
	await frames(2)
	check_eq(keys()[1], [40, 250.0, TetraVideoProject.KEYFRAME_SMOOTH], "the keyframe before the playhead got the type")
	await seek(10)
	check_eq(type.get_selected_id(), TetraVideoProject.KEYFRAME_LINEAR, "the selector shows the type of the stretch under the playhead")
	pr.undo()
	await frames(2)

	# The strip: click selects and seeks, drag moves in time, Delete removes.
	check_eq(strip.get_lane_frames(0), PackedInt32Array([0, 40, 80]), "the lane shows the three keyframes")
	await click(strip, strip.get_keyframe_position(0, 80))
	await frames(4)
	check_eq([strip.get_selected_lane(), strip.get_selected_frame(), ws.get_playhead()], [0, 80, 80], "a click selects the keyframe and moves the playhead there")
	undo_before = pr.get_undo_count()
	var from: Vector2 = strip.get_keyframe_position(0, 80)
	var to: Vector2 = strip.get_keyframe_position(0, 90)
	await mouse_move(strip, from, false)
	await mouse_button(strip, from, true)
	for i in range(1, 6):
		await mouse_move(strip, from.lerp(to, i / 5.0))
	await mouse_button(strip, to, false)
	await frames(3)
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1], [90, 50.0, 1]], "dragged from 80 to 90")
	check_eq([pr.get_undo_count(), strip.get_selected_frame()], [undo_before + 1, 90], "the drag is one undo step and keeps the selection")
	check_eq(pr.get_undo_name(), "Move Keyframe", "undo name")
	# A drag cannot pass onto another keyframe's frame or out of the clip.
	from = strip.get_keyframe_position(0, 90)
	await mouse_move(strip, from, false)
	await mouse_button(strip, from, true)
	await mouse_move(strip, Vector2(strip.size.x + 200, from.y))
	await mouse_button(strip, Vector2(strip.size.x + 200, from.y), false)
	await frames(3)
	check_eq(keys()[2][0], 99, "dragged past the end: stops on the last frame of the clip")
	pr.undo()
	pr.undo()
	await frames(2)
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1], [80, 50.0, 1]], "both drags undone")
	await click(strip, strip.get_keyframe_position(0, 40))
	await key(KEY_DELETE)
	check_eq(keys(), [[0, 100.0, 1], [80, 50.0, 1]], "Delete removes the selected keyframe")
	pr.undo()
	await frames(2)
	# A click beside the keyframes only moves the playhead.
	await click(strip, strip.get_keyframe_position(0, 60))
	await frames(4)
	check_eq([strip.get_selected_frame(), ws.get_playhead()], [-1, 60], "a click on the lane seeks")
	check(ws.get_timeline().get_clip_rect(clip).size.x > 0, "the clip is on the timeline (it carries the keyframe marks)")

	# Switching the toggle off makes the value at the playhead the constant.
	await click(control("toggle"))
	check(not stack.is_param_animated("level") and not control("key").is_visible_in_tree() and not strip.is_visible_in_tree(), "toggle off: constant again, controls and strip hidden")
	check_eq(pr.get_clip_effects(clip)[0].values.level, 150, "the constant is the value at the playhead")
	pr.undo()
	await frames(2)
	check_eq(keys(), [[0, 100.0, 1], [40, 250.0, 1], [80, 50.0, 1]], "undo brings the keyframes back")
	check_eq(pr.check_consistency(), "", "the project is consistent")

	# The monitor shows the interpolated frame: brighter at 250 % than at 100 %, darker at 50 %.
	await seek(0)
	await common.settle(p_tree)
	var plain := monitor_pixel()
	await seek(40)
	await common.settle(p_tree)
	var bright := monitor_pixel()
	await seek(80)
	await common.settle(p_tree)
	var dim := monitor_pixel()
	await seek(60)
	await common.settle(p_tree)
	var between := monitor_pixel()
	check(bright.get_luminance() > plain.get_luminance() + 0.1 and dim.get_luminance() < plain.get_luminance() - 0.05, "the monitor follows the keyframes: %s %s %s" % [plain, bright, dim])
	check(between.get_luminance() < bright.get_luminance() - 0.03 and between.get_luminance() > dim.get_luminance() + 0.03, "between two keyframes the monitor shows the interpolated level: %s" % between)
	check_eq(panel.get_value("level"), 150, "150 % at the playhead")
	print("screenshot video keyframes: %d checks, %d failed" % [_checks, _failures])
