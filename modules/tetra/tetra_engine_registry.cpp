#include "tetra_engine_registry.h"

#include "core/os/mutex.h"
#include "core/templates/vector.h"

namespace {

struct EngineEntry {
	String name;
	String version;
	String details;
	bool available = false;
};

// Allocated on first use and freed by clear(), so that no Godot type outlives the engine's own shutdown.
Vector<EngineEntry> *engines = nullptr;
Mutex engines_mutex;

Dictionary entry_to_dictionary(const EngineEntry &p_entry) {
	Dictionary engine;
	engine["name"] = p_entry.name;
	engine["version"] = p_entry.version;
	engine["available"] = p_entry.available;
	engine["details"] = p_entry.details;
	return engine;
}

} // namespace

void TetraEngineRegistry::add_engine(const String &p_name, const String &p_version, bool p_available, const String &p_details) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "An engine needs a name.");

	EngineEntry entry;
	entry.name = p_name;
	entry.version = p_version;
	entry.available = p_available;
	entry.details = p_details;

	MutexLock lock(engines_mutex);
	if (!engines) {
		engines = memnew(Vector<EngineEntry>);
	}
	for (int i = 0; i < engines->size(); i++) {
		if (engines->get(i).name.nocasecmp_to(p_name) == 0) {
			engines->set(i, entry);
			return;
		}
	}
	engines->push_back(entry);
}

TypedArray<Dictionary> TetraEngineRegistry::get_engines() {
	TypedArray<Dictionary> result;
	MutexLock lock(engines_mutex);
	if (engines) {
		for (const EngineEntry &entry : *engines) {
			result.push_back(entry_to_dictionary(entry));
		}
	}
	return result;
}

Dictionary TetraEngineRegistry::get_engine(const String &p_name) {
	MutexLock lock(engines_mutex);
	if (engines) {
		for (const EngineEntry &entry : *engines) {
			if (entry.name.nocasecmp_to(p_name) == 0) {
				return entry_to_dictionary(entry);
			}
		}
	}
	return Dictionary();
}

bool TetraEngineRegistry::is_engine_available(const String &p_name) {
	return bool(get_engine(p_name).get("available", false));
}

void TetraEngineRegistry::clear() {
	MutexLock lock(engines_mutex);
	if (engines) {
		memdelete(engines);
		engines = nullptr;
	}
}

void TetraEngineRegistry::_bind_methods() {
	const StringName class_name = SNAME("TetraEngineRegistry");
	ClassDB::bind_static_method(class_name, D_METHOD("add_engine", "name", "version", "available", "details"), &TetraEngineRegistry::add_engine, DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("get_engines"), &TetraEngineRegistry::get_engines);
	ClassDB::bind_static_method(class_name, D_METHOD("get_engine", "name"), &TetraEngineRegistry::get_engine);
	ClassDB::bind_static_method(class_name, D_METHOD("is_engine_available", "name"), &TetraEngineRegistry::is_engine_available);
}
