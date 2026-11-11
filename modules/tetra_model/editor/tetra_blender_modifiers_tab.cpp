#include "tetra_blender_modifiers_tab.h"

#include "tetra_blender_editor_plugin.h"

#include "tetra/editor/tetra_param_panel.h"
#include "tetra/tetra_schema.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/tree.h"

static void _collect_mesh_instances(Node *p_node, Vector<MeshInstance3D *> &r_nodes) {
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		r_nodes.push_back(mesh_instance);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_mesh_instances(p_node->get_child(i), r_nodes);
	}
}

void TetraBlenderModifiersTab::_target_changed() {
	selected_index = 0;
	_update_all();
}

void TetraBlenderModifiersTab::_modifiers_changed(Node *p_node) {
	if (p_node != plugin->get_target()) {
		return;
	}
	_update_stack();
	// While the user edits a value the panel already shows it; rebuilding
	// the form would take the slider out of their hand.
	if (!own_change) {
		_update_params();
	}
	_update_info();
}

void TetraBlenderModifiersTab::_modifiers_evaluated(Node *p_node, bool p_ok) {
	if (p_node == plugin->get_target()) {
		_update_info();
	}
}

void TetraBlenderModifiersTab::_schemas_changed(int p_kind) {
	if (p_kind != TetraBlenderEditorPlugin::SCHEMA_MODIFIERS) {
		return;
	}
	const TetraBlenderEditorPlugin::SchemaState state = plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS);
	if (state == TetraBlenderEditorPlugin::SCHEMA_LOADED) {
		_build_add_menu();
		if (add_menu_wanted && is_visible_in_tree()) {
			_popup_add_menu();
		}
	}
	if (state != TetraBlenderEditorPlugin::SCHEMA_LOADING) {
		add_menu_wanted = false;
	}
	_update_all();
}

void TetraBlenderModifiersTab::_add_pressed() {
	if (plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS) == TetraBlenderEditorPlugin::SCHEMA_LOADED) {
		_popup_add_menu();
		return;
	}
	// Blender has to list its modifiers first; the menu opens when it has.
	add_menu_wanted = true;
	plugin->load_schemas(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS);
	_update_stack();
}

void TetraBlenderModifiersTab::_build_add_menu() {
	add_menu->clear();
	// One submenu per heading of Blender's own Add Modifier menu, in the
	// order Blender lists them.
	HashMap<String, PopupMenu *> submenus;
	const Array schemas = plugin->get_schemas(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS);
	for (int i = 0; i < schemas.size(); i++) {
		const Dictionary schema = schemas[i];
		const String category = String(schema.get("category", String())).get_slice("/", 0).strip_edges();
		PopupMenu *menu = add_menu;
		if (!category.is_empty()) {
			if (!submenus.has(category)) {
				PopupMenu *submenu = memnew(PopupMenu);
				submenu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraBlenderModifiersTab::_add_menu_id_pressed));
				add_menu->add_submenu_node_item(category, submenu);
				submenus[category] = submenu;
			}
			menu = submenus[category];
		}
		menu->add_item(schema.get("label", String()), i);
		menu->set_item_tooltip(-1, schema.get("description", String()));
	}
}

void TetraBlenderModifiersTab::_popup_add_menu() {
	add_menu->reset_size();
	add_menu->set_position(add_button->get_screen_position() + Vector2(0, add_button->get_size().y));
	add_menu->popup();
}

void TetraBlenderModifiersTab::_add_menu_id_pressed(int p_id) {
	MeshInstance3D *target = plugin->get_target();
	const Array schemas = plugin->get_schemas(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS);
	if (!target || p_id < 0 || p_id >= schemas.size()) {
		return;
	}
	selected_index = plugin->get_modifier_stack(target).size();
	plugin->add_modifier(target, Dictionary(schemas[p_id]).get("id", String()));
}

void TetraBlenderModifiersTab::_apply_pressed() {
	if (MeshInstance3D *target = plugin->get_target()) {
		plugin->apply_modifiers(target);
	}
}

void TetraBlenderModifiersTab::_clear_pressed() {
	if (MeshInstance3D *target = plugin->get_target()) {
		plugin->clear_modifiers(target);
	}
}

void TetraBlenderModifiersTab::_stack_item_selected() {
	TreeItem *item = stack_tree->get_selected();
	if (updating || !item) {
		return;
	}
	selected_index = item->get_index();
	_update_params();
}

void TetraBlenderModifiersTab::_stack_item_edited() {
	TreeItem *item = stack_tree->get_edited();
	MeshInstance3D *target = plugin->get_target();
	if (updating || !item || !target) {
		return;
	}
	plugin->set_modifier_enabled(target, item->get_index(), item->is_checked(0));
}

void TetraBlenderModifiersTab::_stack_button_clicked(Object *p_item, int p_column, int p_id, int p_mouse_button) {
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	MeshInstance3D *target = plugin->get_target();
	if (!item || !target || p_mouse_button != int(MouseButton::LEFT)) {
		return;
	}
	const int index = item->get_index();
	switch (p_id) {
		case BUTTON_UP: {
			selected_index = index - 1;
			plugin->move_modifier(target, index, index - 1);
		} break;
		case BUTTON_DOWN: {
			selected_index = index + 1;
			plugin->move_modifier(target, index, index + 1);
		} break;
		case BUTTON_REMOVE: {
			if (selected_index > index) {
				selected_index--;
			}
			plugin->remove_modifier(target, index);
		} break;
	}
}

void TetraBlenderModifiersTab::_param_changed(const String &p_name, const Variant &p_value) {
	MeshInstance3D *target = plugin->get_target();
	if (!target || selected_index < 0) {
		return;
	}
	Dictionary values;
	values[p_name] = p_value;
	own_change = true;
	plugin->set_modifier_values(target, selected_index, values);
	own_change = false;
}

void TetraBlenderModifiersTab::_object_selected(int p_item, const String &p_param, OptionButton *p_option) {
	MeshInstance3D *target = plugin->get_target();
	if (target && selected_index >= 0) {
		plugin->set_modifier_object(target, selected_index, p_param, p_option->get_item_metadata(p_item));
	}
}

void TetraBlenderModifiersTab::_update_all() {
	_update_stack();
	_update_params();
	_update_info();
}

void TetraBlenderModifiersTab::_update_stack() {
	if (!is_inside_tree()) {
		return;
	}
	MeshInstance3D *target = plugin->get_target();
	const Array stack = target ? plugin->get_modifier_stack(target) : Array();
	selected_index = stack.is_empty() ? -1 : CLAMP(selected_index, 0, stack.size() - 1);

	if (target) {
		target_label->set_text(String(target->get_name()));
		target_label->set_tooltip_text(String(EditorNode::get_singleton()->get_edited_scene() ? EditorNode::get_singleton()->get_edited_scene()->get_path_to(target) : NodePath()));
	} else {
		target_label->set_text(TTR("Select a MeshInstance3D to edit its modifiers."));
		target_label->set_tooltip_text(String());
	}
	const bool loading = add_menu_wanted && plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS) == TetraBlenderEditorPlugin::SCHEMA_LOADING;
	add_button->set_text(loading ? TTR("Starting Blender…") : TTR("Add Modifier"));
	add_button->set_disabled(loading || !target || target->get_mesh().is_null());
	clear_button->set_disabled(stack.is_empty());

	String rows = target ? itos(target->get_instance_id()) : String();
	for (int i = 0; i < stack.size(); i++) {
		const Dictionary modifier = stack[i];
		rows += "|" + String(modifier.get("type", String())) + ":" + plugin->get_modifier_label(modifier);
	}

	updating = true;
	if (rows != shown_rows) {
		shown_rows = rows;
		stack_tree->clear();
		TreeItem *root = stack_tree->create_item();
		for (int i = 0; i < stack.size(); i++) {
			const Dictionary modifier = stack[i];
			TreeItem *item = stack_tree->create_item(root);
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_editable(0, true);
			item->set_text(0, plugin->get_modifier_label(modifier));
			item->set_tooltip_text(0, vformat(TTR("Blender modifier %s. The check box turns it on and off."), String(modifier.get("type", String()))));
			item->add_button(0, get_editor_theme_icon(SNAME("MoveUp")), BUTTON_UP, i == 0, TTR("Move up: evaluate earlier"));
			item->add_button(0, get_editor_theme_icon(SNAME("MoveDown")), BUTTON_DOWN, i == stack.size() - 1, TTR("Move down: evaluate later"));
			item->add_button(0, get_editor_theme_icon(SNAME("Remove")), BUTTON_REMOVE, false, TTR("Remove this modifier"));
		}
	}
	TreeItem *root = stack_tree->get_root();
	for (int i = 0; root && i < stack.size(); i++) {
		TreeItem *item = root->get_child(i);
		item->set_checked(0, Dictionary(stack[i]).get("enabled", true));
		if (i == selected_index && stack_tree->get_selected() != item) {
			item->select(0);
		}
	}
	updating = false;
}

void TetraBlenderModifiersTab::_update_params() {
	if (!is_inside_tree()) {
		return;
	}
	while (object_rows->get_child_count() > 0) {
		Node *row = object_rows->get_child(0);
		object_rows->remove_child(row);
		row->queue_free();
	}

	MeshInstance3D *target = plugin->get_target();
	const Array stack = target ? plugin->get_modifier_stack(target) : Array();
	if (selected_index < 0 || selected_index >= stack.size()) {
		param_panel->hide();
		shown_params = String();
		params_note->set_text(target ? TTR("No modifiers. \"Add Modifier\" lists what Blender offers: Subdivision Surface, Bevel, Array, Boolean, Solidify, Mirror and the rest.") : String());
		params_note->set_visible(target != nullptr);
		return;
	}

	const Dictionary modifier = stack[selected_index];
	const String type = modifier.get("type", String());
	const Dictionary schema = plugin->find_schema(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS, type);
	if (schema.is_empty()) {
		param_panel->hide();
		shown_params = String();
		switch (plugin->get_schema_state(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS)) {
			case TetraBlenderEditorPlugin::SCHEMA_LOADED:
				params_note->set_text(vformat(TTR("This Blender has no modifier of type %s."), type));
				break;
			case TetraBlenderEditorPlugin::SCHEMA_FAILED:
				params_note->set_text(TTR("The parameters are listed by Blender, which could not be started."));
				break;
			default:
				params_note->set_text(TTR("Asking Blender for the parameters…"));
				if (is_visible_in_tree()) {
					plugin->load_schemas(TetraBlenderEditorPlugin::SCHEMA_MODIFIERS);
				}
				break;
		}
		params_note->show();
		return;
	}

	const String key = vformat("%d|%d|%s", target->get_instance_id(), selected_index, type);
	if (key != shown_params) {
		shown_params = key;
		param_panel->set_operation(schema);
	}
	// Stored values are only what was changed; the rest shows its default.
	param_panel->set_values(TetraSchema::sanitize_values(schema, modifier.get("values", Dictionary())));
	param_panel->show();
	params_note->hide();

	// Parameters that name another object ("object" of a Boolean) choose
	// among the scene's other MeshInstance3D nodes.
	const Array object_params = schema.get("object_params", Array());
	const Dictionary object_paths = modifier.get("object_paths", Dictionary());
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	for (int i = 0; i < object_params.size() && scene_root; i++) {
		const Dictionary param = object_params[i];
		const String name = param.get("name", String());
		HBoxContainer *row = memnew(HBoxContainer);
		object_rows->add_child(row);
		Label *label = memnew(Label);
		label->set_text(param.get("label", name));
		label->set_tooltip_text(param.get("description", String()));
		label->set_mouse_filter(MOUSE_FILTER_PASS);
		label->set_h_size_flags(SIZE_EXPAND_FILL);
		label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row->add_child(label);
		OptionButton *option = memnew(OptionButton);
		option->set_h_size_flags(SIZE_EXPAND_FILL);
		option->set_fit_to_longest_item(false);
		option->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		option->add_item(TTR("None"));
		option->set_item_metadata(-1, NodePath());
		const NodePath current = object_paths.get(name, NodePath());
		Vector<MeshInstance3D *> others;
		_collect_mesh_instances(scene_root, others);
		for (MeshInstance3D *other : others) {
			if (other == target) {
				continue;
			}
			const NodePath path = target->get_path_to(other);
			option->add_item(other == scene_root ? String(other->get_name()) : String(scene_root->get_path_to(other)));
			option->set_item_metadata(-1, path);
			if (path == current) {
				option->select(option->get_item_count() - 1);
			}
		}
		option->connect(SceneStringName(item_selected), callable_mp(this, &TetraBlenderModifiersTab::_object_selected).bind(name, option));
		row->add_child(option);
	}
}

void TetraBlenderModifiersTab::_update_info() {
	if (!is_inside_tree()) {
		return;
	}
	MeshInstance3D *target = plugin->get_target();
	const bool has_stack = target && !plugin->get_modifier_stack(target).is_empty();
	const Dictionary info = has_stack ? plugin->get_evaluation_info(target) : Dictionary();
	const bool evaluating = info.get("evaluating", false);
	const String error = info.get("error", String());
	apply_button->set_disabled(!has_stack || evaluating);

	String text;
	if (evaluating) {
		text = TTR("Blender is evaluating the stack…");
	} else if (!error.is_empty()) {
		text = error;
	} else if (info.has("triangle_count")) {
		text = vformat(TTR("%d vertices, %d triangles"), int(info["vertex_count"]), int(info["triangle_count"]));
	}
	info_label->set_text(text);
	info_label->set_tooltip_text(text);
	info_label->set_visible(has_stack);
	if (!evaluating && !error.is_empty()) {
		info_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
	} else {
		info_label->remove_theme_color_override(SceneStringName(font_color));
	}
}

void TetraBlenderModifiersTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			add_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			shown_rows = String();
			_update_all();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			// Parameters of a stack that came with the scene are asked for
			// when the tab is first looked at.
			if (is_visible_in_tree()) {
				_update_params();
			}
		} break;
	}
}

void TetraBlenderModifiersTab::select_modifier(int p_index) {
	selected_index = p_index;
	_update_stack();
	_update_params();
}

int TetraBlenderModifiersTab::get_selected_modifier() const {
	return selected_index;
}

void TetraBlenderModifiersTab::popup_add_menu() {
	_add_pressed();
}

TetraParamPanel *TetraBlenderModifiersTab::get_param_panel() const {
	return param_panel;
}

void TetraBlenderModifiersTab::_bind_methods() {
	ClassDB::bind_method(D_METHOD("select_modifier", "index"), &TetraBlenderModifiersTab::select_modifier);
	ClassDB::bind_method(D_METHOD("get_selected_modifier"), &TetraBlenderModifiersTab::get_selected_modifier);
	ClassDB::bind_method(D_METHOD("popup_add_menu"), &TetraBlenderModifiersTab::popup_add_menu);
	ClassDB::bind_method(D_METHOD("get_param_panel"), &TetraBlenderModifiersTab::get_param_panel);
}

TetraBlenderModifiersTab::TetraBlenderModifiersTab(TetraBlenderEditorPlugin *p_plugin) {
	plugin = p_plugin;

	target_label = memnew(Label);
	target_label->set_theme_type_variation("HeaderSmall");
	target_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	target_label->set_mouse_filter(MOUSE_FILTER_PASS);
	add_child(target_label);

	add_button = memnew(Button);
	add_button->set_text(TTRC("Add Modifier"));
	add_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	add_button->set_tooltip_text(TTRC("Add one of Blender's modifiers to the stack."));
	add_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderModifiersTab::_add_pressed));
	add_child(add_button);

	add_menu = memnew(PopupMenu);
	add_menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraBlenderModifiersTab::_add_menu_id_pressed));
	add_child(add_menu);

	stack_tree = memnew(Tree);
	stack_tree->set_hide_root(true);
	stack_tree->set_select_mode(Tree::SELECT_ROW);
	stack_tree->set_edit_checkbox_cell_only_when_checkbox_is_pressed(true);
	stack_tree->set_custom_minimum_size(Size2(0, 120) * EDSCALE);
	stack_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	stack_tree->set_stretch_ratio(0.4);
	stack_tree->connect(SceneStringName(item_selected), callable_mp(this, &TetraBlenderModifiersTab::_stack_item_selected));
	stack_tree->connect("item_edited", callable_mp(this, &TetraBlenderModifiersTab::_stack_item_edited));
	stack_tree->connect("button_clicked", callable_mp(this, &TetraBlenderModifiersTab::_stack_button_clicked));
	add_child(stack_tree);

	HBoxContainer *buttons = memnew(HBoxContainer);
	add_child(buttons);
	apply_button = memnew(Button);
	apply_button->set_text(TTRC("Apply All"));
	apply_button->set_h_size_flags(SIZE_EXPAND_FILL);
	apply_button->set_tooltip_text(TTRC("Make the evaluated mesh the node's own mesh and empty the stack."));
	apply_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderModifiersTab::_apply_pressed));
	buttons->add_child(apply_button);
	clear_button = memnew(Button);
	clear_button->set_text(TTRC("Remove All"));
	clear_button->set_h_size_flags(SIZE_EXPAND_FILL);
	clear_button->set_tooltip_text(TTRC("Empty the stack and put the original mesh back."));
	clear_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderModifiersTab::_clear_pressed));
	buttons->add_child(clear_button);

	info_label = memnew(Label);
	info_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	info_label->set_mouse_filter(MOUSE_FILTER_PASS);
	add_child(info_label);

	object_rows = memnew(VBoxContainer);
	add_child(object_rows);

	params_note = memnew(Label);
	params_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	params_note->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(params_note);

	param_panel = memnew(TetraParamPanel);
	param_panel->set_v_size_flags(SIZE_EXPAND_FILL);
	param_panel->connect("value_changed", callable_mp(this, &TetraBlenderModifiersTab::_param_changed));
	param_panel->hide();
	add_child(param_panel);

	plugin->connect("target_changed", callable_mp(this, &TetraBlenderModifiersTab::_target_changed));
	plugin->connect("modifiers_changed", callable_mp(this, &TetraBlenderModifiersTab::_modifiers_changed));
	plugin->connect("modifiers_evaluated", callable_mp(this, &TetraBlenderModifiersTab::_modifiers_evaluated));
	plugin->connect("schemas_changed", callable_mp(this, &TetraBlenderModifiersTab::_schemas_changed));
}
