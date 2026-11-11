#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "scene/resources/mesh.h"

// Conversion between Godot meshes and the mesh exchange files of the Blender
// bridge (the ".tmsh" format documented at the top of
// bridge/tetra_blender_server.py), and between the two programs' conventions.
//
// The files and every transform in the bridge protocol are in Blender's
// conventions; this class is the one place where the conversion happens:
//
//   position / direction   Blender (x, y, z) = Godot (x, -z, y)   (Z up instead of Y up)
//   winding                Blender front faces are counter-clockwise, Godot's clockwise
//   UV                     Blender v = 1 - Godot v                 (origin bottom left)
//
// Cameras and lights look down their local -Z axis with +Y up in both
// programs, so only their placement in the world is converted, while a mesh
// object's local axes are converted together with its vertices.
//
// Everything is static. The functions that only read or produce plain data
// (write_file, read_surfaces, the conversions) may be called from any thread;
// surfaces_to_mesh and read_file create an ArrayMesh.
class TetraBlenderMesh : public Object {
	GDCLASS(TetraBlenderMesh, Object);

protected:
	static void _bind_methods();

public:
	static constexpr uint32_t FORMAT_VERSION = 1;

	static Vector3 position_to_blender(const Vector3 &p_position);
	static Vector3 position_from_blender(const Vector3 &p_position);

	// A Godot transform as the 16 numbers (row-major 4x4) the protocol uses.
	// p_convert_local_axes is true for objects whose data is converted too
	// (meshes) and false for cameras and lights.
	static PackedFloat64Array transform_to_matrix(const Transform3D &p_transform, bool p_convert_local_axes = true);
	static Transform3D matrix_to_transform(const PackedFloat64Array &p_matrix, bool p_convert_local_axes = true);

	// Writes every triangle surface of p_mesh. Vertices at the same position
	// are merged, across surfaces too, so that Blender receives connected
	// topology; normals and UVs are kept per face corner. Surface i becomes
	// material slot i.
	static Error write_file(const Ref<Mesh> &p_mesh, const String &p_path);

	// Reads an exchange file into one entry per material slot that has faces:
	// {material_index: int, material_name: String, arrays: Array} where
	// arrays is an indexed triangle surface with normals, UVs and tangents
	// when the file has UVs. Fails with r_error set when the file is unusable;
	// a mesh without faces is not an error and gives an empty Array.
	static Array read_surfaces(const String &p_path, Error *r_error = nullptr, Dictionary *r_header = nullptr);

	// Builds the mesh from read_surfaces() output. Each surface is named after
	// its material slot and takes the material of surface "material_index" of
	// p_material_source, when that mesh is given and has such a surface.
	static Ref<ArrayMesh> surfaces_to_mesh(const Array &p_surfaces, const Ref<Mesh> &p_material_source = Ref<Mesh>());

	// read_surfaces() followed by surfaces_to_mesh(). Null when the file cannot be read.
	static Ref<ArrayMesh> read_file(const String &p_path, const Ref<Mesh> &p_material_source = Ref<Mesh>());

	// The JSON header of an exchange file (counts, material names), or an empty Dictionary.
	static Dictionary read_header(const String &p_path);
};
