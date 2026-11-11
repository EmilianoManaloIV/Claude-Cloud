extends RefCounted
## Behaviour checks of the Blender workspace. They need the running editor
## (the plugin, the dock and the undo history do not exist in a headless
## test), so they run as a screenshot driver:
##
##   scripts/screenshot.sh build/screens/blender-checks.png 3D res://tetra_model/shots/checks.gd
##
## prints "screenshot blender checks: <n> checks, <m> failed" and one
## "screenshot FAILED: ..." line per failure.

const ShotScene := preload("res://tetra_model/shots/shot_scene.gd")

var _checks := 0
var _failures := 0
var _tree: SceneTree
var _blender: TetraBlenderEditorPlugin


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
	_tree = p_tree
	_blender = TetraBlenderEditorPlugin.get_singleton()
	check(_blender != null, "the plugin singleton exists")
	check(_blender.get_dock() != null and _blender.get_dock().is_inside_tree(), "the dock is in the editor")
	check(_blender.get_bridge() == TetraBlenderBridge.get_shared(), "the plugin uses the shared bridge")

	_check_status_without_blender()
	var cube := await _check_add_primitive()
	if cube != null:
		await _check_modifiers(cube)
		await _check_operators(cube)
		await _check_import(cube)
		await _check_render()
	print("screenshot blender checks: %d checks, %d failed" % [_checks, _failures])


func _history(p_node: Node) -> UndoRedo:
	var manager := EditorInterface.get_editor_undo_redo()
	return manager.get_history_undo_redo(manager.get_object_history_id(p_node))


func _triangles(p_mesh: Mesh) -> int:
	var count := 0
	for surface in p_mesh.get_surface_count():
		var arrays := p_mesh.surface_get_arrays(surface)
		var indices: Variant = arrays[Mesh.ARRAY_INDEX]
		if indices != null and indices.size() > 0:
			count += indices.size() / 3
		else:
			count += arrays[Mesh.ARRAY_VERTEX].size() / 3
	return count


func _evaluated(p_node: MeshInstance3D) -> bool:
	return await ShotScene.wait_for_modifiers(_tree, p_node)


func _check_status_without_blender() -> void:
	check(not _blender.get_bridge().is_running(), "opening the editor does not start Blender")
	check_eq(_blender.get_status(), TetraBlenderEditorPlugin.STATUS_NOT_STARTED, "status before the first request")
	check_eq(_blender.get_status_text(), "Blender not started", "status text before the first request")

	var python := OS.get_environment("TETRA_BLENDER_PYTHON")
	OS.unset_environment("TETRA_BLENDER_PYTHON")
	_blender.restart_bridge()
	if TetraBlenderBridge.is_available():
		print("screenshot note: a Blender is installed besides TETRA_BLENDER_PYTHON; the not-found state is not checked")
	else:
		check_eq(_blender.get_status(), TetraBlenderEditorPlugin.STATUS_NOT_FOUND, "status without a launcher")
		check_eq(_blender.get_status_text(), "Blender not found", "status text without a launcher")
		for needle in ["tetra/blender/python_path", "tetra/blender/executable_path", "scripts/setup-blender-bridge.sh"]:
			check(_blender.get_status_hint().contains(needle), "the hint names " + needle)
	OS.set_environment("TETRA_BLENDER_PYTHON", python)
	_blender.restart_bridge()
	check_eq(_blender.get_status(), TetraBlenderEditorPlugin.STATUS_NOT_STARTED, "status with the launcher back")


func _check_add_primitive() -> MeshInstance3D:
	check(EditorInterface.get_edited_scene_root() == null, "the test starts with an empty scene")
	_blender.add_primitive("blender:mesh.primitive_cube_add")
	check(_blender.get_status() in [TetraBlenderEditorPlugin.STATUS_STARTING, TetraBlenderEditorPlugin.STATUS_WORKING], "the request shows as starting: %d" % _blender.get_status())
	var cube: MeshInstance3D = await _blender.primitive_added
	check_eq(_blender.get_status(), TetraBlenderEditorPlugin.STATUS_READY, "status after the first answer")
	check(_blender.get_status_text().begins_with("Blender 5.") and _blender.get_status_text().ends_with("ready"), "status text names the version: " + _blender.get_status_text())

	var root := EditorInterface.get_edited_scene_root()
	check(root != null and root.get_class() == "Node3D", "an empty scene gets a Node3D root")
	check_eq(String(cube.name), "Cube", "the node is named after the primitive")
	check(cube.get_parent() == root and cube.owner == root, "the cube belongs to the scene")
	check_eq(_triangles(cube.mesh), 12, "Blender's cube")
	var material := cube.mesh.surface_get_material(0) as StandardMaterial3D
	check(material != null and material.albedo_color == Color(0.8, 0.8, 0.8), "the default material")
	check_eq(EditorInterface.get_selection().get_selected_nodes(), [cube], "the new node is selected")
	await _tree.process_frame
	await _tree.process_frame
	check(_blender.get_target() == cube, "the selected MeshInstance3D is the dock's target")

	# With a node selected the next primitive goes below it, as one undo step.
	var history := _history(root)
	var version := history.get_version()
	var sphere := await ShotScene.add_primitive(_tree, "blender:mesh.primitive_uv_sphere_add", {"radius": 0.5})
	check_eq(String(sphere.name), "UVSphere", "node name of the UV sphere")
	check(sphere.get_parent() == cube and sphere.owner == root, "added below the selected node")
	check_eq(history.get_version(), version + 1, "adding a primitive is one action")
	check_eq(history.get_current_action_name(), "Add Blender UV Sphere", "the action's name")
	history.undo()
	check(sphere.get_parent() == null, "undo removes the primitive")
	history.redo()
	check(sphere.get_parent() == cube, "redo puts it back")
	history.undo()

	# An explicit parent wins over the selection.
	var cone := await ShotScene.add_primitive(_tree, "blender:mesh.primitive_cone_add", {}, root)
	check(cone.get_parent() == root, "added below the given parent")
	_history(root).undo()

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(cube)
	await _tree.process_frame
	await _tree.process_frame
	return cube


func _check_modifiers(p_cube: MeshInstance3D) -> void:
	var source := p_cube.mesh
	var history := _history(p_cube)
	var tab := _blender.get_dock().get_modifiers_tab()
	_blender.show_tab("modifiers")
	check_eq(_blender.get_dock().get_current_tab(), "modifiers", "show_tab")

	var index := _blender.add_modifier(p_cube, "SUBSURF", {"levels": 1})
	check_eq(index, 0, "add_modifier returns the index")
	check(p_cube.get_meta("tetra_blender_source_mesh") == source, "the original mesh is kept in the node's metadata")
	check_eq(p_cube.get_meta("tetra_blender_modifiers").size(), 1, "the stack is kept in the node's metadata")
	check(_blender.is_evaluating(p_cube), "an evaluation is queued")
	check(await _evaluated(p_cube), "the evaluation finishes")
	check(p_cube.mesh != source, "the result replaces the node's mesh")
	# 6 quads, subdivided once: 24 quads.
	check_eq(_triangles(p_cube.mesh), 48, "Subdivision Surface level 1 of a cube")
	check_eq(_blender.get_evaluation_info(p_cube).get("triangle_count"), 48, "the counts Blender reports")

	# The dock shows the stack and the parameters of the selected modifier.
	check(await ShotScene.wait_until(_tree, func() -> bool: return tab.get_param_panel().get_operation().get("id", "") == "blender:modifier.SUBSURF", 30.0), "the parameter panel shows the selected modifier")
	check_eq(tab.get_param_panel().get_value("levels"), 1, "the panel shows the stored value")
	check_eq(_blender.get_modifier_label(_blender.get_modifier_stack(p_cube)[0]), "Subdivision Surface", "the label comes from Blender")

	_blender.set_modifier_values(p_cube, 0, {"levels": 2})
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 192, "level 2")
	check_eq(tab.get_param_panel().get_value("levels"), 2, "the panel follows a change made by a script")
	history.undo()
	check_eq(_blender.get_modifier_stack(p_cube)[0]["values"]["levels"], 1, "undo restores the value")
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 48, "undo re-evaluates")
	check_eq(tab.get_param_panel().get_value("levels"), 1, "the panel follows undo")
	history.redo()
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 192, "redo re-evaluates")

	# A dragged slider: many changes in quick succession, one undo step, and
	# the newest value wins.
	var version := history.get_version()
	for level in [1, 3, 2, 3]:
		_blender.set_modifier_values(p_cube, 0, {"levels": level})
	check_eq(history.get_version(), version + 1, "quick changes of one parameter coalesce into one action")
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 768, "the newest value is what is evaluated")
	history.undo()
	check_eq(_blender.get_modifier_stack(p_cube)[0]["values"]["levels"], 2, "one undo takes back the whole drag")
	await _evaluated(p_cube)

	_blender.set_modifier_enabled(p_cube, 0, false)
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 12, "a disabled modifier is not evaluated")
	history.undo()
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 192, "undo enables it again")

	_blender.add_modifier(p_cube, "BEVEL")
	_blender.add_modifier(p_cube, "ARRAY", {"count": 3})
	_blender.move_modifier(p_cube, 2, 0)
	check_eq(_blender.get_modifier_stack(p_cube).map(func(m: Dictionary) -> String: return m["type"]), ["ARRAY", "SUBSURF", "BEVEL"], "move_modifier")
	_blender.remove_modifier(p_cube, 2)
	check_eq(_blender.get_modifier_stack(p_cube).map(func(m: Dictionary) -> String: return m["type"]), ["ARRAY", "SUBSURF"], "remove_modifier")
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 3 * 192, "Array x3 before Subdivision Surface")
	check_eq(_blender.get_evaluation_info(p_cube).get("error"), "", "no evaluation error")
	history.undo()
	history.undo()
	history.undo()
	history.undo()
	check_eq(_blender.get_modifier_stack(p_cube).size(), 1, "four undos lead back to the single modifier")
	await _evaluated(p_cube)

	# A modifier Blender does not know is reported, not applied.
	_blender.add_modifier(p_cube, "NO_SUCH_MODIFIER")
	await _evaluated(p_cube)
	check(_blender.get_evaluation_info(p_cube).get("error") != "", "an unknown modifier is an evaluation error")
	history.undo()
	await _evaluated(p_cube)
	check_eq(_blender.get_evaluation_info(p_cube).get("error"), "", "the error is gone with the modifier")

	# Apply All bakes the result; undo brings the stack back.
	var evaluated_mesh := p_cube.mesh
	check(_blender.apply_modifiers(p_cube), "apply_modifiers")
	check(not p_cube.has_meta("tetra_blender_modifiers") and not p_cube.has_meta("tetra_blender_source_mesh"), "Apply All removes the metadata")
	check(p_cube.mesh == evaluated_mesh, "Apply All keeps the evaluated mesh")
	history.undo()
	check_eq(_blender.get_modifier_stack(p_cube).size(), 1, "undo of Apply All restores the stack")
	check(p_cube.get_meta("tetra_blender_source_mesh") == source, "and the source mesh")
	check(p_cube.mesh == evaluated_mesh and not _blender.is_evaluating(p_cube), "without a new evaluation")

	# Remove All puts the source back; undo evaluates again.
	_blender.clear_modifiers(p_cube)
	check(p_cube.mesh == source and not p_cube.has_meta("tetra_blender_modifiers"), "Remove All restores the original mesh")
	history.undo()
	check_eq(_blender.get_modifier_stack(p_cube).size(), 1, "undo of Remove All restores the stack")
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 192, "and evaluates it")
	history.redo()
	check(p_cube.mesh == source, "redo of Remove All")

	# Undoing the very first modifier leaves the node as it was.
	_blender.add_modifier(p_cube, "SOLIDIFY")
	history.undo()
	check(p_cube.mesh == source and not p_cube.has_meta("tetra_blender_modifiers"), "undo of the first modifier")
	await _tree.create_timer(0.5).timeout
	check(not _blender.is_evaluating(p_cube) and p_cube.mesh == source, "nothing arrives from Blender afterwards")


func _check_operators(p_cube: MeshInstance3D) -> void:
	var source := p_cube.mesh
	var history := _history(p_cube)
	_blender.show_tab("operators")
	check(await ShotScene.wait_until(_tree, func() -> bool:
		return _blender.get_schema_state(TetraBlenderEditorPlugin.SCHEMA_OPERATORS) == TetraBlenderEditorPlugin.SCHEMA_LOADED), "showing the Operators tab loads the operators")
	var tab := _blender.get_dock().get_operators_tab()
	check(tab.get_browser().get_operations().size() > 100, "the browser lists Blender's operators: %d" % tab.get_browser().get_operations().size())
	tab.get_browser().set_filter_text("subdivide")
	tab.select_operator("blender:mesh.subdivide")
	check_eq(tab.get_param_panel().get_operation().get("id"), "blender:mesh.subdivide", "the panel shows the selected operator")
	check(not _blender.can_rerun_last_operator(), "nothing to adjust before the first operator")

	var version := history.get_version()
	tab.get_param_panel().set_value("number_cuts", 1)
	tab.run()
	check(_blender.is_operator_running(), "the operator runs asynchronously")
	var result: Array = await _blender.operator_finished
	check(result[1], "subdivide succeeds: " + str(result[2]))
	# 6 quads cut once: 24 quads.
	check_eq(_triangles(p_cube.mesh), 48, "subdivide with one cut")
	check_eq(history.get_version(), version + 1, "an operator is one action")
	check_eq(history.get_current_action_name(), "Blender: Subdivide", "the action's name")
	check(_blender.can_rerun_last_operator(), "the last operator can be adjusted")
	check_eq(_blender.get_last_operation().get("id"), "blender:mesh.subdivide", "get_last_operation")

	# Adjust Last Operation: same starting mesh, new values, still one action.
	check(_blender.rerun_last_operator({"number_cuts": 2}), "rerun_last_operator")
	result = await _blender.operator_finished
	check(result[1], "the adjusted run succeeds: " + str(result[2]))
	check_eq(_triangles(p_cube.mesh), 108, "two cuts on the original cube, not on the subdivided one")
	check_eq(history.get_version(), version + 1, "adjusting replaces the action instead of adding one")
	history.undo()
	check(p_cube.mesh == source, "one undo leads back to the original mesh")
	check(not _blender.can_rerun_last_operator(), "after undo there is nothing to adjust")
	history.redo()
	check_eq(_triangles(p_cube.mesh), 108, "redo")
	history.undo()

	# With modifiers the operator edits the source mesh below them.
	_blender.add_modifier(p_cube, "ARRAY", {"count": 2})
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 24, "Array x2 of the cube")
	_blender.run_operator(p_cube, "blender:mesh.subdivide", {"number_cuts": 1})
	result = await _blender.operator_finished
	check(result[1], "operator below a modifier stack: " + str(result[2]))
	check_eq(_triangles(p_cube.get_meta("tetra_blender_source_mesh")), 48, "the operator changed the source mesh")
	await _evaluated(p_cube)
	check_eq(_triangles(p_cube.mesh), 96, "and the modifiers were evaluated on top")
	history.undo()
	check(p_cube.get_meta("tetra_blender_source_mesh") == source, "undo restores the source mesh")
	await _evaluated(p_cube)
	history.undo()
	check(p_cube.mesh == source and not p_cube.has_meta("tetra_blender_modifiers"), "back to the plain cube")

	_blender.run_operator(p_cube, "blender:mesh.no_such_operator")
	result = await _blender.operator_finished
	check(not result[1] and result[2] != "", "an unknown operator fails with a message: " + str(result[2]))
	check(p_cube.mesh == source, "a failed operator changes nothing")
	check(_blender.get_bridge().is_running(), "and Blender keeps running")


func _check_import(p_cube: MeshInstance3D) -> void:
	var root := EditorInterface.get_edited_scene_root()
	var path := OS.get_cache_dir().path_join("tetra-tests/blender-workspace-import.obj")
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var exported := _blender.get_bridge().export_meshes(path, [
		{"mesh": p_cube.mesh, "name": "Left", "transform": Transform3D(Basis(), Vector3(-2, 0, 0))},
		{"mesh": p_cube.mesh, "name": "Right", "transform": Transform3D(Basis(), Vector3(2, 0, 0))}])
	if not check_eq(exported, OK, "a model to import: " + _blender.get_bridge().get_last_error()):
		return
	var history := _history(root)
	var version := history.get_version()
	_blender.import_model(path, root)
	var model: Node3D = await _blender.model_imported
	check_eq(String(model.name), "blender-workspace-import", "the container is named after the file")
	check(model.get_parent() == root and model.owner == root, "the container belongs to the scene")
	check_eq(model.get_child_count(), 2, "one MeshInstance3D per object")
	var names: Array = []
	for child in model.get_children():
		names.append(String(child.name))
		check(child is MeshInstance3D and child.owner == root and _triangles(child.mesh) == 12, "imported object " + child.name)
	names.sort()
	check_eq(names, ["Left", "Right"], "object names")
	# OBJ files keep no object transforms: the offset is in the vertices.
	var right: MeshInstance3D = model.get_node("Right")
	var right_centre := (right.transform * right.mesh.get_aabb()).get_center()
	check(right_centre.is_equal_approx(Vector3(2, 0, 0)), "object placement: %s" % right_centre)
	check_eq(history.get_version(), version + 1, "an import is one action")
	history.undo()
	check(model.get_parent() == null, "undo removes the imported model")

	var failed: Array = []
	_blender.request_failed.connect(func(action: String, message: String) -> void: failed.append(action + ": " + message), CONNECT_ONE_SHOT)
	_blender.import_model(path.get_base_dir().path_join("no-such-file.obj"))
	await ShotScene.wait_until(_tree, func() -> bool: return not failed.is_empty(), 30.0)
	check(not failed.is_empty() and failed[0].begins_with("Import model: "), "a failed import is reported: %s" % [failed])


func _check_render() -> void:
	_blender.show_tab("render")
	var schema := _blender.get_render_schema()
	check_eq(TetraSchema.validate_operation(schema), "", "the render settings are a valid operation")
	check_eq(TetraSchema.get_defaults(schema), {"camera": "viewport", "width": 960, "height": 540, "match_viewport": true, "samples": 64, "denoise": true, "transparent": false}, "render defaults")

	var progress: Array = []
	_blender.render_progress.connect(func(fraction: float, _sample: int, _samples: int) -> void: progress.append(fraction))
	check(_blender.render_current_view({"width": 200, "samples": 4, "denoise": false}), "render_current_view")
	check(_blender.is_rendering(), "the render runs asynchronously")
	check(not _blender.render_current_view(), "a second render is refused meanwhile")
	var result: Array = await _blender.render_finished
	check(result[0], "the render succeeds: " + str(result[1]))
	check(not progress.is_empty() and progress[-1] == 1.0, "progress is reported up to 1: %s" % [progress])
	var image := _blender.get_last_render()
	var viewport_size := Vector2(EditorInterface.get_editor_viewport_3d(0).size)
	if check(image != null, "get_last_render"):
		check_eq(image.get_size(), Vector2i(200, roundi(200.0 * viewport_size.y / viewport_size.x)), "the picture has the viewport's aspect ratio")
		check_eq(_blender.get_last_render_info().get("samples"), 4, "render info")
		# The cube sits in the middle of the default view, in front of the grey background.
		var centre := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		var corner := image.get_pixel(2, 2)
		check(absf(centre.get_luminance() - corner.get_luminance()) > 0.05, "the cube is in the picture: centre %s, corner %s" % [centre, corner])

	var view := _blender.get_dock().get_render_tab().get_view()
	check_eq(view.get_content_size(), Vector2(image.get_size()), "the Render tab shows the picture")

	var png := "res://tetra_model/shots/check_render.png"
	check_eq(_blender.save_render(png), OK, "save_render")
	check(FileAccess.file_exists(png), "the PNG is in the project")
	var filesystem := EditorInterface.get_resource_filesystem()
	check(await ShotScene.wait_until(_tree, func() -> bool: return filesystem.get_file_type(png) == "CompressedTexture2D", 30.0), "the editor imported the new file as a texture: '%s'" % filesystem.get_file_type(png))
	for leftover in [png, png + ".import"]:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(leftover))

	check(not _blender.render_current_view({"camera": "scene"}), "rendering through a scene camera needs one")
	check_eq(_blender.get_last_error(), "The scene has no Camera3D.", "and says so")
