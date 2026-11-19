#include "tetra_edit_mesh.h"

#include "core/io/marshalls.h"
#include "core/math/geometry_2d.h"
#include "core/object/class_db.h"

namespace {

const uint32_t TETRA_EDIT_MESH_MAGIC = 0x314d4554; // "TEM1"

struct ByteWriter {
	Vector<uint8_t> bytes;

	void u32(uint32_t p_value) {
		int at = bytes.size();
		bytes.resize(at + 4);
		encode_uint32(p_value, bytes.ptrw() + at);
	}
	void f32(float p_value) {
		int at = bytes.size();
		bytes.resize(at + 4);
		encode_float(p_value, bytes.ptrw() + at);
	}
	void raw(const uint8_t *p_data, int p_count) {
		int at = bytes.size();
		bytes.resize(at + p_count);
		if (p_count > 0) {
			memcpy(bytes.ptrw() + at, p_data, p_count);
		}
	}
};

struct ByteReader {
	const uint8_t *data = nullptr;
	int64_t size = 0;
	int64_t at = 0;
	bool failed = false;

	bool need(int64_t p_count) {
		if (failed || p_count < 0 || at + p_count > size) {
			failed = true;
			return false;
		}
		return true;
	}
	uint32_t u32() {
		if (!need(4)) {
			return 0;
		}
		uint32_t value = decode_uint32(data + at);
		at += 4;
		return value;
	}
	float f32() {
		if (!need(4)) {
			return 0;
		}
		float value = decode_float(data + at);
		at += 4;
		return value;
	}
};

} // namespace

void TetraEditMesh::_ensure_edge_map() const {
	if (!edge_map_dirty) {
		return;
	}
	edge_map.clear();
	for (uint32_t i = 0; i < edges.size(); i++) {
		edge_map[_edge_key(edges[i].x, edges[i].y)] = i;
	}
	edge_map_dirty = false;
}

int TetraEditMesh::_find_edge(int p_a, int p_b) const {
	_ensure_edge_map();
	const int *found = edge_map.getptr(_edge_key(p_a, p_b));
	return found ? *found : -1;
}

int TetraEditMesh::_ensure_edge(int p_a, int p_b) {
	int found = _find_edge(p_a, p_b);
	if (found >= 0) {
		return found;
	}
	edges.push_back(Vector2i(MIN(p_a, p_b), MAX(p_a, p_b)));
	esel.push_back(0);
	edge_map[_edge_key(p_a, p_b)] = edges.size() - 1;
	return edges.size() - 1;
}

int TetraEditMesh::_add_vertex(const Vector3 &p_position) {
	verts.push_back(p_position);
	vsel.push_back(0);
	return verts.size() - 1;
}

int TetraEditMesh::_add_face(const IntList &p_verts, const LocalVector<Vector2> &p_uvs, int p_material, bool p_smooth) {
	Face face;
	face.verts = p_verts;
	face.uvs = p_uvs;
	face.uvs.resize_initialized(p_verts.size());
	face.material = p_material;
	face.smooth = p_smooth;
	for (uint32_t i = 0; i < p_verts.size(); i++) {
		_ensure_edge(p_verts[i], p_verts[(i + 1) % p_verts.size()]);
	}
	faces.push_back(face);
	fsel.push_back(0);
	return faces.size() - 1;
}

bool TetraEditMesh::_has_face_with_verts(const IntList &p_verts) const {
	for (const Face &face : faces) {
		if (face.verts.size() != p_verts.size()) {
			continue;
		}
		bool same = true;
		for (int v : p_verts) {
			if (!face.verts.has(v)) {
				same = false;
				break;
			}
		}
		if (same) {
			return true;
		}
	}
	return false;
}

void TetraEditMesh::_topology_changed() {
	edge_map_dirty = true;
	emit_changed();
}

void TetraEditMesh::_compact(const Mask &p_vdel, const Mask &p_edel, const Mask &p_fdel) {
	Mask vdel(p_vdel);
	Mask edel(p_edel);
	Mask fdel(p_fdel);
	vdel.resize_initialized(verts.size());
	edel.resize_initialized(edges.size());
	fdel.resize_initialized(faces.size());
	_ensure_edge_map();
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (vdel[edges[e].x] || vdel[edges[e].y]) {
			edel[e] = 1;
		}
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		const IntList &loop = faces[f].verts;
		for (uint32_t i = 0; i < loop.size() && !fdel[f]; i++) {
			int e = _find_edge(loop[i], loop[(i + 1) % loop.size()]);
			if (vdel[loop[i]] || e < 0 || edel[e]) {
				fdel[f] = 1;
			}
		}
	}
	IntList remap;
	remap.resize_initialized(verts.size());
	uint32_t count = 0;
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (vdel[v]) {
			remap[v] = -1;
			continue;
		}
		remap[v] = count;
		verts[count] = verts[v];
		vsel[count] = vsel[v];
		count++;
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
	count = 0;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (fdel[f]) {
			continue;
		}
		if (count != f) {
			faces[count] = faces[f];
		}
		for (int &v : faces[count].verts) {
			v = remap[v];
		}
		fsel[count] = fsel[f];
		count++;
	}
	faces.resize(count);
	fsel.resize(count);
	edge_map_dirty = true;
}

void TetraEditMesh::_build_edge_faces(LocalVector<IntList> &r_edge_faces) const {
	r_edge_faces.clear();
	r_edge_faces.resize(edges.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		const IntList &loop = faces[f].verts;
		for (uint32_t i = 0; i < loop.size(); i++) {
			int e = _find_edge(loop[i], loop[(i + 1) % loop.size()]);
			if (e >= 0) {
				r_edge_faces[e].push_back(f);
			}
		}
	}
}

void TetraEditMesh::_build_vertex_edges(LocalVector<IntList> &r_vertex_edges) const {
	r_vertex_edges.clear();
	r_vertex_edges.resize(verts.size());
	for (uint32_t e = 0; e < edges.size(); e++) {
		r_vertex_edges[edges[e].x].push_back(e);
		r_vertex_edges[edges[e].y].push_back(e);
	}
}

void TetraEditMesh::_build_vertex_faces(LocalVector<IntList> &r_vertex_faces) const {
	r_vertex_faces.clear();
	r_vertex_faces.resize(verts.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int v : faces[f].verts) {
			r_vertex_faces[v].push_back(f);
		}
	}
}

void TetraEditMesh::_record_orphans(Orphans &r_orphans) const {
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	r_orphans.loose.clear();
	r_orphans.loose.resize(verts.size());
	for (uint32_t v = 0; v < verts.size(); v++) {
		r_orphans.loose[v] = 1;
	}
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (edge_faces[e].is_empty()) {
			r_orphans.wire.insert(_edge_key(edges[e].x, edges[e].y));
		}
		r_orphans.loose[edges[e].x] = 0;
		r_orphans.loose[edges[e].y] = 0;
	}
}

// Removes the edges that lost their last face and the vertices that lost their
// last edge during an operation. Edges and vertices that were already loose
// before it (recorded in p_orphans) are kept.
void TetraEditMesh::_remove_orphans(const Orphans &p_orphans) {
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	Mask vdel, edel, fdel;
	edel.resize_initialized(edges.size());
	vdel.resize_initialized(verts.size());
	Mask used;
	used.resize_initialized(verts.size());
	bool any = false;
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (edge_faces[e].is_empty() && !p_orphans.wire.has(_edge_key(edges[e].x, edges[e].y))) {
			edel[e] = 1;
			any = true;
		} else {
			used[edges[e].x] = 1;
			used[edges[e].y] = 1;
		}
	}
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (!used[v] && !(v < p_orphans.loose.size() && p_orphans.loose[v])) {
			vdel[v] = 1;
			any = true;
		}
	}
	if (any) {
		_compact(vdel, edel, fdel);
	}
}

void TetraEditMesh::_flush_selection() {
	switch (select_mode) {
		case SELECT_VERTEX: {
			for (uint32_t e = 0; e < edges.size(); e++) {
				esel[e] = vsel[edges[e].x] && vsel[edges[e].y];
			}
			for (uint32_t f = 0; f < faces.size(); f++) {
				bool all = true;
				for (int v : faces[f].verts) {
					all = all && vsel[v];
				}
				fsel[f] = all;
			}
		} break;
		case SELECT_EDGE: {
			for (uint32_t v = 0; v < verts.size(); v++) {
				vsel[v] = 0;
			}
			for (uint32_t e = 0; e < edges.size(); e++) {
				if (esel[e]) {
					vsel[edges[e].x] = 1;
					vsel[edges[e].y] = 1;
				}
			}
			for (uint32_t f = 0; f < faces.size(); f++) {
				const IntList &loop = faces[f].verts;
				bool all = true;
				for (uint32_t i = 0; i < loop.size() && all; i++) {
					int e = _find_edge(loop[i], loop[(i + 1) % loop.size()]);
					all = e >= 0 && esel[e];
				}
				fsel[f] = all;
			}
		} break;
		case SELECT_FACE: {
			for (uint32_t v = 0; v < verts.size(); v++) {
				vsel[v] = 0;
			}
			for (uint32_t e = 0; e < edges.size(); e++) {
				esel[e] = 0;
			}
			for (uint32_t f = 0; f < faces.size(); f++) {
				if (!fsel[f]) {
					continue;
				}
				const IntList &loop = faces[f].verts;
				for (uint32_t i = 0; i < loop.size(); i++) {
					vsel[loop[i]] = 1;
					int e = _find_edge(loop[i], loop[(i + 1) % loop.size()]);
					if (e >= 0) {
						esel[e] = 1;
					}
				}
			}
		} break;
	}
}

// Selects exactly the elements whose vertices are all in p_mask, then applies
// the select mode's flushing.
void TetraEditMesh::_select_only_vertices(const Mask &p_mask) {
	for (uint32_t v = 0; v < verts.size(); v++) {
		vsel[v] = v < p_mask.size() && p_mask[v];
	}
	SelectMode mode = select_mode;
	select_mode = SELECT_VERTEX;
	_flush_selection();
	select_mode = mode;
	_flush_selection();
}

Vector3 TetraEditMesh::_face_normal_raw(const Face &p_face) const {
	// Newell's method: twice the area times the unit normal.
	Vector3 normal;
	uint32_t count = p_face.verts.size();
	for (uint32_t i = 0; i < count; i++) {
		const Vector3 &a = verts[p_face.verts[i]];
		const Vector3 &b = verts[p_face.verts[(i + 1) % count]];
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}
	// Loops are counter-clockwise seen from outside (Blender's convention), so
	// this is the outward normal by the right-hand rule.
	return normal;
}

void TetraEditMesh::_triangulate_face(const Face &p_face, IntList &r_corners) const {
	uint32_t count = p_face.verts.size();
	if (count == 3) {
		r_corners.push_back(0);
		r_corners.push_back(1);
		r_corners.push_back(2);
		return;
	}
	Vector3 normal = _face_normal_raw(p_face).normalized();
	if (count == 4) {
		const Vector3 &a = verts[p_face.verts[0]];
		const Vector3 &b = verts[p_face.verts[1]];
		const Vector3 &c = verts[p_face.verts[2]];
		const Vector3 &d = verts[p_face.verts[3]];
		bool diagonal_02 = (b - a).cross(c - a).dot(normal) > 0 && (c - a).cross(d - a).dot(normal) > 0;
		bool diagonal_13 = (b - a).cross(d - a).dot(normal) > 0 && (c - b).cross(d - b).dot(normal) > 0;
		static const int split_02[6] = { 0, 1, 2, 0, 2, 3 };
		static const int split_13[6] = { 0, 1, 3, 1, 2, 3 };
		const int *split = (diagonal_02 || !diagonal_13) ? split_02 : split_13;
		for (int i = 0; i < 6; i++) {
			r_corners.push_back(split[i]);
		}
		return;
	}
	// Ear clipping in the face's plane.
	Vector3 axis_x = normal.get_any_perpendicular();
	Vector3 axis_y = normal.cross(axis_x);
	Vector<Vector2> polygon;
	polygon.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const Vector3 &p = verts[p_face.verts[i]];
		polygon.write[i] = Vector2(p.dot(axis_x), p.dot(axis_y));
	}
	Vector<int> triangles = Geometry2D::triangulate_polygon(polygon);
	if (triangles.size() == int(count - 2) * 3) {
		for (int i = 0; i < triangles.size(); i += 3) {
			int a = triangles[i];
			int b = triangles[i + 1];
			int c = triangles[i + 2];
			// Keep the polygon's winding: the corners must be in cyclic order.
			bool cyclic = (a < b && b < c) || (b < c && c < a) || (c < a && a < b);
			r_corners.push_back(a);
			r_corners.push_back(cyclic ? b : c);
			r_corners.push_back(cyclic ? c : b);
		}
		return;
	}
	for (uint32_t i = 1; i + 1 < count; i++) {
		r_corners.push_back(0);
		r_corners.push_back(i);
		r_corners.push_back(i + 1);
	}
}

void TetraEditMesh::triangulate_face(int p_face, IntList &r_corners) const {
	ERR_FAIL_INDEX(p_face, int(faces.size()));
	_triangulate_face(faces[p_face], r_corners);
}

void TetraEditMesh::_reverse_face(Face &p_face) {
	p_face.verts.reverse();
	p_face.uvs.reverse();
}

// ---------------------------------------------------------------------------
// Elements.

void TetraEditMesh::clear() {
	verts.clear();
	edges.clear();
	faces.clear();
	vsel.clear();
	esel.clear();
	fsel.clear();
	_topology_changed();
}

int TetraEditMesh::add_vertex(const Vector3 &p_position) {
	int index = _add_vertex(p_position);
	emit_changed();
	return index;
}

int TetraEditMesh::add_edge(int p_a, int p_b) {
	ERR_FAIL_INDEX_V(p_a, int(verts.size()), -1);
	ERR_FAIL_INDEX_V(p_b, int(verts.size()), -1);
	ERR_FAIL_COND_V(p_a == p_b, -1);
	int index = _ensure_edge(p_a, p_b);
	_flush_selection();
	emit_changed();
	return index;
}

int TetraEditMesh::add_face(const PackedInt32Array &p_vertices, const PackedVector2Array &p_uvs, int p_material, bool p_smooth) {
	ERR_FAIL_COND_V(p_vertices.size() < 3, -1);
	IntList loop;
	for (int v : p_vertices) {
		ERR_FAIL_INDEX_V(v, int(verts.size()), -1);
		ERR_FAIL_COND_V_MSG(loop.has(v), -1, "A face cannot use a vertex twice.");
		loop.push_back(v);
	}
	LocalVector<Vector2> uvs;
	for (int i = 0; i < p_uvs.size() && i < p_vertices.size(); i++) {
		uvs.push_back(p_uvs[i]);
	}
	int index = _add_face(loop, uvs, p_material, p_smooth);
	_flush_selection();
	emit_changed();
	return index;
}

Vector3 TetraEditMesh::get_vertex(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(verts.size()), Vector3());
	return verts[p_index];
}

void TetraEditMesh::set_vertex(int p_index, const Vector3 &p_position) {
	ERR_FAIL_INDEX(p_index, int(verts.size()));
	verts[p_index] = p_position;
	emit_changed();
}

PackedVector3Array TetraEditMesh::get_vertex_positions() const {
	PackedVector3Array positions;
	positions.resize(verts.size());
	for (uint32_t i = 0; i < verts.size(); i++) {
		positions.write[i] = verts[i];
	}
	return positions;
}

void TetraEditMesh::set_vertex_positions(const PackedVector3Array &p_positions) {
	ERR_FAIL_COND(p_positions.size() != int(verts.size()));
	for (uint32_t i = 0; i < verts.size(); i++) {
		verts[i] = p_positions[i];
	}
	emit_changed();
}

Vector2i TetraEditMesh::get_edge_vertices(int p_edge) const {
	ERR_FAIL_INDEX_V(p_edge, int(edges.size()), Vector2i(-1, -1));
	return edges[p_edge];
}

int TetraEditMesh::find_edge(int p_a, int p_b) const {
	return _find_edge(p_a, p_b);
}

PackedInt32Array TetraEditMesh::get_edge_faces(int p_edge) const {
	PackedInt32Array result;
	ERR_FAIL_INDEX_V(p_edge, int(edges.size()), result);
	for (uint32_t f = 0; f < faces.size(); f++) {
		const IntList &loop = faces[f].verts;
		for (uint32_t i = 0; i < loop.size(); i++) {
			if (_find_edge(loop[i], loop[(i + 1) % loop.size()]) == p_edge) {
				result.push_back(f);
				break;
			}
		}
	}
	return result;
}

PackedInt32Array TetraEditMesh::get_face_vertices(int p_face) const {
	PackedInt32Array result;
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), result);
	for (int v : faces[p_face].verts) {
		result.push_back(v);
	}
	return result;
}

PackedVector2Array TetraEditMesh::get_face_uvs(int p_face) const {
	PackedVector2Array result;
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), result);
	for (const Vector2 &uv : faces[p_face].uvs) {
		result.push_back(uv);
	}
	return result;
}

void TetraEditMesh::set_face_uvs(int p_face, const PackedVector2Array &p_uvs) {
	ERR_FAIL_INDEX(p_face, int(faces.size()));
	ERR_FAIL_COND(p_uvs.size() != int(faces[p_face].verts.size()));
	for (int i = 0; i < p_uvs.size(); i++) {
		faces[p_face].uvs[i] = p_uvs[i];
	}
	emit_changed();
}

int TetraEditMesh::get_face_material(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), 0);
	return faces[p_face].material;
}

void TetraEditMesh::set_face_material(int p_face, int p_material) {
	ERR_FAIL_INDEX(p_face, int(faces.size()));
	ERR_FAIL_COND(p_material < 0);
	faces[p_face].material = p_material;
	emit_changed();
}

bool TetraEditMesh::is_face_smooth(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), false);
	return faces[p_face].smooth;
}

void TetraEditMesh::set_face_smooth(int p_face, bool p_smooth) {
	ERR_FAIL_INDEX(p_face, int(faces.size()));
	faces[p_face].smooth = p_smooth;
	emit_changed();
}

Vector3 TetraEditMesh::get_face_normal(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), Vector3());
	return _face_normal_raw(faces[p_face]).normalized();
}

Vector3 TetraEditMesh::get_face_center(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, int(faces.size()), Vector3());
	Vector3 center;
	for (int v : faces[p_face].verts) {
		center += verts[v];
	}
	return center / faces[p_face].verts.size();
}

int TetraEditMesh::find_face(const PackedInt32Array &p_vertices) const {
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (int(faces[f].verts.size()) != p_vertices.size()) {
			continue;
		}
		bool same = true;
		for (int v : p_vertices) {
			same = same && faces[f].verts.has(v);
		}
		if (same) {
			return f;
		}
	}
	return -1;
}

void TetraEditMesh::set_materials(const Array &p_materials) {
	materials = p_materials;
	emit_changed();
}

// ---------------------------------------------------------------------------
// Selection.

void TetraEditMesh::set_select_mode(SelectMode p_mode) {
	if (p_mode == select_mode) {
		return;
	}
	// The arrays are consistent in the old mode. Going "up" (vertex -> edge ->
	// face) keeps only the elements that are entirely selected, as in Blender;
	// going "down" keeps everything.
	select_mode = p_mode;
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_vertex(int p_index, bool p_selected) {
	ERR_FAIL_INDEX(p_index, int(verts.size()));
	Mask mask(vsel);
	mask[p_index] = p_selected;
	if (select_mode == SELECT_VERTEX) {
		vsel[p_index] = p_selected;
		_flush_selection();
	} else {
		_select_only_vertices(mask);
	}
	emit_changed();
}

void TetraEditMesh::select_edge(int p_index, bool p_selected) {
	ERR_FAIL_INDEX(p_index, int(edges.size()));
	if (select_mode == SELECT_VERTEX) {
		vsel[edges[p_index].x] = p_selected;
		vsel[edges[p_index].y] = p_selected;
	} else if (select_mode == SELECT_EDGE) {
		esel[p_index] = p_selected;
	}
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_face(int p_index, bool p_selected) {
	ERR_FAIL_INDEX(p_index, int(faces.size()));
	const IntList &loop = faces[p_index].verts;
	if (select_mode == SELECT_VERTEX) {
		for (int v : loop) {
			vsel[v] = p_selected;
		}
	} else if (select_mode == SELECT_EDGE) {
		for (uint32_t i = 0; i < loop.size(); i++) {
			int e = _find_edge(loop[i], loop[(i + 1) % loop.size()]);
			if (e >= 0) {
				esel[e] = p_selected;
			}
		}
	} else {
		fsel[p_index] = p_selected;
	}
	_flush_selection();
	emit_changed();
}

bool TetraEditMesh::is_vertex_selected(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(verts.size()), false);
	return vsel[p_index];
}

bool TetraEditMesh::is_edge_selected(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(edges.size()), false);
	return esel[p_index];
}

bool TetraEditMesh::is_face_selected(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(faces.size()), false);
	return fsel[p_index];
}

static PackedInt32Array _mask_indices(const TetraEditMesh::Mask &p_mask) {
	PackedInt32Array result;
	for (uint32_t i = 0; i < p_mask.size(); i++) {
		if (p_mask[i]) {
			result.push_back(i);
		}
	}
	return result;
}

PackedInt32Array TetraEditMesh::get_selected_vertices() const {
	return _mask_indices(vsel);
}

PackedInt32Array TetraEditMesh::get_selected_edges() const {
	return _mask_indices(esel);
}

PackedInt32Array TetraEditMesh::get_selected_faces() const {
	return _mask_indices(fsel);
}

void TetraEditMesh::select_all() {
	for (uint32_t i = 0; i < verts.size(); i++) {
		vsel[i] = 1;
	}
	for (uint32_t i = 0; i < edges.size(); i++) {
		esel[i] = 1;
	}
	for (uint32_t i = 0; i < faces.size(); i++) {
		fsel[i] = 1;
	}
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_none() {
	for (uint32_t i = 0; i < verts.size(); i++) {
		vsel[i] = 0;
	}
	for (uint32_t i = 0; i < edges.size(); i++) {
		esel[i] = 0;
	}
	for (uint32_t i = 0; i < faces.size(); i++) {
		fsel[i] = 0;
	}
	emit_changed();
}

void TetraEditMesh::select_invert() {
	Mask &primary = select_mode == SELECT_VERTEX ? vsel : (select_mode == SELECT_EDGE ? esel : fsel);
	for (uint32_t i = 0; i < primary.size(); i++) {
		primary[i] = !primary[i];
	}
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_linked() {
	LocalVector<IntList> vertex_edges;
	_build_vertex_edges(vertex_edges);
	Mask mask(vsel);
	IntList stack;
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (mask[v]) {
			stack.push_back(v);
		}
	}
	while (!stack.is_empty()) {
		int v = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int e : vertex_edges[v]) {
			int other = edges[e].x == v ? edges[e].y : edges[e].x;
			if (!mask[other]) {
				mask[other] = 1;
				stack.push_back(other);
			}
		}
	}
	_select_only_vertices(mask);
	emit_changed();
}

void TetraEditMesh::select_linked_from_vertex(int p_vertex, bool p_selected) {
	ERR_FAIL_INDEX(p_vertex, int(verts.size()));
	LocalVector<IntList> vertex_edges;
	_build_vertex_edges(vertex_edges);
	Mask island;
	island.resize_initialized(verts.size());
	IntList stack;
	stack.push_back(p_vertex);
	island[p_vertex] = 1;
	while (!stack.is_empty()) {
		int v = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int e : vertex_edges[v]) {
			int other = edges[e].x == v ? edges[e].y : edges[e].x;
			if (!island[other]) {
				island[other] = 1;
				stack.push_back(other);
			}
		}
	}
	// Work on the mode's primary elements so that deselecting an island does
	// not take neighbours with it.
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (island[v]) {
			vsel[v] = p_selected;
		}
	}
	for (uint32_t e = 0; e < edges.size(); e++) {
		if (island[edges[e].x]) {
			esel[e] = p_selected;
		}
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (island[faces[f].verts[0]]) {
			fsel[f] = p_selected;
		}
	}
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_more() {
	if (select_mode == SELECT_VERTEX) {
		Mask mask(vsel);
		for (uint32_t e = 0; e < edges.size(); e++) {
			if (vsel[edges[e].x] || vsel[edges[e].y]) {
				mask[edges[e].x] = 1;
				mask[edges[e].y] = 1;
			}
		}
		vsel = mask;
	} else if (select_mode == SELECT_EDGE) {
		Mask mask(esel);
		for (uint32_t e = 0; e < edges.size(); e++) {
			if (vsel[edges[e].x] || vsel[edges[e].y]) {
				mask[e] = 1;
			}
		}
		esel = mask;
	} else {
		Mask mask(fsel);
		for (uint32_t f = 0; f < faces.size(); f++) {
			for (int v : faces[f].verts) {
				if (vsel[v]) {
					mask[f] = 1;
				}
			}
		}
		fsel = mask;
	}
	_flush_selection();
	emit_changed();
}

void TetraEditMesh::select_less() {
	// A vertex is on the selection's border when one of its elements of the
	// current mode is not selected.
	Mask border;
	border.resize_initialized(verts.size());
	if (select_mode == SELECT_FACE) {
		for (uint32_t f = 0; f < faces.size(); f++) {
			if (!fsel[f]) {
				for (int v : faces[f].verts) {
					border[v] = 1;
				}
			}
		}
		for (uint32_t f = 0; f < faces.size(); f++) {
			for (int v : faces[f].verts) {
				if (border[v]) {
					fsel[f] = 0;
				}
			}
		}
	} else {
		for (uint32_t e = 0; e < edges.size(); e++) {
			if (!esel[e]) {
				border[edges[e].x] = 1;
				border[edges[e].y] = 1;
			}
		}
		if (select_mode == SELECT_VERTEX) {
			for (uint32_t v = 0; v < verts.size(); v++) {
				if (border[v]) {
					vsel[v] = 0;
				}
			}
		} else {
			for (uint32_t e = 0; e < edges.size(); e++) {
				if (border[edges[e].x] || border[edges[e].y]) {
					esel[e] = 0;
				}
			}
		}
	}
	_flush_selection();
	emit_changed();
}

// Edge loop walk, after Blender's BMW_EDGELOOP walker: through vertices with
// four edges to the edge that shares no face with the current one, and along
// the mesh boundary for boundary edges.
PackedInt32Array TetraEditMesh::get_edge_loop(int p_edge) const {
	PackedInt32Array result;
	ERR_FAIL_INDEX_V(p_edge, int(edges.size()), result);
	LocalVector<IntList> edge_faces;
	LocalVector<IntList> vertex_edges;
	_build_edge_faces(edge_faces);
	_build_vertex_edges(vertex_edges);
	Mask visited;
	visited.resize_initialized(edges.size());
	visited[p_edge] = 1;
	result.push_back(p_edge);
	bool boundary = edge_faces[p_edge].size() == 1;
	for (int side = 0; side < 2; side++) {
		int edge = p_edge;
		int vertex = side == 0 ? edges[p_edge].y : edges[p_edge].x;
		while (true) {
			int next = -1;
			if (boundary) {
				int found = 0;
				for (int e : vertex_edges[vertex]) {
					if (e != edge && edge_faces[e].size() == 1) {
						next = e;
						found++;
					}
				}
				if (found != 1) {
					next = -1;
				}
			} else if (vertex_edges[vertex].size() == 4) {
				int found = 0;
				for (int e : vertex_edges[vertex]) {
					if (e == edge) {
						continue;
					}
					bool shares = false;
					for (int f : edge_faces[e]) {
						shares = shares || edge_faces[edge].has(f);
					}
					if (!shares) {
						next = e;
						found++;
					}
				}
				if (found != 1) {
					next = -1;
				}
			}
			if (next < 0 || visited[next]) {
				break;
			}
			visited[next] = 1;
			result.push_back(next);
			vertex = edges[next].x == vertex ? edges[next].y : edges[next].x;
			edge = next;
		}
	}
	return result;
}

// Edge ring: across quads to the opposite edge.
PackedInt32Array TetraEditMesh::get_edge_ring(int p_edge) const {
	PackedInt32Array result;
	ERR_FAIL_INDEX_V(p_edge, int(edges.size()), result);
	LocalVector<IntList> edge_faces;
	_build_edge_faces(edge_faces);
	Mask visited;
	visited.resize_initialized(edges.size());
	visited[p_edge] = 1;
	result.push_back(p_edge);
	IntList stack;
	stack.push_back(p_edge);
	while (!stack.is_empty()) {
		int edge = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int f : edge_faces[edge]) {
			const IntList &loop = faces[f].verts;
			if (loop.size() != 4) {
				continue;
			}
			for (int i = 0; i < 4; i++) {
				if (_find_edge(loop[i], loop[(i + 1) % 4]) == edge) {
					int opposite = _find_edge(loop[(i + 2) % 4], loop[(i + 3) % 4]);
					if (opposite >= 0 && !visited[opposite]) {
						visited[opposite] = 1;
						result.push_back(opposite);
						stack.push_back(opposite);
					}
				}
			}
		}
	}
	return result;
}

void TetraEditMesh::select_edge_loop(int p_edge, bool p_extend) {
	PackedInt32Array loop = get_edge_loop(p_edge);
	if (!p_extend) {
		select_none();
	}
	for (int e : loop) {
		if (select_mode == SELECT_EDGE) {
			esel[e] = 1;
		} else {
			vsel[edges[e].x] = 1;
			vsel[edges[e].y] = 1;
		}
	}
	if (select_mode == SELECT_FACE) {
		_select_only_vertices(Mask(vsel));
	} else {
		_flush_selection();
	}
	emit_changed();
}

void TetraEditMesh::select_edge_ring(int p_edge, bool p_extend) {
	PackedInt32Array ring = get_edge_ring(p_edge);
	if (!p_extend) {
		select_none();
	}
	for (int e : ring) {
		if (select_mode == SELECT_EDGE) {
			esel[e] = 1;
		} else {
			vsel[edges[e].x] = 1;
			vsel[edges[e].y] = 1;
		}
	}
	if (select_mode == SELECT_FACE) {
		_select_only_vertices(Mask(vsel));
	} else {
		_flush_selection();
	}
	emit_changed();
}

// ---------------------------------------------------------------------------
// Consistency.

String TetraEditMesh::validate() const {
	if (vsel.size() != verts.size() || esel.size() != edges.size() || fsel.size() != faces.size()) {
		return "selection arrays do not match the element counts";
	}
	HashSet<uint64_t> seen_edges;
	for (uint32_t e = 0; e < edges.size(); e++) {
		const Vector2i &edge = edges[e];
		if (edge.x < 0 || edge.y >= int(verts.size()) || edge.x >= edge.y) {
			return vformat("edge %d has invalid vertices (%d, %d)", e, edge.x, edge.y);
		}
		if (seen_edges.has(_edge_key(edge.x, edge.y))) {
			return vformat("edge %d is a duplicate", e);
		}
		seen_edges.insert(_edge_key(edge.x, edge.y));
	}
	HashSet<String> seen_faces;
	for (uint32_t f = 0; f < faces.size(); f++) {
		const Face &face = faces[f];
		if (face.verts.size() < 3) {
			return vformat("face %d has fewer than 3 vertices", f);
		}
		if (face.uvs.size() != face.verts.size()) {
			return vformat("face %d has %d UVs for %d corners", f, face.uvs.size(), face.verts.size());
		}
		if (face.material < 0) {
			return vformat("face %d has a negative material index", f);
		}
		IntList sorted(face.verts);
		sorted.sort();
		String key;
		for (uint32_t i = 0; i < sorted.size(); i++) {
			if (sorted[i] < 0 || sorted[i] >= int(verts.size())) {
				return vformat("face %d uses vertex %d, which does not exist", f, sorted[i]);
			}
			if (i > 0 && sorted[i] == sorted[i - 1]) {
				return vformat("face %d uses vertex %d twice", f, sorted[i]);
			}
			key += itos(sorted[i]) + ",";
		}
		if (seen_faces.has(key)) {
			return vformat("face %d duplicates another face", f);
		}
		seen_faces.insert(key);
		for (uint32_t i = 0; i < face.verts.size(); i++) {
			if (!seen_edges.has(_edge_key(face.verts[i], face.verts[(i + 1) % face.verts.size()]))) {
				return vformat("face %d has no edge between vertices %d and %d", f, face.verts[i], face.verts[(i + 1) % face.verts.size()]);
			}
		}
	}
	for (uint32_t v = 0; v < verts.size(); v++) {
		if (!verts[v].is_finite()) {
			return vformat("vertex %d is not finite", v);
		}
	}
	// The selection must be what the select mode's flushing produces.
	TetraEditMesh *self = const_cast<TetraEditMesh *>(this);
	Mask old_v(vsel);
	Mask old_e(esel);
	Mask old_f(fsel);
	self->_flush_selection();
	bool consistent = true;
	for (uint32_t i = 0; i < verts.size(); i++) {
		consistent = consistent && bool(old_v[i]) == bool(vsel[i]);
	}
	for (uint32_t i = 0; i < edges.size(); i++) {
		consistent = consistent && bool(old_e[i]) == bool(esel[i]);
	}
	for (uint32_t i = 0; i < faces.size(); i++) {
		consistent = consistent && bool(old_f[i]) == bool(fsel[i]);
	}
	self->vsel = old_v;
	self->esel = old_e;
	self->fsel = old_f;
	if (!consistent) {
		return "the selection is not flushed for the select mode";
	}
	return String();
}

// ---------------------------------------------------------------------------
// Conversion and storage.

Ref<ArrayMesh> TetraEditMesh::to_array_mesh() const {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	// Area-weighted vertex normals for smooth faces.
	LocalVector<Vector3> vertex_normals;
	vertex_normals.resize_initialized(verts.size());
	LocalVector<Vector3> face_normals;
	face_normals.resize(faces.size());
	int material_count = 0;
	for (uint32_t f = 0; f < faces.size(); f++) {
		Vector3 raw = _face_normal_raw(faces[f]);
		face_normals[f] = raw.normalized();
		for (int v : faces[f].verts) {
			vertex_normals[v] += raw;
		}
		material_count = MAX(material_count, faces[f].material + 1);
	}

	for (int material = 0; material < material_count; material++) {
		Vector<Vector3> positions;
		Vector<Vector3> normals;
		Vector<Vector2> uvs;
		Vector<int> indices;
		HashMap<int, IntList> smooth_corners; // Vertex -> emitted indices.
		IntList corners;
		for (uint32_t f = 0; f < faces.size(); f++) {
			const Face &face = faces[f];
			if (face.material != material) {
				continue;
			}
			IntList emitted;
			emitted.resize(face.verts.size());
			for (uint32_t i = 0; i < face.verts.size(); i++) {
				int v = face.verts[i];
				int index = -1;
				if (face.smooth) {
					IntList &candidates = smooth_corners[v];
					for (int candidate : candidates) {
						if (uvs[candidate] == face.uvs[i]) {
							index = candidate;
							break;
						}
					}
					if (index < 0) {
						index = positions.size();
						candidates.push_back(index);
						positions.push_back(verts[v]);
						Vector3 normal = vertex_normals[v].normalized();
						normals.push_back(normal == Vector3() ? face_normals[f] : normal);
						uvs.push_back(face.uvs[i]);
					}
				} else {
					index = positions.size();
					positions.push_back(verts[v]);
					normals.push_back(face_normals[f]);
					uvs.push_back(face.uvs[i]);
				}
				emitted[i] = index;
			}
			corners.clear();
			_triangulate_face(face, corners);
			// Loops are counter-clockwise seen from outside; Godot's front
			// faces are clockwise, so each triangle is emitted reversed.
			for (uint32_t i = 0; i + 2 < corners.size(); i += 3) {
				indices.push_back(emitted[corners[i]]);
				indices.push_back(emitted[corners[i + 2]]);
				indices.push_back(emitted[corners[i + 1]]);
			}
		}
		if (indices.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = positions;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		if (material < materials.size()) {
			Ref<Material> surface_material = materials[material];
			if (surface_material.is_valid()) {
				mesh->surface_set_material(mesh->get_surface_count() - 1, surface_material);
			}
		}
	}
	return mesh;
}

void TetraEditMesh::from_mesh(const Ref<Mesh> &p_mesh, bool p_merge_quads, float p_weld_distance) {
	verts.clear();
	edges.clear();
	faces.clear();
	vsel.clear();
	esel.clear();
	fsel.clear();
	materials.clear();
	edge_map_dirty = true;
	if (p_mesh.is_null()) {
		_topology_changed();
		return;
	}
	HashMap<Vector3i, int> welded;
	HashMap<Vector3, int> exact;
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		materials.push_back(p_mesh->surface_get_material(surface));
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		Array arrays = p_mesh->surface_get_arrays(surface);
		Vector<Vector3> positions = arrays[Mesh::ARRAY_VERTEX];
		Vector<Vector3> normals = arrays[Mesh::ARRAY_NORMAL];
		Vector<Vector2> uvs = arrays[Mesh::ARRAY_TEX_UV];
		Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
		if (indices.is_empty()) {
			indices.resize(positions.size());
			for (int i = 0; i < positions.size(); i++) {
				indices.write[i] = i;
			}
		}
		IntList remap;
		remap.resize(positions.size());
		for (int i = 0; i < positions.size(); i++) {
			const Vector3 &p = positions[i];
			int *found = nullptr;
			Vector3i cell;
			if (p_weld_distance > 0) {
				cell = Vector3i(Math::round(p.x / p_weld_distance), Math::round(p.y / p_weld_distance), Math::round(p.z / p_weld_distance));
				found = welded.getptr(cell);
			} else {
				found = exact.getptr(p);
			}
			if (found) {
				remap[i] = *found;
			} else {
				remap[i] = _add_vertex(p);
				if (p_weld_distance > 0) {
					welded[cell] = remap[i];
				} else {
					exact[p] = remap[i];
				}
			}
		}
		for (int i = 0; i + 2 < indices.size(); i += 3) {
			// Godot triangles are clockwise; loops are stored counter-clockwise.
			int corner[3] = { indices[i], indices[i + 2], indices[i + 1] };
			IntList loop;
			LocalVector<Vector2> loop_uvs;
			for (int c = 0; c < 3; c++) {
				ERR_FAIL_INDEX(corner[c], positions.size());
				loop.push_back(remap[corner[c]]);
				loop_uvs.push_back(corner[c] < uvs.size() ? uvs[corner[c]] : Vector2());
			}
			if (loop[0] == loop[1] || loop[1] == loop[2] || loop[0] == loop[2] || _has_face_with_verts(loop)) {
				continue;
			}
			int f = _add_face(loop, loop_uvs, surface, false);
			if (normals.size() == positions.size()) {
				Vector3 normal = _face_normal_raw(faces[f]).normalized();
				for (int c = 0; c < 3; c++) {
					if (normals[corner[c]].dot(normal) < 0.999) {
						faces[f].smooth = true;
					}
				}
			}
		}
	}
	if (p_merge_quads) {
		Mask all;
		all.resize(faces.size());
		for (uint32_t f = 0; f < faces.size(); f++) {
			all[f] = 1;
		}
		_join_triangles(all, 0.01, Math::PI);
	}
	_flush_selection();
	_topology_changed();
}

PackedByteArray TetraEditMesh::get_state() const {
	ByteWriter writer;
	writer.u32(TETRA_EDIT_MESH_MAGIC);
	writer.u32(verts.size());
	writer.u32(edges.size());
	writer.u32(faces.size());
	writer.u32(select_mode);
	for (const Vector3 &v : verts) {
		writer.f32(v.x);
		writer.f32(v.y);
		writer.f32(v.z);
	}
	for (const Vector2i &e : edges) {
		writer.u32(e.x);
		writer.u32(e.y);
	}
	for (const Face &face : faces) {
		writer.u32(face.verts.size());
		writer.u32(face.material);
		writer.u32(face.smooth ? 1 : 0);
		for (int v : face.verts) {
			writer.u32(v);
		}
		for (const Vector2 &uv : face.uvs) {
			writer.f32(uv.x);
			writer.f32(uv.y);
		}
	}
	writer.raw(vsel.ptr(), vsel.size());
	writer.raw(esel.ptr(), esel.size());
	writer.raw(fsel.ptr(), fsel.size());
	return writer.bytes;
}

bool TetraEditMesh::set_state(const PackedByteArray &p_state) {
	if (p_state.is_empty()) {
		clear();
		return true;
	}
	ByteReader reader;
	reader.data = p_state.ptr();
	reader.size = p_state.size();
	ERR_FAIL_COND_V_MSG(reader.u32() != TETRA_EDIT_MESH_MAGIC, false, "Not TetraEditMesh data.");
	uint32_t vertex_count = reader.u32();
	uint32_t edge_count = reader.u32();
	uint32_t face_count = reader.u32();
	uint32_t mode = reader.u32();
	ERR_FAIL_COND_V(reader.failed || mode > SELECT_FACE, false);
	ERR_FAIL_COND_V(!reader.need(int64_t(vertex_count) * 12 + int64_t(edge_count) * 8), false);
	LocalVector<Vector3> new_verts;
	LocalVector<Vector2i> new_edges;
	LocalVector<Face> new_faces;
	new_verts.resize(vertex_count);
	for (uint32_t i = 0; i < vertex_count; i++) {
		new_verts[i].x = reader.f32();
		new_verts[i].y = reader.f32();
		new_verts[i].z = reader.f32();
	}
	new_edges.resize(edge_count);
	for (uint32_t i = 0; i < edge_count; i++) {
		new_edges[i].x = reader.u32();
		new_edges[i].y = reader.u32();
		ERR_FAIL_COND_V(new_edges[i].x < 0 || new_edges[i].x >= new_edges[i].y || uint32_t(new_edges[i].y) >= vertex_count, false);
	}
	for (uint32_t i = 0; i < face_count; i++) {
		Face face;
		uint32_t corners = reader.u32();
		face.material = reader.u32();
		face.smooth = reader.u32() != 0;
		ERR_FAIL_COND_V(reader.failed || corners < 3 || face.material < 0 || !reader.need(int64_t(corners) * 12), false);
		face.verts.resize(corners);
		face.uvs.resize(corners);
		for (uint32_t c = 0; c < corners; c++) {
			face.verts[c] = reader.u32();
			ERR_FAIL_COND_V(uint32_t(face.verts[c]) >= vertex_count, false);
		}
		for (uint32_t c = 0; c < corners; c++) {
			face.uvs[c].x = reader.f32();
			face.uvs[c].y = reader.f32();
		}
		new_faces.push_back(face);
	}
	ERR_FAIL_COND_V(!reader.need(int64_t(vertex_count) + edge_count + face_count), false);
	verts = new_verts;
	edges = new_edges;
	faces = new_faces;
	vsel.resize(vertex_count);
	esel.resize(edge_count);
	fsel.resize(face_count);
	const uint8_t *selection = reader.data + reader.at;
	for (uint32_t i = 0; i < vertex_count; i++) {
		vsel[i] = selection[i] != 0;
	}
	for (uint32_t i = 0; i < edge_count; i++) {
		esel[i] = selection[vertex_count + i] != 0;
	}
	for (uint32_t i = 0; i < face_count; i++) {
		fsel[i] = selection[vertex_count + edge_count + i] != 0;
	}
	select_mode = SelectMode(mode);
	edge_map_dirty = true;
	// Stored data may predate an edge the faces need.
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (uint32_t i = 0; i < faces[f].verts.size(); i++) {
			_ensure_edge(faces[f].verts[i], faces[f].verts[(i + 1) % faces[f].verts.size()]);
		}
	}
	_flush_selection();
	_topology_changed();
	return true;
}

TetraEditMesh::TetraEditMesh() {
}

void TetraEditMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &TetraEditMesh::clear);
	ClassDB::bind_method(D_METHOD("get_vertex_count"), &TetraEditMesh::get_vertex_count);
	ClassDB::bind_method(D_METHOD("get_edge_count"), &TetraEditMesh::get_edge_count);
	ClassDB::bind_method(D_METHOD("get_face_count"), &TetraEditMesh::get_face_count);
	ClassDB::bind_method(D_METHOD("add_vertex", "position"), &TetraEditMesh::add_vertex);
	ClassDB::bind_method(D_METHOD("add_edge", "a", "b"), &TetraEditMesh::add_edge);
	ClassDB::bind_method(D_METHOD("add_face", "vertices", "uvs", "material", "smooth"), &TetraEditMesh::add_face, DEFVAL(PackedVector2Array()), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_vertex", "index"), &TetraEditMesh::get_vertex);
	ClassDB::bind_method(D_METHOD("set_vertex", "index", "position"), &TetraEditMesh::set_vertex);
	ClassDB::bind_method(D_METHOD("get_vertex_positions"), &TetraEditMesh::get_vertex_positions);
	ClassDB::bind_method(D_METHOD("set_vertex_positions", "positions"), &TetraEditMesh::set_vertex_positions);
	ClassDB::bind_method(D_METHOD("get_edge_vertices", "edge"), &TetraEditMesh::get_edge_vertices);
	ClassDB::bind_method(D_METHOD("find_edge", "a", "b"), &TetraEditMesh::find_edge);
	ClassDB::bind_method(D_METHOD("get_edge_faces", "edge"), &TetraEditMesh::get_edge_faces);
	ClassDB::bind_method(D_METHOD("get_face_vertices", "face"), &TetraEditMesh::get_face_vertices);
	ClassDB::bind_method(D_METHOD("get_face_uvs", "face"), &TetraEditMesh::get_face_uvs);
	ClassDB::bind_method(D_METHOD("set_face_uvs", "face", "uvs"), &TetraEditMesh::set_face_uvs);
	ClassDB::bind_method(D_METHOD("get_face_material", "face"), &TetraEditMesh::get_face_material);
	ClassDB::bind_method(D_METHOD("set_face_material", "face", "material"), &TetraEditMesh::set_face_material);
	ClassDB::bind_method(D_METHOD("is_face_smooth", "face"), &TetraEditMesh::is_face_smooth);
	ClassDB::bind_method(D_METHOD("set_face_smooth", "face", "smooth"), &TetraEditMesh::set_face_smooth);
	ClassDB::bind_method(D_METHOD("get_face_normal", "face"), &TetraEditMesh::get_face_normal);
	ClassDB::bind_method(D_METHOD("get_face_center", "face"), &TetraEditMesh::get_face_center);
	ClassDB::bind_method(D_METHOD("find_face", "vertices"), &TetraEditMesh::find_face);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &TetraEditMesh::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &TetraEditMesh::get_materials);

	ClassDB::bind_method(D_METHOD("set_select_mode", "mode"), &TetraEditMesh::set_select_mode);
	ClassDB::bind_method(D_METHOD("get_select_mode"), &TetraEditMesh::get_select_mode);
	ClassDB::bind_method(D_METHOD("select_vertex", "index", "selected"), &TetraEditMesh::select_vertex, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_edge", "index", "selected"), &TetraEditMesh::select_edge, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_face", "index", "selected"), &TetraEditMesh::select_face, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("is_vertex_selected", "index"), &TetraEditMesh::is_vertex_selected);
	ClassDB::bind_method(D_METHOD("is_edge_selected", "index"), &TetraEditMesh::is_edge_selected);
	ClassDB::bind_method(D_METHOD("is_face_selected", "index"), &TetraEditMesh::is_face_selected);
	ClassDB::bind_method(D_METHOD("get_selected_vertices"), &TetraEditMesh::get_selected_vertices);
	ClassDB::bind_method(D_METHOD("get_selected_edges"), &TetraEditMesh::get_selected_edges);
	ClassDB::bind_method(D_METHOD("get_selected_faces"), &TetraEditMesh::get_selected_faces);
	ClassDB::bind_method(D_METHOD("select_all"), &TetraEditMesh::select_all);
	ClassDB::bind_method(D_METHOD("select_none"), &TetraEditMesh::select_none);
	ClassDB::bind_method(D_METHOD("select_invert"), &TetraEditMesh::select_invert);
	ClassDB::bind_method(D_METHOD("select_linked"), &TetraEditMesh::select_linked);
	ClassDB::bind_method(D_METHOD("select_linked_from_vertex", "vertex", "selected"), &TetraEditMesh::select_linked_from_vertex, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_more"), &TetraEditMesh::select_more);
	ClassDB::bind_method(D_METHOD("select_less"), &TetraEditMesh::select_less);
	ClassDB::bind_method(D_METHOD("get_edge_loop", "edge"), &TetraEditMesh::get_edge_loop);
	ClassDB::bind_method(D_METHOD("get_edge_ring", "edge"), &TetraEditMesh::get_edge_ring);
	ClassDB::bind_method(D_METHOD("select_edge_loop", "edge", "extend"), &TetraEditMesh::select_edge_loop, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("select_edge_ring", "edge", "extend"), &TetraEditMesh::select_edge_ring, DEFVAL(false));

	ClassDB::bind_method(D_METHOD("validate"), &TetraEditMesh::validate);
	ClassDB::bind_method(D_METHOD("to_array_mesh"), &TetraEditMesh::to_array_mesh);
	ClassDB::bind_method(D_METHOD("from_mesh", "mesh", "merge_quads", "weld_distance"), &TetraEditMesh::from_mesh, DEFVAL(false), DEFVAL(0.00001));
	ClassDB::bind_method(D_METHOD("get_state"), &TetraEditMesh::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &TetraEditMesh::set_state);
	ClassDB::bind_method(D_METHOD("set_data", "data"), &TetraEditMesh::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &TetraEditMesh::get_data);

	ClassDB::bind_method(D_METHOD("add_plane", "size"), &TetraEditMesh::add_plane, DEFVAL(2.0));
	ClassDB::bind_method(D_METHOD("add_cube", "size"), &TetraEditMesh::add_cube, DEFVAL(2.0));
	ClassDB::bind_method(D_METHOD("add_grid", "x_subdivisions", "y_subdivisions", "size"), &TetraEditMesh::add_grid, DEFVAL(10), DEFVAL(10), DEFVAL(2.0));
	ClassDB::bind_method(D_METHOD("add_circle", "vertices", "radius", "fill"), &TetraEditMesh::add_circle, DEFVAL(32), DEFVAL(1.0), DEFVAL(FILL_NOTHING));
	ClassDB::bind_method(D_METHOD("add_cone", "vertices", "radius1", "radius2", "depth", "fill"), &TetraEditMesh::add_cone, DEFVAL(32), DEFVAL(1.0), DEFVAL(0.0), DEFVAL(2.0), DEFVAL(FILL_NGON));
	ClassDB::bind_method(D_METHOD("add_cylinder", "vertices", "radius", "depth", "fill"), &TetraEditMesh::add_cylinder, DEFVAL(32), DEFVAL(1.0), DEFVAL(2.0), DEFVAL(FILL_NGON));
	ClassDB::bind_method(D_METHOD("add_uv_sphere", "segments", "rings", "radius"), &TetraEditMesh::add_uv_sphere, DEFVAL(32), DEFVAL(16), DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("add_ico_sphere", "subdivisions", "radius"), &TetraEditMesh::add_ico_sphere, DEFVAL(2), DEFVAL(1.0));
	ClassDB::bind_static_method("TetraEditMesh", D_METHOD("get_primitive_operations"), &TetraEditMesh::get_primitive_operations);
	ClassDB::bind_method(D_METHOD("add_primitive", "id", "values"), &TetraEditMesh::add_primitive);

	ClassDB::bind_method(D_METHOD("get_selection_pivot", "mode"), &TetraEditMesh::get_selection_pivot, DEFVAL(PIVOT_MEDIAN));
	ClassDB::bind_method(D_METHOD("get_selection_normal"), &TetraEditMesh::get_selection_normal);
	ClassDB::bind_method(D_METHOD("transform_selection", "transform", "pivot", "proportional_radius"), &TetraEditMesh::transform_selection, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("translate_selection", "offset", "proportional_radius"), &TetraEditMesh::translate_selection, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("rotate_selection", "axis", "angle", "pivot", "proportional_radius"), &TetraEditMesh::rotate_selection, DEFVAL(PIVOT_MEDIAN), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("scale_selection", "scale", "pivot", "proportional_radius"), &TetraEditMesh::scale_selection, DEFVAL(PIVOT_MEDIAN), DEFVAL(0.0));

	ClassDB::bind_method(D_METHOD("extrude_region"), &TetraEditMesh::extrude_region);
	ClassDB::bind_method(D_METHOD("extrude_faces_individual", "offset"), &TetraEditMesh::extrude_faces_individual, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("inset_faces", "thickness", "depth"), &TetraEditMesh::inset_faces, DEFVAL(0.1), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("subdivide", "cuts"), &TetraEditMesh::subdivide, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("loop_cut", "edge", "cuts"), &TetraEditMesh::loop_cut, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("delete_selection", "mode"), &TetraEditMesh::delete_selection, DEFVAL(DELETE_VERTICES));
	ClassDB::bind_method(D_METHOD("dissolve_vertices"), &TetraEditMesh::dissolve_vertices);
	ClassDB::bind_method(D_METHOD("dissolve_edges"), &TetraEditMesh::dissolve_edges);
	ClassDB::bind_method(D_METHOD("dissolve_faces"), &TetraEditMesh::dissolve_faces);
	ClassDB::bind_method(D_METHOD("merge_at_center"), &TetraEditMesh::merge_at_center);
	ClassDB::bind_method(D_METHOD("merge_collapse"), &TetraEditMesh::merge_collapse);
	ClassDB::bind_method(D_METHOD("merge_by_distance", "distance"), &TetraEditMesh::merge_by_distance, DEFVAL(0.0001));
	ClassDB::bind_method(D_METHOD("fill"), &TetraEditMesh::fill);
	ClassDB::bind_method(D_METHOD("bridge_edge_loops"), &TetraEditMesh::bridge_edge_loops);
	ClassDB::bind_method(D_METHOD("flip_normals"), &TetraEditMesh::flip_normals);
	ClassDB::bind_method(D_METHOD("recalculate_normals", "inside"), &TetraEditMesh::recalculate_normals, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("shade_smooth", "smooth"), &TetraEditMesh::shade_smooth, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("duplicate_selection"), &TetraEditMesh::duplicate_selection);
	ClassDB::bind_method(D_METHOD("separate_selection"), &TetraEditMesh::separate_selection);
	ClassDB::bind_method(D_METHOD("triangulate"), &TetraEditMesh::triangulate);
	ClassDB::bind_method(D_METHOD("tris_to_quads", "face_angle", "shape_angle"), &TetraEditMesh::tris_to_quads, DEFVAL(0.698132), DEFVAL(0.698132));

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("Material")), "set_materials", "get_materials");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "select_mode", PROPERTY_HINT_ENUM, "Vertex,Edge,Face", PROPERTY_USAGE_NONE), "set_select_mode", "get_select_mode");

	BIND_ENUM_CONSTANT(SELECT_VERTEX);
	BIND_ENUM_CONSTANT(SELECT_EDGE);
	BIND_ENUM_CONSTANT(SELECT_FACE);
	BIND_ENUM_CONSTANT(PIVOT_MEDIAN);
	BIND_ENUM_CONSTANT(PIVOT_BOUNDING_BOX);
	BIND_ENUM_CONSTANT(DELETE_VERTICES);
	BIND_ENUM_CONSTANT(DELETE_EDGES);
	BIND_ENUM_CONSTANT(DELETE_FACES);
	BIND_ENUM_CONSTANT(DELETE_ONLY_FACES);
	BIND_ENUM_CONSTANT(DELETE_ONLY_EDGES_AND_FACES);
	BIND_ENUM_CONSTANT(FILL_NOTHING);
	BIND_ENUM_CONSTANT(FILL_NGON);
	BIND_ENUM_CONSTANT(FILL_TRIANGLE_FAN);
}
