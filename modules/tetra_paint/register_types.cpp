#include "register_types.h"

#include "tetra_image_document.h"
#include "tetra_layer_modes.h"
#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

void initialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraLayerModes::initialize();
		tetra_paint_utils_initialize();
		GDREGISTER_CLASS(TetraImageDocument);
	}
}

void uninitialize_tetra_paint_module(ModuleInitializationLevel p_level) {
}
