#include "tetra_video_render_dialog.h"

#include "../tetra_video_project.h"
#include "../tetra_video_renderer.h"
#include "tetra_video_timecode.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"

void TetraVideoRenderDialog::_update_presets() {
	updating = true;
	preset_tree->clear();
	TreeItem *root = preset_tree->create_item();
	HashMap<String, TreeItem *> groups;
	TreeItem *selected = nullptr;
	const TypedArray<Dictionary> presets = TetraVideoRenderer::get_presets();
	for (int i = 0; i < presets.size(); i++) {
		const Dictionary preset = presets[i];
		const String group = preset.get("group", String());
		if (!groups.has(group)) {
			TreeItem *folder = preset_tree->create_item(root);
			folder->set_text(0, group);
			folder->set_selectable(0, false);
			folder->set_custom_color(0, get_theme_color(SNAME("font_color"), SNAME("Label")) * Color(1, 1, 1, 0.7));
			// The built-in presets come first and stay open; Kdenlive's many groups start folded.
			folder->set_collapsed(!groups.is_empty());
			groups[group] = folder;
		}
		TreeItem *item = preset_tree->create_item(groups[group]);
		item->set_text(0, String(preset.get("name", String())));
		item->set_suffix(0, "." + String(preset.get("extension", String())));
		item->set_metadata(0, preset.get("id", String()));
		if (String(preset.get("id", String())) == preset_id) {
			selected = item;
		}
	}
	if (selected) {
		selected->get_parent()->set_collapsed(false);
		selected->select(0);
		preset_tree->scroll_to_item(selected);
	}
	updating = false;
	_update_preset_label();
}

void TetraVideoRenderDialog::_update_preset_label() {
	const Dictionary preset = TetraVideoRenderer::get_preset(preset_id);
	if (preset.is_empty()) {
		preset_label->set_text(TTR("Choose a preset."));
		return;
	}
	preset_label->set_text(vformat("%s / %s (.%s)", String(preset.get("group", String())), String(preset.get("name", String())), String(preset.get("extension", String()))));
	preset_label->set_tooltip_text(String(preset.get("args", String())));
}

void TetraVideoRenderDialog::_update_state() {
	const bool rendering = is_rendering();
	const bool zone = range_option->get_selected() == RANGE_ZONE;
	get_ok_button()->set_disabled(rendering || project.is_null() || project->get_duration() == 0 || TetraVideoRenderer::find_melt().is_empty());
	cancel_render_button->set_disabled(!rendering);
	preset_tree->set_mouse_filter(rendering ? Control::MOUSE_FILTER_IGNORE : Control::MOUSE_FILTER_STOP);
	path_edit->set_editable(!rendering);
	browse_button->set_disabled(rendering);
	range_option->set_disabled(rendering);
	scale_option->set_disabled(rendering);
	in_spin->set_editable(zone && !rendering);
	out_spin->set_editable(zone && !rendering);
}

void TetraVideoRenderDialog::_apply_extension() {
	const Dictionary preset = TetraVideoRenderer::get_preset(preset_id);
	const String extension = preset.get("extension", String());
	const String path = path_edit->get_text().strip_edges();
	if (!extension.is_empty() && !path.is_empty() && path.get_extension() != extension) {
		path_edit->set_text(path.get_basename() + "." + extension);
	}
}

void TetraVideoRenderDialog::_preset_selected() {
	TreeItem *item = preset_tree->get_selected();
	if (updating || !item) {
		return;
	}
	preset_id = item->get_metadata(0);
	_apply_extension();
	_update_preset_label();
}

void TetraVideoRenderDialog::_range_selected(int p_index) {
	_update_state();
}

void TetraVideoRenderDialog::_browse_pressed() {
	const Dictionary preset = TetraVideoRenderer::get_preset(preset_id);
	const String extension = preset.get("extension", String());
	file_dialog->clear_filters();
	if (!extension.is_empty()) {
		file_dialog->add_filter("*." + extension, String(preset.get("name", String())));
	}
	file_dialog->set_current_path(ProjectSettings::get_singleton()->globalize_path(get_output_path()));
	file_dialog->popup_file_dialog();
}

void TetraVideoRenderDialog::_file_selected(const String &p_path) {
	set_output_path(p_path);
	_apply_extension();
}

void TetraVideoRenderDialog::_render_progress(int p_frame, double p_percent) {
	progress_bar->set_value(p_percent);
	const double fps = project.is_valid() ? project->get_fps() : 25.0;
	status_label->set_text(vformat(TTR("Rendering %s (frame %d)"), TetraVideoTimecode::from_frames(p_frame, fps), p_frame));
}

void TetraVideoRenderDialog::_render_finished(bool p_ok, const String &p_message) {
	if (p_ok) {
		progress_bar->set_value(100);
		status_label->set_text(vformat(TTR("Finished: %s"), p_message));
		// A file rendered into the project appears in the FileSystem dock after a rescan.
		const String local = ProjectSettings::get_singleton()->localize_path(p_message);
		if (local.begins_with("res://") && EditorFileSystem::get_singleton() && !EditorFileSystem::get_singleton()->is_scanning()) {
			EditorFileSystem::get_singleton()->scan();
		}
	} else {
		progress_bar->set_value(0);
		status_label->set_text(vformat(TTR("Render failed: %s"), p_message));
	}
	_update_state();
	emit_signal(SNAME("render_finished"), p_ok, p_message);
}

void TetraVideoRenderDialog::ok_pressed() {
	start_render();
}

void TetraVideoRenderDialog::popup_render(const Ref<TetraVideoProject> &p_project, int p_zone_in, int p_zone_out) {
	if (!is_rendering()) {
		project = p_project;
		const int last = project.is_valid() ? MAX(0, project->get_duration() - 1) : 0;
		in_spin->set_max(last);
		out_spin->set_max(last);
		const bool with_zone = p_zone_in >= 0 && p_zone_out >= p_zone_in;
		in_spin->set_value(with_zone ? MIN(p_zone_in, last) : 0);
		out_spin->set_value(with_zone ? MIN(p_zone_out, last) : last);
		range_option->select(with_zone ? RANGE_ZONE : RANGE_PROJECT);
		progress_bar->set_value(0);
		if (TetraVideoRenderer::find_melt().is_empty()) {
			status_label->set_text(TTR("The melt program was not found; rendering is not available."));
		} else if (project.is_null() || project->get_duration() == 0) {
			status_label->set_text(TTR("The timeline is empty."));
		} else {
			status_label->set_text(vformat(TTR("Ready: %s, %dx%d at %s fps"), TetraVideoTimecode::from_frames(project->get_duration(), project->get_fps()), project->get_width(), project->get_height(), String::num(project->get_fps(), 2)));
		}
	}
	_update_presets();
	_apply_extension();
	_update_state();
	popup_centered_clamped(Size2(760, 480) * EDSCALE, 0.9);
}

void TetraVideoRenderDialog::set_preset(const String &p_preset_id) {
	if (TetraVideoRenderer::get_preset(p_preset_id).is_empty()) {
		return;
	}
	preset_id = p_preset_id;
	_update_presets();
	_apply_extension();
}

String TetraVideoRenderDialog::get_preset() const {
	return preset_id;
}

void TetraVideoRenderDialog::set_output_path(const String &p_path) {
	// Paths inside the Godot project are kept as res:// so that the project can be moved.
	const String local = ProjectSettings::get_singleton()->localize_path(p_path);
	path_edit->set_text(local.begins_with("res://") ? local : p_path);
}

String TetraVideoRenderDialog::get_output_path() const {
	return path_edit->get_text().strip_edges();
}

void TetraVideoRenderDialog::set_range_mode(RangeMode p_mode) {
	range_option->select(p_mode == RANGE_ZONE ? RANGE_ZONE : RANGE_PROJECT);
	_update_state();
}

TetraVideoRenderDialog::RangeMode TetraVideoRenderDialog::get_range_mode() const {
	return range_option->get_selected() == RANGE_ZONE ? RANGE_ZONE : RANGE_PROJECT;
}

void TetraVideoRenderDialog::set_range(int p_in, int p_out) {
	in_spin->set_value(p_in);
	out_spin->set_value(p_out);
	set_range_mode(RANGE_ZONE);
}

void TetraVideoRenderDialog::set_scale_divisor(int p_divisor) {
	scale_option->select(scale_option->get_item_index(p_divisor >= 4 ? 4 : (p_divisor >= 2 ? 2 : 1)));
}

int TetraVideoRenderDialog::get_scale_divisor() const {
	return scale_option->get_selected_id();
}

Error TetraVideoRenderDialog::start_render() {
	if (project.is_null()) {
		return ERR_UNCONFIGURED;
	}
	if (is_rendering()) {
		return ERR_BUSY;
	}
	const String path = get_output_path();
	if (path.is_empty()) {
		status_label->set_text(TTR("Choose an output file."));
		return ERR_FILE_BAD_PATH;
	}
	Dictionary options;
	if (get_range_mode() == RANGE_ZONE) {
		options["in"] = int(MIN(in_spin->get_value(), out_spin->get_value()));
		options["out"] = int(MAX(in_spin->get_value(), out_spin->get_value()));
	}
	const int divisor = get_scale_divisor();
	if (divisor > 1) {
		// Encoders want even sizes.
		options["width"] = MAX(2, project->get_width() / divisor / 2 * 2);
		options["height"] = MAX(2, project->get_height() / divisor / 2 * 2);
	}
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path).get_base_dir());

	renderer.instantiate();
	renderer->connect("progress", callable_mp(this, &TetraVideoRenderDialog::_render_progress));
	renderer->connect("finished", callable_mp(this, &TetraVideoRenderDialog::_render_finished));
	const Error error = renderer->start(project, path, preset_id, options);
	if (error != OK) {
		status_label->set_text(vformat(TTR("Render failed: %s"), renderer->get_last_error().is_empty() ? String(error_names[error]) : renderer->get_last_error()));
		renderer.unref();
	} else {
		progress_bar->set_value(0);
		status_label->set_text(TTR("Starting the render..."));
	}
	_update_state();
	return error;
}

void TetraVideoRenderDialog::cancel_render() {
	if (renderer.is_valid() && renderer->is_running()) {
		status_label->set_text(TTR("Cancelling..."));
		renderer->cancel();
	}
}

bool TetraVideoRenderDialog::is_rendering() const {
	return renderer.is_valid() && renderer->is_running();
}

Ref<TetraVideoRenderer> TetraVideoRenderDialog::get_renderer() const {
	return renderer;
}

void TetraVideoRenderDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			browse_button->set_button_icon(get_editor_theme_icon(SNAME("Folder")));
		} break;
	}
}

void TetraVideoRenderDialog::_bind_methods() {
	ClassDB::bind_method(D_METHOD("popup_render", "project", "zone_in", "zone_out"), &TetraVideoRenderDialog::popup_render, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_preset", "preset_id"), &TetraVideoRenderDialog::set_preset);
	ClassDB::bind_method(D_METHOD("get_preset"), &TetraVideoRenderDialog::get_preset);
	ClassDB::bind_method(D_METHOD("set_output_path", "path"), &TetraVideoRenderDialog::set_output_path);
	ClassDB::bind_method(D_METHOD("get_output_path"), &TetraVideoRenderDialog::get_output_path);
	ClassDB::bind_method(D_METHOD("set_range_mode", "mode"), &TetraVideoRenderDialog::set_range_mode);
	ClassDB::bind_method(D_METHOD("get_range_mode"), &TetraVideoRenderDialog::get_range_mode);
	ClassDB::bind_method(D_METHOD("set_range", "in", "out"), &TetraVideoRenderDialog::set_range);
	ClassDB::bind_method(D_METHOD("set_scale_divisor", "divisor"), &TetraVideoRenderDialog::set_scale_divisor);
	ClassDB::bind_method(D_METHOD("get_scale_divisor"), &TetraVideoRenderDialog::get_scale_divisor);
	ClassDB::bind_method(D_METHOD("start_render"), &TetraVideoRenderDialog::start_render);
	ClassDB::bind_method(D_METHOD("cancel_render"), &TetraVideoRenderDialog::cancel_render);
	ClassDB::bind_method(D_METHOD("is_rendering"), &TetraVideoRenderDialog::is_rendering);
	ClassDB::bind_method(D_METHOD("get_renderer"), &TetraVideoRenderDialog::get_renderer);

	ADD_SIGNAL(MethodInfo("render_finished", PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(RANGE_PROJECT);
	BIND_ENUM_CONSTANT(RANGE_ZONE);
}

TetraVideoRenderDialog::TetraVideoRenderDialog() {
	set_title(TTR("Render"));
	set_ok_button_text(TTR("Render to File"));
	set_cancel_button_text(TTR("Close"));
	set_hide_on_ok(false);

	HSplitContainer *split = memnew(HSplitContainer);
	add_child(split);

	VBoxContainer *left = memnew(VBoxContainer);
	left->set_custom_minimum_size(Size2(280, 300) * EDSCALE);
	split->add_child(left);
	Label *presets_title = memnew(Label(TTR("Presets")));
	presets_title->set_theme_type_variation("HeaderSmall");
	left->add_child(presets_title);
	preset_tree = memnew(Tree);
	preset_tree->set_hide_root(true);
	preset_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	preset_tree->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoRenderDialog::_preset_selected));
	left->add_child(preset_tree);

	VBoxContainer *right = memnew(VBoxContainer);
	right->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	right->set_custom_minimum_size(Size2(360, 0) * EDSCALE);
	split->add_child(right);

	Label *output_title = memnew(Label(TTR("Output")));
	output_title->set_theme_type_variation("HeaderSmall");
	right->add_child(output_title);

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	right->add_child(grid);

	grid->add_child(memnew(Label(TTR("Preset"))));
	preset_label = memnew(Label);
	preset_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	preset_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	preset_label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	grid->add_child(preset_label);

	grid->add_child(memnew(Label(TTR("File"))));
	HBoxContainer *path_row = memnew(HBoxContainer);
	path_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(path_row);
	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_text("res://renders/video.mp4");
	path_row->add_child(path_edit);
	browse_button = memnew(Button);
	browse_button->set_tooltip_text(TTR("Choose the output file."));
	browse_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoRenderDialog::_browse_pressed));
	path_row->add_child(browse_button);

	grid->add_child(memnew(Label(TTR("Range"))));
	range_option = memnew(OptionButton);
	range_option->add_item(TTR("Whole Project"), RANGE_PROJECT);
	range_option->add_item(TTR("Zone (In to Out)"), RANGE_ZONE);
	range_option->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoRenderDialog::_range_selected));
	grid->add_child(range_option);

	grid->add_child(memnew(Label(TTR("In / Out"))));
	HBoxContainer *zone_row = memnew(HBoxContainer);
	grid->add_child(zone_row);
	in_spin = memnew(SpinBox);
	in_spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	in_spin->set_tooltip_text(TTR("First frame of the zone."));
	zone_row->add_child(in_spin);
	out_spin = memnew(SpinBox);
	out_spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	out_spin->set_tooltip_text(TTR("Last frame of the zone."));
	zone_row->add_child(out_spin);

	grid->add_child(memnew(Label(TTR("Size"))));
	scale_option = memnew(OptionButton);
	scale_option->add_item(TTR("Full (100%)"), 1);
	scale_option->add_item(TTR("Half (50%)"), 2);
	scale_option->add_item(TTR("Quarter (25%)"), 4);
	grid->add_child(scale_option);

	right->add_spacer();
	right->add_child(memnew(HSeparator));

	HBoxContainer *progress_row = memnew(HBoxContainer);
	right->add_child(progress_row);
	progress_bar = memnew(ProgressBar);
	progress_bar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	progress_bar->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	progress_bar->set_min(0);
	progress_bar->set_max(100);
	progress_row->add_child(progress_bar);
	cancel_render_button = memnew(Button(TTR("Cancel")));
	cancel_render_button->set_tooltip_text(TTR("Stop the running render and delete its partial file."));
	cancel_render_button->set_disabled(true);
	cancel_render_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoRenderDialog::cancel_render));
	progress_row->add_child(cancel_render_button);

	status_label = memnew(Label);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_label->set_modulate(Color(1, 1, 1, 0.8));
	right->add_child(status_label);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	file_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->set_title(TTR("Render To"));
	file_dialog->connect("file_selected", callable_mp(this, &TetraVideoRenderDialog::_file_selected));
	add_child(file_dialog);
}
