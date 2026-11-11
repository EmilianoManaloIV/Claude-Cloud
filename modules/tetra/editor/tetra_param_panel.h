#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class ColorPickerButton;
class EditorFileDialog;
class EditorSpinSlider;
class FoldableContainer;
class Label;
class LineEdit;
class OptionButton;
class ScrollContainer;
class TextEdit;

// A form generated from the parameters of one operation of the operation
// schema (docs/ARCHITECTURE.md): one row per parameter with a control that
// suits its type. Parameters with a "group" sit under a heading of that name
// and "advanced" ones in a section that starts folded.
//
// The panel owns the current values. They are always sanitised against the
// operation, so get_values() can be handed to an engine bridge as it is.
class TetraParamPanel : public VBoxContainer {
	GDCLASS(TetraParamPanel, VBoxContainer);

	struct Row {
		String name;
		String type;
		Dictionary param;
		Variant value;

		Label *label = nullptr;
		CheckBox *check_box = nullptr;
		OptionButton *option_button = nullptr;
		LineEdit *line_edit = nullptr;
		TextEdit *text_edit = nullptr;
		ColorPickerButton *color_button = nullptr;
		// "Browse" of a file row, "Randomize" of a seed row.
		Button *action_button = nullptr;
		// One slider for int, float and seed; one per component for vec2 and vec3.
		EditorSpinSlider *spins[3] = { nullptr, nullptr, nullptr };
	};

	Dictionary operation;
	LocalVector<Row> rows;
	HashMap<String, int> row_by_name;

	bool read_only = false;
	bool scrollable = true;
	bool advanced_folded = true;

	ScrollContainer *scroll = nullptr;
	VBoxContainer *content = nullptr;
	FoldableContainer *advanced_section = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	int file_dialog_row = -1;

	void _rebuild();
	void _add_rows(VBoxContainer *p_parent, bool p_advanced);
	Control *_create_row(int p_row);
	Control *_create_editor(int p_row);
	EditorSpinSlider *_create_spin(int p_row, int p_component);
	void _update_row_control(int p_row);
	void _update_read_only();
	void _update_theme_items();

	// Stores a value coming from a control and emits value_changed if it differs from the current one.
	void _commit(int p_row, const Variant &p_value);

	void _bool_toggled(bool p_pressed, int p_row);
	void _spin_changed(double p_value, int p_row, int p_component);
	void _option_selected(int p_item, int p_row);
	void _line_changed(const String &p_text, int p_row);
	void _line_submitted(const String &p_text, int p_row);
	void _line_focus_exited(int p_row);
	void _text_changed(int p_row);
	void _color_changed(const Color &p_color, int p_row);
	void _browse_pressed(int p_row);
	void _file_selected(const String &p_path);
	void _randomize_pressed(int p_row);
	void _advanced_folding_changed(bool p_folded);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Rebuilds the form for p_operation and sets every value to its default.
	void set_operation(const Dictionary &p_operation);
	Dictionary get_operation() const;

	// The value of every parameter, by name.
	Dictionary get_values() const;
	// Sets the given values (sanitised); parameters that are not mentioned keep
	// their value and unknown names are ignored. Does not emit value_changed.
	void set_values(const Dictionary &p_values);
	void set_value(const String &p_name, const Variant &p_value);
	Variant get_value(const String &p_name) const;
	bool has_param(const String &p_name) const;
	void reset_to_defaults();

	void set_read_only(bool p_read_only);
	bool is_read_only() const;

	// A scrollable panel (the default) shows a vertical scroll bar when it is
	// shorter than its form. Turn it off when the panel sits inside something
	// that scrolls already; its minimum height is then the height of the form.
	void set_scrollable(bool p_scrollable);
	bool is_scrollable() const;

	void set_advanced_folded(bool p_folded);
	bool is_advanced_folded() const;

	TetraParamPanel();
};
