#pragma once

#include "core/io/resource.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/typed_array.h"
#include "scene/resources/mesh.h"

// A polygon mesh with real topology (vertices, explicit edges, faces of any
// vertex count), Blender-style selection and the modelling operations of the
// native Edit Mode. It is plain data plus algorithms, so every operation can be
// driven from a headless script.
//
// Edges are explicit: every face edge exists in the edge list, and edges
// without faces ("wire" edges) and vertices without edges are allowed, as in
// Blender. Indices are stable between operations that do not change topology.
class TetraEditMesh : public Resource {
	GDCLASS(TetraEditMesh, Resource);

public:
	enum SelectMode {
		SELECT_VERTEX,
		SELECT_EDGE,
		SELECT_FACE,
	};

	enum PivotMode {
		PIVOT_MEDIAN,
		PIVOT_BOUNDING_BOX,
	};

	enum DeleteMode {
		DELETE_VERTICES,
		DELETE_EDGES,
		DELETE_FACES,
		DELETE_ONLY_FACES,
		DELETE_ONLY_EDGES_AND_FACES,
	};

	enum FillType {
		FILL_NOTHING,
		FILL_NGON,
		FILL_TRIANGLE_FAN,
	};

	typedef LocalVector<int> IntList;
	typedef LocalVector<uint8_t> Mask;

	struct Face {
		IntList verts;
		LocalVector<Vector2> uvs; // One per corner.
		int material = 0;
		bool smooth = false;
	};

private:
	LocalVector<Vector3> verts;
	LocalVector<Vector2i> edges; // x < y.
	LocalVector<Face> faces;
	Mask vsel;
	Mask esel;
	Mask fsel;
	SelectMode select_mode = SELECT_VERTEX;
	Array materials;

	mutable HashMap<uint64_t, int> edge_map;
	mutable bool edge_map_dirty = true;

	// Rung edges created by the last ladder subdivision (loop cut selection).
	LocalVector<Vector2i> last_rungs;

	struct Orphans {
		HashSet<uint64_t> wire;
		Mask loose;
	};

	static _FORCE_INLINE_ uint64_t _edge_key(int p_a, int p_b) {
		if (p_a > p_b) {
			SWAP(p_a, p_b);
		}
		return (uint64_t(uint32_t(p_a)) << 32) | uint64_t(uint32_t(p_b));
	}

	void _ensure_edge_map() const;
	int _find_edge(int p_a, int p_b) const;
	int _ensure_edge(int p_a, int p_b);
	int _add_vertex(const Vector3 &p_position);
	int _add_face(const IntList &p_verts, const LocalVector<Vector2> &p_uvs, int p_material, bool p_smooth);
	bool _has_face_with_verts(const IntList &p_verts) const;
	void _compact(const Mask &p_vdel, const Mask &p_edel, const Mask &p_fdel);
	void _build_edge_faces(LocalVector<IntList> &r_edge_faces) const;
	void _build_vertex_edges(LocalVector<IntList> &r_vertex_edges) const;
	void _build_vertex_faces(LocalVector<IntList> &r_vertex_faces) const;
	void _flush_selection();
	void _select_only_vertices(const Mask &p_mask);
	void _record_orphans(Orphans &r_orphans) const;
	void _remove_orphans(const Orphans &p_orphans);
	void _topology_changed();

	Vector3 _face_normal_raw(const Face &p_face) const;
	void _triangulate_face(const Face &p_face, IntList &r_corners) const;
	void _reverse_face(Face &p_face);
	bool _join_faces(const IntList &p_faces, IntList &r_delete);
	void _weld(const IntList &p_target);
	bool _subdivide_edges(const Mask &p_edge_mask, int p_cuts);
	void _selected_edge_cycles(LocalVector<IntList> &r_cycles, bool &r_all_closed) const;
	int _dissolve_two_edge_vertices(const Mask &p_candidates);
	int _join_triangles(const Mask &p_face_mask, float p_face_angle, float p_shape_angle);
	void _append_blender(const LocalVector<Vector3> &p_verts, const LocalVector<IntList> &p_faces, const LocalVector<LocalVector<Vector2>> &p_uvs, const LocalVector<Vector2i> &p_edges);

protected:
	static void _bind_methods();

public:
	// Raw access for the editor.
	const LocalVector<Vector3> &get_verts() const { return verts; }
	const LocalVector<Vector2i> &get_edge_list() const { return edges; }
	const LocalVector<Face> &get_face_list() const { return faces; }
	const Mask &get_vertex_selection() const { return vsel; }
	const Mask &get_edge_selection() const { return esel; }
	const Mask &get_face_selection() const { return fsel; }
	void triangulate_face(int p_face, IntList &r_corners) const;

	// Elements.
	void clear();
	int get_vertex_count() const { return verts.size(); }
	int get_edge_count() const { return edges.size(); }
	int get_face_count() const { return faces.size(); }
	int add_vertex(const Vector3 &p_position);
	int add_edge(int p_a, int p_b);
	int add_face(const PackedInt32Array &p_vertices, const PackedVector2Array &p_uvs = PackedVector2Array(), int p_material = 0, bool p_smooth = false);
	Vector3 get_vertex(int p_index) const;
	void set_vertex(int p_index, const Vector3 &p_position);
	PackedVector3Array get_vertex_positions() const;
	void set_vertex_positions(const PackedVector3Array &p_positions);
	Vector2i get_edge_vertices(int p_edge) const;
	int find_edge(int p_a, int p_b) const;
	PackedInt32Array get_edge_faces(int p_edge) const;
	PackedInt32Array get_face_vertices(int p_face) const;
	PackedVector2Array get_face_uvs(int p_face) const;
	void set_face_uvs(int p_face, const PackedVector2Array &p_uvs);
	int get_face_material(int p_face) const;
	void set_face_material(int p_face, int p_material);
	bool is_face_smooth(int p_face) const;
	void set_face_smooth(int p_face, bool p_smooth);
	Vector3 get_face_normal(int p_face) const;
	Vector3 get_face_center(int p_face) const;
	int find_face(const PackedInt32Array &p_vertices) const;
	void set_materials(const Array &p_materials);
	Array get_materials() const { return materials; }

	// Selection.
	void set_select_mode(SelectMode p_mode);
	SelectMode get_select_mode() const { return select_mode; }
	void select_vertex(int p_index, bool p_selected = true);
	void select_edge(int p_index, bool p_selected = true);
	void select_face(int p_index, bool p_selected = true);
	bool is_vertex_selected(int p_index) const;
	bool is_edge_selected(int p_index) const;
	bool is_face_selected(int p_index) const;
	PackedInt32Array get_selected_vertices() const;
	PackedInt32Array get_selected_edges() const;
	PackedInt32Array get_selected_faces() const;
	void select_all();
	void select_none();
	void select_invert();
	void select_linked();
	void select_linked_from_vertex(int p_vertex, bool p_selected = true);
	void select_more();
	void select_less();
	PackedInt32Array get_edge_loop(int p_edge) const;
	PackedInt32Array get_edge_ring(int p_edge) const;
	void select_edge_loop(int p_edge, bool p_extend = false);
	void select_edge_ring(int p_edge, bool p_extend = false);

	// Consistency.
	String validate() const;

	// Conversion and storage.
	Ref<ArrayMesh> to_array_mesh() const;
	void from_mesh(const Ref<Mesh> &p_mesh, bool p_merge_quads = false, float p_weld_distance = 0.00001);
	PackedByteArray get_state() const;
	bool set_state(const PackedByteArray &p_state);
	void set_data(const PackedByteArray &p_data) { set_state(p_data); }
	PackedByteArray get_data() const { return get_state(); }

	// Primitives. They append to the mesh and leave only the new geometry
	// selected. Built in Blender's Z-up layout and rotated to Godot's Y-up.
	void add_plane(float p_size = 2.0);
	void add_cube(float p_size = 2.0);
	void add_grid(int p_x_subdivisions = 10, int p_y_subdivisions = 10, float p_size = 2.0);
	void add_circle(int p_vertices = 32, float p_radius = 1.0, FillType p_fill = FILL_NOTHING);
	void add_cone(int p_vertices = 32, float p_radius1 = 1.0, float p_radius2 = 0.0, float p_depth = 2.0, FillType p_fill = FILL_NGON);
	void add_cylinder(int p_vertices = 32, float p_radius = 1.0, float p_depth = 2.0, FillType p_fill = FILL_NGON);
	void add_uv_sphere(int p_segments = 32, int p_rings = 16, float p_radius = 1.0);
	void add_ico_sphere(int p_subdivisions = 2, float p_radius = 1.0);
	static Array get_primitive_operations();
	bool add_primitive(const String &p_id, const Dictionary &p_values);

	// Transform.
	Vector3 get_selection_pivot(PivotMode p_mode = PIVOT_MEDIAN) const;
	Vector3 get_selection_normal() const;
	void transform_selection(const Transform3D &p_transform, const Vector3 &p_pivot, float p_proportional_radius = 0.0);
	void translate_selection(const Vector3 &p_offset, float p_proportional_radius = 0.0);
	void rotate_selection(const Vector3 &p_axis, float p_angle, PivotMode p_pivot = PIVOT_MEDIAN, float p_proportional_radius = 0.0);
	void scale_selection(const Vector3 &p_scale, PivotMode p_pivot = PIVOT_MEDIAN, float p_proportional_radius = 0.0);

	// Operations on the selection.
	bool extrude_region();
	bool extrude_faces_individual(float p_offset = 0.0);
	bool inset_faces(float p_thickness = 0.1, float p_depth = 0.0);
	bool subdivide(int p_cuts = 1);
	bool loop_cut(int p_edge, int p_cuts = 1);
	bool delete_selection(DeleteMode p_mode = DELETE_VERTICES);
	bool dissolve_vertices();
	bool dissolve_edges();
	bool dissolve_faces();
	bool merge_at_center();
	bool merge_collapse();
	int merge_by_distance(float p_distance = 0.0001);
	bool fill();
	bool bridge_edge_loops();
	bool flip_normals();
	bool recalculate_normals(bool p_inside = false);
	bool shade_smooth(bool p_smooth = true);
	bool duplicate_selection();
	Ref<TetraEditMesh> separate_selection();
	bool triangulate();
	int tris_to_quads(float p_face_angle = 0.698132, float p_shape_angle = 0.698132);

	TetraEditMesh();
};

VARIANT_ENUM_CAST(TetraEditMesh::SelectMode);
VARIANT_ENUM_CAST(TetraEditMesh::PivotMode);
VARIANT_ENUM_CAST(TetraEditMesh::DeleteMode);
VARIANT_ENUM_CAST(TetraEditMesh::FillType);
