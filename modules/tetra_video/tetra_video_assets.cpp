#include "tetra_video_assets.h"

#include "tetra_video_project.h"

#include "core/object/class_db.h"

Dictionary TetraVideoAssets::effect_to_mlt_properties(const String &p_operation_id, const Dictionary &p_values, int p_width, int p_height, int p_length) {
	Dictionary properties;
	for (const KeyValue<Variant, Variant> &value : p_values) {
		properties[value.key] = TetraVideoProject::value_to_mlt(value.value);
	}
	return properties;
}

void TetraVideoAssets::_bind_methods() {
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("effect_to_mlt_properties", "operation_id", "values", "width", "height", "length"), &TetraVideoAssets::effect_to_mlt_properties);
}
