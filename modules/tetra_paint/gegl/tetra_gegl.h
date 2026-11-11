#pragma once

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/dictionary.h"

// The GEGL filter bridge: every GEGL operation that works on one image,
// described with the shared operation schema and applied through libgegl.
//
// The class only has static methods. When the editor was built without
// libgegl-0.4, is_available() is false, the list is empty and apply() fails.
// GEGL is initialised on first use. All methods may be called from any
// thread; calls into GEGL are serialised.
class TetraGegl : public Object {
	GDCLASS(TetraGegl, Object);

protected:
	static void _bind_methods();

public:
	static bool is_available();
	// The version of libgegl the editor was built against, e.g. "0.4.48"; empty without GEGL.
	static String get_version();
	// Operation schemas (id "gegl:<name>", source "gegl") found by introspection.
	static Array list_operations();
	// Runs one operation on an image and returns the result as FORMAT_RGBAF
	// with the size of the input. 8-bit input is taken as sRGB, float input as
	// linear. Returns null on failure.
	static Ref<Image> apply(const String &p_operation_id, const Dictionary &p_values, const Ref<Image> &p_image);

	// The same on raw linear RGBA float pixels; p_src and r_dst hold
	// p_width * p_height * 4 floats and must not overlap.
	static Error apply_pixels(const String &p_operation_id, const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height);

	// Announces GEGL in the engine registry of the shared module.
	static void register_engine();
	// Shuts GEGL down if it was started. Called when the module unloads.
	static void shutdown();
};
