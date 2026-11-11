#include "tetra_gegl.h"

#include "../tetra_paint_utils.h"

#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

#include <cstring>

#if __has_include("tetra/tetra_engine_registry.h")
#include "tetra/tetra_engine_registry.h"
#define TETRA_HAS_ENGINE_REGISTRY
#endif

#ifdef TETRA_GEGL_ENABLED

// GLib redefines MIN, MAX and CLAMP as macros, so its headers come last.
#include <gegl-paramspecs.h>
#include <gegl.h>

static Mutex gegl_mutex;
static bool gegl_started = false;
static bool gegl_listed = false;
static Array gegl_operations;
static HashSet<String> gegl_operation_ids;

// GLib and GEGL messages only show up with --verbose, so a missing optional
// plug-in dependency does not spam the editor console.
static void log_handler(const gchar *p_domain, GLogLevelFlags p_level, const gchar *p_message, gpointer p_userdata) {
	print_verbose(vformat("%s: %s", p_domain ? p_domain : "GLib", String::utf8(p_message)));
}

// Starts GEGL once. Must be called with gegl_mutex held.
static void ensure_started() {
	if (gegl_started) {
		return;
	}
	g_log_set_default_handler(log_handler, nullptr);
	// GPL3 makes the operations under GPL3+ available; Tetra is GPL-3.0-or-later.
	g_object_set(gegl_config(), "application-license", "GPL3", "use-opencl", FALSE, nullptr);
	gegl_init(nullptr, nullptr);
	gegl_started = true;
}

struct CategoryRule {
	const char *category;
	const char *menu;
};

// GEGL category to menu path, in order of precedence; mirrors where GIMP
// puts the same filters.
static const CategoryRule category_rules[] = {
	{ "blur", "Filters/Blur" },
	{ "sharpen", "Filters/Enhance" },
	{ "enhance", "Filters/Enhance" },
	{ "denoise", "Filters/Enhance" },
	{ "distort", "Filters/Distorts" },
	{ "light", "Filters/Light and Shadow" },
	{ "noise", "Filters/Noise" },
	{ "edge", "Filters/Edge-Detect" },
	{ "edge-detect", "Filters/Edge-Detect" },
	{ "generic", "Filters/Generic" },
	{ "artistic", "Filters/Artistic" },
	{ "map", "Filters/Map" },
	{ "render", "Filters/Render" },
	{ "tonemapping", "Colors/Tone Mapping" },
	{ "color", "Colors" },
	{ "grayscale", "Colors" },
	{ "dither", "Colors" },
	{ "transform", "Filters/Transform" },
	{ "decor", "Filters/Decor" },
	{ "illusions", "Filters/Map" },
	{ "scramble", "Filters/Noise" },
	{ "stylize", "Filters/Artistic" },
	{ "matting", "Filters/Generic" },
	{ "frequency", "Filters/Generic" },
};

// Categories of operations that are graph plumbing, need files or a display,
// or are otherwise not a filter for one image.
static const char *excluded_categories[] = { "hidden", "core", "programming", "meta", "input", "output", "display", "debug", "video", "compositors", "porter-duff", "blend", "svgfilter", "json" };

// Operations in ordinary categories that still are infrastructure.
static const char *excluded_operations[] = { "gegl:gegl", "gegl:nop", "gegl:clone", "gegl:cache", "gegl:layer", "gegl:load", "gegl:save", "gegl:introspect", "gegl:convert-format", "gegl:cast-format", "gegl:convert-space", "gegl:cast-space", "gegl:copy-buffer", "gegl:write-buffer", "gegl:buffer-source", "gegl:buffer-sink", "gegl:pixbuf", "gegl:save-pixbuf", "gegl:magick-load", "gegl:open-buffer", "gegl:crop", "gegl:translate", "gegl:scale-ratio", "gegl:scale-size", "gegl:scale-size-keepaspect", "gegl:reset-origin", "gegl:border-align", "gegl:abyss-policy" };

static bool is_finite_limit(double p_value) {
	return p_value > -1e15 && p_value < 1e15;
}

static String unit_suffix(const char *p_operation, const char *p_property) {
	const gchar *unit = gegl_operation_get_property_key(p_operation, p_property, "unit");
	if (!unit) {
		return String();
	}
	if (strcmp(unit, "degree") == 0) {
		return String::utf8("°");
	}
	if (strncmp(unit, "pixel-", 6) == 0) {
		return "px";
	}
	return String();
}

// Describes one GObject property as a schema parameter. Returns false for
// property types the schema cannot carry (buffers, paths, curves, formats);
// those keep their defaults.
static bool describe_property(const char *p_operation, GParamSpec *p_spec, Dictionary &r_param) {
	const char *name = g_param_spec_get_name(p_spec);
	const char *nick = g_param_spec_get_nick(p_spec);
	const char *blurb = g_param_spec_get_blurb(p_spec);
	r_param["name"] = String::utf8(name);
	r_param["label"] = (nick && nick[0]) ? String::utf8(nick) : String::utf8(name).capitalize();
	if (blurb && blurb[0]) {
		r_param["description"] = String::utf8(blurb);
	}
	const String unit = unit_suffix(p_operation, name);
	if (!unit.is_empty()) {
		r_param["unit"] = unit;
	}

	if (G_IS_PARAM_SPEC_DOUBLE(p_spec)) {
		const GParamSpecDouble *spec = G_PARAM_SPEC_DOUBLE(p_spec);
		r_param["type"] = "float";
		r_param["default"] = spec->default_value;
		if (is_finite_limit(spec->minimum)) {
			r_param["min"] = spec->minimum;
		}
		if (is_finite_limit(spec->maximum)) {
			r_param["max"] = spec->maximum;
		}
		if (GEGL_IS_PARAM_SPEC_DOUBLE(p_spec)) {
			const GeglParamSpecDouble *gegl_spec = GEGL_PARAM_SPEC_DOUBLE(p_spec);
			if (is_finite_limit(gegl_spec->ui_minimum) && is_finite_limit(gegl_spec->ui_maximum) && gegl_spec->ui_minimum < gegl_spec->ui_maximum) {
				r_param["ui_min"] = gegl_spec->ui_minimum;
				r_param["ui_max"] = gegl_spec->ui_maximum;
			}
		}
		return true;
	}
	if (GEGL_IS_PARAM_SPEC_SEED(p_spec)) {
		r_param["type"] = "seed";
		r_param["default"] = 0;
		r_param["min"] = 0.0;
		r_param["max"] = double(G_PARAM_SPEC_UINT(p_spec)->maximum);
		return true;
	}
	if (G_IS_PARAM_SPEC_INT(p_spec)) {
		const GParamSpecInt *spec = G_PARAM_SPEC_INT(p_spec);
		r_param["type"] = "int";
		r_param["default"] = spec->default_value;
		r_param["min"] = double(spec->minimum);
		r_param["max"] = double(spec->maximum);
		if (GEGL_IS_PARAM_SPEC_INT(p_spec)) {
			const GeglParamSpecInt *gegl_spec = GEGL_PARAM_SPEC_INT(p_spec);
			if (gegl_spec->ui_minimum < gegl_spec->ui_maximum) {
				r_param["ui_min"] = double(gegl_spec->ui_minimum);
				r_param["ui_max"] = double(gegl_spec->ui_maximum);
			}
		}
		r_param["step"] = 1.0;
		return true;
	}
	if (G_IS_PARAM_SPEC_UINT(p_spec)) {
		const GParamSpecUInt *spec = G_PARAM_SPEC_UINT(p_spec);
		r_param["type"] = "int";
		r_param["default"] = int64_t(spec->default_value);
		r_param["min"] = double(spec->minimum);
		r_param["max"] = double(spec->maximum);
		r_param["step"] = 1.0;
		return true;
	}
	if (G_IS_PARAM_SPEC_BOOLEAN(p_spec)) {
		r_param["type"] = "bool";
		r_param["default"] = bool(G_PARAM_SPEC_BOOLEAN(p_spec)->default_value);
		return true;
	}
	if (G_IS_PARAM_SPEC_ENUM(p_spec)) {
		const GParamSpecEnum *spec = G_PARAM_SPEC_ENUM(p_spec);
		Array options;
		String default_nick;
		for (guint i = 0; i < spec->enum_class->n_values; i++) {
			const GEnumValue &value = spec->enum_class->values[i];
			Dictionary option;
			option["value"] = String::utf8(value.value_nick);
			option["label"] = String::utf8(value.value_name);
			options.push_back(option);
			if (value.value == spec->default_value || i == 0) {
				default_nick = String::utf8(value.value_nick);
			}
		}
		if (options.is_empty()) {
			return false;
		}
		r_param["type"] = "enum";
		r_param["options"] = options;
		r_param["default"] = default_nick;
		return true;
	}
	if (G_IS_PARAM_SPEC_STRING(p_spec)) {
		const gchar *default_value = G_PARAM_SPEC_STRING(p_spec)->default_value;
		const gchar *multiline = gegl_operation_get_property_key(p_operation, name, "multiline");
		if (GEGL_IS_PARAM_SPEC_FILE_PATH(p_spec)) {
			r_param["type"] = "file";
		} else {
			r_param["type"] = (multiline && strcmp(multiline, "true") == 0) ? "text" : "string";
		}
		r_param["default"] = default_value ? String::utf8(default_value) : String();
		return true;
	}
	if (GEGL_IS_PARAM_SPEC_COLOR(p_spec)) {
		gfloat rgba[4] = { 0, 0, 0, 1 };
		GeglColor *color = gegl_param_spec_color_get_default(p_spec);
		if (color) {
			gegl_color_get_pixel(color, babl_format("RGBA float"), rgba);
		}
		r_param["type"] = "color";
		r_param["default"] = Color(rgba[0], rgba[1], rgba[2], rgba[3]);
		return true;
	}
	return false;
}

// Builds the schema of one operation, or returns false if it is not a filter
// or generator for a single image.
static bool describe_operation(GeglNode *p_graph, const char *p_name, Dictionary &r_operation) {
	if (strncmp(p_name, "gegl:", 5) != 0) {
		return false;
	}
	for (const char *excluded : excluded_operations) {
		if (strcmp(p_name, excluded) == 0) {
			return false;
		}
	}
	if (strstr(p_name, "-load") || strstr(p_name, "-save")) {
		return false;
	}
	const gchar *categories_key = gegl_operation_get_key(p_name, "categories");
	const PackedStringArray categories = String::utf8(categories_key ? categories_key : "").split(":", false);
	for (const char *excluded : excluded_categories) {
		if (categories.has(excluded)) {
			return false;
		}
	}

	// A usable operation produces an image and takes at most the main input.
	GeglNode *node = gegl_node_new_child(p_graph, "operation", p_name, nullptr);
	bool usable = gegl_node_has_pad(node, "output");
	gchar **inputs = gegl_node_list_input_pads(node);
	for (gchar **pad = inputs; usable && pad && *pad; pad++) {
		usable = strcmp(*pad, "input") == 0;
	}
	g_strfreev(inputs);
	gegl_node_remove_child(p_graph, node);
	if (!usable) {
		return false;
	}

	String menu = "Filters/Generic";
	bool matched = false;
	for (const CategoryRule &rule : category_rules) {
		if (categories.has(rule.category)) {
			menu = rule.menu;
			matched = true;
			break;
		}
	}
	if (!matched && !categories.is_empty()) {
		menu = "Filters/" + categories[0].capitalize();
	}

	const gchar *title = gegl_operation_get_key(p_name, "title");
	const gchar *description = gegl_operation_get_key(p_name, "description");
	r_operation["id"] = String::utf8(p_name);
	r_operation["label"] = (title && title[0]) ? String::utf8(title) : String::utf8(p_name + 5).capitalize();
	r_operation["description"] = description ? String::utf8(description) : String();
	r_operation["category"] = menu;
	r_operation["source"] = "gegl";

	// An operation driven by a path, curve, buffer or other object cannot be
	// configured through plain values, so it is left out.
	Array params;
	bool configurable = true;
	guint count = 0;
	GParamSpec **specs = gegl_operation_list_properties(p_name, &count);
	for (guint i = 0; i < count; i++) {
		Dictionary param;
		if (describe_property(p_name, specs[i], param)) {
			params.push_back(param);
		} else if (!GEGL_IS_PARAM_SPEC_FORMAT(specs[i])) {
			const GType value_type = G_PARAM_SPEC_VALUE_TYPE(specs[i]);
			if (g_type_is_a(value_type, G_TYPE_OBJECT) || value_type == G_TYPE_POINTER || G_TYPE_IS_BOXED(value_type)) {
				configurable = false;
			}
		}
	}
	g_free(specs);
	r_operation["params"] = params;
	return configurable;
}

// Fills the operation list once. Must be called with gegl_mutex held.
static void ensure_listed() {
	if (gegl_listed) {
		return;
	}
	ensure_started();
	GeglNode *graph = gegl_node_new();
	guint count = 0;
	gchar **names = gegl_list_operations(&count);
	for (guint i = 0; i < count; i++) {
		Dictionary operation;
		if (describe_operation(graph, names[i], operation)) {
			gegl_operations.push_back(operation);
			gegl_operation_ids.insert(operation["id"]);
		}
	}
	g_free(names);
	g_object_unref(graph);
	gegl_listed = true;
}

// Sets one property of a node from a Variant, converting by the property's
// declared type. Values of the wrong kind are ignored.
static void set_property(GeglNode *p_node, GParamSpec *p_spec, const Variant &p_value) {
	GValue value = G_VALUE_INIT;
	g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(p_spec));
	bool valid = true;
	const bool numeric = p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::BOOL;
	if (G_IS_PARAM_SPEC_DOUBLE(p_spec) && numeric) {
		g_value_set_double(&value, double(p_value));
	} else if (G_IS_PARAM_SPEC_INT(p_spec) && numeric) {
		g_value_set_int(&value, int(p_value));
	} else if (G_IS_PARAM_SPEC_UINT(p_spec) && numeric) {
		g_value_set_uint(&value, guint(uint64_t(int64_t(p_value)) & 0xffffffffu));
	} else if (G_IS_PARAM_SPEC_BOOLEAN(p_spec) && numeric) {
		g_value_set_boolean(&value, p_value.booleanize());
	} else if (G_IS_PARAM_SPEC_ENUM(p_spec)) {
		GEnumClass *enum_class = G_PARAM_SPEC_ENUM(p_spec)->enum_class;
		const GEnumValue *enum_value = nullptr;
		if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
			enum_value = g_enum_get_value_by_nick(enum_class, String(p_value).utf8().get_data());
		} else if (numeric) {
			enum_value = g_enum_get_value(enum_class, int(p_value));
		}
		valid = enum_value != nullptr;
		if (valid) {
			g_value_set_enum(&value, enum_value->value);
		}
	} else if (G_IS_PARAM_SPEC_STRING(p_spec) && (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME)) {
		g_value_set_string(&value, String(p_value).utf8().get_data());
	} else if (GEGL_IS_PARAM_SPEC_COLOR(p_spec) && p_value.get_type() == Variant::COLOR) {
		const Color color = p_value;
		const gfloat rgba[4] = { color.r, color.g, color.b, color.a };
		GeglColor *gegl_color = gegl_color_new(nullptr);
		gegl_color_set_pixel(gegl_color, babl_format("RGBA float"), rgba);
		g_value_take_object(&value, gegl_color);
	} else {
		valid = false;
	}
	if (valid) {
		gegl_node_set_property(p_node, g_param_spec_get_name(p_spec), &value);
	}
	g_value_unset(&value);
}

bool TetraGegl::is_available() {
	return true;
}

String TetraGegl::get_version() {
	// gegl_get_version() is declared without C linkage in gegl-version.h, so
	// the version comes from the header macros instead.
	return vformat("%d.%d.%d", GEGL_MAJOR_VERSION, GEGL_MINOR_VERSION, GEGL_MICRO_VERSION);
}

Array TetraGegl::list_operations() {
	bool first = false;
	Array operations;
	{
		MutexLock lock(gegl_mutex);
		first = !gegl_listed;
		ensure_listed();
		operations = gegl_operations.duplicate(true);
	}
	if (first) {
		// The registry entry now carries the real count; registries are main-thread data.
		if (Thread::is_main_thread()) {
			register_engine();
		} else {
			callable_mp_static(&TetraGegl::register_engine).call_deferred();
		}
	}
	return operations;
}

Error TetraGegl::apply_pixels(const String &p_operation_id, const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	ERR_FAIL_COND_V(p_width < 1 || p_height < 1, ERR_INVALID_PARAMETER);
	MutexLock lock(gegl_mutex);
	ensure_listed();
	ERR_FAIL_COND_V_MSG(!gegl_operation_ids.has(p_operation_id), ERR_DOES_NOT_EXIST, vformat("Unknown GEGL operation \"%s\".", p_operation_id));
	const CharString name = p_operation_id.utf8();

	// The source pixels are wrapped, not copied: "RGBA float" is babl's name
	// for linear RGB with straight alpha, which is what layers store.
	const GeglRectangle extent = { 0, 0, p_width, p_height };
	const Babl *format = babl_format("RGBA float");
	GeglBuffer *source_buffer = gegl_buffer_linear_new_from_data(const_cast<float *>(p_src), format, &extent, GEGL_AUTO_ROWSTRIDE, nullptr, nullptr);
	GeglNode *graph = gegl_node_new();
	GeglNode *source = gegl_node_new_child(graph, "operation", "gegl:buffer-source", "buffer", source_buffer, nullptr);
	GeglNode *operation = gegl_node_new_child(graph, "operation", name.get_data(), nullptr);

	guint count = 0;
	GParamSpec **specs = gegl_operation_list_properties(name.get_data(), &count);
	for (guint i = 0; i < count; i++) {
		const String property = String::utf8(g_param_spec_get_name(specs[i]));
		if (p_values.has(property)) {
			set_property(operation, specs[i], p_values[property]);
		}
	}
	g_free(specs);

	if (gegl_node_has_pad(operation, "input")) {
		gegl_node_link(source, operation);
	}
	// Only the canvas rectangle is fetched, whatever extent the operation
	// reports (blurs grow it, generators are unbounded).
	gegl_node_blit(operation, 1.0, &extent, format, r_dst, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
	g_object_unref(graph);
	g_object_unref(source_buffer);
	return OK;
}

void TetraGegl::register_engine() {
#ifdef TETRA_HAS_ENGINE_REGISTRY
	String details = "libgegl-0.4; the operations are listed on first use";
	{
		MutexLock lock(gegl_mutex);
		if (gegl_listed) {
			details = vformat("%d operations", gegl_operations.size());
		}
	}
	TetraEngineRegistry::add_engine("GEGL", get_version(), true, details);
#endif
}

void TetraGegl::shutdown() {
	MutexLock lock(gegl_mutex);
	gegl_operations.clear();
	gegl_operation_ids.clear();
	gegl_listed = false;
	if (gegl_started) {
		gegl_exit();
		gegl_started = false;
	}
}

#else // !TETRA_GEGL_ENABLED

bool TetraGegl::is_available() {
	return false;
}

String TetraGegl::get_version() {
	return String();
}

Array TetraGegl::list_operations() {
	return Array();
}

Error TetraGegl::apply_pixels(const String &p_operation_id, const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "This build has no GEGL support (libgegl-0.4 was not found when it was compiled).");
}

void TetraGegl::register_engine() {
#ifdef TETRA_HAS_ENGINE_REGISTRY
	TetraEngineRegistry::add_engine("GEGL", String(), false, "Built without libgegl-0.4");
#endif
}

void TetraGegl::shutdown() {
}

#endif // TETRA_GEGL_ENABLED

Ref<Image> TetraGegl::apply(const String &p_operation_id, const Dictionary &p_values, const Ref<Image> &p_image) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Ref<Image>());
	const Vector<float> source = tetra_image_to_linear(p_image, false);
	ERR_FAIL_COND_V(source.is_empty(), Ref<Image>());
	const int width = p_image->get_width();
	const int height = p_image->get_height();
	Vector<uint8_t> data;
	data.resize(int64_t(width) * height * 16);
	if (apply_pixels(p_operation_id, p_values, source.ptr(), reinterpret_cast<float *>(data.ptrw()), width, height) != OK) {
		return Ref<Image>();
	}
	return Image::create_from_data(width, height, false, Image::FORMAT_RGBAF, data);
}

void TetraGegl::_bind_methods() {
	ClassDB::bind_static_method("TetraGegl", D_METHOD("is_available"), &TetraGegl::is_available);
	ClassDB::bind_static_method("TetraGegl", D_METHOD("get_version"), &TetraGegl::get_version);
	ClassDB::bind_static_method("TetraGegl", D_METHOD("list_operations"), &TetraGegl::list_operations);
	ClassDB::bind_static_method("TetraGegl", D_METHOD("apply", "operation_id", "values", "image"), &TetraGegl::apply);
}
