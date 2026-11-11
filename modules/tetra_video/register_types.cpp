#include "register_types.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_assets.h"
#include "tetra_video_project.h"

#include "core/object/class_db.h"

void initialize_tetra_video_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(TetraVideoProject);
		GDREGISTER_CLASS(TetraMltEngine);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoAssets);
	}
}

void uninitialize_tetra_video_module(ModuleInitializationLevel p_level) {
}
