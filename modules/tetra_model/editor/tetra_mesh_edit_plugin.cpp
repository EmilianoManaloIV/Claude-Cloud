#include "tetra_mesh_edit_plugin.h"

#include "tetra/editor/tetra_operation_dialog.h"

#include "core/input/input.h"
#include "core/math/geometry_2d.h"
#include "core/math/geometry_3d.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/scene_tree_editor.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/debugger/view_3d_controller.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

namespace {

const char *META_EDIT_MESH = "tetra_edit_mesh";

// Blender's edit mode theme: black wire and vertices, orange selection.
const Color COLOR_EDGE = Color(0.02, 0.02, 0.02);
const Color COLOR_EDGE_SELECTED = Color(1.0, 0.6, 0.0);
const Color COLOR_VERTEX = Color(0.0, 0.0, 0.0);
const Color COLOR_VERTEX_SELECTED = Color(1.0, 0.48, 0.0);
const Color COLOR_FACE_SELECTED = Color(1.0, 0.52, 0.0, 0.35);
const Color COLOR_PREVIEW = Color(1.0, 0.95, 0.2);

// The overlay is pulled a little towards the camera so that it does not fight
// with the surface it lies on. Scaling the view-space position keeps a
// perspective projection's screen position unchanged.
// Lines and points need separate shaders: a shader that writes POINT_SIZE
// makes the Compatibility renderer draw a line surface as points.
const char *OVERLAY_LINE_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, skip_vertex_transform, shadows_disabled, fog_disabled;
uniform float bias = 0.004;
void vertex() {
	vec4 view = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	if (PROJECTION_MATRIX[3][3] == 0.0) {
		view.xyz *= 1.0 - bias;
	} else {
		view.z += bias * 10.0;
	}
	VERTEX = view.xyz;
}
void fragment() {
	ALBEDO = OUTPUT_IS_SRGB ? COLOR.rgb : pow(COLOR.rgb, vec3(2.2));
}
)";

const char *OVERLAY_POINT_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, skip_vertex_transform, shadows_disabled, fog_disabled;
uniform float bias = 0.008;
uniform float point_size = 8.0;
void vertex() {
	vec4 view = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	if (PROJECTION_MATRIX[3][3] == 0.0) {
		view.xyz *= 1.0 - bias;
	} else {
		view.z += bias * 10.0;
	}
	VERTEX = view.xyz;
	POINT_SIZE = point_size;
}
void fragment() {
	if (length(POINT_COORD - vec2(0.5)) > 0.5) {
		discard;
	}
	ALBEDO = OUTPUT_IS_SRGB ? COLOR.rgb : pow(COLOR.rgb, vec3(2.2));
}
)";

const char *OVERLAY_FILL_SHADER = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, skip_vertex_transform, shadows_disabled, fog_disabled, blend_mix, depth_draw_never;
uniform float bias = 0.002;
void vertex() {
	vec4 view = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	if (PROJECTION_MATRIX[3][3] == 0.0) {
		view.xyz *= 1.0 - bias;
	} else {
		view.z += bias * 10.0;
	}
	VERTEX = view.xyz;
}
void fragment() {
	ALBEDO = OUTPUT_IS_SRGB ? COLOR.rgb : pow(COLOR.rgb, vec3(2.2));
	ALPHA = COLOR.a;
}
)";

Dictionary make_param(const String &p_name, const String &p_label, const String &p_type, const Variant &p_default) {
	Dictionary param;
	param["name"] = p_name;
	param["label"] = p_label;
	param["type"] = p_type;
	param["default"] = p_default;
	return param;
}

Dictionary with_range(Dictionary p_param, double p_min, double p_max, double p_ui_min, double p_ui_max, double p_step) {
	p_param["min"] = p_min;
	p_param["max"] = p_max;
	p_param["ui_min"] = p_ui_min;
	p_param["ui_max"] = p_ui_max;
	p_param["step"] = p_step;
	return p_param;
}

Basis outer_product(const Vector3 &p_a) {
	return Basis(p_a * p_a.x, p_a * p_a.y, p_a * p_a.z);
}

Basis add_basis(const Basis &p_a, const Basis &p_b) {
	return Basis(p_a.get_column(0) + p_b.get_column(0), p_a.get_column(1) + p_b.get_column(1), p_a.get_column(2) + p_b.get_column(2));
}

// The parameter of the point on the line p_point + t * p_axis closest to a ray.
real_t closest_on_axis(const Vector3 &p_point, const Vector3 &p_axis, const Vector3 &p_from, const Vector3 &p_dir) {
	Vector3 w = p_point - p_from;
	real_t b = p_axis.dot(p_dir);
	real_t d = p_axis.dot(w);
	real_t e = p_dir.dot(w);
	real_t denominator = 1.0 - b * b;
	if (Math::abs(denominator) < 1e-6) {
		return 0;
	}
	return (b * e - d) / denominator;
}

// The 3D viewport rewrites the digit keys to numpad keys when "Emulate
// Numpad" is on; the physical key says what was really pressed.
Key effective_key(const Ref<InputEventKey> &p_key) {
	Key key = p_key->get_keycode();
	Key physical = p_key->get_physical_keycode();
	if (key >= Key::KP_0 && key <= Key::KP_9 && physical >= Key::KEY_0 && physical <= Key::KEY_9) {
		return physical;
	}
	return key;
}

String format_number(real_t p_value) {
	return String::num(p_value, 3);
}

} // namespace

// ---------------------------------------------------------------------------
// Operations.

void TetraMeshEditPlugin::_build_operations() {
	struct Entry {
		const char *id;
		const char *label;
		const char *category;
	};
	static const Entry entries[] = {
		{ "select_all", "All (A)", "Select" },
		{ "select_none", "None (Alt+A)", "Select" },
		{ "select_invert", "Invert (Ctrl+I)", "Select" },
		{ "select_linked", "Linked (L: under the cursor)", "Select" },
		{ "select_more", "More (Ctrl+Numpad +)", "Select" },
		{ "select_less", "Less (Ctrl+Numpad -)", "Select" },
		{ "grab", "Move (G)", "Transform" },
		{ "rotate_modal", "Rotate (R)", "Transform" },
		{ "scale_modal", "Scale (S)", "Transform" },
		{ "translate", "Move by...", "Transform" },
		{ "rotate", "Rotate by...", "Transform" },
		{ "scale", "Scale by...", "Transform" },
		{ "extrude", "Extrude Region (E)", "Add" },
		{ "extrude_faces_individual", "Extrude Individual Faces...", "Add" },
		{ "inset", "Inset Faces (I)", "Add" },
		{ "inset_faces", "Inset Faces...", "Add" },
		{ "loop_cut", "Loop Cut (Ctrl+R)", "Add" },
		{ "subdivide", "Subdivide...", "Add" },
		{ "duplicate", "Duplicate (Shift+D)", "Add" },
		{ "fill", "New Edge/Face from Vertices (F)", "Add" },
		{ "bridge_edge_loops", "Bridge Edge Loops", "Add" },
		{ "delete_vertices", "Delete Vertices", "Delete" },
		{ "delete_edges", "Delete Edges", "Delete" },
		{ "delete_faces", "Delete Faces", "Delete" },
		{ "delete_only_faces", "Delete Only Faces", "Delete" },
		{ "delete_only_edges_faces", "Delete Only Edges & Faces", "Delete" },
		{ "dissolve_vertices", "Dissolve Vertices", "Delete" },
		{ "dissolve_edges", "Dissolve Edges", "Delete" },
		{ "dissolve_faces", "Dissolve Faces", "Delete" },
		{ "merge_center", "Merge at Center", "Merge" },
		{ "merge_collapse", "Merge Collapse", "Merge" },
		{ "merge_by_distance", "Merge by Distance...", "Merge" },
		{ "flip_normals", "Flip Normals", "Normals" },
		{ "recalculate_normals", "Recalculate Outside (Shift+N)", "Normals" },
		{ "recalculate_normals_inside", "Recalculate Inside", "Normals" },
		{ "shade_smooth", "Shade Smooth", "Normals" },
		{ "shade_flat", "Shade Flat", "Normals" },
		{ "triangulate", "Triangulate Faces", "Faces" },
		{ "tris_to_quads", "Triangles to Quads...", "Faces" },
		{ "separate", "Separate Selection", "Faces" },
	};
	operations.clear();
	for (const Entry &entry : entries) {
		Dictionary operation;
		operation["id"] = entry.id;
		operation["label"] = TTR(entry.label);
		operation["category"] = entry.category;
		operation["source"] = "native";
		Array params;
		String id = entry.id;
		if (id == "translate") {
			params.push_back(make_param("offset", "Offset", "vec3", Vector3()));
		} else if (id == "rotate") {
			Dictionary axis = make_param("axis", "Axis", "enum", 1);
			Array options;
			const char *names[3] = { "X", "Y", "Z" };
			for (int i = 0; i < 3; i++) {
				Dictionary option;
				option["value"] = i;
				option["label"] = names[i];
				options.push_back(option);
			}
			axis["options"] = options;
			params.push_back(axis);
			Dictionary angle = with_range(make_param("angle", "Angle", "float", 90.0), -36000, 36000, -180, 180, 0.1);
			angle["unit"] = String::utf8("°");
			params.push_back(angle);
		} else if (id == "scale") {
			params.push_back(make_param("scale", "Scale", "vec3", Vector3(1, 1, 1)));
		} else if (id == "extrude_faces_individual") {
			params.push_back(with_range(make_param("offset", "Offset", "float", 0.5), -10000, 10000, -2, 2, 0.001));
		} else if (id == "inset_faces") {
			params.push_back(with_range(make_param("thickness", "Thickness", "float", 0.1), 0, 10000, 0, 1, 0.001));
			params.push_back(with_range(make_param("depth", "Depth", "float", 0.0), -10000, 10000, -1, 1, 0.001));
		} else if (id == "subdivide") {
			params.push_back(with_range(make_param("cuts", "Number of Cuts", "int", 1), 1, 100, 1, 10, 1));
		} else if (id == "merge_by_distance") {
			params.push_back(with_range(make_param("distance", "Merge Distance", "float", 0.0001), 0, 10000, 0, 1, 0.0001));
		} else if (id == "tris_to_quads") {
			Dictionary face = with_range(make_param("face_angle", "Max Face Angle", "float", 40.0), 0, 180, 0, 180, 0.1);
			face["unit"] = String::utf8("°");
			Dictionary shape = with_range(make_param("shape_angle", "Max Shape Angle", "float", 40.0), 0, 180, 0, 180, 0.1);
			shape["unit"] = String::utf8("°");
			params.push_back(face);
			params.push_back(shape);
		}
		operation["params"] = params;
		operations.push_back(operation);
	}
}

bool TetraMeshEditPlugin::_execute(const String &p_id, const Dictionary &p_values) {
	if (mesh.is_null()) {
		return false;
	}
	if (p_id.begins_with("add:")) {
		return mesh->add_primitive(p_id.trim_prefix("add:"), p_values);
	}
	if (p_id == "select_all") {
		mesh->select_all();
	} else if (p_id == "select_none") {
		mesh->select_none();
	} else if (p_id == "select_invert") {
		mesh->select_invert();
	} else if (p_id == "select_linked") {
		mesh->select_linked();
	} else if (p_id == "select_more") {
		mesh->select_more();
	} else if (p_id == "select_less") {
		mesh->select_less();
	} else if (p_id == "translate") {
		if (mesh->get_selected_vertices().is_empty()) {
			return false;
		}
		mesh->translate_selection(p_values.get("offset", Vector3()));
	} else if (p_id == "rotate") {
		if (mesh->get_selected_vertices().is_empty()) {
			return false;
		}
		Vector3 axis;
		axis[CLAMP(int(p_values.get("axis", 1)), 0, 2)] = 1;
		mesh->rotate_selection(axis, Math::deg_to_rad(float(p_values.get("angle", 90.0))));
	} else if (p_id == "scale") {
		if (mesh->get_selected_vertices().is_empty()) {
			return false;
		}
		mesh->scale_selection(p_values.get("scale", Vector3(1, 1, 1)));
	} else if (p_id == "extrude") {
		return mesh->extrude_region();
	} else if (p_id == "extrude_faces_individual") {
		return mesh->extrude_faces_individual(p_values.get("offset", 0.5));
	} else if (p_id == "inset_faces" || p_id == "inset") {
		return mesh->inset_faces(p_values.get("thickness", 0.1), p_values.get("depth", 0.0));
	} else if (p_id == "loop_cut") {
		int edge = p_values.get("edge", -1);
		if (edge < 0 || edge >= mesh->get_edge_count()) {
			return false;
		}
		return mesh->loop_cut(edge, p_values.get("cuts", 1));
	} else if (p_id == "subdivide") {
		return mesh->subdivide(p_values.get("cuts", 1));
	} else if (p_id == "duplicate") {
		return mesh->duplicate_selection();
	} else if (p_id == "fill") {
		return mesh->fill();
	} else if (p_id == "bridge_edge_loops") {
		return mesh->bridge_edge_loops();
	} else if (p_id == "delete_vertices") {
		return mesh->delete_selection(TetraEditMesh::DELETE_VERTICES);
	} else if (p_id == "delete_edges") {
		return mesh->delete_selection(TetraEditMesh::DELETE_EDGES);
	} else if (p_id == "delete_faces") {
		return mesh->delete_selection(TetraEditMesh::DELETE_FACES);
	} else if (p_id == "delete_only_faces") {
		return mesh->delete_selection(TetraEditMesh::DELETE_ONLY_FACES);
	} else if (p_id == "delete_only_edges_faces") {
		return mesh->delete_selection(TetraEditMesh::DELETE_ONLY_EDGES_AND_FACES);
	} else if (p_id == "dissolve_vertices") {
		return mesh->dissolve_vertices();
	} else if (p_id == "dissolve_edges") {
		return mesh->dissolve_edges();
	} else if (p_id == "dissolve_faces") {
		return mesh->dissolve_faces();
	} else if (p_id == "merge_center") {
		return mesh->merge_at_center();
	} else if (p_id == "merge_collapse") {
		return mesh->merge_collapse();
	} else if (p_id == "merge_by_distance") {
		return mesh->merge_by_distance(p_values.get("distance", 0.0001)) > 0;
	} else if (p_id == "flip_normals") {
		return mesh->flip_normals();
	} else if (p_id == "recalculate_normals") {
		return mesh->recalculate_normals(false);
	} else if (p_id == "recalculate_normals_inside") {
		return mesh->recalculate_normals(true);
	} else if (p_id == "shade_smooth") {
		return mesh->shade_smooth(true);
	} else if (p_id == "shade_flat") {
		return mesh->shade_smooth(false);
	} else if (p_id == "triangulate") {
		return mesh->triangulate();
	} else if (p_id == "tris_to_quads") {
		return mesh->tris_to_quads(Math::deg_to_rad(float(p_values.get("face_angle", 40.0))), Math::deg_to_rad(float(p_values.get("shape_angle", 40.0)))) > 0;
	} else {
		return false;
	}
	return true;
}

bool TetraMeshEditPlugin::run_operation(const String &p_id, const Dictionary &p_values) {
	if (!edited || mesh.is_null()) {
		return false;
	}
	if (modal != MODAL_NONE) {
		_finish_modal(false);
	}
	// The interactive entries start their modal tool.
	if (p_id == "grab") {
		return start_modal(MODAL_GRAB);
	} else if (p_id == "rotate_modal") {
		return start_modal(MODAL_ROTATE);
	} else if (p_id == "scale_modal") {
		return start_modal(MODAL_SCALE);
	} else if (p_id == "inset" && !p_values.has("thickness")) {
		return start_modal(MODAL_INSET);
	} else if (p_id == "loop_cut" && !p_values.has("edge")) {
		return start_modal(MODAL_LOOP_CUT);
	}
	String label = p_id;
	for (int i = 0; i < operations.size(); i++) {
		Dictionary operation = operations[i];
		if (operation["id"] == p_id) {
			label = String(operation["label"]).get_slicec('(', 0).trim_suffix("...").strip_edges();
		}
	}
	if (p_id.begins_with("add:")) {
		label = TTR("Add Mesh");
	}
	PackedByteArray before = mesh->get_state();
	if (p_id == "separate") {
		Ref<TetraEditMesh> part = mesh->separate_selection();
		if (part.is_null()) {
			return false;
		}
		part->set_materials(mesh->get_materials());
		MeshInstance3D *node = memnew(MeshInstance3D);
		node->set_name(String(edited->get_name()) + "_part");
		node->set_transform(edited->get_transform());
		node->set_meta(META_EDIT_MESH, part);
		node->set_mesh(part->to_array_mesh());
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		Node *parent = edited->get_parent();
		Node *root = EditorNode::get_singleton()->get_edited_scene();
		_refresh();
		undo_redo->create_action(TTR("Separate Selection"), UndoRedo::MERGE_DISABLE, edited);
		undo_redo->add_do_method(this, "_apply_state", edited, mesh->get_state());
		undo_redo->add_do_method(parent, "add_child", node, true);
		undo_redo->add_do_method(node, "set_owner", root);
		undo_redo->add_do_reference(node);
		undo_redo->add_undo_method(parent, "remove_child", node);
		undo_redo->add_undo_method(this, "_apply_state", edited, before);
		undo_redo->commit_action();
		return true;
	}
	if (!_execute(p_id, p_values)) {
		return false;
	}
	last_values[p_id] = p_values;
	_refresh();
	_commit(label, before);
	return true;
}

void TetraMeshEditPlugin::_commit(const String &p_name, const PackedByteArray &p_before) {
	if (!edited || mesh.is_null()) {
		return;
	}
	PackedByteArray after = mesh->get_state();
	if (after == p_before) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_name, UndoRedo::MERGE_DISABLE, edited);
	undo_redo->add_do_method(this, "_apply_state", edited, after);
	undo_redo->add_undo_method(this, "_apply_state", edited, p_before);
	undo_redo->commit_action(false);
}

void TetraMeshEditPlugin::_apply_state(Node *p_node, const PackedByteArray &p_state) {
	MeshInstance3D *node = Object::cast_to<MeshInstance3D>(p_node);
	ERR_FAIL_NULL(node);
	Ref<TetraEditMesh> stored;
	if (node->has_meta(META_EDIT_MESH)) {
		stored = node->get_meta(META_EDIT_MESH);
	}
	if (stored.is_null()) {
		stored.instantiate();
		node->set_meta(META_EDIT_MESH, stored);
	}
	stored->set_state(p_state);
	if (node == edited) {
		modal = MODAL_NONE;
		box_pending = false;
		box_dragging = false;
		mesh = stored;
		_refresh();
	} else {
		node->set_mesh(stored->to_array_mesh());
	}
}

void TetraMeshEditPlugin::_set_edit_mesh(Node *p_node, const Ref<TetraEditMesh> &p_mesh, const Ref<Mesh> &p_node_mesh) {
	MeshInstance3D *node = Object::cast_to<MeshInstance3D>(p_node);
	ERR_FAIL_NULL(node);
	if (p_mesh.is_valid()) {
		node->set_meta(META_EDIT_MESH, p_mesh);
	} else {
		if (node == edited) {
			exit_edit_mode();
		}
		node->remove_meta(META_EDIT_MESH);
	}
	node->set_mesh(p_node_mesh);
	if (node == edited) {
		display_mesh = p_node_mesh;
	}
}

// ---------------------------------------------------------------------------
// Entering and leaving.

static bool _same_geometry(const Ref<TetraEditMesh> &p_edit_mesh, const Ref<Mesh> &p_mesh) {
	Ref<ArrayMesh> generated = p_edit_mesh->to_array_mesh();
	if (p_mesh.is_null()) {
		return generated->get_surface_count() == 0;
	}
	if (generated->get_surface_count() != p_mesh->get_surface_count()) {
		return false;
	}
	for (int i = 0; i < generated->get_surface_count(); i++) {
		if (generated->surface_get_array_len(i) != p_mesh->surface_get_array_len(i) || generated->surface_get_array_index_len(i) != p_mesh->surface_get_array_index_len(i)) {
			return false;
		}
	}
	return generated->get_aabb().is_equal_approx(p_mesh->get_aabb());
}

bool TetraMeshEditPlugin::enter_edit_mode(MeshInstance3D *p_node) {
	ERR_FAIL_NULL_V(p_node, false);
	ERR_FAIL_COND_V(!p_node->is_inside_tree(), false);
	if (edited == p_node) {
		return true;
	}
	if (edited) {
		exit_edit_mode();
	}
	Ref<TetraEditMesh> stored;
	if (p_node->has_meta(META_EDIT_MESH)) {
		stored = p_node->get_meta(META_EDIT_MESH);
	}
	Ref<Mesh> current = p_node->get_mesh();
	if (stored.is_valid()) {
		if (!_same_geometry(stored, current)) {
			// Something else (a Blender operator or modifier, the inspector)
			// replaced the mesh: start again from what the node shows.
			stored->from_mesh(current, true);
		}
	} else {
		stored.instantiate();
		stored->from_mesh(current, true);
		Ref<Mesh> generated = stored->to_array_mesh();
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Make Mesh Editable"), UndoRedo::MERGE_DISABLE, p_node);
		undo_redo->add_do_method(this, "_set_edit_mesh", p_node, stored, generated);
		undo_redo->add_undo_method(this, "_set_edit_mesh", p_node, Ref<TetraEditMesh>(), current);
		undo_redo->commit_action();
	}
	edited = p_node;
	mesh = stored;
	display_mesh = p_node->get_mesh();
	edited->connect(SceneStringName(tree_exiting), callable_mp(this, &TetraMeshEditPlugin::_target_exiting));
	edited->set_transform_gizmo_visible(false);
	Node3DEditor::get_singleton()->update_transform_gizmo();
	_create_overlay();
	_refresh();
	set_process(true);
	return true;
}

void TetraMeshEditPlugin::exit_edit_mode() {
	if (!edited) {
		return;
	}
	if (modal != MODAL_NONE) {
		_finish_modal(false);
	}
	if (dialog->is_visible()) {
		dialog->hide();
		_dialog_cancelled(String());
	}
	box_pending = false;
	box_dragging = false;
	_free_overlay();
	_set_selection_box_visible(true);
	edited->disconnect(SceneStringName(tree_exiting), callable_mp(this, &TetraMeshEditPlugin::_target_exiting));
	edited->set_transform_gizmo_visible(true);
	Node3DEditor::get_singleton()->update_transform_gizmo();
	edited = nullptr;
	mesh.unref();
	display_mesh.unref();
	pick_triangles.clear();
	pick_triangle_faces.clear();
	set_process(false);
	_update_toolbar();
	update_overlays();
}

// The node's selection box would cover the edges of a box-shaped mesh, and
// Blender shows no object outline in Edit Mode either.
void TetraMeshEditPlugin::_set_selection_box_visible(bool p_visible) {
	if (!edited) {
		return;
	}
	Node3DEditorSelectedItem *item = EditorNode::get_singleton()->get_editor_selection()->get_node_editor_data<Node3DEditorSelectedItem>(edited);
	if (!item) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	for (RID instance : { item->sbox_instance, item->sbox_instance_offset, item->sbox_instance_xray, item->sbox_instance_xray_offset }) {
		if (instance.is_valid()) {
			rs->instance_set_visible(instance, p_visible);
		}
	}
}

void TetraMeshEditPlugin::_target_exiting() {
	exit_edit_mode();
}

bool TetraMeshEditPlugin::handles(Object *p_object) const {
	return Object::cast_to<MeshInstance3D>(p_object) != nullptr;
}

void TetraMeshEditPlugin::edit(Object *p_object) {
	target = Object::cast_to<MeshInstance3D>(p_object);
	if (edited && edited != target) {
		exit_edit_mode();
	}
	_update_toolbar();
}

void TetraMeshEditPlugin::make_visible(bool p_visible) {
	if (!p_visible) {
		exit_edit_mode();
		target = nullptr;
	}
	toolbar->set_visible(p_visible);
	_update_toolbar();
}

void TetraMeshEditPlugin::set_select_mode(TetraEditMesh::SelectMode p_mode) {
	if (mesh.is_null()) {
		return;
	}
	mesh->set_select_mode(p_mode);
	_refresh(false);
}

TetraEditMesh::SelectMode TetraMeshEditPlugin::get_select_mode() const {
	return mesh.is_valid() ? mesh->get_select_mode() : TetraEditMesh::SELECT_VERTEX;
}

void TetraMeshEditPlugin::request_operation(const String &p_id) {
	_request_operation(p_id);
}

TetraOperationDialog *TetraMeshEditPlugin::get_operation_dialog() const {
	return dialog;
}

void TetraMeshEditPlugin::refresh() {
	_refresh();
}

// ---------------------------------------------------------------------------
// Display.

void TetraMeshEditPlugin::_refresh(bool p_topology) {
	if (!edited || mesh.is_null()) {
		return;
	}
	if (p_topology) {
		display_mesh = mesh->to_array_mesh();
		edited->set_mesh(display_mesh);
		_rebuild_pick_cache();
	}
	_rebuild_overlay();
	_update_toolbar();
	update_overlays();
}

void TetraMeshEditPlugin::_create_overlay() {
	_free_overlay();
	if (overlay_shader.is_null()) {
		overlay_shader.instantiate();
		overlay_shader->set_code(OVERLAY_LINE_SHADER);
		overlay_point_shader.instantiate();
		overlay_point_shader->set_code(OVERLAY_POINT_SHADER);
		overlay_fill_shader.instantiate();
		overlay_fill_shader->set_code(OVERLAY_FILL_SHADER);
		edge_material.instantiate();
		edge_material->set_shader(overlay_shader);
		point_material.instantiate();
		point_material->set_shader(overlay_point_shader);
		point_material->set_shader_parameter("point_size", 8.0 * EDSCALE);
		fill_material.instantiate();
		fill_material->set_shader(overlay_fill_shader);
	}
	overlay_mesh.instantiate();
	RenderingServer *rs = RenderingServer::get_singleton();
	overlay_instance = rs->instance_create2(overlay_mesh->get_rid(), edited->get_world_3d()->get_scenario());
	rs->instance_attach_object_instance_id(overlay_instance, edited->get_instance_id());
	rs->instance_geometry_set_cast_shadows_setting(overlay_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_set_layer_mask(overlay_instance, 1 << Node3DEditorViewport::GIZMO_EDIT_LAYER);
	rs->instance_geometry_set_flag(overlay_instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
	rs->instance_geometry_set_flag(overlay_instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
	// Positions are updated in place while dragging, without new bounds.
	rs->instance_set_ignore_culling(overlay_instance, true);
	overlay_transform = edited->get_global_transform();
	rs->instance_set_transform(overlay_instance, overlay_transform);
}

void TetraMeshEditPlugin::_free_overlay() {
	if (overlay_instance.is_valid()) {
		RenderingServer::get_singleton()->free_rid(overlay_instance);
		overlay_instance = RID();
	}
	overlay_mesh.unref();
	overlay_surfaces.clear();
}

void TetraMeshEditPlugin::_overlay_positions(OverlayKind p_kind, Vector<Vector3> &r_positions) const {
	const LocalVector<Vector3> &verts = mesh->get_verts();
	switch (p_kind) {
		case OVERLAY_EDGES: {
			for (const Vector2i &edge : mesh->get_edge_list()) {
				r_positions.push_back(verts[edge.x]);
				r_positions.push_back(verts[edge.y]);
			}
		} break;
		case OVERLAY_POINTS: {
			if (mesh->get_select_mode() == TetraEditMesh::SELECT_VERTEX) {
				for (const Vector3 &v : verts) {
					r_positions.push_back(v);
				}
			} else if (mesh->get_select_mode() == TetraEditMesh::SELECT_FACE) {
				for (const TetraEditMesh::Face &face : mesh->get_face_list()) {
					Vector3 center;
					for (int v : face.verts) {
						center += verts[v];
					}
					r_positions.push_back(center / face.verts.size());
				}
			}
		} break;
		case OVERLAY_FACES: {
			const LocalVector<TetraEditMesh::Face> &faces = mesh->get_face_list();
			const TetraEditMesh::Mask &selected = mesh->get_face_selection();
			TetraEditMesh::IntList corners;
			for (uint32_t f = 0; f < faces.size(); f++) {
				if (!selected[f]) {
					continue;
				}
				corners.clear();
				mesh->triangulate_face(f, corners);
				for (int corner : corners) {
					r_positions.push_back(verts[faces[f].verts[corner]]);
				}
			}
		} break;
	}
}

void TetraMeshEditPlugin::_rebuild_overlay() {
	if (overlay_mesh.is_null() || mesh.is_null()) {
		return;
	}
	overlay_mesh->clear_surfaces();
	overlay_surfaces.clear();
	static const OverlayKind kinds[3] = { OVERLAY_FACES, OVERLAY_EDGES, OVERLAY_POINTS };
	for (OverlayKind kind : kinds) {
		Vector<Vector3> positions;
		_overlay_positions(kind, positions);
		if (positions.is_empty()) {
			continue;
		}
		Vector<Color> colors;
		colors.resize(positions.size());
		Color *color = colors.ptrw();
		Mesh::PrimitiveType primitive = Mesh::PRIMITIVE_TRIANGLES;
		Ref<Material> material = fill_material;
		if (kind == OVERLAY_EDGES) {
			primitive = Mesh::PRIMITIVE_LINES;
			material = edge_material;
			const TetraEditMesh::Mask &selected = mesh->get_edge_selection();
			for (uint32_t e = 0; e < selected.size(); e++) {
				color[e * 2] = color[e * 2 + 1] = selected[e] ? COLOR_EDGE_SELECTED : COLOR_EDGE;
			}
		} else if (kind == OVERLAY_POINTS) {
			primitive = Mesh::PRIMITIVE_POINTS;
			material = point_material;
			const TetraEditMesh::Mask &selected = mesh->get_select_mode() == TetraEditMesh::SELECT_VERTEX ? mesh->get_vertex_selection() : mesh->get_face_selection();
			for (uint32_t i = 0; i < selected.size(); i++) {
				color[i] = selected[i] ? COLOR_VERTEX_SELECTED : COLOR_VERTEX;
			}
		} else {
			for (int i = 0; i < colors.size(); i++) {
				color[i] = COLOR_FACE_SELECTED;
			}
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = positions;
		arrays[Mesh::ARRAY_COLOR] = colors;
		overlay_mesh->add_surface_from_arrays(primitive, arrays);
		overlay_mesh->surface_set_material(overlay_mesh->get_surface_count() - 1, material);
		overlay_surfaces.push_back(kind);
	}
}

// The cheap path while dragging: the topology and the colours are unchanged,
// so only the position buffers are rewritten.
void TetraMeshEditPlugin::_update_overlay_positions() {
	if (overlay_mesh.is_null() || mesh.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	for (uint32_t surface = 0; surface < overlay_surfaces.size(); surface++) {
		Vector<Vector3> positions;
		_overlay_positions(overlay_surfaces[surface], positions);
		if (positions.size() != overlay_mesh->surface_get_array_len(surface)) {
			_rebuild_overlay();
			return;
		}
		Vector<uint8_t> bytes;
		bytes.resize(positions.size() * 12);
		float *out = (float *)bytes.ptrw();
		for (int i = 0; i < positions.size(); i++) {
			out[i * 3] = positions[i].x;
			out[i * 3 + 1] = positions[i].y;
			out[i * 3 + 2] = positions[i].z;
		}
		rs->mesh_surface_update_vertex_region(overlay_mesh->get_rid(), surface, 0, bytes);
	}
}

void TetraMeshEditPlugin::_rebuild_pick_cache() {
	pick_triangles.clear();
	pick_triangle_faces.clear();
	if (mesh.is_null()) {
		return;
	}
	const LocalVector<Vector3> &verts = mesh->get_verts();
	const LocalVector<TetraEditMesh::Face> &faces = mesh->get_face_list();
	TetraEditMesh::IntList corners;
	for (uint32_t f = 0; f < faces.size(); f++) {
		corners.clear();
		mesh->triangulate_face(f, corners);
		for (uint32_t i = 0; i + 2 < corners.size(); i += 3) {
			pick_triangles.push_back(verts[faces[f].verts[corners[i]]]);
			pick_triangles.push_back(verts[faces[f].verts[corners[i + 1]]]);
			pick_triangles.push_back(verts[faces[f].verts[corners[i + 2]]]);
			pick_triangle_faces.push_back(f);
		}
	}
}

Dictionary TetraMeshEditPlugin::get_overlay_counts() const {
	Dictionary counts;
	counts["visible"] = overlay_instance.is_valid();
	counts["edges"] = 0;
	counts["points"] = 0;
	counts["face_triangles"] = 0;
	if (overlay_mesh.is_null()) {
		return counts;
	}
	for (uint32_t surface = 0; surface < overlay_surfaces.size(); surface++) {
		int length = overlay_mesh->surface_get_array_len(surface);
		switch (overlay_surfaces[surface]) {
			case OVERLAY_EDGES:
				counts["edges"] = length / 2;
				break;
			case OVERLAY_POINTS:
				counts["points"] = length;
				break;
			case OVERLAY_FACES:
				counts["face_triangles"] = length / 3;
				break;
		}
	}
	return counts;
}

// ---------------------------------------------------------------------------
// Picking.

Camera3D *TetraMeshEditPlugin::_camera() const {
	Node3DEditor *editor = Node3DEditor::get_singleton();
	for (int i = 0; i < 4; i++) {
		if (editor->get_editor_viewport(i)->get_camera_3d() == last_camera) {
			return last_camera;
		}
	}
	return editor->get_editor_viewport(0)->get_camera_3d();
}

Control *TetraMeshEditPlugin::_surface_of(Camera3D *p_camera) const {
	Node3DEditor *editor = Node3DEditor::get_singleton();
	for (int i = 0; i < 4; i++) {
		if (editor->get_editor_viewport(i)->get_camera_3d() == p_camera) {
			return editor->get_editor_viewport(i)->get_surface();
		}
	}
	return editor->get_editor_viewport(0)->get_surface();
}

bool TetraMeshEditPlugin::_project(Camera3D *p_camera, const Vector3 &p_local, Point2 &r_screen) const {
	Vector3 world = edited->get_global_transform().xform(p_local);
	if (p_camera->is_position_behind(world)) {
		return false;
	}
	r_screen = p_camera->unproject_position(world);
	return true;
}

bool TetraMeshEditPlugin::_is_occluded(Camera3D *p_camera, const Vector3 &p_local) const {
	Point2 screen;
	if (!_project(p_camera, p_local, screen)) {
		return true;
	}
	Transform3D to_local = edited->get_global_transform().affine_inverse();
	Vector3 from = to_local.xform(p_camera->project_ray_origin(screen));
	Vector3 direction = p_local - from;
	real_t distance = direction.length();
	if (distance < 1e-6) {
		return false;
	}
	direction /= distance;
	for (uint32_t i = 0; i + 2 < pick_triangles.size(); i += 3) {
		Vector3 hit;
		if (Geometry3D::ray_intersects_triangle(from, direction, pick_triangles[i], pick_triangles[i + 1], pick_triangles[i + 2], &hit)) {
			if (from.distance_to(hit) < distance * 0.998 - 1e-4) {
				return true;
			}
		}
	}
	return false;
}

struct TetraPickCandidate {
	real_t distance;
	int index;
	Vector3 point;
	bool operator<(const TetraPickCandidate &p_other) const { return distance < p_other.distance; }
};

int TetraMeshEditPlugin::_pick_vertex(Camera3D *p_camera, const Point2 &p_position, real_t p_radius) const {
	LocalVector<TetraPickCandidate> candidates;
	const LocalVector<Vector3> &verts = mesh->get_verts();
	for (uint32_t v = 0; v < verts.size(); v++) {
		Point2 screen;
		if (_project(p_camera, verts[v], screen) && screen.distance_to(p_position) <= p_radius * EDSCALE) {
			TetraPickCandidate candidate;
			candidate.distance = screen.distance_to(p_position);
			candidate.index = v;
			candidate.point = verts[v];
			candidates.push_back(candidate);
		}
	}
	candidates.sort();
	for (const TetraPickCandidate &candidate : candidates) {
		if (!_is_occluded(p_camera, candidate.point)) {
			return candidate.index;
		}
	}
	return -1;
}

int TetraMeshEditPlugin::_pick_edge(Camera3D *p_camera, const Point2 &p_position, real_t p_radius, bool p_visible_only) const {
	LocalVector<TetraPickCandidate> candidates;
	const LocalVector<Vector3> &verts = mesh->get_verts();
	const LocalVector<Vector2i> &edges = mesh->get_edge_list();
	for (uint32_t e = 0; e < edges.size(); e++) {
		Point2 a, b;
		if (!_project(p_camera, verts[edges[e].x], a) || !_project(p_camera, verts[edges[e].y], b)) {
			continue;
		}
		Point2 closest = Geometry2D::get_closest_point_to_segment(p_position, a, b);
		real_t distance = closest.distance_to(p_position);
		if (distance > p_radius * EDSCALE) {
			continue;
		}
		real_t length = a.distance_to(b);
		real_t t = length > 1e-6 ? a.distance_to(closest) / length : 0.5;
		TetraPickCandidate candidate;
		candidate.distance = distance;
		candidate.index = e;
		// Not the end points themselves: those are shared with other faces.
		candidate.point = verts[edges[e].x].lerp(verts[edges[e].y], CLAMP(t, 0.1, 0.9));
		candidates.push_back(candidate);
	}
	candidates.sort();
	for (const TetraPickCandidate &candidate : candidates) {
		if (!p_visible_only || !_is_occluded(p_camera, candidate.point)) {
			return candidate.index;
		}
	}
	return -1;
}

int TetraMeshEditPlugin::_pick_face(Camera3D *p_camera, const Point2 &p_position) const {
	Transform3D to_local = edited->get_global_transform().affine_inverse();
	Vector3 from = to_local.xform(p_camera->project_ray_origin(p_position));
	Vector3 direction = to_local.basis.xform(p_camera->project_ray_normal(p_position)).normalized();
	int best = -1;
	real_t best_distance = Math::INF;
	for (uint32_t i = 0; i + 2 < pick_triangles.size(); i += 3) {
		Vector3 hit;
		if (Geometry3D::ray_intersects_triangle(from, direction, pick_triangles[i], pick_triangles[i + 1], pick_triangles[i + 2], &hit)) {
			real_t distance = from.distance_squared_to(hit);
			if (distance < best_distance) {
				best_distance = distance;
				best = pick_triangle_faces[i / 3];
			}
		}
	}
	return best;
}

Dictionary TetraMeshEditPlugin::pick(Camera3D *p_camera, const Point2 &p_position) const {
	Dictionary result;
	result["vertex"] = -1;
	result["edge"] = -1;
	result["face"] = -1;
	if (!edited || mesh.is_null() || !p_camera) {
		return result;
	}
	result["vertex"] = _pick_vertex(p_camera, p_position);
	result["edge"] = _pick_edge(p_camera, p_position);
	result["face"] = _pick_face(p_camera, p_position);
	return result;
}

void TetraMeshEditPlugin::_click_select(Camera3D *p_camera, const Point2 &p_position, bool p_extend, bool p_loop) {
	if (p_loop) {
		int edge = _pick_edge(p_camera, p_position, 20.0);
		if (edge >= 0) {
			mesh->select_edge_loop(edge, p_extend);
			_refresh(false);
		}
		return;
	}
	int picked = -1;
	switch (mesh->get_select_mode()) {
		case TetraEditMesh::SELECT_VERTEX: {
			picked = _pick_vertex(p_camera, p_position);
			if (picked >= 0) {
				bool select = !p_extend || !mesh->is_vertex_selected(picked);
				if (!p_extend) {
					mesh->select_none();
				}
				mesh->select_vertex(picked, select);
			}
		} break;
		case TetraEditMesh::SELECT_EDGE: {
			picked = _pick_edge(p_camera, p_position);
			if (picked >= 0) {
				bool select = !p_extend || !mesh->is_edge_selected(picked);
				if (!p_extend) {
					mesh->select_none();
				}
				mesh->select_edge(picked, select);
			}
		} break;
		case TetraEditMesh::SELECT_FACE: {
			picked = _pick_face(p_camera, p_position);
			if (picked >= 0) {
				bool select = !p_extend || !mesh->is_face_selected(picked);
				if (!p_extend) {
					mesh->select_none();
				}
				mesh->select_face(picked, select);
			}
		} break;
	}
	if (picked >= 0) {
		_refresh(false);
	} else {
		// Empty space: a drag selects with a box, a click deselects.
		box_pending = true;
		box_dragging = false;
		box_extend = p_extend;
		box_start = p_position;
	}
}

void TetraMeshEditPlugin::_box_select(Camera3D *p_camera, const Rect2 &p_rect, bool p_extend) {
	if (!p_extend) {
		mesh->select_none();
	}
	const LocalVector<Vector3> &verts = mesh->get_verts();
	LocalVector<int> hits;
	switch (mesh->get_select_mode()) {
		case TetraEditMesh::SELECT_VERTEX: {
			for (uint32_t v = 0; v < verts.size(); v++) {
				Point2 screen;
				if (_project(p_camera, verts[v], screen) && p_rect.has_point(screen) && !_is_occluded(p_camera, verts[v])) {
					hits.push_back(v);
				}
			}
			for (int v : hits) {
				mesh->select_vertex(v, true);
			}
		} break;
		case TetraEditMesh::SELECT_EDGE: {
			const LocalVector<Vector2i> &edges = mesh->get_edge_list();
			for (uint32_t e = 0; e < edges.size(); e++) {
				Point2 a, b;
				if (_project(p_camera, verts[edges[e].x], a) && _project(p_camera, verts[edges[e].y], b) && p_rect.has_point(a) && p_rect.has_point(b) && !_is_occluded(p_camera, (verts[edges[e].x] + verts[edges[e].y]) * 0.5)) {
					hits.push_back(e);
				}
			}
			for (int e : hits) {
				mesh->select_edge(e, true);
			}
		} break;
		case TetraEditMesh::SELECT_FACE: {
			for (int f = 0; f < mesh->get_face_count(); f++) {
				Vector3 center = mesh->get_face_center(f);
				Point2 screen;
				if (_project(p_camera, center, screen) && p_rect.has_point(screen) && !_is_occluded(p_camera, center)) {
					hits.push_back(f);
				}
			}
			for (int f : hits) {
				mesh->select_face(f, true);
			}
		} break;
	}
	_refresh(false);
}

void TetraMeshEditPlugin::_select_linked_under_mouse(bool p_select) {
	Camera3D *camera = _camera();
	int vertex = _pick_vertex(camera, last_mouse, 30.0);
	if (vertex < 0) {
		int edge = _pick_edge(camera, last_mouse, 30.0);
		if (edge >= 0) {
			vertex = mesh->get_edge_vertices(edge).x;
		}
	}
	if (vertex < 0) {
		int face = _pick_face(camera, last_mouse);
		if (face >= 0) {
			vertex = mesh->get_face_vertices(face)[0];
		}
	}
	if (vertex >= 0) {
		mesh->select_linked_from_vertex(vertex, p_select);
		_refresh(false);
	}
}

// ---------------------------------------------------------------------------
// Modal tools.

bool TetraMeshEditPlugin::start_modal(ModalKind p_kind) {
	if (!edited || mesh.is_null() || p_kind == MODAL_NONE) {
		return false;
	}
	if (modal != MODAL_NONE) {
		_finish_modal(false);
	}
	String name;
	switch (p_kind) {
		case MODAL_GRAB:
			name = TTR("Move");
			break;
		case MODAL_ROTATE:
			name = TTR("Rotate");
			break;
		case MODAL_SCALE:
			name = TTR("Scale");
			break;
		case MODAL_INSET:
			name = TTR("Inset Faces");
			break;
		case MODAL_LOOP_CUT:
			name = TTR("Loop Cut");
			break;
		default:
			break;
	}
	_begin_modal(p_kind, name, PackedByteArray(), false);
	return modal == p_kind;
}

void TetraMeshEditPlugin::_begin_modal(ModalKind p_kind, const String &p_name, const PackedByteArray &p_before, bool p_keep_on_cancel) {
	if (p_kind != MODAL_LOOP_CUT && mesh->get_selected_vertices().is_empty()) {
		return;
	}
	if (p_kind == MODAL_INSET && mesh->get_selected_faces().is_empty()) {
		return;
	}
	Camera3D *camera = _camera();
	last_camera = camera;
	if (!have_mouse) {
		last_mouse = _surface_of(camera)->get_size() * 0.5;
	}
	modal = p_kind;
	modal_name = p_name;
	modal_base = mesh->get_state();
	modal_before = p_before.is_empty() ? modal_base : p_before;
	modal_positions = mesh->get_vertex_positions();
	modal_pivot = mesh->get_selection_pivot(TetraEditMesh::PIVOT_MEDIAN);
	modal_start_mouse = last_mouse;
	modal_constraint = CONSTRAINT_NONE;
	modal_axis = Vector3();
	modal_axis_name = String();
	modal_typed = String();
	modal_snap = false;
	modal_keep_on_cancel = p_keep_on_cancel;
	modal_readout = p_name;
	loop_cut_edge = -1;
	loop_cut_count = 1;
	loop_cut_preview.clear();
	_update_modal();
}

void TetraMeshEditPlugin::_set_constraint(const Vector3 &p_axis, const String &p_name, bool p_plane) {
	Constraint wanted = p_plane ? CONSTRAINT_PLANE : CONSTRAINT_AXIS;
	if (modal_constraint == wanted && modal_axis.is_equal_approx(p_axis)) {
		modal_constraint = CONSTRAINT_NONE; // The same key again frees it.
	} else {
		modal_constraint = wanted;
		modal_axis = p_axis;
		modal_axis_name = p_name;
	}
}

void TetraMeshEditPlugin::_update_modal() {
	if (modal == MODAL_NONE || !edited) {
		return;
	}
	Camera3D *camera = _camera();
	const Transform3D global = edited->get_global_transform();
	const Basis to_local = global.basis.inverse();
	const Vector3 pivot_world = global.xform(modal_pivot);
	const bool typed = modal_typed.is_valid_float();
	const real_t typed_value = typed ? modal_typed.to_float() : 0.0;
	Node3DEditor *editor = Node3DEditor::get_singleton();
	String constraint_text;
	if (modal_constraint == CONSTRAINT_AXIS) {
		constraint_text = " " + vformat(TTR("along %s"), modal_axis_name);
	} else if (modal_constraint == CONSTRAINT_PLANE) {
		constraint_text = " " + vformat(TTR("locking %s"), modal_axis_name);
	}
	String typed_text = modal_typed.is_empty() ? String() : " [" + modal_typed + "]";

	if (modal == MODAL_LOOP_CUT) {
		if (typed && typed_value >= 1) {
			loop_cut_count = CLAMP(int(typed_value), 1, 100);
		}
		loop_cut_edge = _pick_edge(camera, last_mouse, 40.0, true);
		loop_cut_preview.clear();
		if (loop_cut_edge >= 0) {
			// Cut a scratch copy; the new loop is what gets previewed.
			Ref<TetraEditMesh> scratch;
			scratch.instantiate();
			scratch->set_state(mesh->get_state());
			scratch->set_select_mode(TetraEditMesh::SELECT_EDGE);
			if (scratch->loop_cut(loop_cut_edge, loop_cut_count)) {
				for (int e : scratch->get_selected_edges()) {
					Vector2i edge = scratch->get_edge_vertices(e);
					loop_cut_preview.push_back(scratch->get_vertex(edge.x));
					loop_cut_preview.push_back(scratch->get_vertex(edge.y));
				}
			}
		}
		modal_readout = vformat(TTR("Loop Cut: %d cut(s). Wheel or digits change the count, click confirms, Esc cancels."), loop_cut_count);
		update_overlays();
		return;
	}

	Point2 pivot_screen = camera->unproject_position(pivot_world);
	Vector3 view_axis = camera->get_global_transform().basis.get_column(2).normalized(); // Towards the viewer.

	if (modal == MODAL_INSET) {
		real_t depth = (pivot_world - camera->get_global_transform().origin).dot(-view_axis);
		real_t world_per_pixel = camera->project_position(pivot_screen + Point2(1, 0), depth).distance_to(camera->project_position(pivot_screen, depth));
		real_t thickness = MAX(0.0, (modal_start_mouse.distance_to(pivot_screen) - last_mouse.distance_to(pivot_screen)) * world_per_pixel);
		thickness /= MAX(global.basis.get_uniform_scale(), 1e-6);
		if (typed) {
			thickness = MAX(0.0, typed_value);
		} else if (modal_snap) {
			thickness = Math::snapped(thickness, MAX(editor->get_translate_snap(), 0.001));
		}
		mesh->set_state(modal_base);
		mesh->inset_faces(thickness, 0.0);
		modal_readout = vformat("%s: %s%s", modal_name, format_number(thickness), typed_text);
		_refresh();
		return;
	}

	Transform3D transform;
	switch (modal) {
		case MODAL_GRAB: {
			Vector3 from0 = camera->project_ray_origin(modal_start_mouse);
			Vector3 dir0 = camera->project_ray_normal(modal_start_mouse);
			Vector3 from1 = camera->project_ray_origin(last_mouse);
			Vector3 dir1 = camera->project_ray_normal(last_mouse);
			Vector3 offset;
			real_t snap = MAX(editor->get_translate_snap(), 0.001);
			if (modal_constraint == CONSTRAINT_AXIS) {
				real_t amount = closest_on_axis(pivot_world, modal_axis, from1, dir1) - closest_on_axis(pivot_world, modal_axis, from0, dir0);
				if (typed) {
					amount = typed_value;
				} else if (modal_snap) {
					amount = Math::snapped(amount, snap);
				}
				offset = modal_axis * amount;
			} else {
				Plane plane(modal_constraint == CONSTRAINT_PLANE ? modal_axis : view_axis, pivot_world);
				Vector3 hit0, hit1;
				if (plane.intersects_ray(from0, dir0, &hit0) && plane.intersects_ray(from1, dir1, &hit1)) {
					offset = hit1 - hit0;
				}
				if (typed) {
					// Like Blender, a typed value without an axis moves along X.
					offset = (modal_constraint == CONSTRAINT_PLANE ? modal_axis.get_any_perpendicular() : Vector3(1, 0, 0)) * typed_value;
				} else if (modal_snap) {
					offset = offset.snapped(Vector3(snap, snap, snap));
				}
			}
			transform.origin = to_local.xform(offset);
			modal_readout = vformat("%s: (%s, %s, %s)%s%s", modal_name, format_number(offset.x), format_number(offset.y), format_number(offset.z), constraint_text, typed_text);
		} break;
		case MODAL_ROTATE: {
			Vector3 axis = modal_constraint == CONSTRAINT_NONE ? view_axis : modal_axis;
			Point2 start = modal_start_mouse - pivot_screen;
			Point2 now = last_mouse - pivot_screen;
			// Screen Y points down.
			real_t angle = Math::atan2(-now.y, now.x) - Math::atan2(-start.y, start.x);
			if (axis.dot(view_axis) < 0) {
				angle = -angle;
			}
			angle = Math::wrapf(angle, real_t(-Math::PI), real_t(Math::PI));
			if (typed) {
				angle = Math::deg_to_rad(typed_value);
			} else if (modal_snap) {
				angle = Math::snapped(angle, Math::deg_to_rad(MAX(editor->get_rotate_snap(), real_t(1.0))));
			}
			transform.basis = to_local * Basis(axis, angle) * global.basis;
			modal_readout = vformat("%s: %s%s%s%s", modal_name, format_number(Math::rad_to_deg(angle)), String::utf8("°"), constraint_text, typed_text);
		} break;
		case MODAL_SCALE: {
			real_t factor = last_mouse.distance_to(pivot_screen) / MAX(modal_start_mouse.distance_to(pivot_screen), real_t(1.0));
			if (typed) {
				factor = typed_value;
			} else if (modal_snap) {
				factor = Math::snapped(factor, real_t(0.1));
			}
			Basis scale;
			if (modal_constraint == CONSTRAINT_AXIS) {
				scale = add_basis(Basis(), outer_product(modal_axis) * (factor - 1.0));
			} else if (modal_constraint == CONSTRAINT_PLANE) {
				scale = add_basis(Basis::from_scale(Vector3(factor, factor, factor)), outer_product(modal_axis) * (1.0 - factor));
			} else {
				scale = Basis::from_scale(Vector3(factor, factor, factor));
			}
			transform.basis = to_local * scale * global.basis;
			modal_readout = vformat("%s: %s%s%s", modal_name, format_number(factor), constraint_text, typed_text);
		} break;
		default:
			break;
	}
	mesh->set_vertex_positions(modal_positions);
	mesh->transform_selection(transform, modal_pivot);
	display_mesh = mesh->to_array_mesh();
	edited->set_mesh(display_mesh);
	_update_overlay_positions();
	update_overlays();
}

void TetraMeshEditPlugin::_finish_modal(bool p_confirm) {
	if (modal == MODAL_NONE) {
		return;
	}
	ModalKind kind = modal;
	modal = MODAL_NONE;
	loop_cut_preview.clear();
	if (!edited || mesh.is_null()) {
		return;
	}
	if (kind == MODAL_LOOP_CUT) {
		if (p_confirm && loop_cut_edge >= 0 && loop_cut_edge < mesh->get_edge_count()) {
			PackedByteArray before = mesh->get_state();
			if (mesh->loop_cut(loop_cut_edge, loop_cut_count)) {
				_refresh();
				_commit(modal_name, before);
			}
		}
		update_overlays();
		return;
	}
	if (!p_confirm) {
		mesh->set_state(modal_keep_on_cancel ? modal_base : modal_before);
	}
	_refresh();
	if (p_confirm || modal_keep_on_cancel) {
		_commit(modal_name, modal_before);
	}
}

void TetraMeshEditPlugin::_accept_event() {
	toolbar->get_viewport()->set_input_as_handled();
}

bool TetraMeshEditPlugin::_modal_key(const Ref<InputEventKey> &p_key) {
	Key key = effective_key(p_key);
	if (key == Key::ENTER || key == Key::KP_ENTER) {
		_finish_modal(true);
		return true;
	}
	if (key == Key::ESCAPE) {
		_finish_modal(false);
		return true;
	}
	if (key == Key::BACKSPACE) {
		if (!modal_typed.is_empty()) {
			modal_typed = modal_typed.substr(0, modal_typed.length() - 1);
		}
		_update_modal();
		return true;
	}
	if (modal != MODAL_LOOP_CUT && modal != MODAL_INSET && (key == Key::X || key == Key::Y || key == Key::Z)) {
		Vector3 axis;
		axis[int(key) - int(Key::X)] = 1;
		_set_constraint(axis, String::chr(char32_t(key)), p_key->is_shift_pressed());
		_update_modal();
		return true;
	}
	char32_t character = 0;
	if (key >= Key::KEY_0 && key <= Key::KEY_9) {
		character = '0' + (int(key) - int(Key::KEY_0));
	} else if (key >= Key::KP_0 && key <= Key::KP_9) {
		character = '0' + (int(key) - int(Key::KP_0));
	} else if (key == Key::PERIOD || key == Key::KP_PERIOD) {
		character = '.';
	} else if (key == Key::MINUS || key == Key::KP_SUBTRACT) {
		character = '-';
	}
	if (character) {
		if (character == '-') {
			modal_typed = modal_typed.begins_with("-") ? modal_typed.substr(1) : "-" + modal_typed;
		} else {
			modal_typed += String::chr(character);
		}
		_update_modal();
		return true;
	}
	return false;
}

bool TetraMeshEditPlugin::_edit_key(const Ref<InputEventKey> &p_key) {
	const Key key = effective_key(p_key);
	const bool ctrl = p_key->is_command_or_control_pressed();
	const bool shift = p_key->is_shift_pressed();
	const bool alt = p_key->is_alt_pressed();
	const bool plain = !ctrl && !shift && !alt;

	if (plain && key >= Key::KEY_1 && key <= Key::KEY_3) {
		set_select_mode(TetraEditMesh::SelectMode(int(key) - int(Key::KEY_1)));
		return true;
	}
	if (key == Key::A && !ctrl && !shift) {
		if (alt) {
			mesh->select_none();
		} else {
			mesh->select_all();
		}
		_refresh(false);
		return true;
	}
	if (key == Key::I && ctrl && !shift && !alt) {
		mesh->select_invert();
		_refresh(false);
		return true;
	}
	if (key == Key::L && !ctrl && !alt) {
		_select_linked_under_mouse(!shift);
		return true;
	}
	if (ctrl && !shift && !alt && (key == Key::KP_ADD || key == Key::PLUS || key == Key::EQUAL)) {
		mesh->select_more();
		_refresh(false);
		return true;
	}
	if (ctrl && !shift && !alt && (key == Key::KP_SUBTRACT || key == Key::MINUS)) {
		mesh->select_less();
		_refresh(false);
		return true;
	}
	if (plain && key == Key::G) {
		start_modal(MODAL_GRAB);
		return true;
	}
	if (plain && key == Key::R) {
		start_modal(MODAL_ROTATE);
		return true;
	}
	if (plain && key == Key::S) {
		start_modal(MODAL_SCALE);
		return true;
	}
	if (plain && key == Key::I) {
		start_modal(MODAL_INSET);
		return true;
	}
	if (ctrl && !shift && !alt && key == Key::R) {
		start_modal(MODAL_LOOP_CUT);
		return true;
	}
	if (plain && key == Key::E) {
		PackedByteArray before = mesh->get_state();
		bool faces = !mesh->get_selected_faces().is_empty();
		if (mesh->extrude_region()) {
			_refresh();
			Vector3 normal = edited->get_global_transform().basis.xform(mesh->get_selection_normal()).normalized();
			_begin_modal(MODAL_GRAB, TTR("Extrude Region"), before, true);
			if (modal == MODAL_GRAB && faces) {
				_set_constraint(normal, TTR("the normal"), false);
				_update_modal();
			}
		}
		return true;
	}
	if (shift && !ctrl && !alt && key == Key::D) {
		PackedByteArray before = mesh->get_state();
		if (mesh->duplicate_selection()) {
			_refresh();
			_begin_modal(MODAL_GRAB, TTR("Duplicate"), before, true);
		}
		return true;
	}
	if (plain && (key == Key::X || key == Key::KEY_DELETE)) {
		_popup_at_mouse(delete_menu);
		return true;
	}
	if (plain && key == Key::M) {
		_popup_at_mouse(merge_menu);
		return true;
	}
	if (plain && key == Key::F) {
		run_operation("fill");
		return true;
	}
	if (shift && !ctrl && !alt && key == Key::N) {
		run_operation("recalculate_normals");
		return true;
	}
	return false;
}

EditorPlugin::AfterGUIInput TetraMeshEditPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!target) {
		return AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_valid()) {
		if (!key->is_pressed()) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (key->get_keycode() == Key::TAB && !key->is_command_or_control_pressed() && !key->is_alt_pressed() && !key->is_shift_pressed()) {
			if (!key->is_echo()) {
				if (edited) {
					exit_edit_mode();
				} else {
					enter_edit_mode(target);
				}
			}
			_accept_event();
			return AFTER_GUI_INPUT_STOP;
		}
		if (!edited) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (modal != MODAL_NONE) {
			// A modal tool owns the keyboard, apart from shortcuts with Ctrl
			// (undo would pull the mesh from under it, so it is swallowed too).
			if (!key->is_echo()) {
				_modal_key(key);
			}
			_accept_event();
			return AFTER_GUI_INPUT_STOP;
		}
		if (key->is_echo()) {
			return AFTER_GUI_INPUT_PASS;
		}
		if (_edit_key(key)) {
			_accept_event();
			return AFTER_GUI_INPUT_STOP;
		}
		if (key->get_keycode() == Key::ESCAPE) {
			// Escape would deselect the node and end the edit mode.
			box_pending = false;
			box_dragging = false;
			update_overlays();
			_accept_event();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (!edited || mesh.is_null()) {
		return AFTER_GUI_INPUT_PASS;
	}
	if (Input::get_singleton()->get_mouse_mode() == Input::MouseMode::MOUSE_MODE_CAPTURED) {
		return AFTER_GUI_INPUT_PASS; // Freelook.
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		last_camera = p_camera;
		last_mouse = motion->get_position();
		have_mouse = true;
		if (modal != MODAL_NONE) {
			modal_snap = motion->is_command_or_control_pressed();
			_update_modal();
			return AFTER_GUI_INPUT_STOP;
		}
		if (box_pending) {
			if (!box_dragging && last_mouse.distance_to(box_start) > 4 * EDSCALE) {
				box_dragging = true;
			}
			update_overlays();
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventMouseButton> button = p_event;
	if (button.is_null()) {
		return AFTER_GUI_INPUT_PASS;
	}
	last_camera = p_camera;
	last_mouse = button->get_position();
	have_mouse = true;
	const MouseButton index = button->get_button_index();
	if (index == MouseButton::WHEEL_UP || index == MouseButton::WHEEL_DOWN) {
		if (modal == MODAL_LOOP_CUT) {
			if (button->is_pressed()) {
				loop_cut_count = CLAMP(loop_cut_count + (index == MouseButton::WHEEL_UP ? 1 : -1), 1, 100);
				modal_typed = String();
				_update_modal();
			}
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (modal != MODAL_NONE) {
		if (index == MouseButton::LEFT) {
			if (button->is_pressed()) {
				_finish_modal(true);
				swallow_release = true;
			}
			return AFTER_GUI_INPUT_STOP;
		}
		if (index == MouseButton::RIGHT) {
			if (button->is_pressed()) {
				_finish_modal(false);
			}
			return AFTER_GUI_INPUT_STOP;
		}
		return AFTER_GUI_INPUT_PASS;
	}
	if (index != MouseButton::LEFT) {
		return AFTER_GUI_INPUT_PASS;
	}
	if (button->is_pressed()) {
		View3DController::NavigationScheme scheme = (View3DController::NavigationScheme)EDITOR_GET("editors/3d/navigation/navigation_scheme").operator int();
		if ((scheme == View3DController::NAV_SCHEME_MAYA || scheme == View3DController::NAV_SCHEME_MODO) && button->is_alt_pressed()) {
			return AFTER_GUI_INPUT_PASS;
		}
		swallow_release = true;
		_click_select(p_camera, last_mouse, button->is_shift_pressed(), button->is_alt_pressed());
		return AFTER_GUI_INPUT_STOP;
	}
	if (box_pending) {
		if (box_dragging) {
			_box_select(p_camera, Rect2(box_start, last_mouse - box_start).abs(), box_extend);
		} else if (!box_extend) {
			mesh->select_none();
			_refresh(false);
		}
		box_pending = false;
		box_dragging = false;
		swallow_release = false;
		update_overlays();
		return AFTER_GUI_INPUT_STOP;
	}
	if (swallow_release) {
		swallow_release = false;
		return AFTER_GUI_INPUT_STOP;
	}
	// Keeps the node selected for a release whose press was not seen here.
	return AFTER_GUI_INPUT_CUSTOM;
}

void TetraMeshEditPlugin::forward_3d_draw_over_viewport(Control *p_overlay) {
	if (!edited || mesh.is_null()) {
		return;
	}
	Camera3D *camera = _camera();
	if (_surface_of(camera) != p_overlay) {
		return;
	}
	if (box_pending && box_dragging) {
		Rect2 rect = Rect2(box_start, last_mouse - box_start).abs();
		p_overlay->draw_rect(rect, p_overlay->get_theme_color(SNAME("box_selection_fill_color"), EditorStringName(Editor)));
		p_overlay->draw_rect(rect, p_overlay->get_theme_color(SNAME("box_selection_stroke_color"), EditorStringName(Editor)), false, Math::round(EDSCALE));
	}
	if (modal == MODAL_LOOP_CUT) {
		for (uint32_t i = 0; i + 1 < loop_cut_preview.size(); i += 2) {
			Point2 a, b;
			if (_project(camera, loop_cut_preview[i], a) && _project(camera, loop_cut_preview[i + 1], b)) {
				p_overlay->draw_line(a, b, COLOR_PREVIEW, Math::round(2 * EDSCALE), true);
			}
		}
	}
	String text;
	if (modal != MODAL_NONE) {
		text = modal_readout;
	} else {
		static const char *mode_names[3] = { "Vertex", "Edge", "Face" };
		text = vformat(TTR("Edit Mode (%s)  Verts %d/%d  Edges %d/%d  Faces %d/%d"), TTR(mode_names[mesh->get_select_mode()]),
				mesh->get_selected_vertices().size(), mesh->get_vertex_count(), mesh->get_selected_edges().size(), mesh->get_edge_count(), mesh->get_selected_faces().size(), mesh->get_face_count());
	}
	Ref<Font> font = p_overlay->get_theme_font(SceneStringName(font), SNAME("Label"));
	int font_size = p_overlay->get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	Point2 position = Point2(20 * EDSCALE, p_overlay->get_size().y - 20 * EDSCALE);
	p_overlay->draw_string(font, position + Point2(1, 1), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.8));
	p_overlay->draw_string(font, position, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, modal != MODAL_NONE ? COLOR_EDGE_SELECTED : Color(1, 1, 1));
}

// ---------------------------------------------------------------------------
// Toolbar, menus, dialog.

void TetraMeshEditPlugin::_update_toolbar() {
	const bool editing = edited != nullptr;
	edit_button->set_pressed_no_signal(editing);
	edit_button->set_disabled(!target);
	for (int i = 0; i < 3; i++) {
		mode_buttons[i]->set_visible(editing);
		mode_buttons[i]->set_pressed_no_signal(editing && mesh.is_valid() && mesh->get_select_mode() == i);
	}
	select_menu->set_visible(editing);
	mesh_menu->set_visible(editing);
}

void TetraMeshEditPlugin::_edit_toggled(bool p_pressed) {
	if (p_pressed && target) {
		enter_edit_mode(target);
	} else {
		exit_edit_mode();
	}
	_update_toolbar();
}

void TetraMeshEditPlugin::_mode_pressed(int p_mode) {
	set_select_mode(TetraEditMesh::SelectMode(p_mode));
}

void TetraMeshEditPlugin::_select_menu_pressed(int p_id) {
	_mesh_menu_pressed(p_id);
}

void TetraMeshEditPlugin::_mesh_menu_pressed(int p_id) {
	ERR_FAIL_INDEX(p_id, operations.size());
	Dictionary operation = operations[p_id];
	_request_operation(operation["id"]);
}

void TetraMeshEditPlugin::_delete_menu_pressed(int p_id) {
	_mesh_menu_pressed(p_id);
}

void TetraMeshEditPlugin::_merge_menu_pressed(int p_id) {
	_mesh_menu_pressed(p_id);
}

void TetraMeshEditPlugin::_add_menu_pressed(int p_id) {
	Array primitives = TetraEditMesh::get_primitive_operations();
	ERR_FAIL_INDEX(p_id, primitives.size());
	Dictionary primitive = primitives[p_id];
	_request_operation("add:" + String(primitive["id"]));
}

void TetraMeshEditPlugin::_popup_at_mouse(PopupMenu *p_menu) {
	Control *surface = _surface_of(_camera());
	Point2 position = surface->get_screen_position() + (have_mouse ? last_mouse : surface->get_size() * 0.5);
	p_menu->set_position(position);
	p_menu->reset_size();
	p_menu->popup();
}

// Runs an operation from a menu: at once when it has no parameters, through
// the dialog (with a live preview) when it has.
void TetraMeshEditPlugin::_request_operation(const String &p_id) {
	Dictionary operation;
	if (p_id.begins_with("add:")) {
		Array primitives = TetraEditMesh::get_primitive_operations();
		for (int i = 0; i < primitives.size(); i++) {
			Dictionary primitive = primitives[i];
			if (primitive["id"] == p_id.trim_prefix("add:")) {
				operation = primitive.duplicate(true);
				operation["id"] = p_id;
				operation["label"] = vformat(TTR("Add %s"), String(primitive["label"]));
			}
		}
	} else {
		for (int i = 0; i < operations.size(); i++) {
			Dictionary candidate = operations[i];
			if (candidate["id"] == p_id) {
				operation = candidate.duplicate(true);
				operation["label"] = String(candidate["label"]).trim_suffix("...");
			}
		}
	}
	ERR_FAIL_COND(operation.is_empty());
	Array params = operation["params"];
	if (params.is_empty()) {
		if (String(operation.get("category", String())) == "Select" && edited) {
			_execute(p_id, Dictionary());
			_refresh(false);
		} else {
			run_operation(p_id);
		}
		return;
	}
	if (modal != MODAL_NONE) {
		_finish_modal(false);
	}
	dialog_adds_node = p_id.begins_with("add:") && !edited;
	if (!dialog_adds_node && !edited) {
		return;
	}
	dialog_before = dialog_adds_node ? PackedByteArray() : mesh->get_state();
	dialog->set_preview_enabled(!dialog_adds_node);
	dialog->popup_operation(operation, last_values.get(p_id, Dictionary()));
	_dialog_preview(dialog->get_values());
}

void TetraMeshEditPlugin::_dialog_preview(const Dictionary &p_values) {
	if (dialog_adds_node || !edited || mesh.is_null()) {
		return;
	}
	mesh->set_state(dialog_before);
	if (dialog->is_preview_checked()) {
		_execute(dialog->get_operation().get("id", String()), p_values);
	}
	_refresh();
}

void TetraMeshEditPlugin::_dialog_confirmed(const String &p_id, const Dictionary &p_values) {
	if (dialog_adds_node) {
		last_values[p_id] = p_values;
		add_mesh(p_id.trim_prefix("add:"), p_values);
		return;
	}
	if (!edited || mesh.is_null()) {
		return;
	}
	mesh->set_state(dialog_before);
	if (!run_operation(p_id, p_values)) {
		_refresh();
	}
}

void TetraMeshEditPlugin::_dialog_cancelled(const String &p_id) {
	if (dialog_adds_node || !edited || mesh.is_null() || dialog_before.is_empty()) {
		return;
	}
	mesh->set_state(dialog_before);
	dialog_before = PackedByteArray();
	_refresh();
}

void TetraMeshEditPlugin::_add_to_scene(Node *p_node, const String &p_action) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!root) {
		// An empty scene tab: the mesh arrives together with a root.
		Node3D *new_root = memnew(Node3D);
		new_root->set_name("Node3D");
		new_root->add_child(p_node, true);
		p_node->set_owner(new_root);
		undo_redo->create_action_for_history(p_action, EditorNode::get_editor_data().get_current_edited_scene_history_id());
		undo_redo->add_do_method(EditorNode::get_singleton(), "set_edited_scene", new_root);
		undo_redo->add_do_method(SceneTreeDock::get_singleton()->get_tree_editor(), "update_tree");
		undo_redo->add_do_reference(new_root);
		undo_redo->add_undo_method(EditorNode::get_singleton(), "set_edited_scene", (Object *)nullptr);
		undo_redo->commit_action();
	} else {
		Node *parent = root;
		const List<Node *> selection = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
		if (selection.size() == 1 && (selection.front()->get() == root || root->is_ancestor_of(selection.front()->get()))) {
			parent = selection.front()->get()->get_parent() && selection.front()->get() != root ? selection.front()->get()->get_parent() : root;
		}
		undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, parent);
		undo_redo->add_do_method(parent, "add_child", p_node, true);
		undo_redo->add_do_method(p_node, "set_owner", root);
		undo_redo->add_do_reference(p_node);
		undo_redo->add_undo_method(parent, "remove_child", p_node);
		undo_redo->commit_action();
	}
	EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
	selection->clear();
	selection->add_node(p_node);
	EditorNode::get_singleton()->edit_node(p_node);
}

MeshInstance3D *TetraMeshEditPlugin::add_mesh(const String &p_primitive, const Dictionary &p_values) {
	Ref<TetraEditMesh> created;
	created.instantiate();
	String label;
	Array primitives = TetraEditMesh::get_primitive_operations();
	for (int i = 0; i < primitives.size(); i++) {
		Dictionary primitive = primitives[i];
		if (primitive["id"] == p_primitive) {
			label = primitive["label"];
		}
	}
	ERR_FAIL_COND_V_MSG(label.is_empty(), nullptr, vformat("Unknown primitive '%s'.", p_primitive));
	created->add_primitive(p_primitive, p_values);
	if (edited) {
		exit_edit_mode();
	}
	MeshInstance3D *node = memnew(MeshInstance3D);
	node->set_name(label.replace(" ", ""));
	node->set_meta(META_EDIT_MESH, created);
	node->set_mesh(created->to_array_mesh());
	_add_to_scene(node, vformat(TTR("Add %s"), label));
	return node;
}

void TetraMeshEditPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			if (!edited) {
				return;
			}
			_set_selection_box_visible(false);
			Transform3D transform = edited->get_global_transform();
			if (transform != overlay_transform && overlay_instance.is_valid()) {
				overlay_transform = transform;
				RenderingServer::get_singleton()->instance_set_transform(overlay_instance, transform);
			}
			if (modal == MODAL_NONE && !dialog->is_visible() && edited->get_mesh() != display_mesh) {
				// The mesh was replaced from outside (a Blender operator or
				// modifier, the inspector): edit what the node shows now.
				mesh->from_mesh(edited->get_mesh(), true);
				_refresh();
			}
		} break;
	}
}

void TetraMeshEditPlugin::_bind_methods() {
	ClassDB::bind_static_method("TetraMeshEditPlugin", D_METHOD("get_singleton"), &TetraMeshEditPlugin::get_singleton);
	ClassDB::bind_method(D_METHOD("enter_edit_mode", "node"), &TetraMeshEditPlugin::enter_edit_mode);
	ClassDB::bind_method(D_METHOD("exit_edit_mode"), &TetraMeshEditPlugin::exit_edit_mode);
	ClassDB::bind_method(D_METHOD("is_edit_mode"), &TetraMeshEditPlugin::is_edit_mode);
	ClassDB::bind_method(D_METHOD("get_edited_node"), &TetraMeshEditPlugin::get_edited_node);
	ClassDB::bind_method(D_METHOD("get_edit_mesh"), &TetraMeshEditPlugin::get_edit_mesh);
	ClassDB::bind_method(D_METHOD("set_select_mode", "mode"), &TetraMeshEditPlugin::set_select_mode);
	ClassDB::bind_method(D_METHOD("get_select_mode"), &TetraMeshEditPlugin::get_select_mode);
	ClassDB::bind_method(D_METHOD("get_operations"), &TetraMeshEditPlugin::get_operations);
	ClassDB::bind_method(D_METHOD("run_operation", "id", "values"), &TetraMeshEditPlugin::run_operation, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("request_operation", "id"), &TetraMeshEditPlugin::request_operation);
	ClassDB::bind_method(D_METHOD("get_operation_dialog"), &TetraMeshEditPlugin::get_operation_dialog);
	ClassDB::bind_method(D_METHOD("start_modal", "kind"), &TetraMeshEditPlugin::start_modal);
	ClassDB::bind_method(D_METHOD("get_modal"), &TetraMeshEditPlugin::get_modal);
	ClassDB::bind_method(D_METHOD("get_modal_readout"), &TetraMeshEditPlugin::get_modal_readout);
	ClassDB::bind_method(D_METHOD("add_mesh", "primitive", "values"), &TetraMeshEditPlugin::add_mesh, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("get_overlay_counts"), &TetraMeshEditPlugin::get_overlay_counts);
	ClassDB::bind_method(D_METHOD("pick", "camera", "position"), &TetraMeshEditPlugin::pick);
	ClassDB::bind_method(D_METHOD("refresh"), &TetraMeshEditPlugin::refresh);
	ClassDB::bind_method(D_METHOD("_apply_state", "node", "state"), &TetraMeshEditPlugin::_apply_state);
	ClassDB::bind_method(D_METHOD("_set_edit_mesh", "node", "edit_mesh", "node_mesh"), &TetraMeshEditPlugin::_set_edit_mesh);

	BIND_ENUM_CONSTANT(MODAL_NONE);
	BIND_ENUM_CONSTANT(MODAL_GRAB);
	BIND_ENUM_CONSTANT(MODAL_ROTATE);
	BIND_ENUM_CONSTANT(MODAL_SCALE);
	BIND_ENUM_CONSTANT(MODAL_INSET);
	BIND_ENUM_CONSTANT(MODAL_LOOP_CUT);
}

TetraMeshEditPlugin::TetraMeshEditPlugin() {
	singleton = this;
	_build_operations();

	// "Add > Mesh" is always there; the rest follows the selected mesh.
	add_menu = memnew(MenuButton);
	add_menu->set_text(TTR("Add"));
	add_menu->set_flat(false);
	add_menu->set_theme_type_variation("FlatMenuButton");
	add_menu->set_tooltip_text(TTR("Add an editable mesh to the scene, or to the mesh in Edit Mode."));
	add_mesh_menu = memnew(PopupMenu);
	Array primitives = TetraEditMesh::get_primitive_operations();
	for (int i = 0; i < primitives.size(); i++) {
		Dictionary primitive = primitives[i];
		add_mesh_menu->add_item(primitive["label"], i);
	}
	add_mesh_menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraMeshEditPlugin::_add_menu_pressed));
	add_menu->get_popup()->add_submenu_node_item(TTR("Mesh"), add_mesh_menu);
	Node3DEditor::get_singleton()->add_control_to_menu_panel(add_menu);

	toolbar = memnew(HBoxContainer);
	toolbar->hide();
	Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);

	edit_button = memnew(Button);
	edit_button->set_text(TTR("Edit Mode"));
	edit_button->set_theme_type_variation(SceneStringName(FlatButton));
	edit_button->set_toggle_mode(true);
	edit_button->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	edit_button->set_tooltip_text(TTR("Edit the vertices, edges and faces of the selected mesh (Tab)."));
	edit_button->connect(SceneStringName(toggled), callable_mp(this, &TetraMeshEditPlugin::_edit_toggled));
	toolbar->add_child(edit_button);

	static const char *mode_names[3] = { "Vertex", "Edge", "Face" };
	static const char *mode_tips[3] = { "Vertex select (1)", "Edge select (2)", "Face select (3)" };
	for (int i = 0; i < 3; i++) {
		Button *button = memnew(Button);
		button->set_text(TTR(mode_names[i]));
		button->set_theme_type_variation(SceneStringName(FlatButton));
		button->set_toggle_mode(true);
		button->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
		button->set_tooltip_text(TTR(mode_tips[i]));
		button->connect(SceneStringName(pressed), callable_mp(this, &TetraMeshEditPlugin::_mode_pressed).bind(i));
		toolbar->add_child(button);
		mode_buttons[i] = button;
	}

	select_menu = memnew(MenuButton);
	select_menu->set_text(TTR("Select"));
	select_menu->set_flat(false);
	select_menu->set_theme_type_variation("FlatMenuButton");
	toolbar->add_child(select_menu);
	mesh_menu = memnew(MenuButton);
	mesh_menu->set_text(TTR("Mesh"));
	mesh_menu->set_flat(false);
	mesh_menu->set_theme_type_variation("FlatMenuButton");
	toolbar->add_child(mesh_menu);
	delete_menu = memnew(PopupMenu);
	add_menu->add_child(delete_menu);
	merge_menu = memnew(PopupMenu);
	add_menu->add_child(merge_menu);

	String category;
	for (int i = 0; i < operations.size(); i++) {
		Dictionary operation = operations[i];
		String operation_category = operation["category"];
		if (operation_category == "Select") {
			select_menu->get_popup()->add_item(operation["label"], i);
			continue;
		}
		if (operation_category != category) {
			mesh_menu->get_popup()->add_separator(TTR(operation_category));
			category = operation_category;
		}
		mesh_menu->get_popup()->add_item(operation["label"], i);
		if (operation_category == "Delete") {
			delete_menu->add_item(String(operation["label"]).trim_prefix("Delete "), i);
		} else if (operation_category == "Merge") {
			merge_menu->add_item(String(operation["label"]).trim_prefix("Merge "), i);
		}
	}
	select_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &TetraMeshEditPlugin::_select_menu_pressed));
	mesh_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &TetraMeshEditPlugin::_mesh_menu_pressed));
	delete_menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraMeshEditPlugin::_delete_menu_pressed));
	merge_menu->connect(SceneStringName(id_pressed), callable_mp(this, &TetraMeshEditPlugin::_merge_menu_pressed));

	dialog = memnew(TetraOperationDialog);
	dialog->set_preview_enabled(true);
	// On the always visible button: "Add > Mesh" works with nothing selected.
	add_menu->add_child(dialog);
	dialog->connect("values_changed", callable_mp(this, &TetraMeshEditPlugin::_dialog_preview));
	dialog->connect("preview_toggled", callable_mp(this, &TetraMeshEditPlugin::_dialog_preview).unbind(1).bind(Dictionary()));
	dialog->connect("operation_confirmed", callable_mp(this, &TetraMeshEditPlugin::_dialog_confirmed));
	dialog->connect("operation_cancelled", callable_mp(this, &TetraMeshEditPlugin::_dialog_cancelled));

	_update_toolbar();
}

TetraMeshEditPlugin::~TetraMeshEditPlugin() {
	_free_overlay();
	if (singleton == this) {
		singleton = nullptr;
	}
}
