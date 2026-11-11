#include "register_types.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_assets.h"
#include "tetra_video_project.h"
#include "tetra_video_renderer.h"

#include "core/object/class_db.h"

#if __has_include("tetra/tetra_engine_registry.h")
#include "tetra/tetra_engine_registry.h"
#define TETRA_HAS_ENGINE_REGISTRY
#endif

void initialize_tetra_video_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(TetraVideoProject);
		GDREGISTER_CLASS(TetraMltEngine);
		GDREGISTER_CLASS(TetraVideoRenderer);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoAssets);
	}
#ifdef TETRA_HAS_ENGINE_REGISTRY
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Announces MLT to the list of engines the editor shows. Counting the
		// services starts MLT, which is why this waits for the editor level.
		if (TetraMltEngine::is_available()) {
			const int services = TetraMltEngine::list_services("producer").size() + TetraMltEngine::list_services("filter").size() +
					TetraMltEngine::list_services("transition").size() + TetraMltEngine::list_services("consumer").size();
			int kdenlive_effects = 0;
			const TypedArray<Dictionary> effects = TetraVideoAssets::get_effects(true);
			for (int i = 0; i < effects.size(); i++) {
				kdenlive_effects += String(Dictionary(effects[i])["source"]) == "kdenlive" ? 1 : 0;
			}
			TetraEngineRegistry::add_engine("MLT", TetraMltEngine::get_version(), true, vformat("%d services, %d Kdenlive effects", services, kdenlive_effects));
		} else {
#ifdef TETRA_MLT_ENABLED
			TetraEngineRegistry::add_engine("MLT", String(), false, "MLT could not be started: its plug-in repository was not found.");
#else
			TetraEngineRegistry::add_engine("MLT", String(), false, "Built without libmlt++-7 (install libmlt++-dev and rebuild).");
#endif
		}
	}
#endif
}

void uninitialize_tetra_video_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraVideoAssets::clear_cache();
	}
}
