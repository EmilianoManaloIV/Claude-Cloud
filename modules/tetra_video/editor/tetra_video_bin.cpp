#include "tetra_video_bin.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_project.h"
#include "tetra_video_thumbnails.h"
#include "tetra_video_timecode.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/text_edit.h"

const char *TetraVideoBin::DRAG_TYPE = "tetra_video_bin";

static const char *VIDEO_EXTENSIONS[] = { "mp4", "mov", "mkv", "webm", "avi", "ogv", "mpg", "mpeg", "m4v", "mts", "flv", "wmv", "mxf", nullptr };
static const char *AUDIO_EXTENSIONS[] = { "wav", "mp3", "ogg", "flac", "m4a", "aac", "opus", "wma", nullptr };
static const char *IMAGE_EXTENSIONS[] = { "png", "jpg", "jpeg", "webp", "bmp", "tga", "tif", "tiff", "svg", "exr", "gif", nullptr };

static String _filter_of(const char **p_extensions) {
	String filter;
	for (int i = 0; p_extensions[i]; i++) {
		filter += String(i ? ", *." : "*.") + p_extensions[i];
	}
	return filter;
}

static bool _has_extension(const char **p_extensions, const String &p_extension) {
	for (int i = 0; p_extensions[i]; i++) {
		if (p_extension == p_extensions[i]) {
			return true;
		}
	}
	return false;
}

bool TetraVideoBin::is_media_file(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return _has_extension(VIDEO_EXTENSIONS, extension) || _has_extension(AUDIO_EXTENSIONS, extension) || _has_extension(IMAGE_EXTENSIONS, extension);
}

Vector2i TetraVideoBin::_thumbnail_size() const {
	return Vector2i(Size2(64, 36) * EDSCALE);
}

int TetraVideoBin::_thumbnail_frame(const Dictionary &p_bin_clip) const {
	// One second in, where a recording has usually faded up from black.
	const int one_second = project.is_valid() ? TetraVideoTimecode::frames_per_second(project->get_fps()) : 25;
	return MIN(MAX(0, int(p_bin_clip["length"]) - 1), one_second);
}

void TetraVideoBin::_update_list() {
	const PackedInt32Array selected = get_selected_ids();
	list->clear();
	if (project.is_valid()) {
		const double fps = project->get_fps();
		const PackedInt32Array ids = project->get_bin_clip_ids();
		for (int i = 0; i < ids.size(); i++) {
			const Dictionary bin = project->get_bin_clip(ids[i]);
			const int item = list->add_item(String(bin["name"]) + "\n" + TetraVideoTimecode::from_frames(bin["length"], fps));
			list->set_item_metadata(item, ids[i]);
			const int used = project->get_bin_clip_use_count(ids[i]);
			String tooltip = String(bin["source"]) + "\n" + vformat(TTR("%d frames"), int(bin["length"]));
			if (bool(bin["has_video"]) && bool(bin["has_audio"])) {
				tooltip += ", " + TTR("video and audio");
			} else if (bool(bin["has_audio"])) {
				tooltip += ", " + TTR("audio only");
			}
			tooltip += "\n" + (used ? vformat(TTR("Used %d time(s) in the timeline"), used) : TTR("Not used in the timeline"));
			list->set_item_tooltip(item, tooltip);
			if (selected.has(ids[i])) {
				list->select(item, false);
			}
		}
	}
	_update_thumbnails();
	_update_buttons();
}

void TetraVideoBin::_update_thumbnails() {
	if (project.is_null() || !is_inside_tree()) {
		return;
	}
	for (int i = 0; i < list->get_item_count(); i++) {
		const int id = list->get_item_metadata(i);
		if (!project->has_bin_clip(id)) {
			continue;
		}
		const Dictionary bin = project->get_bin_clip(id);
		Ref<Texture2D> icon;
		if (bool(bin["has_video"]) && thumbnails.is_valid()) {
			icon = thumbnails->get_thumbnail(bin, _thumbnail_frame(bin), _thumbnail_size());
		}
		if (icon.is_null()) {
			icon = get_editor_theme_icon(bool(bin["has_video"]) ? SNAME("VideoStreamPlayer") : SNAME("AudioStreamPlayer"));
		}
		list->set_item_icon(i, icon);
	}
}

void TetraVideoBin::_update_buttons() {
	delete_button->set_disabled(list->get_selected_items().is_empty());
}

void TetraVideoBin::_update_icons() {
	add_clip_button->set_button_icon(get_editor_theme_icon(SNAME("Load")));
	add_color_button->set_button_icon(get_editor_theme_icon(SNAME("ColorRect")));
	add_title_button->set_button_icon(get_editor_theme_icon(SNAME("Label")));
	delete_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
	list->set_fixed_icon_size(_thumbnail_size());
	_update_thumbnails();
}

void TetraVideoBin::_item_activated(int p_index) {
	emit_signal(SNAME("clip_activated"), int(list->get_item_metadata(p_index)));
}

void TetraVideoBin::_selection_changed(int p_index, bool p_selected) {
	_update_buttons();
}

void TetraVideoBin::_files_selected(const PackedStringArray &p_paths) {
	emit_signal(SNAME("import_requested"), p_paths);
}

void TetraVideoBin::_color_confirmed() {
	if (project.is_null()) {
		return;
	}
	const int length = MAX(1, int(Math::round(color_seconds->get_value() * project->get_fps())));
	String name = color_name->get_text().strip_edges();
	if (name.is_empty()) {
		name = TTR("Color Clip");
	}
	const int id = project->add_bin_color(name, color_picker->get_pick_color(), length);
	if (id > 0) {
		select_clip(id);
	}
}

void TetraVideoBin::_title_confirmed() {
	if (project.is_null()) {
		return;
	}
	const String text = title_text->get_text();
	const int length = MAX(1, int(Math::round(title_seconds->get_value() * project->get_fps())));
	String name = text.strip_edges().get_slice("\n", 0).left(24);
	if (name.is_empty()) {
		name = TTR("Title Clip");
	}
	const int id = project->add_bin_text(name, text, length);
	if (id > 0) {
		select_clip(id);
	}
}

void TetraVideoBin::_delete_confirmed() {
	if (project.is_null()) {
		return;
	}
	const PackedInt32Array ids = get_selected_ids();
	project->begin_undo_group(TTR("Delete Bin Clips"));
	for (int i = 0; i < ids.size(); i++) {
		project->remove_bin_clip(ids[i], true);
	}
	project->end_undo_group();
}

Variant TetraVideoBin::_get_drag_data_fw(const Point2 &p_point) {
	const int item = list->get_item_at_position(p_point, true);
	if (item < 0 || project.is_null()) {
		return Variant();
	}
	PackedInt32Array ids = get_selected_ids();
	const int id = list->get_item_metadata(item);
	if (!ids.has(id)) {
		ids.clear();
		ids.push_back(id);
	}
	Dictionary payload;
	payload["type"] = DRAG_TYPE;
	payload["bin_ids"] = ids;

	Label *preview = memnew(Label);
	preview->set_text(ids.size() == 1 ? String(project->get_bin_clip(id)["name"]) : vformat(TTR("%d clips"), ids.size()));
	list->set_drag_preview(preview);
	return payload;
}

bool TetraVideoBin::_can_drop_data_fw(const Point2 &p_point, const Variant &p_data) const {
	if (p_data.get_type() != Variant::DICTIONARY) {
		return false;
	}
	const Dictionary payload = p_data;
	const String type = payload.get("type", String());
	if (type != "files" && type != "files_and_dirs") {
		return false;
	}
	const PackedStringArray files = payload.get("files", PackedStringArray());
	for (const String &file : files) {
		if (is_media_file(file)) {
			return true;
		}
	}
	return false;
}

void TetraVideoBin::_drop_data_fw(const Point2 &p_point, const Variant &p_data) {
	if (!_can_drop_data_fw(p_point, p_data)) {
		return;
	}
	const Dictionary payload = p_data;
	const PackedStringArray files = payload.get("files", PackedStringArray());
	PackedStringArray media;
	for (const String &file : files) {
		if (is_media_file(file)) {
			media.push_back(file);
		}
	}
	emit_signal(SNAME("import_requested"), media);
}

void TetraVideoBin::set_project(const Ref<TetraVideoProject> &p_project) {
	if (project.is_valid()) {
		project->disconnect("structure_changed", callable_mp(this, &TetraVideoBin::refresh));
	}
	project = p_project;
	if (project.is_valid()) {
		project->connect("structure_changed", callable_mp(this, &TetraVideoBin::refresh));
	}
	_update_list();
}

void TetraVideoBin::set_thumbnails(const Ref<TetraVideoThumbnails> &p_thumbnails) {
	if (thumbnails.is_valid()) {
		thumbnails->disconnect("thumbnail_ready", callable_mp(this, &TetraVideoBin::_update_thumbnails));
	}
	thumbnails = p_thumbnails;
	if (thumbnails.is_valid()) {
		thumbnails->connect("thumbnail_ready", callable_mp(this, &TetraVideoBin::_update_thumbnails));
	}
	_update_thumbnails();
}

void TetraVideoBin::refresh() {
	_update_list();
}

PackedInt32Array TetraVideoBin::get_selected_ids() const {
	PackedInt32Array ids;
	const Vector<int> items = list->get_selected_items();
	for (int item : items) {
		ids.push_back(int(list->get_item_metadata(item)));
	}
	return ids;
}

void TetraVideoBin::select_clip(int p_bin_id) {
	list->deselect_all();
	for (int i = 0; i < list->get_item_count(); i++) {
		if (int(list->get_item_metadata(i)) == p_bin_id) {
			list->select(i);
			list->ensure_current_is_visible();
			break;
		}
	}
	_update_buttons();
}

void TetraVideoBin::popup_add_clip() {
	file_dialog->popup_file_dialog();
}

void TetraVideoBin::popup_add_color() {
	color_name->set_text(TTR("Color Clip"));
	color_dialog->popup_centered(Size2(320, 0) * EDSCALE);
	color_name->grab_focus();
}

void TetraVideoBin::popup_add_title() {
	title_dialog->popup_centered(Size2(360, 240) * EDSCALE);
	title_text->grab_focus();
	title_text->select_all();
}

void TetraVideoBin::delete_selected() {
	if (project.is_null()) {
		return;
	}
	const PackedInt32Array ids = get_selected_ids();
	if (ids.is_empty()) {
		return;
	}
	int uses = 0;
	for (int i = 0; i < ids.size(); i++) {
		uses += project->get_bin_clip_use_count(ids[i]);
	}
	if (uses > 0) {
		delete_dialog->set_text(vformat(TTR("The selected bin clips are used %d time(s) in the timeline.\nDelete them together with their timeline clips?"), uses));
		delete_dialog->popup_centered();
	} else {
		_delete_confirmed();
	}
}

void TetraVideoBin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			_update_icons();
		} break;
	}
}

void TetraVideoBin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("refresh"), &TetraVideoBin::refresh);
	ClassDB::bind_method(D_METHOD("get_selected_ids"), &TetraVideoBin::get_selected_ids);
	ClassDB::bind_method(D_METHOD("select_clip", "bin_id"), &TetraVideoBin::select_clip);
	ClassDB::bind_method(D_METHOD("popup_add_clip"), &TetraVideoBin::popup_add_clip);
	ClassDB::bind_method(D_METHOD("popup_add_color"), &TetraVideoBin::popup_add_color);
	ClassDB::bind_method(D_METHOD("popup_add_title"), &TetraVideoBin::popup_add_title);
	ClassDB::bind_method(D_METHOD("delete_selected"), &TetraVideoBin::delete_selected);
	ClassDB::bind_static_method("TetraVideoBin", D_METHOD("is_media_file", "path"), &TetraVideoBin::is_media_file);

	ADD_SIGNAL(MethodInfo("clip_activated", PropertyInfo(Variant::INT, "bin_id")));
	ADD_SIGNAL(MethodInfo("import_requested", PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths")));
}

TetraVideoBin::TetraVideoBin() {
	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	add_clip_button = memnew(Button(TTR("Add Clip...")));
	add_clip_button->set_tooltip_text(TTR("Add video, audio or image files to the bin."));
	add_clip_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoBin::popup_add_clip));
	toolbar->add_child(add_clip_button);

	add_color_button = memnew(Button);
	add_color_button->set_theme_type_variation(SceneStringName(FlatButton));
	add_color_button->set_tooltip_text(TTR("Add Color Clip..."));
	add_color_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoBin::popup_add_color));
	toolbar->add_child(add_color_button);

	add_title_button = memnew(Button);
	add_title_button->set_theme_type_variation(SceneStringName(FlatButton));
	add_title_button->set_tooltip_text(TTR("Add Title Clip..."));
	add_title_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoBin::popup_add_title));
	// Titles are drawn by MLT's pango or qtext producer; without either there is nothing to offer.
	add_title_button->set_visible(!TetraMltEngine::get_preferred_service("producer", "pango,qtext").is_empty());
	toolbar->add_child(add_title_button);

	toolbar->add_spacer();

	delete_button = memnew(Button);
	delete_button->set_theme_type_variation(SceneStringName(FlatButton));
	delete_button->set_tooltip_text(TTR("Delete the selected clips from the bin."));
	delete_button->set_disabled(true);
	delete_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoBin::delete_selected));
	toolbar->add_child(delete_button);

	list = memnew(ItemList);
	list->set_v_size_flags(SIZE_EXPAND_FILL);
	list->set_select_mode(ItemList::SELECT_MULTI);
	list->set_icon_mode(ItemList::ICON_MODE_LEFT);
	list->set_max_text_lines(2);
	list->set_custom_minimum_size(Size2(150, 60) * EDSCALE);
	list->connect("item_activated", callable_mp(this, &TetraVideoBin::_item_activated));
	list->connect("multi_selected", callable_mp(this, &TetraVideoBin::_selection_changed));
	list->set_drag_forwarding(callable_mp(this, &TetraVideoBin::_get_drag_data_fw), callable_mp(this, &TetraVideoBin::_can_drop_data_fw), callable_mp(this, &TetraVideoBin::_drop_data_fw));
	add_child(list);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	file_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILES);
	file_dialog->set_title(TTR("Add Clip"));
	file_dialog->add_filter(_filter_of(VIDEO_EXTENSIONS) + ", " + _filter_of(AUDIO_EXTENSIONS) + ", " + _filter_of(IMAGE_EXTENSIONS), TTR("All Media"));
	file_dialog->add_filter(_filter_of(VIDEO_EXTENSIONS), TTR("Video"));
	file_dialog->add_filter(_filter_of(AUDIO_EXTENSIONS), TTR("Audio"));
	file_dialog->add_filter(_filter_of(IMAGE_EXTENSIONS), TTR("Images"));
	file_dialog->connect("files_selected", callable_mp(this, &TetraVideoBin::_files_selected));
	add_child(file_dialog);

	color_dialog = memnew(ConfirmationDialog);
	color_dialog->set_title(TTR("Add Color Clip"));
	color_dialog->set_ok_button_text(TTR("Add"));
	color_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoBin::_color_confirmed));
	add_child(color_dialog);
	{
		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(2);
		color_dialog->add_child(grid);
		grid->add_child(memnew(Label(TTR("Name"))));
		color_name = memnew(LineEdit);
		color_name->set_h_size_flags(SIZE_EXPAND_FILL);
		grid->add_child(color_name);
		color_dialog->register_text_enter(color_name);
		grid->add_child(memnew(Label(TTR("Color"))));
		color_picker = memnew(ColorPickerButton);
		color_picker->set_pick_color(Color(0.16, 0.36, 0.72));
		color_picker->set_edit_alpha(false);
		color_picker->set_custom_minimum_size(Size2(0, 28) * EDSCALE);
		grid->add_child(color_picker);
		grid->add_child(memnew(Label(TTR("Duration"))));
		color_seconds = memnew(SpinBox);
		color_seconds->set_min(0.04);
		color_seconds->set_max(3600);
		color_seconds->set_step(0.01);
		color_seconds->set_value(5);
		color_seconds->set_suffix("s");
		grid->add_child(color_seconds);
	}

	title_dialog = memnew(ConfirmationDialog);
	title_dialog->set_title(TTR("Add Title Clip"));
	title_dialog->set_ok_button_text(TTR("Add"));
	title_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoBin::_title_confirmed));
	add_child(title_dialog);
	{
		VBoxContainer *box = memnew(VBoxContainer);
		title_dialog->add_child(box);
		box->add_child(memnew(Label(TTR("Text"))));
		title_text = memnew(TextEdit);
		title_text->set_text(TTR("Title"));
		title_text->set_v_size_flags(SIZE_EXPAND_FILL);
		title_text->set_custom_minimum_size(Size2(0, 90) * EDSCALE);
		box->add_child(title_text);
		HBoxContainer *row = memnew(HBoxContainer);
		box->add_child(row);
		row->add_child(memnew(Label(TTR("Duration"))));
		title_seconds = memnew(SpinBox);
		title_seconds->set_min(0.04);
		title_seconds->set_max(3600);
		title_seconds->set_step(0.01);
		title_seconds->set_value(5);
		title_seconds->set_suffix("s");
		title_seconds->set_h_size_flags(SIZE_EXPAND_FILL);
		row->add_child(title_seconds);
	}

	delete_dialog = memnew(ConfirmationDialog);
	delete_dialog->set_title(TTR("Delete Bin Clips"));
	delete_dialog->set_ok_button_text(TTR("Delete"));
	delete_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraVideoBin::_delete_confirmed));
	add_child(delete_dialog);
}
