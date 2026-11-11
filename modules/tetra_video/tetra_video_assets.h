#pragma once

#include "core/object/object.h"
#include "core/variant/dictionary.h"

// Kdenlive's effect and transition catalog as operation schemas, and the
// conversion of parameter values to MLT properties.
class TetraVideoAssets : public Object {
	GDCLASS(TetraVideoAssets, Object);

protected:
	static void _bind_methods();

public:
	static Dictionary effect_to_mlt_properties(const String &p_operation_id, const Dictionary &p_values, int p_width, int p_height, int p_length);
};
