#include "tetra_operation_dialog.h"

#include "tetra_param_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/main/timer.h"
#include "servers/display/display_server.h"

namespace {

// How long after the last edit values_changed is emitted.
const double CHANGE_DELAY = 0.15;

const int DIALOG_WIDTH = 460;

// The share of the screen the dialog may take before its form starts to scroll.
const float MAX_SCREEN_RATIO = 0.8;

} // namespace

String TetraOperationDialog::_get_operation_id() const {
	const Variant id = operation.get("id", Variant());
	return id.get_type() == Variant::STRING ? String(id) : String();
}

void TetraOperationDialog::_value_changed(const String &p_name, const Variant &p_value) {
	change_timer->start(CHANGE_DELAY);
}

void TetraOperationDialog::_emit_values_changed() {
	emit_signal(SNAME("values_changed"), panel->get_values());
}

void TetraOperationDialog::_preview_toggled(bool p_pressed) {
	emit_signal(SNAME("preview_toggled"), p_pressed);
}

void TetraOperationDialog::ok_pressed() {
	change_timer->stop();
	emit_signal(SNAME("operation_confirmed"), _get_operation_id(), panel->get_values());
}

void TetraOperationDialog::cancel_pressed() {
	change_timer->stop();
	emit_signal(SNAME("operation_cancelled"), _get_operation_id());
}

void TetraOperationDialog::custom_action(const String &p_action) {
	if (p_action == "reset") {
		panel->reset_to_defaults();
		change_timer->start(CHANGE_DELAY);
	}
}

void TetraOperationDialog::popup_operation(const Dictionary &p_operation, const Dictionary &p_initial_values) {
	ERR_FAIL_COND_MSG(!is_inside_tree(), "The dialog must be in the scene tree before it is shown.");
	change_timer->stop();
	operation = p_operation;

	const Variant label = operation.get("label", Variant());
	set_title(label.get_type() == Variant::STRING && !String(label).is_empty() ? String(label) : _get_operation_id());
	panel->set_operation(operation);
	panel->set_values(p_initial_values);

	// The panel scrolls, so its own minimum height is small; the dialog is made
	// tall enough for the whole form where the screen allows it.
	const Size2 chrome = get_contents_minimum_size();
	const Size2 form = panel->get_form_minimum_size();
	const Size2 wanted(MAX(chrome.x, MAX(form.x, DIALOG_WIDTH * EDSCALE)), chrome.y + form.y + 8 * EDSCALE);
	popup_centered_clamped(wanted, MAX_SCREEN_RATIO);
}

Dictionary TetraOperationDialog::get_operation() const {
	return operation;
}

Dictionary TetraOperationDialog::get_values() const {
	return panel->get_values();
}

TetraParamPanel *TetraOperationDialog::get_param_panel() const {
	return panel;
}

void TetraOperationDialog::set_preview_enabled(bool p_enabled) {
	preview_check->set_visible(p_enabled);
}

bool TetraOperationDialog::is_preview_enabled() const {
	return preview_check->is_visible();
}

void TetraOperationDialog::set_preview_checked(bool p_checked) {
	preview_check->set_pressed_no_signal(p_checked);
}

bool TetraOperationDialog::is_preview_checked() const {
	return preview_check->is_visible() && preview_check->is_pressed();
}

void TetraOperationDialog::_bind_methods() {
	ClassDB::bind_method(D_METHOD("popup_operation", "operation", "initial_values"), &TetraOperationDialog::popup_operation, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("get_operation"), &TetraOperationDialog::get_operation);
	ClassDB::bind_method(D_METHOD("get_values"), &TetraOperationDialog::get_values);
	ClassDB::bind_method(D_METHOD("get_param_panel"), &TetraOperationDialog::get_param_panel);
	ClassDB::bind_method(D_METHOD("set_preview_enabled", "enabled"), &TetraOperationDialog::set_preview_enabled);
	ClassDB::bind_method(D_METHOD("is_preview_enabled"), &TetraOperationDialog::is_preview_enabled);
	ClassDB::bind_method(D_METHOD("set_preview_checked", "checked"), &TetraOperationDialog::set_preview_checked);
	ClassDB::bind_method(D_METHOD("is_preview_checked"), &TetraOperationDialog::is_preview_checked);

	ADD_SIGNAL(MethodInfo("values_changed", PropertyInfo(Variant::DICTIONARY, "values")));
	ADD_SIGNAL(MethodInfo("preview_toggled", PropertyInfo(Variant::BOOL, "checked")));
	ADD_SIGNAL(MethodInfo("operation_confirmed", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::DICTIONARY, "values")));
	ADD_SIGNAL(MethodInfo("operation_cancelled", PropertyInfo(Variant::STRING, "id")));
}

TetraOperationDialog::TetraOperationDialog() {
	VBoxContainer *layout = memnew(VBoxContainer);
	add_child(layout);

	panel = memnew(TetraParamPanel);
	panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	panel->connect("value_changed", callable_mp(this, &TetraOperationDialog::_value_changed));
	layout->add_child(panel);

	preview_check = memnew(CheckBox(TTR("Preview")));
	preview_check->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	preview_check->set_pressed_no_signal(true);
	preview_check->hide();
	preview_check->connect(SceneStringName(toggled), callable_mp(this, &TetraOperationDialog::_preview_toggled));
	layout->add_child(preview_check);

	// Reset goes to the far side from OK, so that it is not hit by accident.
	add_button(TTR("Reset"), DisplayServer::get_singleton()->get_swap_cancel_ok(), "reset");

	change_timer = memnew(Timer);
	change_timer->set_one_shot(true);
	change_timer->connect("timeout", callable_mp(this, &TetraOperationDialog::_emit_values_changed));
	add_child(change_timer);
}
