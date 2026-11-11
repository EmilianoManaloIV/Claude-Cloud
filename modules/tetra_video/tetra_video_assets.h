#pragma once

#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

// Kdenlive's effect and transition catalog as operation schemas.
//
// Kdenlive describes each effect in a small XML file: the MLT service it
// drives, the parameters to show and how a displayed value maps to the MLT
// property (a factor, a colour notation, on/off strings...). This class
// reads those files into the operation schema shared by all Tetra bridges,
// lists the ones whose MLT service is installed, and converts panel values
// back to MLT properties.
//
// Besides the schema keys an operation carries "service" (the MLT service),
// "kind" ("video_effect", "audio_effect" or "transition"), "available" and
// "fixed" (properties that are always sent and never shown). Numeric
// parameters are expressed in display units; "factor" divides them on the
// way to MLT.
class TetraVideoAssets : public Object {
	GDCLASS(TetraVideoAssets, Object);

protected:
	static void _bind_methods();

public:
	static String get_data_dir();
	static TypedArray<Dictionary> get_effects(bool p_available_only = true, int p_width = 1920, int p_height = 1080);
	static TypedArray<Dictionary> get_transitions(bool p_available_only = true, int p_width = 1920, int p_height = 1080);
	static Dictionary get_operation(const String &p_id, int p_width = 1920, int p_height = 1080);
	static Dictionary get_default_values(const Dictionary &p_operation);
	static Dictionary values_to_mlt_properties(const Dictionary &p_operation, const Dictionary &p_values);
	static Dictionary effect_to_mlt_properties(const String &p_operation_id, const Dictionary &p_values, int p_width, int p_height, int p_length);
	static void clear_cache();
};
