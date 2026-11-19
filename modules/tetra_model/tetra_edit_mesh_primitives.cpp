// Primitive meshes with the parameters, topology and vertex layout of
// Blender's "Add Mesh" operators (source/blender/bmesh/operators/
// bmo_primitive.cc; copyright Blender Authors, GPL-2.0-or-later). The cube's
// vertex and face order and the icosahedron tables are Blender's.

#include "tetra_edit_mesh.h"

namespace {

typedef TetraEditMesh::IntList IntList;
typedef LocalVector<Vector2> UVList;

struct Builder {
	LocalVector<Vector3> verts;
	LocalVector<IntList> faces;
	LocalVector<UVList> uvs;
	LocalVector<Vector2i> edges;

	int vertex(real_t p_x, real_t p_y, real_t p_z) {
		verts.push_back(Vector3(p_x, p_y, p_z));
		return verts.size() - 1;
	}
	void face(const int *p_verts, const Vector2 *p_uvs, int p_count) {
		IntList loop;
		UVList loop_uvs;
		for (int i = 0; i < p_count; i++) {
			loop.push_back(p_verts[i]);
			loop_uvs.push_back(p_uvs ? p_uvs[i] : Vector2());
		}
		faces.push_back(IntList(loop));
		uvs.push_back(UVList(loop_uvs));
	}
	void tri(int p_a, int p_b, int p_c, Vector2 p_ua, Vector2 p_ub, Vector2 p_uc) {
		int v[3] = { p_a, p_b, p_c };
		Vector2 uv[3] = { p_ua, p_ub, p_uc };
		face(v, uv, 3);
	}
	void quad(int p_a, int p_b, int p_c, int p_d, Vector2 p_ua, Vector2 p_ub, Vector2 p_uc, Vector2 p_ud) {
		int v[4] = { p_a, p_b, p_c, p_d };
		Vector2 uv[4] = { p_ua, p_ub, p_uc, p_ud };
		face(v, uv, 4);
	}
};

const Vector2 UNIT_UVS[4] = { Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1) };

const float ICO_VERTS[12][3] = {
	{ 0.0f, 0.0f, -200.0f },
	{ 144.72f, -105.144f, -89.443f },
	{ -55.277f, -170.128f, -89.443f },
	{ -178.885f, 0.0f, -89.443f },
	{ -55.277f, 170.128f, -89.443f },
	{ 144.72f, 105.144f, -89.443f },
	{ 55.277f, -170.128f, 89.443f },
	{ -144.72f, -105.144f, 89.443f },
	{ -144.72f, 105.144f, 89.443f },
	{ 55.277f, 170.128f, 89.443f },
	{ 178.885f, 0.0f, 89.443f },
	{ 0.0f, 0.0f, 200.0f },
};

const int ICO_FACES[20][3] = {
	{ 0, 1, 2 }, { 1, 0, 5 }, { 0, 2, 3 }, { 0, 3, 4 }, { 0, 4, 5 }, { 1, 5, 10 }, { 2, 1, 6 },
	{ 3, 2, 7 }, { 4, 3, 8 }, { 5, 4, 9 }, { 1, 10, 6 }, { 2, 6, 7 }, { 3, 7, 8 }, { 4, 8, 9 },
	{ 5, 9, 10 }, { 6, 10, 11 }, { 7, 6, 11 }, { 8, 7, 11 }, { 9, 8, 11 }, { 10, 9, 11 },
};

Vector2 disc_uv(const Vector3 &p_position, real_t p_radius) {
	if (p_radius <= 0) {
		return Vector2(0.5, 0.5);
	}
	return Vector2(0.5 + 0.5 * p_position.x / p_radius, 0.5 + 0.5 * p_position.y / p_radius);
}

// A cap over the ring p_ring (counter-clockwise seen from +Z).
void add_cap(Builder &r_builder, const IntList &p_ring, TetraEditMesh::FillType p_fill, bool p_up, real_t p_radius, real_t p_z) {
	const int count = p_ring.size();
	if (p_fill == TetraEditMesh::FILL_NGON) {
		IntList loop;
		UVList loop_uvs;
		for (int i = 0; i < count; i++) {
			int v = p_ring[p_up ? i : count - 1 - i];
			loop.push_back(v);
			loop_uvs.push_back(disc_uv(r_builder.verts[v], p_radius));
		}
		r_builder.faces.push_back(IntList(loop));
		r_builder.uvs.push_back(UVList(loop_uvs));
	} else if (p_fill == TetraEditMesh::FILL_TRIANGLE_FAN) {
		int center = r_builder.vertex(0, 0, p_z);
		for (int i = 0; i < count; i++) {
			int a = p_ring[i];
			int b = p_ring[(i + 1) % count];
			if (!p_up) {
				SWAP(a, b);
			}
			r_builder.tri(center, a, b, Vector2(0.5, 0.5), disc_uv(r_builder.verts[a], p_radius), disc_uv(r_builder.verts[b], p_radius));
		}
	}
}

Dictionary make_param(const String &p_name, const String &p_label, const String &p_type, const Variant &p_default, double p_min, double p_max, double p_ui_max = 0) {
	Dictionary param;
	param["name"] = p_name;
	param["label"] = p_label;
	param["type"] = p_type;
	param["default"] = p_default;
	param["min"] = p_min;
	param["max"] = p_max;
	if (p_ui_max > 0) {
		param["ui_max"] = p_ui_max;
	}
	if (p_type == "int") {
		param["step"] = 1.0;
	}
	return param;
}

Dictionary make_fill_param(int p_default) {
	Dictionary param;
	param["name"] = "fill";
	param["label"] = "Cap Fill Type";
	param["type"] = "enum";
	param["default"] = p_default;
	Array options;
	const char *labels[3] = { "Nothing", "N-Gon", "Triangle Fan" };
	for (int i = 0; i < 3; i++) {
		Dictionary option;
		option["value"] = i;
		option["label"] = labels[i];
		options.push_back(option);
	}
	param["options"] = options;
	return param;
}

Dictionary make_operation(const String &p_id, const String &p_label, const Array &p_params) {
	Dictionary operation;
	operation["id"] = p_id;
	operation["label"] = p_label;
	operation["category"] = "Mesh";
	operation["source"] = "native";
	operation["params"] = p_params;
	return operation;
}

} // namespace

// Appends geometry given in Blender's Z-up coordinates, rotated to Y-up
// (x, y, z) -> (x, z, -y), and selects it.
void TetraEditMesh::_append_blender(const LocalVector<Vector3> &p_verts, const LocalVector<IntList> &p_faces, const LocalVector<LocalVector<Vector2>> &p_uvs, const LocalVector<Vector2i> &p_edges) {
	const int base = verts.size();
	for (const Vector3 &v : p_verts) {
		_add_vertex(Vector3(v.x, v.z, -v.y));
	}
	for (const Vector2i &edge : p_edges) {
		_ensure_edge(base + edge.x, base + edge.y);
	}
	for (uint32_t f = 0; f < p_faces.size(); f++) {
		IntList loop;
		for (int v : p_faces[f]) {
			loop.push_back(base + v);
		}
		_add_face(loop, p_uvs[f], 0, false);
	}
	Mask mask;
	mask.resize_initialized(verts.size());
	for (uint32_t v = base; v < verts.size(); v++) {
		mask[v] = 1;
	}
	_select_only_vertices(mask);
	_topology_changed();
}

void TetraEditMesh::add_plane(float p_size) {
	Builder builder;
	real_t h = p_size * 0.5;
	builder.vertex(-h, -h, 0);
	builder.vertex(h, -h, 0);
	builder.vertex(-h, h, 0);
	builder.vertex(h, h, 0);
	builder.quad(0, 1, 3, 2, UNIT_UVS[0], UNIT_UVS[1], UNIT_UVS[2], UNIT_UVS[3]);
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_cube(float p_size) {
	Builder builder;
	real_t h = p_size * 0.5;
	for (int x = -1; x <= 1; x += 2) {
		for (int y = -1; y <= 1; y += 2) {
			for (int z = -1; z <= 1; z += 2) {
				builder.vertex(x * h, y * h, z * h);
			}
		}
	}
	static const int cube_faces[6][4] = {
		{ 0, 1, 3, 2 },
		{ 2, 3, 7, 6 },
		{ 6, 7, 5, 4 },
		{ 4, 5, 1, 0 },
		{ 2, 6, 4, 0 },
		{ 7, 3, 1, 5 },
	};
	for (int f = 0; f < 6; f++) {
		builder.face(cube_faces[f], UNIT_UVS, 4);
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_grid(int p_x_subdivisions, int p_y_subdivisions, float p_size) {
	ERR_FAIL_COND(p_x_subdivisions < 1 || p_y_subdivisions < 1);
	Builder builder;
	const int xtot = p_x_subdivisions + 1;
	const int ytot = p_y_subdivisions + 1;
	real_t h = p_size * 0.5;
	for (int y = 0; y < ytot; y++) {
		for (int x = 0; x < xtot; x++) {
			builder.vertex(-h + p_size * x / p_x_subdivisions, -h + p_size * y / p_y_subdivisions, 0);
		}
	}
	auto uv = [&](int p_x, int p_y) -> Vector2 {
		return Vector2(real_t(p_x) / p_x_subdivisions, real_t(p_y) / p_y_subdivisions);
	};
	for (int y = 0; y < ytot - 1; y++) {
		for (int x = 0; x < xtot - 1; x++) {
			builder.quad(x + y * xtot, x + 1 + y * xtot, x + 1 + (y + 1) * xtot, x + (y + 1) * xtot, uv(x, y), uv(x + 1, y), uv(x + 1, y + 1), uv(x, y + 1));
		}
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_circle(int p_vertices, float p_radius, FillType p_fill) {
	ERR_FAIL_COND(p_vertices < 3);
	Builder builder;
	IntList ring;
	for (int i = 0; i < p_vertices; i++) {
		real_t phi = Math::TAU * i / p_vertices;
		ring.push_back(builder.vertex(-p_radius * Math::sin(phi), p_radius * Math::cos(phi), 0));
	}
	if (p_fill == FILL_NOTHING) {
		for (int i = 0; i < p_vertices; i++) {
			builder.edges.push_back(Vector2i(ring[i], ring[(i + 1) % p_vertices]));
		}
	} else {
		add_cap(builder, ring, p_fill, true, p_radius, 0);
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_cone(int p_vertices, float p_radius1, float p_radius2, float p_depth, FillType p_fill) {
	ERR_FAIL_COND(p_vertices < 3);
	ERR_FAIL_COND(p_radius1 <= 0 && p_radius2 <= 0);
	Builder builder;
	real_t h = p_depth * 0.5;
	IntList bottom;
	IntList top;
	// Blender creates the bottom and top vertex of each segment together.
	for (int i = 0; i < p_vertices; i++) {
		real_t phi = Math::TAU * i / p_vertices;
		if (p_radius1 > 0) {
			bottom.push_back(builder.vertex(-p_radius1 * Math::sin(phi), p_radius1 * Math::cos(phi), -h));
		}
		if (p_radius2 > 0) {
			top.push_back(builder.vertex(-p_radius2 * Math::sin(phi), p_radius2 * Math::cos(phi), h));
		}
	}
	int apex = -1;
	if (p_radius1 <= 0) {
		apex = builder.vertex(0, 0, -h);
	} else if (p_radius2 <= 0) {
		apex = builder.vertex(0, 0, h);
	}
	for (int i = 0; i < p_vertices; i++) {
		int j = (i + 1) % p_vertices;
		real_t u0 = real_t(i) / p_vertices;
		real_t u1 = real_t(i + 1) / p_vertices;
		if (p_radius1 > 0 && p_radius2 > 0) {
			builder.quad(bottom[i], bottom[j], top[j], top[i], Vector2(u0, 0), Vector2(u1, 0), Vector2(u1, 1), Vector2(u0, 1));
		} else if (p_radius2 <= 0) {
			builder.tri(bottom[i], bottom[j], apex, Vector2(u0, 0), Vector2(u1, 0), Vector2((u0 + u1) * 0.5, 1));
		} else {
			builder.tri(apex, top[j], top[i], Vector2((u0 + u1) * 0.5, 0), Vector2(u1, 1), Vector2(u0, 1));
		}
	}
	if (p_radius1 > 0) {
		add_cap(builder, bottom, p_fill, false, p_radius1, -h);
	}
	if (p_radius2 > 0) {
		add_cap(builder, top, p_fill, true, p_radius2, h);
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_cylinder(int p_vertices, float p_radius, float p_depth, FillType p_fill) {
	ERR_FAIL_COND(p_radius <= 0);
	add_cone(p_vertices, p_radius, p_radius, p_depth, p_fill);
}

void TetraEditMesh::add_uv_sphere(int p_segments, int p_rings, float p_radius) {
	ERR_FAIL_COND(p_segments < 3 || p_rings < 2);
	Builder builder;
	int top = builder.vertex(0, 0, p_radius);
	for (int r = 1; r < p_rings; r++) {
		real_t theta = Math::PI * r / p_rings;
		for (int s = 0; s < p_segments; s++) {
			real_t phi = Math::TAU * s / p_segments;
			builder.vertex(p_radius * Math::sin(theta) * Math::cos(phi), p_radius * Math::sin(theta) * Math::sin(phi), p_radius * Math::cos(theta));
		}
	}
	int bottom = builder.vertex(0, 0, -p_radius);
	auto ring_vertex = [&](int p_ring, int p_segment) -> int {
		return 1 + (p_ring - 1) * p_segments + p_segment % p_segments;
	};
	auto uv = [&](real_t p_segment, int p_ring) -> Vector2 {
		return Vector2(p_segment / p_segments, 1.0 - real_t(p_ring) / p_rings);
	};
	for (int s = 0; s < p_segments; s++) {
		builder.tri(top, ring_vertex(1, s), ring_vertex(1, s + 1), uv(s + 0.5, 0), uv(s, 1), uv(s + 1, 1));
	}
	for (int r = 1; r < p_rings - 1; r++) {
		for (int s = 0; s < p_segments; s++) {
			builder.quad(ring_vertex(r, s), ring_vertex(r + 1, s), ring_vertex(r + 1, s + 1), ring_vertex(r, s + 1), uv(s, r), uv(s, r + 1), uv(s + 1, r + 1), uv(s + 1, r));
		}
	}
	for (int s = 0; s < p_segments; s++) {
		builder.tri(bottom, ring_vertex(p_rings - 1, s + 1), ring_vertex(p_rings - 1, s), uv(s + 0.5, p_rings), uv(s + 1, p_rings - 1), uv(s, p_rings - 1));
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

void TetraEditMesh::add_ico_sphere(int p_subdivisions, float p_radius) {
	ERR_FAIL_COND(p_subdivisions < 1 || p_subdivisions > 8);
	LocalVector<Vector3> points;
	for (int i = 0; i < 12; i++) {
		points.push_back(Vector3(ICO_VERTS[i][0], ICO_VERTS[i][1], ICO_VERTS[i][2]).normalized() * p_radius);
	}
	LocalVector<Vector3i> triangles;
	for (int i = 0; i < 20; i++) {
		triangles.push_back(Vector3i(ICO_FACES[i][0], ICO_FACES[i][1], ICO_FACES[i][2]));
	}
	for (int level = 1; level < p_subdivisions; level++) {
		HashMap<uint64_t, int> midpoints;
		auto midpoint = [&](int p_a, int p_b) -> int {
			uint64_t key = _edge_key(p_a, p_b);
			int *found = midpoints.getptr(key);
			if (found) {
				return *found;
			}
			points.push_back(((points[p_a] + points[p_b]) * 0.5).normalized() * p_radius);
			midpoints[key] = points.size() - 1;
			return points.size() - 1;
		};
		LocalVector<Vector3i> next;
		for (const Vector3i &t : triangles) {
			int ab = midpoint(t.x, t.y);
			int bc = midpoint(t.y, t.z);
			int ca = midpoint(t.z, t.x);
			next.push_back(Vector3i(t.x, ab, ca));
			next.push_back(Vector3i(ab, t.y, bc));
			next.push_back(Vector3i(ca, bc, t.z));
			next.push_back(Vector3i(ab, bc, ca));
		}
		triangles = next;
	}
	Builder builder;
	builder.verts = points;
	auto uv = [&](int p_vertex) -> Vector2 {
		Vector3 n = points[p_vertex].normalized();
		return Vector2(0.5 + Math::atan2(n.y, n.x) / Math::TAU, 0.5 + Math::asin(CLAMP(n.z, -1.0, 1.0)) / Math::PI);
	};
	for (const Vector3i &t : triangles) {
		builder.tri(t.x, t.y, t.z, uv(t.x), uv(t.y), uv(t.z));
	}
	_append_blender(builder.verts, builder.faces, builder.uvs, builder.edges);
}

Array TetraEditMesh::get_primitive_operations() {
	Array operations;
	{
		Array params;
		params.push_back(make_param("size", "Size", "float", 2.0, 0.0, 10000.0, 10.0));
		operations.push_back(make_operation("plane", "Plane", params));
		operations.push_back(make_operation("cube", "Cube", params.duplicate(true)));
	}
	{
		Array params;
		params.push_back(make_param("x_subdivisions", "X Subdivisions", "int", 10, 1, 1000, 64));
		params.push_back(make_param("y_subdivisions", "Y Subdivisions", "int", 10, 1, 1000, 64));
		params.push_back(make_param("size", "Size", "float", 2.0, 0.0, 10000.0, 10.0));
		operations.push_back(make_operation("grid", "Grid", params));
	}
	{
		Array params;
		params.push_back(make_param("vertices", "Vertices", "int", 32, 3, 10000, 128));
		params.push_back(make_param("radius", "Radius", "float", 1.0, 0.0, 10000.0, 10.0));
		params.push_back(make_fill_param(FILL_NOTHING));
		operations.push_back(make_operation("circle", "Circle", params));
	}
	{
		Array params;
		params.push_back(make_param("vertices", "Vertices", "int", 32, 3, 10000, 128));
		params.push_back(make_param("radius", "Radius", "float", 1.0, 0.001, 10000.0, 10.0));
		params.push_back(make_param("depth", "Depth", "float", 2.0, 0.0, 10000.0, 10.0));
		params.push_back(make_fill_param(FILL_NGON));
		operations.push_back(make_operation("cylinder", "Cylinder", params));
	}
	{
		Array params;
		params.push_back(make_param("vertices", "Vertices", "int", 32, 3, 10000, 128));
		params.push_back(make_param("radius1", "Radius 1", "float", 1.0, 0.0, 10000.0, 10.0));
		params.push_back(make_param("radius2", "Radius 2", "float", 0.0, 0.0, 10000.0, 10.0));
		params.push_back(make_param("depth", "Depth", "float", 2.0, 0.0, 10000.0, 10.0));
		params.push_back(make_fill_param(FILL_NGON));
		operations.push_back(make_operation("cone", "Cone", params));
	}
	{
		Array params;
		params.push_back(make_param("segments", "Segments", "int", 32, 3, 10000, 128));
		params.push_back(make_param("rings", "Rings", "int", 16, 2, 10000, 64));
		params.push_back(make_param("radius", "Radius", "float", 1.0, 0.0, 10000.0, 10.0));
		operations.push_back(make_operation("uv_sphere", "UV Sphere", params));
	}
	{
		Array params;
		params.push_back(make_param("subdivisions", "Subdivisions", "int", 2, 1, 8));
		params.push_back(make_param("radius", "Radius", "float", 1.0, 0.0, 10000.0, 10.0));
		operations.push_back(make_operation("ico_sphere", "Ico Sphere", params));
	}
	return operations;
}

bool TetraEditMesh::add_primitive(const String &p_id, const Dictionary &p_values) {
	Dictionary values;
	Array operations = get_primitive_operations();
	bool known = false;
	for (int i = 0; i < operations.size(); i++) {
		Dictionary operation = operations[i];
		if (operation["id"] != p_id) {
			continue;
		}
		known = true;
		Array params = operation["params"];
		for (int j = 0; j < params.size(); j++) {
			Dictionary param = params[j];
			values[param["name"]] = p_values.get(param["name"], param["default"]);
		}
	}
	ERR_FAIL_COND_V_MSG(!known, false, vformat("Unknown primitive '%s'.", p_id));
	if (p_id == "plane") {
		add_plane(values["size"]);
	} else if (p_id == "cube") {
		add_cube(values["size"]);
	} else if (p_id == "grid") {
		add_grid(values["x_subdivisions"], values["y_subdivisions"], values["size"]);
	} else if (p_id == "circle") {
		add_circle(values["vertices"], values["radius"], FillType(int(values["fill"])));
	} else if (p_id == "cylinder") {
		add_cylinder(values["vertices"], values["radius"], values["depth"], FillType(int(values["fill"])));
	} else if (p_id == "cone") {
		add_cone(values["vertices"], values["radius1"], values["radius2"], values["depth"], FillType(int(values["fill"])));
	} else if (p_id == "uv_sphere") {
		add_uv_sphere(values["segments"], values["rings"], values["radius"]);
	} else if (p_id == "ico_sphere") {
		add_ico_sphere(values["subdivisions"], values["radius"]);
	}
	return true;
}
