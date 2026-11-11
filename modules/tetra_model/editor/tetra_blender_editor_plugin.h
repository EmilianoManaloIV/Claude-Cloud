#pragma once

#include "../tetra_blender_bridge.h"

#include "core/io/image.h"
#include "core/templates/hash_map.h"
#include "editor/plugins/editor_plugin.h"

class Camera3D;
class EditorFileDialog;
class MenuButton;
class MeshInstance3D;
class PopupMenu;
class TetraBlenderDock;
class TetraOperationDialog;
class Timer;

// Blender inside the 3D editor: the "Blender" dock (modifier stack, operators,
// Cycles rendering), the "Blender" menu of the 3D toolbar (Add Mesh, Import
// Model) and the logic behind both. The dock and the menu are views; every
// feature is a bound method here, so tool scripts can drive it:
//
//   var blender := TetraBlenderEditorPlugin.get_singleton()
//   blender.add_modifier(node, "SUBSURF", {"levels": 2})
//   await blender.modifiers_evaluated
//
// Blender is started by the first call that needs it and every call returns
// at once: results arrive through signals. Everything that changes the scene
// is one action in the editor's undo history.
class TetraBlenderEditorPlugin : public EditorPlugin {
	GDCLASS(TetraBlenderEditorPlugin, EditorPlugin);

public:
	enum Status {
		// No Python with bpy and no Blender executable is configured.
		STATUS_NOT_FOUND,
		STATUS_NOT_STARTED,
		STATUS_STARTING,
		STATUS_READY,
		// Blender is computing a request.
		STATUS_WORKING,
		// Blender could not be started or stopped unexpectedly.
		STATUS_FAILED,
	};

	enum SchemaKind {
		SCHEMA_MODIFIERS,
		SCHEMA_OPERATORS,
		SCHEMA_PRIMITIVES,
		SCHEMA_MAX,
	};

	enum SchemaState {
		SCHEMA_NOT_LOADED,
		SCHEMA_LOADING,
		SCHEMA_LOADED,
		SCHEMA_FAILED,
	};

private:
	enum MenuOption {
		MENU_ADD_MESH,
		MENU_IMPORT,
		MENU_SHOW_MODIFIERS,
		MENU_SHOW_OPERATORS,
		MENU_SHOW_RENDER,
		MENU_JOIN_TRIANGLES,
		MENU_RESTART,
	};

	// The asynchronous evaluation of one node's modifier stack.
	struct Evaluation {
		// Generation of the node's newest stack and of the request in flight.
		uint64_t wanted = 0;
		uint64_t sent = 0;
		bool in_flight = false;
		// The stack changed after the request in flight was sent.
		bool dirty = false;
		String error;
		Dictionary counts;
	};

	struct LastOperation {
		ObjectID node;
		String id;
		String action_name;
		Dictionary values;
		// The mesh before and after the operator.
		Ref<Mesh> base;
		Ref<Mesh> result;
		int history_id = 0;
		uint64_t history_version = 0;
	};

	static TetraBlenderEditorPlugin *singleton;

	Ref<TetraBlenderBridge> bridge;
	TetraBlenderDock *dock = nullptr;
	MenuButton *menu_button = nullptr;
	PopupMenu *primitive_menu = nullptr;
	TetraOperationDialog *primitive_dialog = nullptr;
	EditorFileDialog *import_dialog = nullptr;
	Timer *status_timer = nullptr;
	Timer *evaluation_timer = nullptr;

	Status status = STATUS_NOT_STARTED;
	String status_text;
	String status_hint;
	// Why Blender is not running although it was asked to.
	String start_error;
	String last_error;

	SchemaState schema_states[SCHEMA_MAX] = {};
	Array schemas[SCHEMA_MAX];

	ObjectID target;

	HashMap<ObjectID, Evaluation> evaluations;
	uint64_t generation_counter = 0;

	bool operator_running = false;
	LastOperation last_operation;

	HashMap<String, Dictionary> primitive_values;

	bool rendering = false;
	Ref<Image> last_render;
	Dictionary last_render_info;

	void _begin_request();
	void _note_result(const Dictionary &p_result);
	void _fail(const String &p_action, const String &p_message);
	void _update_status();
	void _bridge_started();
	void _bridge_stopped();
	void _bridge_progress(const Dictionary &p_event);

	void _schemas_loaded(const Dictionary &p_result, int p_kind);

	void _selection_changed();

	void _commit_stack(MeshInstance3D *p_node, const String &p_action, const Array &p_stack, bool p_merge = false);
	void _set_modifier_state(Node *p_node, const Ref<Mesh> &p_source, const Array &p_stack, const Ref<Mesh> &p_mesh);
	void _queue_evaluation(MeshInstance3D *p_node);
	void _evaluation_timeout();
	void _send_evaluation(ObjectID p_node);
	void _evaluation_done(const Dictionary &p_result, uint64_t p_node, uint64_t p_generation);

	bool _start_operator(MeshInstance3D *p_node, const String &p_id, const Dictionary &p_values, const Ref<Mesh> &p_base, bool p_rerun);
	void _operator_done(const Dictionary &p_result, uint64_t p_node, const String &p_id, const Dictionary &p_values, const Ref<Mesh> &p_base, bool p_rerun);
	void _finish_operator(MeshInstance3D *p_node, bool p_ok, const String &p_message);
	bool _last_operation_is_newest_action() const;

	Node *_resolve_parent(Node *p_parent) const;
	void _add_to_scene(Node *p_node, Node *p_parent, const String &p_action);
	void _primitive_done(const Dictionary &p_result, const String &p_id, uint64_t p_parent);
	void _primitive_confirmed(const String &p_id, const Dictionary &p_values);
	void _import_done(const Dictionary &p_result, const String &p_path, uint64_t p_parent);

	void _render_done(const Dictionary &p_result);

	void _menu_about_to_popup();
	void _menu_option(int p_option);
	void _primitive_menu_about_to_popup();
	void _primitive_menu_option(int p_index);
	void _update_primitive_menu();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static TetraBlenderEditorPlugin *get_singleton();

	virtual String get_plugin_name() const override;

	Ref<TetraBlenderBridge> get_bridge() const;
	TetraBlenderDock *get_dock() const;
	// Opens the dock on "modifiers", "operators" or "render".
	void show_tab(const String &p_tab);

	Status get_status() const;
	// One line such as "Blender 5.3.0 ready".
	String get_status_text() const;
	// What to do about the status: where Blender is looked for, or why it failed.
	String get_status_hint() const;
	// Message of the last request that failed.
	String get_last_error() const;
	// Ends the Blender process; the next request starts a new one.
	void restart_bridge();

	// Asks Blender for the operation schemas of one kind unless they are
	// loaded or on their way; "schemas_changed" tells when the state changes.
	void load_schemas(SchemaKind p_kind);
	SchemaState get_schema_state(SchemaKind p_kind) const;
	Array get_schemas(SchemaKind p_kind) const;
	// The loaded schema with this id ("blender:modifier.BEVEL"; for modifiers
	// the bare type "BEVEL" works too), or an empty Dictionary.
	Dictionary find_schema(SchemaKind p_kind, const String &p_id) const;

	// The values that differ from the schema's defaults. Only these are sent
	// to Blender, which then applies its own defaults as its interface would.
	static Dictionary get_changed_values(const Dictionary &p_schema, const Dictionary &p_values);

	// The MeshInstance3D the dock works on: the only one in the selection.
	MeshInstance3D *get_target() const;

	// The modifier stack of a node (see TetraBlenderScene). Each change is
	// one undoable action, after which Blender evaluates the stack and
	// "modifiers_evaluated" is emitted.
	Array get_modifier_stack(MeshInstance3D *p_node) const;
	// The name of a stack entry as Blender shows it ("Subdivision Surface").
	String get_modifier_label(const Dictionary &p_modifier) const;
	int add_modifier(MeshInstance3D *p_node, const String &p_type, const Dictionary &p_values = Dictionary());
	// Merges p_values into the modifier's values. Calls in quick succession
	// (a slider being dragged) are one undo step.
	void set_modifier_values(MeshInstance3D *p_node, int p_index, const Dictionary &p_values);
	void set_modifier_enabled(MeshInstance3D *p_node, int p_index, bool p_enabled);
	// Points an object parameter ("object" of a Boolean) at another
	// MeshInstance3D; an empty path clears it.
	void set_modifier_object(MeshInstance3D *p_node, int p_index, const String &p_param, const NodePath &p_path);
	void move_modifier(MeshInstance3D *p_node, int p_index, int p_to_index);
	void remove_modifier(MeshInstance3D *p_node, int p_index);
	// Makes the evaluated mesh the node's own and drops the stack. Fails
	// while an evaluation is under way.
	bool apply_modifiers(MeshInstance3D *p_node);
	// Drops the stack and puts the original mesh back.
	void clear_modifiers(MeshInstance3D *p_node);
	// Evaluates the stack again, for example after an object it refers to moved.
	void evaluate_modifiers(MeshInstance3D *p_node);
	bool is_evaluating(MeshInstance3D *p_node) const;
	// {evaluating: bool, error: String, vertex_count, polygon_count, triangle_count}
	Dictionary get_evaluation_info(MeshInstance3D *p_node) const;

	// Runs one Blender operator on the node's mesh (on the source mesh while
	// the node has modifiers) with everything selected. "operator_finished"
	// reports the outcome; a success is one undoable action.
	bool run_operator(MeshInstance3D *p_node, const String &p_id, const Dictionary &p_values = Dictionary());
	// Blender's "Adjust Last Operation": runs the last operator again on the
	// mesh it started from and replaces its result.
	bool rerun_last_operator(const Dictionary &p_values);
	bool can_rerun_last_operator() const;
	bool is_operator_running() const;
	// {node: MeshInstance3D, id: String, values: Dictionary}, empty before the first operator.
	Dictionary get_last_operation() const;

	// Adds a MeshInstance3D with a Blender primitive below p_parent (default:
	// the selected node, else the scene root). "primitive_added" follows.
	void add_primitive(const String &p_id, const Dictionary &p_values = Dictionary(), Node *p_parent = nullptr);
	// Opens the parameter dialog of a primitive; OK adds it.
	void popup_add_primitive(const String &p_id);
	// Imports a file Blender reads as MeshInstance3D nodes below a new
	// Node3D. "model_imported" follows.
	void import_model(const String &p_path, Node *p_parent = nullptr);
	void popup_import_model();

	// The parameters of a render as an operation schema: camera, width,
	// height, match_viewport, samples, denoise, transparent.
	Dictionary get_render_schema() const;
	// Renders the edited scene with Cycles through the camera of the 3D
	// viewport (or the scene's Camera3D with {"camera": "scene"}).
	// "render_progress" and "render_finished" follow.
	bool render_current_view(const Dictionary &p_settings = Dictionary());
	// Stops a render by ending the Blender process.
	void cancel_render();
	bool is_rendering() const;
	Ref<Image> get_last_render() const;
	// {width, height, samples, render_time, denoised} of the last render.
	Dictionary get_last_render_info() const;
	// Writes the last render as a PNG and tells the editor's file system.
	Error save_render(const String &p_path);

	TetraBlenderEditorPlugin();
	~TetraBlenderEditorPlugin();
};

VARIANT_ENUM_CAST(TetraBlenderEditorPlugin::Status);
VARIANT_ENUM_CAST(TetraBlenderEditorPlugin::SchemaKind);
VARIANT_ENUM_CAST(TetraBlenderEditorPlugin::SchemaState);
