#pragma once

#include "scene/gui/box_container.h"

class Button;
class ColorPickerButton;
class ConfirmationDialog;
class EditorFileDialog;
class ItemList;
class LineEdit;
class SpinBox;
class TetraVideoProject;
class TetraVideoThumbnails;
class TextEdit;

// The Project Bin: every clip the project can put on the timeline, with a
// thumbnail, its name and its duration. Clips are dragged from here onto the
// timeline; files dragged from the FileSystem dock are added to the bin.
class TetraVideoBin : public VBoxContainer {
	GDCLASS(TetraVideoBin, VBoxContainer);

	Ref<TetraVideoProject> project;
	Ref<TetraVideoThumbnails> thumbnails;

	Button *add_clip_button = nullptr;
	Button *add_color_button = nullptr;
	Button *add_title_button = nullptr;
	Button *delete_button = nullptr;
	ItemList *list = nullptr;

	EditorFileDialog *file_dialog = nullptr;
	ConfirmationDialog *color_dialog = nullptr;
	LineEdit *color_name = nullptr;
	ColorPickerButton *color_picker = nullptr;
	SpinBox *color_seconds = nullptr;
	ConfirmationDialog *title_dialog = nullptr;
	TextEdit *title_text = nullptr;
	SpinBox *title_seconds = nullptr;
	ConfirmationDialog *delete_dialog = nullptr;

	Vector2i _thumbnail_size() const;
	int _thumbnail_frame(const Dictionary &p_bin_clip) const;
	void _update_list();
	void _update_thumbnails();
	void _update_buttons();
	void _update_icons();
	void _item_activated(int p_index);
	void _selection_changed(int p_index, bool p_selected);
	void _files_selected(const PackedStringArray &p_paths);
	void _color_confirmed();
	void _title_confirmed();
	void _delete_confirmed();

	Variant _get_drag_data_fw(const Point2 &p_point);
	bool _can_drop_data_fw(const Point2 &p_point, const Variant &p_data) const;
	void _drop_data_fw(const Point2 &p_point, const Variant &p_data);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The "type" of the drag data of bin clips: { type, bin_ids: PackedInt32Array }.
	static const char *DRAG_TYPE;
	// File extensions the bin offers in its file dialog, by kind.
	static bool is_media_file(const String &p_path);

	void set_project(const Ref<TetraVideoProject> &p_project);
	void set_thumbnails(const Ref<TetraVideoThumbnails> &p_thumbnails);
	void refresh();

	PackedInt32Array get_selected_ids() const;
	void select_clip(int p_bin_id);
	void popup_add_clip();
	void popup_add_color();
	void popup_add_title();
	// Removes the selected clips; asks first when the timeline uses them.
	void delete_selected();

	TetraVideoBin();
};
