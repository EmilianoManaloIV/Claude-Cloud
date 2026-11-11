#include "register_types.h"

#include "tetra_blender_bridge.h"
#include "tetra_blender_mesh.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"

// Editor settings do not exist yet when modules are initialized.
static void _editor_init() {
	EDITOR_DEF("tetra/blender/python_path", "");
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::STRING, "tetra/blender/python_path", PROPERTY_HINT_GLOBAL_FILE));
	EDITOR_DEF("tetra/blender/executable_path", "");
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::STRING, "tetra/blender/executable_path", PROPERTY_HINT_GLOBAL_FILE));
	// The settings may name a Blender that the first look could not see.
	TetraBlenderBridge::update_engine_registry();
}
#endif

void initialize_tetra_model_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderMesh);
		GDREGISTER_CLASS(TetraBlenderBridge);
		GLOBAL_DEF(PropertyInfo(Variant::STRING, "tetra/blender/python_path", PROPERTY_HINT_GLOBAL_FILE), "");
		TetraBlenderBridge::update_engine_registry();
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback(_editor_init);
	}
#endif
}

void uninitialize_tetra_model_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraBlenderBridge::free_shared();
	}
}
