#include "register_types.h"

#include "gegl/tetra_gegl.h"
#include "tetra_image_document.h"
#include "tetra_layer_modes.h"
#include "tetra_ora_loader.h"
#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/plugins/editor_plugin.h"
#include "editor/tetra_image_canvas.h"
#include "editor/tetra_image_color_swatch.h"
#include "editor/tetra_image_editor_plugin.h"
#include "editor/tetra_image_layers_panel.h"
#include "editor/tetra_image_workspace.h"
#endif

static Ref<TetraImageLoaderORA> ora_loader;

void initialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraLayerModes::initialize();
		tetra_paint_utils_initialize();
		GDREGISTER_CLASS(TetraImageDocument);
		GDREGISTER_ABSTRACT_CLASS(TetraGegl);
		TetraGegl::register_engine();
		ora_loader.instantiate();
		ImageLoader::add_image_format_loader(ora_loader);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(TetraImageCanvas);
		GDREGISTER_CLASS(TetraImageColorSwatch);
		GDREGISTER_CLASS(TetraImageLayersPanel);
		// One workspace exists, created by the plugin; scripts reach it
		// through TetraImageWorkspace.get_singleton().
		GDREGISTER_ABSTRACT_CLASS(TetraImageWorkspace);
		GDREGISTER_ABSTRACT_CLASS(TetraImageEditorPlugin);
		EditorPlugins::add_by_type<TetraImageEditorPlugin>();
	}
#endif
}

void uninitialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ImageLoader::remove_image_format_loader(ora_loader);
		ora_loader.unref();
		TetraGegl::shutdown();
	}
}
