#include "register_types.h"

#include "tetra_blender_bridge.h"
#include "tetra_blender_mesh.h"
#include "tetra_blender_scene.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/tetra_blender_dock.h"
#include "editor/tetra_blender_editor_plugin.h"
#include "editor/tetra_blender_modifiers_tab.h"
#include "editor/tetra_blender_operators_tab.h"
#include "editor/tetra_blender_render_tab.h"

#include "editor/editor_node.h"
#include "editor/plugins/editor_plugin.h"
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
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderScene);
		GLOBAL_DEF(PropertyInfo(Variant::STRING, "tetra/blender/python_path", PROPERTY_HINT_GLOBAL_FILE), "");
		TetraBlenderBridge::update_engine_registry();
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback(_editor_init);
		// The editor creates these; scripts reach them through
		// TetraBlenderEditorPlugin.get_singleton().
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderEditorPlugin);
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderDock);
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderModifiersTab);
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderOperatorsTab);
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderRenderTab);
		GDREGISTER_ABSTRACT_CLASS(TetraBlenderRenderView);
		EditorPlugins::add_by_type<TetraBlenderEditorPlugin>();
	}
#endif
}

void uninitialize_tetra_model_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraBlenderBridge::free_shared();
	}
}
