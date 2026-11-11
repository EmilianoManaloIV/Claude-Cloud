// Native colour adjustments and filters. The colour operations are ported
// from GIMP (app/operations/gimpoperationdesaturate.c,
// gimpoperationbrightnesscontrast.c, gimpoperationlevels.c,
// gimpoperationhuesaturation.c, gimpoperationcolorbalance.c,
// gimpoperationcolorize.c, gimpoperationthreshold.c,
// gimpoperationposterize.c; Copyright (C) 1995 Spencer Kimball and Peter
// Mattis, 2007-2012 Michael Natterer) and the edge, emboss and noise filters
// from GEGL (operations/common/edge-sobel.c, noise-rgb.c and
// operations/common-gpl3+/emboss.c; Copyright (C) 1997 Eric L. Hernes,
// 2011 Victor Oliveira, 1995 Spencer Kimball and Peter Mattis).
// GPL-3.0-or-later.

#include "tetra_image_document.h"

#include "gegl/tetra_gegl.h"
#include "tetra_paint_utils.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"

#include <cmath>
#include <cstring>

using TetraLayerModes::linear_to_srgb;
using TetraLayerModes::srgb_to_linear;

static Dictionary make_param(const String &p_name, const String &p_label, const String &p_type, const Variant &p_default, const String &p_description) {
	Dictionary param;
	param["name"] = p_name;
	param["label"] = p_label;
	param["type"] = p_type;
	param["default"] = p_default;
	if (!p_description.is_empty()) {
		param["description"] = p_description;
	}
	return param;
}

static Dictionary float_param(const String &p_name, const String &p_label, double p_default, double p_min, double p_max, const String &p_description = String(), double p_ui_min = 0.0, double p_ui_max = 0.0) {
	Dictionary param = make_param(p_name, p_label, "float", p_default, p_description);
	param["min"] = p_min;
	param["max"] = p_max;
	if (p_ui_min != p_ui_max) {
		param["ui_min"] = p_ui_min;
		param["ui_max"] = p_ui_max;
	}
	return param;
}

static Dictionary int_param(const String &p_name, const String &p_label, int p_default, int p_min, int p_max, const String &p_description = String(), int p_ui_max = 0) {
	Dictionary param = make_param(p_name, p_label, "int", p_default, p_description);
	param["min"] = double(p_min);
	param["max"] = double(p_max);
	if (p_ui_max != 0) {
		param["ui_min"] = double(p_min);
		param["ui_max"] = double(p_ui_max);
	}
	param["step"] = 1.0;
	return param;
}

static Dictionary bool_param(const String &p_name, const String &p_label, bool p_default, const String &p_description = String()) {
	return make_param(p_name, p_label, "bool", p_default, p_description);
}

// p_options alternates option values and their labels.
static Dictionary enum_param(const String &p_name, const String &p_label, const String &p_default, const Vector<String> &p_options, const String &p_description = String()) {
	Dictionary param = make_param(p_name, p_label, "enum", p_default, p_description);
	Array options;
	for (int i = 0; i + 1 < p_options.size(); i += 2) {
		Dictionary option;
		option["value"] = p_options[i];
		option["label"] = p_options[i + 1];
		options.push_back(option);
	}
	param["options"] = options;
	return param;
}

static Dictionary make_operation(const String &p_name, const String &p_label, const String &p_category, const String &p_description, const Array &p_params) {
	Dictionary operation;
	operation["id"] = "native:" + p_name;
	operation["label"] = p_label;
	operation["description"] = p_description;
	operation["category"] = p_category;
	operation["source"] = "native";
	operation["params"] = p_params;
	return operation;
}

Array TetraImageDocument::get_native_operations() {
	Array operations;
	{
		Array params;
		params.push_back(bool_param("linear", TTR("Linear"), false, TTR("Invert in linear light instead of perceptual space.")));
		operations.push_back(make_operation("invert", TTR("Invert"), "Colors", TTR("Invert the colors."), params));
	}
	{
		Array params;
		params.push_back(enum_param("mode", TTR("Mode"), "luminance", { "luminance", TTR("Luminance"), "luma", TTR("Luma"), "lightness", TTR("Lightness (HSL)"), "average", TTR("Average (HSI Intensity)"), "value", TTR("Value (HSV)") }, TTR("How the gray value is calculated.")));
		operations.push_back(make_operation("desaturate", TTR("Desaturate"), "Colors/Desaturate", TTR("Turn colors into shades of gray."), params));
	}
	{
		Array params;
		params.push_back(float_param("brightness", TTR("Brightness"), 0.0, -1.0, 1.0));
		params.push_back(float_param("contrast", TTR("Contrast"), 0.0, -1.0, 1.0));
		operations.push_back(make_operation("brightness-contrast", TTR("Brightness-Contrast"), "Colors", TTR("Adjust brightness and contrast."), params));
	}
	{
		Array params;
		params.push_back(enum_param("channel", TTR("Channel"), "value", { "value", TTR("Value"), "red", TTR("Red"), "green", TTR("Green"), "blue", TTR("Blue"), "alpha", TTR("Alpha") }, TTR("The channel the levels apply to.")));
		params.push_back(float_param("low-input", TTR("Low Input"), 0.0, 0.0, 1.0));
		params.push_back(float_param("high-input", TTR("High Input"), 1.0, 0.0, 1.0));
		params.push_back(float_param("gamma", TTR("Gamma"), 1.0, 0.1, 10.0));
		params.push_back(float_param("low-output", TTR("Low Output"), 0.0, 0.0, 1.0));
		params.push_back(float_param("high-output", TTR("High Output"), 1.0, 0.0, 1.0));
		params.push_back(bool_param("clamp-input", TTR("Clamp Input"), false, TTR("Clamp input values before applying output levels.")));
		params.push_back(bool_param("clamp-output", TTR("Clamp Output"), false, TTR("Clamp final output values.")));
		params.push_back(enum_param("trc", TTR("Working Space"), "non-linear", { "non-linear", TTR("Perceptual"), "linear", TTR("Linear light") }, TTR("Work on perceptual or on linear values.")));
		operations.push_back(make_operation("levels", TTR("Levels"), "Colors", TTR("Adjust color levels."), params));
	}
	{
		Array params;
		params.push_back(float_param("hue", TTR("Hue"), 0.0, -1.0, 1.0, TTR("Hue shift; 1 is half a turn of the colour wheel.")));
		params.push_back(float_param("lightness", TTR("Lightness"), 0.0, -1.0, 1.0));
		params.push_back(float_param("saturation", TTR("Saturation"), 0.0, -1.0, 1.0));
		operations.push_back(make_operation("hue-saturation", TTR("Hue-Saturation"), "Colors", TTR("Adjust hue, saturation and lightness."), params));
	}
	{
		Array params;
		static const char *ranges[3] = { "shadows", "midtones", "highlights" };
		static const char *range_labels[3] = { "Shadows", "Midtones", "Highlights" };
		for (int i = 0; i < 3; i++) {
			const String prefix = String(ranges[i]) + "-";
			Dictionary cyan_red = float_param(prefix + "cyan-red", TTR("Cyan-Red"), 0.0, -1.0, 1.0);
			Dictionary magenta_green = float_param(prefix + "magenta-green", TTR("Magenta-Green"), 0.0, -1.0, 1.0);
			Dictionary yellow_blue = float_param(prefix + "yellow-blue", TTR("Yellow-Blue"), 0.0, -1.0, 1.0);
			cyan_red["group"] = TTR(range_labels[i]);
			magenta_green["group"] = TTR(range_labels[i]);
			yellow_blue["group"] = TTR(range_labels[i]);
			params.push_back(cyan_red);
			params.push_back(magenta_green);
			params.push_back(yellow_blue);
		}
		params.push_back(bool_param("preserve-luminosity", TTR("Preserve Luminosity"), true));
		operations.push_back(make_operation("color-balance", TTR("Color Balance"), "Colors", TTR("Adjust the color distribution."), params));
	}
	{
		Array params;
		params.push_back(float_param("hue", TTR("Hue"), 0.5, 0.0, 1.0));
		params.push_back(float_param("saturation", TTR("Saturation"), 0.5, 0.0, 1.0));
		params.push_back(float_param("lightness", TTR("Lightness"), 0.0, -1.0, 1.0));
		operations.push_back(make_operation("colorize", TTR("Colorize"), "Colors", TTR("Colorize the image with one hue."), params));
	}
	{
		Array params;
		params.push_back(enum_param("channel", TTR("Channel"), "value", { "value", TTR("Value"), "red", TTR("Red"), "green", TTR("Green"), "blue", TTR("Blue"), "alpha", TTR("Alpha"), "rgb", TTR("RGB"), "luminance", TTR("Luminance") }));
		params.push_back(float_param("low", TTR("Low Threshold"), 0.5, 0.0, 1.0));
		params.push_back(float_param("high", TTR("High Threshold"), 1.0, 0.0, 1.0));
		operations.push_back(make_operation("threshold", TTR("Threshold"), "Colors", TTR("Reduce the image to black and white with a threshold."), params));
	}
	{
		Array params;
		params.push_back(int_param("levels", TTR("Posterize Levels"), 3, 2, 256));
		operations.push_back(make_operation("posterize", TTR("Posterize"), "Colors", TTR("Reduce the number of colors."), params));
	}
	{
		Array params;
		Dictionary x = float_param("std-dev-x", TTR("Size X"), 1.5, 0.0, 1500.0, TTR("Standard deviation for the horizontal axis."), 0.24, 100.0);
		Dictionary y = float_param("std-dev-y", TTR("Size Y"), 1.5, 0.0, 1500.0, TTR("Standard deviation for the vertical axis."), 0.24, 100.0);
		x["unit"] = "px";
		y["unit"] = "px";
		params.push_back(x);
		params.push_back(y);
		operations.push_back(make_operation("gaussian-blur", TTR("Gaussian Blur"), "Filters/Blur", TTR("Blur with a Gaussian kernel."), params));
	}
	{
		Array params;
		params.push_back(float_param("std-dev", TTR("Radius"), 3.0, 0.0, 1500.0, TTR("Standard deviation of the blur the image is compared with."), 0.0, 120.0));
		params.push_back(float_param("scale", TTR("Amount"), 0.5, 0.0, 300.0, TTR("Strength of the sharpening."), 0.0, 10.0));
		params.push_back(float_param("threshold", TTR("Threshold"), 0.0, 0.0, 1.0, TTR("Differences below this value are not sharpened.")));
		operations.push_back(make_operation("unsharp-mask", TTR("Sharpen (Unsharp Mask)"), "Filters/Enhance", TTR("Sharpen by adding back the difference to a blurred copy."), params));
	}
	{
		Array params;
		Dictionary x = int_param("size-x", TTR("Block Width"), 16, 1, 123456, String(), 2048);
		Dictionary y = int_param("size-y", TTR("Block Height"), 16, 1, 123456, String(), 2048);
		x["unit"] = "px";
		y["unit"] = "px";
		params.push_back(x);
		params.push_back(y);
		operations.push_back(make_operation("pixelize", TTR("Pixelize"), "Filters/Blur", TTR("Average the image in rectangular blocks."), params));
	}
	{
		Array params;
		params.push_back(bool_param("horizontal", TTR("Horizontal"), true));
		params.push_back(bool_param("vertical", TTR("Vertical"), true));
		params.push_back(bool_param("keep-sign", TTR("Keep Sign"), true, TTR("Keep negative values in the result; when off, the absolute value of the result is used.")));
		operations.push_back(make_operation("edge-sobel", TTR("Sobel"), "Filters/Edge-Detect", TTR("Sobel edge detection."), params));
	}
	{
		Array params;
		params.push_back(enum_param("type", TTR("Emboss Type"), "emboss", { "emboss", TTR("Emboss"), "bumpmap", TTR("Bumpmap (preserve original colors)") }));
		Dictionary azimuth = float_param("azimuth", TTR("Azimuth"), 30.0, 0.0, 360.0, TTR("Direction of the light."));
		Dictionary elevation = float_param("elevation", TTR("Elevation"), 45.0, 0.0, 180.0, TTR("Height of the light above the surface."));
		azimuth["unit"] = String::utf8("°");
		elevation["unit"] = String::utf8("°");
		params.push_back(azimuth);
		params.push_back(elevation);
		params.push_back(int_param("depth", TTR("Depth"), 20, 1, 100, TTR("Filter width.")));
		operations.push_back(make_operation("emboss", TTR("Emboss"), "Filters/Distorts", TTR("Simulate a relief lit from one side."), params));
	}
	{
		Array params;
		params.push_back(bool_param("correlated", TTR("Correlated Noise"), false));
		params.push_back(bool_param("independent", TTR("Independent RGB"), true));
		params.push_back(bool_param("linear", TTR("Linear RGB"), true, TTR("Add the noise in linear light.")));
		params.push_back(bool_param("gaussian", TTR("Gaussian Distribution"), true));
		params.push_back(float_param("red", TTR("Red"), 0.2, 0.0, 1.0));
		params.push_back(float_param("green", TTR("Green"), 0.2, 0.0, 1.0));
		params.push_back(float_param("blue", TTR("Blue"), 0.2, 0.0, 1.0));
		params.push_back(float_param("alpha", TTR("Alpha"), 0.0, 0.0, 1.0));
		params.push_back(make_param("seed", TTR("Random Seed"), "seed", 0, String()));
		operations.push_back(make_operation("noise-rgb", TTR("RGB Noise"), "Filters/Noise", TTR("Add random noise to each channel."), params));
	}
	return operations;
}

Array TetraImageDocument::get_operations() {
	Array operations = get_native_operations();
	operations.append_array(TetraGegl::list_operations());
	return operations;
}

// Parameter access with the schema defaults as fallbacks.

static double value_of(const Dictionary &p_values, const char *p_name, double p_default) {
	const Variant value = p_values.get(p_name, p_default);
	return (value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT || value.get_type() == Variant::BOOL) ? double(value) : p_default;
}

static bool flag_of(const Dictionary &p_values, const char *p_name, bool p_default) {
	return p_values.get(p_name, p_default).booleanize();
}

static String choice_of(const Dictionary &p_values, const char *p_name, const char *p_default) {
	return p_values.get(p_name, p_default);
}

// Runs p_function(in, out) for every pixel on the worker pool.
template <typename F>
static void map_pixels(const float *p_src, float *r_dst, int p_width, int p_height, F &&p_function) {
	tetra_parallel_rows(p_height, p_width * 4, [&](int p_first, int p_end) {
		for (int64_t i = int64_t(p_first) * p_width; i < int64_t(p_end) * p_width; i++) {
			p_function(p_src + i * 4, r_dst + i * 4);
		}
	});
}

static inline void rgb_to_hsl(const float *p_rgb, float *r_hsl) {
	const float max = MAX(p_rgb[0], MAX(p_rgb[1], p_rgb[2]));
	const float min = MIN(p_rgb[0], MIN(p_rgb[1], p_rgb[2]));
	const float lightness = (max + min) * 0.5f;
	float hue = 0.0f;
	float saturation = 0.0f;
	if (max != min) {
		const float delta = max - min;
		saturation = lightness > 0.5f ? delta / (2.0f - max - min) : delta / (max + min);
		if (p_rgb[0] == max) {
			hue = (p_rgb[1] - p_rgb[2]) / delta + (p_rgb[1] < p_rgb[2] ? 6.0f : 0.0f);
		} else if (p_rgb[1] == max) {
			hue = (p_rgb[2] - p_rgb[0]) / delta + 2.0f;
		} else {
			hue = (p_rgb[0] - p_rgb[1]) / delta + 4.0f;
		}
		hue /= 6.0f;
	}
	r_hsl[0] = hue;
	r_hsl[1] = saturation;
	r_hsl[2] = lightness;
}

static inline float hsl_component(float p_n1, float p_n2, float p_hue) {
	if (p_hue > 6.0f) {
		p_hue -= 6.0f;
	} else if (p_hue < 0.0f) {
		p_hue += 6.0f;
	}
	if (p_hue < 1.0f) {
		return p_n1 + (p_n2 - p_n1) * p_hue;
	}
	if (p_hue < 3.0f) {
		return p_n2;
	}
	if (p_hue < 4.0f) {
		return p_n1 + (p_n2 - p_n1) * (4.0f - p_hue);
	}
	return p_n1;
}

static inline void hsl_to_rgb(const float *p_hsl, float *r_rgb) {
	if (p_hsl[1] == 0.0f) {
		r_rgb[0] = r_rgb[1] = r_rgb[2] = p_hsl[2];
		return;
	}
	const float m2 = p_hsl[2] <= 0.5f ? p_hsl[2] * (1.0f + p_hsl[1]) : p_hsl[2] + p_hsl[1] - p_hsl[2] * p_hsl[1];
	const float m1 = 2.0f * p_hsl[2] - m2;
	r_rgb[0] = hsl_component(m1, m2, p_hsl[0] * 6.0f + 2.0f);
	r_rgb[1] = hsl_component(m1, m2, p_hsl[0] * 6.0f);
	r_rgb[2] = hsl_component(m1, m2, p_hsl[0] * 6.0f - 2.0f);
}

static void op_invert(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const bool linear = flag_of(p_values, "linear", false);
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		for (int c = 0; c < 3; c++) {
			r_out[c] = linear ? 1.0f - p_in[c] : srgb_to_linear(1.0f - linear_to_srgb(p_in[c]));
		}
		r_out[3] = p_in[3];
	});
}

static void op_desaturate(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const String mode = choice_of(p_values, "mode", "luminance");
	const int kind = mode == "luma" ? 1 : (mode == "lightness" ? 2 : (mode == "average" ? 3 : (mode == "value" ? 4 : 0)));
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		float value;
		if (kind == 0) {
			value = TetraLayerModes::luminance(p_in);
		} else {
			const float encoded[3] = { linear_to_srgb(p_in[0]), linear_to_srgb(p_in[1]), linear_to_srgb(p_in[2]) };
			const float max = MAX(encoded[0], MAX(encoded[1], encoded[2]));
			const float min = MIN(encoded[0], MIN(encoded[1], encoded[2]));
			switch (kind) {
				case 1:
					value = TetraLayerModes::luminance(encoded);
					break;
				case 2:
					value = (max + min) / 2.0f;
					break;
				case 3:
					value = (encoded[0] + encoded[1] + encoded[2]) / 3.0f;
					break;
				default:
					value = max;
					break;
			}
			value = srgb_to_linear(value);
		}
		r_out[0] = r_out[1] = r_out[2] = value;
		r_out[3] = p_in[3];
	});
}

static void op_brightness_contrast(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const double brightness = CLAMP(value_of(p_values, "brightness", 0.0), -1.0, 1.0) / 2.0;
	const double slant = tan((CLAMP(value_of(p_values, "contrast", 0.0), -1.0, 1.0) + 1.0) * Math::PI / 4.0);
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		for (int c = 0; c < 3; c++) {
			double value = linear_to_srgb(p_in[c]);
			if (brightness < 0.0) {
				value = value * (1.0 + brightness);
			} else {
				value = value + ((1.0 - value) * brightness);
			}
			value = (value - 0.5) * slant + 0.5;
			r_out[c] = srgb_to_linear(float(value));
		}
		r_out[3] = p_in[3];
	});
}

static inline double levels_map(double p_value, double p_low_input, double p_high_input, bool p_clamp_input, double p_inv_gamma, double p_low_output, double p_high_output, bool p_clamp_output) {
	if (p_high_input != p_low_input) {
		p_value = (p_value - p_low_input) / (p_high_input - p_low_input);
	} else {
		p_value = p_value - p_low_input;
	}
	if (p_clamp_input) {
		p_value = CLAMP(p_value, 0.0, 1.0);
	}
	if (p_inv_gamma != 1.0 && p_value > 0.0) {
		p_value = pow(p_value, p_inv_gamma);
	}
	if (p_high_output >= p_low_output) {
		p_value = p_value * (p_high_output - p_low_output) + p_low_output;
	} else {
		p_value = p_low_output - p_value * (p_low_output - p_high_output);
	}
	if (p_clamp_output) {
		p_value = CLAMP(p_value, 0.0, 1.0);
	}
	return p_value;
}

static void op_levels(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	// Slot 0 is the value channel (applied to R, G and B after their own
	// slots), slots 1-4 are red, green, blue and alpha.
	double low_input[5], high_input[5], inv_gamma[5], low_output[5], high_output[5];
	for (int i = 0; i < 5; i++) {
		low_input[i] = 0.0;
		high_input[i] = 1.0;
		inv_gamma[i] = 1.0;
		low_output[i] = 0.0;
		high_output[i] = 1.0;
	}
	const String channel = choice_of(p_values, "channel", "value");
	const int slot = channel == "red" ? 1 : (channel == "green" ? 2 : (channel == "blue" ? 3 : (channel == "alpha" ? 4 : 0)));
	low_input[slot] = value_of(p_values, "low-input", 0.0);
	high_input[slot] = value_of(p_values, "high-input", 1.0);
	inv_gamma[slot] = 1.0 / CLAMP(value_of(p_values, "gamma", 1.0), 0.1, 10.0);
	low_output[slot] = value_of(p_values, "low-output", 0.0);
	high_output[slot] = value_of(p_values, "high-output", 1.0);
	const bool clamp_input = flag_of(p_values, "clamp-input", false);
	const bool clamp_output = flag_of(p_values, "clamp-output", false);
	const bool linear = choice_of(p_values, "trc", "non-linear") == "linear";
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		for (int c = 0; c < 4; c++) {
			double value = (c < 3 && !linear) ? linear_to_srgb(p_in[c]) : p_in[c];
			value = levels_map(value, low_input[c + 1], high_input[c + 1], clamp_input, inv_gamma[c + 1], low_output[c + 1], high_output[c + 1], clamp_output);
			if (c < 3) {
				value = levels_map(value, low_input[0], high_input[0], clamp_input, inv_gamma[0], low_output[0], high_output[0], clamp_output);
			}
			r_out[c] = (c < 3 && !linear) ? srgb_to_linear(float(value)) : float(value);
		}
	});
}

static void op_hue_saturation(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const float hue = float(CLAMP(value_of(p_values, "hue", 0.0), -1.0, 1.0));
	const float lightness = float(CLAMP(value_of(p_values, "lightness", 0.0), -1.0, 1.0));
	const float saturation = float(CLAMP(value_of(p_values, "saturation", 0.0), -1.0, 1.0));
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		const float encoded[3] = { linear_to_srgb(p_in[0]), linear_to_srgb(p_in[1]), linear_to_srgb(p_in[2]) };
		float hsl[3];
		rgb_to_hsl(encoded, hsl);
		if (hsl[1] > 0.0f) {
			hsl[0] += hue / 2.0f;
			if (hsl[0] < 0.0f) {
				hsl[0] += 1.0f;
			} else if (hsl[0] > 1.0f) {
				hsl[0] -= 1.0f;
			}
			hsl[1] = CLAMP(hsl[1] * (saturation + 1.0f), 0.0f, 1.0f);
		}
		hsl[2] = lightness < 0.0f ? hsl[2] * (lightness + 1.0f) : hsl[2] + lightness * (1.0f - hsl[2]);
		float rgb[3];
		hsl_to_rgb(hsl, rgb);
		for (int c = 0; c < 3; c++) {
			r_out[c] = srgb_to_linear(rgb[c]);
		}
		r_out[3] = p_in[3];
	});
}

static inline float color_balance_map(float p_value, float p_lightness, float p_shadows, float p_midtones, float p_highlights) {
	static const float a = 0.25f, b = 0.333f, scale = 0.7f;
	p_shadows *= CLAMP((p_lightness - b) / -a + 0.5f, 0.0f, 1.0f) * scale;
	p_midtones *= CLAMP((p_lightness - b) / a + 0.5f, 0.0f, 1.0f) * CLAMP((p_lightness + b - 1.0f) / -a + 0.5f, 0.0f, 1.0f) * scale;
	p_highlights *= CLAMP((p_lightness + b - 1.0f) / a + 0.5f, 0.0f, 1.0f) * scale;
	p_value += p_shadows;
	p_value += p_midtones;
	p_value += p_highlights;
	return CLAMP(p_value, 0.0f, 1.0f);
}

static void op_color_balance(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	static const char *names[3][3] = {
		{ "shadows-cyan-red", "midtones-cyan-red", "highlights-cyan-red" },
		{ "shadows-magenta-green", "midtones-magenta-green", "highlights-magenta-green" },
		{ "shadows-yellow-blue", "midtones-yellow-blue", "highlights-yellow-blue" },
	};
	float amounts[3][3];
	for (int c = 0; c < 3; c++) {
		for (int range = 0; range < 3; range++) {
			amounts[c][range] = float(CLAMP(value_of(p_values, names[c][range], 0.0), -1.0, 1.0));
		}
	}
	const bool preserve_luminosity = flag_of(p_values, "preserve-luminosity", true);
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		const float encoded[3] = { linear_to_srgb(p_in[0]), linear_to_srgb(p_in[1]), linear_to_srgb(p_in[2]) };
		float hsl[3];
		rgb_to_hsl(encoded, hsl);
		float rgb[3];
		for (int c = 0; c < 3; c++) {
			rgb[c] = color_balance_map(encoded[c], hsl[2], amounts[c][0], amounts[c][1], amounts[c][2]);
		}
		if (preserve_luminosity) {
			float balanced[3];
			rgb_to_hsl(rgb, balanced);
			balanced[2] = hsl[2];
			hsl_to_rgb(balanced, rgb);
		}
		for (int c = 0; c < 3; c++) {
			r_out[c] = srgb_to_linear(rgb[c]);
		}
		r_out[3] = p_in[3];
	});
}

static void op_colorize(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const float hue = float(CLAMP(value_of(p_values, "hue", 0.5), 0.0, 1.0));
	const float saturation = float(CLAMP(value_of(p_values, "saturation", 0.5), 0.0, 1.0));
	const float lightness = float(CLAMP(value_of(p_values, "lightness", 0.0), -1.0, 1.0));
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		// GIMP takes the luminance of the linear values and writes the HSL
		// result back without a transfer curve; this port keeps that.
		float lum = TetraLayerModes::luminance(p_in);
		if (lightness > 0.0f) {
			lum = lum * (1.0f - lightness);
			lum += 1.0f - (1.0f - lightness);
		} else if (lightness < 0.0f) {
			lum = lum * (lightness + 1.0f);
		}
		const float hsl[3] = { hue, saturation, lum };
		hsl_to_rgb(hsl, r_out);
		r_out[3] = p_in[3];
	});
}

static void op_threshold(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const String channel = choice_of(p_values, "channel", "value");
	const float low = float(value_of(p_values, "low", 0.5));
	const float high = float(value_of(p_values, "high", 1.0));
	const int kind = channel == "red" ? 1 : (channel == "green" ? 2 : (channel == "blue" ? 3 : (channel == "alpha" ? 4 : (channel == "rgb" ? 5 : (channel == "luminance" ? 6 : 0)))));
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		const float encoded[3] = { linear_to_srgb(p_in[0]), linear_to_srgb(p_in[1]), linear_to_srgb(p_in[2]) };
		float value;
		switch (kind) {
			case 1:
			case 2:
			case 3:
				value = encoded[kind - 1];
				break;
			case 4:
				value = p_in[3];
				break;
			case 5:
				value = MIN(encoded[0], MIN(encoded[1], encoded[2]));
				break;
			case 6:
				value = TetraLayerModes::luminance(encoded);
				break;
			default:
				value = MAX(encoded[0], MAX(encoded[1], encoded[2]));
				break;
		}
		value = (value >= low && value <= high) ? 1.0f : 0.0f;
		r_out[0] = r_out[1] = r_out[2] = value;
		r_out[3] = p_in[3];
	});
}

static void op_posterize(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const float levels = float(CLAMP(int(value_of(p_values, "levels", 3)), 2, 256)) - 1.0f;
	map_pixels(p_src, r_dst, p_width, p_height, [&](const float *p_in, float *r_out) {
		for (int c = 0; c < 3; c++) {
			r_out[c] = srgb_to_linear(Math::round(linear_to_srgb(p_in[c]) * levels) / levels);
		}
		r_out[3] = Math::round(p_in[3] * levels) / levels;
	});
}

// Blurs RGBA on premultiplied values so transparent pixels carry no colour.
static void blur_rgba(const float *p_src, float *r_dst, int p_width, int p_height, float p_sigma_x, float p_sigma_y) {
	const size_t count = size_t(p_width) * p_height;
	LocalVector<float> premultiplied;
	premultiplied.resize(count * 4);
	for (size_t i = 0; i < count; i++) {
		const float *in = p_src + i * 4;
		float *out = premultiplied.ptr() + i * 4;
		out[0] = in[0] * in[3];
		out[1] = in[1] * in[3];
		out[2] = in[2] * in[3];
		out[3] = in[3];
	}
	tetra_gaussian_blur(premultiplied.ptr(), r_dst, p_width, p_height, 4, p_sigma_x, p_sigma_y);
	for (size_t i = 0; i < count; i++) {
		float *px = r_dst + i * 4;
		if (px[3] > 1e-6f) {
			px[0] /= px[3];
			px[1] /= px[3];
			px[2] /= px[3];
		} else {
			px[0] = px[1] = px[2] = px[3] = 0.0f;
		}
	}
}

static void op_gaussian_blur(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const float sigma_x = float(CLAMP(value_of(p_values, "std-dev-x", 1.5), 0.0, 1500.0));
	const float sigma_y = float(CLAMP(value_of(p_values, "std-dev-y", 1.5), 0.0, 1500.0));
	blur_rgba(p_src, r_dst, p_width, p_height, sigma_x, sigma_y);
}

static void op_unsharp_mask(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const float sigma = float(CLAMP(value_of(p_values, "std-dev", 3.0), 0.0, 1500.0));
	const float scale = float(CLAMP(value_of(p_values, "scale", 0.5), 0.0, 300.0));
	const float threshold = float(CLAMP(value_of(p_values, "threshold", 0.0), 0.0, 1.0));
	blur_rgba(p_src, r_dst, p_width, p_height, sigma, sigma);
	const size_t count = size_t(p_width) * p_height;
	for (size_t i = 0; i < count; i++) {
		const float *in = p_src + i * 4;
		float *out = r_dst + i * 4;
		const float difference[3] = { in[0] - out[0], in[1] - out[1], in[2] - out[2] };
		const float largest = MAX(Math::abs(difference[0]), MAX(Math::abs(difference[1]), Math::abs(difference[2])));
		const float amount = (threshold > 0.0f && largest < threshold) ? 0.0f : scale;
		for (int c = 0; c < 3; c++) {
			out[c] = in[c] + difference[c] * amount;
		}
		out[3] = in[3];
	}
}

static void op_pixelize(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const int size_x = MAX(int(value_of(p_values, "size-x", 16)), 1);
	const int size_y = MAX(int(value_of(p_values, "size-y", 16)), 1);
	const int blocks_y = (p_height + size_y - 1) / size_y;
	tetra_parallel_for(blocks_y, 2, [&](int p_block) {
		const int y0 = p_block * size_y;
		const int y1 = MIN(y0 + size_y, p_height);
		for (int x0 = 0; x0 < p_width; x0 += size_x) {
			const int x1 = MIN(x0 + size_x, p_width);
			double sum[4] = { 0, 0, 0, 0 };
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					const float *in = p_src + (size_t(y) * p_width + x) * 4;
					sum[0] += in[0] * in[3];
					sum[1] += in[1] * in[3];
					sum[2] += in[2] * in[3];
					sum[3] += in[3];
				}
			}
			float color[4] = { 0, 0, 0, 0 };
			if (sum[3] > 0.0) {
				color[0] = float(sum[0] / sum[3]);
				color[1] = float(sum[1] / sum[3]);
				color[2] = float(sum[2] / sum[3]);
				color[3] = float(sum[3] / ((y1 - y0) * (x1 - x0)));
			}
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					memcpy(r_dst + (size_t(y) * p_width + x) * 4, color, sizeof(color));
				}
			}
		}
	});
}

static void op_edge_sobel(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const bool horizontal = flag_of(p_values, "horizontal", true);
	const bool vertical = flag_of(p_values, "vertical", true);
	const bool keep_sign = flag_of(p_values, "keep-sign", true);
	tetra_parallel_rows(p_height, p_width * 4, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			const int y_above = MAX(y - 1, 0);
			const int y_below = MIN(y + 1, p_height - 1);
			for (int x = 0; x < p_width; x++) {
				const int x_left = MAX(x - 1, 0);
				const int x_right = MIN(x + 1, p_width - 1);
				const float *tl = p_src + (size_t(y_above) * p_width + x_left) * 4;
				const float *t = p_src + (size_t(y_above) * p_width + x) * 4;
				const float *tr = p_src + (size_t(y_above) * p_width + x_right) * 4;
				const float *l = p_src + (size_t(y) * p_width + x_left) * 4;
				const float *center = p_src + (size_t(y) * p_width + x) * 4;
				const float *r = p_src + (size_t(y) * p_width + x_right) * 4;
				const float *bl = p_src + (size_t(y_below) * p_width + x_left) * 4;
				const float *b = p_src + (size_t(y_below) * p_width + x) * 4;
				const float *br = p_src + (size_t(y_below) * p_width + x_right) * 4;
				float *out = r_dst + (size_t(y) * p_width + x) * 4;
				for (int c = 0; c < 3; c++) {
					float horizontal_gradient = 0.0f;
					float vertical_gradient = 0.0f;
					if (horizontal) {
						horizontal_gradient = (tr[c] - tl[c]) + 2.0f * (r[c] - l[c]) + (br[c] - bl[c]);
					}
					if (vertical) {
						vertical_gradient = (tl[c] + 2.0f * t[c] + tr[c]) - (bl[c] + 2.0f * b[c] + br[c]);
					}
					if (horizontal && vertical) {
						out[c] = Math::sqrt(horizontal_gradient * horizontal_gradient + vertical_gradient * vertical_gradient) / 5.656854f;
					} else if (keep_sign) {
						out[c] = 0.5f + (horizontal_gradient + vertical_gradient) / 8.0f;
					} else {
						out[c] = Math::abs(horizontal_gradient + vertical_gradient) / 4.0f;
					}
				}
				out[3] = center[3];
			}
		}
	});
}

static void op_emboss(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const bool bumpmap = choice_of(p_values, "type", "emboss") == "bumpmap";
	const double azimuth = Math::deg_to_rad(value_of(p_values, "azimuth", 30.0));
	const double elevation = Math::deg_to_rad(value_of(p_values, "elevation", 45.0));
	const int depth = CLAMP(int(value_of(p_values, "depth", 20)), 1, 100);
	const double light_x = cos(azimuth) * cos(elevation);
	const double light_y = sin(azimuth) * cos(elevation);
	const double light_z = sin(elevation);
	const double normal_z = 1.0 / depth;
	const double normal_z_squared = normal_z * normal_z;
	const double normal_z_light = normal_z * light_z;
	tetra_parallel_rows(p_height, p_width * 8, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			for (int x = 0; x < p_width; x++) {
				// Alpha-weighted height of the 3x3 neighbourhood: luminance
				// for a grey emboss, the channel sum for a bump map.
				float m[3][3];
				for (int i = 0; i < 3; i++) {
					for (int j = 0; j < 3; j++) {
						const int sy = y + i - 1;
						const int sx = x + j - 1;
						if (sx < 0 || sx >= p_width || sy < 0 || sy >= p_height) {
							m[i][j] = 0.0f;
							continue;
						}
						const float *px = p_src + (size_t(sy) * p_width + sx) * 4;
						m[i][j] = px[3] * (bumpmap ? px[0] + px[1] + px[2] : TetraLayerModes::luminance(px));
					}
				}
				const float nx = m[0][0] + m[1][0] + m[2][0] - m[0][2] - m[1][2] - m[2][2];
				const float ny = m[2][0] + m[2][1] + m[2][2] - m[0][0] - m[0][1] - m[0][2];
				float shade;
				if (nx == 0.0f && ny == 0.0f) {
					shade = float(light_z);
				} else {
					const double n_dot_l = nx * light_x + ny * light_y + normal_z_light;
					shade = n_dot_l < 0.0 ? 0.0f : float(n_dot_l / sqrt(nx * nx + ny * ny + normal_z_squared));
				}
				const float *in = p_src + (size_t(y) * p_width + x) * 4;
				float *out = r_dst + (size_t(y) * p_width + x) * 4;
				for (int c = 0; c < 3; c++) {
					out[c] = bumpmap ? in[c] * shade : shade;
				}
				out[3] = in[3];
			}
		}
	});
}

static inline float noise_gauss(RandomPCG &p_random) {
	float u, v, x;
	do {
		v = p_random.randf();
		do {
			u = p_random.randf();
		} while (u == 0.0f);
		x = 1.71552776992141359295f * (v - 0.5f) / u;
	} while (x * x > -4.0f * logf(u));
	return x;
}

static void op_noise_rgb(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	const bool correlated = flag_of(p_values, "correlated", false);
	const bool independent = flag_of(p_values, "independent", true);
	const bool linear = flag_of(p_values, "linear", true);
	const bool gaussian = flag_of(p_values, "gaussian", true);
	const double amounts[4] = {
		value_of(p_values, "red", 0.2),
		value_of(p_values, "green", 0.2),
		value_of(p_values, "blue", 0.2),
		value_of(p_values, "alpha", 0.0),
	};
	const uint64_t seed = uint64_t(int64_t(value_of(p_values, "seed", 0)));
	// One generator per row keeps the result independent of how rows are
	// distributed over threads.
	tetra_parallel_rows(p_height, p_width * 8, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			RandomPCG random(seed * 0x9E3779B97F4A7C15ull + uint64_t(y) * 0xD1B54A32D192ED03ull + 1);
			for (int x = 0; x < p_width; x++) {
				const float *in = p_src + (size_t(y) * p_width + x) * 4;
				float *out = r_dst + (size_t(y) * p_width + x) * 4;
				double coefficient = 0.0;
				for (int c = 0; c < 4; c++) {
					if (c == 0 || independent || c == 3) {
						const float noise = gaussian ? noise_gauss(random) : random.randf() * 2.0f - 1.0f;
						coefficient = amounts[c] * noise * 0.5;
					}
					if (coefficient == 0.0) {
						out[c] = in[c];
						continue;
					}
					const float value = (c < 3 && !linear) ? linear_to_srgb(in[c]) : in[c];
					const float noisy = correlated ? float(value + value * (coefficient / 0.5)) : float(value + coefficient);
					const float clamped = CLAMP(noisy, 0.0f, 1.0f);
					out[c] = (c < 3 && !linear) ? srgb_to_linear(clamped) : clamped;
				}
			}
		}
	});
}

typedef void (*NativeOperation)(const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height);

static const struct {
	const char *name;
	NativeOperation function;
} native_operations[] = {
	{ "invert", op_invert },
	{ "desaturate", op_desaturate },
	{ "brightness-contrast", op_brightness_contrast },
	{ "levels", op_levels },
	{ "hue-saturation", op_hue_saturation },
	{ "color-balance", op_color_balance },
	{ "colorize", op_colorize },
	{ "threshold", op_threshold },
	{ "posterize", op_posterize },
	{ "gaussian-blur", op_gaussian_blur },
	{ "unsharp-mask", op_unsharp_mask },
	{ "pixelize", op_pixelize },
	{ "edge-sobel", op_edge_sobel },
	{ "emboss", op_emboss },
	{ "noise-rgb", op_noise_rgb },
};

Error TetraImageDocument::_run_operation(const String &p_id, const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height) {
	if (p_id.begins_with("gegl:")) {
		return TetraGegl::apply_pixels(p_id, p_values, p_src, r_dst, p_width, p_height);
	}
	ERR_FAIL_COND_V_MSG(!p_id.begins_with("native:"), ERR_DOES_NOT_EXIST, vformat("Unknown operation \"%s\".", p_id));
	const String name = p_id.substr(7);
	for (const auto &operation : native_operations) {
		if (name == operation.name) {
			operation.function(p_values, p_src, r_dst, p_width, p_height);
			return OK;
		}
	}
	ERR_FAIL_V_MSG(ERR_DOES_NOT_EXIST, vformat("Unknown operation \"%s\".", p_id));
}

Ref<Image> TetraImageDocument::filter_image(const Ref<Image> &p_image, const String &p_operation_id, const Dictionary &p_values) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Ref<Image>());
	const Vector<float> source = _image_to_linear(p_image);
	ERR_FAIL_COND_V(source.is_empty(), Ref<Image>());
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	Vector<uint8_t> data;
	data.resize(int64_t(w) * h * 16);
	if (_run_operation(p_operation_id, p_values, source.ptr(), reinterpret_cast<float *>(data.ptrw()), w, h) != OK) {
		return Ref<Image>();
	}
	return Image::create_from_data(w, h, false, Image::FORMAT_RGBAF, data);
}

Ref<Image> TetraImageDocument::compute_operation(int p_layer_id, const String &p_operation_id, const Dictionary &p_values) const {
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, Ref<Image>());
	const Vector<float> source = layers[index].pixels;
	LocalVector<float> filtered;
	filtered.resize(size_t(width) * height * 4);
	if (_run_operation(p_operation_id, p_values, source.ptr(), filtered.ptr(), width, height) != OK) {
		return Ref<Image>();
	}
	// The result is the layer as it would look after apply_operation(): the
	// filtered pixels inside the selection, the original ones outside.
	Vector<uint8_t> data;
	data.resize(int64_t(width) * height * 16);
	memcpy(data.ptrw(), source.ptr(), data.size());
	const Rect2i rect = _edit_rect();
	if (rect.has_area()) {
		_mix_into(reinterpret_cast<float *>(data.ptrw()), rect, filtered.ptr(), width, Point2i(), layers[index].lock_alpha, true);
	}
	return Image::create_from_data(width, height, false, Image::FORMAT_RGBAF, data);
}

Error TetraImageDocument::apply_operation(int p_layer_id, const String &p_operation_id, const Dictionary &p_values) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	LocalVector<float> filtered;
	filtered.resize(size_t(width) * height * 4);
	const Error err = _run_operation(p_operation_id, p_values, layers[index].pixels.ptr(), filtered.ptr(), width, height);
	if (err != OK) {
		return err;
	}
	_undo_open(p_operation_id.get_slicec(':', 1).capitalize());
	_commit_pixels(index, _edit_rect(), filtered.ptr(), width, Point2i());
	_undo_close();
	return OK;
}

Error TetraImageDocument::apply_filtered_image(int p_layer_id, const Ref<Image> &p_filtered, const String &p_name) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_filtered.is_null() || p_filtered->get_width() != width || p_filtered->get_height() != height, ERR_INVALID_PARAMETER);
	const Vector<float> filtered = _image_to_linear(p_filtered);
	ERR_FAIL_COND_V(filtered.is_empty(), ERR_INVALID_PARAMETER);
	_undo_open(p_name);
	_commit_pixels(index, _edit_rect(), filtered.ptr(), width, Point2i());
	_undo_close();
	return OK;
}

void TetraImageDocument::_bind_operation_methods() {
	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("get_native_operations"), &TetraImageDocument::get_native_operations);
	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("get_operations"), &TetraImageDocument::get_operations);
	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("filter_image", "image", "operation_id", "values"), &TetraImageDocument::filter_image);
	ClassDB::bind_method(D_METHOD("compute_operation", "layer_id", "operation_id", "values"), &TetraImageDocument::compute_operation);
	ClassDB::bind_method(D_METHOD("apply_operation", "layer_id", "operation_id", "values"), &TetraImageDocument::apply_operation);
	ClassDB::bind_method(D_METHOD("apply_filtered_image", "layer_id", "filtered", "name"), &TetraImageDocument::apply_filtered_image);
}
