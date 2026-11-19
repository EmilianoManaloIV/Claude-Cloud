extends RefCounted
## XCF import in the Image workspace: a .xcf in the project is a texture,
## opens from the FileSystem dock and from File > Open, shows what the import
## changed once, and is saved as OpenRaster (never written back as .xcf).
##
##   scripts/screenshot.sh build/screens/image-xcf-import.png Image res://tetra_paint/shots/workspace_xcf.gd
##
## Prints "screenshot xcf import checks: <n> checks, <m> failed".

const TMP_XCF := "res://tetra_paint/shots/_tmp_import.xcf"
const TMP_ORA := "res://tetra_paint/shots/_tmp_import.ora"

var _tree: SceneTree
var ws: Object
var _checks := 0
var _failures := 0


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(typeof(p_actual) == typeof(p_expected) and p_actual == p_expected,
			"%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func _frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func _visible_dialog(p_title: String) -> AcceptDialog:
	for dialog: AcceptDialog in ws.find_children("*", "AcceptDialog", true, false):
		if dialog.visible and dialog.title == p_title:
			return dialog
	return null


func _wait_for(p_condition: Callable, p_seconds: float = 30.0) -> bool:
	var deadline := Time.get_ticks_msec() + int(p_seconds * 1000.0)
	while not p_condition.call():
		if Time.get_ticks_msec() > deadline:
			return false
		await _tree.process_frame
	return true


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	p_tree.root.gui_embed_subwindows = true
	ws = TetraImageWorkspace.get_singleton()
	var repo := OS.get_environment("TETRA_REPO_ROOT")
	var gimp_file := repo.path_join("upstream/gimp/app/tests/files/gimp-2-6-file.xcf")
	var splash := repo.path_join("upstream/blender/release/datafiles/splash_template.xcf")
	if check(FileAccess.file_exists(gimp_file), "GIMP's test file is in the checkout"):
		await _test_project_file(gimp_file)
	await _cleanup()
	# The picture: a real layered file, opened the way File > Open does.
	if FileAccess.file_exists(splash):
		ws.close_document()
		ws.request_open_file(splash)
		await _frames(5)
		var doc: TetraImageDocument = ws.get_document()
		if check(doc != null, "the Blender splash template opens"):
			check_eq(doc.get_layer_count(), 8, "with its eight layers")
			check_eq(ws.get_layers_panel().get_listed_names().size(), 8, "which the layers panel lists")
			var dialog := _visible_dialog("Imported with Changes")
			if check(dialog != null, "its warnings are shown"):
				check(dialog.get_text().contains("Text, vector and link layers") and dialog.get_text().contains("colour profile"), "text layers and the colour profile are named: " + dialog.get_text())
	print("screenshot xcf import checks: %d checks, %d failed" % [_checks, _failures])


func _test_project_file(p_source: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(p_source)
	var copy := FileAccess.open(TMP_XCF, FileAccess.WRITE)
	copy.store_buffer(bytes)
	copy.close()
	var file_system := EditorInterface.get_resource_filesystem()
	file_system.scan()
	await _wait_for(func() -> bool: return not file_system.is_scanning() and ResourceLoader.exists(TMP_XCF))
	await _frames(5)
	var texture: Texture2D = load(TMP_XCF) if ResourceLoader.exists(TMP_XCF) else null
	if not check(texture != null, "a .xcf in the project imports as a texture"):
		return
	check_eq(Vector2i(texture.get_size()), Vector2i(100, 90), "the texture has the image's size")
	var reference := TetraImageDocument.new()
	reference.load(p_source)
	check_eq(texture.get_image().get_pixel(10, 10), reference.composite().get_pixel(10, 10), "and shows the composite")

	# The FileSystem dock's double click ends in edit_resource().
	ws.new_document(16, 16)
	EditorInterface.set_main_screen_editor("2D")
	await _frames(3)
	EditorInterface.edit_resource(texture)
	await _frames(5)
	check(ws.is_visible_in_tree(), "editing the .xcf texture switches to the Image workspace")
	check_eq(ws.get_document_path(), TMP_XCF, "and opens the file")
	var doc: TetraImageDocument = ws.get_document()
	check_eq(doc.get_layer_count(), 2, "with its layers")
	check_eq(ws.get_layers_panel().get_listed_names().size(), 2, "listed in the layers panel")
	check(not doc.is_selection_empty(), "and its selection")
	check_eq(ws.get_title_text(), "_tmp_import.xcf", "the title is the file name")
	check(not ws.is_modified(), "an imported file is not modified")

	var dialog := _visible_dialog("Imported with Changes")
	if check(dialog != null, "the import warnings are shown"):
		var text := dialog.get_text()
		check(text.contains("Save will ask for an OpenRaster"), "the dialog says how it will be saved")
		for warning in doc.get_load_warnings():
			check(text.contains(warning), "the dialog lists: " + warning)
		check(doc.get_load_warnings().size() >= 4, "there are several warnings for this file")
		dialog.get_ok_button().pressed.emit()
		await _frames(2)
		check(not dialog.visible, "OK closes it")
	# Asking for the open file again does not bring the warnings back.
	ws.request_open_file(TMP_XCF)
	EditorInterface.edit_resource(texture)
	await _frames(3)
	check(_visible_dialog("Imported with Changes") == null, "the warnings are shown once")

	# Saving: never as .xcf.
	check(not ws.can_save_directly(), "a .xcf document cannot be saved in place")
	check(ws.save() != OK, "save() refuses to write .xcf")
	check(ws.save_as("res://tetra_paint/shots/_tmp_other.xcf") != OK, "save_as() refuses .xcf")
	check(not FileAccess.file_exists("res://tetra_paint/shots/_tmp_other.xcf"), "and writes nothing")
	doc.fill(doc.get_active_layer(), Color.RED)
	check(ws.is_modified(), "editing marks the document modified")
	ws.menu_option(ws.FILE_SAVE)
	await _frames(3)
	var save_dialog := _visible_dialog("Save Image")
	if check(save_dialog != null, "File > Save opens the Save Image dialog"):
		var file_dialog := save_dialog as FileDialog
		check_eq(file_dialog.get_current_file(), "_tmp_import.ora", "which proposes the .ora name")
		check(Array(file_dialog.filters).all(func(f: String) -> bool: return not f.contains("xcf")), "and offers no .xcf: " + str(file_dialog.filters))
		save_dialog.hide()
	check_eq(FileAccess.get_file_as_bytes(TMP_XCF), bytes, "the .xcf file is untouched")
	check_eq(ws.save_as(TMP_ORA), OK, "saving as .ora works")
	check_eq(ws.get_document_path(), TMP_ORA, "and the document is the .ora now")
	check(ws.can_save_directly() and not ws.is_modified(), "which saves directly from now on")
	var reloaded := TetraImageDocument.new()
	check_eq(reloaded.load(TMP_ORA), OK, "the .ora loads")
	check_eq(reloaded.get_layer_count(), 2, "with both layers")
	check_eq(FileAccess.get_file_as_bytes(TMP_XCF), bytes, "the .xcf file is still untouched")
	await _wait_for(func() -> bool: return not file_system.is_scanning())


func _cleanup() -> void:
	ws.close_document()
	var file_system := EditorInterface.get_resource_filesystem()
	await _wait_for(func() -> bool: return not file_system.is_scanning())
	for path: String in [TMP_XCF, TMP_XCF + ".import", TMP_ORA, TMP_ORA + ".import"]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	file_system.scan()
	await _wait_for(func() -> bool: return not file_system.is_scanning())
