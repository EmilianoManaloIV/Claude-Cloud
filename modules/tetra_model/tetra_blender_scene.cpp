#include "tetra_blender_scene.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/sky.h"

// Godot multiplies a light's energy by pi before shading (so that energy 1
// on a white surface gives white), Blender's sun strength is the irradiance
// in W/m2 and reflects as strength / pi: a sun of strength pi * energy looks
// the same in both.
static constexpr double SUN_STRENGTH_PER_ENERGY = Math::PI;
// A Blender point or spot light of P watts lights a white surface at 1 m
// with P / (4 pi^2); Godot's does with its energy.
static constexpr double POINT_WATTS_PER_ENERGY = 4.0 * Math::PI * Math::PI;

StringName TetraBlenderScene::get_modifiers_meta() {
	return SNAME("tetra_blender_modifiers");
}

StringName TetraBlenderScene::get_source_mesh_meta() {
	return SNAME("tetra_blender_source_mesh");
}

bool TetraBlenderScene::has_modifier_stack(Node *p_node) {
	if (!p_node || !p_node->has_meta(get_modifiers_meta())) {
		return false;
	}
	const Variant stack = p_node->get_meta(get_modifiers_meta());
	return stack.get_type() == Variant::ARRAY && !Array(stack).is_empty();
}

Array TetraBlenderScene::get_modifier_stack(Node *p_node) {
	if (!has_modifier_stack(p_node)) {
		return Array();
	}
	return Array(p_node->get_meta(get_modifiers_meta())).duplicate(true);
}

Ref<Mesh> TetraBlenderScene::get_source_mesh(Node *p_node) {
	if (!p_node || !p_node->has_meta(get_source_mesh_meta())) {
		return Ref<Mesh>();
	}
	return p_node->get_meta(get_source_mesh_meta());
}

Ref<Mesh> TetraBlenderScene::get_base_mesh(MeshInstance3D *p_node) {
	ERR_FAIL_NULL_V(p_node, Ref<Mesh>());
	if (has_modifier_stack(p_node)) {
		const Ref<Mesh> source = get_source_mesh(p_node);
		if (source.is_valid()) {
			return source;
		}
	}
	return p_node->get_mesh();
}

void TetraBlenderScene::set_modifier_stack(Node *p_node, const Ref<Mesh> &p_source, const Array &p_stack) {
	ERR_FAIL_NULL(p_node);
	if (p_stack.is_empty()) {
		p_node->remove_meta(get_modifiers_meta());
		p_node->remove_meta(get_source_mesh_meta());
		return;
	}
	p_node->set_meta(get_modifiers_meta(), p_stack.duplicate(true));
	p_node->set_meta(get_source_mesh_meta(), p_source);
}

Dictionary TetraBlenderScene::make_modifier(const String &p_type, const Dictionary &p_values, const String &p_label) {
	Dictionary modifier;
	modifier["type"] = p_type.trim_prefix("blender:modifier.");
	modifier["label"] = p_label.is_empty() ? String(modifier["type"]).capitalize() : p_label;
	modifier["values"] = p_values.duplicate(true);
	modifier["enabled"] = true;
	modifier["object_paths"] = Dictionary();
	return modifier;
}

Array TetraBlenderScene::to_bridge_stack(MeshInstance3D *p_node, const Array &p_stack) {
	Array stack;
	for (int i = 0; i < p_stack.size(); i++) {
		if (p_stack[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary source = p_stack[i];
		Dictionary entry;
		entry["type"] = String(source.get("type", String())).trim_prefix("blender:modifier.");
		entry["values"] = source.get("values", Dictionary());
		entry["enabled"] = source.get("enabled", true);

		Dictionary objects;
		const Dictionary object_paths = source.get("object_paths", Dictionary());
		for (const KeyValue<Variant, Variant> &kv : object_paths) {
			if (!p_node || !p_node->is_inside_tree()) {
				break;
			}
			MeshInstance3D *other = Object::cast_to<MeshInstance3D>(p_node->get_node_or_null(kv.value));
			if (!other || other == p_node || other->get_mesh().is_null()) {
				continue;
			}
			Dictionary object;
			object["mesh"] = other->get_mesh();
			object["transform"] = p_node->get_global_transform().affine_inverse() * other->get_global_transform();
			objects[kv.key] = object;
		}
		if (!objects.is_empty()) {
			entry["objects"] = objects;
		}
		stack.push_back(entry);
	}
	return stack;
}

Dictionary TetraBlenderScene::material_to_dictionary(const Ref<Material> &p_material) {
	Dictionary description;
	const Ref<BaseMaterial3D> material = p_material;
	if (material.is_null()) {
		description["base_color"] = Color(1, 1, 1);
		description["metallic"] = 0.0;
		description["roughness"] = 1.0;
		description["emission"] = Color(0, 0, 0);
		description["emission_strength"] = 0.0;
		return description;
	}
	description["base_color"] = material->get_albedo();
	description["metallic"] = material->get_metallic();
	description["roughness"] = material->get_roughness();
	const bool emissive = material->get_feature(BaseMaterial3D::FEATURE_EMISSION);
	description["emission"] = emissive ? material->get_emission() : Color(0, 0, 0);
	description["emission_strength"] = emissive ? material->get_emission_energy_multiplier() : 0.0;
	return description;
}

Dictionary TetraBlenderScene::light_to_dictionary(Light3D *p_light) {
	ERR_FAIL_NULL_V(p_light, Dictionary());
	Dictionary light;
	const double energy = p_light->get_param(Light3D::PARAM_ENERGY);
	switch (p_light->get_light_type()) {
		case RSE::LIGHT_DIRECTIONAL: {
			light["type"] = "sun";
			light["energy"] = energy * SUN_STRENGTH_PER_ENERGY;
			// Godot's size of a directional light is its angular diameter.
			light["angle_degrees"] = p_light->get_param(Light3D::PARAM_SIZE);
		} break;
		case RSE::LIGHT_OMNI:
		case RSE::LIGHT_SPOT: {
			const bool spot = p_light->get_light_type() == RSE::LIGHT_SPOT;
			light["type"] = spot ? "spot" : "point";
			// Godot's lights fall off with distance^-attenuation and end at
			// their range; Blender's always with the square. The two are
			// made equally bright halfway to the range, which is where a
			// light is typically used.
			const double reference = MAX(0.01, p_light->get_param(Light3D::PARAM_RANGE) * 0.5);
			const double attenuation = p_light->get_param(Light3D::PARAM_ATTENUATION);
			light["energy"] = energy * POINT_WATTS_PER_ENERGY * Math::pow(reference, 2.0 - attenuation);
			light["radius"] = p_light->get_param(Light3D::PARAM_SIZE);
			if (spot) {
				light["spot_angle_degrees"] = p_light->get_param(Light3D::PARAM_SPOT_ANGLE);
				// A high spot attenuation is a hard edge, a low one a wide soft one.
				light["spot_blend"] = CLAMP(1.0 / (1.0 + p_light->get_param(Light3D::PARAM_SPOT_ATTENUATION)), 0.0, 1.0);
			}
		} break;
		default:
			return Dictionary();
	}
	light["name"] = String(p_light->get_name());
	light["transform"] = p_light->is_inside_tree() ? p_light->get_global_transform() : p_light->get_transform();
	light["color"] = p_light->get_color();
	return light;
}

Dictionary TetraBlenderScene::environment_to_world(const Ref<Environment> &p_environment) {
	Color color = GLOBAL_GET("rendering/environment/defaults/default_clear_color");
	double strength = 1.0;
	if (p_environment.is_valid()) {
		switch (p_environment->get_background()) {
			case Environment::BG_COLOR: {
				color = p_environment->get_bg_color();
				strength = p_environment->get_bg_energy_multiplier();
			} break;
			case Environment::BG_SKY: {
				const Ref<Sky> sky = p_environment->get_sky();
				const Ref<ProceduralSkyMaterial> procedural = sky.is_valid() ? sky->get_material() : Ref<Material>();
				if (procedural.is_valid()) {
					color = procedural->get_sky_top_color().lerp(procedural->get_sky_horizon_color(), 0.5);
					strength = p_environment->get_bg_energy_multiplier() * procedural->get_sky_energy_multiplier() * procedural->get_energy_multiplier();
				}
			} break;
			default:
				break;
		}
	}
	color.a = 1.0;
	Dictionary world;
	world["color"] = color;
	world["strength"] = strength;
	return world;
}

Dictionary TetraBlenderScene::camera_to_dictionary(Camera3D *p_camera, real_t p_aspect) {
	ERR_FAIL_NULL_V(p_camera, Dictionary());
	const double aspect = p_aspect > 0.0 ? double(p_aspect) : 1.0;
	const bool keep_width = p_camera->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH;
	Dictionary camera;
	camera["transform"] = p_camera->is_inside_tree() ? p_camera->get_global_transform() : p_camera->get_transform();
	if (p_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
		// "ortho_size" is the visible height.
		camera["ortho_size"] = keep_width ? p_camera->get_size() / aspect : double(p_camera->get_size());
	} else {
		double fov = p_camera->get_fov();
		if (keep_width) {
			fov = Math::rad_to_deg(2.0 * Math::atan(Math::tan(Math::deg_to_rad(fov) * 0.5) / aspect));
		}
		camera["fov_y_degrees"] = fov;
	}
	camera["near"] = p_camera->get_near();
	camera["far"] = p_camera->get_far();
	return camera;
}

static void _gather_node(Node *p_node, Array &r_meshes, Array &r_lights, Ref<Environment> &r_environment) {
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	if (node_3d && !node_3d->is_visible_in_tree()) {
		// Nothing below an invisible node is drawn either.
		return;
	}
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid() && mesh->get_surface_count() > 0) {
			Dictionary entry;
			entry["mesh"] = mesh;
			entry["name"] = String(mesh_instance->get_name());
			entry["transform"] = mesh_instance->get_global_transform();
			Array materials;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				materials.push_back(TetraBlenderScene::material_to_dictionary(mesh_instance->get_active_material(i)));
			}
			entry["materials"] = materials;
			r_meshes.push_back(entry);
		}
	} else if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		const Dictionary description = TetraBlenderScene::light_to_dictionary(light);
		if (!description.is_empty() && !light->is_editor_only()) {
			r_lights.push_back(description);
		}
	} else if (WorldEnvironment *world_environment = Object::cast_to<WorldEnvironment>(p_node)) {
		if (r_environment.is_null()) {
			r_environment = world_environment->get_environment();
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_gather_node(p_node->get_child(i), r_meshes, r_lights, r_environment);
	}
}

Dictionary TetraBlenderScene::gather_scene(Node *p_root) {
	ERR_FAIL_NULL_V(p_root, Dictionary());
	Array meshes;
	Array lights;
	Ref<Environment> environment;
	_gather_node(p_root, meshes, lights, environment);
	Dictionary scene;
	scene["meshes"] = meshes;
	scene["lights"] = lights;
	scene["world"] = environment_to_world(environment);
	return scene;
}

static void _find_cameras(Node *p_node, TypedArray<Camera3D> &r_cameras) {
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		r_cameras.push_back(camera);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_find_cameras(p_node->get_child(i), r_cameras);
	}
}

TypedArray<Camera3D> TetraBlenderScene::find_cameras(Node *p_root) {
	TypedArray<Camera3D> cameras;
	if (p_root) {
		_find_cameras(p_root, cameras);
	}
	return cameras;
}

Camera3D *TetraBlenderScene::find_scene_camera(Node *p_root) {
	const TypedArray<Camera3D> cameras = find_cameras(p_root);
	for (int i = 0; i < cameras.size(); i++) {
		Camera3D *camera = Object::cast_to<Camera3D>(cameras[i]);
		if (camera->is_current()) {
			return camera;
		}
	}
	return cameras.is_empty() ? nullptr : Object::cast_to<Camera3D>(cameras[0]);
}

Vector2i TetraBlenderScene::fit_resolution(int p_width, int p_height, real_t p_aspect, bool p_match_aspect) {
	Vector2i size(MAX(4, p_width), MAX(4, p_height));
	if (p_match_aspect && p_aspect > 0.0) {
		size.y = MAX(4, int(Math::round(size.x / double(p_aspect))));
	}
	return size;
}

Ref<StandardMaterial3D> TetraBlenderScene::make_default_material() {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_name("Material");
	// Blender's default surface: base colour 0.8, roughness 0.5.
	material->set_albedo(Color(0.8, 0.8, 0.8));
	material->set_roughness(0.5);
	return material;
}

void TetraBlenderScene::_bind_methods() {
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("get_modifiers_meta"), &TetraBlenderScene::get_modifiers_meta);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("get_source_mesh_meta"), &TetraBlenderScene::get_source_mesh_meta);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("has_modifier_stack", "node"), &TetraBlenderScene::has_modifier_stack);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("get_modifier_stack", "node"), &TetraBlenderScene::get_modifier_stack);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("get_source_mesh", "node"), &TetraBlenderScene::get_source_mesh);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("get_base_mesh", "node"), &TetraBlenderScene::get_base_mesh);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("set_modifier_stack", "node", "source", "stack"), &TetraBlenderScene::set_modifier_stack);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("make_modifier", "type", "values", "label"), &TetraBlenderScene::make_modifier, DEFVAL(Dictionary()), DEFVAL(String()));
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("to_bridge_stack", "node", "stack"), &TetraBlenderScene::to_bridge_stack);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("material_to_dictionary", "material"), &TetraBlenderScene::material_to_dictionary);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("light_to_dictionary", "light"), &TetraBlenderScene::light_to_dictionary);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("environment_to_world", "environment"), &TetraBlenderScene::environment_to_world);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("camera_to_dictionary", "camera", "aspect"), &TetraBlenderScene::camera_to_dictionary);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("gather_scene", "root"), &TetraBlenderScene::gather_scene);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("find_scene_camera", "root"), &TetraBlenderScene::find_scene_camera);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("find_cameras", "root"), &TetraBlenderScene::find_cameras);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("fit_resolution", "width", "height", "aspect", "match_aspect"), &TetraBlenderScene::fit_resolution);
	ClassDB::bind_static_method("TetraBlenderScene", D_METHOD("make_default_material"), &TetraBlenderScene::make_default_material);
}
