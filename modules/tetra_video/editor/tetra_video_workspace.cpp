#include "tetra_video_workspace.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_assets.h"
#include "../tetra_video_project.h"
#include "tetra_video_bin.h"
#include "tetra_video_effect_stack.h"
#include "tetra_video_monitor.h"
#include "tetra_video_render_dialog.h"
#include "tetra_video_thumbnails.h"
#include "tetra_video_timecode.h"
#include "tetra_video_timeline.h"

#include "tetra/editor/tetra_operation_browser.h"

#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/tree.h"
#include "scene/main/timer.h"
#include "scene/main/viewport.h"

TetraVideoWorkspace *TetraVideoWorkspace::singleton = nullptr;

static const int ZOOM_SLIDER_STEPS = 1000;

static Button *_make_tool_button(const String &p_tooltip) {
	Button *button = memnew(Button);
	button->set_theme_type_variation(SceneStringName(FlatButton));
	// Tool buttons never take the keyboard: the shortcuts keep going to the timeline.
	button->set_focus_mode(Control::FOCUS_NONE);
	button->set_tooltip_text(p_tooltip);
	return button;
}

void TetraVideoWorkspace::_graph_thread_main(void *p_self) {
	static_cast<TetraVideoWorkspace *>(p_self)->_run_graph_thread();
}

void TetraVideoWorkspace::_run_graph_thread() {
	while (true) {
		graph_semaphore.wait();
		if (graph_quit.is_set()) {
			break;
		}
		String xml;
		{
			MutexLock lock(graph_mutex);
			if (!graph_has_pending) {
				continue;
			}
			xml = graph_pending_xml;
			graph_has_pending = false;
		}
		engine->set_xml(xml);
		callable_mp(this, &TetraVideoWorkspace::_graph_rebuilt).call_deferred();
	}
}

void TetraVideoWorkspace::_rebuild_graph() {
	if (!TetraMltEngine::is_available() || project->get_duration() == 0) {
		// Nothing to build: the monitor shows its "empty" picture and the engine is left alone.
		monitor->refresh();
		return;
	}
	const String xml = project->to_mlt_xml();
	{
		MutexLock lock(graph_mutex);
		graph_pending_xml = xml;
		graph_has_pending = true;
	}
	graph_semaphore.post();
}

void TetraVideoWorkspace::_graph_rebuilt() {
	monitor->refresh();
}

Ref<TetraVideoProject> TetraVideoWorkspace::_make_default_project() const {
	// Kdenlive's default: three video tracks over two audio tracks.
	Ref<TetraVideoProject> fresh;
	fresh.instantiate();
	for (int i = 0; i < 2; i++) {
		fresh->add_track(TetraVideoProject::TRACK_AUDIO);
	}
	for (int i = 0; i < 3; i++) {
		fresh->add_track(TetraVideoProject::TRACK_VIDEO);
	}
	fresh->clear_undo_history();
	return fresh;
}

void TetraVideoWorkspace::_set_project(const Ref<TetraVideoProject> &p_project, const String &p_path) {
	effect_stack->commit_pending();
	if (project.is_valid()) {
		project->disconnect("changed", callable_mp(this, &TetraVideoWorkspace::_project_changed));
		project->disconnect("structure_changed", callable_mp(this, &TetraVideoWorkspace::_project_structure_changed));
	}
	monitor->pause();
	project = p_project;
	project_path = p_path;
	saved_revision = project->get_revision();
	project->connect("changed", callable_mp(this, &TetraVideoWorkspace::_project_changed));
	project->connect("structure_changed", callable_mp(this, &TetraVideoWorkspace::_project_structure_changed));

	thumbnails->clear();
	thumbnails->set_profile(project->get_width(), project->get_height(), project->get_fps_num(), project->get_fps_den());
	bin->set_project(project);
	monitor->set_project(project);
	timeline->set_project(project);
	effect_stack->set_project(project);
	_update_effects_list();
	if (timeline->get_size().x > 0) {
		timeline->zoom_to_fit();
	}
	_update_title();
	_update_info();
	_update_actions();
	graph_timer->start();
}

void TetraVideoWorkspace::_project_changed() {
	// A burst of edits (a group, a drag of a slider) rebuilds the engine's graph once.
	graph_timer->start();
	monitor->project_changed();
	_update_title();
	_update_info();
	_update_actions();
}

void TetraVideoWorkspace::_project_structure_changed() {
	thumbnails->set_profile(project->get_width(), project->get_height(), project->get_fps_num(), project->get_fps_den());
	_update_effects_list();
}

void TetraVideoWorkspace::_update_title() {
	String name = project_path.is_empty() ? TTR("Untitled") : project_path.get_file();
	if (is_modified()) {
		name += " (*)";
	}
	title_label->set_text(name);
	title_label->set_tooltip_text(project_path.is_empty() ? TTR("This video project has not been saved yet.") : project_path);
}

void TetraVideoWorkspace::_update_info() {
	String info = TetraMltEngine::is_available() ? "MLT " + TetraMltEngine::get_version() : TTR("MLT unavailable");
	info += vformat("  |  %dx%d  %s fps  |  %s", project->get_width(), project->get_height(), String::num(project->get_fps(), 3), TetraVideoTimecode::from_frames(project->get_duration(), project->get_fps()));
	info_label->set_text(info);
}

void TetraVideoWorkspace::_update_actions() {
	undo_button->set_disabled(!project->can_undo());
	redo_button->set_disabled(!project->can_redo());
	undo_button->set_tooltip_text(project->can_undo() ? vformat(TTR("Undo: %s"), project->get_undo_name()) : TTR("Undo"));
	redo_button->set_tooltip_text(project->can_redo() ? vformat(TTR("Redo: %s"), project->get_redo_name()) : TTR("Redo"));
	const bool has_selection = !timeline->get_selection().is_empty();
	delete_button->set_disabled(!has_selection);
	ripple_button->set_disabled(!has_selection);

	PopupMenu *edit = edit_menu->get_popup();
	edit->set_item_disabled(edit->get_item_index(EDIT_UNDO), !project->can_undo());
	edit->set_item_disabled(edit->get_item_index(EDIT_REDO), !project->can_redo());
}

void TetraVideoWorkspace::_update_icons() {
	undo_button->set_button_icon(get_editor_theme_icon(SNAME("UndoRedo")));
	redo_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoRedo")));
	render_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoRender")));
	select_tool_button->set_button_icon(get_editor_theme_icon(SNAME("ToolSelect")));
	razor_tool_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoRazor")));
	snap_button->set_button_icon(get_editor_theme_icon(SNAME("Snap")));
	split_button->set_button_icon(get_editor_theme_icon(SNAME("ActionCut")));
	delete_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
	ripple_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoRipple")));
	marker_button->set_button_icon(get_editor_theme_icon(SNAME("Marker")));
	zoom_out_button->set_button_icon(get_editor_theme_icon(SNAME("ZoomLess")));
	zoom_in_button->set_button_icon(get_editor_theme_icon(SNAME("ZoomMore")));
	zoom_fit_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoFit")));
}

void TetraVideoWorkspace::_update_effects_list() {
	// Kdenlive's catalog resolves size placeholders, so it is read again when the profile's size changes.
	const Vector2i size(project->get_width(), project->get_height());
	if (effects_browser->has_meta("tetra_profile_size") && Vector2i(effects_browser->get_meta("tetra_profile_size")) == size) {
		return;
	}
	effects_browser->set_meta("tetra_profile_size", size);
	effects_browser->set_operations(TetraVideoAssets::get_effects(true, size.x, size.y));
}

void TetraVideoWorkspace::_set_status(const String &p_text, bool p_warning) {
	status_label->set_text(p_text);
	if (p_warning) {
		status_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
	} else {
		status_label->remove_theme_color_override(SceneStringName(font_color));
	}
	status_timer->start();
}

void TetraVideoWorkspace::_status_timeout() {
	status_label->remove_theme_color_override(SceneStringName(font_color));
	status_label->set_text(TTR("Drag clips from the bin onto a track. S selects, X cuts, Space plays."));
}

void TetraVideoWorkspace::_show_error(const String &p_text) {
	_set_status(p_text, true);
	if (is_inside_tree()) {
		error_dialog->set_text(p_text);
		error_dialog->popup_centered();
	}
}

void TetraVideoWorkspace::_menu_option(int p_option) {
	commit_pending_edits();
	switch (p_option) {
		case PROJECT_NEW: {
			_ask_discard(PENDING_NEW);
		} break;
		case PROJECT_OPEN: {
			open_dialog->popup_file_dialog();
		} break;
		case PROJECT_SAVE: {
			if (project_path.is_empty()) {
				save_dialog->popup_file_dialog();
			} else {
				save_project();
			}
		} break;
		case PROJECT_SAVE_AS: {
			if (!project_path.is_empty()) {
				save_dialog->set_current_path(ProjectSettings::get_singleton()->globalize_path(project_path));
			}
			save_dialog->popup_file_dialog();
		} break;
		case PROJECT_SETTINGS: {
			open_project_settings();
		} break;
		case PROJECT_RENDER: {
			open_render_dialog();
		} break;
		case EDIT_UNDO: {
			undo();
		} break;
		case EDIT_REDO: {
			redo();
		} break;
		case EDIT_DELETE:
		case EDIT_RIPPLE_DELETE: {
			// With the keyboard in the bin, Delete means its clips.
			const Control *focus = get_viewport()->gui_get_focus_owner();
			if (p_option == EDIT_DELETE && focus && bin->is_ancestor_of(focus)) {
				bin->delete_selected();
			} else {
				delete_selection(p_option == EDIT_RIPPLE_DELETE);
			}
		} break;
		case EDIT_SELECT_ALL: {
			timeline->set_selection(project->get_clip_ids());
		} break;
		case TIMELINE_ADD_VIDEO_TRACK: {
			const int track = project->add_track(TetraVideoProject::TRACK_VIDEO);
			if (track >= 0) {
				_set_status(vformat(TTR("Added track %s."), project->get_track_name(track)));
			}
		} break;
		case TIMELINE_ADD_AUDIO_TRACK: {
			const int track = project->add_track(TetraVideoProject::TRACK_AUDIO);
			if (track >= 0) {
				_set_status(vformat(TTR("Added track %s."), project->get_track_name(track)));
			}
		} break;
		case TIMELINE_DELETE_TRACK: {
			const int track = timeline->get_active_track();
			if (track < 0) {
				_set_status(TTR("Click a track header to choose the track to delete."), true);
			} else {
				const String name = project->get_track_name(track);
				if (project->remove_track(track)) {
					_set_status(vformat(TTR("Deleted track %s."), name));
				} else {
					_set_status(project->get_last_error(), true);
				}
			}
		} break;
		case TIMELINE_SPLIT: {
			split_at_playhead();
		} break;
		case TIMELINE_INSERT_SPACE: {
			space_active_only->set_disabled(timeline->get_active_track() < 0);
			if (timeline->get_active_track() < 0) {
				space_active_only->set_pressed(false);
			}
			space_dialog->popup_centered(Size2(300, 0) * EDSCALE);
		} break;
		case TIMELINE_REMOVE_SPACE: {
			const int removed = project->remove_space(get_playhead(), timeline->get_active_track());
			if (removed > 0) {
				_set_status(vformat(TTR("Removed %d frames of space."), removed));
			} else {
				_set_status(TTR("There is no gap under the playhead on the active track."), true);
			}
		} break;
		case TIMELINE_ADD_MARKER: {
			add_marker();
		} break;
		case TIMELINE_DELETE_MARKER: {
			if (!project->remove_marker(get_playhead())) {
				_set_status(TTR("There is no marker at the playhead."), true);
			}
		} break;
		case TIMELINE_ZONE_IN: {
			const int out = timeline->get_zone_out();
			timeline->set_zone(get_playhead(), out >= get_playhead() ? out : MAX(get_playhead(), project->get_duration() - 1));
		} break;
		case TIMELINE_ZONE_OUT: {
			const int in = timeline->get_zone_in();
			timeline->set_zone(in >= 0 && in <= get_playhead() ? in : 0, get_playhead());
		} break;
		case TIMELINE_ZONE_CLEAR: {
			timeline->set_zone(-1, -1);
		} break;
	}
}

void TetraVideoWorkspace::_ask_discard(PendingAction p_action, const String &p_path) {
	pending_action = p_action;
	pending_path = p_path;
	if (is_modified()) {
		discard_dialog->popup_centered();
	} else {
		_discard_confirmed();
	}
}

void TetraVideoWorkspace::_discard_confirmed() {
	const PendingAction action = pending_action;
	pending_action = PENDING_NONE;
	if (action == PENDING_NEW) {
		new_project();
	} else if (action == PENDING_OPEN) {
		const Error error = open_project(pending_path);
		if (error != OK) {
			error_dialog->set_text(status_label->get_text());
			error_dialog->popup_centered();
		}
	}
}

void TetraVideoWorkspace::_open_file_selected(const String &p_path) {
	_ask_discard(PENDING_OPEN, p_path);
}

void TetraVideoWorkspace::_save_file_selected(const String &p_path) {
	save_project(p_path.get_extension().is_empty() ? p_path + ".mlt" : p_path);
}

void TetraVideoWorkspace::_settings_preset_selected(int p_index) {
	if (p_index <= 0) {
		return;
	}
	// A preset fills in the fields; they can still be changed before OK.
	Ref<TetraVideoProject> probe;
	probe.instantiate();
	if (probe->set_profile_preset(settings_preset->get_item_text(p_index))) {
		settings_width->set_value(probe->get_width());
		settings_height->set_value(probe->get_height());
		settings_fps_num->set_value(probe->get_fps_num());
		settings_fps_den->set_value(probe->get_fps_den());
	}
}

void TetraVideoWorkspace::_settings_confirmed() {
	commit_pending_edits();
	if (!project->set_profile(int(settings_width->get_value()), int(settings_height->get_value()), int(settings_fps_num->get_value()), int(settings_fps_den->get_value()), project->get_sample_rate())) {
		if (!project->get_last_error().is_empty()) {
			_show_error(project->get_last_error());
		}
		return;
	}
	_set_status(vformat(TTR("Profile: %dx%d at %s fps."), project->get_width(), project->get_height(), String::num(project->get_fps(), 3)));
}

void TetraVideoWorkspace::_space_confirmed() {
	commit_pending_edits();
	const int frames = MAX(1, int(Math::round(space_seconds->get_value() * project->get_fps())));
	const int track = space_active_only->is_pressed() ? timeline->get_active_track() : -1;
	if (project->insert_space(get_playhead(), frames, track)) {
		_set_status(vformat(TTR("Inserted %d frames of space."), frames));
	} else {
		_set_status(project->get_last_error(), true);
	}
}

void TetraVideoWorkspace::_bin_clip_activated(int p_bin_id) {
	monitor->show_clip(p_bin_id);
}

void TetraVideoWorkspace::_bin_import_requested(const PackedStringArray &p_paths) {
	PackedStringArray failed;
	int last = -1;
	project->begin_undo_group(TTR("Add Clips"));
	for (const String &path : p_paths) {
		const int id = import_file(path);
		if (id < 0) {
			failed.push_back(path.get_file());
		} else {
			last = id;
		}
	}
	project->end_undo_group();
	if (last >= 0) {
		bin->select_clip(last);
	}
	if (!failed.is_empty()) {
		_show_error(vformat(TTR("These files could not be read as video, audio or image:\n%s"), String("\n").join(failed)));
	}
}

void TetraVideoWorkspace::_monitor_position_changed(int p_frame) {
	timeline->set_playhead(p_frame, monitor->is_playing());
}

void TetraVideoWorkspace::_timeline_seek_requested(int p_frame) {
	seek(p_frame);
}

void TetraVideoWorkspace::_timeline_selection_changed() {
	const PackedInt32Array selection = timeline->get_selection();
	effect_stack->set_clip(selection.is_empty() ? -1 : selection[0]);
	_update_actions();
}

void TetraVideoWorkspace::_timeline_zoom_changed(double p_zoom) {
	const double low = timeline->get_zoom_min();
	const double high = timeline->get_zoom_max();
	zoom_slider->set_value_no_signal(Math::log(p_zoom / low) / Math::log(high / low) * ZOOM_SLIDER_STEPS);
}

void TetraVideoWorkspace::_timeline_tool_changed(int p_tool) {
	select_tool_button->set_pressed_no_signal(p_tool == TOOL_SELECT);
	razor_tool_button->set_pressed_no_signal(p_tool == TOOL_RAZOR);
}

void TetraVideoWorkspace::_timeline_edit_refused(const String &p_message) {
	_set_status(p_message, true);
}

void TetraVideoWorkspace::_timeline_clip_activated(int p_clip_id) {
	show_effect_stack();
}

void TetraVideoWorkspace::_timeline_effect_dropped(int p_clip_id, const String &p_operation_id) {
	commit_pending_edits();
	const Dictionary operation = TetraVideoAssets::get_operation(p_operation_id, project->get_width(), project->get_height());
	if (operation.is_empty() || !project->has_clip(p_clip_id)) {
		return;
	}
	const bool audio_effect = String(operation.get("kind", String())) == "audio_effect";
	const bool audio_clip = project->get_track_kind(project->get_clip(p_clip_id)["track"]) == TetraVideoProject::TRACK_AUDIO;
	if (audio_effect != audio_clip) {
		_set_status(audio_effect ? TTR("Audio effects go on clips of audio tracks.") : TTR("Video effects go on clips of video tracks."), true);
		return;
	}
	const int index = _add_effect_to_clip(p_clip_id, operation);
	if (index < 0) {
		_set_status(project->get_last_error(), true);
		return;
	}
	timeline->select_clip(p_clip_id);
	effect_stack->set_clip(p_clip_id);
	effect_stack->select_effect(index);
	show_effect_stack();
	_set_status(vformat(TTR("Added the effect %s."), String(operation["label"])));
}

void TetraVideoWorkspace::_tool_button_pressed(int p_tool) {
	set_tool(Tool(p_tool));
}

void TetraVideoWorkspace::_snap_toggled(bool p_pressed) {
	timeline->set_snapping(p_pressed);
}

void TetraVideoWorkspace::_zoom_slider_changed(double p_value) {
	const double low = timeline->get_zoom_min();
	const double high = timeline->get_zoom_max();
	timeline->set_zoom(low * Math::pow(high / low, p_value / ZOOM_SLIDER_STEPS));
}

void TetraVideoWorkspace::_zoom_step(int p_direction) {
	timeline->set_zoom(timeline->get_zoom() * (p_direction > 0 ? 1.25 : 0.8));
}

void TetraVideoWorkspace::_effect_activated(const String &p_id) {
	add_effect_to_selection(p_id);
}

Variant TetraVideoWorkspace::_effects_get_drag_data(const Point2 &p_point, Control *p_from) {
	Tree *tree = Object::cast_to<Tree>(p_from);
	const Dictionary operation = effects_browser->get_selected_operation();
	// Only the row that was pressed can be dragged; the press has selected it. A category row has nothing to drag.
	if (!tree || operation.is_empty() || tree->get_item_at_position(p_point) != tree->get_selected()) {
		return Variant();
	}
	Dictionary drag;
	drag["type"] = TetraVideoTimeline::EFFECT_DRAG_TYPE;
	drag["operation_id"] = operation["id"];
	Label *preview = memnew(Label);
	preview->set_text(operation["label"]);
	tree->set_drag_preview(preview);
	return drag;
}

void TetraVideoWorkspace::_render_finished(bool p_ok, const String &p_message) {
	if (p_ok) {
		_set_status(vformat(TTR("Rendered %s."), p_message));
	} else {
		_set_status(vformat(TTR("Render failed: %s"), p_message), true);
	}
}

int TetraVideoWorkspace::_add_effect_to_clip(int p_clip_id, const Dictionary &p_operation) {
	return project->add_clip_effect(p_clip_id, p_operation["id"], p_operation["service"], TetraVideoAssets::get_default_values(p_operation));
}

void TetraVideoWorkspace::shortcut_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || !is_visible_in_tree()) {
		return;
	}
	// The keys belong to the workspace only while the keyboard is in it, and never while text is being typed.
	const Control *focus = get_viewport()->gui_get_focus_owner();
	if (!focus || !(focus == this || is_ancestor_of(focus)) || Object::cast_to<LineEdit>(focus) || Object::cast_to<TextEdit>(focus)) {
		return;
	}
	const bool echo = key->is_echo();
	if (ED_IS_SHORTCUT("tetra_video/step_back", p_event)) {
		monitor->step(-1);
	} else if (ED_IS_SHORTCUT("tetra_video/step_forward", p_event)) {
		monitor->step(1);
	} else if (echo) {
		return;
	} else if (ED_IS_SHORTCUT("tetra_video/play_pause", p_event)) {
		monitor->toggle_play();
	} else if (ED_IS_SHORTCUT("tetra_video/go_to_start", p_event)) {
		monitor->go_to_start();
	} else if (ED_IS_SHORTCUT("tetra_video/go_to_end", p_event)) {
		monitor->go_to_end();
	} else if (ED_IS_SHORTCUT("tetra_video/shuttle_backward", p_event)) {
		monitor->shuttle(-1);
	} else if (ED_IS_SHORTCUT("tetra_video/shuttle_pause", p_event)) {
		monitor->shuttle(0);
	} else if (ED_IS_SHORTCUT("tetra_video/shuttle_forward", p_event)) {
		monitor->shuttle(1);
	} else if (ED_IS_SHORTCUT("tetra_video/tool_select", p_event)) {
		set_tool(TOOL_SELECT);
	} else if (ED_IS_SHORTCUT("tetra_video/tool_razor", p_event)) {
		set_tool(TOOL_RAZOR);
	} else if (ED_IS_SHORTCUT("tetra_video/zoom_in", p_event)) {
		_zoom_step(1);
	} else if (ED_IS_SHORTCUT("tetra_video/zoom_out", p_event)) {
		_zoom_step(-1);
	} else {
		return;
	}
	accept_event();
}

TetraVideoWorkspace *TetraVideoWorkspace::get_singleton() {
	return singleton;
}

Ref<TetraVideoProject> TetraVideoWorkspace::get_project() const {
	return project;
}

Ref<TetraMltEngine> TetraVideoWorkspace::get_engine() const {
	return engine;
}

TetraVideoBin *TetraVideoWorkspace::get_bin() const {
	return bin;
}

TetraVideoMonitor *TetraVideoWorkspace::get_monitor() const {
	return monitor;
}

TetraVideoTimeline *TetraVideoWorkspace::get_timeline() const {
	return timeline;
}

TetraOperationBrowser *TetraVideoWorkspace::get_effects_browser() const {
	return effects_browser;
}

TetraVideoEffectStack *TetraVideoWorkspace::get_effect_stack() const {
	return effect_stack;
}

TetraVideoRenderDialog *TetraVideoWorkspace::get_render_dialog() const {
	return render_dialog;
}

void TetraVideoWorkspace::new_project() {
	_set_project(_make_default_project(), String());
	_set_status(TTR("New video project."));
}

Error TetraVideoWorkspace::open_project(const String &p_path) {
	Ref<TetraVideoProject> loaded;
	loaded.instantiate();
	const Error error = loaded->load(p_path);
	if (error != OK) {
		_set_status(vformat(TTR("Could not open %s: %s"), p_path.get_file(), loaded->get_last_error().is_empty() ? String(error_names[error]) : loaded->get_last_error()), true);
		return error;
	}
	const String local = ProjectSettings::get_singleton()->localize_path(p_path);
	_set_project(loaded, local.begins_with("res://") ? local : p_path);
	_set_status(vformat(TTR("Opened %s."), p_path.get_file()));
	return OK;
}

Error TetraVideoWorkspace::save_project(const String &p_path) {
	commit_pending_edits();
	String path = p_path.is_empty() ? project_path : p_path;
	if (path.is_empty()) {
		return ERR_FILE_BAD_PATH;
	}
	const String local = ProjectSettings::get_singleton()->localize_path(path);
	if (local.begins_with("res://")) {
		path = local;
	}
	const Error error = project->save(path);
	if (error != OK) {
		_show_error(vformat(TTR("Could not save %s: %s"), path, String(error_names[error])));
		return error;
	}
	project_path = path;
	saved_revision = project->get_revision();
	_update_title();
	_set_status(vformat(TTR("Saved %s."), path.get_file()));
	if (path.begins_with("res://") && EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}
	return OK;
}

String TetraVideoWorkspace::get_project_path() const {
	return project_path;
}

bool TetraVideoWorkspace::is_modified() const {
	return project->get_revision() != saved_revision;
}

int TetraVideoWorkspace::import_file(const String &p_path) {
	String path = p_path;
	const String local = ProjectSettings::get_singleton()->localize_path(p_path);
	if (local.begins_with("res://")) {
		path = local;
	}
	// A file is listed once; adding it again finds the clip that is already there.
	const PackedInt32Array ids = project->get_bin_clip_ids();
	for (int i = 0; i < ids.size(); i++) {
		if (String(project->get_bin_clip(ids[i])["source"]) == path) {
			return ids[i];
		}
	}
	const int id = TetraMltEngine::import_media(project, path);
	if (id < 0) {
		_set_status(vformat(TTR("%s could not be read as video, audio or image."), path.get_file()), true);
	} else {
		_set_status(vformat(TTR("Added %s to the bin."), path.get_file()));
	}
	return id;
}

int TetraVideoWorkspace::add_color_clip(const String &p_name, const Color &p_color, int p_length) {
	return project->add_bin_color(p_name, p_color, p_length);
}

int TetraVideoWorkspace::add_title_clip(const String &p_name, const String &p_text, int p_length) {
	return project->add_bin_text(p_name, p_text, p_length);
}

void TetraVideoWorkspace::seek(int p_frame) {
	if (monitor->is_clip_mode()) {
		monitor->show_project();
	}
	monitor->seek(p_frame);
}

int TetraVideoWorkspace::get_playhead() const {
	return timeline->get_playhead();
}

void TetraVideoWorkspace::play() {
	monitor->play();
}

void TetraVideoWorkspace::pause() {
	monitor->pause();
}

bool TetraVideoWorkspace::is_playing() const {
	return monitor->is_playing();
}

void TetraVideoWorkspace::set_tool(Tool p_tool) {
	timeline->set_tool(p_tool == TOOL_RAZOR ? TetraVideoTimeline::TOOL_RAZOR : TetraVideoTimeline::TOOL_SELECT);
}

TetraVideoWorkspace::Tool TetraVideoWorkspace::get_tool() const {
	return timeline->get_tool() == TetraVideoTimeline::TOOL_RAZOR ? TOOL_RAZOR : TOOL_SELECT;
}

void TetraVideoWorkspace::select_clip(int p_clip_id, bool p_add) {
	timeline->select_clip(p_clip_id, p_add);
}

void TetraVideoWorkspace::clear_selection() {
	timeline->clear_selection();
}

PackedInt32Array TetraVideoWorkspace::get_selected_clips() const {
	return timeline->get_selection();
}

int TetraVideoWorkspace::add_effect_to_selection(const String &p_operation_id) {
	commit_pending_edits();
	const Dictionary operation = TetraVideoAssets::get_operation(p_operation_id, project->get_width(), project->get_height());
	if (operation.is_empty()) {
		_set_status(vformat(TTR("There is no effect called %s."), p_operation_id), true);
		return 0;
	}
	const PackedInt32Array selection = timeline->get_selection();
	if (selection.is_empty()) {
		_set_status(TTR("Select a clip in the timeline first."), true);
		return 0;
	}
	const bool audio_effect = String(operation.get("kind", String())) == "audio_effect";
	int count = 0;
	int first_clip = -1;
	int first_index = -1;
	project->begin_undo_group(TTR("Add Effect"));
	for (int i = 0; i < selection.size(); i++) {
		const bool audio_clip = project->get_track_kind(project->get_clip(selection[i])["track"]) == TetraVideoProject::TRACK_AUDIO;
		if (audio_clip != audio_effect) {
			continue;
		}
		const int index = _add_effect_to_clip(selection[i], operation);
		if (index >= 0) {
			if (first_clip < 0) {
				first_clip = selection[i];
				first_index = index;
			}
			count++;
		}
	}
	project->end_undo_group();
	if (count == 0) {
		_set_status(audio_effect ? TTR("Audio effects go on clips of audio tracks.") : TTR("Video effects go on clips of video tracks."), true);
		return 0;
	}
	effect_stack->set_clip(first_clip);
	effect_stack->select_effect(first_index);
	show_effect_stack();
	_set_status(vformat(TTR("Added the effect %s."), String(operation["label"])));
	return count;
}

bool TetraVideoWorkspace::delete_selection(bool p_ripple) {
	commit_pending_edits();
	const PackedInt32Array selection = timeline->get_selection();
	if (selection.is_empty()) {
		_set_status(TTR("Nothing is selected."), true);
		return false;
	}
	bool done = false;
	if (p_ripple) {
		// Last clip first, so that closing one gap does not move the clips still to be removed.
		struct Entry {
			int id = 0;
			int position = 0;
			bool operator<(const Entry &p_other) const { return position > p_other.position; }
		};
		Vector<Entry> entries;
		for (int i = 0; i < selection.size(); i++) {
			Entry entry;
			entry.id = selection[i];
			entry.position = project->get_clip(selection[i])["position"];
			entries.push_back(entry);
		}
		entries.sort();
		project->begin_undo_group(TTR("Ripple Delete"));
		for (const Entry &entry : entries) {
			// A linked partner is gone already when its other half was removed.
			if (project->has_clip(entry.id) && project->ripple_delete(entry.id)) {
				done = true;
			}
		}
		project->end_undo_group();
	} else {
		done = project->delete_clips(selection);
	}
	if (!done) {
		_set_status(project->get_last_error(), true);
	}
	return done;
}

int TetraVideoWorkspace::split_at_playhead() {
	commit_pending_edits();
	const int frame = get_playhead();
	const PackedInt32Array selection = timeline->get_selection();
	int count = 0;
	if (selection.is_empty()) {
		count = project->split_all_at(frame).size();
	} else {
		project->begin_undo_group(TTR("Split Clips"));
		for (int i = 0; i < selection.size(); i++) {
			const Dictionary clip = project->get_clip(selection[i]);
			const int start = clip["position"];
			// A clip that was cut together with its linked partner ends at the playhead by now.
			if (frame > start && frame < start + int(clip["length"]) && project->split_clip(selection[i], frame) >= 0) {
				count++;
			}
		}
		project->end_undo_group();
	}
	if (count == 0) {
		_set_status(TTR("No clip to cut under the playhead."), true);
	}
	return count;
}

bool TetraVideoWorkspace::add_marker(const String &p_text) {
	commit_pending_edits();
	const String text = p_text.is_empty() ? vformat(TTR("Marker %d"), project->get_markers().size() + 1) : p_text;
	if (!project->add_marker(get_playhead(), text)) {
		_set_status(project->get_last_error(), true);
		return false;
	}
	return true;
}

bool TetraVideoWorkspace::undo() {
	commit_pending_edits();
	const String name = project->get_undo_name();
	if (!project->undo()) {
		_set_status(TTR("Nothing to undo."));
		return false;
	}
	_set_status(vformat(TTR("Undo: %s"), name));
	return true;
}

bool TetraVideoWorkspace::redo() {
	commit_pending_edits();
	const String name = project->get_redo_name();
	if (!project->redo()) {
		_set_status(TTR("Nothing to redo."));
		return false;
	}
	_set_status(vformat(TTR("Redo: %s"), name));
	return true;
}

void TetraVideoWorkspace::show_effects() {
	side_tabs->set_current_tab(0);
}

void TetraVideoWorkspace::show_effect_stack() {
	side_tabs->set_current_tab(1);
}

void TetraVideoWorkspace::open_render_dialog() {
	commit_pending_edits();
	monitor->pause();
	if (render_path_source != project_path && !render_dialog->is_rendering()) {
		// A saved project renders next to its file by default.
		render_path_source = project_path;
		render_dialog->set_output_path(project_path.is_empty() ? String("res://renders/video.mp4") : project_path.get_basename() + ".mp4");
	}
	render_dialog->popup_render(project, timeline->get_zone_in(), timeline->get_zone_out());
}

void TetraVideoWorkspace::open_project_settings() {
	settings_preset->select(0);
	settings_width->set_value(project->get_width());
	settings_height->set_value(project->get_height());
	settings_fps_num->set_value(project->get_fps_num());
	settings_fps_den->set_value(project->get_fps_den());
	settings_dialog->popup_centered(Size2(340, 0) * EDSCALE);
}

void TetraVideoWorkspace::focus_timeline() {
	if (timeline->is_visible_in_tree()) {
		timeline->grab_focus();
	}
}

void TetraVideoWorkspace::commit_pending_edits() {
	effect_stack->commit_pending();
}

bool TetraVideoWorkspace::is_graph_pending() const {
	if (!graph_timer->is_stopped()) {
		return true;
	}
	MutexLock lock(graph_mutex);
	return graph_has_pending;
}

void TetraVideoWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			_update_icons();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				// Leaving the workspace stops the playback, as closing a player would.
				monitor->pause();
			}
		} break;
	}
}

void TetraVideoWorkspace::_bind_methods() {
	ClassDB::bind_static_method("TetraVideoWorkspace", D_METHOD("get_singleton"), &TetraVideoWorkspace::get_singleton);
	ClassDB::bind_method(D_METHOD("get_project"), &TetraVideoWorkspace::get_project);
	ClassDB::bind_method(D_METHOD("get_engine"), &TetraVideoWorkspace::get_engine);
	ClassDB::bind_method(D_METHOD("get_bin"), &TetraVideoWorkspace::get_bin);
	ClassDB::bind_method(D_METHOD("get_monitor"), &TetraVideoWorkspace::get_monitor);
	ClassDB::bind_method(D_METHOD("get_timeline"), &TetraVideoWorkspace::get_timeline);
	ClassDB::bind_method(D_METHOD("get_effects_browser"), &TetraVideoWorkspace::get_effects_browser);
	ClassDB::bind_method(D_METHOD("get_effect_stack"), &TetraVideoWorkspace::get_effect_stack);
	ClassDB::bind_method(D_METHOD("get_render_dialog"), &TetraVideoWorkspace::get_render_dialog);

	ClassDB::bind_method(D_METHOD("new_project"), &TetraVideoWorkspace::new_project);
	ClassDB::bind_method(D_METHOD("open_project", "path"), &TetraVideoWorkspace::open_project);
	ClassDB::bind_method(D_METHOD("save_project", "path"), &TetraVideoWorkspace::save_project, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_project_path"), &TetraVideoWorkspace::get_project_path);
	ClassDB::bind_method(D_METHOD("is_modified"), &TetraVideoWorkspace::is_modified);

	ClassDB::bind_method(D_METHOD("import_file", "path"), &TetraVideoWorkspace::import_file);
	ClassDB::bind_method(D_METHOD("add_color_clip", "name", "color", "length"), &TetraVideoWorkspace::add_color_clip);
	ClassDB::bind_method(D_METHOD("add_title_clip", "name", "text", "length"), &TetraVideoWorkspace::add_title_clip);

	ClassDB::bind_method(D_METHOD("seek", "frame"), &TetraVideoWorkspace::seek);
	ClassDB::bind_method(D_METHOD("get_playhead"), &TetraVideoWorkspace::get_playhead);
	ClassDB::bind_method(D_METHOD("play"), &TetraVideoWorkspace::play);
	ClassDB::bind_method(D_METHOD("pause"), &TetraVideoWorkspace::pause);
	ClassDB::bind_method(D_METHOD("is_playing"), &TetraVideoWorkspace::is_playing);

	ClassDB::bind_method(D_METHOD("set_tool", "tool"), &TetraVideoWorkspace::set_tool);
	ClassDB::bind_method(D_METHOD("get_tool"), &TetraVideoWorkspace::get_tool);
	ClassDB::bind_method(D_METHOD("select_clip", "clip_id", "add"), &TetraVideoWorkspace::select_clip, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_selection"), &TetraVideoWorkspace::clear_selection);
	ClassDB::bind_method(D_METHOD("get_selected_clips"), &TetraVideoWorkspace::get_selected_clips);

	ClassDB::bind_method(D_METHOD("add_effect_to_selection", "operation_id"), &TetraVideoWorkspace::add_effect_to_selection);
	ClassDB::bind_method(D_METHOD("delete_selection", "ripple"), &TetraVideoWorkspace::delete_selection, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("split_at_playhead"), &TetraVideoWorkspace::split_at_playhead);
	ClassDB::bind_method(D_METHOD("add_marker", "text"), &TetraVideoWorkspace::add_marker, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("undo"), &TetraVideoWorkspace::undo);
	ClassDB::bind_method(D_METHOD("redo"), &TetraVideoWorkspace::redo);

	ClassDB::bind_method(D_METHOD("show_effects"), &TetraVideoWorkspace::show_effects);
	ClassDB::bind_method(D_METHOD("show_effect_stack"), &TetraVideoWorkspace::show_effect_stack);
	ClassDB::bind_method(D_METHOD("open_render_dialog"), &TetraVideoWorkspace::open_render_dialog);
	ClassDB::bind_method(D_METHOD("open_project_settings"), &TetraVideoWorkspace::open_project_settings);
	ClassDB::bind_method(D_METHOD("focus_timeline"), &TetraVideoWorkspace::focus_timeline);
	ClassDB::bind_method(D_METHOD("commit_pending_edits"), &TetraVideoWorkspace::commit_pending_edits);
	ClassDB::bind_method(D_METHOD("is_graph_pending"), &TetraVideoWorkspace::is_graph_pending);

	BIND_ENUM_CONSTANT(TOOL_SELECT);
	BIND_ENUM_CONSTANT(TOOL_RAZOR);
}

TetraVideoWorkspace::TetraVideoWorkspace() {
	singleton = this;
	set_process_shortcut_input(true);

	// Registered here so that they are listed, and can be rebound, in Editor Settings > Shortcuts.
	ED_SHORTCUT("tetra_video/play_pause", TTRC("Video: Play / Pause"), Key::SPACE);
	ED_SHORTCUT("tetra_video/step_back", TTRC("Video: Previous Frame"), Key::LEFT);
	ED_SHORTCUT("tetra_video/step_forward", TTRC("Video: Next Frame"), Key::RIGHT);
	ED_SHORTCUT("tetra_video/go_to_start", TTRC("Video: Go to Start"), Key::HOME);
	ED_SHORTCUT("tetra_video/go_to_end", TTRC("Video: Go to End"), Key::END);
	ED_SHORTCUT("tetra_video/shuttle_backward", TTRC("Video: Play Backward"), Key::J);
	ED_SHORTCUT("tetra_video/shuttle_pause", TTRC("Video: Pause"), Key::K);
	ED_SHORTCUT("tetra_video/shuttle_forward", TTRC("Video: Play Forward"), Key::L);
	ED_SHORTCUT("tetra_video/tool_select", TTRC("Video: Selection Tool"), Key::S);
	ED_SHORTCUT("tetra_video/tool_razor", TTRC("Video: Razor Tool"), Key::X);
	ED_SHORTCUT("tetra_video/zoom_in", TTRC("Video: Zoom In"), KeyModifierMask::CMD_OR_CTRL | Key::EQUAL);
	ED_SHORTCUT("tetra_video/zoom_out", TTRC("Video: Zoom Out"), KeyModifierMask::CMD_OR_CTRL | Key::MINUS);

	engine.instantiate();
	thumbnails.instantiate();

	// Menu bar.
	HBoxContainer *menu_bar = memnew(HBoxContainer);
	add_child(menu_bar);

	project_menu = memnew(MenuButton);
	project_menu->set_text(TTR("Project"));
	project_menu->set_switch_on_hover(true);
	// Menu shortcuts only act while the keyboard is inside the workspace.
	project_menu->set_shortcut_context(this);
	menu_bar->add_child(project_menu);
	{
		PopupMenu *menu = project_menu->get_popup();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/new_project", TTRC("New")), PROJECT_NEW);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/open_project", TTRC("Open...")), PROJECT_OPEN);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/save_project", TTRC("Save"), KeyModifierMask::CMD_OR_CTRL | Key::S), PROJECT_SAVE);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/save_project_as", TTRC("Save As..."), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::S), PROJECT_SAVE_AS);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/project_settings", TTRC("Project Settings...")), PROJECT_SETTINGS);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/render", TTRC("Render..."), KeyModifierMask::CMD_OR_CTRL | Key::ENTER), PROJECT_RENDER);
		menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option));
	}

	edit_menu = memnew(MenuButton);
	edit_menu->set_text(TTR("Edit"));
	edit_menu->set_switch_on_hover(true);
	edit_menu->set_shortcut_context(this);
	menu_bar->add_child(edit_menu);
	{
		PopupMenu *menu = edit_menu->get_popup();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/undo", TTRC("Undo"), KeyModifierMask::CMD_OR_CTRL | Key::Z), EDIT_UNDO);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/redo", TTRC("Redo"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::Z), EDIT_REDO);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/delete", TTRC("Delete"), Key::KEY_DELETE), EDIT_DELETE);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/ripple_delete", TTRC("Ripple Delete"), KeyModifierMask::SHIFT | Key::KEY_DELETE), EDIT_RIPPLE_DELETE);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/select_all", TTRC("Select All Clips"), KeyModifierMask::CMD_OR_CTRL | Key::A), EDIT_SELECT_ALL);
		menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option));
	}

	timeline_menu = memnew(MenuButton);
	timeline_menu->set_text(TTR("Timeline"));
	timeline_menu->set_switch_on_hover(true);
	timeline_menu->set_shortcut_context(this);
	menu_bar->add_child(timeline_menu);
	{
		PopupMenu *menu = timeline_menu->get_popup();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/add_video_track", TTRC("Add Video Track")), TIMELINE_ADD_VIDEO_TRACK);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/add_audio_track", TTRC("Add Audio Track")), TIMELINE_ADD_AUDIO_TRACK);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/delete_track", TTRC("Delete Track")), TIMELINE_DELETE_TRACK);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/split_at_playhead", TTRC("Split at Playhead"), KeyModifierMask::SHIFT | Key::R), TIMELINE_SPLIT);
		menu->add_shortcut(ED_GET_SHORTCUT("tetra_video/ripple_delete"), EDIT_RIPPLE_DELETE);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/insert_space", TTRC("Insert Space...")), TIMELINE_INSERT_SPACE);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/remove_space", TTRC("Remove Space")), TIMELINE_REMOVE_SPACE);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/add_marker", TTRC("Add Marker"), Key::M), TIMELINE_ADD_MARKER);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/delete_marker", TTRC("Delete Marker")), TIMELINE_DELETE_MARKER);
		menu->add_separator();
		menu->add_shortcut(ED_SHORTCUT("tetra_video/zone_in", TTRC("Set Zone In"), Key::I), TIMELINE_ZONE_IN);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/zone_out", TTRC("Set Zone Out"), Key::O), TIMELINE_ZONE_OUT);
		menu->add_shortcut(ED_SHORTCUT("tetra_video/zone_clear", TTRC("Clear Zone")), TIMELINE_ZONE_CLEAR);
		menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option));
	}

	menu_bar->add_child(memnew(VSeparator));

	undo_button = _make_tool_button(TTR("Undo"));
	undo_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option).bind(int(EDIT_UNDO)));
	menu_bar->add_child(undo_button);
	redo_button = _make_tool_button(TTR("Redo"));
	redo_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option).bind(int(EDIT_REDO)));
	menu_bar->add_child(redo_button);

	title_label = memnew(Label);
	title_label->set_h_size_flags(SIZE_EXPAND_FILL);
	title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	title_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title_label->set_mouse_filter(MOUSE_FILTER_PASS);
	menu_bar->add_child(title_label);

	render_button = memnew(Button(TTR("Render...")));
	render_button->set_focus_mode(FOCUS_NONE);
	render_button->set_tooltip_text(TTR("Render the timeline to a video file."));
	render_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option).bind(int(PROJECT_RENDER)));
	menu_bar->add_child(render_button);

	// Upper area: bin | monitor | effects, over the timeline.
	VSplitContainer *main_split = memnew(VSplitContainer);
	main_split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(main_split);

	HSplitContainer *top_split = memnew(HSplitContainer);
	top_split->set_v_size_flags(SIZE_EXPAND_FILL);
	top_split->set_stretch_ratio(1.2);
	main_split->add_child(top_split);

	VBoxContainer *bin_box = memnew(VBoxContainer);
	bin_box->set_h_size_flags(SIZE_EXPAND_FILL);
	bin_box->set_stretch_ratio(1.0);
	top_split->add_child(bin_box);
	Label *bin_title = memnew(Label(TTR("Project Bin")));
	bin_title->set_theme_type_variation("HeaderSmall");
	bin_box->add_child(bin_title);
	bin = memnew(TetraVideoBin);
	bin->set_v_size_flags(SIZE_EXPAND_FILL);
	bin->connect("clip_activated", callable_mp(this, &TetraVideoWorkspace::_bin_clip_activated));
	bin->connect("import_requested", callable_mp(this, &TetraVideoWorkspace::_bin_import_requested));
	bin_box->add_child(bin);

	HSplitContainer *right_split = memnew(HSplitContainer);
	right_split->set_h_size_flags(SIZE_EXPAND_FILL);
	right_split->set_stretch_ratio(3.4);
	top_split->add_child(right_split);

	monitor = memnew(TetraVideoMonitor);
	monitor->set_h_size_flags(SIZE_EXPAND_FILL);
	monitor->set_stretch_ratio(1.5);
	monitor->set_project_engine(engine);
	monitor->connect("position_changed", callable_mp(this, &TetraVideoWorkspace::_monitor_position_changed));
	right_split->add_child(monitor);

	side_tabs = memnew(TabContainer);
	side_tabs->set_h_size_flags(SIZE_EXPAND_FILL);
	side_tabs->set_stretch_ratio(1.0);
	side_tabs->set_custom_minimum_size(Size2(230, 0) * EDSCALE);
	right_split->add_child(side_tabs);

	effects_browser = memnew(TetraOperationBrowser);
	effects_browser->set_name("Effects");
	effects_browser->connect("operation_activated", callable_mp(this, &TetraVideoWorkspace::_effect_activated));
	side_tabs->add_child(effects_browser);
	side_tabs->set_tab_title(0, TTR("Effects"));
	// The browser's list is a Tree of its own; dragging a row out of it carries the effect to a timeline clip.
	const TypedArray<Node> trees = effects_browser->find_children("*", "Tree", true, false);
	for (int i = 0; i < trees.size(); i++) {
		Tree *tree = Object::cast_to<Tree>(trees[i]);
		if (tree) {
			tree->set_drag_forwarding(callable_mp(this, &TetraVideoWorkspace::_effects_get_drag_data).bind(tree), Callable(), Callable());
		}
	}

	effect_stack = memnew(TetraVideoEffectStack);
	effect_stack->set_name("EffectStack");
	side_tabs->add_child(effect_stack);
	side_tabs->set_tab_title(1, TTR("Effect Stack"));

	// Timeline with its tool bar.
	VBoxContainer *timeline_box = memnew(VBoxContainer);
	timeline_box->set_v_size_flags(SIZE_EXPAND_FILL);
	timeline_box->set_stretch_ratio(1.0);
	main_split->add_child(timeline_box);

	HBoxContainer *tool_bar = memnew(HBoxContainer);
	timeline_box->add_child(tool_bar);

	select_tool_button = _make_tool_button(TTR("Selection Tool (S): select, move and trim clips"));
	select_tool_button->set_toggle_mode(true);
	select_tool_button->set_pressed(true);
	select_tool_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_tool_button_pressed).bind(int(TOOL_SELECT)));
	tool_bar->add_child(select_tool_button);
	razor_tool_button = _make_tool_button(TTR("Razor Tool (X): click a clip to cut it"));
	razor_tool_button->set_toggle_mode(true);
	razor_tool_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_tool_button_pressed).bind(int(TOOL_RAZOR)));
	tool_bar->add_child(razor_tool_button);
	tool_bar->add_child(memnew(VSeparator));
	snap_button = _make_tool_button(TTR("Snap to clip edges, markers and the playhead"));
	snap_button->set_toggle_mode(true);
	snap_button->set_pressed(true);
	snap_button->connect(SceneStringName(toggled), callable_mp(this, &TetraVideoWorkspace::_snap_toggled));
	tool_bar->add_child(snap_button);
	tool_bar->add_child(memnew(VSeparator));
	split_button = _make_tool_button(TTR("Split at Playhead (Shift+R)"));
	split_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option).bind(int(TIMELINE_SPLIT)));
	tool_bar->add_child(split_button);
	delete_button = _make_tool_button(TTR("Delete the selected clips (Delete)"));
	delete_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::delete_selection).bind(false));
	tool_bar->add_child(delete_button);
	ripple_button = _make_tool_button(TTR("Ripple Delete: delete the selected clips and close the gap (Shift+Delete)"));
	ripple_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::delete_selection).bind(true));
	tool_bar->add_child(ripple_button);
	marker_button = _make_tool_button(TTR("Add Marker at the playhead (M)"));
	marker_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_menu_option).bind(int(TIMELINE_ADD_MARKER)));
	tool_bar->add_child(marker_button);

	tool_bar->add_spacer();

	zoom_out_button = _make_tool_button(TTR("Zoom Out (Ctrl+-, Ctrl+Wheel)"));
	zoom_out_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_zoom_step).bind(-1));
	tool_bar->add_child(zoom_out_button);
	zoom_slider = memnew(HSlider);
	zoom_slider->set_min(0);
	zoom_slider->set_max(ZOOM_SLIDER_STEPS);
	zoom_slider->set_step(1);
	zoom_slider->set_focus_mode(FOCUS_NONE);
	zoom_slider->set_custom_minimum_size(Size2(120, 0) * EDSCALE);
	zoom_slider->set_v_size_flags(SIZE_SHRINK_CENTER);
	zoom_slider->set_tooltip_text(TTR("Timeline Zoom"));
	zoom_slider->connect(SceneStringName(value_changed), callable_mp(this, &TetraVideoWorkspace::_zoom_slider_changed));
	tool_bar->add_child(zoom_slider);
	zoom_in_button = _make_tool_button(TTR("Zoom In (Ctrl+=, Ctrl+Wheel)"));
	zoom_in_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoWorkspace::_zoom_step).bind(1));
	tool_bar->add_child(zoom_in_button);

	timeline = memnew(TetraVideoTimeline);
	timeline->set_v_size_flags(SIZE_EXPAND_FILL);
	timeline->set_thumbnails(thumbnails);
	timeline->connect("seek_requested", callable_mp(this, &TetraVideoWorkspace::_timeline_seek_requested));
	timeline->connect("selection_changed", callable_mp(this, &TetraVideoWorkspace::_timeline_selection_changed));
	timeline->connect("zoom_changed", callable_mp(this, &TetraVideoWorkspace::_timeline_zoom_changed));
	timeline->connect("tool_changed", callable_mp(this, &TetraVideoWorkspace::_timeline_tool_changed));
	timeline->connect("edit_refused", callable_mp(this, &TetraVideoWorkspace::_timeline_edit_refused));
	timeline->connect("clip_activated", callable_mp(this, &TetraVideoWorkspace::_timeline_clip_activated));
	timeline->connect("effect_dropped", callable_mp(this, &TetraVideoWorkspace::_timeline_effect_dropped));
	timeline->connect("interaction_started", callable_mp(this, &TetraVideoWorkspace::commit_pending_edits));
	timeline_box->add_child(timeline);

	zoom_fit_button = _make_tool_button(TTR("Zoom to Fit: show the whole timeline"));
	zoom_fit_button->connect(SceneStringName(pressed), callable_mp(timeline, &TetraVideoTimeline::zoom_to_fit));
	tool_bar->add_child(zoom_fit_button);

	// Status line.
	HBoxContainer *status_bar = memnew(HBoxContainer);
	add_child(status_bar);
	status_label = memnew(Label);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_bar->add_child(status_label);
	info_label = memnew(Label);
	info_label->set_modulate(Color(1, 1, 1, 0.7));
	status_bar->add_child(info_label);

	status_timer = memnew(Timer);
	status_timer->set_one_shot(true);
	status_timer->set_wait_time(6.0);
	status_timer->connect("timeout", callable_mp(this, &TetraVideoWorkspace::_status_timeout));
	add_child(status_timer);

	graph_timer = memnew(Timer);
	graph_timer->set_one_shot(true);
	graph_timer->set_wait_time(0.06);
	graph_timer->connect("timeout", callable_mp(this, &TetraVideoWorkspace::_rebuild_graph));
	add_child(graph_timer);

	// Dialogs.
	open_dialog = memnew(EditorFileDialog);
	open_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	open_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
	open_dialog->set_title(TTR("Open Video Project"));
	open_dialog->add_filter("*.mlt", TTR("MLT Video Project"));
	open_dialog->connect("file_selected", callable_mp(this, &TetraVideoWorkspace::_open_file_selected));
	add_child(open_dialog);

	save_dialog = memnew(EditorFileDialog);
	save_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	save_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	save_dialog->set_title(TTR("Save Video Project As"));
	save_dialog->add_filter("*.mlt", TTR("MLT Video Project"));
	save_dialog->connect("file_selected", callable_mp(this, &TetraVideoWorkspace::_save_file_selected));
	add_child(save_dialog);

	discard_dialog = memnew(ConfirmationDialog);
	discard_dialog->set_title(TTR("Unsaved Changes"));
	discard_dialog->set_text(TTR("The video project has unsaved changes.\nDiscard them?"));
	discard_dialog->set_ok_button_text(TTR("Discard"));
	discard_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoWorkspace::_discard_confirmed));
	add_child(discard_dialog);

	error_dialog = memnew(AcceptDialog);
	error_dialog->set_title(TTR("Video"));
	add_child(error_dialog);

	settings_dialog = memnew(ConfirmationDialog);
	settings_dialog->set_title(TTR("Video Project Settings"));
	settings_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoWorkspace::_settings_confirmed));
	add_child(settings_dialog);
	{
		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(2);
		settings_dialog->add_child(grid);
		grid->add_child(memnew(Label(TTR("Preset"))));
		settings_preset = memnew(OptionButton);
		settings_preset->set_h_size_flags(SIZE_EXPAND_FILL);
		settings_preset->add_item(TTR("Custom"));
		const PackedStringArray presets = TetraVideoProject::get_profile_presets();
		for (const String &preset : presets) {
			settings_preset->add_item(preset);
		}
		settings_preset->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoWorkspace::_settings_preset_selected));
		grid->add_child(settings_preset);
		grid->add_child(memnew(Label(TTR("Width"))));
		settings_width = memnew(SpinBox);
		settings_width->set_min(16);
		settings_width->set_max(8192);
		settings_width->set_step(2);
		settings_width->set_suffix("px");
		grid->add_child(settings_width);
		grid->add_child(memnew(Label(TTR("Height"))));
		settings_height = memnew(SpinBox);
		settings_height->set_min(16);
		settings_height->set_max(8192);
		settings_height->set_step(2);
		settings_height->set_suffix("px");
		grid->add_child(settings_height);
		grid->add_child(memnew(Label(TTR("Frame Rate"))));
		HBoxContainer *rate = memnew(HBoxContainer);
		grid->add_child(rate);
		settings_fps_num = memnew(SpinBox);
		settings_fps_num->set_min(1);
		settings_fps_num->set_max(240000);
		settings_fps_num->set_h_size_flags(SIZE_EXPAND_FILL);
		settings_fps_num->set_tooltip_text(TTR("Frames (numerator of the frame rate)"));
		rate->add_child(settings_fps_num);
		rate->add_child(memnew(Label("/")));
		settings_fps_den = memnew(SpinBox);
		settings_fps_den->set_min(1);
		settings_fps_den->set_max(1001);
		settings_fps_den->set_h_size_flags(SIZE_EXPAND_FILL);
		settings_fps_den->set_tooltip_text(TTR("Seconds (denominator of the frame rate; 1001 for 29.97 and 23.976)"));
		rate->add_child(settings_fps_den);
	}

	space_dialog = memnew(ConfirmationDialog);
	space_dialog->set_title(TTR("Insert Space"));
	space_dialog->set_ok_button_text(TTR("Insert"));
	space_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoWorkspace::_space_confirmed));
	add_child(space_dialog);
	{
		VBoxContainer *box = memnew(VBoxContainer);
		space_dialog->add_child(box);
		HBoxContainer *row = memnew(HBoxContainer);
		box->add_child(row);
		row->add_child(memnew(Label(TTR("Duration"))));
		space_seconds = memnew(SpinBox);
		space_seconds->set_min(0.04);
		space_seconds->set_max(3600);
		space_seconds->set_step(0.01);
		space_seconds->set_value(1);
		space_seconds->set_suffix("s");
		space_seconds->set_h_size_flags(SIZE_EXPAND_FILL);
		row->add_child(space_seconds);
		space_active_only = memnew(CheckBox(TTR("Active track only")));
		space_active_only->set_tooltip_text(TTR("Push the clips of the active track only; otherwise every unlocked track is pushed."));
		box->add_child(space_active_only);
	}

	render_dialog = memnew(TetraVideoRenderDialog);
	render_dialog->connect("render_finished", callable_mp(this, &TetraVideoWorkspace::_render_finished));
	add_child(render_dialog);

	graph_thread.start(_graph_thread_main, this);

	bin->set_thumbnails(thumbnails);
	_set_project(_make_default_project(), String());
	_timeline_zoom_changed(timeline->get_zoom());
	_status_timeout();
}

TetraVideoWorkspace::~TetraVideoWorkspace() {
	graph_quit.set();
	graph_semaphore.post();
	graph_thread.wait_to_finish();
	if (singleton == this) {
		singleton = nullptr;
	}
}
