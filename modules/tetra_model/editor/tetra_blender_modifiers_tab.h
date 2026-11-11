#pragma once

#include "scene/gui/box_container.h"

class Button;
class Label;
class OptionButton;
class PopupMenu;
class TetraBlenderEditorPlugin;
class TetraParamPanel;
class Tree;

// The Modifiers tab of the Blender dock: the non-destructive modifier stack
// of the selected MeshInstance3D, as in Blender's Properties > Modifiers.
// Every edit goes through TetraBlenderEditorPlugin, which makes it undoable
// and has Blender evaluate the stack.
class TetraBlenderModifiersTab : public VBoxContainer {
	GDCLASS(TetraBlenderModifiersTab, VBoxContainer);

	enum {
		BUTTON_UP,
		BUTTON_DOWN,
		BUTTON_REMOVE,
	};

	TetraBlenderEditorPlugin *plugin = nullptr;

	Label *target_label = nullptr;
	Button *add_button = nullptr;
	PopupMenu *add_menu = nullptr;
	Button *apply_button = nullptr;
	Button *clear_button = nullptr;
	Tree *stack_tree = nullptr;
	Label *info_label = nullptr;
	VBoxContainer *object_rows = nullptr;
	Label *params_note = nullptr;
	TetraParamPanel *param_panel = nullptr;

	int selected_index = -1;
	// The rows the tree shows, to tell a new stack from changed values.
	String shown_rows;
	// What the parameter panel shows: node, index and type.
	String shown_params;
	bool add_menu_wanted = false;
	bool own_change = false;
	bool updating = false;

	void _target_changed();
	void _modifiers_changed(Node *p_node);
	void _modifiers_evaluated(Node *p_node, bool p_ok);
	void _schemas_changed(int p_kind);

	void _add_pressed();
	void _build_add_menu();
	void _popup_add_menu();
	void _add_menu_id_pressed(int p_id);
	void _apply_pressed();
	void _clear_pressed();
	void _stack_item_selected();
	void _stack_item_edited();
	void _stack_button_clicked(Object *p_item, int p_column, int p_id, int p_mouse_button);
	void _param_changed(const String &p_name, const Variant &p_value);
	void _object_selected(int p_item, const String &p_param, OptionButton *p_option);

	void _update_all();
	void _update_stack();
	void _update_params();
	void _update_info();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Selects a row of the stack; its parameters are shown below it.
	void select_modifier(int p_index);
	int get_selected_modifier() const;
	// Opens the "Add Modifier" menu, once Blender has listed the modifiers.
	void popup_add_menu();
	TetraParamPanel *get_param_panel() const;

	TetraBlenderModifiersTab(TetraBlenderEditorPlugin *p_plugin);
};
