extends RefCounted
## Behaviour checks of the mesh Edit Mode in the running editor:
##
##   scripts/screenshot.sh build/screens/editmode-extruded.png 3D res://tetra_model/shots/edit_mode.gd
##
## prints "screenshot edit mode checks: <n> checks, <m> failed" and one
## "screenshot FAILED: ..." line per failure. It also saves
## build/screens/editmode-vertex.png, editmode-face.png and editmode-loopcut.png.

const U := preload("res://tetra_model/edit_mesh_util.gd")
const UP := Vector3(0, 1, 0)

var _checks := 0
var _failures := 0
var _tree: SceneTree
var _root: Window
var _plugin: TetraMeshEditPlugin
var _camera: Camera3D
var _surface: Control
var _last_mouse := Vector2.ZERO


func check(p_condition: bool, p_what: String) -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String) -> bool:
	return check(p_actual == p_expected, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func key(p_key: Key, p_shift: bool = false, p_ctrl: bool = false, p_alt: bool = false) -> void:
	for pressed in [true, false]:
		var event := InputEventKey.new()
		event.keycode = p_key
		event.physical_keycode = p_key
		event.shift_pressed = p_shift
		event.ctrl_pressed = p_ctrl
		event.alt_pressed = p_alt
		event.pressed = pressed
		_root.push_input(event)
		await frames(1)


func mouse_button(p_local: Vector2, p_pressed: bool, p_shift: bool = false, p_button: MouseButton = MOUSE_BUTTON_LEFT, p_alt: bool = false) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = p_button
	event.pressed = p_pressed
	event.shift_pressed = p_shift
	event.alt_pressed = p_alt
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_pressed and p_button == MOUSE_BUTTON_LEFT else 0
	event.position = _surface.get_global_position() + p_local
	event.global_position = event.position
	_root.push_input(event)
	await frames(1)


func mouse_move(p_local: Vector2, p_held: bool = false, p_ctrl: bool = false) -> void:
	var event := InputEventMouseMotion.new()
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if p_held else 0
	event.ctrl_pressed = p_ctrl
	event.position = _surface.get_global_position() + p_local
	event.global_position = event.position
	event.relative = event.position - _last_mouse
	_last_mouse = event.position
	_root.push_input(event)
	await frames(1)


func click(p_local: Vector2, p_shift: bool = false, p_alt: bool = false) -> void:
	await mouse_move(p_local)
	await mouse_button(p_local, true, p_shift, MOUSE_BUTTON_LEFT, p_alt)
	await mouse_button(p_local, false, p_shift, MOUSE_BUTTON_LEFT, p_alt)


func _screen(p_node: Node3D, p_local: Vector3) -> Vector2:
	return _camera.unproject_position(p_node.global_transform * p_local)


func _history(p_node: Node) -> UndoRedo:
	var manager := EditorInterface.get_editor_undo_redo()
	return manager.get_history_undo_redo(manager.get_object_history_id(p_node))


func _save(p_name: String) -> void:
	await frames(3)
	var path := OS.get_environment("TETRA_REPO_ROOT").path_join("build/screens").path_join(p_name)
	_root.get_texture().get_image().save_png(path)


func _mesh() -> TetraEditMesh:
	return _plugin.get_edit_mesh()


## The highest upward face; of several, the one nearest to the Y axis.
func _top_face() -> int:
	var best := -1
	var best_score := -INF
	for f in _mesh().get_face_count():
		if not _mesh().get_face_normal(f).is_equal_approx(UP):
			continue
		var center := _mesh().get_face_center(f)
		var score := center.y * 100.0 - Vector2(center.x, center.z).length()
		if score > best_score:
			best_score = score
			best = f
	return best


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	_root = p_tree.root
	_root.gui_embed_subwindows = true
	_plugin = TetraMeshEditPlugin.get_singleton()
	if not check(_plugin != null, "the plugin singleton exists"):
		return
	var viewport := EditorInterface.get_editor_viewport_3d(0)
	_camera = viewport.get_camera_3d()
	_surface = viewport.get_parent()

	# Add > Mesh.
	check_eq(_plugin.get_operations().size() > 30, true, "the operation list is exposed")
	var cube := _plugin.add_mesh("cube", {"size": 2.0})
	await frames(3)
	if not check(cube != null and cube.is_inside_tree(), "Add > Mesh > Cube creates a node in the scene"):
		return
	check(cube.get_meta("tetra_edit_mesh", null) is TetraEditMesh, "the node carries its edit mesh")
	check(cube.mesh is ArrayMesh and cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "the node shows the generated mesh")
	check_eq(EditorInterface.get_selection().get_selected_nodes(), [cube], "the new node is selected")

	# Step back so that the taller shape built below stays in view.
	for i in 2:
		await mouse_button(_surface.get_rect().size * 0.5, true, false, MOUSE_BUTTON_WHEEL_DOWN)
		await mouse_button(_surface.get_rect().size * 0.5, false, false, MOUSE_BUTTON_WHEEL_DOWN)
	await frames(10)

	# Enter.
	check(not _plugin.is_edit_mode(), "not in edit mode yet")
	check(_plugin.enter_edit_mode(cube), "enter edit mode")
	check(_plugin.is_edit_mode() and _plugin.get_edited_node() == cube, "edit mode is on the cube")
	check(_mesh() == cube.get_meta("tetra_edit_mesh"), "the edit mesh is the one in the metadata")
	check_eq(U.counts(_mesh()), Vector3i(8, 12, 6), "the cube is eight vertices, six quads")
	_mesh().select_none()
	_plugin.refresh()
	var overlay := _plugin.get_overlay_counts()
	check(overlay["visible"], "the overlay instance exists")
	check_eq([overlay["edges"], overlay["points"], overlay["face_triangles"]], [12, 8, 0], "vertex mode overlay: edges and vertex points")
	await _click_and_modal_checks(cube)
	_mesh().select_none()
	_mesh().select_vertex(U.vertex_at(_mesh(), Vector3(1, 1, 1)))
	_mesh().select_vertex(U.vertex_at(_mesh(), Vector3(-1, 1, 1)))
	_plugin.refresh()
	await _save("editmode-vertex.png")

	# Face mode through the scripting surface.
	_plugin.set_select_mode(TetraEditMesh.SELECT_FACE)
	_mesh().select_none()
	_mesh().select_face(_top_face())
	_plugin.refresh()
	overlay = _plugin.get_overlay_counts()
	check_eq([overlay["edges"], overlay["points"], overlay["face_triangles"]], [12, 6, 2], "face mode overlay: face dots and the selected face")
	await _save("editmode-face.png")

	# Extrude and move, loop cut, inset: one undo action each.
	var history := _history(cube)
	var version := history.get_version()
	check(_plugin.run_operation("extrude"), "extrude")
	check(_plugin.run_operation("translate", {"offset": Vector3(0, 1, 0)}), "move the extruded face")
	check_eq(U.counts(_mesh()), Vector3i(12, 20, 10), "extruded cube")
	check_eq(_mesh().get_face_center(_top_face()), Vector3(0, 2, 0), "the extruded face is one unit up")
	check(cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 3, 2))), "the node's mesh follows")
	check_eq(history.get_version(), version + 2, "two undo actions")
	var edge := _mesh().find_edge(U.vertex_at(_mesh(), Vector3(1, 1, 1)), U.vertex_at(_mesh(), Vector3(1, 2, 1)))
	check(edge >= 0 and _plugin.run_operation("loop_cut", {"edge": edge, "cuts": 1}), "loop cut")
	check_eq(U.counts(_mesh()), Vector3i(16, 28, 14), "loop cut around the extrusion")
	check(U.vertex_at(_mesh(), Vector3(1, 1.5, 1)) >= 0, "the cut is half way up")
	_mesh().select_face(_top_face())
	check(_plugin.run_operation("inset_faces", {"thickness": 0.25, "depth": 0.0}), "inset")
	check_eq(U.counts(_mesh()), Vector3i(20, 36, 18), "inset top")
	check(U.vertex_at(_mesh(), Vector3(0.75, 2, 0.75)) >= 0, "the inset corner")
	check_eq(_mesh().validate(), "", "the edited mesh validates")
	check(U.closed(_mesh()), "the edited mesh is closed")
	overlay = _plugin.get_overlay_counts()
	check_eq(overlay["edges"], 36, "the overlay follows the topology")

	history.undo()
	check_eq(U.counts(_mesh()), Vector3i(16, 28, 14), "undo the inset")
	history.undo()
	check_eq(U.counts(_mesh()), Vector3i(12, 20, 10), "undo the loop cut")
	history.undo()
	check_eq(_mesh().get_face_center(_top_face()), Vector3(0, 1, 0), "undo the move")
	history.undo()
	check_eq(U.counts(_mesh()), Vector3i(8, 12, 6), "undo the extrusion")
	check(cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "the node's mesh is the cube again")
	check_eq(_plugin.get_overlay_counts()["edges"], 12, "the overlay is the cube again")
	for i in 4:
		history.redo()
	check_eq(U.counts(_mesh()), Vector3i(20, 36, 18), "redo everything")
	check(cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 3, 2))), "the node's mesh after redo")
	check(not _plugin.run_operation("merge_by_distance", {"distance": 0.0001}), "an operation that changes nothing reports it")

	await _keyboard_tool_checks(cube)
	await _more_tool_checks(cube)
	await _dialog_checks(cube)
	await _persistence_checks(cube)
	print("screenshot edit mode checks: %d checks, %d failed" % [_checks, _failures])


## Simulated input: click select, Shift+click, box select, modal G.
func _click_and_modal_checks(p_cube: MeshInstance3D) -> void:
	var mesh := _mesh()
	# The vertex nearest to the camera is certainly visible.
	var nearest := 0
	for v in mesh.get_vertex_count():
		if _camera.global_position.distance_to(mesh.get_vertex(v)) < _camera.global_position.distance_to(mesh.get_vertex(nearest)):
			nearest = v
	var position := _screen(p_cube, mesh.get_vertex(nearest))
	check(_surface.get_rect().size.x > position.x and position.x > 0, "the cube is in view")
	check_eq(_plugin.pick(_camera, position)["vertex"], nearest, "picking finds the vertex under the cursor")
	await click(position)
	check_eq(mesh.get_selected_vertices(), PackedInt32Array([nearest]), "a click selects the vertex")
	check_eq(EditorInterface.get_selection().get_selected_nodes(), [p_cube], "the click does not change the node selection")

	# A vertex behind the cube cannot be picked.
	var farthest := 0
	for v in mesh.get_vertex_count():
		if _camera.global_position.distance_to(mesh.get_vertex(v)) > _camera.global_position.distance_to(mesh.get_vertex(farthest)):
			farthest = v
	check(_plugin.pick(_camera, _screen(p_cube, mesh.get_vertex(farthest)))["vertex"] != farthest, "a hidden vertex is not picked")

	# Shift+click extends and toggles.
	var second := -1
	for v in mesh.get_vertex_count():
		if v != nearest and _plugin.pick(_camera, _screen(p_cube, mesh.get_vertex(v)))["vertex"] == v:
			second = v
			break
	check(second >= 0, "a second visible vertex")
	await click(_screen(p_cube, mesh.get_vertex(second)), true)
	check_eq(mesh.get_selected_vertices().size(), 2, "Shift+click extends")
	await click(_screen(p_cube, mesh.get_vertex(second)), true)
	check_eq(mesh.get_selected_vertices(), PackedInt32Array([nearest]), "Shift+click again deselects")

	# Modal G: move, lock to X, type a distance, confirm.
	var history := _history(p_cube)
	var version := history.get_version()
	var before := mesh.get_vertex(nearest)
	await mouse_move(position)
	await key(KEY_G)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_GRAB, "G starts the move tool")
	await mouse_move(position + Vector2(70, -30))
	check(mesh.get_vertex(nearest).distance_to(before) > 0.05, "the vertex follows the mouse")
	check(p_cube.mesh.get_aabb().size != Vector3(2, 2, 2), "the node's mesh follows while dragging")
	await key(KEY_X)
	var moved := mesh.get_vertex(nearest) - before
	check(absf(moved.x) > 0.01 and is_zero_approx(moved.y) and is_zero_approx(moved.z), "X locks the move to the X axis")
	check(_plugin.get_modal_readout().contains("X"), "the readout names the axis")
	await key(KEY_2)
	check(mesh.get_vertex(nearest).is_equal_approx(before + Vector3(2, 0, 0)), "a typed number is the exact distance")
	await key(KEY_ENTER)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_NONE, "Enter confirms")
	check(mesh.get_vertex(nearest).is_equal_approx(before + Vector3(2, 0, 0)), "the move is kept")
	check_eq(history.get_version(), version + 1, "the move is one undo action")
	history.undo()
	check(mesh.get_vertex(nearest).is_equal_approx(before), "undo restores the vertex")
	check(p_cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "undo restores the node's mesh")

	# Cancel with Escape, confirm with a click, snap with Ctrl.
	await key(KEY_G)
	await mouse_move(position + Vector2(40, 40))
	await key(KEY_ESCAPE)
	check(_plugin.get_modal() == TetraMeshEditPlugin.MODAL_NONE and mesh.get_vertex(nearest).is_equal_approx(before), "Escape cancels the move")
	check(_plugin.is_edit_mode(), "Escape does not leave edit mode")
	check_eq(history.get_version(), version, "a cancelled move leaves no undo action")
	await mouse_move(position)
	await key(KEY_G)
	await key(KEY_Y)
	await mouse_move(position + Vector2(0, -260), false, true)
	var snapped_y := mesh.get_vertex(nearest).y - before.y
	check(snapped_y > 0.0 and is_equal_approx(snapped_y, snappedf(snapped_y, 1.0)), "Ctrl snaps the move to the grid step (%s)" % snapped_y)
	await mouse_button(position + Vector2(0, -260), true)
	await mouse_button(position + Vector2(0, -260), false)
	check(_plugin.get_modal() == TetraMeshEditPlugin.MODAL_NONE and mesh.get_vertex(nearest).y > before.y, "a click confirms")
	check_eq(mesh.get_selected_vertices(), PackedInt32Array([nearest]), "the confirming click does not select")
	history.undo()

	# Rotate and scale about the selection: two vertices of the top.
	mesh.select_none()
	mesh.select_vertex(U.vertex_at(mesh, Vector3(1, 1, 1)))
	mesh.select_vertex(U.vertex_at(mesh, Vector3(-1, 1, 1)))
	_plugin.refresh()
	await key(KEY_S)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_SCALE, "S starts the scale tool")
	await key(KEY_PERIOD)
	await key(KEY_5)
	await key(KEY_ENTER)
	check(U.vertex_at(mesh, Vector3(0.5, 1, 1)) >= 0 and U.vertex_at(mesh, Vector3(-0.5, 1, 1)) >= 0, "S .5 Enter halves the edge")
	history.undo()
	await key(KEY_R)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_ROTATE, "R starts the rotate tool, not Godot's scale tool")
	await key(KEY_Y)
	await key(KEY_9)
	await key(KEY_0)
	await key(KEY_ENTER)
	check(U.vertex_at(mesh, Vector3(0, 1, 0)) >= 0 and U.vertex_at(mesh, Vector3(0, 1, 2)) >= 0, "R Y 90 Enter turns the edge about its middle")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(8, 12, 6), "still a cube")
	check(p_cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "the cube is back")

	# Box select from empty space, then a click on empty space deselects.
	var size := _surface.get_rect().size
	await mouse_move(Vector2(30, 60))
	await mouse_button(Vector2(30, 60), true)
	await mouse_move(size * 0.5, true)
	await mouse_move(size - Vector2(30, 30), true)
	await mouse_button(size - Vector2(30, 30), false)
	check(mesh.get_selected_vertices().size() >= 4 and mesh.get_selected_vertices().size() < 8, "box select takes the visible vertices (%d)" % mesh.get_selected_vertices().size())
	await click(Vector2(30, 60))
	check_eq(mesh.get_selected_vertices().size(), 0, "a click on empty space deselects")
	check(_plugin.is_edit_mode() and EditorInterface.get_selection().get_selected_nodes() == [p_cube], "the node stays selected and in edit mode")


## Keys: select modes, select all/none/invert, E, I, Ctrl+R, X menu, Tab.
func _keyboard_tool_checks(p_cube: MeshInstance3D) -> void:
	var history := _history(p_cube)
	var mesh := _mesh()
	var center := _screen(p_cube, Vector3(0, 0.5, 0))
	await mouse_move(center)
	await key(KEY_1)
	check_eq(_plugin.get_select_mode(), TetraEditMesh.SELECT_VERTEX, "1 is vertex mode")
	await key(KEY_2)
	check_eq(_plugin.get_select_mode(), TetraEditMesh.SELECT_EDGE, "2 is edge mode")
	check_eq(_plugin.get_overlay_counts()["points"], 0, "edge mode shows no points")
	await key(KEY_3)
	check_eq(_plugin.get_select_mode(), TetraEditMesh.SELECT_FACE, "3 is face mode")
	await key(KEY_A)
	check_eq(mesh.get_selected_faces().size(), 18, "A selects all")
	await key(KEY_A, false, false, true)
	check_eq(mesh.get_selected_faces().size(), 0, "Alt+A selects none")
	mesh.select_face(_top_face())
	await key(KEY_I, false, true)
	check_eq(mesh.get_selected_faces().size(), 17, "Ctrl+I inverts")
	await key(KEY_I, false, true)
	await key(KEY_KP_ADD, false, true)
	check_eq(mesh.get_selected_faces().size(), 5, "Ctrl+Numpad+ selects more")
	await key(KEY_KP_SUBTRACT, false, true)
	check_eq(mesh.get_selected_faces().size(), 1, "Ctrl+Numpad- selects less")
	await key(KEY_A, false, false, true)
	await key(KEY_L)
	check_eq(mesh.get_selected_faces().size(), 18, "L selects what is linked under the cursor")

	# Alt+click selects an edge loop.
	await key(KEY_2)
	var a := mesh.get_vertex(U.vertex_at(mesh, Vector3(1, 1.5, 1)))
	var b := mesh.get_vertex(U.vertex_at(mesh, Vector3(1, 1.5, -1)))
	var probe := _screen(p_cube, a.lerp(b, 0.5))
	if _plugin.pick(_camera, probe)["edge"] < 0:
		b = mesh.get_vertex(U.vertex_at(mesh, Vector3(-1, 1.5, 1)))
		probe = _screen(p_cube, a.lerp(b, 0.5))
	await click(probe, false, true)
	check_eq(mesh.get_selected_edges().size(), 4, "Alt+click selects the edge loop")

	# E extrudes and moves along the normal; one undo action.
	await key(KEY_3)
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	var version := history.get_version()
	var top_before := mesh.get_face_center(_top_face())
	center = _screen(p_cube, top_before)
	await mouse_move(center)
	await key(KEY_E)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_GRAB, "E extrudes and starts the move")
	check(_plugin.get_modal_readout().contains("normal"), "the move is along the normal")
	await mouse_move(center.lerp(_screen(p_cube, top_before + UP), 0.5))
	var raised := mesh.get_face_center(_top_face()) - top_before
	check(raised.y > 0.1 and is_zero_approx(raised.x) and is_zero_approx(raised.z), "the new face moves along its normal only (%s, %s)" % [raised, _plugin.get_modal_readout()])
	await key(KEY_1)
	await key(KEY_ENTER)
	check_eq(mesh.get_face_center(_top_face()), top_before + UP, "E 1 Enter extrudes by one unit")
	check_eq(U.counts(mesh), Vector3i(24, 44, 22), "one more segment")
	check_eq(history.get_version(), version + 1, "extrude and move are one undo action")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo removes the segment")

	# I insets by dragging towards the centre.
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	await mouse_move(center + Vector2(120, 0))
	await key(KEY_I)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_INSET, "I starts the inset tool")
	await mouse_move(center + Vector2(90, 0))
	check_eq(U.counts(mesh), Vector3i(24, 44, 22), "the inset is previewed while dragging")
	await key(KEY_PERIOD)
	await key(KEY_1)
	await mouse_button(center + Vector2(90, 0), true)
	await mouse_button(center + Vector2(90, 0), false)
	check(_plugin.get_modal() == TetraMeshEditPlugin.MODAL_NONE and U.vertex_at(mesh, Vector3(0.65, 2, 0.65)) >= 0, "I .1 click insets by 0.1")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo the inset")

	# Ctrl+R: preview on the hovered edge ring, wheel changes the count.
	a = mesh.get_vertex(U.vertex_at(mesh, Vector3(1, 1.5, 1)))
	b = mesh.get_vertex(U.vertex_at(mesh, Vector3(1, 2, 1)))
	var over_edge := _screen(p_cube, a.lerp(b, 0.5))
	await mouse_move(over_edge)
	await key(KEY_R, false, true)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_LOOP_CUT, "Ctrl+R starts the loop cut")
	await mouse_move(over_edge + Vector2(1, 0))
	await mouse_button(over_edge, true, false, MOUSE_BUTTON_WHEEL_UP)
	await mouse_button(over_edge, false, false, MOUSE_BUTTON_WHEEL_UP)
	check(_plugin.get_modal_readout().contains("2"), "the wheel changes the number of cuts")
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "the preview does not change the mesh")
	await _save("editmode-loopcut.png")
	await mouse_button(over_edge, true)
	await mouse_button(over_edge, false)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_NONE, "a click confirms the loop cut")
	check_eq(U.counts(mesh), Vector3i(28, 52, 26), "two loops were cut")
	check(U.vertex_at(mesh, Vector3(1, 1.5 + 1.0 / 6.0, 1)) >= 0, "the two cuts are evenly spaced")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo the loop cut")

	# X opens the delete menu; its entries run the operation.
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	await key(KEY_X)
	await frames(3)
	var menu := _visible_popup()
	check(menu != null and menu.item_count >= 6, "X opens the delete menu")
	if menu != null:
		var labels: Array[String] = []
		for i in menu.item_count:
			labels.append(menu.get_item_text(i))
		check(labels.has("Vertices") and labels.has("Faces") and labels.has("Only Faces") and labels.has("Dissolve Edges"), "the delete menu lists %s" % [labels])
		menu.id_pressed.emit(menu.get_item_id(labels.find("Only Faces")))
		menu.hide()
	check_eq(U.counts(mesh), Vector3i(20, 36, 17), "Delete > Only Faces removes the face")
	# F fills the hole again.
	await key(KEY_1)
	for v in mesh.get_vertex_count():
		var p := mesh.get_vertex(v)
		if is_equal_approx(p.y, 2.0) and is_equal_approx(absf(p.x), 0.75):
			mesh.select_vertex(v)
	_plugin.refresh()
	await key(KEY_F)
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "F fills the hole")
	check(U.closed(mesh), "the filled mesh is closed")
	await key(KEY_M)
	await frames(3)
	menu = _visible_popup()
	check(menu != null and menu.item_count == 3, "M opens the merge menu")
	if menu != null:
		menu.hide()

	# Tab leaves and enters.
	await key(KEY_TAB)
	check(not _plugin.is_edit_mode(), "Tab leaves edit mode")
	check(not _plugin.get_overlay_counts()["visible"], "the overlay is gone")
	check_eq(EditorInterface.get_selection().get_selected_nodes(), [p_cube], "the node is still selected")
	await key(KEY_TAB)
	check(_plugin.is_edit_mode() and _plugin.get_edited_node() == p_cube, "Tab enters edit mode again")
	check_eq(U.counts(_mesh()), Vector3i(20, 36, 18), "the mesh re-enters as it was left")


## Plane lock, right-click cancel, Shift+D, Shift+N.
func _more_tool_checks(p_cube: MeshInstance3D) -> void:
	var history := _history(p_cube)
	var mesh := _mesh()
	await key(KEY_3)
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	var before := mesh.get_face_center(_top_face())
	var center := _screen(p_cube, before)
	var version := history.get_version()
	await mouse_move(center)
	await key(KEY_G)
	await key(KEY_Y, true)
	check(_plugin.get_modal_readout().contains("locking Y"), "Shift+Y locks the Y axis")
	await mouse_move(center + Vector2(60, 25))
	var moved := mesh.get_face_center(_top_face()) - before
	check(is_zero_approx(moved.y) and Vector2(moved.x, moved.z).length() > 0.05, "the move stays in the XZ plane (%s)" % moved)
	await mouse_button(center + Vector2(60, 25), true, false, MOUSE_BUTTON_RIGHT)
	await mouse_button(center + Vector2(60, 25), false, false, MOUSE_BUTTON_RIGHT)
	check(_plugin.get_modal() == TetraMeshEditPlugin.MODAL_NONE and mesh.get_face_center(_top_face()).is_equal_approx(before), "the right button cancels")
	check_eq(history.get_version(), version, "no undo action for a cancelled move")

	# Shift+D duplicates and starts a move; cancelling the move keeps the copy.
	await key(KEY_D, true)
	check_eq(_plugin.get_modal(), TetraMeshEditPlugin.MODAL_GRAB, "Shift+D duplicates and starts the move")
	check_eq(U.counts(mesh), Vector3i(24, 40, 19), "the selected face was duplicated")
	await key(KEY_Y)
	await key(KEY_1)
	await key(KEY_ENTER)
	check_eq(mesh.get_face_center(mesh.get_selected_faces()[0]), before + UP, "Shift+D Y 1 Enter puts the copy one unit up")
	check_eq(history.get_version(), version + 1, "duplicate and move are one undo action")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo removes the copy")

	# Shift+N turns flipped faces outwards again.
	mesh.select_none()
	mesh.select_face(_top_face())
	check(_plugin.run_operation("flip_normals"), "flip a face")
	check(not U.closed(mesh), "the mesh has a flipped face")
	await key(KEY_A)
	await key(KEY_N, true)
	check(U.closed(mesh) and U.volume(mesh) > 0.0, "Shift+N recalculates the normals outside")
	history.undo()
	history.undo()
	check(U.closed(mesh), "undo both")
	check_eq(_plugin.get_operations().filter(func(operation: Dictionary) -> bool: return operation["category"] != "Select").size(), 34, "the Mesh menu lists every operation")


## Operations with parameters go through the dialog, with a live preview.
func _dialog_checks(p_cube: MeshInstance3D) -> void:
	var history := _history(p_cube)
	var mesh := _mesh()
	var dialog := _plugin.get_operation_dialog()
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	var version := history.get_version()
	_plugin.request_operation("subdivide")
	await frames(3)
	check(dialog.visible and dialog.get_param_panel().has_param("cuts"), "Mesh > Subdivide... opens the parameter dialog")
	check_eq(U.counts(mesh), Vector3i(25, 44, 21), "the dialog previews one cut")
	dialog.get_param_panel().set_value("cuts", 2)
	dialog.values_changed.emit(dialog.get_values())
	check_eq(U.counts(mesh), Vector3i(32, 56, 26), "changing a value updates the preview")
	await _save("editmode-dialog.png")
	dialog.get_ok_button().pressed.emit()
	await frames(2)
	check(not dialog.visible, "OK closes the dialog")
	check_eq(U.counts(mesh), Vector3i(32, 56, 26), "OK keeps the result")
	check_eq(history.get_version(), version + 1, "the dialog's operation is one undo action")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo the subdivision")
	_plugin.request_operation("subdivide")
	await frames(3)
	check_eq(dialog.get_values()["cuts"], 2, "the dialog remembers the last values")
	dialog.get_cancel_button().pressed.emit()
	await frames(2)
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "Cancel drops the preview")
	check_eq(history.get_version(), version, "Cancel leaves no undo action")

	# Add > Mesh in edit mode adds to the edited mesh.
	check(_plugin.run_operation("add:ico_sphere", {"subdivisions": 1, "radius": 0.5}), "Add > Mesh in edit mode")
	check_eq(U.counts(mesh), Vector3i(32, 66, 38), "the primitive joins the edited mesh")
	check_eq(mesh.get_selected_faces().size(), 20, "and is the selection")
	history.undo()
	check_eq(U.counts(mesh), Vector3i(20, 36, 18), "undo removes it")


func _visible_popup() -> PopupMenu:
	for popup in _root.find_children("*", "PopupMenu", true, false):
		if popup.visible and popup.item_count > 0 and (popup.get_item_text(0) == "Vertices" or popup.get_item_text(0) == "at Center"):
			return popup
	return null


## Leaving keeps the result; the metadata re-enters losslessly; a mesh replaced
## from outside (what a Blender operator does) is picked up.
func _persistence_checks(p_cube: MeshInstance3D) -> void:
	var mesh := _mesh()
	_plugin.set_select_mode(TetraEditMesh.SELECT_FACE)
	mesh.select_none()
	mesh.select_face(_top_face())
	_plugin.refresh()
	await _save("editmode-extruded-overlay.png")
	var state := mesh.get_state()
	_plugin.exit_edit_mode()
	check(p_cube.mesh is ArrayMesh and p_cube.mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 3, 2))), "leaving keeps the result on the node")
	check(p_cube.get_meta("tetra_edit_mesh") == mesh, "leaving keeps the edit mesh in the metadata")
	check(_plugin.enter_edit_mode(p_cube), "enter again")
	check_eq(_mesh().get_state(), state, "re-entering is lossless (quads, selection, select mode)")

	# Undo works on a node that is no longer in edit mode.
	_plugin.run_operation("translate", {"offset": Vector3(0, 1, 0)})
	_plugin.exit_edit_mode()
	check_eq(p_cube.mesh.get_aabb().size.y, 4.0, "moved top")
	_history(p_cube).undo()
	check_eq(p_cube.mesh.get_aabb().size.y, 3.0, "undo outside edit mode updates the node's mesh")

	# The mesh is replaced from outside.
	var box := BoxMesh.new()
	box.size = Vector3(1, 1, 1)
	p_cube.mesh = box
	check(_plugin.enter_edit_mode(p_cube), "enter after the mesh was replaced")
	check_eq(U.counts(_mesh()), Vector3i(8, 12, 6), "the edit mesh is rebuilt from the replacement, as quads")
	check(_mesh().get_vertex(0).abs().is_equal_approx(Vector3(0.5, 0.5, 0.5)), "with the replacement's size")
	var sphere := SphereMesh.new()
	sphere.radial_segments = 8
	sphere.rings = 4
	p_cube.mesh = sphere
	await frames(3)
	check(_plugin.is_edit_mode() and _mesh().get_face_count() > 20, "a replacement while in edit mode is picked up")
	check(p_cube.mesh is ArrayMesh, "and the node shows the editable mesh again")
	check_eq(_mesh().validate(), "", "the converted sphere validates")

	# A node that was never edited converts with an undo step.
	_plugin.exit_edit_mode()
	var plain := MeshInstance3D.new()
	plain.name = "Plain"
	var plain_mesh := BoxMesh.new()
	plain.mesh = plain_mesh
	plain.position = Vector3(3, 0, 0)
	p_cube.get_parent().add_child(plain)
	plain.owner = EditorInterface.get_edited_scene_root()
	check(_plugin.enter_edit_mode(plain), "enter on a node with a BoxMesh")
	check(plain.mesh is ArrayMesh and plain.has_meta("tetra_edit_mesh"), "it is converted")
	check_eq(U.counts(_mesh()), Vector3i(8, 12, 6), "the box becomes six quads")
	_history(plain).undo()
	check(not _plugin.is_edit_mode() and plain.mesh == plain_mesh and not plain.has_meta("tetra_edit_mesh"), "undoing the conversion restores the BoxMesh")

	# Add > Mesh with nothing selected: the dialog, then a new node.
	EditorInterface.get_selection().clear()
	await frames(3)
	var dialog := _plugin.get_operation_dialog()
	_plugin.request_operation("add:cylinder")
	await frames(3)
	check(dialog.visible and dialog.get_param_panel().has_param("vertices"), "Add > Mesh > Cylinder asks for its parameters")
	dialog.get_param_panel().set_value("vertices", 12)
	dialog.get_ok_button().pressed.emit()
	await frames(3)
	var cylinder := p_cube.get_parent().get_node_or_null("Cylinder") as MeshInstance3D
	check(cylinder != null and cylinder.get_meta("tetra_edit_mesh").get_vertex_count() == 24, "a twelve-sided editable cylinder is added")
	if cylinder != null:
		check_eq(EditorInterface.get_selection().get_selected_nodes(), [cylinder], "and selected")
		_history(cylinder).undo()
		check(p_cube.get_parent().get_node_or_null("Cylinder") == null, "undo removes the node")

	# Leave the final picture: the edited cube in face mode.
	p_cube.mesh = _restore(p_cube, state)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(p_cube)
	await frames(3)
	_plugin.enter_edit_mode(p_cube)
	await frames(2)


func _restore(p_cube: MeshInstance3D, p_state: PackedByteArray) -> ArrayMesh:
	var edit_mesh: TetraEditMesh = p_cube.get_meta("tetra_edit_mesh")
	edit_mesh.set_state(p_state)
	return edit_mesh.to_array_mesh()
