extends RefCounted
## Edit Mode next to the Blender dock: a Blender operator on a node that
## carries an edit mesh keeps working, and Edit Mode picks up its result.
##
##   scripts/screenshot.sh build/screens/editmode-blender.png 3D res://tetra_model/shots/edit_mode_blender.gd
##
## prints "screenshot edit mode + blender checks: <n> checks, <m> failed".

const U := preload("res://tetra_model/edit_mesh_util.gd")

var _checks := 0
var _failures := 0


func check(p_condition: bool, p_what: String) -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String) -> bool:
	return check(p_actual == p_expected, "%s: expected %s, got %s" % [p_what, p_expected, p_actual])


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var edit := TetraMeshEditPlugin.get_singleton()
	var blender := TetraBlenderEditorPlugin.get_singleton()
	if TetraBlenderBridge.find_launcher().is_empty():
		print("screenshot edit mode + blender checks: skipped, no Blender")
		return
	var cube := edit.add_mesh("cube", {})
	await p_tree.process_frame
	var manager := EditorInterface.get_editor_undo_redo()
	var history := manager.get_history_undo_redo(manager.get_object_history_id(cube))

	# Outside Edit Mode: the operator replaces the mesh; entering refreshes.
	blender.run_operator(cube, "blender:mesh.subdivide", {"number_cuts": 1})
	var result: Array = await blender.operator_finished
	check(result[1], "a Blender operator runs on a node that carries an edit mesh: " + str(result[2]))
	check_eq(U.counts(cube.get_meta("tetra_edit_mesh")), Vector3i(8, 12, 6), "the stored edit mesh is not touched by Blender")
	check(edit.enter_edit_mode(cube), "enter edit mode after the operator")
	check_eq(U.counts(edit.get_edit_mesh()), Vector3i(26, 48, 24), "the edit mesh is rebuilt from Blender's result, as quads")
	check_eq(edit.get_edit_mesh().validate(), "", "it validates")

	# In Edit Mode: the result is picked up on the spot.
	blender.run_operator(cube, "blender:mesh.subdivide", {"number_cuts": 1})
	result = await blender.operator_finished
	check(result[1], "a Blender operator runs on the node in Edit Mode: " + str(result[2]))
	for i in 5:
		await p_tree.process_frame
	check(edit.is_edit_mode(), "still in Edit Mode")
	check_eq(U.counts(edit.get_edit_mesh()), Vector3i(98, 192, 96), "the edit mesh follows the operator's result")
	check_eq(edit.get_overlay_counts()["edges"], 192, "and so does the overlay")

	# Undoing the Blender operator is followed too, and native operations
	# continue from there.
	history.undo()
	for i in 5:
		await p_tree.process_frame
	check_eq(U.counts(edit.get_edit_mesh()), Vector3i(26, 48, 24), "undoing the operator is followed")
	edit.set_select_mode(TetraEditMesh.SELECT_FACE)
	edit.get_edit_mesh().select_face(0)
	check(edit.run_operation("extrude_faces_individual", {"offset": 0.5}), "a native operation after a Blender one")
	check_eq(U.counts(edit.get_edit_mesh()), Vector3i(30, 56, 28), "extruded one of Blender's quads")
	print("screenshot edit mode + blender checks: %d checks, %d failed" % [_checks, _failures])
