extends "res://tetra_test.gd"
## TetraEditMesh operations, with exact counts and positions on a cube, a
## plane and a grid. Every result must validate.

const U := preload("res://tetra_model/edit_mesh_util.gd")
const V := TetraEditMesh.SELECT_VERTEX
const E := TetraEditMesh.SELECT_EDGE
const F := TetraEditMesh.SELECT_FACE
const UP := Vector3(0, 1, 0)


func run() -> void:
	_test_transform()
	_test_extrude()
	_test_inset()
	_test_subdivide()
	_test_loop_cut()
	_test_delete()
	_test_dissolve()
	_test_merge()
	_test_fill_and_bridge()
	_test_normals()
	_test_duplicate_separate()
	_test_triangulate()


func _expect(p_mesh: TetraEditMesh, p_counts: Vector3i, p_what: String) -> void:
	check_eq(U.counts(p_mesh), p_counts, p_what + ": counts")
	check_eq(p_mesh.validate(), "", p_what + ": validates")


func _cube_with(p_mode: int, p_normals: Array) -> TetraEditMesh:
	var cube := U.cube()
	cube.set_select_mode(p_mode)
	for normal in p_normals:
		cube.select_face(U.face_with_normal(cube, normal))
	return cube


func _selected_center(p_mesh: TetraEditMesh) -> Vector3:
	return p_mesh.get_selection_pivot(TetraEditMesh.PIVOT_MEDIAN)


func _test_transform() -> void:
	var cube := _cube_with(F, [UP])
	check_eq(_selected_center(cube), Vector3(0, 1, 0), "median pivot of the top face")
	check_eq(cube.get_selection_normal(), UP, "selection normal")
	var corner := U.vertex_at(cube, Vector3(1, 1, 1))
	cube.translate_selection(Vector3(0, 1, 0))
	check_eq(cube.get_vertex(corner), Vector3(1, 2, 1), "translate moves the selection")
	check_eq(U.vertex_at(cube, Vector3(1, -1, 1)) >= 0, true, "translate leaves the rest")
	cube.scale_selection(Vector3(0.5, 0.5, 0.5))
	check(cube.get_vertex(corner).is_equal_approx(Vector3(0.5, 2, 0.5)), "scale about the median")
	cube.rotate_selection(UP, PI / 2)
	check(cube.get_vertex(corner).is_equal_approx(Vector3(0.5, 2, -0.5)), "rotate about the median")
	cube.transform_selection(Transform3D(Basis.from_scale(Vector3(2, 1, 2)), Vector3(0, -1, 0)), Vector3(0, 2, 0))
	check(cube.get_vertex(corner).is_equal_approx(Vector3(1, 1, -1)), "transform about an explicit pivot")
	check_eq(cube.validate(), "", "transformed cube validates")

	# Bounding box centre differs from the median for an uneven selection.
	var grid := U.grid(4, 4, 4.0)
	for v in [0, 1, 4]:
		grid.select_vertex(v)
	check(grid.get_selection_pivot(TetraEditMesh.PIVOT_MEDIAN).is_equal_approx(Vector3(-1.0 / 3.0, 0, 2)), "median of three vertices")
	check(grid.get_selection_pivot(TetraEditMesh.PIVOT_BOUNDING_BOX).is_equal_approx(Vector3(0, 0, 2)), "bounding box centre")
	grid.scale_selection(Vector3(0, 0, 0), TetraEditMesh.PIVOT_BOUNDING_BOX)
	check(grid.get_vertex(0).is_equal_approx(Vector3(0, 0, 2)), "scale to zero about the bounding box centre")

	# Proportional editing with the smooth falloff.
	grid = U.grid(4, 4, 4.0)
	grid.select_vertex(12)
	grid.translate_selection(Vector3(0, 1, 0), 1.5)
	check_near(grid.get_vertex(12).y, 1.0, 1e-5, "the selected vertex moves fully")
	var t := 1.0 - 1.0 / 1.5
	check_near(grid.get_vertex(13).y, 3 * t * t - 2 * t * t * t, 1e-5, "a neighbour follows by the falloff")
	t = 1.0 - sqrt(2.0) / 1.5
	check_near(grid.get_vertex(18).y, 3 * t * t - 2 * t * t * t, 1e-5, "a diagonal neighbour follows less")
	check_near(grid.get_vertex(14).y, 0.0, 1e-6, "vertices outside the radius stay")


func _test_extrude() -> void:
	# A face of a closed mesh: the face moves, four sides appear.
	var cube := _cube_with(F, [UP])
	check(cube.extrude_region(), "extrude region")
	_expect(cube, Vector3i(12, 20, 10), "extruded cube face")
	check_eq(cube.get_selected_faces().size(), 1, "the moved face is selected")
	check_eq(cube.get_selected_vertices().size(), 4, "its four new vertices are selected")
	check_eq(cube.get_selection_normal(), UP, "the extruded face keeps its normal")
	check(U.closed(cube), "the extruded cube is closed")
	cube.translate_selection(cube.get_selection_normal())
	check_eq(cube.get_face_center(cube.get_selected_faces()[0]), Vector3(0, 2, 0), "the face moved up")
	check_near(U.volume(cube), 12.0, 1e-4, "side faces wind outwards")
	check_eq(U.vertex_at(cube, Vector3(1, 1, 1)) >= 0, true, "the old corners stay")

	# A lone face: the original stays as the flipped bottom (Blender).
	var plane := U.plane()
	plane.set_select_mode(F)
	plane.select_face(0)
	check(plane.extrude_region(), "extrude a plane")
	_expect(plane, Vector3i(8, 12, 6), "extruded plane")
	plane.translate_selection(UP)
	check(U.closed(plane), "the extruded plane is a closed box")
	check_near(U.volume(plane), 4.0, 1e-4, "box volume, faces outwards")

	# A lone region with interior edges: those faces move, no bottom is left.
	var grid := U.grid(2, 2, 2.0)
	grid.select_all()
	check(grid.extrude_region(), "extrude a grid")
	_expect(grid, Vector3i(17, 28, 12), "extruded grid")
	check_eq(grid.get_selected_faces().size(), 4, "the four moved faces are selected")

	# Two adjacent faces: the edge between them is not extruded.
	cube = _cube_with(F, [UP, Vector3(1, 0, 0)])
	check(cube.extrude_region(), "extrude two faces")
	_expect(cube, Vector3i(14, 24, 12), "two extruded faces")
	check(U.closed(cube), "two extruded faces: closed")
	check_eq(cube.get_selected_faces().size(), 2, "two faces selected")

	# An edge becomes a face, wound against its neighbour.
	plane = U.plane()
	plane.set_select_mode(E)
	plane.select_edge(plane.find_edge(1, 3))
	check(plane.extrude_region(), "extrude an edge")
	_expect(plane, Vector3i(6, 7, 2), "extruded edge")
	check_eq(plane.get_selected_edges().size(), 1, "the new edge is selected")
	plane.translate_selection(Vector3(1, 0, 0))
	check_eq(plane.get_face_normal(1), UP, "the new face continues the surface")

	# A vertex becomes an edge.
	plane = U.plane()
	plane.select_vertex(0)
	check(plane.extrude_region(), "extrude a vertex")
	_expect(plane, Vector3i(5, 5, 1), "extruded vertex")
	check_eq(plane.get_selected_vertices(), PackedInt32Array([4]), "the new vertex is selected")

	# The whole closed cube has no border to extrude from.
	cube = U.cube()
	cube.select_all()
	check(not cube.extrude_region(), "a closed region does not extrude")
	_expect(cube, Vector3i(8, 12, 6), "unchanged cube")
	check(not U.cube().extrude_region(), "nothing selected, nothing extruded")

	# Individual faces.
	cube = _cube_with(F, [UP, Vector3(1, 0, 0)])
	check(cube.extrude_faces_individual(1.0), "extrude individual faces")
	_expect(cube, Vector3i(16, 28, 14), "individually extruded faces")
	check_eq(cube.get_selected_faces().size(), 2, "both caps are selected")
	check(U.face_with_normal(cube, UP) >= 0 and cube.get_face_center(U.face_with_normal(cube, UP)) == Vector3(0, 2, 0), "the top cap moved along its normal")
	check(U.vertex_at(cube, Vector3(2, 1, 1)) >= 0, "the side cap moved along its own normal")
	check(U.closed(cube), "individually extruded cube is closed")
	check_near(U.volume(cube), 16.0, 1e-4, "volume of the two blocks")


func _test_inset() -> void:
	var cube := _cube_with(F, [UP])
	check(cube.inset_faces(0.25, 0.0), "inset")
	_expect(cube, Vector3i(12, 20, 10), "inset cube face")
	var inner := cube.get_selected_faces()
	check_eq(inner.size(), 1, "the inner face is selected")
	var exact := true
	for v in cube.get_face_vertices(inner[0]):
		var p := cube.get_vertex(v)
		exact = exact and is_equal_approx(absf(p.x), 0.75) and is_equal_approx(absf(p.z), 0.75) and is_equal_approx(p.y, 1.0)
	check(exact, "the inner face is 0.25 inside the border")
	check(U.closed(cube), "inset cube is closed")
	check_near(U.volume(cube), 8.0, 1e-4, "inset does not change the volume")

	cube = _cube_with(F, [UP])
	cube.inset_faces(0.5, -0.5)
	check_eq(cube.get_face_center(cube.get_selected_faces()[0]), Vector3(0, 0.5, 0), "negative depth sinks the face")
	check_near(U.volume(cube), 8.0 - (2.0 * 2.0 + 1.0 + 2.0) / 3.0 * 0.5, 1e-4, "volume of the sunk frustum")

	# A region of two faces is inset as one.
	cube = _cube_with(F, [UP, Vector3(1, 0, 0)])
	check(cube.inset_faces(0.25), "inset a region")
	_expect(cube, Vector3i(14, 24, 12), "inset region")
	check(U.closed(cube), "inset region is closed")
	check(U.vertex_at(cube, Vector3(1, 1, 0.75)) >= 0 and U.vertex_at(cube, Vector3(1, 1, -0.75)) >= 0, "vertices on the shared edge slide along it")
	check(U.vertex_at(cube, Vector3(-0.75, 1, 0.75)) >= 0, "the far corners move in both directions")
	check(not U.cube().inset_faces(0.1), "nothing selected, nothing inset")


func _test_subdivide() -> void:
	var cube := U.cube()
	cube.select_all()
	check(cube.subdivide(1), "subdivide")
	_expect(cube, Vector3i(26, 48, 24), "subdivided cube")
	check(U.closed(cube), "subdivided cube is closed")
	check(U.vertex_at(cube, Vector3(0, 1, 0)) >= 0 and U.vertex_at(cube, Vector3(1, 1, 0)) >= 0, "face and edge midpoints exist")
	check_eq(cube.get_selected_faces().size(), 24, "the result is selected")
	check_near(U.volume(cube), 8.0, 1e-4, "volume kept")

	cube = U.cube()
	cube.select_all()
	cube.subdivide(2)
	_expect(cube, Vector3i(56, 108, 54), "cube with two cuts")
	check(U.vertex_at(cube, Vector3(1.0 / 3.0, 1, -1.0 / 3.0)) >= 0, "inner grid points are evenly spaced")
	check(U.closed(cube), "two cuts: closed")

	# One face: its neighbours take the new vertices as n-gons.
	cube = _cube_with(F, [UP])
	cube.subdivide(1)
	_expect(cube, Vector3i(13, 20, 9), "one subdivided face")
	check(U.closed(cube), "one subdivided face: closed")
	check_eq(cube.get_face_vertices(U.face_with_normal(cube, Vector3(1, 0, 0))).size(), 5, "the neighbour became a 5-gon")

	# Triangles.
	var ico := TetraEditMesh.new()
	ico.add_ico_sphere(1, 1.0)
	ico.subdivide(1)
	_expect(ico, Vector3i(42, 120, 80), "subdivided icosahedron")
	check(U.closed(ico), "subdivided icosahedron is closed")
	ico = TetraEditMesh.new()
	ico.add_ico_sphere(1, 1.0)
	ico.subdivide(2)
	_expect(ico, Vector3i(92, 270, 180), "icosahedron with two cuts")
	check(U.closed(ico), "icosahedron with two cuts is closed")

	# A single edge, and wire edges.
	var plane := U.plane()
	plane.set_select_mode(E)
	plane.select_edge(plane.find_edge(0, 1))
	plane.subdivide(1)
	_expect(plane, Vector3i(5, 5, 1), "one subdivided edge")
	check_eq(plane.get_vertex(4), Vector3(0, 0, 1), "the new vertex is the midpoint")
	check_eq(plane.get_face_uvs(0).size(), 5, "the face gets a UV for it")
	var ring := TetraEditMesh.new()
	ring.add_circle(4, 1.0, TetraEditMesh.FILL_NOTHING)
	ring.subdivide(2)
	_expect(ring, Vector3i(12, 12, 0), "subdivided wire circle")
	check(not U.cube().subdivide(1), "nothing selected, nothing subdivided")


func _test_loop_cut() -> void:
	var cube := U.cube()
	cube.set_select_mode(E)
	var vertical := cube.find_edge(U.vertex_at(cube, Vector3(1, -1, 1)), U.vertex_at(cube, Vector3(1, 1, 1)))
	check(cube.loop_cut(vertical, 1), "loop cut")
	_expect(cube, Vector3i(12, 20, 10), "loop cut cube")
	check_eq(cube.get_selected_edges().size(), 4, "the new loop is selected")
	var level := true
	for v in cube.get_selected_vertices():
		level = level and is_zero_approx(cube.get_vertex(v).y)
	check(level, "one cut is at the middle")
	check(U.closed(cube), "loop cut cube is closed")
	check_near(U.volume(cube), 8.0, 1e-4, "loop cut keeps the volume")

	cube = U.cube()
	vertical = cube.find_edge(U.vertex_at(cube, Vector3(1, -1, 1)), U.vertex_at(cube, Vector3(1, 1, 1)))
	cube.loop_cut(vertical, 2)
	_expect(cube, Vector3i(16, 28, 14), "two loop cuts")
	check_eq(cube.get_selected_vertices().size(), 8, "vertex mode selects the new vertices")
	check(U.vertex_at(cube, Vector3(1, 1.0 / 3.0, 1)) >= 0 and U.vertex_at(cube, Vector3(-1, -1.0 / 3.0, -1)) >= 0, "two cuts are evenly spaced")
	check(U.closed(cube), "two loop cuts: closed")

	var grid := U.grid(3, 3, 3.0)
	grid.set_select_mode(E)
	grid.loop_cut(grid.find_edge(0, 1), 1)
	_expect(grid, Vector3i(20, 31, 12), "loop cut grid")
	check_eq(grid.get_selected_edges().size(), 3, "the cut crosses three quads")
	check(U.vertex_at(grid, Vector3(-1, 0, 0.5)) >= 0, "the cut runs through the edge midpoints")


func _test_delete() -> void:
	var cube := U.cube()
	cube.select_vertex(0)
	check(cube.delete_selection(TetraEditMesh.DELETE_VERTICES), "delete vertices")
	_expect(cube, Vector3i(7, 9, 3), "deleted vertex")

	cube = U.cube()
	cube.set_select_mode(E)
	cube.select_edge(0)
	cube.delete_selection(TetraEditMesh.DELETE_EDGES)
	_expect(cube, Vector3i(8, 11, 4), "deleted edge")

	cube = _cube_with(F, [UP])
	cube.delete_selection(TetraEditMesh.DELETE_FACES)
	_expect(cube, Vector3i(8, 12, 5), "deleted face")
	check_eq(cube.get_selected_vertices().size(), 0, "delete clears the selection")

	cube = _cube_with(F, [UP, Vector3(1, 0, 0)])
	cube.delete_selection(TetraEditMesh.DELETE_FACES)
	_expect(cube, Vector3i(8, 11, 4), "deleted faces take the edge between them")

	cube = _cube_with(F, [UP, Vector3(1, 0, 0)])
	cube.delete_selection(TetraEditMesh.DELETE_ONLY_FACES)
	_expect(cube, Vector3i(8, 12, 4), "only faces")

	cube = U.cube()
	cube.set_select_mode(E)
	cube.select_edge(0)
	cube.delete_selection(TetraEditMesh.DELETE_ONLY_EDGES_AND_FACES)
	_expect(cube, Vector3i(8, 11, 4), "only edges and faces")

	var plane := U.plane()
	plane.set_select_mode(F)
	plane.select_face(0)
	plane.delete_selection(TetraEditMesh.DELETE_FACES)
	_expect(plane, Vector3i(0, 0, 0), "deleting the only face leaves nothing")

	plane = U.plane()
	plane.set_select_mode(E)
	plane.select_edge(0)
	plane.delete_selection(TetraEditMesh.DELETE_EDGES)
	_expect(plane, Vector3i(4, 3, 0), "deleting an edge keeps vertices that have other edges")
	check(not U.cube().delete_selection(TetraEditMesh.DELETE_VERTICES), "nothing selected, nothing deleted")


func _test_dissolve() -> void:
	var grid := U.grid(2, 2, 2.0)
	grid.select_vertex(4)
	check(grid.dissolve_vertices(), "dissolve vertex")
	_expect(grid, Vector3i(8, 8, 1), "dissolved centre vertex")
	check_eq(grid.get_face_vertices(0).size(), 8, "the faces around it became one octagon")
	check_eq(grid.get_face_normal(0), UP, "the octagon keeps the winding")

	grid = U.grid(2, 2, 2.0)
	grid.set_select_mode(F)
	grid.select_all()
	check(grid.dissolve_faces(), "dissolve faces")
	_expect(grid, Vector3i(8, 8, 1), "dissolved faces")
	check_eq(grid.get_selected_faces().size(), 1, "the joined face is selected")

	var cube := _cube_with(F, [UP, Vector3(1, 0, 0)])
	cube.dissolve_faces()
	_expect(cube, Vector3i(8, 11, 5), "two cube faces dissolved into a hexagon")
	check(U.closed(cube), "still closed")

	# Dissolving an edge also dissolves the vertices left between two edges.
	cube = U.cube()
	cube.set_select_mode(E)
	cube.select_edge(cube.find_edge(U.vertex_at(cube, Vector3(1, 1, 1)), U.vertex_at(cube, Vector3(1, 1, -1))))
	check(cube.dissolve_edges(), "dissolve edge")
	_expect(cube, Vector3i(6, 9, 5), "dissolved cube edge")
	check(U.closed(cube), "dissolved cube edge: closed")
	check_near(U.volume(cube), 4.0, 1e-4, "the prism that is left")

	grid = U.grid(2, 2, 2.0)
	grid.set_select_mode(E)
	grid.select_edge(grid.find_edge(1, 4))
	grid.dissolve_edges()
	_expect(grid, Vector3i(8, 10, 3), "dissolved inner grid edge")

	# A vertex between two edges.
	var plane := U.plane()
	plane.set_select_mode(E)
	plane.select_edge(plane.find_edge(0, 1))
	plane.subdivide(1)
	plane.set_select_mode(V)
	plane.select_none()
	plane.select_vertex(4)
	check(plane.dissolve_vertices(), "dissolve a vertex between two edges")
	_expect(plane, Vector3i(4, 4, 1), "back to a quad")
	check(not U.cube().dissolve_vertices(), "nothing selected, nothing dissolved")


func _test_merge() -> void:
	var cube := _cube_with(F, [UP])
	check(cube.merge_at_center(), "merge at centre")
	_expect(cube, Vector3i(5, 8, 5), "cube with the top merged: a pyramid")
	check(U.vertex_at(cube, UP) >= 0, "the merged vertex is at the centre")
	check(U.closed(cube), "the pyramid is closed")
	check_near(U.volume(cube), 4.0 + 4.0 * 2.0 / 3.0 - 4.0, 1e-4, "pyramid volume")

	cube = U.cube()
	cube.set_select_mode(E)
	cube.select_edge(cube.find_edge(U.vertex_at(cube, Vector3(1, 1, 1)), U.vertex_at(cube, Vector3(1, 1, -1))))
	check(cube.merge_collapse(), "collapse")
	_expect(cube, Vector3i(7, 11, 6), "collapsed edge")
	check(U.vertex_at(cube, Vector3(1, 1, 0)) >= 0, "the edge collapsed to its middle")
	check(U.closed(cube), "collapsed edge: closed")

	# Collapse keeps separate islands apart.
	cube = U.cube()
	cube.set_select_mode(E)
	cube.select_edge(cube.find_edge(U.vertex_at(cube, Vector3(1, 1, 1)), U.vertex_at(cube, Vector3(1, 1, -1))))
	cube.select_edge(cube.find_edge(U.vertex_at(cube, Vector3(-1, -1, 1)), U.vertex_at(cube, Vector3(-1, -1, -1))))
	cube.merge_collapse()
	_expect(cube, Vector3i(6, 10, 6), "two collapsed edges")
	check(U.vertex_at(cube, Vector3(1, 1, 0)) >= 0 and U.vertex_at(cube, Vector3(-1, -1, 0)) >= 0, "each island has its own centre")

	# Merge by distance.
	var two := TetraEditMesh.new()
	two.add_cube()
	two.add_cube()
	two.select_all()
	check_eq(two.merge_by_distance(0.001), 8, "eight doubles removed")
	_expect(two, Vector3i(8, 12, 6), "two coincident cubes merge into one")

	var planes := TetraEditMesh.new()
	planes.add_plane()
	planes.add_plane()
	planes.translate_selection(Vector3(2, 0, 0))
	planes.select_all()
	check_eq(planes.merge_by_distance(0.001), 2, "two doubles on the shared edge")
	_expect(planes, Vector3i(6, 7, 2), "two planes welded along an edge")
	check_eq(planes.merge_by_distance(0.001), 0, "nothing left to merge")
	planes.select_none()
	planes.select_vertex(0)
	planes.select_vertex(1)
	check_eq(planes.merge_by_distance(0.5), 0, "distant vertices stay apart")
	check_eq(planes.merge_by_distance(2.5), 1, "a larger distance merges them")
	check_eq(planes.validate(), "", "merged planes validate")


func _test_fill_and_bridge() -> void:
	# Fill a hole from its edge loop; the face winds like its neighbours.
	var cube := _cube_with(F, [UP])
	cube.delete_selection(TetraEditMesh.DELETE_ONLY_FACES)
	cube.set_select_mode(V)
	for v in cube.get_vertex_count():
		if cube.get_vertex(v).y > 0:
			cube.select_vertex(v)
	check(cube.fill(), "fill")
	_expect(cube, Vector3i(8, 12, 6), "filled cube")
	check(U.closed(cube), "the new face is wound outwards")
	check_near(U.volume(cube), 8.0, 1e-4, "filled cube volume")
	check(not cube.fill(), "filling twice does not duplicate the face")

	var loose := TetraEditMesh.new()
	loose.add_vertex(Vector3(0, 0, 0))
	loose.add_vertex(Vector3(1, 0, 0))
	loose.select_all()
	check(loose.fill(), "two vertices make an edge")
	_expect(loose, Vector3i(2, 1, 0), "edge from two vertices")
	loose.add_vertex(Vector3(1, 0, 1))
	loose.add_vertex(Vector3(0, 0, 1))
	loose.select_all()
	check(loose.fill(), "loose vertices make a face")
	_expect(loose, Vector3i(4, 4, 1), "face from four vertices")
	check_near(absf(loose.get_face_normal(0).y), 1.0, 1e-5, "the face goes around the vertices")

	# Bridge two circles into a tube.
	var tube := TetraEditMesh.new()
	tube.add_circle(8, 1.0, TetraEditMesh.FILL_NOTHING)
	tube.translate_selection(Vector3(0, 2, 0))
	tube.add_circle(8, 1.0, TetraEditMesh.FILL_NOTHING)
	tube.select_all()
	check(tube.bridge_edge_loops(), "bridge edge loops")
	_expect(tube, Vector3i(16, 24, 8), "bridged tube")
	var upright := true
	for f in tube.get_face_count():
		upright = upright and is_zero_approx(tube.get_face_normal(f).y)
	check(upright, "the rungs connect the nearest vertices")
	check(not U.cube().bridge_edge_loops(), "bridge needs two loops")


func _test_normals() -> void:
	var cube := _cube_with(F, [UP])
	check(cube.flip_normals(), "flip")
	check_eq(cube.get_face_normal(cube.get_selected_faces()[0]), Vector3(0, -1, 0), "the face is reversed")
	check_eq(cube.get_face_uvs(cube.get_selected_faces()[0]).size(), 4, "UVs follow")
	check(not U.closed(cube), "one flipped face breaks the winding")
	cube.select_face(U.face_with_normal(cube, Vector3(1, 0, 0)))
	cube.flip_normals()
	cube.select_all()
	check(cube.recalculate_normals(), "recalculate outside")
	check(U.closed(cube), "the winding is consistent again")
	check_near(U.volume(cube), 8.0, 1e-4, "the faces point outwards")
	cube.recalculate_normals(true)
	check_near(U.volume(cube), -8.0, 1e-4, "recalculate inside")
	check_eq(cube.validate(), "", "recalculated cube validates")

	# Two islands are fixed independently.
	var two := TetraEditMesh.new()
	two.add_cube()
	two.set_select_mode(F)
	two.flip_normals()
	two.add_ico_sphere(1, 1.0)
	two.translate_selection(Vector3(5, 0, 0))
	two.select_face(9)
	two.select_face(9, false)
	two.select_all()
	two.select_face(8, false)
	two.select_all()
	two.recalculate_normals()
	check(U.closed(two), "both islands are consistent")

	check(cube.shade_smooth(true), "shade smooth")
	check(cube.is_face_smooth(0) and cube.is_face_smooth(5), "every selected face is smooth")
	cube.select_none()
	cube.select_face(0)
	cube.shade_smooth(false)
	check(not cube.is_face_smooth(0) and cube.is_face_smooth(5), "shade flat on the selection only")


func _test_duplicate_separate() -> void:
	var cube := _cube_with(F, [UP])
	check(cube.duplicate_selection(), "duplicate")
	_expect(cube, Vector3i(12, 16, 7), "duplicated face")
	check_eq(cube.get_selected_vertices(), PackedInt32Array([8, 9, 10, 11]), "the copy is selected")
	check_eq(cube.get_selected_faces(), PackedInt32Array([6]), "the copied face is selected")
	cube.translate_selection(UP)
	check_eq(cube.get_face_center(6), Vector3(0, 2, 0), "the copy moves alone")
	check_eq(cube.get_face_uvs(6), cube.get_face_uvs(5), "the copy keeps UVs")

	cube = _cube_with(F, [UP])
	var part := cube.separate_selection()
	check(part != null, "separate")
	_expect(part, Vector3i(4, 4, 1), "the separated face")
	_expect(cube, Vector3i(8, 12, 5), "what is left")
	check_eq(part.get_face_normal(0), UP, "the separated face keeps its place")

	cube = U.cube()
	cube.select_all()
	part = cube.separate_selection()
	_expect(part, Vector3i(8, 12, 6), "everything separated")
	_expect(cube, Vector3i(0, 0, 0), "nothing left")
	check(U.cube().separate_selection() == null, "nothing selected, nothing separated")


func _test_triangulate() -> void:
	var cube := U.cube()
	cube.select_all()
	check(cube.triangulate(), "triangulate")
	_expect(cube, Vector3i(8, 18, 12), "triangulated cube")
	check(U.closed(cube), "triangulated cube is closed")
	check_near(U.volume(cube), 8.0, 1e-4, "triangulated cube volume")
	check_eq(cube.tris_to_quads(), 6, "tris to quads joins six pairs")
	_expect(cube, Vector3i(8, 12, 6), "quads again")
	check(U.closed(cube), "quads again: closed")
	check_eq(cube.get_selected_faces().size(), 6, "the quads are selected")

	var circle := TetraEditMesh.new()
	circle.add_circle(8, 1.0, TetraEditMesh.FILL_NGON)
	circle.triangulate()
	_expect(circle, Vector3i(8, 13, 6), "triangulated octagon")
	var up := true
	for f in circle.get_face_count():
		up = up and circle.get_face_normal(f).is_equal_approx(UP)
	check(up, "the triangles keep the winding")

	# Only the selection is triangulated, and non-coplanar pairs are not joined
	# beyond the angle limit.
	cube = _cube_with(F, [UP])
	cube.triangulate()
	_expect(cube, Vector3i(8, 13, 7), "one triangulated face")
	var ico := TetraEditMesh.new()
	ico.add_ico_sphere(1, 1.0)
	check_eq(ico.tris_to_quads(0.1, 0.1), 0, "angled triangles stay under a small limit")
	check(ico.tris_to_quads(PI, PI) > 0, "and join under a large one")
	check_eq(ico.validate(), "", "joined icosahedron validates")
