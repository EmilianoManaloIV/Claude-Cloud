extends RefCounted
## What the tetra_model screenshot drivers share: waiting for the Blender
## workspace and building small scenes through it.


## Waits until p_condition returns true; false when p_timeout seconds passed first.
static func wait_until(p_tree: SceneTree, p_condition: Callable, p_timeout: float = 120.0) -> bool:
	var waited := 0.0
	while not p_condition.call():
		if waited >= p_timeout:
			return false
		await p_tree.create_timer(0.1).timeout
		waited += 0.1
	return true


## Adds a Blender primitive through the workspace and returns its node.
static func add_primitive(p_tree: SceneTree, p_id: String, p_values: Dictionary = {}, p_parent: Node = null) -> MeshInstance3D:
	var blender := TetraBlenderEditorPlugin.get_singleton()
	var added: Array[MeshInstance3D] = []
	var failed: Array[String] = []
	var on_added := func(node: MeshInstance3D) -> void: added.append(node)
	var on_failed := func(action: String, message: String) -> void: failed.append(action + ": " + message)
	blender.primitive_added.connect(on_added)
	blender.request_failed.connect(on_failed)
	blender.add_primitive(p_id, p_values, p_parent)
	await wait_until(p_tree, func() -> bool: return not added.is_empty() or not failed.is_empty())
	blender.primitive_added.disconnect(on_added)
	blender.request_failed.disconnect(on_failed)
	if added.is_empty():
		printerr("SCRIPT ERROR: could not add %s: %s" % [p_id, failed])
		return null
	return added[0]


## Waits for Blender to finish evaluating the modifiers of p_node.
static func wait_for_modifiers(p_tree: SceneTree, p_node: MeshInstance3D) -> bool:
	var blender := TetraBlenderEditorPlugin.get_singleton()
	# The evaluation is queued a moment after the change.
	await p_tree.create_timer(0.3).timeout
	return await wait_until(p_tree, func() -> bool: return not blender.is_evaluating(p_node))


static func colored_material(p_color: Color, p_roughness: float = 0.5, p_metallic: float = 0.0) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = p_color
	material.roughness = p_roughness
	material.metallic = p_metallic
	return material
