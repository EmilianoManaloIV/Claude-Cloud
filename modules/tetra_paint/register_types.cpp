#include "register_types.h"

#include "gegl/tetra_gegl.h"
#include "tetra_image_document.h"
#include "tetra_layer_modes.h"
#include "tetra_ora_loader.h"
#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

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
}

void uninitialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ImageLoader::remove_image_format_loader(ora_loader);
		ora_loader.unref();
		TetraGegl::shutdown();
	}
}
