#include "tetra_image_workspace.h"

#include "tetra_image_canvas.h"
#include "tetra_image_layers_panel.h"

#include "tetra/editor/tetra_operation_browser.h"
#include "tetra/editor/tetra_operation_dialog.h"
#include "tetra/tetra_schema.h"

#include "core/config/project_settings.h"
#include "core/input/shortcut.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/templates/hash_set.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_bar.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/texture.h"

static const char *OPEN_EXTENSIONS[] = { "png", "jpg", "jpeg", "webp", "exr", "tga", "bmp", "ora" };
static const char *FLAT_EXTENSIONS[] = { "png", "jpg", "jpeg", "webp", "exr" };

static bool is_open_extension(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	for (const char *known : OPEN_EXTENSIONS) {
		if (extension == known) {
			return true;
		}
	}
	return false;
}

static bool is_flat_extension(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	for (const char *known : FLAT_EXTENSIONS) {
		if (extension == known) {
			return true;
		}
	}
	return false;
}

static void set_option_disabled(PopupMenu *p_menu, int p_option, bool p_disabled) {
	p_menu->set_item_disabled(p_menu->get_item_index(p_option), p_disabled);
}

static SpinBox *make_pixel_spin(double p_min, double p_max, double p_value) {
	SpinBox *spin = memnew(SpinBox);
	spin->set_min(p_min);
	spin->set_max(p_max);
	spin->set_step(1.0);
	spin->set_value(p_value);
	spin->set_suffix("px");
	spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	spin->set_custom_minimum_size(Size2(120, 0) * EDSCALE);
	return spin;
}

String TetraImageWorkspace::get_editable_path(Object *p_object) {
	const Resource *resource = Object::cast_to<Resource>(p_object);
	if (!resource || !(Object::cast_to<Texture2D>(p_object) || Object::cast_to<Image>(p_object))) {
		return String();
	}
	const String path = resource->get_path();
	// A resource built into a scene has no image file of its own.
	if (path.is_empty() || path.contains("::") || !is_open_extension(path)) {
		return String();
	}
	return path;
}

void TetraImageWorkspace::_build_menus() {
	PopupMenu **const targets[] = { &file_menu, &edit_menu, &select_menu, &view_menu, &image_menu, &layer_menu, &colors_menu, &filters_menu };
	const char *const names[] = { "File", "Edit", "Select", "View", "Image", "Layer", "Colors", "Filters" };
	const String titles[] = { TTR("File"), TTR("Edit"), TTR("Select"), TTR("View"), TTR("Image"), TTR("Layer"), TTR("Colors"), TTR("Filters") };
	for (int i = 0; i < 8; i++) {
		PopupMenu *menu = memnew(PopupMenu);
		menu->set_name(names[i]);
		menu_bar->add_child(menu);
		menu_bar->set_menu_title(i, titles[i]);
		menu->connect("index_pressed", callable_mp(this, &TetraImageWorkspace::_operation_menu_pressed).bind(menu));
		*targets[i] = menu;
	}

	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/new", TTRC("New..."), KeyModifierMask::CMD_OR_CTRL | Key::N), FILE_NEW);
	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/open", TTRC("Open..."), KeyModifierMask::CMD_OR_CTRL | Key::O), FILE_OPEN);
	file_menu->add_separator();
	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/save", TTRC("Save"), KeyModifierMask::CMD_OR_CTRL | Key::S), FILE_SAVE);
	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/save_as", TTRC("Save As..."), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::S), FILE_SAVE_AS);
	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/export_as", TTRC("Export As..."), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::E), FILE_EXPORT_AS);
	file_menu->add_separator();
	file_menu->add_shortcut(ED_SHORTCUT("tetra_image/close", TTRC("Close"), KeyModifierMask::CMD_OR_CTRL | Key::W), FILE_CLOSE);

	edit_menu->add_shortcut(ED_SHORTCUT("tetra_image/undo", TTRC("Undo"), KeyModifierMask::CMD_OR_CTRL | Key::Z), EDIT_UNDO);
	edit_menu->add_shortcut(ED_SHORTCUT_ARRAY("tetra_image/redo", TTRC("Redo"), { int32_t(KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::Z), int32_t(KeyModifierMask::CMD_OR_CTRL | Key::Y) }), EDIT_REDO);
	edit_menu->add_separator();
	edit_menu->add_shortcut(ED_SHORTCUT("tetra_image/clear", TTRC("Clear"), Key::KEY_DELETE), EDIT_CLEAR);
	edit_menu->add_shortcut(ED_SHORTCUT("tetra_image/fill_foreground", TTRC("Fill with FG Color"), KeyModifierMask::CMD_OR_CTRL | Key::COMMA), EDIT_FILL_FOREGROUND);
	edit_menu->add_shortcut(ED_SHORTCUT("tetra_image/fill_background", TTRC("Fill with BG Color"), KeyModifierMask::CMD_OR_CTRL | Key::PERIOD), EDIT_FILL_BACKGROUND);

	select_menu->add_shortcut(ED_SHORTCUT("tetra_image/select_all", TTRC("All"), KeyModifierMask::CMD_OR_CTRL | Key::A), SELECT_ALL);
	select_menu->add_shortcut(ED_SHORTCUT("tetra_image/select_none", TTRC("None"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::A), SELECT_NONE);
	select_menu->add_shortcut(ED_SHORTCUT("tetra_image/select_invert", TTRC("Invert"), KeyModifierMask::CMD_OR_CTRL | Key::I), SELECT_INVERT);
	select_menu->add_separator();
	select_menu->add_item(TTR("Feather..."), SELECT_FEATHER);
	select_menu->add_item(TTR("Grow..."), SELECT_GROW);
	select_menu->add_item(TTR("Shrink..."), SELECT_SHRINK);

	view_menu->add_shortcut(ED_SHORTCUT_ARRAY("tetra_image/zoom_in", TTRC("Zoom In"), { int32_t(Key::EQUAL), int32_t(Key::PLUS), int32_t(Key::KP_ADD) }), VIEW_ZOOM_IN);
	view_menu->add_shortcut(ED_SHORTCUT_ARRAY("tetra_image/zoom_out", TTRC("Zoom Out"), { int32_t(Key::MINUS), int32_t(Key::KP_SUBTRACT) }), VIEW_ZOOM_OUT);
	view_menu->add_shortcut(ED_SHORTCUT("tetra_image/zoom_100", TTRC("Zoom 100%"), Key::KEY_1), VIEW_ZOOM_100);
	view_menu->add_shortcut(ED_SHORTCUT("tetra_image/zoom_fit", TTRC("Fit Image in Window"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::J), VIEW_ZOOM_FIT);

	image_menu->add_item(TTR("Flip Horizontally"), IMAGE_FLIP_HORIZONTAL);
	image_menu->add_item(TTR("Flip Vertically"), IMAGE_FLIP_VERTICAL);
	image_menu->add_separator();
	image_menu->add_item(TTR(U"Rotate 90° Clockwise"), IMAGE_ROTATE_90_CW);
	image_menu->add_item(TTR(U"Rotate 90° Counter-Clockwise"), IMAGE_ROTATE_90_CCW);
	image_menu->add_item(TTR(U"Rotate 180°"), IMAGE_ROTATE_180);
	image_menu->add_separator();
	image_menu->add_item(TTR("Scale Image..."), IMAGE_SCALE);
	image_menu->add_item(TTR("Canvas Size..."), IMAGE_CANVAS_SIZE);
	image_menu->add_item(TTR("Crop to Selection"), IMAGE_CROP_TO_SELECTION);
	image_menu->add_separator();
	image_menu->add_item(TTR("Flatten Image"), IMAGE_FLATTEN);

	layer_menu->add_shortcut(ED_SHORTCUT("tetra_image/layer_new", TTRC("New Layer"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::N), LAYER_NEW);
	layer_menu->add_shortcut(ED_SHORTCUT("tetra_image/layer_duplicate", TTRC("Duplicate Layer"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::D), LAYER_DUPLICATE);
	layer_menu->add_separator();
	layer_menu->add_item(TTR("Raise Layer"), LAYER_RAISE);
	layer_menu->add_item(TTR("Lower Layer"), LAYER_LOWER);
	layer_menu->add_item(TTR("Merge Down"), LAYER_MERGE_DOWN);
	layer_menu->add_item(TTR("Delete Layer"), LAYER_DELETE);
	layer_menu->add_separator();
	layer_menu->add_item(TTR("Flip Horizontally"), LAYER_FLIP_HORIZONTAL);
	layer_menu->add_item(TTR("Flip Vertically"), LAYER_FLIP_VERTICAL);

	ED_SHORTCUT("tetra_image/repeat_filter", TTRC("Repeat Last Filter"), KeyModifierMask::CMD_OR_CTRL | Key::F);
	ED_SHORTCUT("tetra_image/search_filters", TTRC("Search Filters..."), Key::SLASH);
	_rebuild_operation_menus();
}

void TetraImageWorkspace::_rebuild_operation_menus() {
	colors_menu->clear();
	filters_menu->clear();
	filters_menu->add_shortcut(ED_GET_SHORTCUT("tetra_image/repeat_filter"), FILTER_REPEAT_LAST);
	filters_menu->add_shortcut(ED_GET_SHORTCUT("tetra_image/search_filters"), FILTER_SEARCH);
	filters_menu->add_separator();
	if (!operations_loaded) {
		colors_menu->add_item(TTR("Loading the operations..."));
		colors_menu->set_item_disabled(0, true);
		filters_menu->add_item(TTR("Loading the operations..."));
		filters_menu->set_item_disabled(filters_menu->get_item_count() - 1, true);
		return;
	}
	const Dictionary root = TetraSchema::category_tree(operations);
	const Array categories = root["categories"];
	Dictionary rest = root.duplicate();
	Array other_categories;
	for (int i = 0; i < categories.size(); i++) {
		const Dictionary category = categories[i];
		if (category["name"] == "Colors") {
			_add_operation_items(colors_menu, category);
		} else if (category["name"] == "Filters") {
			_add_operation_items(filters_menu, category);
		} else {
			other_categories.push_back(category);
		}
	}
	// Whatever an engine files elsewhere still gets a place in Filters.
	rest["categories"] = other_categories;
	_add_operation_items(filters_menu, rest);
}

void TetraImageWorkspace::_add_operation_items(PopupMenu *p_menu, const Dictionary &p_node) {
	const Array categories = p_node["categories"];
	for (int i = 0; i < categories.size(); i++) {
		const Dictionary category = categories[i];
		PopupMenu *submenu = memnew(PopupMenu);
		submenu->connect("index_pressed", callable_mp(this, &TetraImageWorkspace::_operation_menu_pressed).bind(submenu));
		p_menu->add_submenu_node_item(category["name"], submenu);
		_add_operation_items(submenu, category);
	}
	const Array ids = p_node["operations"];
	// Where Tetra's own operation and an engine's carry the same name in one
	// menu, the engine's is marked.
	HashSet<String> native_labels;
	for (int i = 0; i < ids.size(); i++) {
		const HashMap<String, Dictionary>::ConstIterator found = operations_by_id.find(ids[i]);
		if (found && found->value.get("source", String()) == "native") {
			native_labels.insert(found->value["label"]);
		}
	}
	for (int i = 0; i < ids.size(); i++) {
		const String id = ids[i];
		const HashMap<String, Dictionary>::ConstIterator found = operations_by_id.find(id);
		if (!found) {
			continue;
		}
		const Dictionary &operation = found->value;
		const bool has_params = !Array(operation.get("params", Array())).is_empty();
		String label = operation["label"];
		const String source = operation.get("source", String());
		if (source != "native" && native_labels.has(label)) {
			label += " (" + source.to_upper() + ")";
		}
		p_menu->add_item(label + (has_params ? "..." : ""));
		const int index = p_menu->get_item_count() - 1;
		p_menu->set_item_metadata(index, id);
		p_menu->set_item_tooltip(index, operation.get("description", String()));
	}
}

void TetraImageWorkspace::_operation_menu_pressed(int p_index, PopupMenu *p_menu) {
	ERR_FAIL_INDEX(p_index, p_menu->get_item_count());
	const Variant item_data = p_menu->get_item_metadata(p_index);
	if (item_data.get_type() == Variant::STRING) {
		_open_operation(item_data, Dictionary(), false);
	} else {
		_menu_option(p_menu->get_item_id(p_index));
	}
}

void TetraImageWorkspace::_update_menus() {
	const bool none = document.is_null();
	const bool no_selection = none || document->is_selection_empty();
	const MenuOption file_options[] = { FILE_SAVE, FILE_SAVE_AS, FILE_EXPORT_AS, FILE_CLOSE };
	for (MenuOption option : file_options) {
		set_option_disabled(file_menu, option, none);
	}

	const PackedStringArray undo_names = none ? PackedStringArray() : document->get_undo_names();
	const PackedStringArray redo_names = none ? PackedStringArray() : document->get_redo_names();
	const int undo_index = edit_menu->get_item_index(EDIT_UNDO);
	edit_menu->set_item_text(undo_index, undo_names.is_empty() ? TTR("Undo") : vformat(TTR("Undo %s"), undo_names[undo_names.size() - 1]));
	edit_menu->set_item_disabled(undo_index, undo_names.is_empty());
	const int redo_index = edit_menu->get_item_index(EDIT_REDO);
	edit_menu->set_item_text(redo_index, redo_names.is_empty() ? TTR("Redo") : vformat(TTR("Redo %s"), redo_names[redo_names.size() - 1]));
	edit_menu->set_item_disabled(redo_index, redo_names.is_empty());
	set_option_disabled(edit_menu, EDIT_CLEAR, none);
	set_option_disabled(edit_menu, EDIT_FILL_FOREGROUND, none);
	set_option_disabled(edit_menu, EDIT_FILL_BACKGROUND, none);

	set_option_disabled(select_menu, SELECT_ALL, none);
	set_option_disabled(select_menu, SELECT_INVERT, none);
	const MenuOption selection_options[] = { SELECT_NONE, SELECT_FEATHER, SELECT_GROW, SELECT_SHRINK };
	for (MenuOption option : selection_options) {
		set_option_disabled(select_menu, option, no_selection);
	}
	for (int i = 0; i < view_menu->get_item_count(); i++) {
		view_menu->set_item_disabled(i, none);
	}
	for (int i = 0; i < image_menu->get_item_count(); i++) {
		if (!image_menu->is_item_separator(i)) {
			image_menu->set_item_disabled(i, none);
		}
	}
	set_option_disabled(image_menu, IMAGE_CROP_TO_SELECTION, no_selection);
	set_option_disabled(image_menu, IMAGE_FLATTEN, none || document->get_layer_count() < 2);

	const int layer_index = none ? -1 : document->get_layer_index(document->get_active_layer());
	const int layer_count = none ? 0 : document->get_layer_count();
	for (int i = 0; i < layer_menu->get_item_count(); i++) {
		if (!layer_menu->is_item_separator(i)) {
			layer_menu->set_item_disabled(i, none);
		}
	}
	set_option_disabled(layer_menu, LAYER_RAISE, none || layer_index >= layer_count - 1);
	set_option_disabled(layer_menu, LAYER_LOWER, none || layer_index <= 0);
	set_option_disabled(layer_menu, LAYER_MERGE_DOWN, none || layer_index <= 0);
	set_option_disabled(layer_menu, LAYER_DELETE, none || layer_count < 2);

	const int repeat_index = filters_menu->get_item_index(FILTER_REPEAT_LAST);
	filters_menu->set_item_text(repeat_index, last_filter_id.is_empty() ? TTR("Repeat Last Filter") : vformat(TTR("Repeat \"%s\""), last_filter_label));
	filters_menu->set_item_disabled(repeat_index, none || last_filter_id.is_empty());
}

void TetraImageWorkspace::_build_dialogs() {
	new_dialog = memnew(ConfirmationDialog);
	new_dialog->set_title(TTR("Create a New Image"));
	new_dialog->set_ok_button_text(TTR("Create"));
	new_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_new_dialog_confirmed));
	add_child(new_dialog);
	GridContainer *new_grid = memnew(GridContainer);
	new_grid->set_columns(2);
	new_dialog->add_child(new_grid);
	new_grid->add_child(memnew(Label(TTR("Width:"))));
	new_width = make_pixel_spin(1, 16384, 1024);
	new_grid->add_child(new_width);
	new_grid->add_child(memnew(Label(TTR("Height:"))));
	new_height = make_pixel_spin(1, 16384, 768);
	new_grid->add_child(new_height);
	new_grid->add_child(memnew(Label(TTR("Fill with:"))));
	new_fill = memnew(OptionButton);
	new_fill->add_item(TTR("White"));
	new_fill->add_item(TTR("Background colour"));
	new_fill->add_item(TTR("Foreground colour"));
	new_fill->add_item(TTR("Transparency"));
	new_grid->add_child(new_fill);
	new_dialog->register_text_enter(new_width->get_line_edit());
	new_dialog->register_text_enter(new_height->get_line_edit());

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	file_dialog->connect("file_selected", callable_mp(this, &TetraImageWorkspace::_file_selected));
	add_child(file_dialog);

	amount_dialog = memnew(ConfirmationDialog);
	amount_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_amount_confirmed));
	add_child(amount_dialog);
	HBoxContainer *amount_box = memnew(HBoxContainer);
	amount_dialog->add_child(amount_box);
	amount_label = memnew(Label);
	amount_box->add_child(amount_label);
	amount_value = make_pixel_spin(1, 1000, 5);
	amount_box->add_child(amount_value);
	amount_dialog->register_text_enter(amount_value->get_line_edit());

	size_dialog = memnew(ConfirmationDialog);
	size_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_size_confirmed));
	add_child(size_dialog);
	GridContainer *size_grid = memnew(GridContainer);
	size_grid->set_columns(2);
	size_dialog->add_child(size_grid);
	size_grid->add_child(memnew(Label(TTR("Width:"))));
	size_width = make_pixel_spin(1, 16384, 1024);
	size_width->connect(SceneStringName(value_changed), callable_mp(this, &TetraImageWorkspace::_size_value_changed).bind(true));
	size_grid->add_child(size_width);
	size_grid->add_child(memnew(Label(TTR("Height:"))));
	size_height = make_pixel_spin(1, 16384, 768);
	size_height->connect(SceneStringName(value_changed), callable_mp(this, &TetraImageWorkspace::_size_value_changed).bind(false));
	size_grid->add_child(size_height);
	size_grid->add_child(memnew(Control));
	size_keep_aspect = memnew(CheckBox(TTR("Keep the aspect ratio")));
	size_keep_aspect->set_pressed(true);
	size_grid->add_child(size_keep_aspect);
	size_interpolation_label = memnew(Label(TTR("Interpolation:")));
	size_grid->add_child(size_interpolation_label);
	size_interpolation = memnew(OptionButton);
	size_interpolation->add_item(TTR("None (nearest)"), TetraImageDocument::INTERPOLATION_NEAREST);
	size_interpolation->add_item(TTR("Linear"), TetraImageDocument::INTERPOLATION_LINEAR);
	size_interpolation->add_item(TTR("Cubic"), TetraImageDocument::INTERPOLATION_CUBIC);
	size_interpolation->select(2);
	size_grid->add_child(size_interpolation);
	size_offset_x_label = memnew(Label(TTR("Offset X:")));
	size_grid->add_child(size_offset_x_label);
	size_offset_x = make_pixel_spin(-16384, 16384, 0);
	size_grid->add_child(size_offset_x);
	size_offset_y_label = memnew(Label(TTR("Offset Y:")));
	size_grid->add_child(size_offset_y_label);
	size_offset_y = make_pixel_spin(-16384, 16384, 0);
	size_grid->add_child(size_offset_y);
	size_center_spacer = memnew(Control);
	size_grid->add_child(size_center_spacer);
	size_center = memnew(Button(TTR("Center")));
	size_center->set_tooltip_text(TTR("Place the image in the middle of the new canvas."));
	size_center->connect(SceneStringName(pressed), callable_mp(this, &TetraImageWorkspace::_size_center_pressed));
	size_grid->add_child(size_center);

	discard_dialog = memnew(ConfirmationDialog);
	discard_dialog->set_title(TTR("Unsaved Changes"));
	discard_dialog->set_ok_button_text(TTR("Discard Changes"));
	discard_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_discard_confirmed));
	add_child(discard_dialog);

	save_choice_dialog = memnew(ConfirmationDialog);
	save_choice_dialog->set_title(TTR("Save Layered Image"));
	save_choice_dialog->set_ok_button_text(TTR("Save as .ora"));
	save_choice_dialog->add_button(TTR("Export Flattened"), true, "flatten");
	save_choice_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_save_choice_confirmed));
	save_choice_dialog->connect("custom_action", callable_mp(this, &TetraImageWorkspace::_save_choice_action));
	add_child(save_choice_dialog);

	search_dialog = memnew(AcceptDialog);
	search_dialog->set_title(TTR("Search Filters"));
	search_dialog->set_ok_button_text(TTR("Open"));
	search_dialog->connect(SceneStringName(confirmed), callable_mp(this, &TetraImageWorkspace::_search_confirmed));
	add_child(search_dialog);
	operation_browser = memnew(TetraOperationBrowser);
	operation_browser->set_custom_minimum_size(Size2(420, 460) * EDSCALE);
	operation_browser->connect("operation_activated", callable_mp(this, &TetraImageWorkspace::_search_activated));
	search_dialog->add_child(operation_browser);

	operation_dialog = memnew(TetraOperationDialog);
	operation_dialog->set_preview_enabled(true);
	operation_dialog->connect("values_changed", callable_mp(this, &TetraImageWorkspace::_dialog_values_changed));
	operation_dialog->connect("preview_toggled", callable_mp(this, &TetraImageWorkspace::_dialog_preview_toggled));
	operation_dialog->connect("operation_confirmed", callable_mp(this, &TetraImageWorkspace::_dialog_confirmed));
	operation_dialog->connect("operation_cancelled", callable_mp(this, &TetraImageWorkspace::_dialog_cancelled));
	add_child(operation_dialog);
}

void TetraImageWorkspace::_menu_option(int p_option) {
	if (busy || filter_session) {
		return;
	}
	switch (p_option) {
		case FILE_NEW: {
			_confirm_discard(callable_mp(this, &TetraImageWorkspace::_popup_new_dialog));
		}
			return;
		case FILE_OPEN: {
			_confirm_discard(callable_mp(this, &TetraImageWorkspace::_popup_file_dialog).bind(int(FILE_PURPOSE_OPEN)));
		}
			return;
		default:
			break;
	}
	if (document.is_null() || canvas->is_tool_active()) {
		return;
	}
	const int layer = document->get_active_layer();
	switch (p_option) {
		case FILE_SAVE: {
			_save_requested();
		} break;
		case FILE_SAVE_AS: {
			_popup_file_dialog(FILE_PURPOSE_SAVE_AS);
		} break;
		case FILE_EXPORT_AS: {
			_popup_file_dialog(FILE_PURPOSE_EXPORT);
		} break;
		case FILE_CLOSE: {
			_confirm_discard(callable_mp(this, &TetraImageWorkspace::_close_confirmed));
		} break;
		case EDIT_UNDO: {
			undo();
		} break;
		case EDIT_REDO: {
			redo();
		} break;
		case EDIT_CLEAR: {
			document->clear(layer);
		} break;
		case EDIT_FILL_FOREGROUND: {
			document->fill(layer, foreground.srgb_to_linear());
		} break;
		case EDIT_FILL_BACKGROUND: {
			document->fill(layer, background.srgb_to_linear());
		} break;
		case SELECT_ALL: {
			document->select_all();
		} break;
		case SELECT_NONE: {
			document->select_none();
		} break;
		case SELECT_INVERT: {
			document->invert_selection();
		} break;
		case SELECT_FEATHER:
		case SELECT_GROW:
		case SELECT_SHRINK: {
			_popup_amount_dialog(MenuOption(p_option));
		} break;
		case IMAGE_FLIP_HORIZONTAL: {
			document->flip_image(false);
		} break;
		case IMAGE_FLIP_VERTICAL: {
			document->flip_image(true);
		} break;
		case IMAGE_ROTATE_90_CW: {
			document->rotate_image(90);
		} break;
		case IMAGE_ROTATE_90_CCW: {
			document->rotate_image(270);
		} break;
		case IMAGE_ROTATE_180: {
			document->rotate_image(180);
		} break;
		case IMAGE_SCALE: {
			_popup_size_dialog(SIZE_DIALOG_SCALE);
		} break;
		case IMAGE_CANVAS_SIZE: {
			_popup_size_dialog(SIZE_DIALOG_CANVAS);
		} break;
		case IMAGE_CROP_TO_SELECTION: {
			if (document->is_selection_empty()) {
				_status_message(TTR("Select the area to keep first."));
			} else {
				document->crop_to_selection();
			}
		} break;
		case IMAGE_FLATTEN: {
			document->flatten();
		} break;
		case LAYER_NEW: {
			layers_panel->new_layer();
		} break;
		case LAYER_DUPLICATE: {
			layers_panel->duplicate_layer();
		} break;
		case LAYER_RAISE: {
			layers_panel->raise_layer();
		} break;
		case LAYER_LOWER: {
			layers_panel->lower_layer();
		} break;
		case LAYER_MERGE_DOWN: {
			layers_panel->merge_down();
		} break;
		case LAYER_DELETE: {
			layers_panel->delete_layer();
		} break;
		case LAYER_FLIP_HORIZONTAL: {
			document->flip_layer(layer, false);
		} break;
		case LAYER_FLIP_VERTICAL: {
			document->flip_layer(layer, true);
		} break;
		case FILTER_REPEAT_LAST: {
			repeat_last_operation();
		} break;
		case FILTER_SEARCH: {
			_popup_search();
		} break;
		case VIEW_ZOOM_IN: {
			canvas->set_zoom_centered(canvas->get_zoom() * 1.5);
		} break;
		case VIEW_ZOOM_OUT: {
			canvas->set_zoom_centered(canvas->get_zoom() / 1.5);
		} break;
		case VIEW_ZOOM_100: {
			canvas->set_zoom_centered(1.0);
		} break;
		case VIEW_ZOOM_FIT: {
			canvas->zoom_to_fit();
		} break;
		default:
			break;
	}
}

void TetraImageWorkspace::_confirm_discard(const Callable &p_then) {
	if (document.is_null() || !modified) {
		p_then.call();
		return;
	}
	discard_action = p_then;
	discard_dialog->set_text(vformat(TTR("\"%s\" has unsaved changes.\nDiscard them?"), document_path.is_empty() ? TTR("[Untitled]") : document_path.get_file()));
	discard_dialog->popup_centered();
}

void TetraImageWorkspace::_discard_confirmed() {
	const Callable action = discard_action;
	discard_action = Callable();
	if (action.is_valid()) {
		action.call();
	}
}

void TetraImageWorkspace::_popup_new_dialog() {
	new_dialog->popup_centered();
	new_width->get_line_edit()->grab_focus();
}

void TetraImageWorkspace::_new_dialog_confirmed() {
	Color fill(1, 1, 1, 1);
	switch (new_fill->get_selected()) {
		case 1: {
			fill = background;
		} break;
		case 2: {
			fill = foreground;
		} break;
		case 3: {
			fill = Color(0, 0, 0, 0);
		} break;
		default:
			break;
	}
	const Size2i size(int(new_width->get_value()), int(new_height->get_value()));
	if (!_check_size(size)) {
		return;
	}
	new_document(size.x, size.y, fill);
}

bool TetraImageWorkspace::_check_size(const Size2i &p_size) {
	if (TetraImageDocument::is_size_supported(p_size.x, p_size.y)) {
		return true;
	}
	// Every layer is an untiled float buffer: 16 bytes per pixel, several times over while painting.
	EditorNode::get_singleton()->show_warning(vformat(TTR("%d x %d pixels is too large for the Image workspace, which handles up to %d megapixels (for example %d x %d)."), p_size.x, p_size.y, TetraImageDocument::MAX_PIXELS / 1000000, 8192, 8192));
	return false;
}

void TetraImageWorkspace::_popup_file_dialog(int p_purpose) {
	file_purpose = FilePurpose(p_purpose);
	file_dialog->clear_filters();
	const String directory = ProjectSettings::get_singleton()->globalize_path(document_path.is_empty() ? String("res://") : document_path.get_base_dir());
	const String base_name = document_path.is_empty() ? String("untitled") : document_path.get_file().get_basename();
	switch (file_purpose) {
		case FILE_PURPOSE_OPEN: {
			file_dialog->set_title(TTR("Open Image"));
			file_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
			file_dialog->add_filter("*.png, *.jpg, *.jpeg, *.webp, *.exr, *.tga, *.bmp, *.ora", TTR("Images"));
			file_dialog->set_current_dir(directory);
		} break;
		case FILE_PURPOSE_SAVE_AS: {
			file_dialog->set_title(TTR("Save Image"));
			file_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
			file_dialog->add_filter("*.ora", TTR("OpenRaster, keeps the layers"));
			file_dialog->set_current_dir(directory);
			file_dialog->set_current_file(base_name + ".ora");
		} break;
		case FILE_PURPOSE_EXPORT: {
			file_dialog->set_title(TTR("Export Image"));
			file_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
			file_dialog->add_filter("*.png", TTR("PNG"));
			file_dialog->add_filter("*.jpg, *.jpeg", TTR("JPEG"));
			file_dialog->add_filter("*.webp", TTR("WebP"));
			file_dialog->add_filter("*.exr", TTR("OpenEXR, linear float"));
			file_dialog->set_current_dir(directory);
			file_dialog->set_current_file(base_name + ".png");
		} break;
	}
	file_dialog->popup_file_dialog();
}

void TetraImageWorkspace::_file_selected(const String &p_path) {
	String path = ProjectSettings::get_singleton()->localize_path(p_path);
	Error err = OK;
	switch (file_purpose) {
		case FILE_PURPOSE_OPEN: {
			_open_checked(path);
		}
			return;
		case FILE_PURPOSE_SAVE_AS: {
			if (path.get_extension().to_lower() != "ora") {
				path += ".ora";
			}
			err = _write_file(path, true);
		} break;
		case FILE_PURPOSE_EXPORT: {
			if (!is_flat_extension(path)) {
				path += ".png";
			}
			err = _write_file(path, false);
		} break;
	}
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not write \"%s\" (%s)."), path, error_names[err]));
	}
}

void TetraImageWorkspace::_open_checked(const String &p_path) {
	const Error err = open_file(p_path);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not open \"%s\" as an image (%s)."), p_path, error_names[err]));
	}
}

bool TetraImageWorkspace::can_save_directly() const {
	if (document.is_null() || document_path.is_empty()) {
		return false;
	}
	if (document_path.get_extension().to_lower() == "ora") {
		return true;
	}
	return is_flat_extension(document_path) && document->get_layer_count() == 1;
}

void TetraImageWorkspace::_save_requested() {
	if (document_path.is_empty()) {
		_popup_file_dialog(FILE_PURPOSE_SAVE_AS);
	} else if (can_save_directly()) {
		const Error err = save();
		if (err != OK) {
			EditorNode::get_singleton()->show_warning(vformat(TTR("Could not write \"%s\" (%s)."), document_path, error_names[err]));
		}
	} else if (is_flat_extension(document_path)) {
		save_choice_dialog->set_text(vformat(TTR("\"%s\" has %d layers, which a .%s file cannot hold.\n\nExport Flattened writes the visible result to that file and keeps the layers here.\nSave as .ora writes \"%s\" with the layers."), document_path.get_file(), document->get_layer_count(), document_path.get_extension(), document_path.get_file().get_basename() + ".ora"));
		save_choice_dialog->popup_centered();
	} else {
		_status_message(vformat(TTR("Tetra cannot write .%s files; save as OpenRaster or export instead."), document_path.get_extension()));
		_popup_file_dialog(FILE_PURPOSE_SAVE_AS);
	}
}

void TetraImageWorkspace::_save_choice_action(const String &p_action) {
	if (p_action != "flatten") {
		return;
	}
	save_choice_dialog->hide();
	const Error err = _write_file(document_path, true);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not write \"%s\" (%s)."), document_path, error_names[err]));
	}
}

void TetraImageWorkspace::_save_choice_confirmed() {
	const String path = document_path.get_basename() + ".ora";
	if (FileAccess::exists(path)) {
		// Some other image may be in that file: the file dialog asks before it overwrites.
		_status_message(vformat(TTR("\"%s\" already exists; choose where to save."), path.get_file()));
		_popup_file_dialog(FILE_PURPOSE_SAVE_AS);
		return;
	}
	const Error err = _write_file(path, true);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not write \"%s\" (%s)."), path, error_names[err]));
	}
}

Error TetraImageWorkspace::_write_file(const String &p_path, bool p_becomes_document) {
	ERR_FAIL_COND_V(document.is_null(), ERR_UNCONFIGURED);
	Error err;
	if (p_path.get_extension().to_lower() == "ora") {
		err = document->save_ora(p_path);
	} else if (is_flat_extension(p_path)) {
		err = document->export_image(p_path);
	} else {
		err = ERR_FILE_UNRECOGNIZED;
	}
	if (err != OK) {
		return err;
	}
	if (p_becomes_document) {
		document_path = p_path;
		// A flat file of a layered image is an export: the layers exist only
		// here, so the document still counts as unsaved.
		if (can_save_directly()) {
			modified = false;
		}
		_update_title();
	}
	_status_message(vformat(modified && p_becomes_document ? TTR("Exported \"%s\"; the layers are not saved yet.") : TTR("Saved \"%s\"."), p_path));
	_file_written(p_path);
	return OK;
}

void TetraImageWorkspace::_file_written(const String &p_path) {
	EditorFileSystem *file_system = EditorFileSystem::get_singleton();
	if (!file_system || !p_path.begins_with("res://")) {
		return;
	}
	file_system->update_file(p_path);
	if (file_system->is_scanning()) {
		return;
	}
	// A texture that is already imported is imported again at once, so the
	// scenes that use it show the new pixels; a new file is picked up by a scan.
	if (FileAccess::exists(p_path + ".import")) {
		file_system->reimport_files({ p_path });
	} else {
		file_system->scan_changes();
	}
}

Error TetraImageWorkspace::save() {
	if (document.is_null() || document_path.is_empty()) {
		return ERR_UNCONFIGURED;
	}
	return _write_file(document_path, true);
}

Error TetraImageWorkspace::save_as(const String &p_path) {
	return _write_file(p_path, true);
}

Error TetraImageWorkspace::export_as(const String &p_path) {
	return _write_file(p_path, false);
}

void TetraImageWorkspace::_popup_amount_dialog(MenuOption p_option) {
	if (document->is_selection_empty()) {
		_status_message(TTR("Nothing is selected."));
		return;
	}
	amount_option = p_option;
	switch (p_option) {
		case SELECT_FEATHER: {
			amount_dialog->set_title(TTR("Feather Selection"));
			amount_label->set_text(TTR("Feather the selection by:"));
		} break;
		case SELECT_GROW: {
			amount_dialog->set_title(TTR("Grow Selection"));
			amount_label->set_text(TTR("Grow the selection by:"));
		} break;
		default: {
			amount_dialog->set_title(TTR("Shrink Selection"));
			amount_label->set_text(TTR("Shrink the selection by:"));
		} break;
	}
	amount_dialog->reset_size();
	amount_dialog->popup_centered();
	amount_value->get_line_edit()->grab_focus();
}

void TetraImageWorkspace::_amount_confirmed() {
	if (document.is_null()) {
		return;
	}
	const double amount = amount_value->get_value();
	switch (amount_option) {
		case SELECT_FEATHER: {
			document->feather_selection(amount);
		} break;
		case SELECT_GROW: {
			document->grow_selection(int(amount));
		} break;
		default: {
			document->shrink_selection(int(amount));
		} break;
	}
}

void TetraImageWorkspace::_popup_size_dialog(SizeDialogMode p_mode) {
	size_dialog_mode = p_mode;
	const bool scale = p_mode == SIZE_DIALOG_SCALE;
	size_dialog->set_title(scale ? TTR("Scale Image") : TTR("Set Canvas Size"));
	size_dialog->set_ok_button_text(scale ? TTR("Scale") : TTR("Resize"));
	size_dialog_updating = true;
	size_width->set_value(document->get_width());
	size_height->set_value(document->get_height());
	size_offset_x->set_value(0);
	size_offset_y->set_value(0);
	size_dialog_updating = false;
	size_interpolation_label->set_visible(scale);
	size_interpolation->set_visible(scale);
	Control *const canvas_controls[] = { size_offset_x_label, size_offset_x, size_offset_y_label, size_offset_y, size_center_spacer, size_center };
	for (Control *control : canvas_controls) {
		control->set_visible(!scale);
	}
	size_dialog->reset_size();
	size_dialog->popup_centered();
	size_width->get_line_edit()->grab_focus();
}

void TetraImageWorkspace::_size_value_changed(double p_value, bool p_width) {
	if (size_dialog_updating || document.is_null() || !size_keep_aspect->is_pressed()) {
		return;
	}
	size_dialog_updating = true;
	const double aspect = double(document->get_width()) / double(document->get_height());
	if (p_width) {
		size_height->set_value(MAX(1.0, Math::round(p_value / aspect)));
	} else {
		size_width->set_value(MAX(1.0, Math::round(p_value * aspect)));
	}
	size_dialog_updating = false;
}

void TetraImageWorkspace::_size_center_pressed() {
	if (document.is_null()) {
		return;
	}
	size_offset_x->set_value(Math::floor((size_width->get_value() - document->get_width()) / 2.0));
	size_offset_y->set_value(Math::floor((size_height->get_value() - document->get_height()) / 2.0));
}

void TetraImageWorkspace::_size_confirmed() {
	if (document.is_null()) {
		return;
	}
	const Size2i size(int(size_width->get_value()), int(size_height->get_value()));
	if (!_check_size(size)) {
		return;
	}
	if (size_dialog_mode == SIZE_DIALOG_SCALE) {
		document->scale_image(size, TetraImageDocument::Interpolation(size_interpolation->get_selected_id()));
	} else {
		document->resize_canvas(size, Vector2i(int(size_offset_x->get_value()), int(size_offset_y->get_value())));
	}
}

void TetraImageWorkspace::_close_confirmed() {
	close_document();
}

void TetraImageWorkspace::_operations_thread(void *p_userdata) {
	TetraImageWorkspace *workspace = static_cast<TetraImageWorkspace *>(p_userdata);
	const Array found = TetraImageDocument::get_operations();
	callable_mp(workspace, &TetraImageWorkspace::_operations_loaded).call_deferred(found);
}

void TetraImageWorkspace::_load_operations() {
	if (operations_loaded || operations_task_running) {
		return;
	}
	// Listing the GEGL operations starts GEGL, which takes about a second.
	operations_task_running = true;
	operations_task = WorkerThreadPool::get_singleton()->add_native_task(&TetraImageWorkspace::_operations_thread, this, false, "Tetra image operations");
}

void TetraImageWorkspace::_operations_loaded(const Array &p_operations) {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(operations_task);
	operations_task_running = false;
	operations_loaded = true;
	operations = p_operations;
	operations_by_id.clear();
	for (int i = 0; i < operations.size(); i++) {
		const Dictionary operation = operations[i];
		operations_by_id[operation["id"]] = operation;
	}
	_rebuild_operation_menus();
	_update_menus();
	operation_browser->set_operations(operations);
	emit_signal(SNAME("operations_loaded"));
}

Dictionary TetraImageWorkspace::_find_operation(const String &p_id) const {
	const HashMap<String, Dictionary>::ConstIterator found = operations_by_id.find(p_id);
	if (found) {
		return found->value;
	}
	if (!operations_loaded) {
		// The native operations are known without waiting for GEGL.
		const Array native = TetraImageDocument::get_native_operations();
		for (int i = 0; i < native.size(); i++) {
			const Dictionary operation = native[i];
			if (operation["id"] == p_id) {
				return operation;
			}
		}
	}
	return Dictionary();
}

void TetraImageWorkspace::_popup_search() {
	search_dialog->popup_centered();
	operation_browser->set_filter_text(String());
	operation_browser->focus_search();
}

void TetraImageWorkspace::_search_confirmed() {
	const String id = operation_browser->get_selected_id();
	if (!id.is_empty()) {
		_open_operation(id, Dictionary(), false);
	}
}

void TetraImageWorkspace::_search_activated(const String &p_id) {
	search_dialog->hide();
	_open_operation(p_id, Dictionary(), false);
}

// The operation dialog edits colours with a colour picker, which shows and
// returns display (sRGB) values, while the engines and everything the workspace
// keeps (remembered values, the scripting surface) use linear ones.
static Dictionary convert_colors(const Dictionary &p_values, bool p_to_linear) {
	Dictionary converted = p_values.duplicate(true);
	for (const KeyValue<Variant, Variant> &entry : p_values) {
		if (entry.value.get_type() == Variant::COLOR) {
			const Color color = entry.value;
			converted[entry.key] = p_to_linear ? color.srgb_to_linear() : color.linear_to_srgb();
		}
	}
	return converted;
}

static Dictionary colors_to_linear(const Dictionary &p_values) {
	return convert_colors(p_values, true);
}

static Dictionary colors_to_display(const Dictionary &p_values) {
	return convert_colors(p_values, false);
}

static Dictionary operation_for_display(const Dictionary &p_operation) {
	Dictionary operation = p_operation.duplicate(true);
	Array params = operation.get("params", Array());
	for (int i = 0; i < params.size(); i++) {
		Dictionary param = params[i];
		if (param.get("type", String()) == "color" && param.get("default", Variant()).get_type() == Variant::COLOR) {
			const Color linear = param["default"];
			param["default"] = linear.linear_to_srgb();
		}
	}
	return operation;
}

void TetraImageWorkspace::_open_operation(const String &p_id, const Dictionary &p_values, bool p_use_given_values) {
	const Dictionary operation = _find_operation(p_id);
	if (operation.is_empty()) {
		_status_message(operations_loaded ? vformat(TTR("There is no operation \"%s\"."), p_id) : TTR("The operations are still being listed; try again in a moment."));
		return;
	}
	if (!_begin_filter(p_id)) {
		return;
	}
	if (Array(operation.get("params", Array())).is_empty()) {
		_request_filter(Dictionary(), true);
		return;
	}
	Dictionary initial = p_values;
	if (!p_use_given_values && remembered_values.has(p_id)) {
		initial = remembered_values[p_id];
	}
	operation_dialog->set_preview_checked(true);
	operation_dialog->popup_operation(operation_for_display(operation), colors_to_display(TetraSchema::sanitize_values(operation, initial)));
	_request_filter(colors_to_linear(operation_dialog->get_values()), false);
}

bool TetraImageWorkspace::_begin_filter(const String &p_id) {
	if (document.is_null()) {
		_status_message(TTR("Create or open an image first."));
		return false;
	}
	if (busy || filter_session || canvas->is_tool_active() || document->is_stroke_active() || document->is_undo_group_open()) {
		return false;
	}
	const Dictionary operation = _find_operation(p_id);
	if (operation.is_empty()) {
		return false;
	}
	filter_session = true;
	filter_document = document;
	filter_layer = document->get_active_layer();
	filter_id = p_id;
	filter_label = operation["label"];
	filter_source = document->layer_get_image(filter_layer);
	filter_preview_shown = false;
	filter_shown_values = Dictionary();
	canvas->set_input_locked(true);
	return true;
}

void TetraImageWorkspace::_request_filter(const Dictionary &p_values, bool p_commit) {
	ERR_FAIL_COND(!filter_session);
	FilterRequest *request = memnew(FilterRequest);
	request->serial = ++filter_serial;
	request->operation_id = filter_id;
	request->values = p_values.duplicate(true);
	request->source = filter_source;
	request->commit = p_commit;
	if (p_commit) {
		_set_busy(true, vformat(TTR("Applying %s..."), filter_label));
	}
	if (filter_task_running) {
		// The running computation cannot be stopped; the newest request waits
		// for it and replaces whatever was waiting before.
		if (filter_waiting) {
			memdelete(filter_waiting);
		}
		filter_waiting = request;
	} else {
		_start_filter_task(request);
	}
}

void TetraImageWorkspace::_start_filter_task(FilterRequest *p_request) {
	filter_task_running = true;
	filter_task = WorkerThreadPool::get_singleton()->add_template_task(this, &TetraImageWorkspace::_filter_thread, p_request, false, "Tetra image filter");
}

void TetraImageWorkspace::_filter_thread(FilterRequest *p_request) {
	const Ref<Image> result = TetraImageDocument::filter_image(p_request->source, p_request->operation_id, p_request->values);
	callable_mp(this, &TetraImageWorkspace::_filter_finished).call_deferred(p_request->serial, result, p_request->commit, p_request->values);
	memdelete(p_request);
}

void TetraImageWorkspace::_filter_finished(uint64_t p_serial, const Ref<Image> &p_result, bool p_commit, const Dictionary &p_values) {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(filter_task);
	filter_task_running = false;
	if (filter_waiting) {
		FilterRequest *next = filter_waiting;
		filter_waiting = nullptr;
		_start_filter_task(next);
		return;
	}
	if (!filter_session || p_serial != filter_serial) {
		return;
	}
	if (p_result.is_null()) {
		_status_message(vformat(TTR("%s failed with these values."), filter_label));
		if (p_commit) {
			_drop_preview();
			_set_busy(false);
			_end_filter();
		}
		return;
	}
	_drop_preview();
	if (p_commit) {
		const Error err = filter_document->apply_filtered_image(filter_layer, p_result, filter_label);
		_set_busy(false);
		const String id = filter_id;
		const String label = filter_label;
		_end_filter();
		if (err == OK) {
			_remember_filter(id, p_values);
			_status_message(vformat(TTR("Applied %s."), label));
			emit_signal(SNAME("operation_applied"), id);
		}
		_document_edited();
		return;
	}
	if (!operation_dialog->is_preview_checked()) {
		return;
	}
	// The preview is a real edit inside an undo group that stays open: OK
	// closes the group, anything else cancels it.
	filter_document->begin_undo_group(filter_label);
	if (filter_document->apply_filtered_image(filter_layer, p_result, filter_label) == OK) {
		filter_preview_shown = true;
		filter_shown_values = p_values;
	} else {
		filter_document->cancel_undo_group();
	}
}

void TetraImageWorkspace::_drop_preview() {
	if (filter_preview_shown) {
		filter_document->cancel_undo_group();
		filter_preview_shown = false;
		filter_shown_values = Dictionary();
	}
}

void TetraImageWorkspace::_end_filter() {
	filter_session = false;
	// Results of computations still under way belong to no session now.
	filter_serial++;
	if (filter_waiting) {
		memdelete(filter_waiting);
		filter_waiting = nullptr;
	}
	filter_document.unref();
	filter_source.unref();
	filter_layer = -1;
	canvas->set_input_locked(false);
	if (operation_dialog->is_visible()) {
		operation_dialog->hide();
	}
}

void TetraImageWorkspace::_dialog_values_changed(const Dictionary &p_values) {
	if (filter_session && operation_dialog->is_preview_checked()) {
		_request_filter(colors_to_linear(p_values), false);
	}
}

void TetraImageWorkspace::_dialog_preview_toggled(bool p_checked) {
	if (!filter_session) {
		return;
	}
	if (p_checked) {
		_request_filter(colors_to_linear(operation_dialog->get_values()), false);
	} else {
		// Also outdates a preview that is still being computed.
		filter_serial++;
		_drop_preview();
	}
}

void TetraImageWorkspace::_dialog_confirmed(const String &p_id, const Dictionary &p_dialog_values) {
	if (!filter_session) {
		return;
	}
	const Dictionary values = colors_to_linear(p_dialog_values);
	if (filter_preview_shown && !filter_task_running && filter_shown_values.recursive_equal(values, 0)) {
		// What is on screen is exactly what was asked for: keep it as one undo step.
		filter_document->end_undo_group();
		filter_preview_shown = false;
		const String label = filter_label;
		_end_filter();
		_remember_filter(p_id, values);
		_status_message(vformat(TTR("Applied %s."), label));
		emit_signal(SNAME("operation_applied"), p_id);
		_document_edited();
		return;
	}
	_request_filter(values, true);
}

void TetraImageWorkspace::_dialog_cancelled(const String &p_id) {
	if (!filter_session || busy) {
		return;
	}
	_drop_preview();
	_end_filter();
	_status_message(vformat(TTR("%s cancelled."), _find_operation(p_id).get("label", p_id)));
}

void TetraImageWorkspace::_remember_filter(const String &p_id, const Dictionary &p_values) {
	last_filter_id = p_id;
	last_filter_label = _find_operation(p_id).get("label", p_id);
	last_filter_values = p_values.duplicate(true);
	remembered_values[p_id] = last_filter_values;
	_update_menus();
}

Error TetraImageWorkspace::apply_operation(const String &p_id, const Dictionary &p_values) {
	ERR_FAIL_COND_V(document.is_null(), ERR_UNCONFIGURED);
	if (busy || filter_session || canvas->is_tool_active()) {
		return ERR_BUSY;
	}
	const Dictionary operation = _find_operation(p_id);
	// GEGL operations can be applied by id before the list has arrived.
	const Dictionary values = operation.is_empty() ? p_values : TetraSchema::sanitize_values(operation, p_values);
	const Error err = document->apply_operation(document->get_active_layer(), p_id, values);
	if (err == OK) {
		_remember_filter(p_id, values);
		emit_signal(SNAME("operation_applied"), p_id);
	}
	return err;
}

void TetraImageWorkspace::show_operation_dialog(const String &p_id, const Dictionary &p_values) {
	_open_operation(p_id, p_values, !p_values.is_empty());
}

void TetraImageWorkspace::repeat_last_operation() {
	if (last_filter_id.is_empty()) {
		_status_message(TTR("No filter has been applied yet."));
		return;
	}
	if (_begin_filter(last_filter_id)) {
		_request_filter(last_filter_values, true);
	}
}
