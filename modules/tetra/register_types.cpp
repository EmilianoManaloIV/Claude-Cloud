#include "register_types.h"

#include "tetra_engine_registry.h"
#include "tetra_schema.h"

#include "core/object/class_db.h"

void initialize_tetra_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_ABSTRACT_CLASS(TetraSchema);
		GDREGISTER_ABSTRACT_CLASS(TetraEngineRegistry);
	}
}

void uninitialize_tetra_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		TetraEngineRegistry::clear();
	}
}
