#pragma once

#include "core/object/object.h"
#include "core/variant/typed_array.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class Camera3D;
class Light3D;
class MeshInstance3D;
class Node;

// What ties a Godot scene to the Blender bridge: the modifier stack a
// MeshInstance3D carries in its metadata, and the description of a scene
// (meshes, materials, lights, world, camera) that TetraBlenderBridge renders.
// Static methods only; everything works without the editor.
//
// A MeshInstance3D with modifiers keeps two metadata entries, which travel
// with the scene file:
//   "tetra_blender_source_mesh"  the Mesh the modifiers start from
//   "tetra_blender_modifiers"    Array of {type: "SUBSURF", label: String,
//                                values: Dictionary, enabled: bool,
//                                object_paths: {param name: NodePath}}
// and its "mesh" property holds the evaluated result.
class TetraBlenderScene : public Object {
	GDCLASS(TetraBlenderScene, Object);

protected:
	static void _bind_methods();

public:
	static StringName get_modifiers_meta();
	static StringName get_source_mesh_meta();

	static bool has_modifier_stack(Node *p_node);
	// A deep copy: changing it does not change the node.
	static Array get_modifier_stack(Node *p_node);
	static Ref<Mesh> get_source_mesh(Node *p_node);
	// The mesh that operators and modifiers start from: the stored source
	// while the node has modifiers, its mesh otherwise.
	static Ref<Mesh> get_base_mesh(MeshInstance3D *p_node);
	// Writes both metadata entries, or removes them when p_stack is empty.
	// The node's mesh is left alone.
	static void set_modifier_stack(Node *p_node, const Ref<Mesh> &p_source, const Array &p_stack);
	// One stack entry with every key present.
	static Dictionary make_modifier(const String &p_type, const Dictionary &p_values = Dictionary(), const String &p_label = String());
	// The stack as TetraBlenderBridge::apply_modifier_stack takes it. Object
	// parameters are resolved from "object_paths" (relative to p_node) to
	// the other MeshInstance3D's mesh and relative transform; a path that
	// leads nowhere is left out.
	static Array to_bridge_stack(MeshInstance3D *p_node, const Array &p_stack);

	// {base_color, metallic, roughness, emission, emission_strength}; a
	// material that is not a BaseMaterial3D (or none) is described like
	// Godot's fallback material: white, rough, not metallic.
	static Dictionary material_to_dictionary(const Ref<Material> &p_material);
	// {type, transform, color, energy, ...} in Blender's units, or an empty
	// Dictionary for a light Blender has no counterpart of.
	static Dictionary light_to_dictionary(Light3D *p_light);
	// {color, strength} of the background a scene with this environment (or
	// none) has: the background colour, an average of a procedural sky, or
	// the project's default clear colour.
	static Dictionary environment_to_world(const Ref<Environment> &p_environment);
	// {transform, fov_y_degrees | ortho_size, near, far}; p_aspect (width /
	// height of the picture) converts a field of view that is kept
	// horizontally.
	static Dictionary camera_to_dictionary(Camera3D *p_camera, real_t p_aspect);
	// {meshes, lights, world} of everything visible below (and including)
	// p_root, ready for TetraBlenderBridge::render_scene once a "camera" is
	// added. Nodes must be inside the tree.
	static Dictionary gather_scene(Node *p_root);
	// The Camera3D below p_root that is marked current, else the first one.
	static Camera3D *find_scene_camera(Node *p_root);
	// The cameras below p_root in tree order.
	static TypedArray<Camera3D> find_cameras(Node *p_root);
	// p_width x p_height, or with p_match_aspect the height that gives
	// p_width the aspect ratio p_aspect. Both sides are at least 4 pixels.
	static Vector2i fit_resolution(int p_width, int p_height, real_t p_aspect, bool p_match_aspect);

	// The grey material new Blender objects start with.
	static Ref<StandardMaterial3D> make_default_material();
};
