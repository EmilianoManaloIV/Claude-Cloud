extends "res://tetra_test.gd"
## TetraBlenderMesh: coordinate conventions and the exchange file format.
## Needs no Blender.


func run() -> void:
	check_eq(TetraBlenderMesh.position_to_blender(Vector3(1, 2, 3)), Vector3(1, -3, 2), "Godot up (+Y) is Blender +Z, Godot forward (-Z) is Blender +Y")
	check_eq(TetraBlenderMesh.position_from_blender(Vector3(1, -3, 2)), Vector3(1, 2, 3), "position round trip")

	# A mesh object: its local axes are converted along with its vertices, so
	# a pure translation stays a pure translation.
	var moved := Transform3D(Basis(), Vector3(1, 2, 3))
	var matrix := TetraBlenderMesh.transform_to_matrix(moved, true)
	check_eq(matrix, PackedFloat64Array([1, 0, 0, 1, 0, 1, 0, -3, 0, 0, 1, 2, 0, 0, 0, 1]), "object matrix is row-major, Z up")
	# A camera keeps its local axes (it looks down -Z in both programs): the
	# identity camera of Godot looks along Blender's +Y with Blender's +Z up.
	matrix = TetraBlenderMesh.transform_to_matrix(Transform3D(), false)
	check_eq(matrix, PackedFloat64Array([1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1]), "camera matrix")
	var turned := Transform3D(Basis(Vector3(0, 1, 0), 0.7).rotated(Vector3(1, 0, 0), -0.3), Vector3(4, 5, 6))
	for local_axes: bool in [true, false]:
		var back := TetraBlenderMesh.matrix_to_transform(TetraBlenderMesh.transform_to_matrix(turned, local_axes), local_axes)
		check(back.is_equal_approx(turned), "transform round trip (local axes %s)" % local_axes)

	var box := BoxMesh.new()
	box.size = Vector3(1, 2, 3)
	var path := tmp_dir().path_join("exchange-box.tmsh")
	check_eq(TetraBlenderMesh.write_file(box, path), OK, "write a BoxMesh")
	var header := TetraBlenderMesh.read_header(path)
	check_eq(int(header["vertex_count"]), 8, "the 24 vertices of a box weld to 8")
	check_eq(int(header["polygon_count"]), 12, "12 triangles")
	check_eq(int(header["corner_count"]), 36, "36 corners")
	check(header["has_uvs"] and header["has_normals"], "UVs and normals are written")
	check_eq(header["materials"], ["Surface0"], "one material slot per surface")

	# Files without a stored triangulation are fanned on the Godot side.
	var back_mesh := TetraBlenderMesh.read_file(path)
	check(back_mesh != null and back_mesh.get_surface_count() == 1, "read the box back")
	if back_mesh != null and back_mesh.get_surface_count() == 1:
		check(back_mesh.get_aabb().is_equal_approx(box.get_aabb()), "bounding box and axes survive: %s" % back_mesh.get_aabb())
		var arrays := back_mesh.surface_get_arrays(0)
		check_eq((arrays[Mesh.ARRAY_INDEX] as PackedInt32Array).size(), 36, "12 triangles come back")
		check_eq((arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size(), 24, "corners with equal normal and UV share a vertex")
		check(arrays[Mesh.ARRAY_TANGENT] != null, "tangents are generated when there are UVs")
		check_eq(_misoriented_triangles(arrays), 0, "winding matches the normals")
		check_eq(_uv_by_corner(arrays), _uv_by_corner(box.surface_get_arrays(0)), "every corner keeps its UV")
	check_eq(_misoriented_triangles(box.surface_get_arrays(0)), 0, "the orientation check holds for the source mesh")

	# Two surfaces sharing an edge: welded across surfaces, one slot each.
	var material := StandardMaterial3D.new()
	material.resource_name = "Red"
	var two := ArrayMesh.new()
	two.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, _triangle(Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(1, 0, 0)))
	two.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, _triangle(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(1, 1, 0)))
	two.surface_set_material(1, material)
	path = tmp_dir().path_join("exchange-two.tmsh")
	check_eq(TetraBlenderMesh.write_file(two, path), OK, "write two surfaces")
	header = TetraBlenderMesh.read_header(path)
	check_eq(int(header["vertex_count"]), 4, "vertices weld across surfaces")
	check_eq(header["materials"], ["Surface0", "Red"], "slots are named after the materials")
	var surfaces := TetraBlenderMesh.read_surfaces(path)
	check_eq(surfaces.size(), 2, "two surfaces come back")
	back_mesh = TetraBlenderMesh.surfaces_to_mesh(surfaces, two)
	check(back_mesh.get_surface_count() == 2 and back_mesh.surface_get_material(1) == material and back_mesh.surface_get_material(0) == null, "materials are taken from the source mesh by slot")
	check_eq(back_mesh.surface_get_name(1), "Red", "surfaces are named after their slot")

	check(TetraBlenderMesh.read_file(tmp_dir().path_join("missing.tmsh")) == null, "a missing file gives null")
	var garbage := FileAccess.open(tmp_dir().path_join("garbage.tmsh"), FileAccess.WRITE)
	garbage.store_string("TMSH but not really")
	garbage.close()
	check(TetraBlenderMesh.read_file(tmp_dir().path_join("garbage.tmsh")) == null, "a malformed file gives null")


func _triangle(p_a: Vector3, p_b: Vector3, p_c: Vector3) -> Array:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([p_a, p_b, p_c])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1)])
	return arrays


## Triangles whose clockwise front side disagrees with their vertex normals.
func _misoriented_triangles(p_arrays: Array) -> int:
	var vertices: PackedVector3Array = p_arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = p_arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = p_arrays[Mesh.ARRAY_INDEX]
	var wrong := 0
	for i in range(0, indices.size(), 3):
		var a := vertices[indices[i]]
		var b := vertices[indices[i + 1]]
		var c := vertices[indices[i + 2]]
		if (c - a).cross(b - a).dot(normals[indices[i]]) <= 0.0:
			wrong += 1
	return wrong


## The set of (position, UV) pairs of a surface, order-independent.
func _uv_by_corner(p_arrays: Array) -> Array:
	var vertices: PackedVector3Array = p_arrays[Mesh.ARRAY_VERTEX]
	var uvs: PackedVector2Array = p_arrays[Mesh.ARRAY_TEX_UV]
	var seen := {}
	for index: int in p_arrays[Mesh.ARRAY_INDEX]:
		seen["%s %s" % [vertices[index].snappedf(0.001), uvs[index].snappedf(0.001)]] = true
	var keys := seen.keys()
	keys.sort()
	return keys
