#include "tetra_param_panel.h"

#include "../tetra_schema.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/foldable_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/text_edit.h"

namespace {

// Unscaled widths below which a row's label and its control do not shrink.
const int LABEL_MIN_WIDTH = 64;
const int EDITOR_MIN_WIDTH = 120;
const int TEXT_MIN_HEIGHT = 84;

// The step of a float parameter that does not give one; the same as the inspector's default.
const double DEFAULT_FLOAT_STEP = 0.001;

bool get_limit(const Dictionary &p_param, const char *p_key, double &r_limit) {
	const Variant value = p_param.get(p_key, Variant());
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) {
		return false;
	}
	const double number = value;
	if (!Math::is_finite(number)) {
		return false;
	}
	r_limit = number;
	return true;
}

String get_string(const Dictionary &p_dictionary, const char *p_key) {
	const Variant value = p_dictionary.get(p_key, Variant());
	return value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME ? String(value) : String();
}

bool same_value(const Variant &p_a, const Variant &p_b) {
	return p_a.get_type() == p_b.get_type() && p_a == p_b;
}

} // namespace

void TetraParamPanel::_rebuild() {
	file_dialog_row = -1;
	rows.clear();
	row_by_name.clear();
	advanced_section = nullptr;
	while (content->get_child_count() > 0) {
		Node *child = content->get_child(0);
		content->remove_child(child);
		child->queue_free();
	}

	const Variant params_value = operation.get("params", Variant());
	const Array params = params_value.get_type() == Variant::ARRAY ? Array(params_value) : Array();
	bool has_advanced = false;
	for (int i = 0; i < params.size(); i++) {
		if (params[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary param = params[i];
		const String name = get_string(param, "name");
		if (name.is_empty() || row_by_name.has(name)) {
			continue;
		}
		Row row;
		row.name = name;
		row.type = get_string(param, "type");
		row.param = param;
		row.value = TetraSchema::get_param_default(param);
		row_by_name[name] = rows.size();
		rows.push_back(row);
		has_advanced = has_advanced || bool(param.get("advanced", false));
	}

	if (rows.is_empty()) {
		Label *empty = memnew(Label(TTR("This operation has no parameters.")));
		empty->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		empty->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		empty->set_modulate(Color(1, 1, 1, 0.6));
		content->add_child(empty);
		return;
	}

	_add_rows(content, false);
	if (has_advanced) {
		advanced_section = memnew(FoldableContainer);
		advanced_section->set_title(TTR("Advanced"));
		advanced_section->set_title_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		advanced_section->set_folded(advanced_folded);
		advanced_section->connect("folding_changed", callable_mp(this, &TetraParamPanel::_advanced_folding_changed));
		content->add_child(advanced_section);

		VBoxContainer *advanced_rows = memnew(VBoxContainer);
		advanced_section->add_child(advanced_rows);
		_add_rows(advanced_rows, true);
	}

	_update_read_only();
	if (is_inside_tree()) {
		_update_theme_items();
	}
}

void TetraParamPanel::_add_rows(VBoxContainer *p_parent, bool p_advanced) {
	// Parameters without a group come first, then each group under its heading
	// in the order the groups first appear.
	Vector<String> groups;
	groups.push_back(String());
	for (const Row &row : rows) {
		const String group = get_string(row.param, "group");
		if (bool(row.param.get("advanced", false)) == p_advanced && !groups.has(group)) {
			groups.push_back(group);
		}
	}
	for (const String &group : groups) {
		bool heading_added = group.is_empty();
		for (uint32_t i = 0; i < rows.size(); i++) {
			if (bool(rows[i].param.get("advanced", false)) != p_advanced || get_string(rows[i].param, "group") != group) {
				continue;
			}
			if (!heading_added) {
				Label *heading = memnew(Label(group));
				heading->set_theme_type_variation("HeaderSmall");
				heading->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
				heading->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
				p_parent->add_child(heading);
				heading_added = true;
			}
			p_parent->add_child(_create_row(i));
		}
	}
}

Control *TetraParamPanel::_create_row(int p_row) {
	Row &row = rows[p_row];
	String text = get_string(row.param, "label");
	if (text.is_empty()) {
		text = row.name;
	}
	const String description = get_string(row.param, "description");

	HBoxContainer *line = memnew(HBoxContainer);

	row.label = memnew(Label(text));
	row.label->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	row.label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	row.label->set_custom_minimum_size(Size2(LABEL_MIN_WIDTH * EDSCALE, 0));
	row.label->set_h_size_flags(SIZE_EXPAND_FILL);
	row.label->set_stretch_ratio(0.75);
	// Pass, not stop: the label shows a tooltip yet lets the wheel scroll the form.
	row.label->set_mouse_filter(MOUSE_FILTER_PASS);
	row.label->set_tooltip_text(description.is_empty() ? text : description);
	row.label->set_tooltip_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	if (row.type == "text") {
		row.label->set_v_size_flags(SIZE_SHRINK_BEGIN);
	}
	line->add_child(row.label);

	Control *editor = _create_editor(p_row);
	editor->set_h_size_flags(SIZE_EXPAND_FILL);
	editor->set_custom_minimum_size(Size2(EDITOR_MIN_WIDTH * EDSCALE, editor->get_custom_minimum_size().y));
	line->add_child(editor);

	_update_row_control(p_row);
	return line;
}

Control *TetraParamPanel::_create_editor(int p_row) {
	Row &row = rows[p_row];

	if (row.type == "bool") {
		row.check_box = memnew(CheckBox(TTR("On")));
		row.check_box->connect(SceneStringName(toggled), callable_mp(this, &TetraParamPanel::_bool_toggled).bind(p_row));
		return row.check_box;
	}

	if (row.type == "int" || row.type == "float") {
		row.spins[0] = _create_spin(p_row, 0);
		return row.spins[0];
	}

	if (row.type == "seed") {
		HBoxContainer *box = memnew(HBoxContainer);
		row.spins[0] = _create_spin(p_row, 0);
		box->add_child(row.spins[0]);
		row.action_button = memnew(Button);
		row.action_button->set_tooltip_text(TTR("Randomize"));
		row.action_button->set_accessibility_name(TTR("Randomize"));
		row.action_button->connect(SceneStringName(pressed), callable_mp(this, &TetraParamPanel::_randomize_pressed).bind(p_row));
		box->add_child(row.action_button);
		return box;
	}

	if (row.type == "vec2" || row.type == "vec3") {
		HBoxContainer *box = memnew(HBoxContainer);
		const int components = row.type == "vec2" ? 2 : 3;
		for (int i = 0; i < components; i++) {
			row.spins[i] = _create_spin(p_row, i);
			box->add_child(row.spins[i]);
		}
		return box;
	}

	if (row.type == "enum") {
		row.option_button = memnew(OptionButton);
		row.option_button->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
		row.option_button->set_fit_to_longest_item(false);
		row.option_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		const Variant options_value = row.param.get("options", Variant());
		const Array options = options_value.get_type() == Variant::ARRAY ? Array(options_value) : Array();
		for (int i = 0; i < options.size(); i++) {
			if (options[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary option = options[i];
			const Variant option_value = option.get("value", Variant());
			String option_label = get_string(option, "label");
			if (option_label.is_empty()) {
				option_label = option_value;
			}
			row.option_button->add_item(option_label);
			row.option_button->set_item_metadata(row.option_button->get_item_count() - 1, option_value);
		}
		row.option_button->connect(SceneStringName(item_selected), callable_mp(this, &TetraParamPanel::_option_selected).bind(p_row));
		return row.option_button;
	}

	if (row.type == "text") {
		row.text_edit = memnew(TextEdit);
		row.text_edit->set_custom_minimum_size(Size2(0, TEXT_MIN_HEIGHT * EDSCALE));
		row.text_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
		row.text_edit->connect(SceneStringName(text_changed), callable_mp(this, &TetraParamPanel::_text_changed).bind(p_row));
		return row.text_edit;
	}

	if (row.type == "color") {
		row.color_button = memnew(ColorPickerButton);
		row.color_button->set_edit_alpha(true);
		row.color_button->connect("color_changed", callable_mp(this, &TetraParamPanel::_color_changed).bind(p_row));
		return row.color_button;
	}

	if (row.type == "file") {
		HBoxContainer *box = memnew(HBoxContainer);
		row.line_edit = memnew(LineEdit);
		row.line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
		row.line_edit->set_structured_text_bidi_override(TextServer::STRUCTURED_TEXT_FILE);
		// A path is taken when it is complete, not on every keystroke.
		row.line_edit->connect(SceneStringName(text_submitted), callable_mp(this, &TetraParamPanel::_line_submitted).bind(p_row));
		row.line_edit->connect(SceneStringName(focus_exited), callable_mp(this, &TetraParamPanel::_line_focus_exited).bind(p_row));
		box->add_child(row.line_edit);
		row.action_button = memnew(Button);
		row.action_button->set_tooltip_text(TTR("Browse…"));
		row.action_button->set_accessibility_name(TTR("Browse"));
		row.action_button->connect(SceneStringName(pressed), callable_mp(this, &TetraParamPanel::_browse_pressed).bind(p_row));
		box->add_child(row.action_button);
		return box;
	}

	// "string", and any type this version does not know, as a line of text.
	row.line_edit = memnew(LineEdit);
	if (row.type == "string") {
		row.line_edit->connect(SceneStringName(text_changed), callable_mp(this, &TetraParamPanel::_line_changed).bind(p_row));
	} else {
		row.line_edit->set_editable(false);
	}
	return row.line_edit;
}

EditorSpinSlider *TetraParamPanel::_create_spin(int p_row, int p_component) {
	const Row &row = rows[p_row];
	const bool seed = row.type == "seed";
	const bool integer = seed || row.type == "int";

	double hard_min = 0.0;
	double hard_max = 0.0;
	bool has_min = get_limit(row.param, "min", hard_min);
	bool has_max = get_limit(row.param, "max", hard_max);
	if (seed && !has_min) {
		hard_min = 0.0;
		has_min = true;
	}
	if (seed && !has_max) {
		hard_max = TetraSchema::SEED_MAX;
		has_max = true;
	}

	// The slider covers the "ui" range where one is given, never more than the hard limits.
	double slider_min = hard_min;
	double slider_max = hard_max;
	bool has_slider_min = has_min;
	bool has_slider_max = has_max;
	double limit = 0.0;
	if (get_limit(row.param, "ui_min", limit)) {
		slider_min = has_min ? MAX(limit, hard_min) : limit;
		has_slider_min = true;
	}
	if (get_limit(row.param, "ui_max", limit)) {
		slider_max = has_max ? MIN(limit, hard_max) : limit;
		has_slider_max = true;
	}
	if (integer) {
		hard_min = Math::ceil(hard_min);
		hard_max = Math::floor(hard_max);
		slider_min = Math::ceil(slider_min);
		slider_max = Math::floor(slider_max);
	}
	const bool has_slider = !seed && has_slider_min && has_slider_max && slider_max > slider_min;

	double step = integer ? 1.0 : DEFAULT_FLOAT_STEP;
	if (get_limit(row.param, "step", limit) && limit > 0.0) {
		step = integer ? MAX(1.0, Math::round(limit)) : limit;
	}

	EditorSpinSlider *spin = memnew(EditorSpinSlider);
	spin->set_h_size_flags(SIZE_EXPAND_FILL);
	spin->set_editing_integer(integer);
	spin->set_step(step);
	if (has_slider) {
		spin->set_min(slider_min);
		spin->set_max(slider_max);
		// Typing may go past the slider up to the hard limit; _commit() enforces that limit.
		spin->set_allow_lesser(!has_min || hard_min < slider_min);
		spin->set_allow_greater(!has_max || hard_max > slider_max);
		spin->set_control_state(integer ? EditorSpinSlider::CONTROL_STATE_PREFER_SLIDER : EditorSpinSlider::CONTROL_STATE_DEFAULT);
	} else {
		// Without a slider range the Range only anchors the limits that exist, as the inspector does.
		const double anchor = has_min ? hard_min : (has_max ? hard_max : 0.0);
		spin->set_min(has_min ? hard_min : anchor);
		spin->set_max(has_max ? MAX(hard_max, hard_min) : anchor);
		spin->set_allow_lesser(!has_min);
		spin->set_allow_greater(!has_max);
		spin->set_control_state(integer && !seed ? EditorSpinSlider::CONTROL_STATE_DEFAULT : EditorSpinSlider::CONTROL_STATE_HIDE);
	}

	const String unit = get_string(row.param, "unit");
	if (!unit.is_empty()) {
		// A degree or percent sign follows the number directly; other units after a space.
		spin->set_suffix(unit == U"°" || unit == "%" ? unit : " " + unit);
	}
	if (row.type == "vec2" || row.type == "vec3") {
		spin->set_label(String::chr("xyz"[p_component]));
	}
	spin->connect(SceneStringName(value_changed), callable_mp(this, &TetraParamPanel::_spin_changed).bind(p_row, p_component));
	return spin;
}

void TetraParamPanel::_update_row_control(int p_row) {
	const Row &row = rows[p_row];
	if (row.check_box) {
		row.check_box->set_pressed_no_signal(bool(row.value));
	} else if (row.option_button) {
		int selected = -1;
		for (int i = 0; i < row.option_button->get_item_count(); i++) {
			if (same_value(row.option_button->get_item_metadata(i), row.value)) {
				selected = i;
				break;
			}
		}
		row.option_button->select(selected);
	} else if (row.text_edit) {
		const String text = row.value;
		if (row.text_edit->get_text() != text) {
			row.text_edit->set_text(text);
		}
	} else if (row.color_button) {
		row.color_button->set_pick_color(row.value);
	} else if (row.line_edit) {
		// Left alone when it already shows the value, so the caret stays where the user is typing.
		const String text = row.value;
		if (row.line_edit->get_text() != text) {
			row.line_edit->set_text(text);
		}
	} else if (row.type == "vec2") {
		const Vector2 vector = row.value;
		row.spins[0]->set_value_no_signal(vector.x);
		row.spins[1]->set_value_no_signal(vector.y);
	} else if (row.type == "vec3") {
		const Vector3 vector = row.value;
		row.spins[0]->set_value_no_signal(vector.x);
		row.spins[1]->set_value_no_signal(vector.y);
		row.spins[2]->set_value_no_signal(vector.z);
	} else if (row.spins[0]) {
		row.spins[0]->set_value_no_signal(double(row.value));
	}
}

void TetraParamPanel::_update_read_only() {
	for (const Row &row : rows) {
		if (row.check_box) {
			row.check_box->set_disabled(read_only);
		}
		if (row.option_button) {
			row.option_button->set_disabled(read_only);
		}
		if (row.line_edit) {
			row.line_edit->set_editable(!read_only && (row.type == "string" || row.type == "file"));
		}
		if (row.text_edit) {
			row.text_edit->set_editable(!read_only);
		}
		if (row.color_button) {
			row.color_button->set_disabled(read_only);
		}
		if (row.action_button) {
			row.action_button->set_disabled(read_only);
		}
		for (EditorSpinSlider *spin : row.spins) {
			if (spin) {
				spin->set_read_only(read_only);
			}
		}
	}
}

void TetraParamPanel::_update_theme_items() {
	const Color component_colors[3] = {
		get_theme_color(SNAME("property_color_x"), EditorStringName(Editor)),
		get_theme_color(SNAME("property_color_y"), EditorStringName(Editor)),
		get_theme_color(SNAME("property_color_z"), EditorStringName(Editor)),
	};
	for (const Row &row : rows) {
		if (row.action_button) {
			row.action_button->set_button_icon(get_editor_theme_icon(row.type == "seed" ? SNAME("RandomNumberGenerator") : SNAME("FileBrowse")));
		}
		if (row.type == "vec2" || row.type == "vec3") {
			for (int i = 0; i < 3; i++) {
				if (row.spins[i]) {
					row.spins[i]->add_theme_color_override("label_color", component_colors[i]);
				}
			}
		}
	}
}

void TetraParamPanel::_commit(int p_row, const Variant &p_value) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	Row &row = rows[p_row];
	const Variant value = TetraSchema::sanitize_value(row.param, p_value);
	const bool changed = !same_value(value, row.value);
	row.value = value;
	// What the control shows may differ from what was accepted, such as a typed number beyond a hard limit.
	_update_row_control(p_row);
	if (changed) {
		const String name = row.name;
		emit_signal(SNAME("value_changed"), name, value);
	}
}

void TetraParamPanel::_bool_toggled(bool p_pressed, int p_row) {
	_commit(p_row, p_pressed);
}

void TetraParamPanel::_spin_changed(double p_value, int p_row, int p_component) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	const Row &row = rows[p_row];
	if (row.type == "vec2") {
		Vector2 vector = row.value;
		vector[p_component] = p_value;
		_commit(p_row, vector);
	} else if (row.type == "vec3") {
		Vector3 vector = row.value;
		vector[p_component] = p_value;
		_commit(p_row, vector);
	} else if (row.type == "float") {
		_commit(p_row, p_value);
	} else {
		_commit(p_row, int64_t(Math::round(p_value)));
	}
}

void TetraParamPanel::_option_selected(int p_item, int p_row) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	_commit(p_row, rows[p_row].option_button->get_item_metadata(p_item));
}

void TetraParamPanel::_line_changed(const String &p_text, int p_row) {
	_commit(p_row, p_text);
}

void TetraParamPanel::_line_submitted(const String &p_text, int p_row) {
	_commit(p_row, p_text);
}

void TetraParamPanel::_line_focus_exited(int p_row) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	_commit(p_row, rows[p_row].line_edit->get_text());
}

void TetraParamPanel::_text_changed(int p_row) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	_commit(p_row, rows[p_row].text_edit->get_text());
}

void TetraParamPanel::_color_changed(const Color &p_color, int p_row) {
	_commit(p_row, p_color);
}

void TetraParamPanel::_browse_pressed(int p_row) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	const Row &row = rows[p_row];
	if (!file_dialog) {
		file_dialog = memnew(EditorFileDialog);
		file_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
		file_dialog->connect("file_selected", callable_mp(this, &TetraParamPanel::_file_selected));
		file_dialog->connect("dir_selected", callable_mp(this, &TetraParamPanel::_file_selected));
		add_child(file_dialog);
	}
	file_dialog_row = p_row;

	// "file_mode" and "filters" are optional hints of a file parameter; see the README.
	const String mode = get_string(row.param, "file_mode");
	if (mode == "save") {
		file_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	} else if (mode == "dir") {
		file_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_DIR);
	} else {
		file_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
	}
	file_dialog->clear_filters();
	const Variant filters_value = row.param.get("filters", Variant());
	if (filters_value.get_type() == Variant::ARRAY || filters_value.get_type() == Variant::PACKED_STRING_ARRAY) {
		const Array filters = filters_value;
		for (int i = 0; i < filters.size(); i++) {
			if (filters[i].get_type() == Variant::STRING) {
				file_dialog->add_filter(filters[i]);
			}
		}
	}
	const String current = row.value;
	if (!current.is_empty()) {
		file_dialog->set_current_path(current);
	}
	file_dialog->popup_file_dialog();
}

void TetraParamPanel::_file_selected(const String &p_path) {
	if (file_dialog_row >= 0 && file_dialog_row < int(rows.size())) {
		_commit(file_dialog_row, p_path);
	}
}

void TetraParamPanel::_randomize_pressed(int p_row) {
	ERR_FAIL_INDEX(p_row, int(rows.size()));
	const Row &row = rows[p_row];
	int64_t low = 0;
	int64_t high = int64_t(TetraSchema::SEED_MAX);
	double limit = 0.0;
	if (get_limit(row.param, "min", limit)) {
		low = int64_t(Math::ceil(CLAMP(limit, -4.0e18, 4.0e18)));
	}
	if (get_limit(row.param, "max", limit)) {
		high = int64_t(Math::floor(CLAMP(limit, -4.0e18, 4.0e18)));
	}
	if (high < low) {
		high = low;
	}
	const uint64_t span = uint64_t(high - low) + 1;
	const uint64_t random = (uint64_t(Math::rand()) << 32) | uint64_t(Math::rand());
	_commit(p_row, low + int64_t(random % span));
}

void TetraParamPanel::_advanced_folding_changed(bool p_folded) {
	advanced_folded = p_folded;
}

void TetraParamPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_theme_items();
		} break;
	}
}

void TetraParamPanel::set_operation(const Dictionary &p_operation) {
	operation = p_operation;
	_rebuild();
}

Dictionary TetraParamPanel::get_operation() const {
	return operation;
}

Dictionary TetraParamPanel::get_values() const {
	Dictionary values;
	for (const Row &row : rows) {
		values[row.name] = row.value;
	}
	return values;
}

void TetraParamPanel::set_values(const Dictionary &p_values) {
	for (uint32_t i = 0; i < rows.size(); i++) {
		if (p_values.has(rows[i].name)) {
			rows[i].value = TetraSchema::sanitize_value(rows[i].param, p_values[rows[i].name]);
			_update_row_control(i);
		}
	}
}

void TetraParamPanel::set_value(const String &p_name, const Variant &p_value) {
	const HashMap<String, int>::ConstIterator found = row_by_name.find(p_name);
	ERR_FAIL_COND_MSG(!found, vformat("The operation has no parameter '%s'.", p_name));
	rows[found->value].value = TetraSchema::sanitize_value(rows[found->value].param, p_value);
	_update_row_control(found->value);
}

Variant TetraParamPanel::get_value(const String &p_name) const {
	const HashMap<String, int>::ConstIterator found = row_by_name.find(p_name);
	ERR_FAIL_COND_V_MSG(!found, Variant(), vformat("The operation has no parameter '%s'.", p_name));
	return rows[found->value].value;
}

bool TetraParamPanel::has_param(const String &p_name) const {
	return row_by_name.has(p_name);
}

void TetraParamPanel::reset_to_defaults() {
	for (uint32_t i = 0; i < rows.size(); i++) {
		rows[i].value = TetraSchema::get_param_default(rows[i].param);
		_update_row_control(i);
	}
}

void TetraParamPanel::set_read_only(bool p_read_only) {
	read_only = p_read_only;
	_update_read_only();
}

bool TetraParamPanel::is_read_only() const {
	return read_only;
}

void TetraParamPanel::set_scrollable(bool p_scrollable) {
	scrollable = p_scrollable;
	scroll->set_vertical_scroll_mode(scrollable ? ScrollContainer::SCROLL_MODE_AUTO : ScrollContainer::SCROLL_MODE_DISABLED);
}

bool TetraParamPanel::is_scrollable() const {
	return scrollable;
}

void TetraParamPanel::set_advanced_folded(bool p_folded) {
	advanced_folded = p_folded;
	if (advanced_section) {
		advanced_section->set_folded(p_folded);
	}
}

bool TetraParamPanel::is_advanced_folded() const {
	return advanced_folded;
}

void TetraParamPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_operation", "operation"), &TetraParamPanel::set_operation);
	ClassDB::bind_method(D_METHOD("get_operation"), &TetraParamPanel::get_operation);
	ClassDB::bind_method(D_METHOD("get_values"), &TetraParamPanel::get_values);
	ClassDB::bind_method(D_METHOD("set_values", "values"), &TetraParamPanel::set_values);
	ClassDB::bind_method(D_METHOD("set_value", "name", "value"), &TetraParamPanel::set_value);
	ClassDB::bind_method(D_METHOD("get_value", "name"), &TetraParamPanel::get_value);
	ClassDB::bind_method(D_METHOD("has_param", "name"), &TetraParamPanel::has_param);
	ClassDB::bind_method(D_METHOD("reset_to_defaults"), &TetraParamPanel::reset_to_defaults);
	ClassDB::bind_method(D_METHOD("set_read_only", "read_only"), &TetraParamPanel::set_read_only);
	ClassDB::bind_method(D_METHOD("is_read_only"), &TetraParamPanel::is_read_only);
	ClassDB::bind_method(D_METHOD("set_scrollable", "scrollable"), &TetraParamPanel::set_scrollable);
	ClassDB::bind_method(D_METHOD("is_scrollable"), &TetraParamPanel::is_scrollable);
	ClassDB::bind_method(D_METHOD("set_advanced_folded", "folded"), &TetraParamPanel::set_advanced_folded);
	ClassDB::bind_method(D_METHOD("is_advanced_folded"), &TetraParamPanel::is_advanced_folded);

	ADD_SIGNAL(MethodInfo("value_changed", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

TetraParamPanel::TetraParamPanel() {
	scroll = memnew(ScrollContainer);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(scroll);

	content = memnew(VBoxContainer);
	content->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->add_child(content);
}
