#pragma once

#include "scene/gui/dialogs.h"

class CheckBox;
class TetraParamPanel;
class Timer;

// The dialog in which the parameters of one operation are set before it is
// run: a TetraParamPanel with OK, Cancel and Reset, and optionally a "Preview"
// check box for callers that can show the result while the dialog is open.
class TetraOperationDialog : public ConfirmationDialog {
	GDCLASS(TetraOperationDialog, ConfirmationDialog);

	Dictionary operation;
	TetraParamPanel *panel = nullptr;
	CheckBox *preview_check = nullptr;
	Timer *change_timer = nullptr;

	String _get_operation_id() const;
	void _value_changed(const String &p_name, const Variant &p_value);
	void _emit_values_changed();
	void _preview_toggled(bool p_pressed);

protected:
	virtual void ok_pressed() override;
	virtual void cancel_pressed() override;
	virtual void custom_action(const String &p_action) override;

	static void _bind_methods();

public:
	// Shows the dialog for p_operation, with its defaults overridden by p_initial_values.
	void popup_operation(const Dictionary &p_operation, const Dictionary &p_initial_values = Dictionary());
	Dictionary get_operation() const;
	Dictionary get_values() const;
	TetraParamPanel *get_param_panel() const;

	// Shows or hides the "Preview" check box. It starts checked.
	void set_preview_enabled(bool p_enabled);
	bool is_preview_enabled() const;
	void set_preview_checked(bool p_checked);
	// True only while the check box is shown and checked.
	bool is_preview_checked() const;

	TetraOperationDialog();
};
