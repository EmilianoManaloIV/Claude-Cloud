extends RefCounted
## Behaviour checks of the Video workspace. The workspace classes are part of
## the editor API and cannot be created by a headless test, so the checks run
## inside the editor and drive it with real mouse and key events.
##
##   scripts/screenshot.sh build/screens/video-checks.png Video res://tetra_video/shots/checks.gd

var _tree: SceneTree
var root: Window
var _checks := 0
var _failures := 0

var common: RefCounted
var ws: Object
var pr: TetraVideoProject
var tl: Control
var mon: Control
var refused := []
var _last_mouse := Vector2()


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	root = p_tree.root
	root.gui_embed_subwindows = true
	common = load("res://tetra_video/shots/video_common.gd").new()
	ws = ClassDB.class_call_static("TetraVideoWorkspace", "get_singleton")
	await _test_structure()
	await common.setup(p_tree)
	pr = common.project
	tl = ws.get_timeline()
	mon = ws.get_monitor()
	tl.edit_refused.connect(func(message: String) -> void: refused.append(message))
	await _test_bin()
	await _test_monitor()
	await _test_keys()
	await _test_key_scope()
	await _test_timeline_mouse()
	await _test_effects()
	await _test_bin_drag()
	await _test_menus()
	await _test_project_file()
	await _test_render()
	await _test_open_from_editor()
	print("screenshot video checks: %d checks, %d failed" % [_checks, _failures])


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func check_near(p_actual: float, p_expected: float, p_epsilon: float = 1e-4, p_what: String = "") -> bool:
	return check(absf(p_actual - p_expected) <= p_epsilon, "%s: expected %f, got %f" % [p_what, p_expected, p_actual])


func frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func key(p_key: Key, p_shift: bool = false, p_ctrl: bool = false, p_unicode: int = 0) -> void:
	for pressed in [true, false]:
		var event := InputEventKey.new()
		event.keycode = p_key
		event.physical_keycode = p_key
		event.unicode = p_unicode if pressed else 0
		event.shift_pressed = p_shift
		event.ctrl_pressed = p_ctrl
		event.pressed = pressed
		root.push_input(event)
		await frames(1)
	await frames(1)


func mouse_button(p_control: Control, p_local: Vector2, p_pressed: bool, p_shift: bool = false, p_button: MouseButton = MOUSE_BUTTON_LEFT, p_ctrl: bool = false) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = p_button
	event.pressed = p_pressed
	event.shift_pressed = p_shift
	event.ctrl_pressed = p_ctrl
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_pressed and p_button == MOUSE_BUTTON_LEFT else 0
	event.position = p_control.get_global_position() + p_local
	event.global_position = event.position
	root.push_input(event)
	await frames(1)


func mouse_move(p_control: Control, p_local: Vector2, p_held: bool = true) -> void:
	var event := InputEventMouseMotion.new()
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_held else 0
	event.position = p_control.get_global_position() + p_local
	event.global_position = event.position
	# The viewport measures the drag distance from the relative motion.
	event.relative = event.position - _last_mouse
	_last_mouse = event.position
	# A drop asks the window system where the pointer is, so the real pointer goes along.
	Input.warp_mouse(event.position)
	root.push_input(event)
	await frames(1)


func click(p_control: Control, p_local: Vector2, p_shift: bool = false) -> void:
	await mouse_move(p_control, p_local, false)
	await mouse_button(p_control, p_local, true, p_shift)
	await mouse_button(p_control, p_local, false, p_shift)


func drag(p_control: Control, p_from: Vector2, p_to: Vector2, p_target: Control = null) -> void:
	var target := p_target if p_target else p_control
	await mouse_move(p_control, p_from, false)
	await mouse_button(p_control, p_from, true)
	# Several steps: a drag only starts after the pointer has left the click tolerance.
	var from_global := p_control.get_global_position() + p_from
	var to_global := target.get_global_position() + p_to
	for i in range(1, 9):
		await mouse_move(target, from_global.lerp(to_global, i / 8.0) - target.get_global_position())
	await mouse_move(target, p_to)
	await mouse_button(target, p_to, false)
	await frames(2)


func clip(p_id: int) -> Dictionary:
	return pr.get_clip(p_id)


## Point in the middle of a frame's column at the height of a clip.
func at_frame(p_frame: int, p_clip: int) -> Vector2:
	return Vector2(tl.frame_to_x(p_frame) + tl.get_zoom() * 0.5, tl.get_clip_rect(p_clip).get_center().y)


func clip_center(p_id: int) -> Vector2:
	return tl.get_clip_rect(p_id).get_center()


func _test_structure() -> void:
	check(ws != null, "workspace singleton")
	check_eq(ws.get_parent(), EditorInterface.get_editor_main_screen(), "workspace is a main screen child")
	check(ws.is_visible_in_tree(), "workspace visible on the Video screen")
	var project: TetraVideoProject = ws.get_project()
	check_eq(project.get_track_count(), 5, "default tracks")
	var video := 0
	var audio := 0
	for i in project.get_track_count():
		if project.get_track_kind(i) == TetraVideoProject.TRACK_VIDEO:
			video += 1
		else:
			audio += 1
	check_eq([video, audio], [3, 2], "3 video + 2 audio tracks")
	check_eq(project.get_clip_ids().size(), 0, "new project is empty")
	check(not ws.is_modified(), "new project is unmodified")
	check(ws.get_engine() != null and ws.get_bin() != null and ws.get_monitor() != null and ws.get_timeline() != null
			and ws.get_effects_browser() != null and ws.get_effect_stack() != null and ws.get_render_dialog() != null, "parts exist")
	check(ws.get_effects_browser().get_operations().size() > 50, "effects catalog is listed")
	await frames()


func _test_bin() -> void:
	var list: ItemList = ws.get_bin().find_children("*", "ItemList", true, false)[0]
	check_eq(list.item_count, pr.get_bin_clip_ids().size(), "bin lists every clip")
	check(list.get_item_text(0).contains("Blue") and list.get_item_text(0).contains("00:00:04:00"), "bin row shows name and duration: " + list.get_item_text(0))
	# Thumbnails are made on a worker thread and arrive a little later.
	for i in 40:
		if list.get_item_icon(list.item_count - 1) != null and list.get_item_icon(0) != null:
			break
		await _tree.create_timer(0.1).timeout
	var icon := list.get_item_icon(0)
	if check(icon != null, "bin thumbnail arrived"):
		var pixel := icon.get_image().get_pixel(icon.get_width() / 2, icon.get_height() / 2)
		check(pixel.b > pixel.r and pixel.b > 0.5, "thumbnail of the blue clip is blue: %s" % pixel)
	if common.bin.has("video"):
		check(common.bin["video"] >= 0, "test video imported")
		check_eq(ws.import_file(common.make_test_video()), common.bin["video"], "importing a file twice finds the clip")
		check_eq(pr.get_bin_clip(common.bin["video"])["length"], 100, "probed length of the 4 s test video")
		check_eq(pr.get_linked_clip(common.clips["video"]) >= 0, true, "AV clip has a linked audio partner")
	check_eq(ws.import_file("/nonexistent/file.mp4"), -1, "unreadable file is refused")
	# Double-click shows the clip in the clip monitor.
	ws.get_bin().clip_activated.emit(common.bin["orange"])
	await common.settle(_tree)
	check(mon.is_clip_mode(), "bin activation switches to the clip monitor")
	check_eq(mon.get_clip_bin_id(), common.bin["orange"], "clip monitor shows the activated clip")
	var image: Image = mon.get_frame_image()
	if check(image != null and not image.is_empty(), "clip monitor has a frame"):
		var pixel := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		check(pixel.r > 0.7 and pixel.b < 0.3, "clip monitor shows orange: %s" % pixel)
	mon.show_project()
	await common.settle(_tree)
	check(not mon.is_clip_mode(), "back to the project monitor")


func _test_monitor() -> void:
	ws.seek(10)
	await common.settle(_tree)
	check_eq(ws.get_playhead(), 10, "seek")
	check_eq(tl.get_playhead(), 10, "timeline follows seek")
	var image: Image = mon.get_frame_image()
	if check(image != null and not image.is_empty(), "monitor has a frame"):
		check_near(float(image.get_width()) / image.get_height(), 16.0 / 9.0, 0.01, "frame aspect")
		var pixel := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		check(absf(pixel.r - 0.15) < 0.08 and absf(pixel.g - 0.35) < 0.08 and absf(pixel.b - 0.8) < 0.08, "frame 10 shows the blue clip: %s" % pixel)
	ws.seek(230)
	await common.settle(_tree)
	image = mon.get_frame_image()
	if check(image != null and not image.is_empty(), "monitor has a frame at 230"):
		var pixel := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		check(absf(pixel.r - 0.9) < 0.08 and absf(pixel.g - 0.5) < 0.08 and absf(pixel.b - 0.1) < 0.08, "frame 230 shows the orange clip: %s" % pixel)
	var timecode: LineEdit = mon.find_children("*", "LineEdit", true, false)[0]
	check_eq(timecode.text, "00:00:09:05", "timecode readout")
	timecode.text = "00:00:02:00"
	timecode.text_submitted.emit(timecode.text)
	await frames()
	check_eq(ws.get_playhead(), 50, "typed timecode seeks")
	timecode.text = "75"
	timecode.text_submitted.emit(timecode.text)
	await frames()
	check_eq(ws.get_playhead(), 75, "typed frame number seeks")

	var full_width := pr.get_width()
	for divisor in [1, 2, 4]:
		mon.set_preview_divisor(divisor)
		await common.settle(_tree)
		image = mon.get_frame_image()
		check_eq(image.get_width() if image else 0, full_width / divisor, "preview width at 1/%d" % divisor)
	mon.set_preview_divisor(2)

	# An edit re-renders the frame: hiding the title's track at a frame it covers changes the picture.
	ws.seek(60)
	await common.settle(_tree)
	var before: Image = mon.get_frame_image()
	var title_track: int = clip(common.clips["title"])["track"]
	pr.set_track_hidden(title_track, true)
	await common.settle(_tree, 0.8)
	var after: Image = mon.get_frame_image()
	check(before.get_data() != after.get_data(), "monitor re-renders after an edit")
	var pixel := after.get_pixel(after.get_width() / 2, after.get_height() / 8)
	check(pixel.b > 0.6 and pixel.r < 0.3, "with the title track hidden the blue clip shows: %s" % pixel)
	pr.undo()
	await common.settle(_tree)

	mon.set_loop(true)
	check(mon.is_loop(), "loop toggle")
	mon.set_loop(false)


func _test_keys() -> void:
	ws.focus_timeline()
	await frames()
	ws.seek(20)
	await key(KEY_RIGHT)
	check_eq(ws.get_playhead(), 21, "Right steps one frame forward")
	await key(KEY_LEFT)
	await key(KEY_LEFT)
	check_eq(ws.get_playhead(), 19, "Left steps one frame back")
	await key(KEY_END)
	check_eq(ws.get_playhead(), pr.get_duration() - 1, "End goes to the last frame")
	await key(KEY_HOME)
	check_eq(ws.get_playhead(), 0, "Home goes to the start")

	await key(KEY_SPACE)
	check(ws.is_playing(), "Space plays")
	await _tree.create_timer(0.8).timeout
	await key(KEY_SPACE)
	check(not ws.is_playing(), "Space pauses")
	check(ws.get_playhead() > 3, "playback advanced the playhead: %d" % ws.get_playhead())
	var paused_at: int = ws.get_playhead()
	await _tree.create_timer(0.3).timeout
	check_eq(ws.get_playhead(), paused_at, "paused playhead stays")

	ws.seek(100)
	await key(KEY_L)
	check(ws.is_playing(), "L plays forward")
	await _tree.create_timer(0.6).timeout
	await key(KEY_K)
	check(not ws.is_playing(), "K pauses")
	check(ws.get_playhead() > 100, "L moved forward: %d" % ws.get_playhead())
	var position: int = ws.get_playhead()
	await key(KEY_J)
	await _tree.create_timer(0.6).timeout
	await key(KEY_K)
	check(ws.get_playhead() < position, "J moved backward: %d < %d" % [ws.get_playhead(), position])
	mon.stop()
	await frames()
	check(not ws.is_playing(), "stop")

	await key(KEY_X)
	check_eq(ws.get_tool(), 1, "X selects the razor")
	await key(KEY_S)
	check_eq(ws.get_tool(), 0, "S selects the selection tool")

	ws.seek(40)
	var markers := pr.get_markers().size()
	var undo_count := pr.get_undo_count()
	await key(KEY_M)
	check(pr.has_marker(40), "M adds a marker at the playhead")
	check_eq(pr.get_undo_count(), undo_count + 1, "marker is one undo step")
	await key(KEY_Z, false, true)
	check_eq(pr.get_markers().size(), markers, "Ctrl+Z undoes")
	await key(KEY_Z, true, true)
	check(pr.has_marker(40), "Ctrl+Shift+Z redoes")
	pr.undo()

	# Shift+R cuts every clip under the playhead when nothing is selected.
	ws.clear_selection()
	ws.seek(60)
	var count := pr.get_clip_ids().size()
	await key(KEY_R, true)
	check_eq(pr.get_clip_ids().size(), count + 2, "Shift+R splits the blue and the title clip at the playhead")
	check_eq(clip(common.clips["blue"])["length"], 60, "left part ends at the playhead")
	await key(KEY_Z, false, true)
	check_eq(pr.get_clip_ids().size(), count, "split undone in one step")


func _test_key_scope() -> void:
	# Typing in a text field of the workspace does not trigger the single-key shortcuts.
	var markers := pr.get_markers().size()
	ws.show_effects()
	var browser: Object = ws.get_effects_browser()
	browser.focus_search()
	await frames()
	await key(KEY_M, false, false, "m".unicode_at(0))
	check_eq(browser.get_filter_text(), "m", "the key is typed into the search field")
	check_eq(pr.get_markers().size(), markers, "M in a text field adds no marker")
	browser.set_filter_text("")

	# On another main screen the workspace's keys do nothing.
	EditorInterface.set_main_screen_editor("2D")
	await frames(4)
	check(not ws.is_visible_in_tree(), "workspace hidden on the 2D screen")
	var playhead: int = ws.get_playhead()
	await key(KEY_SPACE)
	await key(KEY_M)
	await key(KEY_RIGHT)
	check(not ws.is_playing(), "Space outside the workspace does not play")
	check_eq(pr.get_markers().size(), markers, "M outside the workspace adds no marker")
	check_eq(ws.get_playhead(), playhead, "Right outside the workspace does not step")
	EditorInterface.set_main_screen_editor("Video")
	await frames(4)
	check(ws.is_visible_in_tree(), "workspace visible again")
	ws.focus_timeline()
	await frames()


func _test_timeline_mouse() -> void:
	var blue: int = common.clips["blue"]
	var orange: int = common.clips["orange"]
	var title: int = common.clips["title"]
	tl.zoom_to_fit()
	await frames()
	check(tl.frame_to_x(pr.get_duration()) <= tl.size.x, "zoom to fit shows the whole project")
	check(tl.get_clip_rect(blue).size.x > 20.0, "clip has a rectangle")

	# Selection.
	await click(tl, clip_center(orange))
	check_eq(Array(ws.get_selected_clips()), [orange], "click selects")
	await click(tl, clip_center(blue), true)
	var selected := Array(ws.get_selected_clips())
	selected.sort()
	var expected := [blue, orange]
	expected.sort()
	check_eq(selected, expected, "Shift+click adds to the selection")
	var v3: int = common.track_of(TetraVideoProject.TRACK_VIDEO, 2)
	await click(tl, tl.get_track_rect(v3).get_center())
	check_eq(ws.get_selected_clips().size(), 0, "click on an empty lane clears the selection")

	# Ruler: click and drag seek.
	var ruler: Rect2 = tl.get_ruler_rect()
	await click(tl, Vector2(tl.frame_to_x(120) + 0.5, ruler.get_center().y))
	check_eq(ws.get_playhead(), 120, "click on the ruler seeks")
	await drag(tl, Vector2(tl.frame_to_x(120) + 0.5, ruler.get_center().y), Vector2(tl.frame_to_x(30) + 0.5, ruler.get_center().y))
	check_eq(ws.get_playhead(), 30, "dragging on the ruler scrubs")

	# Move without snapping: exact frame arithmetic.
	var zoom: float = tl.get_zoom()
	tl.set_snapping(false)
	var undo_count := pr.get_undo_count()
	var from := clip_center(orange)
	await drag(tl, from, from + Vector2(40 * zoom, 0))
	check_eq(clip(orange)["position"], 240, "drag moves the clip by the dragged frames")
	check_eq(pr.get_undo_count(), undo_count + 1, "a drag is one undo step")
	await key(KEY_Z, false, true)
	check_eq(clip(orange)["position"], 200, "Ctrl+Z undoes the move")

	# Snapping: dropping a few pixels away from another clip's edge lands on it.
	pr.move_clip(orange, clip(orange)["track"], 240)
	await frames()
	tl.set_snapping(true)
	from = clip_center(orange)
	await drag(tl, from, from + Vector2(-40 * zoom + 6.0, 0))
	check_eq(clip(orange)["position"], 200, "the dragged clip snaps to the neighbour's end")
	# ... and to the playhead.
	pr.move_clip(orange, clip(orange)["track"], 300)
	ws.seek(250)
	tl.zoom_to_fit()
	await frames()
	zoom = tl.get_zoom()
	from = clip_center(orange)
	await drag(tl, from, from + Vector2(-50 * zoom - 5.0, 0))
	check_eq(clip(orange)["position"], 250, "the dragged clip snaps to the playhead")
	tl.set_snapping(false)

	# A drop onto another clip is refused and the clip snaps back.
	refused.clear()
	var state := pr.get_state()
	from = clip_center(orange)
	await drag(tl, from, clip_center(blue))
	check_eq(clip(orange)["position"], 250, "an overlapping drop leaves the clip where it was")
	check(pr.get_state() == state, "refused drop changes nothing")
	check(refused.size() > 0, "refused drop reports why")
	check_eq(pr.check_consistency(), "", "document is sound")

	# Across tracks.
	from = clip_center(title)
	await drag(tl, from, Vector2(from.x, tl.get_track_rect(v3).get_center().y))
	check_eq(clip(title)["track"], v3, "dragging vertically changes the track")
	check_eq(clip(title)["position"], 25, "vertical drag keeps the position")

	# A video clip cannot go to an audio track.
	var a1: int = common.track_of(TetraVideoProject.TRACK_AUDIO, 0)
	var a2: int = common.track_of(TetraVideoProject.TRACK_AUDIO, 1)
	from = clip_center(title)
	await drag(tl, from, Vector2(from.x, tl.get_track_rect(a2).get_center().y))
	check_eq(clip(title)["track"], v3, "a video clip does not move to an audio track")

	# Linked partners move together.
	if common.clips.has("video"):
		var video: int = common.clips["video"]
		var partner := pr.get_linked_clip(video)
		check_eq(clip(partner)["track"], a1, "the audio half sits on A1")
		await drag(tl, at_frame(150, video), at_frame(170, video))
		check_eq(clip(video)["position"], 120, "linked video moved")
		check_eq(clip(partner)["position"], 120, "linked audio followed")
		await click(tl, clip_center(video))
		check_eq(ws.get_selected_clips().size(), 2, "clicking one half selects both linked clips")

	# Trim by dragging an edge.
	await drag(tl, at_frame(99, blue), at_frame(79, blue))
	check_eq(clip(blue)["length"], 80, "dragging the right edge trims the end")
	check_eq(clip(blue)["position"], 0, "trimming the end keeps the start")
	await drag(tl, at_frame(0, blue), at_frame(10, blue))
	check_eq(clip(blue)["position"], 10, "dragging the left edge trims the start")
	check_eq(clip(blue)["length"], 70, "trimming the start keeps the end")

	# Razor.
	ws.set_tool(1)
	var count := pr.get_clip_ids().size()
	await click(tl, at_frame(60, title))
	check_eq(pr.get_clip_ids().size(), count + 1, "razor click splits the clip")
	check_eq(clip(title)["length"], 35, "razor cut at the clicked frame")
	var right_part := pr.get_clip_at(v3, 60)
	check(right_part >= 0 and right_part != title, "right part exists")
	check_eq(clip(right_part)["position"], 60, "right part starts at the cut")
	ws.set_tool(0)

	# Delete and ripple delete.
	await click(tl, clip_center(right_part))
	await key(KEY_DELETE)
	check(not pr.has_clip(right_part), "Delete removes the selection")
	check(pr.has_clip(title), "Delete leaves other clips")
	await key(KEY_Z, false, true)
	check(pr.has_clip(right_part), "Delete undone")
	await click(tl, clip_center(title))
	await key(KEY_DELETE, true)
	check(not pr.has_clip(title), "Shift+Delete removes the clip")
	check_eq(clip(right_part)["position"], 25, "Shift+Delete closes the gap")

	# Zoom with Ctrl+wheel keeps the frame under the pointer.
	var before_zoom: float = tl.get_zoom()
	var anchor := Vector2(tl.frame_to_x(100), tl.get_track_rect(v3).get_center().y)
	await mouse_move(tl, anchor, false)
	await mouse_button(tl, anchor, true, false, MOUSE_BUTTON_WHEEL_UP, true)
	await mouse_button(tl, anchor, false, false, MOUSE_BUTTON_WHEEL_UP, true)
	await frames()
	check(tl.get_zoom() > before_zoom, "Ctrl+wheel zooms in")
	check_near(tl.frame_to_x(100), anchor.x, 1.5, "zoom keeps the frame under the pointer")
	tl.zoom_to_fit()
	await frames()

	# Track header toggles.
	var v1: int = common.track_of(TetraVideoProject.TRACK_VIDEO, 0)
	await click(tl, tl.get_track_header_button_rect(v1, 0).get_center())
	check(pr.is_track_hidden(v1), "eye button hides a video track")
	await click(tl, tl.get_track_header_button_rect(v1, 0).get_center())
	check(not pr.is_track_hidden(v1), "eye button shows it again")
	await click(tl, tl.get_track_header_button_rect(a1, 0).get_center())
	check(pr.is_track_muted(a1), "speaker button mutes an audio track")
	await click(tl, tl.get_track_header_button_rect(a1, 0).get_center())
	await click(tl, tl.get_track_header_button_rect(v1, 1).get_center())
	check(pr.is_track_locked(v1), "lock button locks a track")
	# Clips on a locked track cannot be dragged.
	from = clip_center(orange)
	var locked_position: int = clip(orange)["position"]
	await drag(tl, from, from + Vector2(-30 * zoom, 0))
	check_eq(clip(orange)["position"], locked_position, "a clip on a locked track does not move")
	await click(tl, tl.get_track_header_button_rect(v1, 1).get_center())
	check(not pr.is_track_locked(v1), "lock button unlocks")
	check_eq(pr.check_consistency(), "", "document is sound after the mouse edits")


func _test_effects() -> void:
	var blue: int = common.clips["blue"]
	var stack: Object = ws.get_effect_stack()
	ws.select_clip(blue)
	ws.seek(clip(blue)["position"] + 5)
	await common.settle(_tree)
	check_eq(stack.get_clip(), blue, "effect stack follows the selection")
	var plain: Image = mon.get_frame_image()
	check_eq(ws.add_effect_to_selection("kdenlive:no-such-effect"), 0, "unknown effect is refused")
	var undo_count := pr.get_undo_count()
	check_eq(ws.add_effect_to_selection("kdenlive:invert"), 1, "effect added to the selected clip")
	check_eq(pr.get_undo_count(), undo_count + 1, "adding an effect is one undo step")
	check_eq(stack.get_effect_count(), 1, "effect stack lists it")
	await common.settle(_tree, 0.8)
	var inverted: Image = mon.get_frame_image()
	var a := plain.get_pixel(plain.get_width() / 2, plain.get_height() / 2)
	var b := inverted.get_pixel(inverted.get_width() / 2, inverted.get_height() / 2)
	check(absf(a.r + b.r - 1.0) < 0.1 and absf(a.b + b.b - 1.0) < 0.1, "monitor shows the inverted frame: %s -> %s" % [a, b])

	# The browser adds on activation.
	var browser: Object = ws.get_effects_browser()
	browser.operation_activated.emit("kdenlive:vignette")
	await frames()
	check_eq(pr.get_clip_effect_count(blue), 2, "double-click in the browser adds to the selected clip")
	# Dropping an effect on a clip adds it to that clip, selected or not.
	var orange: int = common.clips["orange"]
	tl.effect_dropped.emit(orange, "kdenlive:sepia")
	await frames()
	check_eq(pr.get_clip_effect_count(orange), 1, "an effect dropped on a clip is added to it")
	# An audio effect does not fit a video clip.
	var audio_effect := ""
	for operation: Dictionary in browser.get_operations():
		if operation.get("kind", "") == "audio_effect":
			audio_effect = operation["id"]
			break
	if check(audio_effect != "", "catalog has audio effects"):
		check_eq(ws.add_effect_to_selection(audio_effect), 0, "an audio effect is not added to a video clip")

	# Parameters: rapid edits make one undo step.
	ws.select_clip(blue)
	await frames()
	stack.select_effect(1)
	await frames()
	check_eq(stack.get_selected_effect(), 1, "effect selected in the stack")
	var panel: Object = stack.get_param_panel()
	check(panel.has_param("smooth"), "panel shows the vignette's parameters")
	undo_count = pr.get_undo_count()
	var effects: Array = pr.get_clip_effects(blue)
	var old_value: float = effects[1]["values"].get("smooth", -1.0)
	for value in [10.0, 20.0, 30.0]:
		panel.set_value("smooth", value)
		panel.value_changed.emit("smooth", value)
		await frames(1)
	effects = pr.get_clip_effects(blue)
	check_eq(float(effects[1]["values"]["smooth"]), 30.0, "panel edit reaches the project")
	stack.commit_pending()
	check_eq(pr.get_undo_count(), undo_count + 1, "rapid parameter edits are one undo step")
	pr.undo()
	await frames()
	effects = pr.get_clip_effects(blue)
	check_eq(float(effects[1]["values"].get("smooth", -1.0)), old_value, "undo restores the old value")
	check_eq(float(panel.get_value("smooth")), old_value, "panel shows the restored value")

	# Enable, reorder, remove through the project; the stack follows.
	pr.set_clip_effect_enabled(blue, 0, false)
	await common.settle(_tree, 0.8)
	var disabled: Image = mon.get_frame_image()
	b = disabled.get_pixel(disabled.get_width() / 2, disabled.get_height() / 2)
	check(absf(a.b - b.b) < 0.1 and absf(a.r - b.r) < 0.1, "a disabled effect is not rendered: %s" % b)
	pr.remove_clip_effect(blue, 0)
	await frames()
	check_eq(stack.get_effect_count(), 1, "stack follows removals")
	ws.clear_selection()
	await frames()
	check_eq(stack.get_clip(), -1, "stack is empty without a selection")


func _test_bin_drag() -> void:
	ws.get_bin().show()
	var list: ItemList = ws.get_bin().find_children("*", "ItemList", true, false)[0]
	var v2: int = common.track_of(TetraVideoProject.TRACK_VIDEO, 1)
	var index := -1
	for i in list.item_count:
		if list.get_item_metadata(i) == common.bin["orange"]:
			index = i
	var before := pr.get_clips_in_track(v2).size()
	tl.set_snapping(true)
	ws.seek(150)
	await frames()
	# Dropped a few pixels beside the playhead: the new clip starts on it.
	var target := Vector2(tl.frame_to_x(150) + 5.0, tl.get_track_rect(v2).get_center().y)
	await drag(list, list.get_item_rect(index).get_center(), target, tl)
	check_eq(pr.get_clips_in_track(v2).size(), before + 1, "dragging from the bin inserts a clip")
	var inserted := pr.get_clip_at(v2, 150)
	if check(inserted >= 0, "inserted at the drop position, snapped to the playhead"):
		check_eq(clip(inserted)["position"], 150, "snapped start")
		check_eq(clip(inserted)["bin_id"], common.bin["orange"], "the dragged bin clip")
		check_eq(clip(inserted)["length"], 75, "whole clip inserted")
	# Dropping onto an occupied place is refused.
	var count := pr.get_clip_ids().size()
	await drag(list, list.get_item_rect(index).get_center(), Vector2(tl.frame_to_x(160), tl.get_track_rect(v2).get_center().y), tl)
	check_eq(pr.get_clip_ids().size(), count, "a drop onto another clip inserts nothing")
	# A video-only clip does not go to an audio track.
	var a2: int = common.track_of(TetraVideoProject.TRACK_AUDIO, 1)
	await drag(list, list.get_item_rect(index).get_center(), Vector2(tl.frame_to_x(10), tl.get_track_rect(a2).get_center().y), tl)
	check_eq(pr.get_clips_in_track(a2).size(), 0, "a colour clip cannot be dropped on an audio track")

	# Delete from the bin: an unused clip goes at once.
	var spare: int = ws.add_color_clip("Spare", Color.RED, 50)
	await frames()
	ws.get_bin().select_clip(spare)
	ws.get_bin().delete_selected()
	await frames()
	check(not pr.has_bin_clip(spare), "Delete removes an unused bin clip")


## Chooses an item of one of the workspace's menus, as a click on it would.
func menu(p_menu: String, p_item: String) -> bool:
	for button: MenuButton in ws.find_children("*", "MenuButton", true, false):
		if button.text != p_menu:
			continue
		var popup := button.get_popup()
		for i in popup.item_count:
			if popup.get_item_text(i) == p_item:
				if popup.is_item_disabled(i):
					return false
				popup.id_pressed.emit(popup.get_item_id(i))
				await frames()
				return true
	return false


## The visible dialog of the workspace, null when none is open.
func open_dialog() -> AcceptDialog:
	for dialog: AcceptDialog in ws.find_children("*", "AcceptDialog", true, false):
		if dialog.visible:
			return dialog
	return null


func _test_menus() -> void:
	ws.clear_selection()
	var tracks := pr.get_track_count()
	check(await menu("Timeline", "Add Video Track"), "menu item Add Video Track")
	check_eq(pr.get_track_count(), tracks + 1, "Add Video Track")
	var added: int = common.track_of(TetraVideoProject.TRACK_VIDEO, 3)
	check_eq(pr.get_track_name(added), "V4", "the new track is V4")
	# Delete Track removes the track whose header was clicked last.
	await click(tl, Vector2(8, tl.get_track_rect(added).get_center().y))
	check_eq(tl.get_active_track(), added, "clicking a header makes the track active")
	check(await menu("Timeline", "Delete Track"), "menu item Delete Track")
	check_eq(pr.get_track_count(), tracks, "Delete Track")
	check(await menu("Timeline", "Add Audio Track"), "menu item Add Audio Track")
	check_eq(pr.get_track_count(), tracks + 1, "Add Audio Track")
	pr.undo()

	# Insert Space pushes everything from the playhead on; Remove Space closes a gap again.
	tl.set_active_track(-1)
	var orange: int = common.clips["orange"]
	var position: int = clip(orange)["position"]
	ws.seek(position)
	check(await menu("Timeline", "Insert Space..."), "menu item Insert Space")
	var dialog := open_dialog()
	if check(dialog != null, "Insert Space asks for the length"):
		var seconds: SpinBox = dialog.find_children("*", "SpinBox", true, false)[0]
		seconds.value = 2.0
		dialog.get_ok_button().pressed.emit()
		await frames()
		check_eq(clip(orange)["position"], position + 50, "Insert Space moved the clip by 2 s")
		ws.seek(position + 10)
		tl.set_active_track(clip(orange)["track"])
		check(await menu("Timeline", "Remove Space"), "menu item Remove Space")
		# The whole gap goes: the clip lands on the end of its left neighbour.
		var neighbour_end := 0
		for id in pr.get_clips_in_track(clip(orange)["track"]):
			if id != orange and clip(id)["position"] < position:
				neighbour_end = maxi(neighbour_end, clip(id)["position"] + clip(id)["length"])
		check_eq(clip(orange)["position"], neighbour_end, "Remove Space closed the gap")
		tl.set_active_track(-1)

	ws.seek(33)
	check(await menu("Timeline", "Add Marker"), "menu item Add Marker")
	check(pr.has_marker(33), "Add Marker at the playhead")
	pr.undo()

	# Project Settings: a preset fills in the fields, OK applies them as one undoable step.
	check(await menu("Project", "Project Settings..."), "menu item Project Settings")
	dialog = open_dialog()
	if check(dialog != null, "Project Settings dialog opens"):
		var presets: OptionButton = dialog.find_children("*", "OptionButton", true, false)[0]
		var wanted := -1
		for i in presets.item_count:
			if presets.get_item_text(i).contains("720") and presets.get_item_text(i).contains("30"):
				wanted = i
				break
		if check(wanted >= 0, "a 720p 30 fps preset is offered"):
			presets.select(wanted)
			presets.item_selected.emit(wanted)
			dialog.get_ok_button().pressed.emit()
			await frames()
			check_eq([pr.get_width(), pr.get_height()], [1280, 720], "profile size applied")
			check_near(pr.get_fps(), 30.0, 0.05, "profile fps applied")
			pr.undo()
			check_eq([pr.get_width(), pr.get_height()], [1920, 1080], "profile change undone")
		else:
			dialog.hide()
	await common.settle(_tree)

	# The bin's dialogs.
	var bin_count := pr.get_bin_clip_ids().size()
	ws.get_bin().popup_add_color()
	await frames()
	dialog = open_dialog()
	if check(dialog != null, "Add Color Clip dialog opens"):
		dialog.get_ok_button().pressed.emit()
		await frames()
		check_eq(pr.get_bin_clip_ids().size(), bin_count + 1, "Add Color Clip adds a bin clip")
	ws.get_bin().popup_add_title()
	await frames()
	dialog = open_dialog()
	if check(dialog != null, "Add Title Clip dialog opens"):
		dialog.get_ok_button().pressed.emit()
		await frames()
		check_eq(pr.get_bin_clip_ids().size(), bin_count + 2, "Add Title Clip adds a bin clip")
	# Files handed over by a drop from the FileSystem dock or the OS.
	var before := pr.get_bin_clip_ids().size()
	ws.get_bin().import_requested.emit(PackedStringArray(["/nonexistent/clip.mp4"]))
	await frames()
	check_eq(pr.get_bin_clip_ids().size(), before, "an unreadable dropped file adds nothing")


func _test_open_from_editor() -> void:
	# Files opened from the FileSystem dock arrive through the plugin's edit().
	var path: String = common.tmp_dir().path_join("tetra_video_checks.mlt")
	ws.new_project()
	await frames()
	EditorInterface.set_main_screen_editor("2D")
	await frames(4)
	var file := ResourceLoader.load(path)
	if check(file != null and file.get_class() == "TetraVideoProjectFile", "an .mlt file loads as TetraVideoProjectFile"):
		EditorInterface.edit_resource(file)
		await common.settle(_tree)
		check(ws.is_visible_in_tree(), "opening an .mlt file switches to the Video workspace")
		check_eq(ws.get_project_path(), path, "the .mlt file is the open project")
		check(ws.get_project().get_clip_ids().size() > 0, "its clips are loaded")
	var video: String = common.make_test_video()
	if video != "":
		ws.new_project()
		await frames()
		var media := ResourceLoader.load(video)
		if check(media != null and media.get_class() == "TetraVideoMediaFile", "an .mp4 file loads as TetraVideoMediaFile"):
			EditorInterface.edit_resource(media)
			await common.settle(_tree)
			check_eq(ws.get_project().get_bin_clip_ids().size(), 1, "opening a video file adds it to the bin")
			check(ws.get_monitor().is_clip_mode(), "and previews it in the clip monitor")
			ws.get_monitor().show_project()


func _test_project_file() -> void:
	var path: String = common.tmp_dir().path_join("tetra_video_checks.mlt")
	check(ws.is_modified(), "edited project is modified")
	check_eq(ws.save_project(path), OK, "save")
	check(FileAccess.file_exists(path), "project file written")
	check(not ws.is_modified(), "saved project is unmodified")
	check_eq(ws.get_project_path(), path, "project path")
	var state := pr.get_state()
	ws.new_project()
	await frames()
	check_eq(ws.get_project().get_clip_ids().size(), 0, "New gives an empty project")
	check_eq(ws.get_project_path(), "", "new project has no path")
	check_eq(ws.open_project(path), OK, "open")
	await common.settle(_tree)
	pr = ws.get_project()
	common.project = pr
	check_eq(pr.get_clip_ids().size(), state["clips"].size() if state.has("clips") else pr.get_clip_ids().size(), "clips restored")
	check(pr.get_state() == state, "opened project equals the saved one")
	check_eq(ws.open_project("/nonexistent/project.mlt") != OK, true, "missing project file is an error")
	check(pr == ws.get_project() or ws.get_project().get_clip_ids().size() > 0, "a failed open keeps the project")
	pr = ws.get_project()


func _test_render() -> void:
	var dialog: Object = ws.get_render_dialog()
	var path: String = common.tmp_dir().path_join("tetra_video_checks_render.mp4")
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)
	tl.set_zone(0, 25)
	ws.open_render_dialog()
	await frames(4)
	check(dialog.visible, "render dialog opens")
	check(dialog.get_preset() != "", "a preset is selected: " + dialog.get_preset())
	dialog.set_output_path(path)
	dialog.set_range_mode(1)
	check_eq(dialog.get_range_mode(), 1, "zone range")
	dialog.set_scale_divisor(4)
	var result := []
	dialog.render_finished.connect(func(ok: bool, message: String) -> void: result.append([ok, message]), CONNECT_ONE_SHOT)
	dialog.start_render()
	check(dialog.is_rendering(), "render started")
	for i in 600:
		if not result.is_empty():
			break
		await _tree.create_timer(0.1).timeout
	if check(not result.is_empty(), "render finished in time"):
		check(result[0][0], "render succeeded: " + str(result[0][1]))
		check(FileAccess.file_exists(path) and FileAccess.open(path, FileAccess.READ).get_length() > 1000, "rendered file exists")
	check(not dialog.is_rendering(), "renderer idle")

	# Cancel stops a running render.
	dialog.set_range_mode(0)
	dialog.set_scale_divisor(1)
	result.clear()
	dialog.render_finished.connect(func(ok: bool, message: String) -> void: result.append([ok, message]), CONNECT_ONE_SHOT)
	dialog.start_render()
	await _tree.create_timer(0.3).timeout
	dialog.cancel_render()
	for i in 100:
		if not result.is_empty():
			break
		await _tree.create_timer(0.1).timeout
	if check(not result.is_empty(), "cancelled render reports back"):
		check(not result[0][0], "cancelled render is not a success")
		check_eq(result[0][1], "Render cancelled.", "cancelled render says so")
	dialog.hide()
	await frames()
