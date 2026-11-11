#pragma once

#include "tetra_image_tools.h"

#include "../tetra_image_document.h"

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"

class AcceptDialog;
class Button;
class ButtonGroup;
class CheckBox;
class ColorPicker;
class ConfirmationDialog;
class EditorFileDialog;
class GridContainer;
class Label;
class MenuBar;
class OptionButton;
class PopupMenu;
class PopupPanel;
class Shortcut;
class SpinBox;
class TetraImageCanvas;
class TetraImageColorSwatch;
class TetraImageLayersPanel;
class TetraOperationBrowser;
class TetraOperationDialog;
class TetraParamPanel;

// The root of the Image workspace: menu bar, toolbox, canvas, Tool Options,
// Layers and status bar around one TetraImageDocument.
//
// It is also the scripting surface of the workspace. Everything a menu or a
// tool does is reachable through the bound methods (new_document(),
// open_file(), set_tool(), apply_operation(), save(), menu_option(), ...), so
// screenshot drivers and users' tool scripts can drive it:
//
//   var workspace := TetraImageWorkspace.get_singleton()
//   var doc := workspace.new_document(1024, 768)
//   workspace.set_tool("paintbrush")
//   workspace.get_canvas().tool_press(Vector2(100, 100))
//
// Filters run on a worker thread from a snapshot of the active layer. While
// the operation dialog is open the newest result is shown in the document
// inside an open undo group, which OK closes (one undo step) and Cancel
// takes back.
class TetraImageWorkspace : public VBoxContainer {
	GDCLASS(TetraImageWorkspace, VBoxContainer);

public:
	enum MenuOption {
		FILE_NEW,
		FILE_OPEN,
		FILE_SAVE,
		FILE_SAVE_AS,
		FILE_EXPORT_AS,
		FILE_CLOSE,
		EDIT_UNDO,
		EDIT_REDO,
		EDIT_CLEAR,
		EDIT_FILL_FOREGROUND,
		EDIT_FILL_BACKGROUND,
		SELECT_ALL,
		SELECT_NONE,
		SELECT_INVERT,
		SELECT_FEATHER,
		SELECT_GROW,
		SELECT_SHRINK,
		IMAGE_FLIP_HORIZONTAL,
		IMAGE_FLIP_VERTICAL,
		IMAGE_ROTATE_90_CW,
		IMAGE_ROTATE_90_CCW,
		IMAGE_ROTATE_180,
		IMAGE_SCALE,
		IMAGE_CANVAS_SIZE,
		IMAGE_CROP_TO_SELECTION,
		IMAGE_FLATTEN,
		LAYER_NEW,
		LAYER_DUPLICATE,
		LAYER_RAISE,
		LAYER_LOWER,
		LAYER_MERGE_DOWN,
		LAYER_DELETE,
		LAYER_FLIP_HORIZONTAL,
		LAYER_FLIP_VERTICAL,
		FILTER_REPEAT_LAST,
		FILTER_SEARCH,
		VIEW_ZOOM_IN,
		VIEW_ZOOM_OUT,
		VIEW_ZOOM_100,
		VIEW_ZOOM_FIT,
	};

private:
	enum FilePurpose {
		FILE_PURPOSE_OPEN,
		FILE_PURPOSE_SAVE_AS,
		FILE_PURPOSE_EXPORT,
	};

	enum SizeDialogMode {
		SIZE_DIALOG_SCALE,
		SIZE_DIALOG_CANVAS,
	};

	// One run of an operation on a worker thread.
	struct FilterRequest {
		uint64_t serial = 0;
		String operation_id;
		Dictionary values;
		Ref<Image> source;
		bool commit = false;
	};

	static TetraImageWorkspace *singleton;

	Ref<TetraImageDocument> document;
	String document_path;
	bool modified = false;

	MenuBar *menu_bar = nullptr;
	PopupMenu *file_menu = nullptr;
	PopupMenu *edit_menu = nullptr;
	PopupMenu *select_menu = nullptr;
	PopupMenu *view_menu = nullptr;
	PopupMenu *image_menu = nullptr;
	PopupMenu *layer_menu = nullptr;
	PopupMenu *colors_menu = nullptr;
	PopupMenu *filters_menu = nullptr;
	Label *title_label = nullptr;

	Button *tool_buttons[TetraImageTools::TOOL_MAX] = {};
	Ref<ButtonGroup> tool_group;
	TetraImageColorSwatch *swatch = nullptr;
	Control *welcome = nullptr;
	TetraImageCanvas *canvas = nullptr;
	Control *busy_shield = nullptr;
	Label *busy_label = nullptr;
	Label *tool_options_title = nullptr;
	TetraParamPanel *tool_options_panel = nullptr;
	TetraImageLayersPanel *layers_panel = nullptr;
	Label *status_hint = nullptr;
	Label *status_pointer = nullptr;
	Label *status_size = nullptr;
	Button *status_zoom = nullptr;

	TetraImageTools::Tool tool = TetraImageTools::TOOL_PAINTBRUSH;
	Dictionary tool_schemas[TetraImageTools::TOOL_MAX];
	Dictionary tool_values[TetraImageTools::TOOL_MAX];
	Ref<Shortcut> tool_shortcuts[TetraImageTools::TOOL_MAX];
	Ref<Shortcut> swap_shortcut;
	Ref<Shortcut> default_colors_shortcut;
	Ref<Shortcut> smaller_brush_shortcut;
	Ref<Shortcut> larger_brush_shortcut;
	Color foreground = Color(0, 0, 0, 1);
	Color background = Color(1, 1, 1, 1);
	String tool_hint_override;

	PopupPanel *color_popup = nullptr;
	ColorPicker *color_picker = nullptr;
	bool color_popup_for_background = false;

	ConfirmationDialog *new_dialog = nullptr;
	SpinBox *new_width = nullptr;
	SpinBox *new_height = nullptr;
	OptionButton *new_fill = nullptr;

	EditorFileDialog *file_dialog = nullptr;
	FilePurpose file_purpose = FILE_PURPOSE_OPEN;

	ConfirmationDialog *amount_dialog = nullptr;
	Label *amount_label = nullptr;
	SpinBox *amount_value = nullptr;
	MenuOption amount_option = SELECT_FEATHER;

	ConfirmationDialog *size_dialog = nullptr;
	SizeDialogMode size_dialog_mode = SIZE_DIALOG_SCALE;
	SpinBox *size_width = nullptr;
	SpinBox *size_height = nullptr;
	CheckBox *size_keep_aspect = nullptr;
	Label *size_interpolation_label = nullptr;
	OptionButton *size_interpolation = nullptr;
	Label *size_offset_x_label = nullptr;
	SpinBox *size_offset_x = nullptr;
	Label *size_offset_y_label = nullptr;
	SpinBox *size_offset_y = nullptr;
	Button *size_center = nullptr;
	bool size_dialog_updating = false;

	ConfirmationDialog *discard_dialog = nullptr;
	Callable discard_action;
	ConfirmationDialog *save_choice_dialog = nullptr;

	AcceptDialog *search_dialog = nullptr;
	TetraOperationBrowser *operation_browser = nullptr;
	TetraOperationDialog *operation_dialog = nullptr;

	Array operations;
	HashMap<String, Dictionary> operations_by_id;
	bool operations_loaded = false;
	bool operations_task_running = false;
	WorkerThreadPool::TaskID operations_task = WorkerThreadPool::INVALID_TASK_ID;

	bool filter_session = false;
	Ref<TetraImageDocument> filter_document;
	int filter_layer = -1;
	String filter_id;
	String filter_label;
	Ref<Image> filter_source;
	bool filter_preview_shown = false;
	Dictionary filter_shown_values;
	uint64_t filter_serial = 0;
	bool filter_task_running = false;
	WorkerThreadPool::TaskID filter_task = WorkerThreadPool::INVALID_TASK_ID;
	FilterRequest *filter_waiting = nullptr;
	bool busy = false;
	String last_filter_id;
	Dictionary last_filter_values;
	HashMap<String, Dictionary> remembered_values;

	// tetra_image_workspace.cpp: layout, document, tools.
	void _build_layout();
	void _set_document(const Ref<TetraImageDocument> &p_document, const String &p_path);
	void _document_edited();
	void _update_title();
	void _update_status();
	void _update_tool_options();
	void _tool_button_pressed(int p_tool);
	void _tool_option_changed(const String &p_name, const Variant &p_value);
	void _change_brush_size(bool p_larger);
	void _colors_changed();
	void _popup_color_picker(bool p_background);
	void _color_picker_changed(const Color &p_color);
	void _color_picked(const Color &p_color, bool p_background);
	void _pointer_moved(const Vector2 &p_position);
	void _pointer_exited();
	void _status_message(const String &p_text);
	bool _has_focus_inside() const;
	void _set_busy(bool p_busy, const String &p_text = String());

	// tetra_image_workspace_menus.cpp: menus, dialogs, files, filters.
	void _build_menus();
	void _build_dialogs();
	void _update_menus();
	void _menu_option(int p_option);
	void _confirm_discard(const Callable &p_then);
	void _discard_confirmed();
	void _popup_new_dialog();
	void _new_dialog_confirmed();
	void _popup_file_dialog(FilePurpose p_purpose);
	void _file_selected(const String &p_path);
	void _open_checked(const String &p_path);
	void _save_requested();
	void _save_choice_action(const String &p_action);
	void _save_choice_confirmed();
	Error _write_file(const String &p_path, bool p_becomes_document);
	void _file_written(const String &p_path);
	void _popup_amount_dialog(MenuOption p_option);
	void _amount_confirmed();
	void _popup_size_dialog(SizeDialogMode p_mode);
	void _size_value_changed(double p_value, bool p_width);
	void _size_center_pressed();
	void _size_confirmed();
	void _close_confirmed();

	static void _operations_thread(void *p_userdata);
	void _load_operations();
	void _operations_loaded(const Array &p_operations);
	void _add_operation_items(PopupMenu *p_menu, const Dictionary &p_node);
	void _operation_menu_pressed(int p_index, PopupMenu *p_menu);
	void _popup_search();
	void _search_confirmed();
	void _search_activated(const String &p_id);
	void _open_operation(const String &p_id, const Dictionary &p_values, bool p_use_given_values);
	bool _begin_filter(const String &p_id);
	void _request_filter(const Dictionary &p_values, bool p_commit);
	void _start_filter_task(FilterRequest *p_request);
	void _filter_thread(FilterRequest *p_request);
	void _filter_finished(uint64_t p_serial, const Ref<Image> &p_result, bool p_commit);
	void _drop_preview();
	void _end_filter();
	void _dialog_values_changed(const Dictionary &p_values);
	void _dialog_preview_toggled(bool p_checked);
	void _dialog_confirmed(const String &p_id, const Dictionary &p_values);
	void _dialog_cancelled(const String &p_id);
	void _remember_filter(const String &p_id, const Dictionary &p_values);

protected:
	void _notification(int p_what);
	static void _bind_methods();

	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

public:
	static TetraImageWorkspace *get_singleton() { return singleton; }

	// The file an image resource was loaded from, when the workspace can edit
	// it (png, jpg, jpeg, webp, exr, tga, bmp, ora); empty otherwise.
	static String get_editable_path(Object *p_object);

	// p_fill is a display (sRGB) colour; alpha 0 gives a transparent image.
	// Replaces the open document without asking.
	Ref<TetraImageDocument> new_document(int p_width, int p_height, const Color &p_fill = Color(1, 1, 1, 1));
	Error open_file(const String &p_path);
	// Like open_file(), but asks first when the open document has unsaved
	// changes, and does nothing when that file is already open.
	void request_open_file(const String &p_path);
	// Saves to the document's path: OpenRaster for .ora, otherwise a flattened export.
	Error save();
	Error save_as(const String &p_path);
	Error export_as(const String &p_path);
	void close_document();

	Ref<TetraImageDocument> get_document() const { return document; }
	bool has_document() const { return document.is_valid(); }
	String get_document_path() const { return document_path; }
	bool is_modified() const { return modified; }
	// True when save() can run without asking the user anything.
	bool can_save_directly() const;

	bool set_tool(const String &p_name);
	String get_tool() const;
	PackedStringArray get_tool_names() const;
	Dictionary get_tool_options_schema(const String &p_tool) const;
	Dictionary get_tool_options(const String &p_tool) const;
	void set_tool_option(const String &p_tool, const String &p_name, const Variant &p_value);

	void set_foreground_color(const Color &p_color);
	Color get_foreground_color() const { return foreground; }
	void set_background_color(const Color &p_color);
	Color get_background_color() const { return background; }
	void swap_colors();
	void reset_colors();

	Array get_operations() const { return operations; }
	bool are_operations_loaded() const { return operations_loaded; }
	// Applies an operation to the active layer at once (one undo step).
	Error apply_operation(const String &p_id, const Dictionary &p_values = Dictionary());
	// Opens the operation dialog with live preview; operations without parameters are applied.
	void show_operation_dialog(const String &p_id, const Dictionary &p_values = Dictionary());
	void repeat_last_operation();
	// True while a filter is being computed for a preview or to be applied.
	bool is_filter_running() const { return filter_task_running; }
	bool is_preview_shown() const { return filter_preview_shown; }

	bool undo();
	bool redo();
	void menu_option(MenuOption p_option);

	void zoom_to_fit();
	void set_zoom(real_t p_zoom);
	real_t get_zoom() const;

	TetraImageCanvas *get_canvas() const { return canvas; }
	TetraImageLayersPanel *get_layers_panel() const { return layers_panel; }
	TetraParamPanel *get_tool_options_panel() const { return tool_options_panel; }
	TetraOperationDialog *get_operation_dialog() const { return operation_dialog; }
	MenuBar *get_menu_bar() const { return menu_bar; }
	// "File", "Edit", "Select", "View", "Image", "Layer", "Colors" or "Filters".
	PopupMenu *get_menu(const String &p_name) const;
	String get_status_text() const;
	String get_title_text() const;

	TetraImageWorkspace();
	~TetraImageWorkspace();
};

VARIANT_ENUM_CAST(TetraImageWorkspace::MenuOption);
