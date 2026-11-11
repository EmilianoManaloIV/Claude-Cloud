#include "register_types.h"

#include "tetra_engine_registry.h"
#include "tetra_schema.h"

#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/plugins/editor_plugin.h"
#include "editor/tetra_canvas_view.h"
#include "editor/tetra_editor_plugin.h"
#include "editor/tetra_operation_browser.h"
#include "editor/tetra_operation_dialog.h"
#include "editor/tetra_param_panel.h"
#endif

void initialize_tetra_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_ABSTRACT_CLASS(TetraSchema);
		GDREGISTER_ABSTRACT_CLASS(TetraEngineRegistry);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(TetraParamPanel);
		GDREGISTER_CLASS(TetraOperationBrowser);
		GDREGISTER_CLASS(TetraOperationDialog);
		GDREGISTER_CLASS(TetraCanvasView);
		// Created by the editor only; scripts reach it through get_singleton().
		GDREGISTER_ABSTRACT_CLASS(TetraEditorPlugin);
		EditorPlugins::add_by_type<TetraEditorPlugin>();
	}
#endif
}

void uninitialize_tetra_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraEngineRegistry::clear();
	}
}
