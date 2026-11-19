extends RefCounted
## Helpers shared by the TetraEditMesh tests.


static func counts(p_mesh: TetraEditMesh) -> Vector3i:
	return Vector3i(p_mesh.get_vertex_count(), p_mesh.get_edge_count(), p_mesh.get_face_count())


## Six times the signed volume; positive when the faces point outwards.
static func volume(p_mesh: TetraEditMesh) -> float:
	var total := 0.0
	for f in p_mesh.get_face_count():
		var loop := p_mesh.get_face_vertices(f)
		var first := p_mesh.get_vertex(loop[0])
		for i in range(1, loop.size() - 1):
			total += first.dot(p_mesh.get_vertex(loop[i]).cross(p_mesh.get_vertex(loop[i + 1])))
	return total / 6.0


static func _directed(p_loop: PackedInt32Array, p_a: int, p_b: int) -> bool:
	for i in p_loop.size():
		if p_loop[i] == p_a and p_loop[(i + 1) % p_loop.size()] == p_b:
			return true
	return false


## True when every edge has exactly two faces that run along it in opposite
## directions: a closed surface with consistent normals.
static func closed(p_mesh: TetraEditMesh) -> bool:
	var uses := {}
	for f in p_mesh.get_face_count():
		var loop := p_mesh.get_face_vertices(f)
		for i in loop.size():
			var key := Vector2i(loop[i], loop[(i + 1) % loop.size()])
			if uses.has(key):
				return false
			uses[key] = true
	for e in p_mesh.get_edge_count():
		var edge := p_mesh.get_edge_vertices(e)
		if not uses.has(edge) or not uses.has(Vector2i(edge.y, edge.x)):
			return false
	return true


static func cube() -> TetraEditMesh:
	var mesh := TetraEditMesh.new()
	mesh.add_cube()
	mesh.select_none()
	return mesh


static func grid(p_x: int, p_y: int, p_size: float) -> TetraEditMesh:
	var mesh := TetraEditMesh.new()
	mesh.add_grid(p_x, p_y, p_size)
	mesh.select_none()
	return mesh


static func plane() -> TetraEditMesh:
	var mesh := TetraEditMesh.new()
	mesh.add_plane()
	mesh.select_none()
	return mesh


## The index of the vertex at p_position, or -1.
static func vertex_at(p_mesh: TetraEditMesh, p_position: Vector3) -> int:
	for v in p_mesh.get_vertex_count():
		if p_mesh.get_vertex(v).distance_to(p_position) < 1e-4:
			return v
	return -1


static func face_with_normal(p_mesh: TetraEditMesh, p_normal: Vector3) -> int:
	for f in p_mesh.get_face_count():
		if p_mesh.get_face_normal(f).distance_to(p_normal) < 1e-4:
			return f
	return -1
