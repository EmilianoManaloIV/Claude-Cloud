extends "res://tetra_test.gd"
## TetraEditMesh: topology, selection flushing, validation, primitives,
## conversion to and from Mesh, storage.

const U := preload("res://tetra_model/edit_mesh_util.gd")


func run() -> void:
	_test_elements()
	_test_selection()
	_test_loops_and_rings()
	_test_primitives()
	_test_to_array_mesh()
	_test_from_mesh()
	_test_storage()


func _valid(p_mesh: TetraEditMesh, p_what: String) -> void:
	check_eq(p_mesh.validate(), "", p_what + " validates")


func _test_elements() -> void:
	var mesh := TetraEditMesh.new()
	_valid(mesh, "empty mesh")
	for p in [Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0), Vector3(2, 0, 0)]:
		mesh.add_vertex(p)
	var quad := mesh.add_face([0, 1, 2, 3], [Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
	check_eq(quad, 0, "first face index")
	check_eq(U.counts(mesh), Vector3i(5, 4, 1), "a quad derives four edges")
	var tri := mesh.add_face([1, 4, 2])
	check_eq(U.counts(mesh), Vector3i(5, 6, 2), "the shared edge is not duplicated")
	check_eq(mesh.get_face_vertices(tri), PackedInt32Array([1, 4, 2]), "face loop")
	check_eq(mesh.get_face_uvs(quad)[2], Vector2(1, 1), "corner UV")
	check_eq(mesh.get_face_uvs(tri).size(), 3, "missing UVs are filled in")
	check_eq(mesh.get_face_normal(quad), Vector3(0, 0, 1), "counter-clockwise loops face +Z")
	check_eq(mesh.get_face_center(quad), Vector3(0.5, 0.5, 0), "face centre")
	var shared := mesh.find_edge(2, 1)
	check(shared >= 0, "find_edge ignores direction")
	check_eq(mesh.get_edge_vertices(shared), Vector2i(1, 2), "edges store the smaller index first")
	check_eq(mesh.get_edge_faces(shared), PackedInt32Array([0, 1]), "edge faces")
	check_eq(mesh.find_edge(0, 2), -1, "no diagonal edge")
	check_eq(mesh.find_face([3, 2, 1, 0]), 0, "find_face ignores order")
	mesh.set_face_material(tri, 2)
	mesh.set_face_smooth(tri, true)
	check_eq(mesh.get_face_material(tri), 2, "material index")
	check(mesh.is_face_smooth(tri) and not mesh.is_face_smooth(quad), "smooth flag is per face")
	var wire := mesh.add_edge(0, 4)
	check_eq(mesh.get_edge_faces(wire).size(), 0, "wire edges are allowed")
	mesh.set_vertex(4, Vector3(3, 0, 0))
	check_eq(mesh.get_vertex(4), Vector3(3, 0, 0), "set_vertex")
	var moved := mesh.get_vertex_positions()
	moved[0] = Vector3(-1, 0, 0)
	mesh.set_vertex_positions(moved)
	check_eq(mesh.get_vertex(0), Vector3(-1, 0, 0), "set_vertex_positions")
	_valid(mesh, "hand-built mesh")
	mesh.clear()
	check_eq(U.counts(mesh), Vector3i(0, 0, 0), "clear")


func _test_selection() -> void:
	var cube := U.cube()
	var top := U.face_with_normal(cube, Vector3(0, 1, 0))
	check(top >= 0, "the cube has a top face")
	check_eq(cube.get_select_mode(), TetraEditMesh.SELECT_VERTEX, "vertex mode by default")

	# Vertex mode flushes up: an edge or face is selected when all its vertices are.
	var top_verts := cube.get_face_vertices(top)
	for i in 3:
		cube.select_vertex(top_verts[i])
	check_eq(cube.get_selected_edges().size(), 2, "three corners select two edges")
	check_eq(cube.get_selected_faces().size(), 0, "three corners do not select the face")
	cube.select_vertex(top_verts[3])
	check_eq(cube.get_selected_edges().size(), 4, "four corners select four edges")
	check_eq(cube.get_selected_faces(), PackedInt32Array([top]), "four corners select the face")
	_valid(cube, "vertex selection")

	# Going up keeps what is entirely selected.
	cube.set_select_mode(TetraEditMesh.SELECT_EDGE)
	check_eq(cube.get_selected_edges().size(), 4, "vertex -> edge keeps the edges")
	check(cube.is_face_selected(top), "vertex -> edge keeps the face")
	cube.set_select_mode(TetraEditMesh.SELECT_FACE)
	check_eq(cube.get_selected_faces(), PackedInt32Array([top]), "edge -> face keeps the face")
	cube.set_select_mode(TetraEditMesh.SELECT_VERTEX)
	check_eq(cube.get_selected_vertices().size(), 4, "face -> vertex keeps the vertices")
	_valid(cube, "mode round trip")

	# Two diagonal corners select no edge, so edge mode drops them.
	cube.select_none()
	cube.select_vertex(top_verts[0])
	cube.select_vertex(top_verts[2])
	cube.set_select_mode(TetraEditMesh.SELECT_EDGE)
	check_eq(cube.get_selected_vertices().size(), 0, "vertex -> edge drops lone vertices")

	# Edge mode: vertices follow edges, faces need all their edges.
	var e0 := cube.find_edge(top_verts[0], top_verts[1])
	cube.select_edge(e0)
	check_eq(cube.get_selected_vertices().size(), 2, "an edge selects its vertices")
	for i in range(1, 4):
		cube.select_edge(cube.find_edge(top_verts[i], top_verts[(i + 1) % 4]))
	check(cube.is_face_selected(top), "four edges select the face")
	cube.select_edge(e0, false)
	check(not cube.is_face_selected(top), "deselecting an edge deselects the face")
	check_eq(cube.get_selected_vertices().size(), 4, "the other edges keep the vertices")
	_valid(cube, "edge selection")
	cube.set_select_mode(TetraEditMesh.SELECT_FACE)
	check_eq(cube.get_selected_edges().size(), 0, "edge -> face drops partly selected faces")

	# Face mode: vertices and edges follow faces.
	cube.select_face(top)
	check_eq(cube.get_selected_vertices().size(), 4, "a face selects its vertices")
	check_eq(cube.get_selected_edges().size(), 4, "a face selects its edges")
	cube.select_invert()
	check_eq(cube.get_selected_faces().size(), 5, "invert in face mode")
	check_eq(cube.get_selected_vertices().size(), 8, "the other five faces use every vertex")
	_valid(cube, "face selection")
	cube.select_face(top)
	cube.select_face(top, false)
	check_eq(cube.get_selected_faces().size(), 5, "deselect one face")
	cube.select_all()
	check_eq(Vector3i(cube.get_selected_vertices().size(), cube.get_selected_edges().size(), cube.get_selected_faces().size()), Vector3i(8, 12, 6), "select all")
	cube.select_none()
	check_eq(cube.get_selected_vertices().size(), 0, "select none")

	# More / less.
	cube.set_select_mode(TetraEditMesh.SELECT_VERTEX)
	cube.select_vertex(0)
	cube.select_more()
	check_eq(cube.get_selected_vertices().size(), 4, "more: the vertex and its three neighbours")
	cube.select_less()
	check_eq(cube.get_selected_vertices(), PackedInt32Array([0]), "less undoes more")
	cube.set_select_mode(TetraEditMesh.SELECT_FACE)
	cube.select_none()
	cube.select_face(top)
	cube.select_more()
	check_eq(cube.get_selected_faces().size(), 5, "more in face mode: the faces around")
	cube.select_less()
	check_eq(cube.get_selected_faces(), PackedInt32Array([top]), "less in face mode")
	_valid(cube, "more/less")

	# Linked.
	var two := TetraEditMesh.new()
	two.add_cube()
	two.add_cube()
	check_eq(two.get_selected_vertices().size(), 8, "a new primitive is the only selection")
	check(two.is_vertex_selected(8) and not two.is_vertex_selected(0), "the second cube is selected")
	two.select_none()
	two.select_vertex(3)
	two.select_linked()
	check_eq(two.get_selected_vertices(), PackedInt32Array([0, 1, 2, 3, 4, 5, 6, 7]), "select linked takes the island")
	two.select_linked_from_vertex(12)
	check_eq(two.get_selected_vertices().size(), 16, "select linked from a vertex extends")
	two.select_linked_from_vertex(1, false)
	check_eq(two.get_selected_faces().size(), 6, "deselect linked")
	_valid(two, "linked")


func _test_loops_and_rings() -> void:
	var grid := U.grid(4, 4, 4.0)
	check_eq(U.counts(grid), Vector3i(25, 40, 16), "4x4 grid")
	var inner := grid.find_edge(6, 7)
	check_eq(grid.get_edge_loop(inner).size(), 4, "an inner edge loop crosses the grid")
	check_eq(grid.get_edge_loop(grid.find_edge(0, 1)).size(), 16, "a boundary loop goes around")
	check_eq(grid.get_edge_ring(inner).size(), 5, "an edge ring crosses the rows")
	grid.set_select_mode(TetraEditMesh.SELECT_EDGE)
	grid.select_edge_loop(inner)
	check_eq(grid.get_selected_edges().size(), 4, "select edge loop")
	check_eq(grid.get_selected_vertices().size(), 5, "loop vertices")
	grid.select_edge_ring(inner, true)
	check_eq(grid.get_selected_edges().size(), 8, "extend with the ring (shares one edge)")
	_valid(grid, "loop selection")
	grid.set_select_mode(TetraEditMesh.SELECT_VERTEX)
	grid.select_edge_loop(inner)
	check_eq(grid.get_selected_vertices().size(), 5, "edge loop in vertex mode")
	_valid(grid, "loop selection in vertex mode")

	var cube := U.cube()
	var edge := cube.find_edge(0, 1)
	check_eq(cube.get_edge_ring(edge).size(), 4, "a cube edge ring has four edges")
	check_eq(cube.get_edge_loop(edge).size(), 1, "three-edge corners end an edge loop")


func _check_primitive(p_mesh: TetraEditMesh, p_counts: Vector3i, p_what: String, p_closed: bool) -> void:
	check_eq(U.counts(p_mesh), p_counts, p_what + " counts")
	_valid(p_mesh, p_what)
	check_eq(p_mesh.get_selected_vertices().size(), p_counts.x, p_what + " is selected")
	if p_closed:
		check(U.closed(p_mesh), p_what + " is closed with consistent winding")
		check(U.volume(p_mesh) > 0.0, p_what + " faces point outwards")


func _test_primitives() -> void:
	var mesh := TetraEditMesh.new()
	mesh.add_plane()
	_check_primitive(mesh, Vector3i(4, 4, 1), "plane", false)
	check_eq(mesh.get_face_normal(0), Vector3(0, 1, 0), "the plane faces up (Y)")
	check_eq(mesh.get_vertex(0), Vector3(-1, 0, 1), "plane vertex 0 is Blender's (-1, -1, 0)")
	check_eq(mesh.get_vertex(3), Vector3(1, 0, -1), "plane vertex 3")
	check_eq(mesh.get_face_vertices(0), PackedInt32Array([0, 1, 3, 2]), "plane loop")

	mesh = TetraEditMesh.new()
	mesh.add_cube()
	_check_primitive(mesh, Vector3i(8, 12, 6), "cube", true)
	check_eq(mesh.get_vertex(0), Vector3(-1, -1, 1), "cube vertex 0")
	check_eq(mesh.get_vertex(7), Vector3(1, 1, -1), "cube vertex 7")
	check_eq(mesh.get_face_vertices(5), PackedInt32Array([7, 3, 1, 5]), "cube face 5 is Blender's")
	check_eq(mesh.get_face_normal(5), Vector3(0, 1, 0), "cube face 5 is the top")
	check_near(U.volume(mesh), 8.0, 1e-4, "cube volume")

	mesh = TetraEditMesh.new()
	mesh.add_grid(10, 10, 2.0)
	_check_primitive(mesh, Vector3i(121, 220, 100), "grid", false)
	check_eq(mesh.get_vertex(120), Vector3(1, 0, -1), "grid last vertex")
	check(mesh.get_vertex(1).is_equal_approx(Vector3(-0.8, 0, 1)), "grid runs along X first")
	check_eq(mesh.get_face_normal(37), Vector3(0, 1, 0), "grid faces up")
	check(mesh.get_face_uvs(0)[2].is_equal_approx(Vector2(0.1, 0.1)), "grid UVs")

	mesh = TetraEditMesh.new()
	mesh.add_circle(32, 1.0, TetraEditMesh.FILL_NOTHING)
	_check_primitive(mesh, Vector3i(32, 32, 0), "circle", false)
	check(mesh.get_vertex(0).is_equal_approx(Vector3(0, 0, -1)), "circle starts at Blender's +Y")
	mesh = TetraEditMesh.new()
	mesh.add_circle(32, 1.0, TetraEditMesh.FILL_NGON)
	_check_primitive(mesh, Vector3i(32, 32, 1), "filled circle", false)
	check(mesh.get_face_normal(0).is_equal_approx(Vector3(0, 1, 0)), "filled circle faces up")
	mesh = TetraEditMesh.new()
	mesh.add_circle(32, 1.0, TetraEditMesh.FILL_TRIANGLE_FAN)
	_check_primitive(mesh, Vector3i(33, 64, 32), "triangle fan circle", false)

	mesh = TetraEditMesh.new()
	mesh.add_cylinder(32, 1.0, 2.0, TetraEditMesh.FILL_NGON)
	_check_primitive(mesh, Vector3i(64, 96, 34), "cylinder", true)
	check_near(mesh.get_vertex(0).y, -1.0, 1e-5, "cylinder vertex 0 is on the bottom")
	check_near(mesh.get_vertex(1).y, 1.0, 1e-5, "cylinder vertex 1 is on the top")
	mesh = TetraEditMesh.new()
	mesh.add_cylinder(32, 1.0, 2.0, TetraEditMesh.FILL_TRIANGLE_FAN)
	_check_primitive(mesh, Vector3i(66, 160, 96), "triangle fan cylinder", true)
	mesh = TetraEditMesh.new()
	mesh.add_cylinder(8, 1.0, 2.0, TetraEditMesh.FILL_NOTHING)
	_check_primitive(mesh, Vector3i(16, 24, 8), "open cylinder", false)

	mesh = TetraEditMesh.new()
	mesh.add_cone()
	_check_primitive(mesh, Vector3i(33, 64, 33), "cone", true)
	check(mesh.get_vertex(32).is_equal_approx(Vector3(0, 1, 0)), "cone apex")
	mesh = TetraEditMesh.new()
	mesh.add_cone(16, 1.0, 0.5, 2.0, TetraEditMesh.FILL_NGON)
	_check_primitive(mesh, Vector3i(32, 48, 18), "truncated cone", true)

	mesh = TetraEditMesh.new()
	mesh.add_uv_sphere()
	_check_primitive(mesh, Vector3i(482, 992, 512), "uv sphere", true)
	var on_sphere := true
	for v in mesh.get_vertex_count():
		on_sphere = on_sphere and absf(mesh.get_vertex(v).length() - 1.0) < 1e-5
	check(on_sphere, "uv sphere vertices are at the radius")
	mesh = TetraEditMesh.new()
	mesh.add_uv_sphere(8, 4, 2.0)
	_check_primitive(mesh, Vector3i(26, 56, 32), "small uv sphere", true)

	for level in [[1, Vector3i(12, 30, 20)], [2, Vector3i(42, 120, 80)], [3, Vector3i(162, 480, 320)]]:
		mesh = TetraEditMesh.new()
		mesh.add_ico_sphere(level[0], 1.0)
		_check_primitive(mesh, level[1], "ico sphere %d" % level[0], true)
	check_near(mesh.get_vertex(0).y, -1.0, 1e-5, "ico vertex 0 is the bottom pole")
	check_near(mesh.get_vertex(100).length(), 1.0, 1e-5, "ico vertices are at the radius")

	var operations := TetraEditMesh.get_primitive_operations()
	check_eq(operations.size(), 8, "eight primitives are described")
	check_eq(operations[1]["id"], "cube", "operation ids")
	mesh = TetraEditMesh.new()
	check(mesh.add_primitive("cube", {"size": 4.0}), "add_primitive")
	check_eq(mesh.get_vertex(0), Vector3(-2, -2, 2), "add_primitive passes the values")
	mesh.add_primitive("uv_sphere", {})
	check_eq(U.counts(mesh), Vector3i(490, 1004, 518), "primitives append; defaults apply")
	check_eq(mesh.get_selected_faces().size(), 512, "only the new primitive is selected")
	_valid(mesh, "two primitives")


func _triangle_normals_agree(p_mesh: ArrayMesh) -> bool:
	for s in p_mesh.get_surface_count():
		var arrays := p_mesh.surface_get_arrays(s)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		for i in range(0, indices.size(), 3):
			# Godot's front faces are clockwise.
			var normal := (positions[indices[i + 2]] - positions[indices[i]]).cross(positions[indices[i + 1]] - positions[indices[i]])
			for k in 3:
				if normal.dot(normals[indices[i + k]]) <= 0.0:
					return false
	return true


func _area(p_mesh: ArrayMesh) -> float:
	var total := 0.0
	for s in p_mesh.get_surface_count():
		var arrays := p_mesh.surface_get_arrays(s)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		for i in range(0, indices.size(), 3):
			total += (positions[indices[i + 1]] - positions[indices[i]]).cross(positions[indices[i + 2]] - positions[indices[i]]).length() * 0.5
	return total


func _test_to_array_mesh() -> void:
	var cube := U.cube()
	var mesh := cube.to_array_mesh()
	check_eq(mesh.get_surface_count(), 1, "one material, one surface")
	var arrays := mesh.surface_get_arrays(0)
	check_eq(arrays[Mesh.ARRAY_VERTEX].size(), 24, "flat faces split their corners")
	check_eq(arrays[Mesh.ARRAY_INDEX].size(), 36, "six quads are twelve triangles")
	check_eq(arrays[Mesh.ARRAY_TEX_UV].size(), 24, "UVs are written")
	check(_triangle_normals_agree(mesh), "triangles wind to their normals")
	check_near(_area(mesh), 24.0, 1e-4, "cube surface area")
	check(mesh.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "bounds")

	# Smooth faces share vertices with averaged normals.
	var ico := TetraEditMesh.new()
	ico.add_ico_sphere(2, 1.0)
	check_eq(ico.to_array_mesh().surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size(), 240, "flat sphere: three corners per face")
	check(ico.shade_smooth(true), "shade smooth")
	var smooth := ico.to_array_mesh()
	arrays = smooth.surface_get_arrays(0)
	check_eq(arrays[Mesh.ARRAY_VERTEX].size(), 42, "smooth sphere: one corner per vertex")
	var radial := true
	for i in arrays[Mesh.ARRAY_VERTEX].size():
		radial = radial and arrays[Mesh.ARRAY_NORMAL][i].dot(arrays[Mesh.ARRAY_VERTEX][i].normalized()) > 0.99
	check(radial, "smooth normals are the vertex normals")
	check(_triangle_normals_agree(smooth), "smooth triangles wind outwards")

	# One surface per material index.
	cube.set_face_material(5, 1)
	var red := StandardMaterial3D.new()
	var blue := StandardMaterial3D.new()
	cube.set_materials([red, blue])
	mesh = cube.to_array_mesh()
	check_eq(mesh.get_surface_count(), 2, "two materials, two surfaces")
	check_eq(mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX].size(), 30, "five faces on material 0")
	check_eq(mesh.surface_get_arrays(1)[Mesh.ARRAY_INDEX].size(), 6, "one face on material 1")
	check(mesh.surface_get_material(0) == red and mesh.surface_get_material(1) == blue, "surface materials")

	# N-gons, including a concave one.
	var circle := TetraEditMesh.new()
	circle.add_circle(32, 1.0, TetraEditMesh.FILL_NGON)
	mesh = circle.to_array_mesh()
	check_eq(mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX].size(), 90, "a 32-gon is thirty triangles")
	check(_triangle_normals_agree(mesh), "n-gon triangles wind to the normal")
	var shape := TetraEditMesh.new()
	for p in [Vector2(0, 0), Vector2(2, 0), Vector2(2, 1), Vector2(1, 1), Vector2(1, 2), Vector2(0, 2)]:
		shape.add_vertex(Vector3(p.x, p.y, 0))
	shape.add_face([0, 1, 2, 3, 4, 5])
	mesh = shape.to_array_mesh()
	check_eq(mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX].size(), 12, "an L-shaped hexagon is four triangles")
	check_near(_area(mesh), 3.0, 1e-4, "the concave n-gon is covered exactly")
	check(_triangle_normals_agree(mesh), "concave n-gon triangles wind to the normal")
	check_eq(TetraEditMesh.new().to_array_mesh().get_surface_count(), 0, "an empty mesh has no surface")


func _test_from_mesh() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(2, 2, 2)
	var material := StandardMaterial3D.new()
	box.material = material
	var mesh := TetraEditMesh.new()
	mesh.from_mesh(box)
	check_eq(U.counts(mesh), Vector3i(8, 18, 12), "a box welds to eight vertices and twelve triangles")
	_valid(mesh, "box triangles")
	check(U.closed(mesh), "the welded box is closed")
	check_near(U.volume(mesh), 8.0, 1e-4, "the box faces point outwards")
	check(not mesh.is_face_smooth(0), "box faces are flat")
	check(mesh.get_materials()[0] == material, "the surface material is kept")

	mesh.from_mesh(box, true)
	check_eq(U.counts(mesh), Vector3i(8, 12, 6), "coplanar triangle pairs become quads")
	_valid(mesh, "box quads")
	check(U.closed(mesh), "the quad box is closed")
	check_near(U.volume(mesh), 8.0, 1e-4, "quad box volume")
	check(mesh.get_face_uvs(0)[0] != mesh.get_face_uvs(0)[2], "UVs survive")

	# Round trip through an ArrayMesh.
	var again := TetraEditMesh.new()
	again.from_mesh(mesh.to_array_mesh(), true)
	check_eq(U.counts(again), Vector3i(8, 12, 6), "to_array_mesh -> from_mesh round trip")
	check(again.to_array_mesh().surface_get_material(0) == material, "the material makes the round trip")

	var plane := PlaneMesh.new()
	plane.size = Vector2(2, 2)
	plane.subdivide_width = 1
	plane.subdivide_depth = 1
	mesh.from_mesh(plane, true)
	check_eq(U.counts(mesh), Vector3i(9, 12, 4), "a subdivided plane becomes four quads")
	check_eq(mesh.get_face_normal(0), Vector3(0, 1, 0), "the plane faces up")
	_valid(mesh, "plane quads")

	var sphere := SphereMesh.new()
	sphere.radial_segments = 8
	sphere.rings = 4
	mesh.from_mesh(sphere, true)
	_valid(mesh, "sphere")
	var all_smooth := mesh.get_face_count() > 0
	for f in mesh.get_face_count():
		all_smooth = all_smooth and mesh.is_face_smooth(f)
	check(all_smooth, "averaged normals mark faces smooth")
	check(U.volume(mesh) > 0.0, "sphere faces point outwards")

	# Two surfaces keep their material indices.
	var cube := U.cube()
	cube.set_face_material(5, 1)
	again.from_mesh(cube.to_array_mesh(), true)
	check_eq(U.counts(again), Vector3i(8, 12, 6), "two-surface cube")
	var on_second := 0
	for f in again.get_face_count():
		on_second += again.get_face_material(f)
	check_eq(on_second, 1, "one face on the second material")

	mesh.from_mesh(null)
	check_eq(U.counts(mesh), Vector3i(0, 0, 0), "a null mesh clears")


func _test_storage() -> void:
	var cube := U.cube()
	cube.set_select_mode(TetraEditMesh.SELECT_FACE)
	cube.select_face(5)
	cube.set_face_material(2, 3)
	cube.set_face_smooth(4, true)
	cube.add_vertex(Vector3(5, 5, 5))
	cube.add_edge(0, 8)
	var state := cube.get_state()
	check(state.size() > 0, "state bytes")

	var copy := TetraEditMesh.new()
	check(copy.set_state(state), "set_state")
	check_eq(U.counts(copy), Vector3i(9, 13, 6), "state restores the elements")
	check_eq(copy.get_select_mode(), TetraEditMesh.SELECT_FACE, "state restores the select mode")
	check_eq(copy.get_selected_faces(), PackedInt32Array([5]), "state restores the selection")
	check_eq(copy.get_face_material(2), 3, "state restores materials")
	check(copy.is_face_smooth(4), "state restores smooth flags")
	check_eq(copy.get_face_uvs(1), cube.get_face_uvs(1), "state restores UVs")
	check_eq(copy.get_vertex_positions(), cube.get_vertex_positions(), "state restores positions")
	check_eq(copy.get_state(), state, "state is stable")
	_valid(copy, "restored mesh")

	# Snapshot undo.
	var before := cube.get_state()
	cube.extrude_region()
	cube.translate_selection(Vector3(0, 1, 0))
	check(cube.get_state() != before, "an operation changes the state")
	cube.set_state(before)
	check_eq(cube.get_state(), state, "set_state undoes it")

	# Saved as a resource, alone and inside a scene.
	var path := tmp_dir().path_join("edit_mesh.tres")
	check_eq(ResourceSaver.save(cube, path), OK, "save as a resource")
	var loaded: TetraEditMesh = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	check(loaded != null and U.counts(loaded) == Vector3i(9, 13, 6), "load the resource")
	check_eq(loaded.get_face_material(2), 3, "loaded face data")

	var node := MeshInstance3D.new()
	node.name = "Edited"
	node.mesh = cube.to_array_mesh()
	node.set_meta("tetra_edit_mesh", cube)
	var scene := PackedScene.new()
	check_eq(scene.pack(node), OK, "pack a node that carries an edit mesh")
	var scene_path := tmp_dir().path_join("edit_mesh_scene.tscn")
	check_eq(ResourceSaver.save(scene, scene_path), OK, "save the scene")
	node.free()
	var reloaded: PackedScene = ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	var instance := reloaded.instantiate()
	var stored: TetraEditMesh = instance.get_meta("tetra_edit_mesh")
	check(stored != null and U.counts(stored) == Vector3i(9, 13, 6), "the edit mesh is saved inside the scene")
	_valid(stored, "edit mesh from the scene")
	instance.free()

	var empty := TetraEditMesh.new()
	check(empty.set_state(PackedByteArray()), "empty data is an empty mesh")
	check_eq(U.counts(empty), Vector3i(0, 0, 0), "empty state")
