#pragma once

#include "../tetra_edit_mesh.h"

#include "editor/plugins/editor_plugin.h"

class Button;
class Camera3D;
class HBoxContainer;
class MenuButton;
class MeshInstance3D;
class PopupMenu;
class ShaderMaterial;
class TetraOperationDialog;

// Blender-style mesh Edit Mode in the 3D viewport.
//
// The edited MeshInstance3D keeps its TetraEditMesh in the node metadata
// "tetra_edit_mesh"; the node's mesh is the ArrayMesh generated from it. The
// overlay (edges, vertex points, face dots, selected faces) is real 3D
// geometry on the editor's gizmo layer. Every operation is one undo action
// that swaps snapshots of the edit mesh.
//
// Scripts and tests reach it through TetraMeshEditPlugin.get_singleton().
class TetraMeshEditPlugin : public EditorPlugin {
	GDCLASS(TetraMeshEditPlugin, EditorPlugin);

public:
	enum ModalKind {
		MODAL_NONE,
		MODAL_GRAB,
		MODAL_ROTATE,
		MODAL_SCALE,
		MODAL_INSET,
		MODAL_LOOP_CUT,
	};

private:
	enum Constraint {
		CONSTRAINT_NONE,
		CONSTRAINT_AXIS,
		CONSTRAINT_PLANE,
	};

	enum OverlayKind {
		OVERLAY_EDGES,
		OVERLAY_POINTS,
		OVERLAY_FACES,
	};

	static inline TetraMeshEditPlugin *singleton = nullptr;

	MeshInstance3D *target = nullptr; // The selected MeshInstance3D, if any.
	MeshInstance3D *edited = nullptr; // The node in edit mode, if any.
	Ref<TetraEditMesh> mesh;
	Ref<Mesh> display_mesh; // What the plugin last gave the node.

	// Toolbar.
	HBoxContainer *toolbar = nullptr;
	Button *edit_button = nullptr;
	Button *mode_buttons[3] = {};
	MenuButton *select_menu = nullptr;
	MenuButton *mesh_menu = nullptr;
	MenuButton *add_menu = nullptr;
	PopupMenu *add_mesh_menu = nullptr;
	PopupMenu *delete_menu = nullptr;
	PopupMenu *merge_menu = nullptr;
	TetraOperationDialog *dialog = nullptr;
	Array operations;
	Dictionary last_values; // Operation id -> values of its last run.
	PackedByteArray dialog_before;
	bool dialog_adds_node = false;

	// Overlay.
	Ref<ArrayMesh> overlay_mesh;
	RID overlay_instance;
	Ref<Shader> overlay_shader;
	Ref<Shader> overlay_point_shader;
	Ref<Shader> overlay_fill_shader;
	Ref<ShaderMaterial> edge_material;
	Ref<ShaderMaterial> point_material;
	Ref<ShaderMaterial> fill_material;
	LocalVector<OverlayKind> overlay_surfaces;
	Transform3D overlay_transform;

	// Picking.
	LocalVector<Vector3> pick_triangles; // Three per triangle, local space.
	LocalVector<int> pick_triangle_faces;

	// Mouse.
	Camera3D *last_camera = nullptr;
	Point2 last_mouse;
	bool have_mouse = false;
	bool box_pending = false;
	bool box_dragging = false;
	bool box_extend = false;
	Point2 box_start;
	bool swallow_release = false;

	// Modal operation.
	ModalKind modal = MODAL_NONE;
	String modal_name;
	PackedByteArray modal_before; // For the undo action.
	PackedByteArray modal_base; // The mesh when the modal part began.
	PackedVector3Array modal_positions;
	Vector3 modal_pivot;
	Point2 modal_start_mouse;
	Constraint modal_constraint = CONSTRAINT_NONE;
	Vector3 modal_axis;
	String modal_axis_name;
	String modal_typed;
	bool modal_snap = false;
	bool modal_keep_on_cancel = false;
	String modal_readout;
	int loop_cut_edge = -1;
	int loop_cut_count = 1;
	LocalVector<Vector3> loop_cut_preview; // Pairs of local points.

	void _build_operations();
	void _update_toolbar();
	void _edit_toggled(bool p_pressed);
	void _mode_pressed(int p_mode);
	void _select_menu_pressed(int p_id);
	void _mesh_menu_pressed(int p_id);
	void _add_menu_pressed(int p_id);
	void _delete_menu_pressed(int p_id);
	void _merge_menu_pressed(int p_id);
	void _popup_at_mouse(PopupMenu *p_menu);
	void _request_operation(const String &p_id);
	void _dialog_preview(const Dictionary &p_values);
	void _dialog_confirmed(const String &p_id, const Dictionary &p_values);
	void _dialog_cancelled(const String &p_id);
	bool _execute(const String &p_id, const Dictionary &p_values);
	void _commit(const String &p_name, const PackedByteArray &p_before);
	void _apply_state(Node *p_node, const PackedByteArray &p_state);
	void _set_edit_mesh(Node *p_node, const Ref<TetraEditMesh> &p_mesh, const Ref<Mesh> &p_node_mesh);
	void _add_to_scene(Node *p_node, const String &p_action);
	void _target_exiting();
	void _set_selection_box_visible(bool p_visible);

	void _refresh(bool p_topology = true);
	void _create_overlay();
	void _free_overlay();
	void _rebuild_overlay();
	void _overlay_positions(OverlayKind p_kind, Vector<Vector3> &r_positions) const;
	void _update_overlay_positions();
	void _rebuild_pick_cache();

	Camera3D *_camera() const;
	Control *_surface_of(Camera3D *p_camera) const;
	bool _project(Camera3D *p_camera, const Vector3 &p_local, Point2 &r_screen) const;
	bool _is_occluded(Camera3D *p_camera, const Vector3 &p_local) const;
	int _pick_vertex(Camera3D *p_camera, const Point2 &p_position, real_t p_radius = 14.0) const;
	int _pick_edge(Camera3D *p_camera, const Point2 &p_position, real_t p_radius = 12.0, bool p_visible_only = true) const;
	int _pick_face(Camera3D *p_camera, const Point2 &p_position) const;
	void _click_select(Camera3D *p_camera, const Point2 &p_position, bool p_extend, bool p_loop);
	void _box_select(Camera3D *p_camera, const Rect2 &p_rect, bool p_extend);
	void _select_linked_under_mouse(bool p_select);

	void _begin_modal(ModalKind p_kind, const String &p_name, const PackedByteArray &p_before, bool p_keep_on_cancel);
	void _update_modal();
	void _finish_modal(bool p_confirm);
	void _set_constraint(const Vector3 &p_axis, const String &p_name, bool p_plane);
	bool _modal_key(const Ref<InputEventKey> &p_key);
	bool _edit_key(const Ref<InputEventKey> &p_key);
	void _accept_event();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static TetraMeshEditPlugin *get_singleton() { return singleton; }

	virtual String get_plugin_name() const override { return "TetraMeshEdit"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual void forward_3d_draw_over_viewport(Control *p_overlay) override;

	bool enter_edit_mode(MeshInstance3D *p_node);
	void exit_edit_mode();
	bool is_edit_mode() const { return edited != nullptr; }
	MeshInstance3D *get_edited_node() const { return edited; }
	Ref<TetraEditMesh> get_edit_mesh() const { return mesh; }
	void set_select_mode(TetraEditMesh::SelectMode p_mode);
	TetraEditMesh::SelectMode get_select_mode() const;
	Array get_operations() const { return operations; }
	bool run_operation(const String &p_id, const Dictionary &p_values = Dictionary());
	// What a menu entry does: runs at once, or opens the parameter dialog.
	void request_operation(const String &p_id);
	TetraOperationDialog *get_operation_dialog() const;
	bool start_modal(ModalKind p_kind);
	ModalKind get_modal() const { return modal; }
	String get_modal_readout() const { return modal_readout; }
	MeshInstance3D *add_mesh(const String &p_primitive, const Dictionary &p_values = Dictionary());
	Dictionary get_overlay_counts() const;
	Dictionary pick(Camera3D *p_camera, const Point2 &p_position) const;
	void refresh();

	TetraMeshEditPlugin();
	~TetraMeshEditPlugin();
};

VARIANT_ENUM_CAST(TetraMeshEditPlugin::ModalKind);
