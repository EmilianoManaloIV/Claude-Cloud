#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

// The list of upstream engines the running editor was built with and whether
// each one works. Every Tetra module announces its engine from its
// register_types (GEGL, MLT, Blender); the About dialog and the workspaces
// read the list to tell the user what is available.
class TetraEngineRegistry : public Object {
	GDCLASS(TetraEngineRegistry, Object);

protected:
	static void _bind_methods();

public:
	// Announces an engine, or replaces the entry of the same name. p_details is a
	// one-line explanation: where the engine was found, or why it is unavailable.
	static void add_engine(const String &p_name, const String &p_version, bool p_available, const String &p_details = String());
	// One {name, version, available, details} Dictionary per engine, in the order they were first added.
	static TypedArray<Dictionary> get_engines();
	// The entry called p_name (case-insensitive), or an empty Dictionary.
	static Dictionary get_engine(const String &p_name);
	static bool is_engine_available(const String &p_name);

	// Releases the list; called when the module is unloaded.
	static void clear();
};
