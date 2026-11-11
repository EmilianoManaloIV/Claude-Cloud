#include "tetra_video_assets.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_project.h"
#include "tetra_video_xml.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/expression.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"

// The mapping of Kdenlive's parameter types follows
// kdenlive/src/assets/model/assetparametermodel.cpp (GPL-3.0-or-later,
// copyright Nicolas Carion and the Kdenlive authors): a numeric parameter's
// "default" is the MLT value, "min"/"max" are display values and
// display = MLT * factor; placeholders such as %width are replaced by the
// profile's values and may be part of an arithmetic expression.

namespace {

struct Catalog {
	Vector<Dictionary> effects;
	Vector<Dictionary> transitions;
	HashMap<String, Dictionary> by_id;
};

Mutex catalog_mutex;
HashMap<Vector2i, Catalog> catalogs;

// Stand-ins for the placeholders that depend on the clip an effect sits on, used where a number is needed up front.
const int NOMINAL_OUT = 124;
const int NOMINAL_FADE = 25;

String _resolve_size(const String &p_text, int p_width, int p_height) {
	if (!p_text.contains_char('%')) {
		return p_text;
	}
	return p_text.replace("%maxWidth", itos(p_width))
			.replace("%maxHeight", itos(p_height))
			.replace("%width", itos(p_width))
			.replace("%height", itos(p_height))
			.replace("%contentWidth", itos(p_width))
			.replace("%contentHeight", itos(p_height))
			.replace("%fittedContentWidth", itos(p_width))
			.replace("%fittedContentHeight", itos(p_height));
}

String _resolve_clip(const String &p_text, int p_length) {
	if (!p_text.contains_char('%')) {
		return p_text;
	}
	return p_text.replace("%out", itos(MAX(0, p_length - 1)))
			.replace("%fade", itos(NOMINAL_FADE))
			.replace("%position", "0")
			.replace("%in", "0");
}

// A number that may be written as arithmetic on placeholders ("%width/2").
double _number(const String &p_text, int p_width, int p_height, double p_fallback) {
	const String text = _resolve_clip(_resolve_size(p_text, p_width, p_height), NOMINAL_OUT + 1).strip_edges();
	if (text.is_empty()) {
		return p_fallback;
	}
	if (text.is_valid_float()) {
		return text.to_float();
	}
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		if (!(is_digit(c) || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' || c == ' ')) {
			return p_fallback;
		}
	}
	Ref<Expression> expression;
	expression.instantiate();
	// Kdenlive evaluates with floating-point division.
	if (expression->parse(text.replace("/", "*1.0/")) != OK) {
		return p_fallback;
	}
	const Variant result = expression->execute(Array(), nullptr, false, true);
	return expression->has_execute_failed() || !result.is_num() ? p_fallback : double(result);
}

bool _is_integral(double p_value) {
	return Math::is_equal_approx(p_value, Math::round(p_value));
}

// MLT writes colours as "#rrggbb", "#aarrggbb", "0xrrggbbaa" or a name.
Color _parse_color(const String &p_text) {
	const String text = p_text.strip_edges().to_lower();
	if (text.begins_with("0x") && text.length() == 10) {
		return Color::html(text.substr(2));
	}
	if (text.begins_with("0x") && text.length() == 8) {
		return Color::html(text.substr(2));
	}
	if (text.begins_with("#") && text.length() == 9) {
		return Color::html(text.substr(3) + text.substr(1, 2));
	}
	return Color::from_string(text, Color(0, 0, 0, 1));
}

String _child_text(const Vector<TetraXmlNode> &p_nodes, int p_node, const String &p_name) {
	const int child = tetra_xml_find_child(p_nodes, p_node, p_name);
	return child < 0 ? String() : p_nodes[child].text.strip_edges();
}

// One <parameter> of a Kdenlive asset. Returns an empty dictionary for a
// parameter that is not shown; r_fixed collects the ones that are always sent.
Dictionary _parse_parameter(const Vector<TetraXmlNode> &p_nodes, int p_node, int p_width, int p_height, Dictionary &r_fixed) {
	const TetraXmlNode &node = p_nodes[p_node];
	const String type = node.get_attribute("type");
	const String name = node.get_attribute("name");
	const String default_text = node.get_attribute("default");
	Dictionary param;
	if (name.is_empty()) {
		return param;
	}
	if (type == "fixed" || type == "hidden") {
		const String value = node.attributes.has("value") ? node.get_attribute("value") : default_text;
		if (!value.is_empty()) {
			r_fixed[name] = _resolve_size(value, p_width, p_height);
		}
		return param;
	}
	// These drive several properties at once or start background jobs; the service's own defaults apply.
	if (type == "multiswitch" || type == "filterjob" || type == "readonly") {
		return param;
	}
	const String label = _child_text(p_nodes, p_node, "name");
	param["name"] = name;
	param["label"] = label.is_empty() ? name.capitalize() : label;
	param["description"] = _child_text(p_nodes, p_node, "comment");
	param["kdenlive_type"] = type;

	static const char *numeric_types[] = { "constant", "double", "animated", "keyframe", "simplekeyframe", "integer", "float", "colorwheel" };
	bool numeric = false;
	for (const char *numeric_type : numeric_types) {
		numeric = numeric || type == numeric_type;
	}
	if (numeric) {
		double factor = _number(node.get_attribute("factor"), p_width, p_height, 1.0);
		if (factor == 0.0) {
			factor = 1.0;
		}
		const double value = _number(default_text, p_width, p_height, 0.0) * factor;
		const bool has_min = node.attributes.has("min");
		const bool has_max = node.attributes.has("max");
		const double minimum = _number(node.get_attribute("min"), p_width, p_height, 0.0);
		const double maximum = _number(node.get_attribute("max"), p_width, p_height, 0.0);
		const int decimals = node.get_attribute("decimals").to_int();
		const bool integer = decimals == 0 && _is_integral(value) && _is_integral(minimum) && _is_integral(maximum);
		param["type"] = integer ? "int" : "float";
		param["default"] = integer ? Variant(int64_t(Math::round(value))) : Variant(value);
		if (has_min && has_max && maximum > minimum) {
			param["min"] = minimum;
			param["max"] = maximum;
		}
		if (integer) {
			param["step"] = 1.0;
		} else if (decimals > 0) {
			param["step"] = Math::pow(10.0, -double(decimals));
		}
		if (factor != 1.0) {
			param["factor"] = factor;
		}
		if (node.attributes.has("suffix")) {
			param["unit"] = node.get_attribute("suffix");
		}
		if (type == "animated" || type == "keyframe" || type == "simplekeyframe") {
			param["animated"] = true;
		}
	} else if (type == "bool") {
		param["type"] = "bool";
		param["default"] = default_text == "1";
	} else if (type == "switch") {
		// A switch stores arbitrary "on" and "off" strings in max and min.
		param["type"] = "bool";
		param["switch_on"] = node.get_attribute("max");
		param["switch_off"] = node.get_attribute("min");
		param["default"] = default_text == node.get_attribute("max");
	} else if ((type == "list" || type == "listdependency") && !node.get_attribute("paramlist").begins_with("%")) {
		const String list = node.get_attribute("paramlist");
		const PackedStringArray values = list.split(list.contains_char(';') || !list.contains_char(',') ? ";" : ",");
		const PackedStringArray labels = _child_text(p_nodes, p_node, "paramlistdisplay").split(",");
		Array options;
		bool default_listed = false;
		for (int i = 0; i < values.size(); i++) {
			Dictionary option;
			option["value"] = values[i];
			option["label"] = labels.size() == values.size() ? labels[i].strip_edges() : values[i];
			options.push_back(option);
			default_listed = default_listed || values[i] == default_text;
		}
		if (!default_listed) {
			Dictionary option;
			option["value"] = default_text;
			option["label"] = default_text;
			options.push_front(option);
		}
		param["type"] = "enum";
		param["default"] = default_text;
		param["options"] = options;
	} else if (type == "color" || type == "fixedcolor") {
		param["type"] = "color";
		param["default"] = _parse_color(default_text);
		param["color_format"] = default_text.begins_with("#") ? "#" : "0x";
		param["alpha"] = node.get_attribute("alpha") == "1" || default_text.length() > 8;
	} else if (type == "position") {
		param["type"] = "int";
		param["default"] = int64_t(Math::round(_number(default_text, p_width, p_height, 0.0)));
		param["unit"] = "frames";
	} else if (type == "url" || type == "urllist" || type == "list" || type == "listdependency") {
		param["type"] = "file";
		param["default"] = default_text.begins_with("%") ? String() : default_text;
	} else if (type == "text") {
		param["type"] = "text";
		param["default"] = default_text;
	} else {
		// Rectangles, curves, splines, wipes... are edited as the raw MLT value until they get editors of their own.
		param["type"] = "string";
		param["default"] = _resolve_size(default_text, p_width, p_height);
		param["raw"] = true;
	}
	return param;
}

void _collect_assets(const Vector<TetraXmlNode> &p_nodes, int p_node, const String &p_element, Vector<int> &r_found) {
	for (int child : p_nodes[p_node].children) {
		if (p_nodes[child].name == p_element) {
			r_found.push_back(child);
		} else {
			_collect_assets(p_nodes, child, p_element, r_found);
		}
	}
}

void _load_assets(const String &p_path, bool p_transitions, const HashMap<String, String> &p_categories, int p_width, int p_height, Catalog &r_catalog) {
	Error error = OK;
	const String xml = FileAccess::get_file_as_string(p_path, &error);
	Vector<TetraXmlNode> nodes;
	String message;
	if (error != OK || tetra_parse_xml(xml, nodes, message) != OK) {
		return;
	}
	Vector<int> found;
	_collect_assets(nodes, 0, p_transitions ? "transition" : "effect", found);
	for (int index : found) {
		const TetraXmlNode &node = nodes[index];
		const String service = node.get_attribute("tag");
		const String id = "kdenlive:" + node.get_attribute("id", service);
		// A file may offer one effect in several versions of the underlying plug-in; the first one is taken.
		if (service.is_empty() || r_catalog.by_id.has(id)) {
			continue;
		}
		const String type = node.get_attribute("type");
		const bool audio = !p_transitions && (type == "audio" || type == "customAudio");
		const String label = _child_text(nodes, index, "name");
		Dictionary fixed;
		Array params;
		for (int child : node.children) {
			if (nodes[child].name == "parameter") {
				const Dictionary param = _parse_parameter(nodes, child, p_width, p_height, fixed);
				if (!param.is_empty()) {
					params.push_back(param);
				}
			}
		}
		Dictionary operation;
		operation["id"] = id;
		operation["label"] = label.is_empty() ? node.get_attribute("id", service) : label;
		operation["description"] = _child_text(nodes, index, "description");
		const String *category = p_categories.getptr(node.get_attribute("id", service));
		operation["category"] = p_transitions ? String("Transitions") : (category ? *category : String(audio ? "Audio" : "Misc"));
		operation["source"] = "kdenlive";
		operation["params"] = params;
		operation["service"] = service;
		operation["kind"] = p_transitions ? "transition" : (audio ? "audio_effect" : "video_effect");
		operation["fixed"] = fixed;
		operation["available"] = TetraMltEngine::has_service(p_transitions ? "transition" : "filter", service);
		r_catalog.by_id[id] = operation;
		if (p_transitions) {
			r_catalog.transitions.push_back(operation);
		} else {
			r_catalog.effects.push_back(operation);
		}
	}
}

PackedStringArray _xml_files(const String &p_dir) {
	PackedStringArray files;
	for (const String &file : DirAccess::get_files_at(p_dir)) {
		if (file.get_extension() == "xml") {
			files.push_back(p_dir.path_join(file));
		}
	}
	files.sort();
	return files;
}

void _build_catalog(int p_width, int p_height, Catalog &r_catalog) {
	const String data_dir = TetraVideoAssets::get_data_dir();
	HashSet<String> covered_services;
	if (!data_dir.is_empty()) {
		// kdenliveeffectscategory.rc: <group list="id,id,..."><text>Category</text></group>.
		HashMap<String, String> categories;
		Vector<TetraXmlNode> nodes;
		String message;
		if (tetra_parse_xml(FileAccess::get_file_as_string(data_dir.path_join("kdenliveeffectscategory.rc")), nodes, message) == OK) {
			Vector<int> groups;
			_collect_assets(nodes, 0, "group", groups);
			for (int group : groups) {
				const String category = _child_text(nodes, group, "text");
				for (const String &id : nodes[group].get_attribute("list").split(",", false)) {
					categories[id.strip_edges()] = category;
				}
			}
		}
		const String effects_dir = data_dir.path_join("effects");
		PackedStringArray files = _xml_files(effects_dir);
		PackedStringArray subdirs = DirAccess::get_directories_at(effects_dir);
		subdirs.sort();
		for (const String &subdir : subdirs) {
			// "templates" holds saved effect stacks and "update" migration scripts, not effects.
			if (subdir != "templates" && subdir != "update") {
				files.append_array(_xml_files(effects_dir.path_join(subdir)));
			}
		}
		for (const String &file : files) {
			_load_assets(file, false, categories, p_width, p_height, r_catalog);
		}
		for (const String &file : _xml_files(data_dir.path_join("transitions"))) {
			_load_assets(file, true, categories, p_width, p_height, r_catalog);
		}
		for (const Dictionary &effect : r_catalog.effects) {
			covered_services.insert(effect["service"]);
		}
	}
	// MLT filters Kdenlive has no description for are offered from MLT's own metadata.
	for (const String &name : TetraMltEngine::list_services("filter")) {
		if (covered_services.has(name)) {
			continue;
		}
		Dictionary operation = TetraMltEngine::get_service_operation("filter", name);
		if (operation.is_empty() || !bool(operation["has_metadata"]) || bool(operation["hidden"])) {
			continue;
		}
		operation["fixed"] = Dictionary();
		operation["available"] = true;
		r_catalog.by_id[operation["id"]] = operation;
		r_catalog.effects.push_back(operation);
	}
}

const Catalog &_get_catalog(int p_width, int p_height) {
	const Vector2i key(p_width, p_height);
	if (!catalogs.has(key)) {
		// A handful of sizes is all a session uses: the project's and a render's.
		if (catalogs.size() >= 4) {
			catalogs.clear();
		}
		_build_catalog(p_width, p_height, catalogs[key]);
	}
	return catalogs[key];
}

TypedArray<Dictionary> _list(const Vector<Dictionary> &p_operations, bool p_available_only) {
	TypedArray<Dictionary> result;
	for (const Dictionary &operation : p_operations) {
		if (!p_available_only || bool(operation["available"])) {
			result.push_back(operation.duplicate(true));
		}
	}
	return result;
}

} // namespace

// The first existing candidate wins: the project setting, TETRA_KDENLIVE_DATA,
// the source tree named by TETRA_REPO_ROOT, the source tree the editor binary
// was built in, and finally an installed Kdenlive.
String TetraVideoAssets::get_data_dir() {
	PackedStringArray candidates;
	if (ProjectSettings::get_singleton() && ProjectSettings::get_singleton()->has_setting("tetra/video/kdenlive_data_dir")) {
		candidates.push_back(ProjectSettings::get_singleton()->globalize_path(GLOBAL_GET("tetra/video/kdenlive_data_dir")));
	}
	candidates.push_back(OS::get_singleton()->get_environment("TETRA_KDENLIVE_DATA"));
	if (OS::get_singleton()->has_environment("TETRA_REPO_ROOT")) {
		candidates.push_back(OS::get_singleton()->get_environment("TETRA_REPO_ROOT").path_join("upstream/kdenlive/data"));
	}
	candidates.push_back(OS::get_singleton()->get_executable_path().get_base_dir().path_join("../../kdenlive/data").simplify_path());
	candidates.push_back("/usr/share/kdenlive");
	for (const String &candidate : candidates) {
		if (!candidate.is_empty() && DirAccess::dir_exists_absolute(candidate.path_join("effects"))) {
			return candidate;
		}
	}
	return String();
}

TypedArray<Dictionary> TetraVideoAssets::get_effects(bool p_available_only, int p_width, int p_height) {
	MutexLock lock(catalog_mutex);
	return _list(_get_catalog(p_width, p_height).effects, p_available_only);
}

TypedArray<Dictionary> TetraVideoAssets::get_transitions(bool p_available_only, int p_width, int p_height) {
	MutexLock lock(catalog_mutex);
	return _list(_get_catalog(p_width, p_height).transitions, p_available_only);
}

Dictionary TetraVideoAssets::get_operation(const String &p_id, int p_width, int p_height) {
	MutexLock lock(catalog_mutex);
	const Dictionary *operation = _get_catalog(p_width, p_height).by_id.getptr(p_id);
	return operation ? operation->duplicate(true) : Dictionary();
}

Dictionary TetraVideoAssets::get_default_values(const Dictionary &p_operation) {
	Dictionary values;
	const Array params = p_operation.get("params", Array());
	for (const Variant &entry : params) {
		const Dictionary param = entry;
		values[param["name"]] = param.get("default", Variant());
	}
	return values;
}

Dictionary TetraVideoAssets::values_to_mlt_properties(const Dictionary &p_operation, const Dictionary &p_values) {
	Dictionary properties;
	const Dictionary fixed = p_operation.get("fixed", Dictionary());
	for (const KeyValue<Variant, Variant> &entry : fixed) {
		properties[entry.key] = entry.value.operator String();
	}
	const Array params = p_operation.get("params", Array());
	HashSet<String> known;
	for (const Variant &entry : params) {
		const Dictionary param = entry;
		const String name = param["name"];
		const String type = param.get("type", "string");
		known.insert(name);
		const bool given = p_values.has(name);
		const Variant value = given ? p_values[name] : param.get("default", Variant());
		// Without a default in MLT's metadata the service's built-in one stays until the user changes the value.
		if (bool(param.get("no_default", false)) && (!given || value == param.get("default", Variant()))) {
			continue;
		}
		String text;
		if (type == "int" || type == "float") {
			const double factor = param.get("factor", 1.0);
			const double number = double(value) / factor;
			text = type == "int" && factor == 1.0 ? itos(int64_t(Math::round(number))) : String::num(number);
		} else if (type == "bool") {
			if (param.has("switch_on")) {
				text = bool(value) ? param["switch_on"] : param["switch_off"];
			} else {
				text = bool(value) ? "1" : "0";
			}
		} else if (type == "color") {
			const Color color = value;
			const bool alpha = param.get("alpha", true);
			if (String(param.get("color_format", "0x")) == "#") {
				text = alpha ? "#" + color.to_html(true).substr(6, 2) + color.to_html(false) : "#" + color.to_html(false);
			} else {
				text = "0x" + color.to_html(true);
			}
		} else {
			text = TetraVideoProject::value_to_mlt(value);
			// An empty file, text or raw value means "not set".
			if (text.is_empty()) {
				continue;
			}
		}
		properties[name] = text;
	}
	// Values the schema does not know (set by hand or loaded from a foreign file) pass through.
	for (const KeyValue<Variant, Variant> &entry : p_values) {
		if (!known.has(entry.key)) {
			properties[entry.key] = TetraVideoProject::value_to_mlt(entry.value);
		}
	}
	return properties;
}

Dictionary TetraVideoAssets::effect_to_mlt_properties(const String &p_operation_id, const Dictionary &p_values, int p_width, int p_height, int p_length) {
	Dictionary operation;
	{
		MutexLock lock(catalog_mutex);
		const Dictionary *found = _get_catalog(p_width, p_height).by_id.getptr(p_operation_id);
		if (found) {
			operation = *found;
		}
	}
	Dictionary properties = values_to_mlt_properties(operation, p_values);
	for (const Variant &key : properties.keys()) {
		properties[key] = _resolve_clip(properties[key], p_length);
	}
	return properties;
}

void TetraVideoAssets::clear_cache() {
	MutexLock lock(catalog_mutex);
	catalogs.clear();
}

void TetraVideoAssets::_bind_methods() {
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("get_data_dir"), &TetraVideoAssets::get_data_dir);
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("get_effects", "available_only", "width", "height"), &TetraVideoAssets::get_effects, DEFVAL(true), DEFVAL(1920), DEFVAL(1080));
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("get_transitions", "available_only", "width", "height"), &TetraVideoAssets::get_transitions, DEFVAL(true), DEFVAL(1920), DEFVAL(1080));
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("get_operation", "id", "width", "height"), &TetraVideoAssets::get_operation, DEFVAL(1920), DEFVAL(1080));
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("get_default_values", "operation"), &TetraVideoAssets::get_default_values);
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("values_to_mlt_properties", "operation", "values"), &TetraVideoAssets::values_to_mlt_properties);
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("effect_to_mlt_properties", "operation_id", "values", "width", "height", "length"), &TetraVideoAssets::effect_to_mlt_properties);
	ClassDB::bind_static_method("TetraVideoAssets", D_METHOD("clear_cache"), &TetraVideoAssets::clear_cache);
}
