#pragma once

#include <cstdint>

// GIMP's non-legacy layer modes on 32-bit float RGBA rows (linear light,
// straight alpha). The numbering follows GIMP's layer-mode menu, top to
// bottom, so a mode value is also the index of its name in the menu.
namespace TetraLayerModes {

enum Mode {
	MODE_NORMAL,
	MODE_DISSOLVE,
	MODE_BEHIND,
	MODE_COLOR_ERASE,
	MODE_ERASE,
	MODE_MERGE,
	MODE_SPLIT,
	MODE_LIGHTEN_ONLY,
	MODE_LUMA_LIGHTEN_ONLY,
	MODE_SCREEN,
	MODE_DODGE,
	MODE_ADDITION,
	MODE_DARKEN_ONLY,
	MODE_LUMA_DARKEN_ONLY,
	MODE_MULTIPLY,
	MODE_BURN,
	MODE_LINEAR_BURN,
	MODE_OVERLAY,
	MODE_SOFT_LIGHT,
	MODE_HARD_LIGHT,
	MODE_VIVID_LIGHT,
	MODE_PIN_LIGHT,
	MODE_LINEAR_LIGHT,
	MODE_HARD_MIX,
	MODE_DIFFERENCE,
	MODE_EXCLUSION,
	MODE_SUBTRACT,
	MODE_GRAIN_EXTRACT,
	MODE_GRAIN_MERGE,
	MODE_DIVIDE,
	MODE_HSV_HUE,
	MODE_HSV_SATURATION,
	MODE_HSL_COLOR,
	MODE_HSV_VALUE,
	MODE_LCH_HUE,
	MODE_LCH_CHROMA,
	MODE_LCH_COLOR,
	MODE_LCH_LIGHTNESS,
	MODE_LUMINANCE,
	MODE_MAX
};

// The colour space the blend function runs in. Compositing is always linear.
enum BlendSpace {
	SPACE_AUTO, // Blend directly on the linear values.
	SPACE_LINEAR,
	SPACE_PERCEPTUAL, // sRGB transfer curve.
	SPACE_LAB, // CIE L*a*b* (D50).
};

enum CompositeMode {
	COMPOSITE_UNION,
	COMPOSITE_CLIP_TO_BACKDROP,
};

struct Info {
	const char *name; // GIMP's menu label.
	const char *ora_op; // OpenRaster composite-op written for the mode.
	uint8_t blend_space;
	uint8_t layer_composite; // Default composite mode of a layer in this mode.
	uint8_t paint_composite; // Default composite mode of a paint tool in this mode.
};

// Builds the Dissolve random table. Must run once before process().
void initialize();

const Info &get_info(int p_mode);

// Mode for an OpenRaster composite-op string; MODE_NORMAL for unknown values.
int mode_from_ora_op(const char *p_op);

// Composites p_count pixels of p_layer over p_in into p_out (p_out may alias
// p_in). p_mask is one float per pixel or null. p_x/p_y are the canvas
// coordinates of the first pixel; Dissolve anchors its noise to them.
void process(int p_mode, int p_composite, const float *p_in, const float *p_layer, const float *p_mask, float *p_out, float p_opacity, int p_count, int p_x, int p_y);

// GIMP's "replace" compositing: every component, alpha included, moves
// towards the layer by opacity * mask. Used to commit filter results through
// a selection and by the smudge tool.
void process_replace(const float *p_in, const float *p_layer, const float *p_mask, float *p_out, float p_opacity, int p_count);

float linear_to_srgb(float p_value);
float srgb_to_linear(float p_value);

// Relative luminance (CIE Y) of a linear sRGB colour, with the D50-adapted
// weights babl uses.
float luminance(const float *p_rgb);

void rgb_to_lab(const float *p_rgb, float *r_lab);
void lab_to_rgb(const float *p_lab, float *r_rgb);

} // namespace TetraLayerModes
