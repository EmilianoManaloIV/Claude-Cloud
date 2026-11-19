extends RefCounted
## Checks of the Video workspace's guards against losing work or acting on
## the wrong thing: foreign MLT files, the default render target, the active
## track across track changes, playback of an emptied timeline and refused
## bin deletions.
##
##   scripts/screenshot.sh build/screens/video-safety.png Video res://tetra_video/shots/safety_checks.gd

const V := TetraVideoProject.TRACK_VIDEO
const A := TetraVideoProject.TRACK_AUDIO

var _tree: SceneTree
var _checks := 0
var _failures := 0
var common: RefCounted
var ws: Object


func drive(p_tree: SceneTree) -> void:
	_tree = p_tree
	p_tree.root.gui_embed_subwindows = true
	common = load("res://tetra_video/shots/video_common.gd").new()
	ws = ClassDB.class_call_static("TetraVideoWorkspace", "get_singleton")
	common.workspace = ws
	await _test_active_track()
	await _test_empty_timeline_stops_playback()
	await _test_refused_bin_delete()
	await _test_foreign_file()
	await _test_render_target()
	ws.new_project()
	print("screenshot video safety checks: %d checks, %d failed" % [_checks, _failures])


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		print("screenshot FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(p_actual == p_expected, "%s (got %s, expected %s)" % [p_what, p_actual, p_expected])


func frames(p_count: int = 2) -> void:
	for i in p_count:
		await _tree.process_frame


func menu(p_menu: String, p_item: String) -> bool:
	for button: MenuButton in ws.find_children("*", "MenuButton", true, false):
		if button.text != p_menu:
			continue
		var popup := button.get_popup()
		for i in popup.item_count:
			if popup.get_item_text(i) == p_item and not popup.is_item_disabled(i):
				popup.id_pressed.emit(popup.get_item_id(i))
				await frames()
				return true
	return false


func track_named(p_name: String) -> int:
	var pr: TetraVideoProject = ws.get_project()
	for i in pr.get_track_count():
		if pr.get_track_name(i) == p_name:
			return i
	return -1


func status_text() -> String:
	var text := ""
	for label: Label in ws.find_children("*", "Label", true, false):
		text += label.text + "\n"
	return text


## The active track is a track, not a row number: it follows its track when
## others are added, removed or brought back by undo.
func _test_active_track() -> void:
	ws.new_project()
	await frames()
	var pr: TetraVideoProject = ws.get_project()
	var tl: Control = ws.get_timeline()
	var color: int = ws.add_color_clip("Red", Color.RED, 50)
	pr.insert_clip(color, track_named("V1"), 0)
	tl.set_active_track(track_named("V1"))
	check(await menu("Timeline", "Add Audio Track"), "menu item Add Audio Track")
	check_eq(pr.get_track_name(tl.get_active_track()), "V1", "the active track is still V1 after a track was added below it")
	var tracks := pr.get_track_count()
	check(await menu("Timeline", "Delete Track"), "menu item Delete Track")
	check_eq(pr.get_track_count(), tracks - 1, "Delete Track removed a track")
	check_eq(track_named("V1"), -1, "Delete Track removed V1, the active track")
	check(track_named("A1") >= 0, "A1 was not removed in its place")
	check_eq(tl.get_active_track(), -1, "no track is active after the active one was deleted")
	ws.undo()
	await frames()
	check(track_named("V1") >= 0, "undo brings V1 back")
	tl.set_active_track(track_named("V2"))
	pr.remove_track(track_named("A1"))
	await frames()
	check_eq(pr.get_track_name(tl.get_active_track()), "V2", "the active track is still V2 after a lower track was removed")
	pr.undo()
	await frames()
	check_eq(pr.get_track_name(tl.get_active_track()), "V2", "and after the removal was undone")
	pr.move_track(track_named("V2"), 0)
	await frames()
	check_eq(pr.get_track_name(tl.get_active_track()), "V2", "and after it was moved")


## Deleting the last clip while it plays stops the playback.
func _test_empty_timeline_stops_playback() -> void:
	ws.new_project()
	var pr: TetraVideoProject = ws.get_project()
	var color: int = ws.add_color_clip("Red", Color.RED, 250)
	var clip := pr.insert_clip(color, track_named("V1"), 0)
	await common.settle(_tree)
	ws.seek(0)
	ws.play()
	await _tree.create_timer(0.3).timeout
	check(ws.is_playing() and ws.get_engine().is_playing(), "playback runs")
	pr.delete_clip(clip)
	await common.settle(_tree)
	check_eq(pr.get_duration(), 0, "the timeline is empty")
	check(not ws.get_engine().is_playing(), "the engine stopped playing the deleted clips")
	check(not ws.is_playing(), "the monitor is paused")
	ws.pause()


## A bin clip that cannot be deleted says why.
func _test_refused_bin_delete() -> void:
	ws.new_project()
	var pr: TetraVideoProject = ws.get_project()
	var bin: Control = ws.get_bin()
	var kept: int = ws.add_color_clip("Kept", Color.RED, 50)
	var gone: int = ws.add_color_clip("Gone", Color.GREEN, 50)
	var v1 := track_named("V1")
	pr.insert_clip(kept, v1, 0)
	pr.insert_clip(gone, track_named("V2"), 0)
	pr.set_track_locked(v1, true)
	await frames()
	var list: ItemList = bin.find_children("*", "ItemList", true, false)[0]
	for i in list.item_count:
		list.select(i, false)
	check_eq(bin.get_selected_ids().size(), 2, "both bin clips are selected")
	bin.delete_selected()
	await frames()
	var dialog: ConfirmationDialog = null
	for candidate: ConfirmationDialog in bin.find_children("*", "ConfirmationDialog", true, false):
		if candidate.visible:
			dialog = candidate
	if check(dialog != null, "deleting used bin clips asks first"):
		dialog.get_ok_button().pressed.emit()
		await frames()
	check(pr.has_bin_clip(kept) and not pr.has_bin_clip(gone), "the clip on the locked track stays, the other goes")
	check(status_text().contains("Kept") and status_text().contains("locked"), "the refusal is reported: " + status_text().replace("\n", " | ").right(160))
	pr.set_track_locked(v1, false)


## A file written by another program is imported, never overwritten in place.
func _test_foreign_file() -> void:
	var path: String = common.tmp_dir().path_join("tetra_video_foreign.mlt")
	var xml := """<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" version="7.22.0" title="Shotcut" producer="main_bin">
  <profile description="HD" width="1920" height="1080" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="16" display_aspect_den="9" frame_rate_num="25" frame_rate_den="1" colorspace="709"/>
  <producer id="black" in="0" out="99"><property name="length">100</property><property name="resource">0</property><property name="mlt_service">color</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="99"/></playlist>
  <producer id="red" in="0" out="99"><property name="length">100</property><property name="resource">0xff0000ff</property><property name="mlt_service">color</property></producer>
  <producer id="green" in="0" out="99"><property name="length">100</property><property name="resource">0x00ff00ff</property><property name="mlt_service">color</property></producer>
  <playlist id="playlist0"><entry producer="red" in="0" out="59"/></playlist>
  <playlist id="playlist1"><blank length="40"/><entry producer="green" in="0" out="59"/></playlist>
  <tractor id="tractor0" title="Shotcut" in="0" out="99">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <track producer="playlist1"/>
    <transition id="transition0" in="40" out="59"><property name="a_track">1</property><property name="b_track">2</property><property name="mlt_service">luma</property></transition>
  </tractor>
</mlt>
"""
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(xml)
	file.close()
	check_eq(ws.open_project(path), OK, "a Shotcut file opens")
	await frames()
	var pr: TetraVideoProject = ws.get_project()
	check_eq(pr.get_clip_ids().size(), 2, "its clips are on the timeline")
	check_eq(ws.get_project_path(), "", "an imported file is not the project file")
	check(not ws.is_modified(), "a freshly imported project is unmodified")
	pr.add_marker(10, "edit")
	check(ws.is_modified(), "edited")
	check(ws.save_project() != OK, "Save without a path has nowhere to write")
	check_eq(FileAccess.get_file_as_string(path), xml, "the foreign file is untouched")
	# Saving it under a name of its own makes it a Tetra project; reopening that one keeps the path.
	var own: String = common.tmp_dir().path_join("tetra_video_foreign_saved.mlt")
	check_eq(ws.save_project(own), OK, "Save As")
	check_eq(ws.open_project(own), OK, "reopen")
	check_eq(ws.get_project_path(), own, "a Tetra project file is the project file")


## The default render target is never one of the project's own source files.
func _test_render_target() -> void:
	var video: String = common.make_test_video()
	if video == "":
		return
	ws.new_project()
	var pr: TetraVideoProject = ws.get_project()
	var bin_id: int = ws.import_file(video)
	pr.insert_clip(bin_id, track_named("V1"), 0)
	check_eq(ws.save_project(video.get_basename() + ".mlt"), OK, "save next to the source")
	var size := FileAccess.open(video, FileAccess.READ).get_length()
	var dialog: Object = ws.get_render_dialog()
	ws.open_render_dialog()
	await frames(4)
	check(dialog.get_output_path() != video, "the proposed output is not the source clip: " + dialog.get_output_path())
	# Asking for it anyway is refused before melt is started.
	dialog.set_output_path(video)
	check(dialog.start_render() != OK, "rendering onto a source clip is refused")
	check(not dialog.is_rendering(), "nothing was started")
	if dialog.is_rendering():
		dialog.cancel_render()
		await _tree.create_timer(1.0).timeout
	check(FileAccess.file_exists(video) and FileAccess.open(video, FileAccess.READ).get_length() == size, "the source clip is intact")
	# A file that is already there is only replaced after a question.
	var existing: String = common.tmp_dir().path_join("tetra_video_existing.mp4")
	var file := FileAccess.open(existing, FileAccess.WRITE)
	file.store_string("keep me")
	file.close()
	dialog.set_output_path(existing)
	dialog.get_ok_button().pressed.emit()
	await frames(4)
	check(not dialog.is_rendering(), "Render to File does not start over an existing file")
	var question: ConfirmationDialog = null
	for candidate: ConfirmationDialog in dialog.find_children("*", "ConfirmationDialog", true, false):
		if candidate.visible and candidate.get_text().contains("already exists"):
			question = candidate
	if check(question != null, "it asks about replacing the file"):
		question.hide()
	check_eq(FileAccess.get_file_as_string(existing), "keep me", "the existing file is untouched")
	if dialog.is_rendering():
		dialog.cancel_render()
		await _tree.create_timer(1.0).timeout
	dialog.hide()
	await frames()
	if not FileAccess.file_exists(video) or FileAccess.open(video, FileAccess.READ).get_length() != size:
		DirAccess.remove_absolute(video)
