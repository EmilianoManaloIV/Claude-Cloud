// The modelling operations of TetraEditMesh. Their behaviour follows Blender's
// BMesh operators (source/blender/bmesh/operators/bmo_extrude.cc, bmo_inset.cc,
// bmo_subdivide.cc, bmo_dissolve.cc, bmo_removedoubles.cc, bmo_normals.cc,
// bmo_join_triangles.cc; copyright Blender Authors, GPL-2.0-or-later). The
// code is written for this class's indexed data structure, not copied.

#include "tetra_edit_mesh.h"

namespace {

struct UnionFind {
	LocalVector<int> parent;

	void init(int p_count) {
		parent.resize(p_count);
		for (int i = 0; i < p_count; i++) {
			parent[i] = i;
		}
	}
	int find(int p_index) {
		while (parent[p_index] != p_index) {
			parent[p_index] = parent[parent[p_index]];
			p_index = parent[p_index];
		}
		return p_index;
	}
	void join(int p_a, int p_b) {
		int a = find(p_a);
		int b = find(p_b);
		if (a != b) {
			parent[MAX(a, b)] = MIN(a, b);
		}
	}
};

// True when p_loop goes from p_a directly to p_b.
bool loop_has_directed(const TetraEditMesh::IntList &p_loop, int p_a, int p_b) {
	for (uint32_t i = 0; i < p_loop.size(); i++) {
		if (p_loop[i] == p_a && p_loop[(i + 1) % p_loop.size()] == p_b) {
			return true;
		}
	}
	return false;
}

Vector2 corner_uv(const TetraEditMesh::Face &p_face, int p_vertex) {
	int64_t at = p_face.verts.find(p_vertex);
	return at >= 0 ? p_face.uvs[at] : Vector2();
}

bool any_set(const TetraEditMesh::Mask &p_mask) {
	for (uint8_t value : p_mask) {
		if (value) {
			return true;
		}
	}
	return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Transform.

Vector3 TetraEditMesh::get_selection_pivot(PivotMode p_mode) const {
	Vector3 sum;
	AABB bounds;
	int count = 0;
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (!vsel[v]) {
			continue;
		}
		sum += verts[v];
		if (count == 0) {
			bounds = AABB(verts[v], Vector3());
		} else {
			bounds.expand_to(verts[v]);
		}
		count++;
	}
	if (count == 0) {
		return Vector3();
	}
	return p_mode == PIVOT_BOUNDING_BOX ? bounds.get_center() : sum / count;
}

Vector3 TetraEditMesh::get_selection_normal() const {
	Vector3 normal;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fsel[f]) {
			normal += _face_normal_raw(faces[f]);
		}
	}
	if (normal.length_squared() < 1e-12) {
		normal = Vector3();
		for (uint32_t f = 0; f < faces.size(); f++) {
			for (int v : faces[f].verts) {
				if (vsel[v]) {
					normal += _face_normal_raw(faces[f]);
					break;
				}
			}
		}
	}
	if (normal.length_squared() < 1e-12) {
		return Vector3(0, 1, 0);
	}
	return normal.normalized();
}

void TetraEditMesh::transform_selection(const Transform3D &p_transform, const Vector3 &p_pivot, float p_proportional_radius) {
	LocalVector<Vector3> selected;
	if (p_proportional_radius > 0) {
		for (uint32_t v = 0; v < verts.size(); v++) {
			if (vsel[v]) {
				selected.push_back(verts[v]);
			}
		}
	}
	for (uint32_t v = 0; v < verts.size(); v++) {
		Vector3 moved = p_pivot + p_transform.basis.xform(verts[v] - p_pivot) + p_transform.origin;
		if (vsel[v]) {
			verts[v] = moved;
		} else if (p_proportional_radius > 0 && !selected.is_empty()) {
			real_t nearest = Math::INF;
			for (const Vector3 &point : selected) {
				nearest = MIN(nearest, point.distance_squared_to(verts[v]));
			}
			nearest = Math::sqrt(nearest);
			if (nearest < p_proportional_radius) {
				// Blender's "Smooth" falloff.
				real_t t = 1.0 - nearest / p_proportional_radius;
				real_t weight = 3.0 * t * t - 2.0 * t * t * t;
				verts[v] = verts[v].lerp(moved, weight);
			}
		}
	}
	emit_changed();
}

void TetraEditMesh::translate_selection(const Vector3 &p_offset, float p_proportional_radius) {
	transform_selection(Transform3D(Basis(), p_offset), Vector3(), p_proportional_radius);
}

void TetraEditMesh::rotate_selection(const Vector3 &p_axis, float p_angle, PivotMode p_pivot, float p_proportional_radius) {
	ERR_FAIL_COND(p_axis.length_squared() < 1e-12);
	transform_selection(Transform3D(Basis(p_axis.normalized(), p_angle), Vector3()), get_selection_pivot(p_pivot), p_proportional_radius);
}

void TetraEditMesh::scale_selection(const Vector3 &p_scale, PivotMode p_pivot, float p_proportional_radius) {
	transform_selection(Transform3D(Basis::from_scale(p_scale), Vector3()), get_selection_pivot(p_pivot), p_proportional_radius);
}

// ---------------------------------------------------------------------------
// Extrude and inset.

bool TetraEditMesh::extrude_region() {
	if (!any_set(vsel)) {
		return false;
	}
	Orphans orphans;
	_record_orphans(orphans);
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	const uint32_t old_vertex_count = verts.size();
	const uint32_t old_edge_count = edges.size();
	const uint32_t old_face_count = faces.size();

	// Blender: the original faces are deleted when the region borders a face
	// that is not part of it; otherwise only those with an interior edge are.
	bool delete_original = false;
	Mask interior_edge;
	interior_edge.resize_initialized(old_edge_count);
	Mask extrude_edge;
	extrude_edge.resize_initialized(old_edge_count);
	Mask has_selected_edge;
	has_selected_edge.resize_initialized(old_vertex_count);
	bool any_side = false;
	for (uint32_t e = 0; e < old_edge_count; e++) {
		if (!esel[e]) {
			continue;
		}
		has_selected_edge[edges[e].x] = 1;
		has_selected_edge[edges[e].y] = 1;
		int selected_faces = 0;
		int other_faces = 0;
		for (int f : edge_faces[e]) {
			if (fsel[f]) {
				selected_faces++;
			} else {
				other_faces++;
			}
		}
		if (selected_faces > 0 && other_faces > 0) {
			delete_original = true;
		}
		if (selected_faces >= 2) {
			interior_edge[e] = other_faces == 0;
		} else {
			extrude_edge[e] = 1;
			any_side = true;
		}
	}
	bool any_lone = false;
	for (uint32_t v = 0; v < old_vertex_count; v++) {
		any_lone = any_lone || (vsel[v] && !has_selected_edge[v]);
	}
	if (!any_side && !any_lone) {
		return false; // A closed region has nothing to extrude from.
	}

	IntList twin;
	twin.resize(old_vertex_count);
	for (uint32_t v = 0; v < old_vertex_count; v++) {
		twin[v] = vsel[v] ? _add_vertex(verts[v]) : -1;
	}

	// Side faces.
	for (uint32_t e = 0; e < old_edge_count; e++) {
		if (!extrude_edge[e]) {
			continue;
		}
		int a = edges[e].x;
		int b = edges[e].y;
		int material = 0;
		bool smooth = false;
		Vector2 uv_a, uv_b;
		int owner = -1;
		for (int f : edge_faces[e]) {
			if (fsel[f]) {
				owner = f;
			}
		}
		if (owner >= 0) {
			// The side continues the region's face: same direction below it.
			if (!loop_has_directed(faces[owner].verts, a, b)) {
				SWAP(a, b);
			}
		} else if (!edge_faces[e].is_empty()) {
			// An edge of a face that stays: the new face runs the other way.
			owner = edge_faces[e][0];
			if (loop_has_directed(faces[owner].verts, a, b)) {
				SWAP(a, b);
			}
		}
		if (owner >= 0) {
			material = faces[owner].material;
			smooth = faces[owner].smooth;
			uv_a = corner_uv(faces[owner], a);
			uv_b = corner_uv(faces[owner], b);
		}
		IntList loop;
		loop.push_back(a);
		loop.push_back(b);
		loop.push_back(twin[b]);
		loop.push_back(twin[a]);
		LocalVector<Vector2> uvs;
		uvs.push_back(uv_a);
		uvs.push_back(uv_b);
		uvs.push_back(uv_b);
		uvs.push_back(uv_a);
		_add_face(loop, uvs, material, smooth);
	}

	// The region's faces move to the new vertices.
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (!fsel[f]) {
			continue;
		}
		bool move = delete_original;
		IntList top;
		for (uint32_t i = 0; i < faces[f].verts.size(); i++) {
			int a = faces[f].verts[i];
			int b = faces[f].verts[(i + 1) % faces[f].verts.size()];
			int e = _find_edge(a, b);
			if (e >= 0 && e < int(old_edge_count) && interior_edge[e]) {
				move = true;
			}
			top.push_back(twin[a]);
		}
		if (move) {
			faces[f].verts = top;
			for (uint32_t i = 0; i < top.size(); i++) {
				_ensure_edge(top[i], top[(i + 1) % top.size()]);
			}
		} else {
			LocalVector<Vector2> uvs(faces[f].uvs);
			_add_face(top, uvs, faces[f].material, faces[f].smooth);
			_reverse_face(faces[f]);
		}
	}

	// Lone vertices become edges.
	for (uint32_t v = 0; v < old_vertex_count; v++) {
		if (vsel[v] && !has_selected_edge[v]) {
			_ensure_edge(v, twin[v]);
			orphans.wire.insert(_edge_key(v, twin[v]));
		}
	}

	Mask mask;
	mask.resize_initialized(verts.size());
	for (uint32_t v = old_vertex_count; v < verts.size(); v++) {
		mask[v] = 1;
	}
	_select_only_vertices(mask);
	_remove_orphans(orphans);
	_flush_selection();
	_topology_changed();
	return true;
}

bool TetraEditMesh::extrude_faces_individual(float p_offset) {
	if (!any_set(fsel)) {
		return false;
	}
	const uint32_t old_vertex_count = verts.size();
	const uint32_t old_face_count = faces.size();
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (!fsel[f]) {
			continue;
		}
		Face source = faces[f];
		Vector3 offset = _face_normal_raw(source).normalized() * p_offset;
		IntList top;
		for (int v : source.verts) {
			top.push_back(_add_vertex(verts[v] + offset));
		}
		uint32_t count = source.verts.size();
		for (uint32_t i = 0; i < count; i++) {
			uint32_t j = (i + 1) % count;
			IntList loop;
			loop.push_back(source.verts[i]);
			loop.push_back(source.verts[j]);
			loop.push_back(top[j]);
			loop.push_back(top[i]);
			LocalVector<Vector2> uvs;
			uvs.push_back(source.uvs[i]);
			uvs.push_back(source.uvs[j]);
			uvs.push_back(source.uvs[j]);
			uvs.push_back(source.uvs[i]);
			_add_face(loop, uvs, source.material, source.smooth);
		}
		faces[f].verts = top;
		for (uint32_t i = 0; i < count; i++) {
			_ensure_edge(top[i], top[(i + 1) % count]);
		}
	}
	Mask mask;
	mask.resize_initialized(verts.size());
	for (uint32_t v = old_vertex_count; v < verts.size(); v++) {
		mask[v] = 1;
	}
	_select_only_vertices(mask);
	_topology_changed();
	return true;
}

bool TetraEditMesh::inset_faces(float p_thickness, float p_depth) {
	if (!any_set(fsel)) {
		return false;
	}
	Orphans orphans;
	_record_orphans(orphans);
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	const uint32_t old_vertex_count = verts.size();
	const uint32_t old_edge_count = edges.size();
	const uint32_t old_face_count = faces.size();

	// Inward directions of the region's boundary edges, per vertex.
	LocalVector<LocalVector<Vector3>> inward;
	inward.resize(old_vertex_count);
	struct Border {
		int a;
		int b;
		int face;
	};
	LocalVector<Border> borders;
	for (uint32_t e = 0; e < old_edge_count; e++) {
		int owner = -1;
		int selected_faces = 0;
		for (int f : edge_faces[e]) {
			if (fsel[f]) {
				owner = f;
				selected_faces++;
			}
		}
		if (selected_faces != 1) {
			continue;
		}
		Border border;
		border.a = edges[e].x;
		border.b = edges[e].y;
		border.face = owner;
		if (!loop_has_directed(faces[owner].verts, border.a, border.b)) {
			SWAP(border.a, border.b);
		}
		Vector3 normal = _face_normal_raw(faces[owner]).normalized();
		Vector3 direction = normal.cross((verts[border.b] - verts[border.a]).normalized());
		inward[border.a].push_back(direction);
		inward[border.b].push_back(direction);
		borders.push_back(border);
	}
	if (borders.is_empty()) {
		return false;
	}

	// Vertex normals of the region, for the depth.
	LocalVector<Vector3> region_normals;
	region_normals.resize_initialized(old_vertex_count);
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (fsel[f]) {
			Vector3 normal = _face_normal_raw(faces[f]).normalized();
			for (int v : faces[f].verts) {
				region_normals[v] += normal;
			}
		}
	}

	IntList twin;
	twin.resize(old_vertex_count);
	for (uint32_t v = 0; v < old_vertex_count; v++) {
		twin[v] = -1;
		if (inward[v].is_empty()) {
			continue;
		}
		Vector3 offset;
		if (inward[v].size() == 2 && 1.0 + inward[v][0].dot(inward[v][1]) > 0.1) {
			// Even offset: the inset edges stay parallel at p_thickness.
			offset = (inward[v][0] + inward[v][1]) / (1.0 + inward[v][0].dot(inward[v][1]));
		} else {
			for (const Vector3 &direction : inward[v]) {
				offset += direction;
			}
			offset = offset.normalized();
		}
		twin[v] = _add_vertex(verts[v] + offset * p_thickness);
	}

	for (const Border &border : borders) {
		const Face &owner = faces[border.face];
		IntList loop;
		loop.push_back(border.a);
		loop.push_back(border.b);
		loop.push_back(twin[border.b]);
		loop.push_back(twin[border.a]);
		LocalVector<Vector2> uvs;
		uvs.push_back(corner_uv(owner, border.a));
		uvs.push_back(corner_uv(owner, border.b));
		uvs.push_back(corner_uv(owner, border.b));
		uvs.push_back(corner_uv(owner, border.a));
		int material = owner.material;
		bool smooth = owner.smooth;
		_add_face(loop, uvs, material, smooth);
	}

	Mask mask;
	mask.resize_initialized(verts.size());
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (!fsel[f]) {
			continue;
		}
		IntList &loop = faces[f].verts;
		for (int &v : loop) {
			Vector3 normal = region_normals[v].normalized();
			if (twin[v] >= 0) {
				v = twin[v];
			}
			if (!mask[v]) {
				mask[v] = 1;
				verts[v] += normal * p_depth;
			}
		}
		for (uint32_t i = 0; i < loop.size(); i++) {
			_ensure_edge(loop[i], loop[(i + 1) % loop.size()]);
		}
	}
	_select_only_vertices(mask);
	_remove_orphans(orphans);
	_flush_selection();
	_topology_changed();
	return true;
}

// ---------------------------------------------------------------------------
// Subdivide and loop cut.

bool TetraEditMesh::_subdivide_edges(const Mask &p_edge_mask, int p_cuts) {
	if (p_cuts < 1 || !any_set(p_edge_mask)) {
		return false;
	}
	const uint32_t old_edge_count = edges.size();
	const uint32_t old_face_count = faces.size();
	const int segments = p_cuts + 1;
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);

	LocalVector<IntList> edge_points;
	edge_points.resize(old_edge_count);
	for (uint32_t e = 0; e < old_edge_count; e++) {
		if (!p_edge_mask[e]) {
			continue;
		}
		Vector3 a = verts[edges[e].x];
		Vector3 b = verts[edges[e].y];
		for (int k = 1; k <= p_cuts; k++) {
			edge_points[e].push_back(_add_vertex(a.lerp(b, real_t(k) / segments)));
		}
	}
	// The k-th point (0..segments) on the cut edge from p_a to p_b.
	auto edge_point = [&](int p_a, int p_b, int p_k) -> int {
		if (p_k == 0) {
			return p_a;
		}
		if (p_k == segments) {
			return p_b;
		}
		int e = _find_edge(p_a, p_b);
		return edges[e].x == p_a ? edge_points[e][p_k - 1] : edge_points[e][p_cuts - p_k];
	};
	auto is_cut = [&](int p_a, int p_b) -> bool {
		int e = _find_edge(p_a, p_b);
		return e >= 0 && e < int(old_edge_count) && p_edge_mask[e];
	};

	last_rungs.clear();
	Mask fdel;
	fdel.resize_initialized(old_face_count);
	for (uint32_t f = 0; f < old_face_count; f++) {
		const Face source = faces[f];
		const uint32_t count = source.verts.size();
		uint32_t cut_count = 0;
		Mask side_cut;
		side_cut.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			side_cut[i] = is_cut(source.verts[i], source.verts[(i + 1) % count]);
			cut_count += side_cut[i];
		}
		if (cut_count == 0) {
			continue;
		}
		if (count == 4 && cut_count == 4) {
			// A grid of quads.
			LocalVector<int> grid;
			grid.resize((segments + 1) * (segments + 1));
			const int *c = source.verts.ptr();
			for (int j = 0; j <= segments; j++) {
				for (int i = 0; i <= segments; i++) {
					int index;
					if (j == 0) {
						index = edge_point(c[0], c[1], i);
					} else if (j == segments) {
						index = edge_point(c[3], c[2], i);
					} else if (i == 0) {
						index = edge_point(c[0], c[3], j);
					} else if (i == segments) {
						index = edge_point(c[1], c[2], j);
					} else {
						real_t s = real_t(i) / segments;
						real_t t = real_t(j) / segments;
						index = _add_vertex(verts[c[0]].lerp(verts[c[1]], s).lerp(verts[c[3]].lerp(verts[c[2]], s), t));
					}
					grid[j * (segments + 1) + i] = index;
				}
			}
			auto grid_uv = [&](int p_i, int p_j) -> Vector2 {
				real_t s = real_t(p_i) / segments;
				real_t t = real_t(p_j) / segments;
				return source.uvs[0].lerp(source.uvs[1], s).lerp(source.uvs[3].lerp(source.uvs[2], s), t);
			};
			for (int j = 0; j < segments; j++) {
				for (int i = 0; i < segments; i++) {
					IntList loop;
					LocalVector<Vector2> uvs;
					static const int di[4] = { 0, 1, 1, 0 };
					static const int dj[4] = { 0, 0, 1, 1 };
					for (int k = 0; k < 4; k++) {
						loop.push_back(grid[(j + dj[k]) * (segments + 1) + i + di[k]]);
						uvs.push_back(grid_uv(i + di[k], j + dj[k]));
					}
					_add_face(loop, uvs, source.material, source.smooth);
				}
			}
			fdel[f] = 1;
		} else if (count == 3 && cut_count == 3) {
			// A triangular grid.
			const int *c = source.verts.ptr();
			HashMap<int, int> points;
			auto point = [&](int p_i, int p_j) -> int {
				if (p_j == 0) {
					return edge_point(c[0], c[1], p_i);
				}
				if (p_i == 0) {
					return edge_point(c[0], c[2], p_j);
				}
				if (p_i + p_j == segments) {
					return edge_point(c[1], c[2], p_j);
				}
				int key = p_j * (segments + 1) + p_i;
				int *found = points.getptr(key);
				if (found) {
					return *found;
				}
				Vector3 position = verts[c[0]] + (verts[c[1]] - verts[c[0]]) * (real_t(p_i) / segments) + (verts[c[2]] - verts[c[0]]) * (real_t(p_j) / segments);
				int index = _add_vertex(position);
				points[key] = index;
				return index;
			};
			auto point_uv = [&](int p_i, int p_j) -> Vector2 {
				return source.uvs[0] + (source.uvs[1] - source.uvs[0]) * (real_t(p_i) / segments) + (source.uvs[2] - source.uvs[0]) * (real_t(p_j) / segments);
			};
			for (int j = 0; j < segments; j++) {
				for (int i = 0; i + j < segments; i++) {
					IntList loop;
					LocalVector<Vector2> uvs;
					loop.push_back(point(i, j));
					loop.push_back(point(i + 1, j));
					loop.push_back(point(i, j + 1));
					uvs.push_back(point_uv(i, j));
					uvs.push_back(point_uv(i + 1, j));
					uvs.push_back(point_uv(i, j + 1));
					_add_face(loop, uvs, source.material, source.smooth);
					if (i + j < segments - 1) {
						loop.clear();
						uvs.clear();
						loop.push_back(point(i + 1, j));
						loop.push_back(point(i + 1, j + 1));
						loop.push_back(point(i, j + 1));
						uvs.push_back(point_uv(i + 1, j));
						uvs.push_back(point_uv(i + 1, j + 1));
						uvs.push_back(point_uv(i, j + 1));
						_add_face(loop, uvs, source.material, source.smooth);
					}
				}
			}
			fdel[f] = 1;
		} else if (count == 4 && cut_count == 2 && side_cut[0] == side_cut[2]) {
			// Two opposite edges: a ladder of quads.
			int first = side_cut[0] ? 0 : 1;
			int c[4];
			Vector2 uv[4];
			for (int k = 0; k < 4; k++) {
				c[k] = source.verts[(first + k) % 4];
				uv[k] = source.uvs[(first + k) % 4];
			}
			for (int k = 0; k < segments; k++) {
				IntList loop;
				LocalVector<Vector2> uvs;
				loop.push_back(edge_point(c[0], c[1], k));
				loop.push_back(edge_point(c[0], c[1], k + 1));
				loop.push_back(edge_point(c[3], c[2], k + 1));
				loop.push_back(edge_point(c[3], c[2], k));
				uvs.push_back(uv[0].lerp(uv[1], real_t(k) / segments));
				uvs.push_back(uv[0].lerp(uv[1], real_t(k + 1) / segments));
				uvs.push_back(uv[3].lerp(uv[2], real_t(k + 1) / segments));
				uvs.push_back(uv[3].lerp(uv[2], real_t(k) / segments));
				_add_face(loop, uvs, source.material, source.smooth);
				if (k > 0) {
					last_rungs.push_back(Vector2i(loop[0], loop[3]));
				}
			}
			fdel[f] = 1;
		} else {
			// Any other pattern: the new vertices join the face's loop.
			IntList loop;
			LocalVector<Vector2> uvs;
			for (uint32_t i = 0; i < count; i++) {
				uint32_t j = (i + 1) % count;
				loop.push_back(source.verts[i]);
				uvs.push_back(source.uvs[i]);
				if (side_cut[i]) {
					for (int k = 1; k < segments; k++) {
						loop.push_back(edge_point(source.verts[i], source.verts[j], k));
						uvs.push_back(source.uvs[i].lerp(source.uvs[j], real_t(k) / segments));
					}
				}
			}
			faces[f].verts = loop;
			faces[f].uvs = uvs;
			for (uint32_t i = 0; i < loop.size(); i++) {
				_ensure_edge(loop[i], loop[(i + 1) % loop.size()]);
			}
		}
	}
	// The cut edges are replaced by their chains.
	Mask vdel, edel;
	edel.resize_initialized(edges.size());
	for (uint32_t e = 0; e < old_edge_count; e++) {
		if (!p_edge_mask[e]) {
			continue;
		}
		edel[e] = 1;
		if (edge_faces[e].is_empty()) {
			int previous = edges[e].x;
			for (int k = 1; k <= segments; k++) {
				int next = edge_point(edges[e].x, edges[e].y, k);
				_ensure_edge(previous, next);
				previous = next;
			}
		}
	}
	// _compact() would also drop the faces of the deleted edges; the ones that
	// were rebuilt in place no longer use them.
	edel.resize_initialized(edges.size());
	fdel.resize_initialized(faces.size());
	_compact(vdel, edel, fdel);
	return true;
}

bool TetraEditMesh::subdivide(int p_cuts) {
	Mask mask(esel);
	Mask selected(vsel);
	if (!_subdivide_edges(mask, p_cuts)) {
		return false;
	}
	// Blender leaves the subdivided region selected: the old and new vertices.
	uint32_t old_count = selected.size();
	selected.resize_initialized(verts.size());
	for (uint32_t v = old_count; v < verts.size(); v++) {
		selected[v] = 1;
	}
	_select_only_vertices(selected);
	_topology_changed();
	return true;
}

bool TetraEditMesh::loop_cut(int p_edge, int p_cuts) {
	ERR_FAIL_INDEX_V(p_edge, int(edges.size()), false);
	PackedInt32Array ring = get_edge_ring(p_edge);
	Mask mask;
	mask.resize_initialized(edges.size());
	for (int e : ring) {
		mask[e] = 1;
	}
	uint32_t old_count = verts.size();
	if (!_subdivide_edges(mask, p_cuts)) {
		return false;
	}
	// Select the new loops.
	for (uint32_t v = 0; v < verts.size(); v++) {
		vsel[v] = select_mode == SELECT_VERTEX && v >= old_count;
	}
	for (uint32_t e = 0; e < edges.size(); e++) {
		esel[e] = 0;
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		fsel[f] = 0;
	}
	if (select_mode == SELECT_EDGE) {
		for (const Vector2i &rung : last_rungs) {
			int e = _find_edge(rung.x, rung.y);
			if (e >= 0) {
				esel[e] = 1;
			}
		}
	}
	_flush_selection();
	_topology_changed();
	return true;
}

// ---------------------------------------------------------------------------
// Delete, dissolve, merge.

bool TetraEditMesh::delete_selection(DeleteMode p_mode) {
	if (!any_set(vsel)) {
		return false;
	}
	LocalVector<IntList> edge_faces;
	LocalVector<IntList> vertex_edges;
	_build_edge_faces(edge_faces);
	_build_vertex_edges(vertex_edges);
	Mask vdel, edel, fdel;
	vdel.resize_initialized(verts.size());
	edel.resize_initialized(edges.size());
	fdel.resize_initialized(faces.size());
	switch (p_mode) {
		case DELETE_VERTICES: {
			vdel = vsel;
		} break;
		case DELETE_EDGES: {
			edel = esel;
			for (uint32_t v = 0; v < verts.size(); v++) {
				bool all = !vertex_edges[v].is_empty();
				for (int e : vertex_edges[v]) {
					all = all && edel[e];
				}
				vdel[v] = all;
			}
		} break;
		case DELETE_FACES: {
			fdel = fsel;
			for (uint32_t e = 0; e < edges.size(); e++) {
				bool all = esel[e];
				for (int f : edge_faces[e]) {
					all = all && fdel[f];
				}
				edel[e] = all;
			}
			for (uint32_t v = 0; v < verts.size(); v++) {
				bool all = vsel[v];
				for (int e : vertex_edges[v]) {
					all = all && edel[e];
				}
				vdel[v] = all;
			}
		} break;
		case DELETE_ONLY_FACES: {
			fdel = fsel;
		} break;
		case DELETE_ONLY_EDGES_AND_FACES: {
			edel = esel;
			fdel = fsel;
		} break;
	}
	_compact(vdel, edel, fdel);
	select_none();
	_topology_changed();
	return true;
}

// Replaces a connected group of faces by the single face along its outline.
// Fails (changing nothing) when the outline is not one simple loop.
bool TetraEditMesh::_join_faces(const IntList &p_faces, IntList &r_delete) {
	if (p_faces.size() < 2) {
		return false;
	}
	HashMap<uint64_t, int> directed; // (from << 32 | to) -> face.
	for (int f : p_faces) {
		const IntList &loop = faces[f].verts;
		for (uint32_t i = 0; i < loop.size(); i++) {
			uint64_t key = (uint64_t(uint32_t(loop[i])) << 32) | uint32_t(loop[(i + 1) % loop.size()]);
			if (directed.has(key)) {
				return false; // Inconsistent winding.
			}
			directed[key] = f;
		}
	}
	HashMap<int, int> next;
	HashMap<int, int> owner;
	int start = -1;
	for (const KeyValue<uint64_t, int> &entry : directed) {
		int from = int(entry.key >> 32);
		int to = int(entry.key & 0xffffffff);
		uint64_t reverse = (uint64_t(uint32_t(to)) << 32) | uint32_t(from);
		if (directed.has(reverse)) {
			continue;
		}
		if (next.has(from)) {
			return false; // The outline touches itself.
		}
		next[from] = to;
		owner[from] = entry.value;
		if (start < 0 || from < start) {
			start = from;
		}
	}
	if (next.size() < 3) {
		return false;
	}
	IntList loop;
	LocalVector<Vector2> uvs;
	int vertex = start;
	do {
		if (!next.has(vertex) || loop.size() > next.size()) {
			return false;
		}
		loop.push_back(vertex);
		uvs.push_back(corner_uv(faces[owner[vertex]], vertex));
		vertex = next[vertex];
	} while (vertex != start);
	if (loop.size() != next.size()) {
		return false; // More than one outline (a ring of faces).
	}
	int material = faces[p_faces[0]].material;
	bool smooth = faces[p_faces[0]].smooth;
	bool selected = fsel[p_faces[0]];
	for (int f : p_faces) {
		r_delete.push_back(f);
	}
	int created = _add_face(loop, uvs, material, smooth);
	fsel[created] = selected;
	return true;
}

// Removes vertices that sit in the middle of exactly two edges, joining the
// edges. Returns the number removed.
int TetraEditMesh::_dissolve_two_edge_vertices(const Mask &p_candidates) {
	LocalVector<IntList> vertex_edges;
	LocalVector<IntList> vertex_faces;
	_build_vertex_edges(vertex_edges);
	_build_vertex_faces(vertex_faces);
	_ensure_edge_map();
	Mask vdel, edel, fdel;
	vdel.resize_initialized(verts.size());
	edel.resize_initialized(edges.size());
	int removed = 0;
	for (uint32_t v = 0; v < verts.size() && v < p_candidates.size(); v++) {
		if (!p_candidates[v] || vertex_edges[v].size() != 2) {
			continue;
		}
		int e1 = vertex_edges[v][0];
		int e2 = vertex_edges[v][1];
		int a = edges[e1].x == int(v) ? edges[e1].y : edges[e1].x;
		int b = edges[e2].x == int(v) ? edges[e2].y : edges[e2].x;
		if (a == b || _find_edge(a, b) >= 0) {
			continue;
		}
		bool possible = true;
		for (int f : vertex_faces[v]) {
			possible = possible && faces[f].verts.size() > 3;
		}
		if (!possible) {
			continue;
		}
		for (int f : vertex_faces[v]) {
			int64_t at = faces[f].verts.find(v);
			faces[f].verts.remove_at(at);
			faces[f].uvs.remove_at(at);
		}
		edge_map.erase(_edge_key(edges[e1].x, edges[e1].y));
		edge_map.erase(_edge_key(edges[e2].x, edges[e2].y));
		edges[e1] = Vector2i(MIN(a, b), MAX(a, b));
		edge_map[_edge_key(a, b)] = e1;
		esel[e1] = esel[e1] && esel[e2];
		edel[e2] = 1;
		vdel[v] = 1;
		for (int &e : vertex_edges[b]) {
			if (e == e2) {
				e = e1;
			}
		}
		vertex_edges[v].clear();
		removed++;
	}
	if (removed > 0) {
		// Compact by hand: _compact() would drop the faces around the removed
		// edges, which were already rewired.
		for (uint32_t e = 0; e < edges.size(); e++) {
			if (edel[e]) {
				// Park the dead edge on its (deleted) vertex pair so that no
				// face refers to it.
				edges[e] = Vector2i(-1, -1);
			}
		}
		IntList remap;
		remap.resize(verts.size());
		uint32_t count = 0;
		for (uint32_t v = 0; v < verts.size(); v++) {
			remap[v] = vdel[v] ? -1 : int(count);
			if (!vdel[v]) {
				verts[count] = verts[v];
				vsel[count] = vsel[v];
				count++;
			}
		}
		verts.resize(count);
		vsel.resize(count);
		count = 0;
		for (uint32_t e = 0; e < edges.size(); e++) {
			if (edel[e]) {
				continue;
			}
			edges[count] = Vector2i(remap[edges[e].x], remap[edges[e].y]);
			esel[count] = esel[e];
			count++;
		}
		edges.resize(count);
		esel.resize(count);
		for (Face &face : faces) {
			for (int &v : face.verts) {
				v = remap[v];
			}
		}
		edge_map_dirty = true;
	}
	return removed;
}

bool TetraEditMesh::dissolve_faces() {
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	UnionFind groups;
	groups.init(faces.size());
	for (uint32_t e = 0; e < edges.size(); e++) {
		int first = -1;
		for (int f : edge_faces[e]) {
			if (!fsel[f]) {
				continue;
			}
			if (first >= 0) {
				groups.join(first, f);
			}
			first = f;
		}
	}
	HashMap<int, IntList> regions;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fsel[f]) {
			regions[groups.find(f)].push_back(f);
		}
	}
	Orphans orphans;
	_record_orphans(orphans);
	IntList doomed;
	for (const KeyValue<int, IntList> &region : regions) {
		_join_faces(region.value, doomed);
	}
	if (doomed.is_empty()) {
		return false;
	}
	Mask vdel, edel, fdel;
	fdel.resize_initialized(faces.size());
	for (int f : doomed) {
		fdel[f] = 1;
	}
	_compact(vdel, edel, fdel);
	_remove_orphans(orphans);
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

bool TetraEditMesh::dissolve_edges() {
	if (!any_set(esel)) {
		return false;
	}
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	UnionFind groups;
	groups.init(faces.size());
	Mask grouped;
	grouped.resize_initialized(faces.size());
	Mask edel;
	edel.resize_initialized(edges.size());
	bool any = false;
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (!esel[e]) {
			continue;
		}
		if (edge_faces[e].size() == 2) {
			groups.join(edge_faces[e][0], edge_faces[e][1]);
			grouped[edge_faces[e][0]] = 1;
			grouped[edge_faces[e][1]] = 1;
			any = true;
		} else if (edge_faces[e].is_empty()) {
			edel[e] = 1;
			any = true;
		}
	}
	if (!any) {
		return false;
	}
	// Scratch: mark the end points of the edges that dissolve. The selection
	// is cleared at the end anyway.
	for (uint32_t v = 0; v < verts.size(); v++) {
		vsel[v] = 0;
	}
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (esel[e] && edge_faces[e].size() == 2) {
			vsel[edges[e].x] = 1;
			vsel[edges[e].y] = 1;
		}
	}
	HashMap<int, IntList> regions;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (grouped[f]) {
			regions[groups.find(f)].push_back(f);
		}
	}
	Orphans orphans;
	_record_orphans(orphans);
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (edel[e]) {
			orphans.wire.erase(_edge_key(edges[e].x, edges[e].y));
		}
	}
	IntList doomed;
	for (const KeyValue<int, IntList> &region : regions) {
		_join_faces(region.value, doomed);
	}
	Mask vdel, fdel;
	fdel.resize_initialized(faces.size());
	for (int f : doomed) {
		fdel[f] = 1;
	}
	edel.clear();
	_compact(vdel, edel, fdel);
	_remove_orphans(orphans);
	// The end points that are left between two edges go too (Blender's
	// "Dissolve Vertices" option, on by default).
	_dissolve_two_edge_vertices(Mask(vsel));
	for (uint32_t v = 0; v < verts.size(); v++) {
		vsel[v] = 0;
	}
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

bool TetraEditMesh::dissolve_vertices() {
	if (!any_set(vsel)) {
		return false;
	}
	LocalVector<IntList> vertex_faces;
	LocalVector<IntList> vertex_edges;
	_build_vertex_faces(vertex_faces);
	_build_vertex_edges(vertex_edges);
	UnionFind groups;
	groups.init(faces.size());
	Mask grouped;
	grouped.resize_initialized(faces.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (!vsel[v] || vertex_edges[v].size() <= 2) {
			continue;
		}
		for (int f : vertex_faces[v]) {
			groups.join(vertex_faces[v][0], f);
			grouped[f] = 1;
		}
	}
	HashMap<int, IntList> regions;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (grouped[f]) {
			regions[groups.find(f)].push_back(f);
		}
	}
	Orphans orphans;
	_record_orphans(orphans);
	uint32_t before = verts.size() + faces.size();
	IntList doomed;
	for (const KeyValue<int, IntList> &region : regions) {
		_join_faces(region.value, doomed);
	}
	Mask vdel, edel, fdel;
	fdel.resize_initialized(faces.size());
	for (int f : doomed) {
		fdel[f] = 1;
	}
	_compact(vdel, edel, fdel);
	_remove_orphans(orphans);
	_dissolve_two_edge_vertices(Mask(vsel));
	bool changed = verts.size() + faces.size() != before || !doomed.is_empty();
	for (uint32_t v = 0; v < verts.size(); v++) {
		vsel[v] = 0;
	}
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return changed;
}

// Moves every vertex onto p_target[vertex] and removes what degenerates.
void TetraEditMesh::_weld(const IntList &p_target) {
	Mask vdel, edel, fdel;
	vdel.resize_initialized(verts.size());
	fdel.resize_initialized(faces.size());
	HashSet<String> seen_faces;
	for (uint32_t f = 0; f < faces.size(); f++) {
		Face &face = faces[f];
		IntList loop;
		LocalVector<Vector2> uvs;
		for (uint32_t i = 0; i < face.verts.size(); i++) {
			int v = p_target[face.verts[i]];
			if (!loop.is_empty() && loop[loop.size() - 1] == v) {
				continue;
			}
			loop.push_back(v);
			uvs.push_back(face.uvs[i]);
		}
		if (loop.size() > 1 && loop[0] == loop[loop.size() - 1]) {
			loop.resize(loop.size() - 1);
			uvs.resize(uvs.size() - 1);
		}
		IntList sorted(loop);
		sorted.sort();
		bool valid = loop.size() >= 3;
		String key;
		for (uint32_t i = 0; i < sorted.size(); i++) {
			valid = valid && (i == 0 || sorted[i] != sorted[i - 1]);
			key += itos(sorted[i]) + ",";
		}
		if (!valid || seen_faces.has(key)) {
			fdel[f] = 1;
			continue;
		}
		seen_faces.insert(key);
		face.verts = loop;
		face.uvs = uvs;
	}
	LocalVector<Vector2i> new_edges;
	Mask new_esel;
	HashMap<uint64_t, int> index;
	for (uint32_t e = 0; e < edges.size(); e++) {
		int a = p_target[edges[e].x];
		int b = p_target[edges[e].y];
		if (a == b) {
			continue;
		}
		int *found = index.getptr(_edge_key(a, b));
		if (found) {
			new_esel[*found] = new_esel[*found] || esel[e];
		} else {
			index[_edge_key(a, b)] = new_edges.size();
			new_edges.push_back(Vector2i(MIN(a, b), MAX(a, b)));
			new_esel.push_back(esel[e]);
		}
	}
	edges = new_edges;
	esel = new_esel;
	edge_map_dirty = true;
	for (uint32_t v = 0; v < verts.size(); v++) {
		vdel[v] = p_target[v] != int(v);
	}
	// A face that lost a duplicate may need an edge no old edge maps to.
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fdel[f]) {
			continue;
		}
		for (uint32_t i = 0; i < faces[f].verts.size(); i++) {
			_ensure_edge(faces[f].verts[i], faces[f].verts[(i + 1) % faces[f].verts.size()]);
		}
	}
	_compact(vdel, edel, fdel);
}

bool TetraEditMesh::merge_at_center() {
	PackedInt32Array selected = get_selected_vertices();
	if (selected.size() < 2) {
		return false;
	}
	Vector3 center = get_selection_pivot(PIVOT_MEDIAN);
	IntList target;
	target.resize(verts.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		target[v] = vsel[v] ? selected[0] : int(v);
	}
	verts[selected[0]] = center;
	_weld(target);
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

bool TetraEditMesh::merge_collapse() {
	UnionFind groups;
	groups.init(verts.size());
	bool any = false;
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (esel[e]) {
			groups.join(edges[e].x, edges[e].y);
			any = true;
		}
	}
	if (!any) {
		return false;
	}
	LocalVector<Vector3> sums;
	LocalVector<int> counts;
	sums.resize_initialized(verts.size());
	counts.resize_initialized(verts.size());
	IntList target;
	target.resize(verts.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		target[v] = groups.find(v);
		sums[target[v]] += verts[v];
		counts[target[v]]++;
	}
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (target[v] == int(v) && counts[v] > 1) {
			verts[v] = sums[v] / counts[v];
		}
	}
	_weld(target);
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

int TetraEditMesh::merge_by_distance(float p_distance) {
	// Sweep along X.
	IntList order;
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (vsel[v]) {
			order.push_back(v);
		}
	}
	struct ByX {
		const LocalVector<Vector3> *positions;
		bool operator()(int p_a, int p_b) const {
			return (*positions)[p_a].x < (*positions)[p_b].x;
		}
	};
	SortArray<int, ByX> sorter;
	sorter.compare.positions = &verts;
	sorter.sort(order.ptr(), order.size());
	UnionFind groups;
	groups.init(verts.size());
	real_t limit = real_t(p_distance) * p_distance;
	for (uint32_t i = 0; i < order.size(); i++) {
		for (uint32_t j = i + 1; j < order.size(); j++) {
			if (verts[order[j]].x - verts[order[i]].x > p_distance) {
				break;
			}
			if (verts[order[i]].distance_squared_to(verts[order[j]]) <= limit) {
				groups.join(order[i], order[j]);
			}
		}
	}
	IntList target;
	target.resize(verts.size());
	int removed = 0;
	for (uint32_t v = 0; v < verts.size(); v++) {
		target[v] = groups.find(v);
		removed += target[v] != int(v);
	}
	if (removed == 0) {
		return 0;
	}
	_weld(target);
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return removed;
}

// ---------------------------------------------------------------------------
// Fill and bridge.

void TetraEditMesh::_selected_edge_cycles(LocalVector<IntList> &r_cycles, bool &r_all_closed) const {
	LocalVector<IntList> neighbours;
	neighbours.resize(verts.size());
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (esel[e]) {
			neighbours[edges[e].x].push_back(edges[e].y);
			neighbours[edges[e].y].push_back(edges[e].x);
		}
	}
	r_all_closed = true;
	Mask visited;
	visited.resize_initialized(verts.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (!neighbours[v].is_empty() && neighbours[v].size() != 2) {
			r_all_closed = false;
		}
	}
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (visited[v] || neighbours[v].size() != 2) {
			continue;
		}
		IntList cycle;
		int previous = -1;
		int current = v;
		bool closed = false;
		while (true) {
			visited[current] = 1;
			cycle.push_back(current);
			if (neighbours[current].size() != 2) {
				break;
			}
			int next = neighbours[current][0] != previous ? neighbours[current][0] : neighbours[current][1];
			if (next == int(v)) {
				closed = true;
				break;
			}
			if (visited[next]) {
				break;
			}
			previous = current;
			current = next;
		}
		if (closed && cycle.size() >= 3) {
			r_cycles.push_back(IntList(cycle));
		} else {
			r_all_closed = false;
		}
	}
}

bool TetraEditMesh::fill() {
	PackedInt32Array selected = get_selected_vertices();
	if (selected.size() < 2) {
		return false;
	}
	if (selected.size() == 2) {
		if (_find_edge(selected[0], selected[1]) >= 0) {
			return false;
		}
		_ensure_edge(selected[0], selected[1]);
		_select_only_vertices(Mask(vsel));
		_topology_changed();
		return true;
	}
	LocalVector<IntList> loops;
	bool all_closed = false;
	_selected_edge_cycles(loops, all_closed);
	if (loops.is_empty() || !all_closed) {
		// No closed edge loop: a face through the selected vertices, ordered
		// by angle around their centre.
		loops.clear();
		Vector3 center = get_selection_pivot(PIVOT_MEDIAN);
		int farthest = selected[0];
		for (int v : selected) {
			if (verts[v].distance_squared_to(center) > verts[farthest].distance_squared_to(center)) {
				farthest = v;
			}
		}
		Vector3 axis_x = (verts[farthest] - center).normalized();
		Vector3 normal;
		for (int v : selected) {
			Vector3 candidate = axis_x.cross(verts[v] - center);
			if (candidate.length_squared() > normal.length_squared()) {
				normal = candidate;
			}
		}
		if (normal.length_squared() < 1e-12) {
			return false; // Collinear.
		}
		normal.normalize();
		Vector3 axis_y = normal.cross(axis_x);
		struct Entry {
			real_t angle;
			int vertex;
			bool operator<(const Entry &p_other) const { return angle < p_other.angle; }
		};
		LocalVector<Entry> entries;
		for (int v : selected) {
			Vector3 offset = verts[v] - center;
			Entry entry;
			entry.angle = Math::atan2(offset.dot(axis_y), offset.dot(axis_x));
			entry.vertex = v;
			entries.push_back(entry);
		}
		entries.sort();
		IntList loop;
		for (const Entry &entry : entries) {
			loop.push_back(entry.vertex);
		}
		loops.push_back(IntList(loop));
	}
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	bool created = false;
	for (IntList &loop : loops) {
		if (_has_face_with_verts(loop)) {
			continue;
		}
		// Wind against the neighbouring faces.
		int material = 0;
		bool smooth = false;
		bool decided = false;
		for (uint32_t i = 0; i < loop.size() && !decided; i++) {
			int a = loop[i];
			int b = loop[(i + 1) % loop.size()];
			int e = _find_edge(a, b);
			if (e < 0 || e >= int(edge_faces.size()) || edge_faces[e].is_empty()) {
				continue;
			}
			const Face &neighbour = faces[edge_faces[e][0]];
			material = neighbour.material;
			smooth = neighbour.smooth;
			if (loop_has_directed(neighbour.verts, a, b)) {
				loop.reverse();
			}
			decided = true;
		}
		LocalVector<Vector2> uvs;
		_add_face(loop, uvs, material, smooth);
		created = true;
	}
	if (!created) {
		return false;
	}
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

bool TetraEditMesh::bridge_edge_loops() {
	LocalVector<IntList> loops;
	bool all_closed = false;
	_selected_edge_cycles(loops, all_closed);
	if (!all_closed || loops.size() != 2 || loops[0].size() != loops[1].size()) {
		return false;
	}
	const IntList &a = loops[0];
	const IntList &b = loops[1];
	const int count = a.size();
	// The rotation and direction of the second loop with the shortest rungs.
	int best_offset = 0;
	int best_direction = 1;
	real_t best = Math::INF;
	for (int direction = -1; direction <= 1; direction += 2) {
		for (int offset = 0; offset < count; offset++) {
			real_t total = 0;
			for (int i = 0; i < count; i++) {
				total += verts[a[i]].distance_to(verts[b[((offset + direction * i) % count + count) % count]]);
			}
			if (total < best - 1e-6) {
				best = total;
				best_offset = offset;
				best_direction = direction;
			}
		}
	}
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	auto match = [&](int p_i) -> int {
		return b[((best_offset + best_direction * p_i) % count + count) % count];
	};
	bool flip = false;
	bool decided = false;
	int material = 0;
	bool smooth = false;
	for (int i = 0; i < count && !decided; i++) {
		int a0 = a[i];
		int a1 = a[(i + 1) % count];
		int e = _find_edge(a0, a1);
		if (e >= 0 && !edge_faces[e].is_empty()) {
			const Face &neighbour = faces[edge_faces[e][0]];
			flip = loop_has_directed(neighbour.verts, a0, a1);
			material = neighbour.material;
			smooth = neighbour.smooth;
			decided = true;
			break;
		}
		int b0 = match(i);
		int b1 = match(i + 1);
		e = _find_edge(b0, b1);
		if (e >= 0 && !edge_faces[e].is_empty()) {
			const Face &neighbour = faces[edge_faces[e][0]];
			flip = loop_has_directed(neighbour.verts, b1, b0);
			material = neighbour.material;
			smooth = neighbour.smooth;
			decided = true;
		}
	}
	bool created = false;
	for (int i = 0; i < count; i++) {
		IntList loop;
		loop.push_back(a[i]);
		loop.push_back(a[(i + 1) % count]);
		loop.push_back(match(i + 1));
		loop.push_back(match(i));
		if (flip) {
			loop.reverse();
		}
		if (_has_face_with_verts(loop)) {
			continue;
		}
		LocalVector<Vector2> uvs;
		_add_face(loop, uvs, material, smooth);
		created = true;
	}
	if (!created) {
		return false;
	}
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

// ---------------------------------------------------------------------------
// Normals and shading.

bool TetraEditMesh::flip_normals() {
	bool any = false;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fsel[f]) {
			_reverse_face(faces[f]);
			any = true;
		}
	}
	if (any) {
		emit_changed();
	}
	return any;
}

bool TetraEditMesh::recalculate_normals(bool p_inside) {
	if (!any_set(fsel)) {
		return false;
	}
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	Mask visited;
	visited.resize_initialized(faces.size());
	for (uint32_t seed = 0; seed < faces.size(); seed++) {
		if (!fsel[seed] || visited[seed]) {
			continue;
		}
		// Make the island's winding consistent with its seed face.
		IntList island;
		IntList stack;
		stack.push_back(seed);
		visited[seed] = 1;
		while (!stack.is_empty()) {
			int f = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			island.push_back(f);
			const uint32_t count = faces[f].verts.size();
			for (uint32_t i = 0; i < count; i++) {
				int a = faces[f].verts[i];
				int b = faces[f].verts[(i + 1) % count];
				int e = _find_edge(a, b);
				if (e < 0 || edge_faces[e].size() != 2) {
					continue;
				}
				int other = edge_faces[e][0] == f ? edge_faces[e][1] : edge_faces[e][0];
				if (!fsel[other] || visited[other]) {
					continue;
				}
				if (loop_has_directed(faces[other].verts, a, b)) {
					_reverse_face(faces[other]);
				}
				visited[other] = 1;
				stack.push_back(other);
			}
		}
		// Signed volume about the island's centre: positive when the faces
		// point away from it.
		Vector3 center;
		int corner_count = 0;
		for (int f : island) {
			for (int v : faces[f].verts) {
				center += verts[v];
				corner_count++;
			}
		}
		center /= corner_count;
		real_t volume = 0;
		for (int f : island) {
			const IntList &loop = faces[f].verts;
			for (uint32_t i = 1; i + 1 < loop.size(); i++) {
				volume += (verts[loop[0]] - center).dot((verts[loop[i]] - center).cross(verts[loop[i + 1]] - center));
			}
		}
		if (Math::abs(volume) > 1e-9 && (volume < 0) != p_inside) {
			for (int f : island) {
				_reverse_face(faces[f]);
			}
		}
	}
	emit_changed();
	return true;
}

bool TetraEditMesh::shade_smooth(bool p_smooth) {
	bool any = false;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fsel[f]) {
			faces[f].smooth = p_smooth;
			any = true;
		}
	}
	if (any) {
		emit_changed();
	}
	return any;
}

// ---------------------------------------------------------------------------
// Duplicate, separate, triangulate, join triangles.

bool TetraEditMesh::duplicate_selection() {
	if (!any_set(vsel)) {
		return false;
	}
	const uint32_t old_vertex_count = verts.size();
	const uint32_t old_edge_count = edges.size();
	const uint32_t old_face_count = faces.size();
	IntList twin;
	twin.resize(old_vertex_count);
	for (uint32_t v = 0; v < old_vertex_count; v++) {
		twin[v] = vsel[v] ? _add_vertex(verts[v]) : -1;
	}
	for (uint32_t e = 0; e < old_edge_count; e++) {
		if (esel[e]) {
			_ensure_edge(twin[edges[e].x], twin[edges[e].y]);
		}
	}
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (!fsel[f]) {
			continue;
		}
		IntList loop;
		for (int v : faces[f].verts) {
			loop.push_back(twin[v]);
		}
		LocalVector<Vector2> uvs(faces[f].uvs);
		_add_face(loop, uvs, faces[f].material, faces[f].smooth);
	}
	Mask mask;
	mask.resize_initialized(verts.size());
	for (uint32_t v = old_vertex_count; v < verts.size(); v++) {
		mask[v] = 1;
	}
	_select_only_vertices(mask);
	_topology_changed();
	return true;
}

Ref<TetraEditMesh> TetraEditMesh::separate_selection() {
	Ref<TetraEditMesh> part;
	if (!any_set(vsel)) {
		return part;
	}
	part.instantiate();
	part->set_state(get_state());
	part->materials = materials.duplicate();
	Mask vdel, edel, fdel;
	vdel.resize(verts.size());
	edel.resize(edges.size());
	fdel.resize(faces.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		vdel[v] = !vsel[v];
	}
	for (uint32_t e = 0; e < edges.size(); e++) {
		edel[e] = !esel[e];
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		fdel[f] = !fsel[f];
	}
	part->_compact(vdel, edel, fdel);
	part->_flush_selection();
	part->_topology_changed();
	delete_selection(DELETE_FACES);
	return part;
}

bool TetraEditMesh::triangulate() {
	const uint32_t old_face_count = faces.size();
	Mask vdel, edel, fdel;
	fdel.resize_initialized(old_face_count);
	bool any = false;
	for (uint32_t f = 0; f < old_face_count; f++) {
		if (!fsel[f] || faces[f].verts.size() == 3) {
			continue;
		}
		const Face source = faces[f];
		IntList corners;
		_triangulate_face(source, corners);
		for (uint32_t i = 0; i + 2 < corners.size(); i += 3) {
			IntList loop;
			LocalVector<Vector2> uvs;
			for (int k = 0; k < 3; k++) {
				loop.push_back(source.verts[corners[i + k]]);
				uvs.push_back(source.uvs[corners[i + k]]);
			}
			_add_face(loop, uvs, source.material, source.smooth);
		}
		fdel[f] = 1;
		any = true;
	}
	if (!any) {
		return false;
	}
	_compact(vdel, edel, fdel);
	_select_only_vertices(Mask(vsel));
	_topology_changed();
	return true;
}

// Joins pairs of triangles into quads, best-shaped pairs first.
int TetraEditMesh::_join_triangles(const Mask &p_face_mask, float p_face_angle, float p_shape_angle) {
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	struct Candidate {
		real_t score;
		int edge;
		int quad[4];
		Vector2 uv[4];
		bool operator<(const Candidate &p_other) const { return score < p_other.score; }
	};
	LocalVector<Candidate> candidates;
	const real_t min_face_dot = Math::cos(MIN(real_t(p_face_angle), real_t(Math::PI)));
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (edge_faces[e].size() != 2) {
			continue;
		}
		int f1 = edge_faces[e][0];
		int f2 = edge_faces[e][1];
		if (!p_face_mask[f1] || !p_face_mask[f2]) {
			continue;
		}
		const Face &first = faces[f1];
		const Face &second = faces[f2];
		if (first.verts.size() != 3 || second.verts.size() != 3 || first.material != second.material || first.smooth != second.smooth) {
			continue;
		}
		int a = edges[e].x;
		int b = edges[e].y;
		if (!loop_has_directed(first.verts, a, b)) {
			SWAP(a, b);
		}
		if (!loop_has_directed(second.verts, b, a)) {
			continue; // Inconsistent winding.
		}
		int c = -1;
		int d = -1;
		for (int v : first.verts) {
			if (v != a && v != b) {
				c = v;
			}
		}
		for (int v : second.verts) {
			if (v != a && v != b) {
				d = v;
			}
		}
		if (c < 0 || d < 0 || c == d) {
			continue;
		}
		if (!corner_uv(first, a).is_equal_approx(corner_uv(second, a)) || !corner_uv(first, b).is_equal_approx(corner_uv(second, b))) {
			continue; // A UV seam.
		}
		Vector3 n1 = _face_normal_raw(first).normalized();
		Vector3 n2 = _face_normal_raw(second).normalized();
		if (n1.dot(n2) < min_face_dot - 1e-6) {
			continue;
		}
		Candidate candidate;
		candidate.edge = e;
		candidate.quad[0] = a;
		candidate.quad[1] = d;
		candidate.quad[2] = b;
		candidate.quad[3] = c;
		candidate.uv[0] = corner_uv(first, a);
		candidate.uv[1] = corner_uv(second, d);
		candidate.uv[2] = corner_uv(first, b);
		candidate.uv[3] = corner_uv(first, c);
		Vector3 normal = (n1 + n2).normalized();
		bool convex = true;
		real_t worst = 0;
		for (int i = 0; i < 4; i++) {
			Vector3 previous = verts[candidate.quad[(i + 3) % 4]] - verts[candidate.quad[i]];
			Vector3 next = verts[candidate.quad[(i + 1) % 4]] - verts[candidate.quad[i]];
			convex = convex && next.cross(previous).dot(normal) > 1e-9;
			worst = MAX(worst, Math::abs(real_t(Math::PI * 0.5) - previous.angle_to(next)));
		}
		if (!convex || worst > p_shape_angle + 1e-6) {
			continue;
		}
		candidate.score = worst + (1.0 - n1.dot(n2));
		candidates.push_back(candidate);
	}
	candidates.sort();
	Mask used;
	used.resize_initialized(faces.size());
	Mask vdel, edel, fdel;
	fdel.resize_initialized(faces.size());
	IntList dead_edges;
	int joined = 0;
	for (const Candidate &candidate : candidates) {
		int f1 = edge_faces[candidate.edge][0];
		int f2 = edge_faces[candidate.edge][1];
		if (used[f1] || used[f2]) {
			continue;
		}
		used[f1] = 1;
		used[f2] = 1;
		fdel[f1] = 1;
		fdel[f2] = 1;
		dead_edges.push_back(candidate.edge);
		IntList loop;
		LocalVector<Vector2> uvs;
		for (int i = 0; i < 4; i++) {
			loop.push_back(candidate.quad[i]);
			uvs.push_back(candidate.uv[i]);
		}
		int material = faces[f1].material;
		bool smooth = faces[f1].smooth;
		bool selected = fsel[f1] && fsel[f2];
		int created = _add_face(loop, uvs, material, smooth);
		fsel[created] = selected;
		joined++;
	}
	if (joined > 0) {
		edel.resize_initialized(edges.size());
		for (int e : dead_edges) {
			edel[e] = 1;
		}
		_compact(vdel, edel, fdel);
	}
	return joined;
}

int TetraEditMesh::tris_to_quads(float p_face_angle, float p_shape_angle) {
	int joined = _join_triangles(fsel, p_face_angle, p_shape_angle);
	if (joined > 0) {
		_select_only_vertices(Mask(vsel));
		_topology_changed();
	}
	return joined;
}
