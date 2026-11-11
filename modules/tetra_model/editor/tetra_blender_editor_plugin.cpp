#include "tetra_blender_editor_plugin.h"

#include "../tetra_blender_scene.h"
#include "tetra_blender_dock.h"

#include "tetra/editor/tetra_operation_dialog.h"
#include "tetra/tetra_schema.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/undo_redo.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/scene_tree_editor.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/main/timer.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"

static constexpr const char *SETTING_JOIN_TRIANGLES = "tetra/blender/join_triangles";

TetraBlenderEditorPlugin *TetraBlenderEditorPlugin::singleton = nullptr;

TetraBlenderEditorPlugin *TetraBlenderEditorPlugin::get_singleton() {
	return singleton;
}

String TetraBlenderEditorPlugin::get_plugin_name() const {
	return "Blender";
}

Ref<TetraBlenderBridge> TetraBlenderEditorPlugin::get_bridge() const {
	return bridge;
}

TetraBlenderDock *TetraBlenderEditorPlugin::get_dock() const {
	return dock;
}

void TetraBlenderEditorPlugin::show_tab(const String &p_tab) {
	dock->make_visible();
	dock->show_tab(p_tab);
}

// --- Status ---

void TetraBlenderEditorPlugin::_begin_request() {
	if (status_timer->is_stopped()) {
		status_timer->start();
	}
	// The bridge counts the job as soon as it is queued.
	_update_status();
}

void TetraBlenderEditorPlugin::_note_result(const Dictionary &p_result) {
	if (!bool(p_result.get("ok", false))) {
		last_error = p_result.get("error", String());
		if (!bridge->is_running()) {
			start_error = last_error;
		}
	}
	_update_status();
}

void TetraBlenderEditorPlugin::_fail(const String &p_action, const String &p_message) {
	last_error = p_message;
	EditorToaster::get_singleton()->popup_str(vformat("%s: %s", p_action, p_message), EditorToaster::SEVERITY_ERROR);
	emit_signal(SNAME("request_failed"), p_action, p_message);
}

void TetraBlenderEditorPlugin::_update_status() {
	const bool running = bridge->is_running();
	const bool busy = bridge->is_busy();
	Status new_status = STATUS_NOT_STARTED;
	String text;
	String hint;
	if (running) {
		const String version = bridge->get_blender_version();
		new_status = busy ? STATUS_WORKING : STATUS_READY;
		text = busy ? vformat(TTR("Blender %s working…"), version) : vformat(TTR("Blender %s ready"), version);
		hint = bridge->get_launch_description();
	} else if (busy) {
		new_status = STATUS_STARTING;
		text = TTR("Starting Blender…");
		hint = TetraBlenderBridge::find_launcher().get("description", String());
	} else if (!start_error.is_empty()) {
		new_status = STATUS_FAILED;
		text = TTR("Blender is not running");
		hint = start_error;
	} else if (!TetraBlenderBridge::is_available()) {
		new_status = STATUS_NOT_FOUND;
		text = TTR("Blender not found");
		hint = TTR("Set the editor setting tetra/blender/python_path (a Python with the bpy module) or tetra/blender/executable_path (a Blender executable), or run scripts/setup-blender-bridge.sh.");
	} else {
		text = TTR("Blender not started");
		hint = vformat(TTR("Starts when first needed: %s"), String(TetraBlenderBridge::find_launcher().get("description", String())));
	}
	if (!busy) {
		status_timer->stop();
	}
	if (new_status != status || text != status_text || hint != status_hint) {
		status = new_status;
		status_text = text;
		status_hint = hint;
		emit_signal(SNAME("status_changed"));
	}
}

void TetraBlenderEditorPlugin::_bridge_started() {
	start_error = String();
	_update_status();
}

void TetraBlenderEditorPlugin::_bridge_stopped() {
	_update_status();
}

void TetraBlenderEditorPlugin::_bridge_progress(const Dictionary &p_event) {
	if (rendering && String(p_event.get("cmd", String())) == "render") {
		emit_signal(SNAME("render_progress"), double(p_event.get("fraction", 0.0)), int(p_event.get("sample", 0)), int(p_event.get("samples", 0)));
	}
}

TetraBlenderEditorPlugin::Status TetraBlenderEditorPlugin::get_status() const {
	return status;
}

String TetraBlenderEditorPlugin::get_status_text() const {
	return status_text;
}

String TetraBlenderEditorPlugin::get_status_hint() const {
	return status_hint;
}

String TetraBlenderEditorPlugin::get_last_error() const {
	return last_error;
}

void TetraBlenderEditorPlugin::restart_bridge() {
	start_error = String();
	bridge->stop();
	_update_status();
}

// --- Schemas ---

void TetraBlenderEditorPlugin::load_schemas(SchemaKind p_kind) {
	ERR_FAIL_INDEX(p_kind, SCHEMA_MAX);
	if (schema_states[p_kind] == SCHEMA_LOADING || schema_states[p_kind] == SCHEMA_LOADED) {
		return;
	}
	schema_states[p_kind] = SCHEMA_LOADING;
	const Callable done = callable_mp(this, &TetraBlenderEditorPlugin::_schemas_loaded).bind(int(p_kind));
	switch (p_kind) {
		case SCHEMA_MODIFIERS:
			bridge->list_modifiers_async(done);
			break;
		case SCHEMA_OPERATORS:
			bridge->list_operators_async(done);
			break;
		default:
			bridge->list_primitives_async(done);
			break;
	}
	_begin_request();
	emit_signal(SNAME("schemas_changed"), int(p_kind));
}

void TetraBlenderEditorPlugin::_schemas_loaded(const Dictionary &p_result, int p_kind) {
	_note_result(p_result);
	if (bool(p_result.get("ok", false))) {
		schemas[p_kind] = p_result.get("operations", Array());
		schema_states[p_kind] = SCHEMA_LOADED;
	} else {
		schema_states[p_kind] = SCHEMA_FAILED;
	}
	emit_signal(SNAME("schemas_changed"), p_kind);
}

TetraBlenderEditorPlugin::SchemaState TetraBlenderEditorPlugin::get_schema_state(SchemaKind p_kind) const {
	ERR_FAIL_INDEX_V(p_kind, SCHEMA_MAX, SCHEMA_NOT_LOADED);
	return schema_states[p_kind];
}

Array TetraBlenderEditorPlugin::get_schemas(SchemaKind p_kind) const {
	ERR_FAIL_INDEX_V(p_kind, SCHEMA_MAX, Array());
	return schemas[p_kind];
}

Dictionary TetraBlenderEditorPlugin::find_schema(SchemaKind p_kind, const String &p_id) const {
	ERR_FAIL_INDEX_V(p_kind, SCHEMA_MAX, Dictionary());
	String id = p_id;
	if (p_kind == SCHEMA_MODIFIERS && !id.begins_with("blender:")) {
		id = "blender:modifier." + id;
	}
	for (int i = 0; i < schemas[p_kind].size(); i++) {
		const Dictionary schema = schemas[p_kind][i];
		if (String(schema.get("id", String())) == id) {
			return schema;
		}
	}
	return Dictionary();
}

Dictionary TetraBlenderEditorPlugin::get_changed_values(const Dictionary &p_schema, const Dictionary &p_values) {
	const Dictionary defaults = TetraSchema::get_defaults(p_schema);
	Dictionary changed;
	for (const KeyValue<Variant, Variant> &kv : p_values) {
		if (!defaults.has(kv.key) || defaults[kv.key] != kv.value) {
			changed[kv.key] = kv.value;
		}
	}
	return changed;
}

// --- Selection ---

void TetraBlenderEditorPlugin::_selection_changed() {
	MeshInstance3D *found = nullptr;
	int count = 0;
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_full_selected_node_list()) {
		if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node)) {
			found = mesh_instance;
			count++;
		}
	}
	const ObjectID new_target = count == 1 ? found->get_instance_id() : ObjectID();
	if (new_target != target) {
		target = new_target;
		emit_signal(SNAME("target_changed"));
	}
}

MeshInstance3D *TetraBlenderEditorPlugin::get_target() const {
	return ObjectDB::get_instance<MeshInstance3D>(target);
}

// --- Modifiers ---

Array TetraBlenderEditorPlugin::get_modifier_stack(MeshInstance3D *p_node) const {
	return TetraBlenderScene::get_modifier_stack(p_node);
}

void TetraBlenderEditorPlugin::_commit_stack(MeshInstance3D *p_node, const String &p_action, const Array &p_stack, bool p_merge) {
	const Array old_stack = TetraBlenderScene::get_modifier_stack(p_node);
	// The first modifier makes the node's mesh the source; removing the last
	// one puts the source back.
	const Ref<Mesh> source = TetraBlenderScene::get_base_mesh(p_node);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, p_merge ? UndoRedo::MERGE_ENDS : UndoRedo::MERGE_DISABLE, p_node);
	undo_redo->add_do_method(this, "_set_modifier_state", p_node, source, p_stack, p_stack.is_empty() ? Variant(source) : Variant());
	undo_redo->add_undo_method(this, "_set_modifier_state", p_node, source, old_stack, old_stack.is_empty() ? Variant(source) : Variant());
	undo_redo->commit_action();
}

void TetraBlenderEditorPlugin::_set_modifier_state(Node *p_node, const Ref<Mesh> &p_source, const Array &p_stack, const Ref<Mesh> &p_mesh) {
	MeshInstance3D *node = Object::cast_to<MeshInstance3D>(p_node);
	ERR_FAIL_NULL(node);
	TetraBlenderScene::set_modifier_stack(node, p_source, p_stack);

	// Whatever is on its way from Blender belongs to an older state.
	Evaluation &evaluation = evaluations[node->get_instance_id()];
	evaluation.wanted = ++generation_counter;
	evaluation.dirty = false;
	evaluation.error = String();
	if (p_mesh.is_valid()) {
		node->set_mesh(p_mesh);
	} else if (!p_stack.is_empty()) {
		_queue_evaluation(node);
	}
	emit_signal(SNAME("modifiers_changed"), node);
}

void TetraBlenderEditorPlugin::_queue_evaluation(MeshInstance3D *p_node) {
	Evaluation &evaluation = evaluations[p_node->get_instance_id()];
	evaluation.wanted = ++generation_counter;
	evaluation.dirty = true;
	// Restarting the timer is the debounce: a dragged slider sends one
	// request when it rests, or when the previous answer arrives.
	evaluation_timer->start();
}

void TetraBlenderEditorPlugin::_evaluation_timeout() {
	Vector<ObjectID> due;
	for (const KeyValue<ObjectID, Evaluation> &kv : evaluations) {
		if (kv.value.dirty && !kv.value.in_flight) {
			due.push_back(kv.key);
		}
	}
	for (const ObjectID &id : due) {
		_send_evaluation(id);
	}
}

void TetraBlenderEditorPlugin::_send_evaluation(ObjectID p_node) {
	MeshInstance3D *node = ObjectDB::get_instance<MeshInstance3D>(p_node);
	if (!node || !TetraBlenderScene::has_modifier_stack(node)) {
		evaluations.erase(p_node);
		return;
	}
	Evaluation &evaluation = evaluations[p_node];
	evaluation.dirty = false;
	evaluation.in_flight = true;
	evaluation.sent = evaluation.wanted;
	bridge->set_join_triangles(EDITOR_GET(SETTING_JOIN_TRIANGLES));
	const Array stack = TetraBlenderScene::to_bridge_stack(node, TetraBlenderScene::get_modifier_stack(node));
	bridge->apply_modifier_stack_async(TetraBlenderScene::get_source_mesh(node), stack, callable_mp(this, &TetraBlenderEditorPlugin::_evaluation_done).bind(uint64_t(p_node), evaluation.sent));
	_begin_request();
}

void TetraBlenderEditorPlugin::_evaluation_done(const Dictionary &p_result, uint64_t p_node, uint64_t p_generation) {
	_note_result(p_result);
	const ObjectID id = ObjectID(p_node);
	HashMap<ObjectID, Evaluation>::Iterator found = evaluations.find(id);
	if (!found) {
		return;
	}
	MeshInstance3D *node = ObjectDB::get_instance<MeshInstance3D>(id);
	if (!node || !TetraBlenderScene::has_modifier_stack(node)) {
		evaluations.erase(id);
		return;
	}
	Evaluation &evaluation = found->value;
	evaluation.in_flight = false;
	const bool ok = p_result.get("ok", false);
	// One request per node is in flight at a time, so answers arrive in
	// order: an answer to an older stack is shown while its successor is
	// still to come, but never once the state was set by something else.
	const bool newest = p_generation == evaluation.wanted;
	if (ok && (newest || evaluation.dirty)) {
		node->set_mesh(p_result.get("mesh", Variant()));
		evaluation.error = String();
		evaluation.counts = Dictionary();
		evaluation.counts["vertex_count"] = p_result.get("vertex_count", 0);
		evaluation.counts["polygon_count"] = p_result.get("polygon_count", 0);
		evaluation.counts["triangle_count"] = p_result.get("triangle_count", 0);
	} else if (!ok && newest) {
		evaluation.error = p_result.get("error", String());
	}
	if (evaluation.dirty) {
		if (evaluation_timer->is_stopped()) {
			_send_evaluation(id);
		}
	} else if (newest) {
		emit_signal(SNAME("modifiers_evaluated"), node, ok);
	}
}

String TetraBlenderEditorPlugin::get_modifier_label(const Dictionary &p_modifier) const {
	// A modifier added before Blender listed its modifiers only knows its
	// type; the proper name is used once it is known.
	const Dictionary schema = find_schema(SCHEMA_MODIFIERS, p_modifier.get("type", String()));
	return schema.get("label", p_modifier.get("label", String()));
}

int TetraBlenderEditorPlugin::add_modifier(MeshInstance3D *p_node, const String &p_type, const Dictionary &p_values) {
	ERR_FAIL_NULL_V(p_node, -1);
	ERR_FAIL_COND_V_MSG(TetraBlenderScene::get_base_mesh(p_node).is_null(), -1, "The node has no mesh to modify.");
	// The stack is shown with the names Blender gives its modifiers.
	load_schemas(SCHEMA_MODIFIERS);
	const String label = find_schema(SCHEMA_MODIFIERS, p_type).get("label", String());
	const Dictionary modifier = TetraBlenderScene::make_modifier(p_type, p_values, label);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	stack.push_back(modifier);
	_commit_stack(p_node, vformat(TTR("Add %s Modifier"), get_modifier_label(modifier)), stack);
	return stack.size() - 1;
}

void TetraBlenderEditorPlugin::set_modifier_values(MeshInstance3D *p_node, int p_index, const Dictionary &p_values) {
	ERR_FAIL_NULL(p_node);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	ERR_FAIL_INDEX(p_index, stack.size());
	Dictionary modifier = stack[p_index];
	Dictionary values = modifier.get("values", Dictionary());
	values.merge(p_values, true);
	modifier["values"] = values;
	stack[p_index] = modifier;
	// The parameter is part of the name so that only changes of the same
	// one coalesce into a single undo step.
	const String what = p_values.size() == 1 ? String(p_values.get_key_at_index(0)) : String("values");
	_commit_stack(p_node, vformat(TTR("Set %s %s"), get_modifier_label(modifier), what), stack, true);
}

void TetraBlenderEditorPlugin::set_modifier_enabled(MeshInstance3D *p_node, int p_index, bool p_enabled) {
	ERR_FAIL_NULL(p_node);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	ERR_FAIL_INDEX(p_index, stack.size());
	Dictionary modifier = stack[p_index];
	modifier["enabled"] = p_enabled;
	stack[p_index] = modifier;
	_commit_stack(p_node, vformat(p_enabled ? TTR("Enable %s Modifier") : TTR("Disable %s Modifier"), get_modifier_label(modifier)), stack);
}

void TetraBlenderEditorPlugin::set_modifier_object(MeshInstance3D *p_node, int p_index, const String &p_param, const NodePath &p_path) {
	ERR_FAIL_NULL(p_node);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	ERR_FAIL_INDEX(p_index, stack.size());
	Dictionary modifier = stack[p_index];
	Dictionary object_paths = modifier.get("object_paths", Dictionary());
	if (p_path.is_empty()) {
		object_paths.erase(p_param);
	} else {
		object_paths[p_param] = p_path;
	}
	modifier["object_paths"] = object_paths;
	stack[p_index] = modifier;
	_commit_stack(p_node, vformat(TTR("Set %s %s"), get_modifier_label(modifier), p_param), stack);
}

void TetraBlenderEditorPlugin::move_modifier(MeshInstance3D *p_node, int p_index, int p_to_index) {
	ERR_FAIL_NULL(p_node);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	ERR_FAIL_INDEX(p_index, stack.size());
	ERR_FAIL_INDEX(p_to_index, stack.size());
	if (p_index == p_to_index) {
		return;
	}
	const Dictionary modifier = stack[p_index];
	stack.remove_at(p_index);
	stack.insert(p_to_index, modifier);
	_commit_stack(p_node, vformat(TTR("Move %s Modifier"), get_modifier_label(modifier)), stack);
}

void TetraBlenderEditorPlugin::remove_modifier(MeshInstance3D *p_node, int p_index) {
	ERR_FAIL_NULL(p_node);
	Array stack = TetraBlenderScene::get_modifier_stack(p_node);
	ERR_FAIL_INDEX(p_index, stack.size());
	const Dictionary modifier = stack[p_index];
	stack.remove_at(p_index);
	_commit_stack(p_node, vformat(TTR("Remove %s Modifier"), get_modifier_label(modifier)), stack);
}

bool TetraBlenderEditorPlugin::apply_modifiers(MeshInstance3D *p_node) {
	ERR_FAIL_NULL_V(p_node, false);
	if (!TetraBlenderScene::has_modifier_stack(p_node) || is_evaluating(p_node)) {
		return false;
	}
	const Ref<Mesh> result = p_node->get_mesh();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Apply Blender Modifiers"), UndoRedo::MERGE_DISABLE, p_node);
	undo_redo->add_do_method(this, "_set_modifier_state", p_node, Ref<Mesh>(), Array(), result);
	undo_redo->add_undo_method(this, "_set_modifier_state", p_node, TetraBlenderScene::get_source_mesh(p_node), TetraBlenderScene::get_modifier_stack(p_node), result);
	undo_redo->commit_action();
	return true;
}

void TetraBlenderEditorPlugin::clear_modifiers(MeshInstance3D *p_node) {
	ERR_FAIL_NULL(p_node);
	if (TetraBlenderScene::has_modifier_stack(p_node)) {
		_commit_stack(p_node, TTR("Remove Blender Modifiers"), Array());
	}
}

void TetraBlenderEditorPlugin::evaluate_modifiers(MeshInstance3D *p_node) {
	ERR_FAIL_NULL(p_node);
	if (TetraBlenderScene::has_modifier_stack(p_node)) {
		_queue_evaluation(p_node);
	}
}

bool TetraBlenderEditorPlugin::is_evaluating(MeshInstance3D *p_node) const {
	ERR_FAIL_NULL_V(p_node, false);
	HashMap<ObjectID, Evaluation>::ConstIterator found = evaluations.find(p_node->get_instance_id());
	return found && (found->value.in_flight || found->value.dirty);
}

Dictionary TetraBlenderEditorPlugin::get_evaluation_info(MeshInstance3D *p_node) const {
	ERR_FAIL_NULL_V(p_node, Dictionary());
	Dictionary info;
	HashMap<ObjectID, Evaluation>::ConstIterator found = evaluations.find(p_node->get_instance_id());
	if (found) {
		info = found->value.counts.duplicate();
		info["error"] = found->value.error;
	} else {
		info["error"] = String();
	}
	info["evaluating"] = is_evaluating(p_node);
	return info;
}

// --- Operators ---

bool TetraBlenderEditorPlugin::run_operator(MeshInstance3D *p_node, const String &p_id, const Dictionary &p_values) {
	ERR_FAIL_NULL_V(p_node, false);
	return _start_operator(p_node, p_id, p_values, TetraBlenderScene::get_base_mesh(p_node), false);
}

bool TetraBlenderEditorPlugin::rerun_last_operator(const Dictionary &p_values) {
	if (!can_rerun_last_operator()) {
		return false;
	}
	return _start_operator(ObjectDB::get_instance<MeshInstance3D>(last_operation.node), last_operation.id, p_values, last_operation.base, true);
}

bool TetraBlenderEditorPlugin::can_rerun_last_operator() const {
	MeshInstance3D *node = ObjectDB::get_instance<MeshInstance3D>(last_operation.node);
	return node && !operator_running && last_operation.result.is_valid() && TetraBlenderScene::get_base_mesh(node) == last_operation.result;
}

bool TetraBlenderEditorPlugin::is_operator_running() const {
	return operator_running;
}

Dictionary TetraBlenderEditorPlugin::get_last_operation() const {
	Dictionary operation;
	MeshInstance3D *node = ObjectDB::get_instance<MeshInstance3D>(last_operation.node);
	if (node) {
		operation["node"] = node;
		operation["id"] = last_operation.id;
		operation["values"] = last_operation.values;
	}
	return operation;
}

bool TetraBlenderEditorPlugin::_start_operator(MeshInstance3D *p_node, const String &p_id, const Dictionary &p_values, const Ref<Mesh> &p_base, bool p_rerun) {
	if (operator_running) {
		_fail(TTR("Blender operator"), TTR("The previous operator is still running."));
		return false;
	}
	if (p_base.is_null()) {
		_fail(TTR("Blender operator"), TTR("The node has no mesh."));
		return false;
	}
	operator_running = true;
	// Answered before the operator's result: names the undo step.
	load_schemas(SCHEMA_OPERATORS);
	Dictionary entry;
	entry["id"] = p_id;
	entry["values"] = p_values;
	Array operators;
	operators.push_back(entry);
	Dictionary options;
	options["join_triangles"] = EDITOR_GET(SETTING_JOIN_TRIANGLES);
	bridge->run_operators_async(p_base, operators, callable_mp(this, &TetraBlenderEditorPlugin::_operator_done).bind(uint64_t(p_node->get_instance_id()), p_id, p_values, p_base, p_rerun), options);
	_begin_request();
	emit_signal(SNAME("operator_started"));
	return true;
}

bool TetraBlenderEditorPlugin::_last_operation_is_newest_action() const {
	UndoRedo *history = EditorUndoRedoManager::get_singleton()->get_history_undo_redo(last_operation.history_id);
	return history && history->get_version() == last_operation.history_version && history->get_current_action_name() == last_operation.action_name;
}

void TetraBlenderEditorPlugin::_finish_operator(MeshInstance3D *p_node, bool p_ok, const String &p_message) {
	operator_running = false;
	if (!p_ok) {
		last_error = p_message;
	}
	emit_signal(SNAME("operator_finished"), p_node, p_ok, p_message);
}

void TetraBlenderEditorPlugin::_operator_done(const Dictionary &p_result, uint64_t p_node, const String &p_id, const Dictionary &p_values, const Ref<Mesh> &p_base, bool p_rerun) {
	_note_result(p_result);
	MeshInstance3D *node = ObjectDB::get_instance<MeshInstance3D>(ObjectID(p_node));
	if (!node) {
		_finish_operator(nullptr, false, TTR("The node was removed while Blender worked."));
		return;
	}
	if (!bool(p_result.get("ok", false))) {
		_finish_operator(node, false, p_result.get("error", String()));
		return;
	}
	const Array results = p_result.get("results", Array());
	if (!results.is_empty() && String(Dictionary(results[0]).get("result", String())) == "CANCELLED") {
		_finish_operator(node, false, TTR("Blender cancelled the operator: with these values it has nothing to do."));
		return;
	}
	// The answer replaces the mesh the request started from (for a re-run:
	// the result of the run it adjusts). Anything else got there meanwhile.
	const Ref<Mesh> expected = p_rerun ? last_operation.result : p_base;
	if (TetraBlenderScene::get_base_mesh(node) != expected) {
		_finish_operator(node, false, TTR("The mesh changed while Blender worked; the result was dropped."));
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	// Adjusting the last operation replaces its undo step when nothing was
	// done since; otherwise the adjustment becomes a step of its own.
	if (p_rerun && _last_operation_is_newest_action()) {
		undo_redo->undo_history(last_operation.history_id);
	}
	const Ref<Mesh> before = TetraBlenderScene::get_base_mesh(node);
	const Ref<Mesh> mesh = p_result.get("mesh", Variant());
	const String label = find_schema(SCHEMA_OPERATORS, p_id).get("label", p_id.get_slice(":", 1));
	const String action_name = vformat(TTR("Blender: %s"), label);
	undo_redo->create_action(action_name, UndoRedo::MERGE_DISABLE, node);
	if (TetraBlenderScene::has_modifier_stack(node)) {
		// The operator edits what the modifiers start from, as in Blender.
		const Array stack = TetraBlenderScene::get_modifier_stack(node);
		undo_redo->add_do_method(this, "_set_modifier_state", node, mesh, stack, Variant());
		undo_redo->add_undo_method(this, "_set_modifier_state", node, before, stack, Variant());
	} else {
		undo_redo->add_do_method(node, "set_mesh", mesh);
		undo_redo->add_undo_method(node, "set_mesh", before);
	}
	undo_redo->commit_action();

	last_operation.node = node->get_instance_id();
	last_operation.id = p_id;
	last_operation.action_name = action_name;
	last_operation.values = p_values;
	last_operation.base = p_base;
	last_operation.result = mesh;
	last_operation.history_id = undo_redo->get_history_id_for_object(node);
	UndoRedo *history = undo_redo->get_history_undo_redo(last_operation.history_id);
	last_operation.history_version = history ? history->get_version() : 0;
	_finish_operator(node, true, String());
}

// --- Adding objects ---

Node *TetraBlenderEditorPlugin::_resolve_parent(Node *p_parent) const {
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!root) {
		return nullptr;
	}
	if (p_parent && (p_parent == root || root->is_ancestor_of(p_parent))) {
		return p_parent;
	}
	const List<Node *> selection = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
	if (selection.size() == 1 && (selection.front()->get() == root || root->is_ancestor_of(selection.front()->get()))) {
		return selection.front()->get();
	}
	return root;
}

static void _collect_nodes(Node *p_node, Vector<Node *> &r_nodes) {
	r_nodes.push_back(p_node);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_nodes(p_node->get_child(i), r_nodes);
	}
}

void TetraBlenderEditorPlugin::_add_to_scene(Node *p_node, Node *p_parent, const String &p_action) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	Vector<Node *> nodes;
	_collect_nodes(p_node, nodes);
	if (!root) {
		// An empty scene tab: the object arrives together with a root.
		Node3D *new_root = memnew(Node3D);
		new_root->set_name("Node3D");
		new_root->add_child(p_node, true);
		for (Node *node : nodes) {
			node->set_owner(new_root);
		}
		undo_redo->create_action_for_history(p_action, EditorNode::get_editor_data().get_current_edited_scene_history_id());
		undo_redo->add_do_method(EditorNode::get_singleton(), "set_edited_scene", new_root);
		undo_redo->add_do_method(SceneTreeDock::get_singleton()->get_tree_editor(), "update_tree");
		undo_redo->add_do_reference(new_root);
		undo_redo->add_undo_method(EditorNode::get_singleton(), "set_edited_scene", (Object *)nullptr);
		undo_redo->commit_action();
	} else {
		Node *parent = _resolve_parent(p_parent);
		undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, parent);
		undo_redo->add_do_method(parent, "add_child", p_node, true);
		for (Node *node : nodes) {
			undo_redo->add_do_method(node, "set_owner", root);
		}
		undo_redo->add_do_reference(p_node);
		undo_redo->add_undo_method(parent, "remove_child", p_node);
		undo_redo->commit_action();
	}
	EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
	selection->clear();
	selection->add_node(p_node);
	EditorNode::get_singleton()->edit_node(p_node);
}

void TetraBlenderEditorPlugin::add_primitive(const String &p_id, const Dictionary &p_values, Node *p_parent) {
	const uint64_t parent = p_parent ? uint64_t(p_parent->get_instance_id()) : 0;
	// Requests are answered in order, so the primitive's proper name is
	// known by the time its mesh arrives.
	load_schemas(SCHEMA_PRIMITIVES);
	bridge->create_primitive_async(p_id, p_values, callable_mp(this, &TetraBlenderEditorPlugin::_primitive_done).bind(p_id, parent));
	_begin_request();
}

void TetraBlenderEditorPlugin::_primitive_done(const Dictionary &p_result, const String &p_id, uint64_t p_parent) {
	_note_result(p_result);
	const Ref<ArrayMesh> mesh = p_result.get("mesh", Variant());
	if (!bool(p_result.get("ok", false)) || mesh.is_null()) {
		_fail(TTR("Add Blender mesh"), p_result.get("error", String()));
		return;
	}
	// "blender:mesh.primitive_uv_sphere_add" is "UV Sphere" once the schemas are known.
	String label = find_schema(SCHEMA_PRIMITIVES, p_id).get("label", String());
	if (label.is_empty()) {
		label = p_id.get_slice(".", 1).trim_prefix("primitive_").trim_suffix("_add").capitalize();
	}
	mesh->set_name(label);
	const Ref<StandardMaterial3D> material = TetraBlenderScene::make_default_material();
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		mesh->surface_set_material(i, material);
	}
	MeshInstance3D *node = memnew(MeshInstance3D);
	node->set_name(label.replace(" ", "").validate_node_name());
	node->set_mesh(mesh);
	_add_to_scene(node, ObjectDB::get_instance<Node>(ObjectID(p_parent)), vformat(TTR("Add Blender %s"), label));
	emit_signal(SNAME("primitive_added"), node);
}

void TetraBlenderEditorPlugin::popup_add_primitive(const String &p_id) {
	const Dictionary schema = find_schema(SCHEMA_PRIMITIVES, p_id);
	ERR_FAIL_COND_MSG(schema.is_empty(), vformat("Unknown Blender primitive '%s' (are the primitives loaded?).", p_id));
	HashMap<String, Dictionary>::ConstIterator previous = primitive_values.find(p_id);
	primitive_dialog->popup_operation(schema, previous ? previous->value : Dictionary());
}

void TetraBlenderEditorPlugin::_primitive_confirmed(const String &p_id, const Dictionary &p_values) {
	primitive_values[p_id] = p_values;
	add_primitive(p_id, get_changed_values(find_schema(SCHEMA_PRIMITIVES, p_id), p_values));
}

void TetraBlenderEditorPlugin::import_model(const String &p_path, Node *p_parent) {
	const uint64_t parent = p_parent ? uint64_t(p_parent->get_instance_id()) : 0;
	bridge->import_meshes_async(p_path, callable_mp(this, &TetraBlenderEditorPlugin::_import_done).bind(p_path, parent));
	_begin_request();
}

void TetraBlenderEditorPlugin::_import_done(const Dictionary &p_result, const String &p_path, uint64_t p_parent) {
	_note_result(p_result);
	if (!bool(p_result.get("ok", false))) {
		_fail(TTR("Import model"), p_result.get("error", String()));
		return;
	}
	const Array meshes = p_result.get("meshes", Array());
	if (meshes.is_empty()) {
		_fail(TTR("Import model"), vformat(TTR("Blender found no meshes in %s."), p_path.get_file()));
		return;
	}

	Node3D *container = memnew(Node3D);
	String container_name = p_path.get_file().get_basename().validate_node_name();
	container->set_name(container_name.is_empty() ? String("Model") : container_name);

	// Blender names its objects uniquely, so the parent of each can be
	// looked up by name. The transforms are world transforms.
	HashMap<String, MeshInstance3D *> nodes;
	HashMap<String, Transform3D> world;
	for (int i = 0; i < meshes.size(); i++) {
		const Dictionary entry = meshes[i];
		const String name = entry.get("name", String());
		MeshInstance3D *node = memnew(MeshInstance3D);
		const String node_name = name.validate_node_name();
		node->set_name(node_name.is_empty() ? String("Mesh") : node_name);
		node->set_mesh(entry.get("mesh", Variant()));
		nodes[name] = node;
		world[name] = entry.get("transform", Transform3D());
	}
	for (int i = 0; i < meshes.size(); i++) {
		const Dictionary entry = meshes[i];
		const String name = entry.get("name", String());
		const String parent_name = entry.get("parent", String());
		MeshInstance3D *node = nodes[name];
		if (parent_name != name && nodes.has(parent_name)) {
			nodes[parent_name]->add_child(node, true);
			node->set_transform(world[parent_name].affine_inverse() * world[name]);
		} else {
			container->add_child(node, true);
			node->set_transform(world[name]);
		}
	}
	_add_to_scene(container, ObjectDB::get_instance<Node>(ObjectID(p_parent)), vformat(TTR("Import %s"), p_path.get_file()));
	emit_signal(SNAME("model_imported"), container);
}

void TetraBlenderEditorPlugin::popup_import_model() {
	import_dialog->popup_file_dialog();
}

// --- Rendering ---

Dictionary TetraBlenderEditorPlugin::get_render_schema() const {
	Dictionary schema = TetraSchema::make_operation("native:blender-render", TTR("Cycles Render"), "Render", "native", TTR("Renders the edited scene with Blender's path tracer."));
	Array cameras;
	cameras.push_back(TetraSchema::make_option("viewport", TTR("3D Viewport")));
	cameras.push_back(TetraSchema::make_option("scene", TTR("Scene Camera")));
	TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_enum("camera", TTR("Camera"), "viewport", cameras), TTR("Render what the 3D viewport shows, or through the scene's current Camera3D.")));
	TetraSchema::add_param(schema, TetraSchema::set_ui_range(TetraSchema::param_int("width", TTR("Width"), 960, 16, 8192, 1, "px"), 160, 3840));
	TetraSchema::add_param(schema, TetraSchema::set_ui_range(TetraSchema::param_int("height", TTR("Height"), 540, 16, 8192, 1, "px"), 90, 2160));
	TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("match_viewport", TTR("Viewport Aspect"), true), TTR("Use the aspect ratio of the 3D viewport, so the picture frames exactly what the viewport shows. Height is ignored.")));
	TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::set_ui_range(TetraSchema::param_int("samples", TTR("Samples"), 64, 1, 65536, 1), 1, 512), TTR("Paths traced per pixel: more is slower and less noisy.")));
	TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("denoise", TTR("Denoise"), true), TTR("Remove the remaining noise with OpenImageDenoise.")));
	TetraSchema::add_param(schema, TetraSchema::set_advanced(TetraSchema::set_description(TetraSchema::param_bool("transparent", TTR("Transparent Background"), false), TTR("Leave the background out of the picture (alpha 0)."))));
	return schema;
}

// The 3D editor lights a scene that has no DirectionalLight3D or no
// WorldEnvironment of its own with a preview sun and a preview sky (the two
// toggles of its toolbar). A render of what the viewport shows needs the same.
static void _add_preview_lighting(Dictionary &r_scene, Node *p_root) {
	const Dictionary state = Node3DEditor::get_singleton()->get_state();
	const Dictionary preview = state.get("preview_sun_env", Dictionary());

	const bool has_sun = Object::cast_to<DirectionalLight3D>(p_root) || !p_root->find_children("*", "DirectionalLight3D", true, false).is_empty();
	if (!has_sun && bool(preview.get("sun_enabled", false))) {
		const Vector2 rotation = preview.get("sun_rotation", Vector2());
		DirectionalLight3D *sun = memnew(DirectionalLight3D);
		sun->set_name("PreviewSun");
		sun->set_transform(Transform3D(Basis::from_euler(Vector3(rotation.x, rotation.y, 0)), Vector3()));
		sun->set_color(preview.get("sun_color", Color(1, 1, 1)));
		sun->set_param(Light3D::PARAM_ENERGY, preview.get("sun_energy", 1.0));
		Array lights = r_scene.get("lights", Array());
		lights.push_back(TetraBlenderScene::light_to_dictionary(sun));
		r_scene["lights"] = lights;
		memdelete(sun);
	}

	const bool has_environment = Object::cast_to<WorldEnvironment>(p_root) || !p_root->find_children("*", "WorldEnvironment", true, false).is_empty();
	if (!has_environment && bool(preview.get("environ_enabled", false))) {
		// The preview sky as Node3DEditor builds it from its two colours.
		const Color sky_color = preview.get("environ_sky_color", Color(0.385, 0.454, 0.55));
		const Color ground_color = preview.get("environ_ground_color", Color(0.2, 0.169, 0.133));
		Color horizon = sky_color.lerp(ground_color, 0.5);
		const float luminance = horizon.get_luminance() * 3.333;
		horizon = horizon.lerp(Color(luminance, luminance, luminance), 0.5);
		Ref<ProceduralSkyMaterial> sky_material;
		sky_material.instantiate();
		sky_material->set_sky_top_color(sky_color);
		sky_material->set_sky_horizon_color(horizon);
		sky_material->set_energy_multiplier(preview.get("environ_energy", 1.0));
		Ref<Sky> sky;
		sky.instantiate();
		sky->set_material(sky_material);
		Ref<Environment> environment;
		environment.instantiate();
		environment->set_background(Environment::BG_SKY);
		environment->set_sky(sky);
		r_scene["world"] = TetraBlenderScene::environment_to_world(environment);
	}
}

bool TetraBlenderEditorPlugin::render_current_view(const Dictionary &p_settings) {
	if (rendering) {
		_fail(TTR("Cycles render"), TTR("A render is already under way."));
		return false;
	}
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!root) {
		_fail(TTR("Cycles render"), TTR("There is no scene to render."));
		return false;
	}
	const Dictionary settings = TetraSchema::sanitize_values(get_render_schema(), p_settings);
	Node3DEditorViewport *viewport = Node3DEditor::get_singleton()->get_last_used_viewport();
	const Size2 viewport_size = viewport->get_viewport_node()->get_size();
	const real_t viewport_aspect = viewport_size.y > 0 ? viewport_size.x / viewport_size.y : real_t(16.0 / 9.0);
	const bool through_viewport = String(settings["camera"]) != "scene";
	Camera3D *camera = through_viewport ? viewport->get_camera_3d() : TetraBlenderScene::find_scene_camera(root);
	if (!camera) {
		_fail(TTR("Cycles render"), TTR("The scene has no Camera3D."));
		return false;
	}
	Dictionary scene = TetraBlenderScene::gather_scene(root);
	if (Array(scene["meshes"]).is_empty()) {
		_fail(TTR("Cycles render"), TTR("The scene has no visible MeshInstance3D."));
		return false;
	}
	if (through_viewport) {
		_add_preview_lighting(scene, root);
	}
	const Vector2i size = TetraBlenderScene::fit_resolution(settings["width"], settings["height"], viewport_aspect, settings["match_viewport"]);
	scene["camera"] = TetraBlenderScene::camera_to_dictionary(camera, real_t(size.x) / real_t(size.y));

	Dictionary render_settings;
	render_settings["engine"] = "CYCLES";
	render_settings["width"] = size.x;
	render_settings["height"] = size.y;
	render_settings["samples"] = settings["samples"];
	render_settings["denoise"] = settings["denoise"];
	render_settings["transparent"] = settings["transparent"];
	rendering = true;
	bridge->render_scene_async(scene, render_settings, callable_mp(this, &TetraBlenderEditorPlugin::_render_done));
	_begin_request();
	emit_signal(SNAME("render_started"));
	return true;
}

void TetraBlenderEditorPlugin::_render_done(const Dictionary &p_result) {
	_note_result(p_result);
	rendering = false;
	const Ref<Image> image = p_result.get("image", Variant());
	if (!bool(p_result.get("ok", false)) || image.is_null()) {
		const String message = p_result.get("error", String());
		last_error = message;
		emit_signal(SNAME("render_finished"), false, message);
		return;
	}
	last_render = image;
	last_render_info = Dictionary();
	for (const char *key : { "width", "height", "samples", "render_time", "denoised" }) {
		last_render_info[key] = p_result.get(key, Variant());
	}
	emit_signal(SNAME("render_finished"), true, String());
}

void TetraBlenderEditorPlugin::cancel_render() {
	if (rendering) {
		// A running render cannot be interrupted any other way; the next
		// request starts Blender again.
		bridge->stop();
	}
}

bool TetraBlenderEditorPlugin::is_rendering() const {
	return rendering;
}

Ref<Image> TetraBlenderEditorPlugin::get_last_render() const {
	return last_render;
}

Dictionary TetraBlenderEditorPlugin::get_last_render_info() const {
	return last_render_info;
}

Error TetraBlenderEditorPlugin::save_render(const String &p_path) {
	ERR_FAIL_COND_V_MSG(last_render.is_null(), ERR_UNCONFIGURED, "Nothing has been rendered yet.");
	const Error error = last_render->save_png(p_path);
	if (error == OK && p_path.begins_with("res://")) {
		// A scan, not update_file(): the new picture has to be imported to
		// become a texture.
		EditorFileSystem::get_singleton()->scan_changes();
	}
	return error;
}

// --- The 3D toolbar menu ---

void TetraBlenderEditorPlugin::_menu_about_to_popup() {
	PopupMenu *menu = menu_button->get_popup();
	menu->set_item_checked(menu->get_item_index(MENU_JOIN_TRIANGLES), EDITOR_GET(SETTING_JOIN_TRIANGLES));
	menu->set_item_disabled(menu->get_item_index(MENU_RESTART), !bridge->is_running() && !bridge->is_busy());
}

void TetraBlenderEditorPlugin::_menu_option(int p_option) {
	switch (p_option) {
		case MENU_IMPORT:
			popup_import_model();
			break;
		case MENU_SHOW_MODIFIERS:
			show_tab("modifiers");
			break;
		case MENU_SHOW_OPERATORS:
			show_tab("operators");
			break;
		case MENU_SHOW_RENDER:
			show_tab("render");
			break;
		case MENU_JOIN_TRIANGLES:
			EditorSettings::get_singleton()->set_setting(SETTING_JOIN_TRIANGLES, !bool(EDITOR_GET(SETTING_JOIN_TRIANGLES)));
			break;
		case MENU_RESTART:
			restart_bridge();
			break;
		default:
			break;
	}
}

void TetraBlenderEditorPlugin::_primitive_menu_about_to_popup() {
	// Listing the primitives is what starts Blender when the menu is the
	// first thing used.
	load_schemas(SCHEMA_PRIMITIVES);
	_update_primitive_menu();
}

void TetraBlenderEditorPlugin::_update_primitive_menu() {
	primitive_menu->clear();
	switch (schema_states[SCHEMA_PRIMITIVES]) {
		case SCHEMA_LOADED: {
			for (int i = 0; i < schemas[SCHEMA_PRIMITIVES].size(); i++) {
				const Dictionary schema = schemas[SCHEMA_PRIMITIVES][i];
				primitive_menu->add_item(schema.get("label", String()));
				primitive_menu->set_item_metadata(-1, schema.get("id", String()));
				primitive_menu->set_item_tooltip(-1, schema.get("description", String()));
			}
		} break;
		case SCHEMA_FAILED: {
			primitive_menu->add_item(status == STATUS_NOT_FOUND ? TTR("Blender not found (see the Blender dock)") : TTR("Blender could not be started (see the Blender dock)"));
			primitive_menu->set_item_disabled(-1, true);
		} break;
		default: {
			primitive_menu->add_item(TTR("Starting Blender…"));
			primitive_menu->set_item_disabled(-1, true);
		} break;
	}
	if (primitive_menu->is_visible()) {
		primitive_menu->reset_size();
	}
}

void TetraBlenderEditorPlugin::_primitive_menu_option(int p_index) {
	const String id = primitive_menu->get_item_metadata(p_index);
	if (!id.is_empty()) {
		popup_add_primitive(id);
	}
}

void TetraBlenderEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			EditorNode::get_singleton()->get_editor_selection()->connect("selection_changed", callable_mp(this, &TetraBlenderEditorPlugin::_selection_changed));
			EditorSettings::get_singleton()->connect("settings_changed", callable_mp(this, &TetraBlenderEditorPlugin::_update_status));
			_update_status();
		} break;
	}
}

void TetraBlenderEditorPlugin::_bind_methods() {
	ClassDB::bind_static_method("TetraBlenderEditorPlugin", D_METHOD("get_singleton"), &TetraBlenderEditorPlugin::get_singleton);

	ClassDB::bind_method(D_METHOD("get_bridge"), &TetraBlenderEditorPlugin::get_bridge);
	ClassDB::bind_method(D_METHOD("get_dock"), &TetraBlenderEditorPlugin::get_dock);
	ClassDB::bind_method(D_METHOD("show_tab", "tab"), &TetraBlenderEditorPlugin::show_tab);
	ClassDB::bind_method(D_METHOD("get_status"), &TetraBlenderEditorPlugin::get_status);
	ClassDB::bind_method(D_METHOD("get_status_text"), &TetraBlenderEditorPlugin::get_status_text);
	ClassDB::bind_method(D_METHOD("get_status_hint"), &TetraBlenderEditorPlugin::get_status_hint);
	ClassDB::bind_method(D_METHOD("get_last_error"), &TetraBlenderEditorPlugin::get_last_error);
	ClassDB::bind_method(D_METHOD("restart_bridge"), &TetraBlenderEditorPlugin::restart_bridge);

	ClassDB::bind_method(D_METHOD("load_schemas", "kind"), &TetraBlenderEditorPlugin::load_schemas);
	ClassDB::bind_method(D_METHOD("get_schema_state", "kind"), &TetraBlenderEditorPlugin::get_schema_state);
	ClassDB::bind_method(D_METHOD("get_schemas", "kind"), &TetraBlenderEditorPlugin::get_schemas);
	ClassDB::bind_method(D_METHOD("find_schema", "kind", "id"), &TetraBlenderEditorPlugin::find_schema);

	ClassDB::bind_static_method("TetraBlenderEditorPlugin", D_METHOD("get_changed_values", "schema", "values"), &TetraBlenderEditorPlugin::get_changed_values);
	ClassDB::bind_method(D_METHOD("get_target"), &TetraBlenderEditorPlugin::get_target);

	ClassDB::bind_method(D_METHOD("get_modifier_stack", "node"), &TetraBlenderEditorPlugin::get_modifier_stack);
	ClassDB::bind_method(D_METHOD("get_modifier_label", "modifier"), &TetraBlenderEditorPlugin::get_modifier_label);
	ClassDB::bind_method(D_METHOD("add_modifier", "node", "type", "values"), &TetraBlenderEditorPlugin::add_modifier, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("set_modifier_values", "node", "index", "values"), &TetraBlenderEditorPlugin::set_modifier_values);
	ClassDB::bind_method(D_METHOD("set_modifier_enabled", "node", "index", "enabled"), &TetraBlenderEditorPlugin::set_modifier_enabled);
	ClassDB::bind_method(D_METHOD("set_modifier_object", "node", "index", "param", "path"), &TetraBlenderEditorPlugin::set_modifier_object);
	ClassDB::bind_method(D_METHOD("move_modifier", "node", "index", "to_index"), &TetraBlenderEditorPlugin::move_modifier);
	ClassDB::bind_method(D_METHOD("remove_modifier", "node", "index"), &TetraBlenderEditorPlugin::remove_modifier);
	ClassDB::bind_method(D_METHOD("apply_modifiers", "node"), &TetraBlenderEditorPlugin::apply_modifiers);
	ClassDB::bind_method(D_METHOD("clear_modifiers", "node"), &TetraBlenderEditorPlugin::clear_modifiers);
	ClassDB::bind_method(D_METHOD("evaluate_modifiers", "node"), &TetraBlenderEditorPlugin::evaluate_modifiers);
	ClassDB::bind_method(D_METHOD("is_evaluating", "node"), &TetraBlenderEditorPlugin::is_evaluating);
	ClassDB::bind_method(D_METHOD("get_evaluation_info", "node"), &TetraBlenderEditorPlugin::get_evaluation_info);
	// Called by name from the undo history.
	ClassDB::bind_method(D_METHOD("_set_modifier_state", "node", "source", "stack", "mesh"), &TetraBlenderEditorPlugin::_set_modifier_state);

	ClassDB::bind_method(D_METHOD("run_operator", "node", "id", "values"), &TetraBlenderEditorPlugin::run_operator, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("rerun_last_operator", "values"), &TetraBlenderEditorPlugin::rerun_last_operator);
	ClassDB::bind_method(D_METHOD("can_rerun_last_operator"), &TetraBlenderEditorPlugin::can_rerun_last_operator);
	ClassDB::bind_method(D_METHOD("is_operator_running"), &TetraBlenderEditorPlugin::is_operator_running);
	ClassDB::bind_method(D_METHOD("get_last_operation"), &TetraBlenderEditorPlugin::get_last_operation);

	ClassDB::bind_method(D_METHOD("add_primitive", "id", "values", "parent"), &TetraBlenderEditorPlugin::add_primitive, DEFVAL(Dictionary()), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("popup_add_primitive", "id"), &TetraBlenderEditorPlugin::popup_add_primitive);
	ClassDB::bind_method(D_METHOD("import_model", "path", "parent"), &TetraBlenderEditorPlugin::import_model, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("popup_import_model"), &TetraBlenderEditorPlugin::popup_import_model);

	ClassDB::bind_method(D_METHOD("get_render_schema"), &TetraBlenderEditorPlugin::get_render_schema);
	ClassDB::bind_method(D_METHOD("render_current_view", "settings"), &TetraBlenderEditorPlugin::render_current_view, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("cancel_render"), &TetraBlenderEditorPlugin::cancel_render);
	ClassDB::bind_method(D_METHOD("is_rendering"), &TetraBlenderEditorPlugin::is_rendering);
	ClassDB::bind_method(D_METHOD("get_last_render"), &TetraBlenderEditorPlugin::get_last_render);
	ClassDB::bind_method(D_METHOD("get_last_render_info"), &TetraBlenderEditorPlugin::get_last_render_info);
	ClassDB::bind_method(D_METHOD("save_render", "path"), &TetraBlenderEditorPlugin::save_render);

	ADD_SIGNAL(MethodInfo("status_changed"));
	ADD_SIGNAL(MethodInfo("schemas_changed", PropertyInfo(Variant::INT, "kind")));
	ADD_SIGNAL(MethodInfo("target_changed"));
	ADD_SIGNAL(MethodInfo("modifiers_changed", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "MeshInstance3D")));
	ADD_SIGNAL(MethodInfo("modifiers_evaluated", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "MeshInstance3D"), PropertyInfo(Variant::BOOL, "ok")));
	ADD_SIGNAL(MethodInfo("operator_started"));
	ADD_SIGNAL(MethodInfo("operator_finished", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "MeshInstance3D"), PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("primitive_added", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "MeshInstance3D")));
	ADD_SIGNAL(MethodInfo("model_imported", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "Node3D")));
	ADD_SIGNAL(MethodInfo("request_failed", PropertyInfo(Variant::STRING, "action"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("render_started"));
	ADD_SIGNAL(MethodInfo("render_progress", PropertyInfo(Variant::FLOAT, "fraction"), PropertyInfo(Variant::INT, "sample"), PropertyInfo(Variant::INT, "samples")));
	ADD_SIGNAL(MethodInfo("render_finished", PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(STATUS_NOT_FOUND);
	BIND_ENUM_CONSTANT(STATUS_NOT_STARTED);
	BIND_ENUM_CONSTANT(STATUS_STARTING);
	BIND_ENUM_CONSTANT(STATUS_READY);
	BIND_ENUM_CONSTANT(STATUS_WORKING);
	BIND_ENUM_CONSTANT(STATUS_FAILED);
	BIND_ENUM_CONSTANT(SCHEMA_MODIFIERS);
	BIND_ENUM_CONSTANT(SCHEMA_OPERATORS);
	BIND_ENUM_CONSTANT(SCHEMA_PRIMITIVES);
	BIND_ENUM_CONSTANT(SCHEMA_NOT_LOADED);
	BIND_ENUM_CONSTANT(SCHEMA_LOADING);
	BIND_ENUM_CONSTANT(SCHEMA_LOADED);
	BIND_ENUM_CONSTANT(SCHEMA_FAILED);
}

TetraBlenderEditorPlugin::TetraBlenderEditorPlugin() {
	singleton = this;
	bridge = TetraBlenderBridge::get_shared();
	bridge->connect("started", callable_mp(this, &TetraBlenderEditorPlugin::_bridge_started));
	bridge->connect("stopped", callable_mp(this, &TetraBlenderEditorPlugin::_bridge_stopped));
	bridge->connect("progress", callable_mp(this, &TetraBlenderEditorPlugin::_bridge_progress));

	EDITOR_DEF(SETTING_JOIN_TRIANGLES, true);

	status_timer = memnew(Timer);
	status_timer->set_wait_time(0.1);
	status_timer->connect("timeout", callable_mp(this, &TetraBlenderEditorPlugin::_update_status));
	add_child(status_timer);

	evaluation_timer = memnew(Timer);
	evaluation_timer->set_wait_time(0.15);
	evaluation_timer->set_one_shot(true);
	evaluation_timer->connect("timeout", callable_mp(this, &TetraBlenderEditorPlugin::_evaluation_timeout));
	add_child(evaluation_timer);

	menu_button = memnew(MenuButton);
	menu_button->set_text(TTRC("Blender"));
	menu_button->set_flat(false);
	menu_button->set_theme_type_variation("FlatMenuButton");
	menu_button->set_tooltip_text(TTRC("Blender's modeling tools and Cycles rendering."));
	Node3DEditor::get_singleton()->add_control_to_menu_panel(menu_button);

	PopupMenu *menu = menu_button->get_popup();
	primitive_menu = memnew(PopupMenu);
	primitive_menu->connect("about_to_popup", callable_mp(this, &TetraBlenderEditorPlugin::_primitive_menu_about_to_popup));
	primitive_menu->connect("index_pressed", callable_mp(this, &TetraBlenderEditorPlugin::_primitive_menu_option));
	menu->add_submenu_node_item(TTRC("Add Mesh"), primitive_menu, MENU_ADD_MESH);
	menu->add_item(TTRC("Import Model..."), MENU_IMPORT);
	menu->add_separator();
	menu->add_item(TTRC("Modifiers"), MENU_SHOW_MODIFIERS);
	menu->add_item(TTRC("Operators"), MENU_SHOW_OPERATORS);
	menu->add_item(TTRC("Render"), MENU_SHOW_RENDER);
	menu->add_separator();
	menu->add_check_item(TTRC("Join Triangles into Quads"), MENU_JOIN_TRIANGLES);
	menu->set_item_tooltip(-1, TTRC("Godot meshes are triangles. Before modifiers and operators run, pairs of triangles are merged back into the quads Blender's tools expect."));
	menu->add_item(TTRC("Restart Blender"), MENU_RESTART);
	menu->connect("about_to_popup", callable_mp(this, &TetraBlenderEditorPlugin::_menu_about_to_popup));
	menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraBlenderEditorPlugin::_menu_option));
	connect("schemas_changed", callable_mp(this, &TetraBlenderEditorPlugin::_update_primitive_menu).unbind(1));

	primitive_dialog = memnew(TetraOperationDialog);
	primitive_dialog->connect("operation_confirmed", callable_mp(this, &TetraBlenderEditorPlugin::_primitive_confirmed));
	add_child(primitive_dialog);

	import_dialog = memnew(EditorFileDialog);
	import_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	import_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	import_dialog->set_title(TTRC("Import Model with Blender"));
	import_dialog->add_filter("*.blend, *.fbx, *.obj, *.ply, *.stl, *.usd, *.usda, *.usdc, *.usdz, *.abc, *.gltf, *.glb, *.dae", TTRC("Models Blender reads"));
	import_dialog->connect("file_selected", callable_mp(this, &TetraBlenderEditorPlugin::import_model).bind((Node *)nullptr));
	add_child(import_dialog);

	dock = memnew(TetraBlenderDock(this));
	EditorDockManager::get_singleton()->add_dock(dock);
}

TetraBlenderEditorPlugin::~TetraBlenderEditorPlugin() {
	singleton = nullptr;
}
