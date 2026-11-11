#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

#include <limits>

// Helpers for the operation schema of docs/ARCHITECTURE.md: the plain
// Dictionaries with which every engine bridge describes its operations and
// their parameters, and from which the shared UI is generated.
//
// Bridge authors build schemas with the make_operation / param_* / set_*
// functions; consumers read them with get_defaults, sanitize_values and the
// lookup functions. Everything is static and also callable from scripts.
class TetraSchema : public Object {
	GDCLASS(TetraSchema, Object);

protected:
	static void _bind_methods();

public:
	// Passed as a limit or step to say "not set": the key is left out of the parameter.
	static constexpr double UNSET = std::numeric_limits<double>::quiet_NaN();

	// A seed without explicit limits is an unsigned 32-bit number.
	static constexpr double SEED_MAX = 4294967295.0;

	static Dictionary make_operation(const String &p_id, const String &p_label, const String &p_category, const String &p_source, const String &p_description = String());
	// Appends p_param to the operation's "params" and returns the operation.
	static Dictionary add_param(Dictionary p_operation, const Dictionary &p_param);

	static Dictionary param_bool(const String &p_name, const String &p_label, bool p_default = false);
	static Dictionary param_int(const String &p_name, const String &p_label, int64_t p_default = 0, double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
	static Dictionary param_float(const String &p_name, const String &p_label, double p_default = 0.0, double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
	static Dictionary param_string(const String &p_name, const String &p_label, const String &p_default = String());
	static Dictionary param_text(const String &p_name, const String &p_label, const String &p_default = String());
	// p_options is an Array of {value, label} Dictionaries; see make_option and make_options.
	static Dictionary param_enum(const String &p_name, const String &p_label, const Variant &p_default, const Array &p_options);
	static Dictionary param_color(const String &p_name, const String &p_label, const Color &p_default = Color(0, 0, 0, 1));
	static Dictionary param_vec2(const String &p_name, const String &p_label, const Vector2 &p_default = Vector2(), double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
	static Dictionary param_vec3(const String &p_name, const String &p_label, const Vector3 &p_default = Vector3(), double p_min = UNSET, double p_max = UNSET, double p_step = UNSET, const String &p_unit = String());
	static Dictionary param_file(const String &p_name, const String &p_label, const String &p_default = String());
	static Dictionary param_seed(const String &p_name, const String &p_label, int64_t p_default = 0, double p_min = UNSET, double p_max = UNSET);

	static Dictionary make_option(const Variant &p_value, const String &p_label);
	// Options whose value and label are the same strings.
	static Array make_options(const PackedStringArray &p_values);

	// Setters for the optional keys. Each changes p_param in place and returns it, so calls can be nested.
	static Dictionary set_description(Dictionary p_param, const String &p_description);
	static Dictionary set_range(Dictionary p_param, double p_min, double p_max);
	static Dictionary set_ui_range(Dictionary p_param, double p_ui_min, double p_ui_max);
	static Dictionary set_step(Dictionary p_param, double p_step);
	static Dictionary set_unit(Dictionary p_param, const String &p_unit);
	static Dictionary set_group(Dictionary p_param, const String &p_group);
	static Dictionary set_advanced(Dictionary p_param, bool p_advanced = true);

	static PackedStringArray get_param_types();
	static PackedStringArray get_sources();

	// The value a parameter takes when nothing is given: its "default" coerced to
	// its type, or the type's zero value (for an enum, the first option).
	static Variant get_param_default(const Dictionary &p_param);
	// Coerces p_value to the parameter's type and limits; falls back to the default when it cannot be converted.
	static Variant sanitize_value(const Dictionary &p_param, const Variant &p_value);

	static Dictionary get_defaults(const Dictionary &p_operation);
	static Dictionary sanitize_values(const Dictionary &p_operation, const Dictionary &p_values);
	// Empty when the operation follows the schema, otherwise one line per problem.
	static String validate_operation(const Dictionary &p_operation);
	// The parameter called p_name, or an empty Dictionary.
	static Dictionary find_param(const Dictionary &p_operation, const String &p_name);

	// The root node of the category hierarchy. A node is
	// {name, path, count, categories: Array of nodes, operations: Array of ids};
	// "count" includes the operations of sub-categories.
	static Dictionary category_tree(const Array &p_operations);
	// The operations matching every word of p_text, best matches first.
	static Array search(const Array &p_operations, const String &p_text);
	// Same ranking as search(), as indices into p_operations.
	static PackedInt32Array search_indices(const Array &p_operations, const String &p_text);
};
