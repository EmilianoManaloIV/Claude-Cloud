#include "tetra_blender_operators_tab.h"

#include "tetra_blender_editor_plugin.h"

#include "tetra/editor/tetra_operation_browser.h"
#include "tetra/editor/tetra_param_panel.h"
#include "tetra/tetra_schema.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/split_container.h"

void TetraBlenderOperatorsTab::_schemas_changed(int p_kind) {
	if (p_kind != TetraBlenderEditorPlugin::SCHEMA_OPERATORS) {
		return;
	}
	switch (plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_OPERATORS)) {
		case TetraBlenderEditorPlugin::SCHEMA_LOADED: {
			const Array operators = plugin->get_schemas(TetraBlenderEditorPlugin::SCHEMA_OPERATORS);
			browser->set_operations(operators);
			_set_status(vformat(TTR("%d Blender operators."), operators.size()));
		} break;
		case TetraBlenderEditorPlugin::SCHEMA_FAILED: {
			_set_status(TTR("The operators are listed by Blender, which could not be started."), true);
		} break;
		default: {
			_set_status(TTR("Asking Blender for its operators…"));
		} break;
	}
	_update_buttons();
}

void TetraBlenderOperatorsTab::_operation_selected(const String &p_id) {
	_show_operation(p_id);
}

void TetraBlenderOperatorsTab::_operation_activated(const String &p_id) {
	_show_operation(p_id);
	run();
}

void TetraBlenderOperatorsTab::_show_operation(const String &p_id) {
	if (!shown_id.is_empty()) {
		remembered_values[shown_id] = param_panel->get_values();
	}
	const Dictionary schema = plugin->find_schema(TetraBlenderEditorPlugin::SCHEMA_OPERATORS, p_id);
	shown_id = schema.is_empty() ? String() : p_id;
	if (schema.is_empty()) {
		operation_label->set_text(TTR("Choose an operator above."));
		operation_label->set_tooltip_text(String());
		param_panel->hide();
	} else {
		operation_label->set_text(schema.get("label", String()));
		operation_label->set_tooltip_text(schema.get("description", String()));
		param_panel->set_operation(schema);
		HashMap<String, Dictionary>::ConstIterator remembered = remembered_values.find(p_id);
		if (remembered) {
			param_panel->set_values(remembered->value);
		}
		param_panel->show();
	}
	_update_buttons();
}

Dictionary TetraBlenderOperatorsTab::_changed_values() const {
	return TetraBlenderEditorPlugin::get_changed_values(param_panel->get_operation(), param_panel->get_values());
}

void TetraBlenderOperatorsTab::_run_pressed() {
	run();
}

void TetraBlenderOperatorsTab::run() {
	MeshInstance3D *target = plugin->get_target();
	if (!target || shown_id.is_empty()) {
		return;
	}
	if (!plugin->run_operator(target, shown_id, _changed_values())) {
		_set_status(plugin->get_last_error(), true);
	}
}

void TetraBlenderOperatorsTab::_rerun_pressed() {
	if (!plugin->rerun_last_operator(_changed_values())) {
		_set_status(TTR("The mesh is no longer the result of the last operator."), true);
		_update_buttons();
	}
}

void TetraBlenderOperatorsTab::_operator_started() {
	_set_status(TTR("Blender is working…"));
	_update_buttons();
}

void TetraBlenderOperatorsTab::_operator_finished(Node *p_node, bool p_ok, const String &p_message) {
	if (p_ok) {
		const String id = plugin->get_last_operation().get("id", String());
		const String label = plugin->find_schema(TetraBlenderEditorPlugin::SCHEMA_OPERATORS, id).get("label", id);
		_set_status(vformat(TTR("%s applied. Change the values and re-run to adjust it."), label));
	} else {
		_set_status(p_message, true);
	}
	_update_buttons();
}

void TetraBlenderOperatorsTab::_set_status(const String &p_text, bool p_error) {
	status_label->set_text(p_text);
	status_label->set_tooltip_text(p_text);
	if (p_error && is_inside_tree()) {
		status_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
	} else {
		status_label->remove_theme_color_override(SceneStringName(font_color));
	}
}

void TetraBlenderOperatorsTab::_update_buttons() {
	MeshInstance3D *target = plugin->get_target();
	if (target) {
		target_label->set_text(String(target->get_name()));
	} else {
		target_label->set_text(TTR("Select a MeshInstance3D to run operators on its mesh."));
	}
	const bool running = plugin->is_operator_running();
	run_button->set_disabled(!target || shown_id.is_empty() || running);
	// Adjusting is offered while the mesh still is the last result and the
	// panel shows the operator that produced it.
	const bool adjustable = plugin->can_rerun_last_operator() && String(plugin->get_last_operation().get("id", String())) == shown_id;
	rerun_button->set_disabled(!adjustable);
}

void TetraBlenderOperatorsTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_VISIBILITY_CHANGED: {
			// Looking at the tab is what asks Blender for the list.
			if (is_visible_in_tree() && plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_OPERATORS) != TetraBlenderEditorPlugin::SCHEMA_LOADED) {
				plugin->load_schemas(TetraBlenderEditorPlugin::SCHEMA_OPERATORS);
			}
			_update_buttons();
		} break;
		case NOTIFICATION_READY: {
			// Undo and redo change whether the last operator can still be adjusted.
			EditorUndoRedoManager::get_singleton()->connect("version_changed", callable_mp(this, &TetraBlenderOperatorsTab::_update_buttons));
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			run_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
			rerun_button->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
		} break;
	}
}

void TetraBlenderOperatorsTab::select_operator(const String &p_id) {
	browser->select_operation(p_id);
	_show_operation(browser->get_selected_id());
}

String TetraBlenderOperatorsTab::get_selected_operator() const {
	return shown_id;
}

TetraOperationBrowser *TetraBlenderOperatorsTab::get_browser() const {
	return browser;
}

TetraParamPanel *TetraBlenderOperatorsTab::get_param_panel() const {
	return param_panel;
}

void TetraBlenderOperatorsTab::_bind_methods() {
	ClassDB::bind_method(D_METHOD("select_operator", "id"), &TetraBlenderOperatorsTab::select_operator);
	ClassDB::bind_method(D_METHOD("get_selected_operator"), &TetraBlenderOperatorsTab::get_selected_operator);
	ClassDB::bind_method(D_METHOD("run"), &TetraBlenderOperatorsTab::run);
	ClassDB::bind_method(D_METHOD("get_browser"), &TetraBlenderOperatorsTab::get_browser);
	ClassDB::bind_method(D_METHOD("get_param_panel"), &TetraBlenderOperatorsTab::get_param_panel);
}

TetraBlenderOperatorsTab::TetraBlenderOperatorsTab(TetraBlenderEditorPlugin *p_plugin) {
	plugin = p_plugin;

	target_label = memnew(Label);
	target_label->set_theme_type_variation("HeaderSmall");
	target_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(target_label);

	VSplitContainer *split = memnew(VSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split);

	browser = memnew(TetraOperationBrowser);
	browser->set_v_size_flags(SIZE_EXPAND_FILL);
	browser->set_custom_minimum_size(Size2(0, 180) * EDSCALE);
	browser->connect("operation_selected", callable_mp(this, &TetraBlenderOperatorsTab::_operation_selected));
	browser->connect("operation_activated", callable_mp(this, &TetraBlenderOperatorsTab::_operation_activated));
	split->add_child(browser);

	VBoxContainer *lower = memnew(VBoxContainer);
	lower->set_v_size_flags(SIZE_EXPAND_FILL);
	lower->set_custom_minimum_size(Size2(0, 200) * EDSCALE);
	split->add_child(lower);

	operation_label = memnew(Label);
	operation_label->set_theme_type_variation("HeaderSmall");
	operation_label->set_text(TTRC("Choose an operator above."));
	operation_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	operation_label->set_mouse_filter(MOUSE_FILTER_PASS);
	lower->add_child(operation_label);

	param_panel = memnew(TetraParamPanel);
	param_panel->set_v_size_flags(SIZE_EXPAND_FILL);
	param_panel->hide();
	lower->add_child(param_panel);

	Control *spacer = memnew(Control);
	spacer->set_v_size_flags(SIZE_EXPAND);
	spacer->set_stretch_ratio(0.01);
	lower->add_child(spacer);

	HBoxContainer *buttons = memnew(HBoxContainer);
	lower->add_child(buttons);
	run_button = memnew(Button);
	run_button->set_text(TTRC("Run"));
	run_button->set_h_size_flags(SIZE_EXPAND_FILL);
	run_button->set_tooltip_text(TTRC("Apply the operator to the selected node's mesh, with the whole mesh selected."));
	run_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderOperatorsTab::_run_pressed));
	buttons->add_child(run_button);
	rerun_button = memnew(Button);
	rerun_button->set_text(TTRC("Re-run with New Values"));
	rerun_button->set_h_size_flags(SIZE_EXPAND_FILL);
	rerun_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	rerun_button->set_tooltip_text(TTRC("Adjust the last operation: run it again with the values above on the mesh it started from."));
	rerun_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderOperatorsTab::_rerun_pressed));
	buttons->add_child(rerun_button);

	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_mouse_filter(MOUSE_FILTER_PASS);
	lower->add_child(status_label);

	plugin->connect("schemas_changed", callable_mp(this, &TetraBlenderOperatorsTab::_schemas_changed));
	plugin->connect("target_changed", callable_mp(this, &TetraBlenderOperatorsTab::_update_buttons));
	plugin->connect("operator_started", callable_mp(this, &TetraBlenderOperatorsTab::_operator_started));
	plugin->connect("operator_finished", callable_mp(this, &TetraBlenderOperatorsTab::_operator_finished));
}
