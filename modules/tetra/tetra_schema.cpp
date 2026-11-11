#include "tetra_schema.h"

#include "core/math/color.h"
#include "core/math/math_funcs.h"
#include "core/string/char_utils.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

namespace {

bool is_unset(double p_number) {
	return Math::is_nan(p_number);
}

bool is_number(const Variant &p_value) {
	return p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT;
}

bool is_text(const Variant &p_value) {
	return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
}

String get_string(const Dictionary &p_dictionary, const char *p_key) {
	const Variant value = p_dictionary.get(p_key, Variant());
	return is_text(value) ? String(value) : String();
}

// Equality in which 2 and 2.0, and a String and a StringName of the same text, are the same value.
bool values_equal(const Variant &p_a, const Variant &p_b) {
	if (is_number(p_a) && is_number(p_b)) {
		if (p_a.get_type() == Variant::INT && p_b.get_type() == Variant::INT) {
			return int64_t(p_a) == int64_t(p_b);
		}
		return double(p_a) == double(p_b);
	}
	if (is_text(p_a) && is_text(p_b)) {
		return String(p_a) == String(p_b);
	}
	return p_a.get_type() == p_b.get_type() && p_a == p_b;
}

// Reads an optional numeric key such as "min"; false when it is absent or not a finite number.
bool get_limit(const Dictionary &p_param, const char *p_key, double &r_limit) {
	const Variant value = p_param.get(p_key, Variant());
	if (!is_number(value)) {
		return false;
	}
	const double number = value;
	if (!Math::is_finite(number)) {
		return false;
	}
	r_limit = number;
	return true;
}

Array get_params(const Dictionary &p_operation) {
	const Variant params = p_operation.get("params", Variant());
	return params.get_type() == Variant::ARRAY ? Array(params) : Array();
}

bool to_float(const Variant &p_value, double &r_number) {
	double number = 0.0;
	switch (p_value.get_type()) {
		case Variant::BOOL: {
			number = bool(p_value) ? 1.0 : 0.0;
		} break;
		case Variant::INT:
		case Variant::FLOAT: {
			number = p_value;
		} break;
		case Variant::STRING:
		case Variant::STRING_NAME: {
			const String text = String(p_value).strip_edges();
			if (!text.is_valid_float()) {
				return false;
			}
			number = text.to_float();
		} break;
		default: {
			return false;
		}
	}
	if (!Math::is_finite(number)) {
		return false;
	}
	r_number = number;
	return true;
}

// Integers keep their full 64-bit precision; fractional input is rounded to the nearest integer.
bool to_int(const Variant &p_value, int64_t &r_number) {
	if (p_value.get_type() == Variant::INT) {
		r_number = p_value;
		return true;
	}
	if (is_text(p_value)) {
		const String text = String(p_value).strip_edges();
		if (text.is_valid_int()) {
			r_number = text.to_int();
			return true;
		}
	}
	double number = 0.0;
	if (!to_float(p_value, number)) {
		return false;
	}
	// Doubles beyond this magnitude cannot be converted to int64_t without overflow.
	if (Math::abs(number) >= 9.2e18) {
		return false;
	}
	r_number = int64_t(Math::round(number));
	return true;
}

bool to_bool(const Variant &p_value, bool &r_flag) {
	switch (p_value.get_type()) {
		case Variant::BOOL: {
			r_flag = p_value;
			return true;
		}
		case Variant::INT: {
			r_flag = int64_t(p_value) != 0;
			return true;
		}
		case Variant::FLOAT: {
			r_flag = double(p_value) != 0.0;
			return true;
		}
		case Variant::STRING:
		case Variant::STRING_NAME: {
			const String text = String(p_value).strip_edges().to_lower();
			if (text == "true" || text == "yes" || text == "on" || text == "1") {
				r_flag = true;
				return true;
			}
			if (text == "false" || text == "no" || text == "off" || text == "0") {
				r_flag = false;
				return true;
			}
			return false;
		}
		default: {
			return false;
		}
	}
}

bool to_text(const Variant &p_value, String &r_text) {
	switch (p_value.get_type()) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT: {
			r_text = p_value;
			return true;
		}
		default: {
			return false;
		}
	}
}

// Collects the numbers of a vector-like value: vectors, a colour, or an array of numbers.
bool to_components(const Variant &p_value, LocalVector<double> &r_components) {
	switch (p_value.get_type()) {
		case Variant::VECTOR2: {
			const Vector2 vector = p_value;
			r_components.push_back(vector.x);
			r_components.push_back(vector.y);
		} break;
		case Variant::VECTOR2I: {
			const Vector2i vector = p_value;
			r_components.push_back(vector.x);
			r_components.push_back(vector.y);
		} break;
		case Variant::VECTOR3: {
			const Vector3 vector = p_value;
			r_components.push_back(vector.x);
			r_components.push_back(vector.y);
			r_components.push_back(vector.z);
		} break;
		case Variant::VECTOR3I: {
			const Vector3i vector = p_value;
			r_components.push_back(vector.x);
			r_components.push_back(vector.y);
			r_components.push_back(vector.z);
		} break;
		case Variant::VECTOR4: {
			const Vector4 vector = p_value;
			r_components.push_back(vector.x);
			r_components.push_back(vector.y);
			r_components.push_back(vector.z);
			r_components.push_back(vector.w);
		} break;
		case Variant::COLOR: {
			const Color color = p_value;
			r_components.push_back(color.r);
			r_components.push_back(color.g);
			r_components.push_back(color.b);
			r_components.push_back(color.a);
		} break;
		case Variant::ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY: {
			const Array array = p_value;
			for (int i = 0; i < array.size(); i++) {
				const Variant element = array[i];
				if (!is_number(element) || !Math::is_finite(double(element))) {
					return false;
				}
				r_components.push_back(double(element));
			}
		} break;
		default: {
			return false;
		}
	}
	for (const double component : r_components) {
		if (!Math::is_finite(component)) {
			return false;
		}
	}
	return true;
}

double clamp_to_limits(const Dictionary &p_param, double p_number) {
	double limit = 0.0;
	if (get_limit(p_param, "min", limit) && p_number < limit) {
		p_number = limit;
	}
	if (get_limit(p_param, "max", limit) && p_number > limit) {
		p_number = limit;
	}
	return p_number;
}

int64_t clamp_int_to_limits(const Dictionary &p_param, int64_t p_number, bool p_seed) {
	double minimum = 0.0;
	double maximum = 0.0;
	bool has_minimum = get_limit(p_param, "min", minimum);
	bool has_maximum = get_limit(p_param, "max", maximum);
	if (p_seed && !has_minimum) {
		minimum = 0.0;
		has_minimum = true;
	}
	if (p_seed && !has_maximum) {
		maximum = TetraSchema::SEED_MAX;
		has_maximum = true;
	}
	// The limits are compared as integers so that values above 2^53 are not disturbed.
	if (has_minimum && Math::abs(minimum) < 9.2e18) {
		const int64_t lowest = int64_t(Math::ceil(minimum));
		if (p_number < lowest) {
			p_number = lowest;
		}
	}
	if (has_maximum && Math::abs(maximum) < 9.2e18) {
		const int64_t highest = int64_t(Math::floor(maximum));
		if (p_number > highest) {
			p_number = highest;
		}
	}
	return p_number;
}

// Index of the option whose value is p_value, or -1. Values of the same kind
// are compared exactly; otherwise "2" also selects the option 2.
int find_option(const Array &p_options, const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return -1;
	}
	for (int i = 0; i < p_options.size(); i++) {
		if (p_options[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Variant option_value = Dictionary(p_options[i]).get("value", Variant());
		if (values_equal(option_value, p_value)) {
			return i;
		}
	}
	String wanted;
	if (!to_text(p_value, wanted)) {
		return -1;
	}
	for (int i = 0; i < p_options.size(); i++) {
		if (p_options[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		String option_text;
		if (to_text(Dictionary(p_options[i]).get("value", Variant()), option_text) && option_text == wanted) {
			return i;
		}
	}
	return -1;
}

Array get_options(const Dictionary &p_param) {
	const Variant options = p_param.get("options", Variant());
	return options.get_type() == Variant::ARRAY ? Array(options) : Array();
}

// The value used when a parameter has no usable default.
Variant zero_value(const Dictionary &p_param, const String &p_type) {
	if (p_type == "bool") {
		return false;
	}
	if (p_type == "int") {
		return clamp_int_to_limits(p_param, 0, false);
	}
	if (p_type == "seed") {
		return clamp_int_to_limits(p_param, 0, true);
	}
	if (p_type == "float") {
		return clamp_to_limits(p_param, 0.0);
	}
	if (p_type == "string" || p_type == "text" || p_type == "file") {
		return String();
	}
	if (p_type == "color") {
		return Color(0, 0, 0, 1);
	}
	if (p_type == "vec2") {
		const real_t component = clamp_to_limits(p_param, 0.0);
		return Vector2(component, component);
	}
	if (p_type == "vec3") {
		const real_t component = clamp_to_limits(p_param, 0.0);
		return Vector3(component, component, component);
	}
	if (p_type == "enum") {
		const Array options = get_options(p_param);
		for (int i = 0; i < options.size(); i++) {
			if (options[i].get_type() == Variant::DICTIONARY && Dictionary(options[i]).has("value")) {
				return Dictionary(options[i])["value"];
			}
		}
	}
	return Variant();
}

// Converts p_value to the parameter's type and limits, or returns p_fallback when it cannot be converted.
Variant coerce(const Dictionary &p_param, const String &p_type, const Variant &p_value, const Variant &p_fallback) {
	if (p_type == "bool") {
		bool flag = false;
		return to_bool(p_value, flag) ? Variant(flag) : p_fallback;
	}
	if (p_type == "int" || p_type == "seed") {
		int64_t number = 0;
		return to_int(p_value, number) ? Variant(clamp_int_to_limits(p_param, number, p_type == "seed")) : p_fallback;
	}
	if (p_type == "float") {
		double number = 0.0;
		return to_float(p_value, number) ? Variant(clamp_to_limits(p_param, number)) : p_fallback;
	}
	if (p_type == "string" || p_type == "text" || p_type == "file") {
		String text;
		return to_text(p_value, text) ? Variant(text) : p_fallback;
	}
	if (p_type == "enum") {
		const Array options = get_options(p_param);
		const int index = find_option(options, p_value);
		return index >= 0 ? Dictionary(options[index])["value"] : p_fallback;
	}
	if (p_type == "color") {
		if (p_value.get_type() == Variant::COLOR) {
			return p_value;
		}
		if (is_text(p_value)) {
			const String text = String(p_value).strip_edges();
			const Color invalid(-1, -1, -1, -1);
			const Color parsed = Color::from_string(text, invalid);
			return parsed == invalid ? p_fallback : Variant(parsed);
		}
		LocalVector<double> components;
		if (to_components(p_value, components) && (components.size() == 3 || components.size() == 4)) {
			return Color(components[0], components[1], components[2], components.size() == 4 ? components[3] : 1.0);
		}
		return p_fallback;
	}
	if (p_type == "vec2" || p_type == "vec3") {
		const uint32_t wanted = p_type == "vec2" ? 2 : 3;
		LocalVector<double> components;
		double uniform = 0.0;
		if (is_number(p_value) && to_float(p_value, uniform)) {
			components.resize(wanted);
			for (uint32_t i = 0; i < wanted; i++) {
				components[i] = uniform;
			}
		} else if (p_value.get_type() == Variant::COLOR || !to_components(p_value, components) || components.size() != wanted) {
			return p_fallback;
		}
		for (uint32_t i = 0; i < wanted; i++) {
			components[i] = clamp_to_limits(p_param, components[i]);
		}
		if (wanted == 2) {
			return Vector2(components[0], components[1]);
		}
		return Vector3(components[0], components[1], components[2]);
	}
	// A type this version does not know: keep what the caller gave.
	return p_value.get_type() == Variant::NIL ? p_fallback : p_value;
}

Dictionary make_param(const String &p_name, const String &p_label, const char *p_type, const Variant &p_default) {
	Dictionary param;
	param["name"] = p_name;
	param["label"] = p_label;
	param["type"] = p_type;
	param["default"] = p_default;
	return param;
}

void set_numeric_keys(Dictionary &p_param, double p_min, double p_max, double p_step, const String &p_unit) {
	if (!is_unset(p_min)) {
		p_param["min"] = p_min;
	}
	if (!is_unset(p_max)) {
		p_param["max"] = p_max;
	}
	if (!is_unset(p_step)) {
		p_param["step"] = p_step;
	}
	if (!p_unit.is_empty()) {
		p_param["unit"] = p_unit;
	}
}

bool is_word_character(char32_t p_character) {
	return is_unicode_identifier_continue(p_character);
}

// How well one lowercase search word matches an operation; 0 when it does not occur at all.
int term_score(const String &p_term, const String &p_label, const String &p_id, const String &p_category, const String &p_description) {
	const int in_label = p_label.find(p_term);
	if (in_label == 0) {
		return p_label.length() == p_term.length() ? 100 : 90;
	}
	if (in_label > 0) {
		// A match at the start of any word of the label counts more than one inside a word.
		int position = in_label;
		while (position >= 0) {
			if (!is_word_character(p_label[position - 1])) {
				return 70;
			}
			position = p_label.find(p_term, position + 1);
		}
		return 60;
	}
	if (p_id.get_slice(":", p_id.get_slice_count(":") - 1).begins_with(p_term)) {
		return 50;
	}
	if (p_id.contains(p_term)) {
		return 40;
	}
	if (p_category.contains(p_term)) {
		return 25;
	}
	if (p_description.contains(p_term)) {
		return 10;
	}
	return 0;
}

struct SearchHit {
	int score = 0;
	int index = 0;
	String label;
	String id;

	bool operator<(const SearchHit &p_other) const {
		if (score != p_other.score) {
			return score > p_other.score;
		}
		if (label.length() != p_other.label.length()) {
			return label.length() < p_other.label.length();
		}
		if (label != p_other.label) {
			return label < p_other.label;
		}
		if (id != p_other.id) {
			return id < p_other.id;
		}
		return index < p_other.index;
	}
};

struct NamedEntry {
	String name;
	String tie_breaker;
	int index = 0;

	bool operator<(const NamedEntry &p_other) const {
		const int order = name.naturalnocasecmp_to(p_other.name);
		if (order != 0) {
			return order < 0;
		}
		if (tie_breaker != p_other.tie_breaker) {
			return tie_breaker < p_other.tie_breaker;
		}
		return index < p_other.index;
	}
};

struct CategoryNode {
	String name;
	String path;
	LocalVector<int> children;
	LocalVector<int> operations;
};

Dictionary category_node_to_dictionary(const LocalVector<CategoryNode> &p_nodes, int p_node, const Array &p_operations) {
	const CategoryNode &node = p_nodes[p_node];

	Vector<NamedEntry> child_order;
	for (const int child : node.children) {
		NamedEntry entry;
		entry.name = p_nodes[child].name;
		entry.index = child;
		child_order.push_back(entry);
	}
	child_order.sort();

	Vector<NamedEntry> operation_order;
	for (const int operation_index : node.operations) {
		const Dictionary operation = p_operations[operation_index];
		NamedEntry entry;
		entry.tie_breaker = get_string(operation, "id");
		entry.name = get_string(operation, "label");
		if (entry.name.is_empty()) {
			entry.name = entry.tie_breaker;
		}
		entry.index = operation_index;
		operation_order.push_back(entry);
	}
	operation_order.sort();

	int count = node.operations.size();
	Array categories;
	for (const NamedEntry &entry : child_order) {
		const Dictionary child = category_node_to_dictionary(p_nodes, entry.index, p_operations);
		count += int(child["count"]);
		categories.push_back(child);
	}
	Array operations;
	for (const NamedEntry &entry : operation_order) {
		operations.push_back(entry.tie_breaker);
	}

	Dictionary result;
	result["name"] = node.name;
	result["path"] = node.path;
	result["count"] = count;
	result["categories"] = categories;
	result["operations"] = operations;
	return result;
}

void validate_param(const Variant &p_entry, int p_index, HashSet<String> &r_names, PackedStringArray &r_problems) {
	if (p_entry.get_type() != Variant::DICTIONARY) {
		r_problems.push_back(vformat("param %d: must be a Dictionary", p_index));
		return;
	}
	const Dictionary param = p_entry;
	const Variant name_value = param.get("name", Variant());
	String where = vformat("param %d", p_index);
	if (name_value.get_type() != Variant::STRING || String(name_value).is_empty()) {
		r_problems.push_back(where + ": 'name' must be a non-empty String");
	} else {
		where = vformat("param '%s'", String(name_value));
		if (r_names.has(name_value)) {
			r_problems.push_back(where + ": duplicate name");
		}
		r_names.insert(name_value);
	}
	const Variant label = param.get("label", Variant());
	if (label.get_type() != Variant::STRING || String(label).is_empty()) {
		r_problems.push_back(where + ": 'label' must be a non-empty String");
	}
	for (const char *key : { "description", "unit", "group" }) {
		if (param.has(key) && param[key].get_type() != Variant::STRING) {
			r_problems.push_back(vformat("%s: '%s' must be a String", where, key));
		}
	}
	if (param.has("advanced") && param["advanced"].get_type() != Variant::BOOL) {
		r_problems.push_back(where + ": 'advanced' must be a bool");
	}
	bool numbers_valid = true;
	for (const char *key : { "min", "max", "ui_min", "ui_max", "step" }) {
		if (param.has(key) && (!is_number(param[key]) || !Math::is_finite(double(param[key])))) {
			r_problems.push_back(vformat("%s: '%s' must be a finite number", where, key));
			numbers_valid = false;
		}
	}
	if (numbers_valid) {
		double low = 0.0;
		double high = 0.0;
		if (get_limit(param, "min", low) && get_limit(param, "max", high) && low > high) {
			r_problems.push_back(where + ": 'min' is greater than 'max'");
		}
		if (get_limit(param, "ui_min", low) && get_limit(param, "ui_max", high) && low > high) {
			r_problems.push_back(where + ": 'ui_min' is greater than 'ui_max'");
		}
		if (get_limit(param, "step", low) && low <= 0.0) {
			r_problems.push_back(where + ": 'step' must be greater than 0");
		}
	}

	const Variant type_value = param.get("type", Variant());
	if (type_value.get_type() != Variant::STRING || !TetraSchema::get_param_types().has(String(type_value))) {
		r_problems.push_back(vformat("%s: 'type' must be one of %s", where, String(", ").join(TetraSchema::get_param_types())));
		return;
	}
	const String type = type_value;

	if (type == "enum") {
		const Variant options_value = param.get("options", Variant());
		if (options_value.get_type() != Variant::ARRAY || Array(options_value).is_empty()) {
			r_problems.push_back(where + ": 'options' must be a non-empty Array of {value, label}");
			return;
		}
		const Array options = options_value;
		for (int i = 0; i < options.size(); i++) {
			if (options[i].get_type() != Variant::DICTIONARY) {
				r_problems.push_back(vformat("%s: option %d must be a Dictionary", where, i));
				return;
			}
			const Dictionary option = options[i];
			const Variant option_value = option.get("value", Variant());
			if (!is_number(option_value) && option_value.get_type() != Variant::STRING) {
				r_problems.push_back(vformat("%s: option %d needs a String or int 'value'", where, i));
				return;
			}
			if (option.get("label", Variant()).get_type() != Variant::STRING) {
				r_problems.push_back(vformat("%s: option %d needs a String 'label'", where, i));
			}
			for (int j = 0; j < i; j++) {
				const Variant earlier = Dictionary(options[j]).get("value", Variant());
				if (earlier.get_type() == option_value.get_type() && earlier == option_value) {
					r_problems.push_back(vformat("%s: option %d repeats the value of option %d", where, i, j));
				}
			}
		}
	}

	if (!param.has("default")) {
		r_problems.push_back(where + ": 'default' is missing");
		return;
	}
	const Variant default_value = param["default"];
	bool default_matches = false;
	const char *expected = "";
	if (type == "bool") {
		default_matches = default_value.get_type() == Variant::BOOL;
		expected = "a bool";
	} else if (type == "int" || type == "seed") {
		default_matches = default_value.get_type() == Variant::INT || (default_value.get_type() == Variant::FLOAT && Math::is_finite(double(default_value)) && Math::floor(double(default_value)) == double(default_value));
		expected = "an int";
	} else if (type == "float") {
		default_matches = is_number(default_value) && Math::is_finite(double(default_value));
		expected = "a float";
	} else if (type == "string" || type == "text" || type == "file") {
		default_matches = default_value.get_type() == Variant::STRING;
		expected = "a String";
	} else if (type == "color") {
		default_matches = default_value.get_type() == Variant::COLOR;
		expected = "a Color";
	} else if (type == "vec2") {
		default_matches = default_value.get_type() == Variant::VECTOR2;
		expected = "a Vector2";
	} else if (type == "vec3") {
		default_matches = default_value.get_type() == Variant::VECTOR3;
		expected = "a Vector3";
	} else if (type == "enum") {
		const Array options = get_options(param);
		for (int i = 0; i < options.size() && !default_matches; i++) {
			const Variant option_value = Dictionary(options[i]).get("value", Variant());
			default_matches = option_value.get_type() == default_value.get_type() && option_value == default_value;
		}
		expected = "the 'value' of one of the options";
	}
	if (!default_matches) {
		r_problems.push_back(vformat("%s: 'default' must be %s", where, expected));
		return;
	}
	if (numbers_valid && type != "enum" && !values_equal(coerce(param, type, default_value, default_value), default_value)) {
		r_problems.push_back(where + ": 'default' is outside 'min'/'max'");
	}
}

} // namespace

Dictionary TetraSchema::make_operation(const String &p_id, const String &p_label, const String &p_category, const String &p_source, const String &p_description) {
	Dictionary operation;
	operation["id"] = p_id;
	operation["label"] = p_label;
	if (!p_description.is_empty()) {
		operation["description"] = p_description;
	}
	operation["category"] = p_category;
	operation["source"] = p_source;
	operation["params"] = Array();
	return operation;
}

Dictionary TetraSchema::add_param(Dictionary p_operation, const Dictionary &p_param) {
	Array params = get_params(p_operation);
	params.push_back(p_param);
	p_operation["params"] = params;
	return p_operation;
}

Dictionary TetraSchema::param_bool(const String &p_name, const String &p_label, bool p_default) {
	return make_param(p_name, p_label, "bool", p_default);
}

Dictionary TetraSchema::param_int(const String &p_name, const String &p_label, int64_t p_default, double p_min, double p_max, double p_step, const String &p_unit) {
	Dictionary param = make_param(p_name, p_label, "int", p_default);
	set_numeric_keys(param, p_min, p_max, p_step, p_unit);
	return param;
}

Dictionary TetraSchema::param_float(const String &p_name, const String &p_label, double p_default, double p_min, double p_max, double p_step, const String &p_unit) {
	Dictionary param = make_param(p_name, p_label, "float", p_default);
	set_numeric_keys(param, p_min, p_max, p_step, p_unit);
	return param;
}

Dictionary TetraSchema::param_string(const String &p_name, const String &p_label, const String &p_default) {
	return make_param(p_name, p_label, "string", p_default);
}

Dictionary TetraSchema::param_text(const String &p_name, const String &p_label, const String &p_default) {
	return make_param(p_name, p_label, "text", p_default);
}

Dictionary TetraSchema::param_enum(const String &p_name, const String &p_label, const Variant &p_default, const Array &p_options) {
	Dictionary param = make_param(p_name, p_label, "enum", p_default);
	param["options"] = p_options;
	return param;
}

Dictionary TetraSchema::param_color(const String &p_name, const String &p_label, const Color &p_default) {
	return make_param(p_name, p_label, "color", p_default);
}

Dictionary TetraSchema::param_vec2(const String &p_name, const String &p_label, const Vector2 &p_default, double p_min, double p_max, double p_step, const String &p_unit) {
	Dictionary param = make_param(p_name, p_label, "vec2", p_default);
	set_numeric_keys(param, p_min, p_max, p_step, p_unit);
	return param;
}

Dictionary TetraSchema::param_vec3(const String &p_name, const String &p_label, const Vector3 &p_default, double p_min, double p_max, double p_step, const String &p_unit) {
	Dictionary param = make_param(p_name, p_label, "vec3", p_default);
	set_numeric_keys(param, p_min, p_max, p_step, p_unit);
	return param;
}

Dictionary TetraSchema::param_file(const String &p_name, const String &p_label, const String &p_default) {
	return make_param(p_name, p_label, "file", p_default);
}

Dictionary TetraSchema::param_seed(const String &p_name, const String &p_label, int64_t p_default, double p_min, double p_max) {
	Dictionary param = make_param(p_name, p_label, "seed", p_default);
	set_numeric_keys(param, p_min, p_max, UNSET, String());
	return param;
}

Dictionary TetraSchema::make_option(const Variant &p_value, const String &p_label) {
	Dictionary option;
	option["value"] = p_value;
	option["label"] = p_label;
	return option;
}

Array TetraSchema::make_options(const PackedStringArray &p_values) {
	Array options;
	for (const String &value : p_values) {
		options.push_back(make_option(value, value));
	}
	return options;
}

Dictionary TetraSchema::set_description(Dictionary p_param, const String &p_description) {
	p_param["description"] = p_description;
	return p_param;
}

Dictionary TetraSchema::set_range(Dictionary p_param, double p_min, double p_max) {
	if (!is_unset(p_min)) {
		p_param["min"] = p_min;
	}
	if (!is_unset(p_max)) {
		p_param["max"] = p_max;
	}
	return p_param;
}

Dictionary TetraSchema::set_ui_range(Dictionary p_param, double p_ui_min, double p_ui_max) {
	if (!is_unset(p_ui_min)) {
		p_param["ui_min"] = p_ui_min;
	}
	if (!is_unset(p_ui_max)) {
		p_param["ui_max"] = p_ui_max;
	}
	return p_param;
}

Dictionary TetraSchema::set_step(Dictionary p_param, double p_step) {
	p_param["step"] = p_step;
	return p_param;
}

Dictionary TetraSchema::set_unit(Dictionary p_param, const String &p_unit) {
	p_param["unit"] = p_unit;
	return p_param;
}

Dictionary TetraSchema::set_group(Dictionary p_param, const String &p_group) {
	p_param["group"] = p_group;
	return p_param;
}

Dictionary TetraSchema::set_advanced(Dictionary p_param, bool p_advanced) {
	p_param["advanced"] = p_advanced;
	return p_param;
}

PackedStringArray TetraSchema::get_param_types() {
	return PackedStringArray({ "bool", "int", "float", "string", "text", "enum", "color", "vec2", "vec3", "file", "seed" });
}

PackedStringArray TetraSchema::get_sources() {
	return PackedStringArray({ "native", "gegl", "mlt", "kdenlive", "blender" });
}

Variant TetraSchema::get_param_default(const Dictionary &p_param) {
	const String type = get_string(p_param, "type");
	const Variant fallback = zero_value(p_param, type);
	if (!p_param.has("default")) {
		return fallback;
	}
	return coerce(p_param, type, p_param["default"], fallback);
}

Variant TetraSchema::sanitize_value(const Dictionary &p_param, const Variant &p_value) {
	return coerce(p_param, get_string(p_param, "type"), p_value, get_param_default(p_param));
}

Dictionary TetraSchema::get_defaults(const Dictionary &p_operation) {
	Dictionary defaults;
	const Array params = get_params(p_operation);
	for (int i = 0; i < params.size(); i++) {
		if (params[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary param = params[i];
		const String name = get_string(param, "name");
		if (!name.is_empty()) {
			defaults[name] = get_param_default(param);
		}
	}
	return defaults;
}

Dictionary TetraSchema::sanitize_values(const Dictionary &p_operation, const Dictionary &p_values) {
	Dictionary values;
	const Array params = get_params(p_operation);
	for (int i = 0; i < params.size(); i++) {
		if (params[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary param = params[i];
		const String name = get_string(param, "name");
		if (name.is_empty()) {
			continue;
		}
		values[name] = p_values.has(name) ? sanitize_value(param, p_values[name]) : get_param_default(param);
	}
	return values;
}

String TetraSchema::validate_operation(const Dictionary &p_operation) {
	PackedStringArray problems;
	for (const char *key : { "id", "label" }) {
		const Variant value = p_operation.get(key, Variant());
		if (value.get_type() != Variant::STRING || String(value).is_empty()) {
			problems.push_back(vformat("'%s' must be a non-empty String", key));
		}
	}
	if (p_operation.has("description") && p_operation["description"].get_type() != Variant::STRING) {
		problems.push_back("'description' must be a String");
	}
	if (p_operation.get("category", Variant()).get_type() != Variant::STRING) {
		problems.push_back("'category' must be a String");
	}
	const Variant source = p_operation.get("source", Variant());
	if (source.get_type() != Variant::STRING || !get_sources().has(String(source))) {
		problems.push_back(vformat("'source' must be one of %s", String(", ").join(get_sources())));
	}
	const Variant params = p_operation.get("params", Variant());
	if (params.get_type() != Variant::ARRAY) {
		problems.push_back("'params' must be an Array");
	} else {
		const Array entries = params;
		HashSet<String> names;
		for (int i = 0; i < entries.size(); i++) {
			validate_param(entries[i], i, names, problems);
		}
	}
	return String("\n").join(problems);
}

Dictionary TetraSchema::find_param(const Dictionary &p_operation, const String &p_name) {
	const Array params = get_params(p_operation);
	for (int i = 0; i < params.size(); i++) {
		if (params[i].get_type() == Variant::DICTIONARY && get_string(params[i], "name") == p_name) {
			return params[i];
		}
	}
	return Dictionary();
}

Dictionary TetraSchema::category_tree(const Array &p_operations) {
	LocalVector<CategoryNode> nodes;
	nodes.push_back(CategoryNode());
	HashMap<String, int> node_by_path;
	node_by_path[String()] = 0;

	for (int i = 0; i < p_operations.size(); i++) {
		if (p_operations[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const PackedStringArray segments = get_string(p_operations[i], "category").split("/", false);
		int current = 0;
		String path;
		for (const String &segment : segments) {
			const String name = segment.strip_edges();
			if (name.is_empty()) {
				continue;
			}
			path = path.is_empty() ? name : path + "/" + name;
			const HashMap<String, int>::Iterator found = node_by_path.find(path);
			if (found) {
				current = found->value;
				continue;
			}
			CategoryNode node;
			node.name = name;
			node.path = path;
			const int created = nodes.size();
			nodes.push_back(node);
			nodes[current].children.push_back(created);
			node_by_path[path] = created;
			current = created;
		}
		nodes[current].operations.push_back(i);
	}
	return category_node_to_dictionary(nodes, 0, p_operations);
}

PackedInt32Array TetraSchema::search_indices(const Array &p_operations, const String &p_text) {
	const String query = String(" ").join(p_text.to_lower().split_spaces());
	const PackedStringArray terms = query.split(" ", false);

	PackedInt32Array result;
	if (terms.is_empty()) {
		for (int i = 0; i < p_operations.size(); i++) {
			if (p_operations[i].get_type() == Variant::DICTIONARY) {
				result.push_back(i);
			}
		}
		return result;
	}

	Vector<SearchHit> hits;
	for (int i = 0; i < p_operations.size(); i++) {
		if (p_operations[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary operation = p_operations[i];
		const String label = get_string(operation, "label").to_lower();
		const String id = get_string(operation, "id").to_lower();
		const String category = get_string(operation, "category").to_lower();
		const String description = get_string(operation, "description").to_lower();

		int score = 0;
		for (const String &term : terms) {
			const int matched = term_score(term, label, id, category, description);
			if (matched == 0) {
				score = 0;
				break;
			}
			score += matched;
		}
		if (score == 0) {
			continue;
		}
		// The whole phrase appearing in the label outranks any combination of scattered words.
		if (label == query) {
			score += 1000;
		} else if (label.begins_with(query)) {
			score += 500;
		} else if (label.contains(query)) {
			score += 250;
		}
		SearchHit hit;
		hit.score = score;
		hit.index = i;
		hit.label = label;
		hit.id = id;
		hits.push_back(hit);
	}
	hits.sort();
	for (const SearchHit &hit : hits) {
		result.push_back(hit.index);
	}
	return result;
}

Array TetraSchema::search(const Array &p_operations, const String &p_text) {
	Array result;
	for (const int index : search_indices(p_operations, p_text)) {
		result.push_back(p_operations[index]);
	}
	return result;
}

void TetraSchema::_bind_methods() {
	const StringName class_name = SNAME("TetraSchema");

	ClassDB::bind_static_method(class_name, D_METHOD("make_operation", "id", "label", "category", "source", "description"), &TetraSchema::make_operation, DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("add_param", "operation", "param"), &TetraSchema::add_param);

	ClassDB::bind_static_method(class_name, D_METHOD("param_bool", "name", "label", "default"), &TetraSchema::param_bool, DEFVAL(false));
	ClassDB::bind_static_method(class_name, D_METHOD("param_int", "name", "label", "default", "min", "max", "step", "unit"), &TetraSchema::param_int, DEFVAL(0), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_float", "name", "label", "default", "min", "max", "step", "unit"), &TetraSchema::param_float, DEFVAL(0.0), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_string", "name", "label", "default"), &TetraSchema::param_string, DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_text", "name", "label", "default"), &TetraSchema::param_text, DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_enum", "name", "label", "default", "options"), &TetraSchema::param_enum);
	ClassDB::bind_static_method(class_name, D_METHOD("param_color", "name", "label", "default"), &TetraSchema::param_color, DEFVAL(Color(0, 0, 0, 1)));
	ClassDB::bind_static_method(class_name, D_METHOD("param_vec2", "name", "label", "default", "min", "max", "step", "unit"), &TetraSchema::param_vec2, DEFVAL(Vector2()), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_vec3", "name", "label", "default", "min", "max", "step", "unit"), &TetraSchema::param_vec3, DEFVAL(Vector3()), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(UNSET), DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_file", "name", "label", "default"), &TetraSchema::param_file, DEFVAL(String()));
	ClassDB::bind_static_method(class_name, D_METHOD("param_seed", "name", "label", "default", "min", "max"), &TetraSchema::param_seed, DEFVAL(0), DEFVAL(UNSET), DEFVAL(UNSET));

	ClassDB::bind_static_method(class_name, D_METHOD("make_option", "value", "label"), &TetraSchema::make_option);
	ClassDB::bind_static_method(class_name, D_METHOD("make_options", "values"), &TetraSchema::make_options);

	ClassDB::bind_static_method(class_name, D_METHOD("set_description", "param", "description"), &TetraSchema::set_description);
	ClassDB::bind_static_method(class_name, D_METHOD("set_range", "param", "min", "max"), &TetraSchema::set_range);
	ClassDB::bind_static_method(class_name, D_METHOD("set_ui_range", "param", "ui_min", "ui_max"), &TetraSchema::set_ui_range);
	ClassDB::bind_static_method(class_name, D_METHOD("set_step", "param", "step"), &TetraSchema::set_step);
	ClassDB::bind_static_method(class_name, D_METHOD("set_unit", "param", "unit"), &TetraSchema::set_unit);
	ClassDB::bind_static_method(class_name, D_METHOD("set_group", "param", "group"), &TetraSchema::set_group);
	ClassDB::bind_static_method(class_name, D_METHOD("set_advanced", "param", "advanced"), &TetraSchema::set_advanced, DEFVAL(true));

	ClassDB::bind_static_method(class_name, D_METHOD("get_param_types"), &TetraSchema::get_param_types);
	ClassDB::bind_static_method(class_name, D_METHOD("get_sources"), &TetraSchema::get_sources);
	ClassDB::bind_static_method(class_name, D_METHOD("get_param_default", "param"), &TetraSchema::get_param_default);
	ClassDB::bind_static_method(class_name, D_METHOD("sanitize_value", "param", "value"), &TetraSchema::sanitize_value);

	ClassDB::bind_static_method(class_name, D_METHOD("get_defaults", "operation"), &TetraSchema::get_defaults);
	ClassDB::bind_static_method(class_name, D_METHOD("sanitize_values", "operation", "values"), &TetraSchema::sanitize_values);
	ClassDB::bind_static_method(class_name, D_METHOD("validate_operation", "operation"), &TetraSchema::validate_operation);
	ClassDB::bind_static_method(class_name, D_METHOD("find_param", "operation", "name"), &TetraSchema::find_param);
	ClassDB::bind_static_method(class_name, D_METHOD("category_tree", "operations"), &TetraSchema::category_tree);
	ClassDB::bind_static_method(class_name, D_METHOD("search", "operations", "text"), &TetraSchema::search);
	ClassDB::bind_static_method(class_name, D_METHOD("search_indices", "operations", "text"), &TetraSchema::search_indices);
}
