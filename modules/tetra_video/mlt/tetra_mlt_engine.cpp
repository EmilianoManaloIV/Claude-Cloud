#include "tetra_mlt_engine.h"

#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"

#ifdef TETRA_MLT_ENABLED

#include <Mlt.h>
#include <framework/mlt_log.h>
#include <framework/mlt_version.h>

#include <clocale>

namespace {

// Every call into MLT, from any engine instance or thread, holds this mutex.
Mutex mlt_mutex;
Mlt::Repository *repository = nullptr;
bool init_attempted = false;
HashMap<String, PackedStringArray> service_lists;
HashMap<String, HashSet<String>> service_sets;

void _collect_services(const String &p_kind, Mlt::Properties *p_services) {
	PackedStringArray names;
	HashSet<String> set;
	if (p_services) {
		for (int i = 0; i < p_services->count(); i++) {
			const String name = String::utf8(p_services->get_name(i));
			names.push_back(name);
			set.insert(name);
		}
		delete p_services;
	}
	names.sort();
	service_lists[p_kind] = names;
	service_sets[p_kind] = set;
}

// Starts MLT once. Its plug-ins may switch the process locale to the user's,
// which would make both MLT and Godot print decimal commas, so the numeric
// locale is pinned back to "C" afterwards.
bool _ensure_mlt() {
	MutexLock lock(mlt_mutex);
	if (!init_attempted) {
		init_attempted = true;
		mlt_log_set_level(MLT_LOG_ERROR);
		repository = Mlt::Factory::init();
		setlocale(LC_NUMERIC, "C");
		if (repository) {
			_collect_services("producer", repository->producers());
			_collect_services("filter", repository->filters());
			_collect_services("transition", repository->transitions());
			_collect_services("consumer", repository->consumers());
		}
	}
	return repository != nullptr;
}

} // namespace

bool TetraMltEngine::is_available() {
	return _ensure_mlt();
}

String TetraMltEngine::get_version() {
	return _ensure_mlt() ? String::utf8(mlt_version_get_string()) : String();
}

PackedStringArray TetraMltEngine::list_services(const String &p_kind) {
	if (!_ensure_mlt() || !service_lists.has(p_kind)) {
		return PackedStringArray();
	}
	return service_lists[p_kind];
}

bool TetraMltEngine::has_service(const String &p_kind, const String &p_name) {
	return _ensure_mlt() && service_sets.has(p_kind) && service_sets[p_kind].has(p_name);
}

#else // TETRA_MLT_ENABLED

bool TetraMltEngine::is_available() {
	return false;
}

String TetraMltEngine::get_version() {
	return String();
}

PackedStringArray TetraMltEngine::list_services(const String &p_kind) {
	return PackedStringArray();
}

bool TetraMltEngine::has_service(const String &p_kind, const String &p_name) {
	return false;
}

#endif // TETRA_MLT_ENABLED

String TetraMltEngine::get_preferred_service(const String &p_kind, const String &p_candidates) {
	const PackedStringArray candidates = p_candidates.split(",", false);
	for (const String &candidate : candidates) {
		if (has_service(p_kind, candidate)) {
			return candidate;
		}
	}
	return candidates.is_empty() ? String() : candidates[0];
}

void TetraMltEngine::_bind_methods() {
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("is_available"), &TetraMltEngine::is_available);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_version"), &TetraMltEngine::get_version);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("list_services", "kind"), &TetraMltEngine::list_services);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("has_service", "kind", "name"), &TetraMltEngine::has_service);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_preferred_service", "kind", "candidates"), &TetraMltEngine::get_preferred_service);
}
