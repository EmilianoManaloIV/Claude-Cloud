#include "register_types.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_assets.h"
#include "tetra_video_file.h"
#include "tetra_video_project.h"
#include "tetra_video_renderer.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/plugins/editor_plugin.h"
#include "editor/tetra_video_bin.h"
#include "editor/tetra_video_editor_plugin.h"
#include "editor/tetra_video_effect_stack.h"
#include "editor/tetra_video_keyframe_strip.h"
#include "editor/tetra_video_monitor.h"
#include "editor/tetra_video_render_dialog.h"
#include "editor/tetra_video_timeline.h"
#include "editor/tetra_video_workspace.h"
#endif

static Ref<TetraVideoFileLoader> video_file_loader;

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
		// Makes .mlt projects and MLT's video formats files the FileSystem dock lists and the Video workspace opens.
		GDREGISTER_CLASS(TetraVideoProjectFile);
		GDREGISTER_CLASS(TetraVideoMediaFile);
		video_file_loader.instantiate();
		ResourceLoader::add_resource_format_loader(video_file_loader);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		// Created by the editor only; scripts reach them through TetraVideoWorkspace.get_singleton().
		GDREGISTER_ABSTRACT_CLASS(TetraVideoBin);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoMonitor);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoTimeline);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoEffectStack);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoKeyframeStrip);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoRenderDialog);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoWorkspace);
		GDREGISTER_ABSTRACT_CLASS(TetraVideoEditorPlugin);
		EditorPlugins::add_by_type<TetraVideoEditorPlugin>();
	}
#endif
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
		ResourceLoader::remove_resource_format_loader(video_file_loader);
		video_file_loader.unref();
	}
}
