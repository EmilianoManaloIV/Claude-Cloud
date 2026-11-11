#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

// The bridge to MLT, the engine that plays and renders Kdenlive's timelines.
// Built without libmlt++ every method still exists, is_available() is false
// and every call fails cleanly.
class TetraMltEngine : public RefCounted {
	GDCLASS(TetraMltEngine, RefCounted);

protected:
	static void _bind_methods();

public:
	static bool is_available();
	static String get_version();
	static PackedStringArray list_services(const String &p_kind);
	static bool has_service(const String &p_kind, const String &p_name);
	static String get_preferred_service(const String &p_kind, const String &p_candidates);
};
