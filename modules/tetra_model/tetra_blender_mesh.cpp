#include "tetra_blender_mesh.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/marshalls.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"

namespace {

// A face corner as Godot needs it: corners that agree on all three become one vertex.
struct CornerKey {
	int32_t vertex = 0;
	uint32_t normal[3] = {};
	uint32_t uv[2] = {};

	bool operator==(const CornerKey &p_other) const {
		return memcmp(this, &p_other, sizeof(CornerKey)) == 0;
	}
};

struct CornerKeyHasher {
	static uint32_t hash(const CornerKey &p_key) {
		return hash_murmur3_buffer(&p_key, sizeof(CornerKey));
	}
};

struct PositionHasher {
	static uint32_t hash(const Vector3 &p_position) {
		uint32_t h = hash_murmur3_one_real(p_position.x);
		h = hash_murmur3_one_real(p_position.y, h);
		h = hash_murmur3_one_real(p_position.z, h);
		return hash_fmix32(h);
	}
};

struct SurfaceBuilder {
	HashMap<CornerKey, int32_t, CornerKeyHasher> lookup;
	LocalVector<Vector3> vertices;
	LocalVector<Vector3> normals;
	LocalVector<Vector2> uvs;
	LocalVector<int32_t> indices;
};

uint32_t float_bits(float p_value) {
	// Negative zero would split a vertex that compares equal.
	if (p_value == 0.0f) {
		p_value = 0.0f;
	}
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

void append_floats(Vector<uint8_t> &r_blob, const LocalVector<float> &p_values) {
	const int64_t start = r_blob.size();
	r_blob.resize(start + int64_t(p_values.size()) * 4);
	uint8_t *write = r_blob.ptrw() + start;
	for (uint32_t i = 0; i < p_values.size(); i++) {
		encode_float(p_values[i], write + i * 4);
	}
}

void append_ints(Vector<uint8_t> &r_blob, const LocalVector<int32_t> &p_values) {
	const int64_t start = r_blob.size();
	r_blob.resize(start + int64_t(p_values.size()) * 4);
	uint8_t *write = r_blob.ptrw() + start;
	for (uint32_t i = 0; i < p_values.size(); i++) {
		encode_uint32(uint32_t(p_values[i]), write + i * 4);
	}
}

String material_slot_name(const Ref<Mesh> &p_mesh, int p_surface) {
	const Ref<Material> material = p_mesh->surface_get_material(p_surface);
	if (material.is_valid()) {
		if (!material->get_name().is_empty()) {
			return material->get_name();
		}
		if (!material->get_path().is_empty() && !material->get_path().contains("::")) {
			return material->get_path().get_file().get_basename();
		}
	}
	return vformat("Surface%d", p_surface);
}

// Godot -> Blender for positions and directions.
constexpr Basis TO_BLENDER(Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0));

} // namespace

Vector3 TetraBlenderMesh::position_to_blender(const Vector3 &p_position) {
	return Vector3(p_position.x, -p_position.z, p_position.y);
}

Vector3 TetraBlenderMesh::position_from_blender(const Vector3 &p_position) {
	return Vector3(p_position.x, p_position.z, -p_position.y);
}

PackedFloat64Array TetraBlenderMesh::transform_to_matrix(const Transform3D &p_transform, bool p_convert_local_axes) {
	Basis basis = TO_BLENDER * p_transform.basis;
	if (p_convert_local_axes) {
		basis = basis * TO_BLENDER.inverse();
	}
	const Vector3 origin = position_to_blender(p_transform.origin);
	PackedFloat64Array matrix;
	matrix.resize(16);
	double *write = matrix.ptrw();
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			// Adding zero turns the negative zeros of the axis swap into plain ones.
			write[row * 4 + column] = basis.rows[row][column] + 0.0;
		}
		write[row * 4 + 3] = origin[row] + 0.0;
	}
	write[12] = 0.0;
	write[13] = 0.0;
	write[14] = 0.0;
	write[15] = 1.0;
	return matrix;
}

Transform3D TetraBlenderMesh::matrix_to_transform(const PackedFloat64Array &p_matrix, bool p_convert_local_axes) {
	ERR_FAIL_COND_V_MSG(p_matrix.size() != 16, Transform3D(), "A Blender matrix has 16 numbers.");
	const double *read = p_matrix.ptr();
	Basis blender_basis;
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			blender_basis.rows[row][column] = read[row * 4 + column];
		}
	}
	Basis basis = TO_BLENDER.inverse() * blender_basis;
	if (p_convert_local_axes) {
		basis = basis * TO_BLENDER;
	}
	return Transform3D(basis, position_from_blender(Vector3(read[3], read[7], read[11])));
}

Error TetraBlenderMesh::write_file(const Ref<Mesh> &p_mesh, const String &p_path) {
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_PARAMETER);

	HashMap<Vector3, int32_t, PositionHasher> welded;
	LocalVector<float> positions;
	LocalVector<int32_t> loop_start;
	LocalVector<int32_t> corner_vertices;
	LocalVector<int32_t> polygon_material;
	LocalVector<float> corner_uvs;
	LocalVector<float> corner_normals;
	bool any_uvs = false;
	bool all_normals = true;
	Array material_names;

	const int surface_count = p_mesh->get_surface_count();
	for (int surface = 0; surface < surface_count; surface++) {
		material_names.push_back(material_slot_name(p_mesh, surface));
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface);
		if (arrays.size() != Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY) {
			continue;
		}
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		PackedVector3Array normals;
		if (arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY) {
			normals = arrays[Mesh::ARRAY_NORMAL];
		}
		PackedVector2Array uvs;
		if (arrays[Mesh::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY) {
			uvs = arrays[Mesh::ARRAY_TEX_UV];
		}
		PackedInt32Array indices;
		if (arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
			indices = arrays[Mesh::ARRAY_INDEX];
		}
		const int vertex_count = vertices.size();
		const bool has_normals = normals.size() == vertex_count;
		const bool has_uvs = uvs.size() == vertex_count;
		const bool indexed = !indices.is_empty();
		const int64_t triangle_count = (indexed ? indices.size() : vertex_count) / 3;
		if (triangle_count == 0) {
			continue;
		}
		any_uvs = any_uvs || has_uvs;
		all_normals = all_normals && has_normals;

		// The surface's vertices, welded into the shared vertex list.
		LocalVector<int32_t> remap;
		remap.resize(vertex_count);
		for (int i = 0; i < vertex_count; i++) {
			const Vector3 &position = vertices[i];
			const HashMap<Vector3, int32_t, PositionHasher>::Iterator found = welded.find(position);
			if (found) {
				remap[i] = found->value;
				continue;
			}
			const int32_t index = int32_t(positions.size() / 3);
			welded.insert(position, index);
			remap[i] = index;
			const Vector3 converted = position_to_blender(position);
			positions.push_back(converted.x);
			positions.push_back(converted.y);
			positions.push_back(converted.z);
		}

		for (int64_t triangle = 0; triangle < triangle_count; triangle++) {
			// Reversed to turn Godot's clockwise front faces counter-clockwise.
			int source[3];
			for (int corner = 0; corner < 3; corner++) {
				const int64_t slot = triangle * 3 + (corner == 0 ? 0 : 3 - corner);
				source[corner] = indexed ? indices[slot] : int(slot);
			}
			if (source[0] < 0 || source[1] < 0 || source[2] < 0 || source[0] >= vertex_count || source[1] >= vertex_count || source[2] >= vertex_count) {
				continue;
			}
			const int32_t a = remap[source[0]];
			const int32_t b = remap[source[1]];
			const int32_t c = remap[source[2]];
			if (a == b || b == c || a == c) {
				// Welding collapsed the triangle; Blender rejects such faces.
				continue;
			}
			loop_start.push_back(int32_t(corner_vertices.size()));
			polygon_material.push_back(surface);
			corner_vertices.push_back(a);
			corner_vertices.push_back(b);
			corner_vertices.push_back(c);
			for (int corner = 0; corner < 3; corner++) {
				const Vector2 uv = has_uvs ? uvs[source[corner]] : Vector2();
				corner_uvs.push_back(uv.x);
				corner_uvs.push_back(1.0f - uv.y);
				const Vector3 normal = has_normals ? position_to_blender(normals[source[corner]]) : Vector3(0, 0, 1);
				corner_normals.push_back(normal.x);
				corner_normals.push_back(normal.y);
				corner_normals.push_back(normal.z);
			}
		}
	}

	const bool write_normals = all_normals && !loop_start.is_empty();
	Dictionary header;
	header["name"] = p_mesh->get_name();
	header["vertex_count"] = int64_t(positions.size() / 3);
	header["polygon_count"] = int64_t(loop_start.size());
	header["corner_count"] = int64_t(corner_vertices.size());
	header["triangle_count"] = 0;
	header["has_uvs"] = any_uvs;
	header["has_normals"] = write_normals;
	header["materials"] = material_names;
	CharString header_text = JSON::stringify(header, "", false).utf8();

	Vector<uint8_t> blob;
	const int64_t header_length = (header_text.length() + 3) / 4 * 4;
	blob.resize(12 + header_length);
	uint8_t *write = blob.ptrw();
	memcpy(write, "TMSH", 4);
	encode_uint32(FORMAT_VERSION, write + 4);
	encode_uint32(uint32_t(header_length), write + 8);
	memset(write + 12, ' ', header_length);
	memcpy(write + 12, header_text.get_data(), header_text.length());

	append_floats(blob, positions);
	append_ints(blob, loop_start);
	append_ints(blob, corner_vertices);
	append_ints(blob, polygon_material);
	if (any_uvs) {
		append_floats(blob, corner_uvs);
	}
	if (write_normals) {
		append_floats(blob, corner_normals);
	}

	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		return error == OK ? ERR_FILE_CANT_WRITE : error;
	}
	if (!file->store_buffer(blob.ptr(), blob.size())) {
		return ERR_FILE_CANT_WRITE;
	}
	file->close();
	return OK;
}

Array TetraBlenderMesh::read_surfaces(const String &p_path, Error *r_error, Dictionary *r_header) {
	Array surfaces;
	Error error = OK;
	if (r_error) {
		*r_error = OK;
	}
#define FAIL_READ(m_error) \
	if (r_error) { \
		*r_error = m_error; \
	} \
	return Array();

	const Vector<uint8_t> blob = FileAccess::get_file_as_bytes(p_path, &error);
	if (error != OK) {
		FAIL_READ(error);
	}
	const uint8_t *data = blob.ptr();
	const uint64_t size = blob.size();
	if (size < 12 || memcmp(data, "TMSH", 4) != 0 || decode_uint32(data + 4) != FORMAT_VERSION) {
		FAIL_READ(ERR_FILE_UNRECOGNIZED);
	}
	const uint64_t header_length = decode_uint32(data + 8);
	if (12 + header_length > size) {
		FAIL_READ(ERR_FILE_CORRUPT);
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(String::utf8((const char *)(data + 12), int(header_length))) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		FAIL_READ(ERR_FILE_CORRUPT);
	}
	const Dictionary header = json->get_data();
	if (r_header) {
		*r_header = header;
	}
	const int64_t vertex_count = int64_t(header.get("vertex_count", 0));
	const int64_t polygon_count = int64_t(header.get("polygon_count", 0));
	const int64_t corner_count = int64_t(header.get("corner_count", 0));
	int64_t triangle_count = int64_t(header.get("triangle_count", 0));
	const bool has_uvs = bool(header.get("has_uvs", false));
	const bool has_normals = bool(header.get("has_normals", false));
	const Array material_names = header.get("materials", Array());
	if (vertex_count < 0 || polygon_count < 0 || corner_count < 0 || triangle_count < 0) {
		FAIL_READ(ERR_FILE_CORRUPT);
	}

	uint64_t offset = 12 + header_length;
	bool truncated = false;
	auto take = [&](int64_t p_count) -> const uint8_t * {
		const uint8_t *block = data + offset;
		offset += uint64_t(p_count) * 4;
		truncated = truncated || offset > size;
		return block;
	};
	const uint8_t *positions = take(vertex_count * 3);
	const uint8_t *loop_start = take(polygon_count);
	const uint8_t *corner_vertices = take(corner_count);
	const uint8_t *polygon_material = take(polygon_count);
	const uint8_t *corner_uvs = has_uvs ? take(corner_count * 2) : nullptr;
	const uint8_t *corner_normals = has_normals ? take(corner_count * 3) : nullptr;
	const uint8_t *stored_triangles = take(triangle_count * 3);
	const uint8_t *stored_triangle_polygons = take(triangle_count);
	if (truncated) {
		FAIL_READ(ERR_FILE_CORRUPT);
	}
#undef FAIL_READ

	auto read_int = [](const uint8_t *p_block, int64_t p_index) -> int32_t {
		return int32_t(decode_uint32(p_block + p_index * 4));
	};

	// Triangles as corner indices plus the polygon each belongs to. Files
	// from Blender carry its triangulation; anything else is fanned.
	LocalVector<int32_t> triangles;
	LocalVector<int32_t> triangle_polygons;
	if (triangle_count > 0) {
		triangles.resize(triangle_count * 3);
		triangle_polygons.resize(triangle_count);
		for (int64_t i = 0; i < triangle_count * 3; i++) {
			triangles[i] = read_int(stored_triangles, i);
		}
		for (int64_t i = 0; i < triangle_count; i++) {
			triangle_polygons[i] = read_int(stored_triangle_polygons, i);
		}
	} else {
		for (int64_t polygon = 0; polygon < polygon_count; polygon++) {
			const int32_t first = read_int(loop_start, polygon);
			const int32_t end = polygon + 1 < polygon_count ? read_int(loop_start, polygon + 1) : int32_t(corner_count);
			for (int32_t corner = first + 1; corner + 1 < end; corner++) {
				triangles.push_back(first);
				triangles.push_back(corner);
				triangles.push_back(corner + 1);
				triangle_polygons.push_back(int32_t(polygon));
			}
		}
		triangle_count = triangle_polygons.size();
	}

	LocalVector<SurfaceBuilder> builders;
	for (int64_t triangle = 0; triangle < triangle_count; triangle++) {
		const int32_t polygon = triangle_polygons[triangle];
		if (polygon < 0 || polygon >= polygon_count) {
			continue;
		}
		const int32_t slot = MAX(read_int(polygon_material, polygon), 0);
		if (uint32_t(slot) >= builders.size()) {
			builders.resize(slot + 1);
		}
		SurfaceBuilder &builder = builders[slot];
		int32_t emitted[3] = { 0, 0, 0 };
		bool valid = true;
		for (int corner_slot = 0; corner_slot < 3 && valid; corner_slot++) {
			const int32_t corner = triangles[triangle * 3 + corner_slot];
			if (corner < 0 || corner >= corner_count) {
				valid = false;
				break;
			}
			CornerKey key;
			key.vertex = read_int(corner_vertices, corner);
			if (key.vertex < 0 || key.vertex >= vertex_count) {
				valid = false;
				break;
			}
			Vector3 normal;
			Vector2 uv;
			if (corner_normals) {
				normal = Vector3(decode_float(corner_normals + corner * 12), decode_float(corner_normals + corner * 12 + 4), decode_float(corner_normals + corner * 12 + 8));
				key.normal[0] = float_bits(normal.x);
				key.normal[1] = float_bits(normal.y);
				key.normal[2] = float_bits(normal.z);
			}
			if (corner_uvs) {
				uv = Vector2(decode_float(corner_uvs + corner * 8), decode_float(corner_uvs + corner * 8 + 4));
				key.uv[0] = float_bits(uv.x);
				key.uv[1] = float_bits(uv.y);
			}
			const HashMap<CornerKey, int32_t, CornerKeyHasher>::Iterator found = builder.lookup.find(key);
			if (found) {
				emitted[corner_slot] = found->value;
				continue;
			}
			const int32_t index = int32_t(builder.vertices.size());
			builder.lookup.insert(key, index);
			emitted[corner_slot] = index;
			const uint8_t *position = positions + int64_t(key.vertex) * 12;
			builder.vertices.push_back(position_from_blender(Vector3(decode_float(position), decode_float(position + 4), decode_float(position + 8))));
			if (corner_normals) {
				builder.normals.push_back(position_from_blender(normal));
			}
			if (corner_uvs) {
				builder.uvs.push_back(Vector2(uv.x, 1.0f - uv.y));
			}
		}
		if (!valid) {
			continue;
		}
		// Reversed to turn Blender's counter-clockwise front faces clockwise.
		builder.indices.push_back(emitted[0]);
		builder.indices.push_back(emitted[2]);
		builder.indices.push_back(emitted[1]);
	}

	for (uint32_t slot = 0; slot < builders.size(); slot++) {
		SurfaceBuilder &builder = builders[slot];
		if (builder.indices.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = Vector<Vector3>(builder.vertices);
		if (corner_normals) {
			arrays[Mesh::ARRAY_NORMAL] = Vector<Vector3>(builder.normals);
		}
		if (corner_uvs) {
			arrays[Mesh::ARRAY_TEX_UV] = Vector<Vector2>(builder.uvs);
		}
		arrays[Mesh::ARRAY_INDEX] = Vector<int32_t>(builder.indices);
		if (corner_normals && corner_uvs) {
			Ref<SurfaceTool> tool;
			tool.instantiate();
			tool->create_from_arrays(arrays);
			tool->generate_tangents();
			arrays = tool->commit_to_arrays();
		}
		Dictionary surface;
		surface["material_index"] = int64_t(slot);
		surface["material_name"] = int(slot) < material_names.size() ? String(material_names[slot]) : String();
		surface["arrays"] = arrays;
		surfaces.push_back(surface);
	}
	return surfaces;
}

Ref<ArrayMesh> TetraBlenderMesh::surfaces_to_mesh(const Array &p_surfaces, const Ref<Mesh> &p_material_source) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for (int i = 0; i < p_surfaces.size(); i++) {
		const Dictionary surface = p_surfaces[i];
		const Array arrays = surface.get("arrays", Array());
		if (arrays.size() != Mesh::ARRAY_MAX) {
			continue;
		}
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		const int index = mesh->get_surface_count() - 1;
		const String name = surface.get("material_name", String());
		if (!name.is_empty()) {
			mesh->surface_set_name(index, name);
		}
		const int slot = int(surface.get("material_index", -1));
		if (p_material_source.is_valid() && slot >= 0 && slot < p_material_source->get_surface_count()) {
			mesh->surface_set_material(index, p_material_source->surface_get_material(slot));
		}
	}
	return mesh;
}

Ref<ArrayMesh> TetraBlenderMesh::read_file(const String &p_path, const Ref<Mesh> &p_material_source) {
	Error error = OK;
	const Array surfaces = read_surfaces(p_path, &error);
	if (error != OK) {
		return Ref<ArrayMesh>();
	}
	return surfaces_to_mesh(surfaces, p_material_source);
}

Dictionary TetraBlenderMesh::read_header(const String &p_path) {
	Dictionary header;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return header;
	}
	uint8_t prefix[12];
	if (file->get_buffer(prefix, 12) != 12 || memcmp(prefix, "TMSH", 4) != 0) {
		return header;
	}
	const uint32_t header_length = decode_uint32(prefix + 8);
	if (header_length > file->get_length()) {
		return header;
	}
	Vector<uint8_t> text;
	text.resize(header_length);
	if (file->get_buffer(text.ptrw(), header_length) != header_length) {
		return header;
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(String::utf8((const char *)text.ptr(), int(header_length))) == OK && json->get_data().get_type() == Variant::DICTIONARY) {
		header = json->get_data();
	}
	return header;
}

static Array _read_surfaces_bind(const String &p_path) {
	return TetraBlenderMesh::read_surfaces(p_path);
}

void TetraBlenderMesh::_bind_methods() {
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("position_to_blender", "position"), &TetraBlenderMesh::position_to_blender);
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("position_from_blender", "position"), &TetraBlenderMesh::position_from_blender);
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("transform_to_matrix", "transform", "convert_local_axes"), &TetraBlenderMesh::transform_to_matrix, DEFVAL(true));
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("matrix_to_transform", "matrix", "convert_local_axes"), &TetraBlenderMesh::matrix_to_transform, DEFVAL(true));
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("write_file", "mesh", "path"), &TetraBlenderMesh::write_file);
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("read_surfaces", "path"), &_read_surfaces_bind);
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("surfaces_to_mesh", "surfaces", "material_source"), &TetraBlenderMesh::surfaces_to_mesh, DEFVAL(Ref<Mesh>()));
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("read_file", "path", "material_source"), &TetraBlenderMesh::read_file, DEFVAL(Ref<Mesh>()));
	ClassDB::bind_static_method("TetraBlenderMesh", D_METHOD("read_header", "path"), &TetraBlenderMesh::read_header);
}
