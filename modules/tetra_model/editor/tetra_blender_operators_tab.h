#pragma once

#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"

class Button;
class Label;
class TetraBlenderEditorPlugin;
class TetraOperationBrowser;
class TetraParamPanel;

// The Operators tab of the Blender dock: every mesh and UV operator Blender
// can run without a viewport, searchable, with the parameters of the chosen
// one. "Run" applies it to the selected MeshInstance3D with the whole mesh
// selected; afterwards the values can be changed and the operator run again
// on the mesh it started from, like Blender's "Adjust Last Operation".
class TetraBlenderOperatorsTab : public VBoxContainer {
	GDCLASS(TetraBlenderOperatorsTab, VBoxContainer);

	TetraBlenderEditorPlugin *plugin = nullptr;

	Label *target_label = nullptr;
	TetraOperationBrowser *browser = nullptr;
	Label *operation_label = nullptr;
	TetraParamPanel *param_panel = nullptr;
	Button *run_button = nullptr;
	Button *rerun_button = nullptr;
	Label *status_label = nullptr;

	String shown_id;
	// The values each operator was last left with.
	HashMap<String, Dictionary> remembered_values;

	void _schemas_changed(int p_kind);
	void _operation_selected(const String &p_id);
	void _operation_activated(const String &p_id);
	void _run_pressed();
	void _rerun_pressed();
	void _operator_started();
	void _operator_finished(Node *p_node, bool p_ok, const String &p_message);
	void _show_operation(const String &p_id);
	void _set_status(const String &p_text, bool p_error = false);
	void _update_buttons();
	Dictionary _changed_values() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Selects an operator ("blender:mesh.bevel") and shows its parameters.
	void select_operator(const String &p_id);
	String get_selected_operator() const;
	// Runs the selected operator with the values of the panel on the selected node.
	void run();
	TetraOperationBrowser *get_browser() const;
	TetraParamPanel *get_param_panel() const;

	TetraBlenderOperatorsTab(TetraBlenderEditorPlugin *p_plugin);
};
