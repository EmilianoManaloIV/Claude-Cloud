#include "tetra_image_workspace.h"

#include "tetra_image_canvas.h"
#include "tetra_image_color_swatch.h"
#include "tetra_image_layers_panel.h"

#include "tetra/editor/tetra_operation_dialog.h"
#include "tetra/editor/tetra_param_panel.h"
#include "tetra/tetra_schema.h"

#include "core/input/input_event.h"
#include "core/input/shortcut.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/center_container.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/menu_bar.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/text_edit.h"
#include "scene/main/window.h"

TetraImageWorkspace *TetraImageWorkspace::singleton = nullptr;

static const char *MENU_NAMES[] = { "File", "Edit", "Select", "View", "Image", "Layer", "Colors", "Filters" };

static Label *make_header(const String &p_text) {
	Label *label = memnew(Label(p_text));
	label->set_theme_type_variation("HeaderSmall");
	label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	return label;
}

void TetraImageWorkspace::_build_layout() {
	HBoxContainer *top = memnew(HBoxContainer);
	add_child(top);

	menu_bar = memnew(MenuBar);
	menu_bar->set_flat(true);
	menu_bar->set_prefer_global_menu(false);
	// The workspace dispatches the menu shortcuts itself, and only while the
	// keyboard focus is inside it (see shortcut_input()).
	menu_bar->set_disable_shortcuts(true);
	top->add_child(menu_bar);

	title_label = memnew(Label);
	title_label->set_h_size_flags(SIZE_EXPAND_FILL);
	title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	title_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	top->add_child(title_label);

	HBoxContainer *body = memnew(HBoxContainer);
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(body);

	// Toolbox: two columns of tools above the colour area, as in GIMP.
	VBoxContainer *toolbox = memnew(VBoxContainer);
	body->add_child(toolbox);
	GridContainer *tool_grid = memnew(GridContainer);
	tool_grid->set_columns(2);
	toolbox->add_child(tool_grid);
	tool_group.instantiate();
	const TetraImageTools::Tool *order = TetraImageTools::get_toolbox_order();
	for (int i = 0; i < TetraImageTools::TOOL_MAX; i++) {
		const TetraImageTools::Tool t = order[i];
		const TetraImageTools::Info &info = TetraImageTools::get_info(t);
		Button *button = memnew(Button);
		button->set_theme_type_variation("FlatButton");
		button->set_toggle_mode(true);
		button->set_button_group(tool_group);
		button->set_focus_mode(FOCUS_NONE);
		button->set_custom_minimum_size(Size2(30, 30) * EDSCALE);
		button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		button->set_tooltip_text(vformat("%s (%s)\n%s", TTRGET(info.label), tool_shortcuts[t]->get_as_text(), TTRGET(info.hint)));
		button->set_pressed(t == tool);
		button->connect(SceneStringName(pressed), callable_mp(this, &TetraImageWorkspace::_tool_button_pressed).bind(int(t)));
		tool_grid->add_child(button);
		tool_buttons[t] = button;
	}
	toolbox->add_child(memnew(HSeparator));
	swatch = memnew(TetraImageColorSwatch);
	swatch->set_h_size_flags(SIZE_SHRINK_CENTER);
	swatch->connect("foreground_pressed", callable_mp(this, &TetraImageWorkspace::_popup_color_picker).bind(false));
	swatch->connect("background_pressed", callable_mp(this, &TetraImageWorkspace::_popup_color_picker).bind(true));
	swatch->connect("swap_pressed", callable_mp(this, &TetraImageWorkspace::swap_colors));
	swatch->connect("default_pressed", callable_mp(this, &TetraImageWorkspace::reset_colors));
	toolbox->add_child(swatch);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_h_size_flags(SIZE_EXPAND_FILL);
	body->add_child(split);

	// Canvas, welcome page and the "working" shield share the centre.
	PanelContainer *centre = memnew(PanelContainer);
	centre->set_h_size_flags(SIZE_EXPAND_FILL);
	centre->set_clip_contents(true);
	split->add_child(centre);

	canvas = memnew(TetraImageCanvas);
	canvas->set_focus_mode(FOCUS_ALL);
	canvas->set_custom_minimum_size(Size2(200, 120) * EDSCALE);
	canvas->hide();
	canvas->connect("pointer_moved", callable_mp(this, &TetraImageWorkspace::_pointer_moved));
	canvas->connect("pointer_exited", callable_mp(this, &TetraImageWorkspace::_pointer_exited));
	canvas->connect("color_picked", callable_mp(this, &TetraImageWorkspace::_color_picked));
	canvas->connect("status_message", callable_mp(this, &TetraImageWorkspace::_status_message));
	canvas->connect("view_changed", callable_mp(this, &TetraImageWorkspace::_update_status));
	centre->add_child(canvas);

	CenterContainer *welcome_center = memnew(CenterContainer);
	welcome = welcome_center;
	centre->add_child(welcome);
	VBoxContainer *welcome_box = memnew(VBoxContainer);
	welcome_box->add_theme_constant_override("separation", 12 * EDSCALE);
	welcome_center->add_child(welcome_box);
	Label *welcome_title = memnew(Label(TTR("Image")));
	welcome_title->set_theme_type_variation("HeaderLarge");
	welcome_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	welcome_box->add_child(welcome_title);
	Label *welcome_text = memnew(Label(TTR("Paint and edit layered images with GIMP's layer modes, tools and GEGL filters.\nCreate an image, open one, or double-click an image in the FileSystem dock.")));
	welcome_text->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	welcome_box->add_child(welcome_text);
	HBoxContainer *welcome_buttons = memnew(HBoxContainer);
	welcome_buttons->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	welcome_box->add_child(welcome_buttons);
	welcome_new = memnew(Button(TTR("New Image...")));
	welcome_new->connect(SceneStringName(pressed), callable_mp(this, &TetraImageWorkspace::_menu_option).bind(int(FILE_NEW)));
	welcome_buttons->add_child(welcome_new);
	Button *welcome_open = memnew(Button(TTR("Open...")));
	welcome_open->connect(SceneStringName(pressed), callable_mp(this, &TetraImageWorkspace::_menu_option).bind(int(FILE_OPEN)));
	welcome_buttons->add_child(welcome_open);

	CenterContainer *shield = memnew(CenterContainer);
	busy_shield = shield;
	busy_shield->set_mouse_filter(MOUSE_FILTER_STOP);
	busy_shield->hide();
	centre->add_child(busy_shield);
	PanelContainer *busy_panel = memnew(PanelContainer);
	shield->add_child(busy_panel);
	busy_label = memnew(Label);
	busy_panel->add_child(busy_label);

	// Right column: Tool Options above Layers.
	VSplitContainer *right = memnew(VSplitContainer);
	right->set_custom_minimum_size(Size2(270, 0) * EDSCALE);
	split->add_child(right);

	VBoxContainer *options_box = memnew(VBoxContainer);
	options_box->set_v_size_flags(SIZE_EXPAND_FILL);
	right->add_child(options_box);
	tool_options_title = make_header(TTR("Tool Options"));
	options_box->add_child(tool_options_title);
	tool_options_panel = memnew(TetraParamPanel);
	tool_options_panel->set_v_size_flags(SIZE_EXPAND_FILL);
	tool_options_panel->set_custom_minimum_size(Size2(0, 120) * EDSCALE);
	tool_options_panel->connect("value_changed", callable_mp(this, &TetraImageWorkspace::_tool_option_changed));
	options_box->add_child(tool_options_panel);

	VBoxContainer *layers_box = memnew(VBoxContainer);
	layers_box->set_v_size_flags(SIZE_EXPAND_FILL);
	right->add_child(layers_box);
	layers_box->add_child(make_header(TTR("Layers")));
	layers_panel = memnew(TetraImageLayersPanel);
	layers_panel->set_v_size_flags(SIZE_EXPAND_FILL);
	layers_box->add_child(layers_panel);

	HBoxContainer *status = memnew(HBoxContainer);
	status->add_theme_constant_override("separation", 12 * EDSCALE);
	add_child(status);
	status_hint = memnew(Label);
	status_hint->set_h_size_flags(SIZE_EXPAND_FILL);
	status_hint->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_hint->set_mouse_filter(MOUSE_FILTER_PASS);
	status->add_child(status_hint);
	status_pointer = memnew(Label);
	status_pointer->set_custom_minimum_size(Size2(90, 0) * EDSCALE);
	status_pointer->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	status_pointer->set_tooltip_text(TTR("Pointer position in image pixels."));
	status_pointer->set_mouse_filter(MOUSE_FILTER_PASS);
	status->add_child(status_pointer);
	status_size = memnew(Label);
	status_size->set_tooltip_text(TTR("Size of the image."));
	status_size->set_mouse_filter(MOUSE_FILTER_PASS);
	status->add_child(status_size);
	status_zoom = memnew(Button);
	status_zoom->set_flat(true);
	status_zoom->set_focus_mode(FOCUS_NONE);
	status_zoom->set_custom_minimum_size(Size2(64, 0) * EDSCALE);
	status_zoom->set_tooltip_text(TTR("Zoom. Click to show the image at 100 %."));
	status_zoom->connect(SceneStringName(pressed), callable_mp(this, &TetraImageWorkspace::_menu_option).bind(int(VIEW_ZOOM_100)));
	status->add_child(status_zoom);

	color_popup = memnew(PopupPanel);
	add_child(color_popup);
	color_picker = memnew(ColorPicker);
	color_picker->set_edit_alpha(false);
	color_picker->connect("color_changed", callable_mp(this, &TetraImageWorkspace::_color_picker_changed));
	color_popup->add_child(color_picker);
}

void TetraImageWorkspace::_set_document(const Ref<TetraImageDocument> &p_document, const String &p_path) {
	if (filter_session) {
		_drop_preview();
		_end_filter();
	}
	if (document.is_valid()) {
		document->disconnect(SNAME("undo_changed"), callable_mp(this, &TetraImageWorkspace::_document_edited));
		document->disconnect(SNAME("size_changed"), callable_mp(this, &TetraImageWorkspace::_update_status));
		document->disconnect(SNAME("selection_changed"), callable_mp(this, &TetraImageWorkspace::_update_menus));
		document->disconnect(SNAME("layers_changed"), callable_mp(this, &TetraImageWorkspace::_update_menus));
	}
	document = p_document;
	document_path = p_path;
	modified = false;
	if (document.is_valid()) {
		document->connect(SNAME("undo_changed"), callable_mp(this, &TetraImageWorkspace::_document_edited));
		document->connect(SNAME("size_changed"), callable_mp(this, &TetraImageWorkspace::_update_status));
		document->connect(SNAME("selection_changed"), callable_mp(this, &TetraImageWorkspace::_update_menus));
		document->connect(SNAME("layers_changed"), callable_mp(this, &TetraImageWorkspace::_update_menus));
	}
	canvas->set_document(document);
	layers_panel->set_document(document);
	canvas->set_visible(document.is_valid());
	welcome->set_visible(document.is_null());
	tool_hint_override = String();
	status_pointer->set_text(String());
	_update_title();
	_update_status();
	_update_menus();
	if (is_visible_in_tree()) {
		if (document.is_valid()) {
			canvas->grab_focus();
		} else {
			welcome_new->grab_focus();
		}
	}
	emit_signal(SNAME("document_changed"));
}

void TetraImageWorkspace::_document_edited() {
	// While a filter preview is shown the document is changed back and forth
	// inside an undo group; only the commit counts as an edit.
	if (!filter_session && !modified) {
		modified = true;
		_update_title();
	}
	_update_menus();
}

void TetraImageWorkspace::_update_title() {
	if (document.is_null()) {
		title_label->set_text(String());
		return;
	}
	String name = document_path.is_empty() ? TTR("[Untitled]") : document_path.get_file();
	if (modified) {
		name += " (*)";
	}
	title_label->set_text(name);
	title_label->set_tooltip_text(document_path);
}

void TetraImageWorkspace::_update_status() {
	const TetraImageTools::Info &info = TetraImageTools::get_info(tool);
	status_hint->set_text(tool_hint_override.is_empty() ? vformat("%s: %s", TTRGET(info.label), TTRGET(info.hint)) : tool_hint_override);
	status_hint->set_tooltip_text(status_hint->get_text());
	if (document.is_valid()) {
		status_size->set_text(vformat(U"%d × %d px", document->get_width(), document->get_height()));
		status_zoom->set_text(vformat("%s %%", String::num(canvas->get_zoom() * 100.0, canvas->get_zoom() < 0.1 ? 1 : 0)));
	} else {
		status_size->set_text(String());
		status_zoom->set_text(String());
	}
}

void TetraImageWorkspace::_update_tool_options() {
	const TetraImageTools::Info &info = TetraImageTools::get_info(tool);
	tool_options_title->set_text(vformat(TTR("Tool Options - %s"), TTRGET(info.label)));
	tool_options_panel->set_operation(tool_schemas[tool]);
	tool_options_panel->set_values(tool_values[tool]);
	canvas->set_tool_options(tool_values[tool]);
}

void TetraImageWorkspace::_tool_button_pressed(int p_tool) {
	set_tool(TetraImageTools::get_info(TetraImageTools::Tool(p_tool)).name);
}

void TetraImageWorkspace::_tool_option_changed(const String &p_name, const Variant &p_value) {
	tool_values[tool][p_name] = p_value;
	canvas->set_tool_options(tool_values[tool]);
}

void TetraImageWorkspace::_change_brush_size(bool p_larger) {
	if (!tool_values[tool].has("size")) {
		return;
	}
	// Ten percent steps, at least one pixel, as GIMP's bracket keys feel.
	const double size = tool_values[tool]["size"];
	const double step = MAX(1.0, Math::round(size * 0.1));
	const TetraImageTools::Info &info = TetraImageTools::get_info(tool);
	set_tool_option(info.name, "size", p_larger ? size + step : size - step);
	_status_message(vformat(TTR("%s size: %s px"), TTRGET(info.label), String::num(double(tool_values[tool]["size"]), 1)));
}

void TetraImageWorkspace::_colors_changed() {
	swatch->set_colors(foreground, background);
	canvas->set_colors(foreground, background);
}

void TetraImageWorkspace::_popup_color_picker(bool p_background) {
	color_popup_for_background = p_background;
	color_picker->set_pick_color(p_background ? background : foreground);
	const Point2 position = swatch->get_screen_position() + Vector2(swatch->get_size().x + 8 * EDSCALE, 0);
	color_popup->popup(Rect2i(position, Size2i()));
}

void TetraImageWorkspace::_color_picker_changed(const Color &p_color) {
	if (color_popup_for_background) {
		set_background_color(p_color);
	} else {
		set_foreground_color(p_color);
	}
}

void TetraImageWorkspace::_color_picked(const Color &p_color, bool p_background) {
	if (p_background) {
		set_background_color(p_color);
	} else {
		set_foreground_color(p_color);
	}
	_status_message(vformat(TTR("Picked #%s as the %s colour."), p_color.to_html(false), p_background ? TTR("background") : TTR("foreground")));
}

void TetraImageWorkspace::_pointer_moved(const Vector2 &p_position) {
	status_pointer->set_text(vformat("%d, %d", int(Math::floor(p_position.x)), int(Math::floor(p_position.y))));
}

void TetraImageWorkspace::_pointer_exited() {
	status_pointer->set_text(String());
}

void TetraImageWorkspace::_status_message(const String &p_text) {
	tool_hint_override = p_text;
	_update_status();
}

bool TetraImageWorkspace::_has_focus_inside() const {
	// A dialog or the inline editor of the layer list has the keyboard.
	const Window *focused_window = Window::get_focused_window();
	if (focused_window && focused_window != get_window()) {
		return false;
	}
	const Control *focus = get_viewport()->gui_get_focus_owner();
	if (!focus || !(focus == this || is_ancestor_of(focus))) {
		return false;
	}
	return !Object::cast_to<LineEdit>(focus) && !Object::cast_to<TextEdit>(focus);
}

void TetraImageWorkspace::_set_busy(bool p_busy, const String &p_text) {
	busy = p_busy;
	busy_label->set_text(p_text);
	busy_shield->set_visible(p_busy);
}

void TetraImageWorkspace::shortcut_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || !is_visible_in_tree() || !_has_focus_inside()) {
		return;
	}
	if (smaller_brush_shortcut->matches_event(p_event)) {
		_change_brush_size(false);
		accept_event();
		return;
	}
	if (larger_brush_shortcut->matches_event(p_event)) {
		_change_brush_size(true);
		accept_event();
		return;
	}
	if (key->is_echo()) {
		return;
	}
	for (const char *name : MENU_NAMES) {
		if (get_menu(name)->activate_item_by_event(p_event, false)) {
			accept_event();
			return;
		}
	}
	for (int i = 0; i < TetraImageTools::TOOL_MAX; i++) {
		if (tool_shortcuts[i]->matches_event(p_event)) {
			set_tool(TetraImageTools::get_info(TetraImageTools::Tool(i)).name);
			accept_event();
			return;
		}
	}
	if (swap_shortcut->matches_event(p_event)) {
		swap_colors();
		accept_event();
	} else if (default_colors_shortcut->matches_event(p_event)) {
		reset_colors();
		accept_event();
	} else if (key->get_keycode() == Key::ESCAPE && canvas->is_tool_active()) {
		canvas->tool_cancel();
		accept_event();
	}
}

void TetraImageWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			for (int i = 0; i < TetraImageTools::TOOL_MAX; i++) {
				tool_buttons[i]->set_button_icon(get_editor_theme_icon(TetraImageTools::get_info(TetraImageTools::Tool(i)).icon));
			}
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible_in_tree()) {
				_load_operations();
				// The keys of the workspace work as soon as it is shown.
				if (document.is_valid()) {
					callable_mp((Control *)canvas, &Control::grab_focus).call_deferred(false);
				} else {
					callable_mp((Control *)welcome_new, &Control::grab_focus).call_deferred(false);
				}
			}
		} break;
	}
}

Ref<TetraImageDocument> TetraImageWorkspace::new_document(int p_width, int p_height, const Color &p_fill) {
	ERR_FAIL_COND_V(p_width < 1 || p_height < 1 || p_width > 16384 || p_height > 16384, Ref<TetraImageDocument>());
	Ref<TetraImageDocument> created;
	created.instantiate();
	ERR_FAIL_COND_V(created->create(p_width, p_height, p_fill.srgb_to_linear()) != OK, Ref<TetraImageDocument>());
	_set_document(created, String());
	return created;
}

Error TetraImageWorkspace::open_file(const String &p_path) {
	Ref<TetraImageDocument> loaded;
	loaded.instantiate();
	const Error err = loaded->load(p_path);
	if (err != OK) {
		return err;
	}
	_set_document(loaded, p_path);
	return OK;
}

void TetraImageWorkspace::request_open_file(const String &p_path) {
	if (document.is_valid() && document_path == p_path) {
		return;
	}
	_confirm_discard(callable_mp(this, &TetraImageWorkspace::_open_checked).bind(p_path));
}

void TetraImageWorkspace::close_document() {
	_set_document(Ref<TetraImageDocument>(), String());
}

bool TetraImageWorkspace::set_tool(const String &p_name) {
	const TetraImageTools::Tool found = TetraImageTools::find(p_name);
	if (found == TetraImageTools::TOOL_MAX) {
		return false;
	}
	if (canvas->is_tool_active()) {
		canvas->tool_cancel();
	}
	tool = found;
	tool_buttons[tool]->set_pressed_no_signal(true);
	canvas->set_tool(tool);
	tool_hint_override = String();
	_update_tool_options();
	_update_status();
	return true;
}

String TetraImageWorkspace::get_tool() const {
	return TetraImageTools::get_info(tool).name;
}

PackedStringArray TetraImageWorkspace::get_tool_names() const {
	PackedStringArray names;
	for (int i = 0; i < TetraImageTools::TOOL_MAX; i++) {
		names.push_back(TetraImageTools::get_info(TetraImageTools::Tool(i)).name);
	}
	return names;
}

Dictionary TetraImageWorkspace::get_tool_options_schema(const String &p_tool) const {
	const TetraImageTools::Tool found = TetraImageTools::find(p_tool);
	ERR_FAIL_COND_V(found == TetraImageTools::TOOL_MAX, Dictionary());
	return tool_schemas[found].duplicate(true);
}

Dictionary TetraImageWorkspace::get_tool_options(const String &p_tool) const {
	const TetraImageTools::Tool found = TetraImageTools::find(p_tool);
	ERR_FAIL_COND_V(found == TetraImageTools::TOOL_MAX, Dictionary());
	return tool_values[found].duplicate();
}

void TetraImageWorkspace::set_tool_option(const String &p_tool, const String &p_name, const Variant &p_value) {
	const TetraImageTools::Tool found = TetraImageTools::find(p_tool);
	ERR_FAIL_COND(found == TetraImageTools::TOOL_MAX);
	const Dictionary param = TetraSchema::find_param(tool_schemas[found], p_name);
	ERR_FAIL_COND_MSG(param.is_empty(), vformat("The tool '%s' has no option '%s'.", p_tool, p_name));
	tool_values[found][p_name] = TetraSchema::sanitize_value(param, p_value);
	if (found == tool) {
		tool_options_panel->set_value(p_name, tool_values[found][p_name]);
		canvas->set_tool_options(tool_values[found]);
	}
}

void TetraImageWorkspace::set_foreground_color(const Color &p_color) {
	foreground = Color(p_color.r, p_color.g, p_color.b, 1.0f);
	_colors_changed();
}

void TetraImageWorkspace::set_background_color(const Color &p_color) {
	background = Color(p_color.r, p_color.g, p_color.b, 1.0f);
	_colors_changed();
}

void TetraImageWorkspace::swap_colors() {
	SWAP(foreground, background);
	_colors_changed();
}

void TetraImageWorkspace::reset_colors() {
	foreground = Color(0, 0, 0, 1);
	background = Color(1, 1, 1, 1);
	_colors_changed();
}

bool TetraImageWorkspace::undo() {
	if (document.is_null() || filter_session || canvas->is_tool_active()) {
		return false;
	}
	const PackedStringArray names = document->get_undo_names();
	if (!document->undo()) {
		return false;
	}
	_status_message(vformat(TTR("Undid: %s"), names[names.size() - 1]));
	return true;
}

bool TetraImageWorkspace::redo() {
	if (document.is_null() || filter_session || canvas->is_tool_active()) {
		return false;
	}
	const PackedStringArray names = document->get_redo_names();
	if (!document->redo()) {
		return false;
	}
	_status_message(vformat(TTR("Redid: %s"), names[names.size() - 1]));
	return true;
}

void TetraImageWorkspace::menu_option(MenuOption p_option) {
	_menu_option(p_option);
}

void TetraImageWorkspace::zoom_to_fit() {
	canvas->zoom_to_fit();
}

void TetraImageWorkspace::set_zoom(real_t p_zoom) {
	canvas->set_zoom_centered(p_zoom);
}

real_t TetraImageWorkspace::get_zoom() const {
	return canvas->get_zoom();
}

PopupMenu *TetraImageWorkspace::get_menu(const String &p_name) const {
	PopupMenu *const menus[] = { file_menu, edit_menu, select_menu, view_menu, image_menu, layer_menu, colors_menu, filters_menu };
	for (int i = 0; i < 8; i++) {
		if (p_name == MENU_NAMES[i]) {
			return menus[i];
		}
	}
	return nullptr;
}

String TetraImageWorkspace::get_status_text() const {
	return String(" | ").join({ status_hint->get_text(), status_pointer->get_text(), status_size->get_text(), status_zoom->get_text() });
}

String TetraImageWorkspace::get_title_text() const {
	return title_label->get_text();
}

void TetraImageWorkspace::_bind_methods() {
	ClassDB::bind_static_method("TetraImageWorkspace", D_METHOD("get_singleton"), &TetraImageWorkspace::get_singleton);

	ClassDB::bind_method(D_METHOD("new_document", "width", "height", "fill"), &TetraImageWorkspace::new_document, DEFVAL(Color(1, 1, 1, 1)));
	ClassDB::bind_method(D_METHOD("open_file", "path"), &TetraImageWorkspace::open_file);
	ClassDB::bind_method(D_METHOD("request_open_file", "path"), &TetraImageWorkspace::request_open_file);
	ClassDB::bind_method(D_METHOD("save"), &TetraImageWorkspace::save);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &TetraImageWorkspace::save_as);
	ClassDB::bind_method(D_METHOD("export_as", "path"), &TetraImageWorkspace::export_as);
	ClassDB::bind_method(D_METHOD("close_document"), &TetraImageWorkspace::close_document);
	ClassDB::bind_method(D_METHOD("get_document"), &TetraImageWorkspace::get_document);
	ClassDB::bind_method(D_METHOD("has_document"), &TetraImageWorkspace::has_document);
	ClassDB::bind_method(D_METHOD("get_document_path"), &TetraImageWorkspace::get_document_path);
	ClassDB::bind_method(D_METHOD("is_modified"), &TetraImageWorkspace::is_modified);
	ClassDB::bind_method(D_METHOD("can_save_directly"), &TetraImageWorkspace::can_save_directly);

	ClassDB::bind_method(D_METHOD("set_tool", "name"), &TetraImageWorkspace::set_tool);
	ClassDB::bind_method(D_METHOD("get_tool"), &TetraImageWorkspace::get_tool);
	ClassDB::bind_method(D_METHOD("get_tool_names"), &TetraImageWorkspace::get_tool_names);
	ClassDB::bind_method(D_METHOD("get_tool_options_schema", "tool"), &TetraImageWorkspace::get_tool_options_schema);
	ClassDB::bind_method(D_METHOD("get_tool_options", "tool"), &TetraImageWorkspace::get_tool_options);
	ClassDB::bind_method(D_METHOD("set_tool_option", "tool", "name", "value"), &TetraImageWorkspace::set_tool_option);

	ClassDB::bind_method(D_METHOD("set_foreground_color", "color"), &TetraImageWorkspace::set_foreground_color);
	ClassDB::bind_method(D_METHOD("get_foreground_color"), &TetraImageWorkspace::get_foreground_color);
	ClassDB::bind_method(D_METHOD("set_background_color", "color"), &TetraImageWorkspace::set_background_color);
	ClassDB::bind_method(D_METHOD("get_background_color"), &TetraImageWorkspace::get_background_color);
	ClassDB::bind_method(D_METHOD("swap_colors"), &TetraImageWorkspace::swap_colors);
	ClassDB::bind_method(D_METHOD("reset_colors"), &TetraImageWorkspace::reset_colors);

	ClassDB::bind_method(D_METHOD("get_operations"), &TetraImageWorkspace::get_operations);
	ClassDB::bind_method(D_METHOD("are_operations_loaded"), &TetraImageWorkspace::are_operations_loaded);
	ClassDB::bind_method(D_METHOD("apply_operation", "id", "values"), &TetraImageWorkspace::apply_operation, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("show_operation_dialog", "id", "values"), &TetraImageWorkspace::show_operation_dialog, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("repeat_last_operation"), &TetraImageWorkspace::repeat_last_operation);
	ClassDB::bind_method(D_METHOD("is_filter_running"), &TetraImageWorkspace::is_filter_running);
	ClassDB::bind_method(D_METHOD("is_preview_shown"), &TetraImageWorkspace::is_preview_shown);

	ClassDB::bind_method(D_METHOD("undo"), &TetraImageWorkspace::undo);
	ClassDB::bind_method(D_METHOD("redo"), &TetraImageWorkspace::redo);
	ClassDB::bind_method(D_METHOD("menu_option", "option"), &TetraImageWorkspace::menu_option);
	ClassDB::bind_method(D_METHOD("zoom_to_fit"), &TetraImageWorkspace::zoom_to_fit);
	ClassDB::bind_method(D_METHOD("set_zoom", "zoom"), &TetraImageWorkspace::set_zoom);
	ClassDB::bind_method(D_METHOD("get_zoom"), &TetraImageWorkspace::get_zoom);

	ClassDB::bind_method(D_METHOD("get_canvas"), &TetraImageWorkspace::get_canvas);
	ClassDB::bind_method(D_METHOD("get_layers_panel"), &TetraImageWorkspace::get_layers_panel);
	ClassDB::bind_method(D_METHOD("get_tool_options_panel"), &TetraImageWorkspace::get_tool_options_panel);
	ClassDB::bind_method(D_METHOD("get_operation_dialog"), &TetraImageWorkspace::get_operation_dialog);
	ClassDB::bind_method(D_METHOD("get_menu_bar"), &TetraImageWorkspace::get_menu_bar);
	ClassDB::bind_method(D_METHOD("get_menu", "name"), &TetraImageWorkspace::get_menu);
	ClassDB::bind_method(D_METHOD("get_status_text"), &TetraImageWorkspace::get_status_text);
	ClassDB::bind_method(D_METHOD("get_title_text"), &TetraImageWorkspace::get_title_text);

	ADD_SIGNAL(MethodInfo("document_changed"));
	ADD_SIGNAL(MethodInfo("operations_loaded"));
	ADD_SIGNAL(MethodInfo("operation_applied", PropertyInfo(Variant::STRING, "id")));

	BIND_ENUM_CONSTANT(FILE_NEW);
	BIND_ENUM_CONSTANT(FILE_OPEN);
	BIND_ENUM_CONSTANT(FILE_SAVE);
	BIND_ENUM_CONSTANT(FILE_SAVE_AS);
	BIND_ENUM_CONSTANT(FILE_EXPORT_AS);
	BIND_ENUM_CONSTANT(FILE_CLOSE);
	BIND_ENUM_CONSTANT(EDIT_UNDO);
	BIND_ENUM_CONSTANT(EDIT_REDO);
	BIND_ENUM_CONSTANT(EDIT_CLEAR);
	BIND_ENUM_CONSTANT(EDIT_FILL_FOREGROUND);
	BIND_ENUM_CONSTANT(EDIT_FILL_BACKGROUND);
	BIND_ENUM_CONSTANT(SELECT_ALL);
	BIND_ENUM_CONSTANT(SELECT_NONE);
	BIND_ENUM_CONSTANT(SELECT_INVERT);
	BIND_ENUM_CONSTANT(SELECT_FEATHER);
	BIND_ENUM_CONSTANT(SELECT_GROW);
	BIND_ENUM_CONSTANT(SELECT_SHRINK);
	BIND_ENUM_CONSTANT(IMAGE_FLIP_HORIZONTAL);
	BIND_ENUM_CONSTANT(IMAGE_FLIP_VERTICAL);
	BIND_ENUM_CONSTANT(IMAGE_ROTATE_90_CW);
	BIND_ENUM_CONSTANT(IMAGE_ROTATE_90_CCW);
	BIND_ENUM_CONSTANT(IMAGE_ROTATE_180);
	BIND_ENUM_CONSTANT(IMAGE_SCALE);
	BIND_ENUM_CONSTANT(IMAGE_CANVAS_SIZE);
	BIND_ENUM_CONSTANT(IMAGE_CROP_TO_SELECTION);
	BIND_ENUM_CONSTANT(IMAGE_FLATTEN);
	BIND_ENUM_CONSTANT(LAYER_NEW);
	BIND_ENUM_CONSTANT(LAYER_DUPLICATE);
	BIND_ENUM_CONSTANT(LAYER_RAISE);
	BIND_ENUM_CONSTANT(LAYER_LOWER);
	BIND_ENUM_CONSTANT(LAYER_MERGE_DOWN);
	BIND_ENUM_CONSTANT(LAYER_DELETE);
	BIND_ENUM_CONSTANT(LAYER_FLIP_HORIZONTAL);
	BIND_ENUM_CONSTANT(LAYER_FLIP_VERTICAL);
	BIND_ENUM_CONSTANT(FILTER_REPEAT_LAST);
	BIND_ENUM_CONSTANT(FILTER_SEARCH);
	BIND_ENUM_CONSTANT(VIEW_ZOOM_IN);
	BIND_ENUM_CONSTANT(VIEW_ZOOM_OUT);
	BIND_ENUM_CONSTANT(VIEW_ZOOM_100);
	BIND_ENUM_CONSTANT(VIEW_ZOOM_FIT);
}

TetraImageWorkspace::TetraImageWorkspace() {
	singleton = this;
	set_process_shortcut_input(true);

	for (int i = 0; i < TetraImageTools::TOOL_MAX; i++) {
		const TetraImageTools::Tool t = TetraImageTools::Tool(i);
		const TetraImageTools::Info &info = TetraImageTools::get_info(t);
		tool_schemas[i] = TetraImageTools::get_options_schema(t);
		tool_values[i] = TetraSchema::get_defaults(tool_schemas[i]);
		tool_shortcuts[i] = ED_SHORTCUT(String("tetra_image/tool_") + String(info.name).replace_char('-', '_'), String("Image: ") + info.label, info.shortcut);
	}
	swap_shortcut = ED_SHORTCUT("tetra_image/swap_colors", TTRC("Image: Swap Colors"), Key::X);
	default_colors_shortcut = ED_SHORTCUT("tetra_image/default_colors", TTRC("Image: Default Colors"), Key::D);
	smaller_brush_shortcut = ED_SHORTCUT("tetra_image/smaller_brush", TTRC("Image: Smaller Brush"), Key::BRACKETLEFT);
	larger_brush_shortcut = ED_SHORTCUT("tetra_image/larger_brush", TTRC("Image: Larger Brush"), Key::BRACKETRIGHT);

	_build_layout();
	_build_menus();
	_build_dialogs();

	_colors_changed();
	canvas->set_tool(tool);
	_update_tool_options();
	_update_title();
	_update_status();
	_update_menus();
}

TetraImageWorkspace::~TetraImageWorkspace() {
	if (filter_task_running) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(filter_task);
	}
	if (operations_task_running) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(operations_task);
	}
	if (filter_waiting) {
		memdelete(filter_waiting);
	}
	if (singleton == this) {
		singleton = nullptr;
	}
}
