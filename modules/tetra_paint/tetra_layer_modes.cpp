// The blend, composite and special-mode functions below are ported from GIMP
// (app/operations/layer-modes/gimpoperationlayermode-blend.c,
// gimpoperationlayermode-composite.c, gimpoperationnormal.c,
// gimpoperationdissolve.c, gimpoperationbehind.c, gimpoperationerase.c,
// gimpoperationmerge.c, gimpoperationsplit.c, gimpoperationreplace.c and the
// mode table of gimp-layer-modes.c).
// Copyright (C) 1995 Spencer Kimball and Peter Mattis; 2008 Michael Natterer;
// 2012 Daniel Sabo; 2017 Øyvind Kolås, Ell and the GIMP team.
// GPL-3.0-or-later.

#include "tetra_layer_modes.h"

#include <cmath>
#include <cstring>

namespace TetraLayerModes {

static const float EPSILON = 1e-6f;
static const float SAFE_DIV_MAX = 1.0f / EPSILON;

#define U COMPOSITE_UNION
#define CB COMPOSITE_CLIP_TO_BACKDROP

static const Info mode_infos[MODE_MAX] = {
	{ "Normal", "svg:src-over", SPACE_AUTO, U, U },
	{ "Dissolve", "tetra:dissolve", SPACE_AUTO, U, U },
	{ "Behind", "svg:dst-over", SPACE_AUTO, U, U },
	{ "Color erase", "tetra:color-erase", SPACE_LINEAR, CB, CB },
	{ "Erase", "svg:dst-out", SPACE_AUTO, CB, CB },
	{ "Merge", "tetra:merge", SPACE_AUTO, U, U },
	{ "Split", "tetra:split", SPACE_AUTO, CB, CB },
	{ "Lighten only", "svg:lighten", SPACE_AUTO, CB, U },
	{ "Luma/Luminance lighten only", "tetra:luma-lighten-only", SPACE_LINEAR, CB, U },
	{ "Screen", "svg:screen", SPACE_PERCEPTUAL, CB, U },
	{ "Dodge", "svg:color-dodge", SPACE_PERCEPTUAL, CB, U },
	{ "Addition", "svg:plus", SPACE_LINEAR, CB, U },
	{ "Darken only", "svg:darken", SPACE_AUTO, CB, U },
	{ "Luma/Luminance darken only", "tetra:luma-darken-only", SPACE_LINEAR, CB, U },
	{ "Multiply", "svg:multiply", SPACE_LINEAR, CB, U },
	{ "Burn", "svg:color-burn", SPACE_PERCEPTUAL, CB, U },
	{ "Linear burn", "tetra:linear-burn", SPACE_PERCEPTUAL, CB, U },
	{ "Overlay", "svg:overlay", SPACE_PERCEPTUAL, CB, U },
	{ "Soft light", "svg:soft-light", SPACE_PERCEPTUAL, CB, U },
	{ "Hard light", "svg:hard-light", SPACE_PERCEPTUAL, CB, U },
	{ "Vivid light", "tetra:vivid-light", SPACE_PERCEPTUAL, CB, U },
	{ "Pin light", "tetra:pin-light", SPACE_PERCEPTUAL, CB, U },
	{ "Linear light", "tetra:linear-light", SPACE_PERCEPTUAL, CB, U },
	{ "Hard mix", "tetra:hard-mix", SPACE_PERCEPTUAL, CB, U },
	{ "Difference", "svg:difference", SPACE_PERCEPTUAL, CB, U },
	{ "Exclusion", "svg:exclusion", SPACE_PERCEPTUAL, CB, U },
	{ "Subtract", "tetra:subtract", SPACE_LINEAR, CB, U },
	{ "Grain extract", "tetra:grain-extract", SPACE_PERCEPTUAL, CB, U },
	{ "Grain merge", "tetra:grain-merge", SPACE_PERCEPTUAL, CB, U },
	{ "Divide", "tetra:divide", SPACE_LINEAR, CB, U },
	{ "HSV Hue", "svg:hue", SPACE_PERCEPTUAL, CB, U },
	{ "HSV Saturation", "svg:saturation", SPACE_PERCEPTUAL, CB, U },
	{ "HSL Color", "svg:color", SPACE_PERCEPTUAL, CB, U },
	{ "HSV Value", "svg:luminosity", SPACE_PERCEPTUAL, CB, U },
	{ "LCh Hue", "tetra:lch-hue", SPACE_LAB, CB, U },
	{ "LCh Chroma", "tetra:lch-chroma", SPACE_LAB, CB, U },
	{ "LCh Color", "tetra:lch-color", SPACE_LAB, CB, U },
	{ "LCh Lightness", "tetra:lch-lightness", SPACE_LAB, CB, U },
	{ "Luminance", "tetra:luminance", SPACE_LINEAR, CB, U },
};

#undef U
#undef CB

const Info &get_info(int p_mode) {
	if (p_mode < 0 || p_mode >= MODE_MAX) {
		return mode_infos[MODE_NORMAL];
	}
	return mode_infos[p_mode];
}

int mode_from_ora_op(const char *p_op) {
	for (int i = 0; i < MODE_MAX; i++) {
		if (strcmp(mode_infos[i].ora_op, p_op) == 0) {
			return i;
		}
	}
	return MODE_NORMAL;
}

float linear_to_srgb(float p_value) {
	return p_value > 0.0031308f ? 1.055f * powf(p_value, 1.0f / 2.4f) - 0.055f : 12.92f * p_value;
}

float srgb_to_linear(float p_value) {
	return p_value > 0.04045f ? powf((p_value + 0.055f) / 1.055f, 2.4f) : p_value / 12.92f;
}

// Linear sRGB to CIE XYZ adapted to D50, as measured on babl.
static const float RGB_TO_XYZ[3][3] = {
	{ 0.4360348f, 0.3851167f, 0.1430514f },
	{ 0.2224879f, 0.7169038f, 0.0606083f },
	{ 0.0139159f, 0.0970605f, 0.7139290f },
};
static float xyz_to_rgb_matrix[3][3];
static const float D50_WHITE[3] = { 0.96420f, 1.0f, 0.82491f };

float luminance(const float *p_rgb) {
	return p_rgb[0] * RGB_TO_XYZ[1][0] + p_rgb[1] * RGB_TO_XYZ[1][1] + p_rgb[2] * RGB_TO_XYZ[1][2];
}

static inline float lab_f(float p_t) {
	return p_t > 216.0f / 24389.0f ? cbrtf(p_t) : (24389.0f / 27.0f * p_t + 16.0f) / 116.0f;
}

static inline float lab_f_inv(float p_t) {
	const float cube = p_t * p_t * p_t;
	return cube > 216.0f / 24389.0f ? cube : (116.0f * p_t - 16.0f) * 27.0f / 24389.0f;
}

void rgb_to_lab(const float *p_rgb, float *r_lab) {
	float f[3];
	for (int i = 0; i < 3; i++) {
		const float v = RGB_TO_XYZ[i][0] * p_rgb[0] + RGB_TO_XYZ[i][1] * p_rgb[1] + RGB_TO_XYZ[i][2] * p_rgb[2];
		f[i] = lab_f(v / D50_WHITE[i]);
	}
	r_lab[0] = 116.0f * f[1] - 16.0f;
	r_lab[1] = 500.0f * (f[0] - f[1]);
	r_lab[2] = 200.0f * (f[1] - f[2]);
}

void lab_to_rgb(const float *p_lab, float *r_rgb) {
	const float fy = (p_lab[0] + 16.0f) / 116.0f;
	const float xyz[3] = {
		lab_f_inv(fy + p_lab[1] / 500.0f) * D50_WHITE[0],
		lab_f_inv(fy) * D50_WHITE[1],
		lab_f_inv(fy - p_lab[2] / 200.0f) * D50_WHITE[2],
	};
	for (int i = 0; i < 3; i++) {
		r_rgb[i] = xyz_to_rgb_matrix[i][0] * xyz[0] + xyz_to_rgb_matrix[i][1] * xyz[1] + xyz_to_rgb_matrix[i][2] * xyz[2];
	}
}

// GLib's GRand (MT19937 with g_rand_int_range's rejection sampling), which
// GIMP's Dissolve mode uses, so the dither pattern matches GIMP's.
struct MersenneTwister {
	static const int N = 624;
	uint32_t mt[N];
	int index = N;

	explicit MersenneTwister(uint32_t p_seed) {
		mt[0] = p_seed;
		for (int i = 1; i < N; i++) {
			mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + uint32_t(i);
		}
	}

	uint32_t next() {
		if (index >= N) {
			for (int k = 0; k < N; k++) {
				const uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % N] & 0x7fffffffu);
				mt[k] = mt[(k + 397) % N] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
			}
			index = 0;
		}
		uint32_t y = mt[index++];
		y ^= y >> 11;
		y ^= (y << 7) & 0x9d2c5680u;
		y ^= (y << 15) & 0xefc60000u;
		y ^= y >> 18;
		return y;
	}

	// g_rand_int_range(rand, 0, p_range) for 0 < p_range <= 2^31.
	uint32_t next_range(uint32_t p_range) {
		uint32_t leftover = (0x80000000u % p_range) * 2;
		if (leftover >= p_range) {
			leftover -= p_range;
		}
		const uint32_t max_value = 0xffffffffu - leftover;
		uint32_t value;
		do {
			value = next();
		} while (value > max_value);
		return value % p_range;
	}
};

static const int DISSOLVE_TABLE_SIZE = 4096;
static uint32_t dissolve_table[DISSOLVE_TABLE_SIZE];

void initialize() {
	MersenneTwister generator(314159265u);
	for (int i = 0; i < DISSOLVE_TABLE_SIZE; i++) {
		dissolve_table[i] = generator.next();
	}

	const float(*m)[3] = RGB_TO_XYZ;
	const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			const int r0 = (j + 1) % 3, r1 = (j + 2) % 3;
			const int c0 = (i + 1) % 3, c1 = (i + 2) % 3;
			xyz_to_rgb_matrix[i][j] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / det;
		}
	}
}

static inline float safe_div(float p_a, float p_b) {
	float result = 0.0f;
	if (fabsf(p_a) > EPSILON) {
		result = p_a / p_b;
		result = result > SAFE_DIV_MAX ? SAFE_DIV_MAX : (result < -SAFE_DIV_MAX ? -SAFE_DIV_MAX : result);
	}
	return result;
}

static inline float min3(const float *p_v) {
	const float m = p_v[0] < p_v[1] ? p_v[0] : p_v[1];
	return m < p_v[2] ? m : p_v[2];
}

static inline float max3(const float *p_v) {
	const float m = p_v[0] > p_v[1] ? p_v[0] : p_v[1];
	return m > p_v[2] ? m : p_v[2];
}

// Blends one pixel in the mode's blend space: i is the backdrop, l the layer.
// Returns the fraction of the backdrop that is kept (Color erase) or 1.
static inline float blend_pixel(int p_mode, const float *i, const float *l, float *c) {
	switch (p_mode) {
		case MODE_LIGHTEN_ONLY:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] > l[k] ? i[k] : l[k];
			}
			break;
		case MODE_DARKEN_ONLY:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] < l[k] ? i[k] : l[k];
			}
			break;
		case MODE_LUMA_LIGHTEN_ONLY:
		case MODE_LUMA_DARKEN_ONLY: {
			const float dest_luminance = luminance(i);
			const float src_luminance = luminance(l);
			const bool keep = p_mode == MODE_LUMA_LIGHTEN_ONLY ? dest_luminance >= src_luminance : dest_luminance <= src_luminance;
			memcpy(c, keep ? i : l, sizeof(float) * 3);
		} break;
		case MODE_SCREEN:
			for (int k = 0; k < 3; k++) {
				c[k] = 1.0f - (1.0f - i[k]) * (1.0f - l[k]);
			}
			break;
		case MODE_DODGE:
			for (int k = 0; k < 3; k++) {
				c[k] = safe_div(i[k], 1.0f - l[k]);
			}
			break;
		case MODE_ADDITION:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] + l[k];
			}
			break;
		case MODE_MULTIPLY:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] * l[k];
			}
			break;
		case MODE_BURN:
			for (int k = 0; k < 3; k++) {
				c[k] = 1.0f - safe_div(1.0f - i[k], l[k]);
			}
			break;
		case MODE_LINEAR_BURN:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] + l[k] - 1.0f;
			}
			break;
		case MODE_OVERLAY:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] < 0.5f ? 2.0f * i[k] * l[k] : 1.0f - 2.0f * (1.0f - l[k]) * (1.0f - i[k]);
			}
			break;
		case MODE_SOFT_LIGHT:
			for (int k = 0; k < 3; k++) {
				const float multiply = i[k] * l[k];
				const float screen = 1.0f - (1.0f - i[k]) * (1.0f - l[k]);
				c[k] = (1.0f - i[k]) * multiply + i[k] * screen;
			}
			break;
		case MODE_HARD_LIGHT:
			for (int k = 0; k < 3; k++) {
				float v;
				if (l[k] > 0.5f) {
					v = (1.0f - i[k]) * (1.0f - (l[k] - 0.5f) * 2.0f);
					v = 1.0f - v < 1.0f ? 1.0f - v : 1.0f;
				} else {
					v = i[k] * (l[k] * 2.0f);
					v = v < 1.0f ? v : 1.0f;
				}
				c[k] = v;
			}
			break;
		case MODE_VIVID_LIGHT:
			for (int k = 0; k < 3; k++) {
				float v;
				if (l[k] <= 0.5f) {
					v = 1.0f - safe_div(1.0f - i[k], 2.0f * l[k]);
					v = v > 0.0f ? v : 0.0f;
				} else {
					v = safe_div(i[k], 2.0f * (1.0f - l[k]));
					v = v < 1.0f ? v : 1.0f;
				}
				c[k] = v;
			}
			break;
		case MODE_PIN_LIGHT:
			for (int k = 0; k < 3; k++) {
				if (l[k] > 0.5f) {
					const float v = 2.0f * (l[k] - 0.5f);
					c[k] = i[k] > v ? i[k] : v;
				} else {
					const float v = 2.0f * l[k];
					c[k] = i[k] < v ? i[k] : v;
				}
			}
			break;
		case MODE_LINEAR_LIGHT:
			for (int k = 0; k < 3; k++) {
				c[k] = l[k] <= 0.5f ? i[k] + 2.0f * l[k] - 1.0f : i[k] + 2.0f * (l[k] - 0.5f);
			}
			break;
		case MODE_HARD_MIX:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] + l[k] < 1.0f ? 0.0f : 1.0f;
			}
			break;
		case MODE_DIFFERENCE:
			for (int k = 0; k < 3; k++) {
				c[k] = fabsf(i[k] - l[k]);
			}
			break;
		case MODE_EXCLUSION:
			for (int k = 0; k < 3; k++) {
				c[k] = 0.5f - 2.0f * (i[k] - 0.5f) * (l[k] - 0.5f);
			}
			break;
		case MODE_SUBTRACT:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] - l[k];
			}
			break;
		case MODE_GRAIN_EXTRACT:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] - l[k] + 0.5f;
			}
			break;
		case MODE_GRAIN_MERGE:
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] + l[k] - 0.5f;
			}
			break;
		case MODE_DIVIDE:
			for (int k = 0; k < 3; k++) {
				c[k] = safe_div(i[k], l[k]);
			}
			break;
		case MODE_HSV_HUE: {
			const float src_max = max3(l);
			const float src_delta = src_max - min3(l);
			if (src_delta > EPSILON) {
				const float dest_max = max3(i);
				const float dest_delta = dest_max - min3(i);
				const float dest_s = dest_max ? dest_delta / dest_max : 0.0f;
				const float ratio = dest_s * dest_max / src_delta;
				const float offset = dest_max - src_max * ratio;
				for (int k = 0; k < 3; k++) {
					c[k] = l[k] * ratio + offset;
				}
			} else {
				memcpy(c, i, sizeof(float) * 3);
			}
		} break;
		case MODE_HSV_SATURATION: {
			const float dest_max = max3(i);
			const float dest_delta = dest_max - min3(i);
			if (dest_delta > EPSILON) {
				const float src_max = max3(l);
				const float src_s = src_max ? (src_max - min3(l)) / src_max : 0.0f;
				const float ratio = src_s * dest_max / dest_delta;
				const float offset = (1.0f - ratio) * dest_max;
				for (int k = 0; k < 3; k++) {
					c[k] = i[k] * ratio + offset;
				}
			} else {
				c[0] = c[1] = c[2] = dest_max;
			}
		} break;
		case MODE_HSL_COLOR: {
			float dest_l = (min3(i) + max3(i)) / 2.0f;
			float src_l = (min3(l) + max3(l)) / 2.0f;
			if (fabsf(src_l) > EPSILON && fabsf(1.0f - src_l) > EPSILON) {
				const bool dest_high = dest_l > 0.5f;
				const bool src_high = src_l > 0.5f;
				dest_l = dest_l < 1.0f - dest_l ? dest_l : 1.0f - dest_l;
				src_l = src_l < 1.0f - src_l ? src_l : 1.0f - src_l;
				const float ratio = dest_l / src_l;
				float offset = 0.0f;
				if (dest_high) {
					offset += 1.0f - 2.0f * dest_l;
				}
				if (src_high) {
					offset += 2.0f * dest_l - ratio;
				}
				for (int k = 0; k < 3; k++) {
					c[k] = l[k] * ratio + offset;
				}
			} else {
				c[0] = c[1] = c[2] = dest_l;
			}
		} break;
		case MODE_HSV_VALUE: {
			const float dest_v = max3(i);
			const float src_v = max3(l);
			if (fabsf(dest_v) > EPSILON) {
				const float ratio = src_v / dest_v;
				for (int k = 0; k < 3; k++) {
					c[k] = i[k] * ratio;
				}
			} else {
				c[0] = c[1] = c[2] = src_v;
			}
		} break;
		case MODE_LCH_HUE: {
			const float src_chroma = hypotf(l[1], l[2]);
			if (src_chroma > EPSILON) {
				const float dest_chroma = hypotf(i[1], i[2]);
				c[0] = i[0];
				c[1] = dest_chroma * l[1] / src_chroma;
				c[2] = dest_chroma * l[2] / src_chroma;
			} else {
				memcpy(c, i, sizeof(float) * 3);
			}
		} break;
		case MODE_LCH_CHROMA: {
			const float dest_chroma = hypotf(i[1], i[2]);
			if (dest_chroma > EPSILON) {
				const float src_chroma = hypotf(l[1], l[2]);
				c[0] = i[0];
				c[1] = src_chroma * i[1] / dest_chroma;
				c[2] = src_chroma * i[2] / dest_chroma;
			} else {
				memcpy(c, i, sizeof(float) * 3);
			}
		} break;
		case MODE_LCH_COLOR:
			c[0] = i[0];
			c[1] = l[1];
			c[2] = l[2];
			break;
		case MODE_LCH_LIGHTNESS:
			c[0] = l[0];
			c[1] = i[1];
			c[2] = i[2];
			break;
		case MODE_LUMINANCE: {
			const float ratio = safe_div(luminance(l), luminance(i));
			for (int k = 0; k < 3; k++) {
				c[k] = i[k] * ratio;
			}
		} break;
		case MODE_COLOR_ERASE: {
			float alpha = 0.0f;
			for (int k = 0; k < 3; k++) {
				const float col = i[k] > 1.0f ? 1.0f : (i[k] < 0.0f ? 0.0f : i[k]);
				const float bgcol = l[k] > 1.0f ? 1.0f : (l[k] < 0.0f ? 0.0f : l[k]);
				if (fabsf(col - bgcol) > EPSILON) {
					const float a = col > bgcol ? (col - bgcol) / (1.0f - bgcol) : (bgcol - col) / bgcol;
					alpha = alpha > a ? alpha : a;
				}
			}
			if (alpha > EPSILON) {
				const float alpha_inv = 1.0f / alpha;
				for (int k = 0; k < 3; k++) {
					c[k] = (i[k] - l[k]) * alpha_inv + l[k];
				}
			} else {
				c[0] = c[1] = c[2] = 0.0f;
			}
			return alpha;
		}
		default:
			memcpy(c, l, sizeof(float) * 3);
			break;
	}
	return 1.0f;
}

// Runs the blend function of p_mode on one pixel, converting to the blend
// space and back. r_comp receives linear RGB plus the alpha the composite
// step expects: the layer alpha, or for Color erase the kept fraction.
static inline void blend_in_space(int p_mode, int p_space, const float *p_in, const float *p_layer, float *r_comp) {
	if (p_in[3] == 0.0f || p_layer[3] == 0.0f) {
		r_comp[0] = r_comp[1] = r_comp[2] = 0.0f;
		r_comp[3] = p_mode == MODE_COLOR_ERASE ? 0.0f : p_layer[3];
		return;
	}
	float kept;
	if (p_space == SPACE_PERCEPTUAL) {
		float i[3], l[3], c[3];
		for (int k = 0; k < 3; k++) {
			i[k] = linear_to_srgb(p_in[k]);
			l[k] = linear_to_srgb(p_layer[k]);
		}
		kept = blend_pixel(p_mode, i, l, c);
		for (int k = 0; k < 3; k++) {
			r_comp[k] = srgb_to_linear(c[k]);
		}
	} else if (p_space == SPACE_LAB) {
		float i[3], l[3], c[3];
		rgb_to_lab(p_in, i);
		rgb_to_lab(p_layer, l);
		kept = blend_pixel(p_mode, i, l, c);
		lab_to_rgb(c, r_comp);
	} else {
		kept = blend_pixel(p_mode, p_in, p_layer, r_comp);
	}
	r_comp[3] = p_mode == MODE_COLOR_ERASE ? kept : p_layer[3];
}

static void process_normal(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float layer_alpha = layer[3] * opacity;
		if (mask) {
			layer_alpha *= mask[n];
		}
		if (p_composite == COMPOSITE_UNION) {
			const float out_alpha = layer_alpha + in[3] - layer_alpha * in[3];
			if (out_alpha) {
				const float layer_weight = layer_alpha / out_alpha;
				const float in_weight = 1.0f - layer_weight;
				for (int k = 0; k < 3; k++) {
					out[k] = layer[k] * layer_weight + in[k] * in_weight;
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			}
			out[3] = out_alpha;
		} else {
			const float out_alpha = in[3];
			if (out_alpha) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k] + (layer[k] - in[k]) * layer_alpha;
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			}
			out[3] = out_alpha;
		}
	}
}

static void process_dissolve(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count, int p_x, int p_y) {
	MersenneTwister generator(dissolve_table[((p_y % DISSOLVE_TABLE_SIZE) + DISSOLVE_TABLE_SIZE) % DISSOLVE_TABLE_SIZE]);
	for (int x = 0; x < p_x; x++) {
		generator.next();
	}
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float value = layer[3] * opacity * 255.0f;
		if (mask) {
			value *= mask[n];
		}
		if (float(generator.next_range(255)) >= value) {
			for (int k = 0; k < 4; k++) {
				out[k] = in[k];
			}
		} else {
			const float alpha = p_composite == COMPOSITE_UNION ? 1.0f : in[3];
			for (int k = 0; k < 3; k++) {
				out[k] = layer[k];
			}
			out[3] = alpha;
		}
	}
}

static void process_behind(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		const float src1_alpha = in[3];
		float src2_alpha = layer[3] * opacity;
		if (mask) {
			src2_alpha *= mask[n];
		}
		if (p_composite == COMPOSITE_UNION) {
			const float new_alpha = src2_alpha + (1.0f - src2_alpha) * src1_alpha;
			if (new_alpha) {
				const float ratio = in[3] / new_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = in[k] * ratio + layer[k] * (1.0f - ratio);
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = layer[k];
				}
			}
			out[3] = new_alpha;
		} else {
			for (int k = 0; k < 3; k++) {
				out[k] = src1_alpha ? in[k] : layer[k];
			}
			out[3] = src1_alpha;
		}
	}
}

static void process_erase(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float layer_alpha = layer[3] * opacity;
		if (mask) {
			layer_alpha *= mask[n];
		}
		if (p_composite == COMPOSITE_UNION) {
			const float new_alpha = in[3] + layer_alpha - 2.0f * in[3] * layer_alpha;
			if (new_alpha != 0.0f) {
				const float ratio = (1.0f - in[3]) * layer_alpha / new_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = ratio * layer[k] + (1.0f - ratio) * in[k];
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			}
			out[3] = new_alpha;
		} else {
			const float new_alpha = (1.0f - layer_alpha) * in[3];
			for (int k = 0; k < 3; k++) {
				out[k] = in[k];
			}
			out[3] = new_alpha;
		}
	}
}

static void process_merge(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float layer_alpha = layer[3] * opacity;
		if (mask) {
			layer_alpha *= mask[n];
		}
		if (p_composite == COMPOSITE_UNION) {
			const float in_alpha = in[3] < 1.0f - layer_alpha ? in[3] : 1.0f - layer_alpha;
			const float new_alpha = in_alpha + layer_alpha;
			if (new_alpha) {
				const float ratio = layer_alpha / new_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = in[k] + (layer[k] - in[k]) * ratio;
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			}
			out[3] = new_alpha;
		} else {
			const float in_alpha = in[3];
			layer_alpha -= 1.0f - in_alpha;
			if (layer_alpha > 0.0f) {
				const float ratio = layer_alpha / in_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = in[k] + (layer[k] - in[k]) * ratio;
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			}
			out[3] = in_alpha;
		}
	}
}

static void process_split(int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		const float in_alpha = in[3];
		float layer_alpha = layer[3] * opacity;
		if (mask) {
			layer_alpha *= mask[n];
		}
		if (p_composite == COMPOSITE_UNION) {
			if (layer_alpha <= in_alpha) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
				out[3] = in_alpha - layer_alpha;
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = layer[k];
				}
				out[3] = layer_alpha - in_alpha;
			}
		} else {
			const float new_alpha = in_alpha - layer_alpha;
			for (int k = 0; k < 3; k++) {
				out[k] = in[k];
			}
			out[3] = new_alpha > 0.0f ? new_alpha : 0.0f;
		}
	}
}

static void process_blend(int p_mode, int p_composite, const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	const int space = mode_infos[p_mode].blend_space;
	const bool subtractive = p_mode == MODE_COLOR_ERASE;
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float comp[4];
		blend_in_space(p_mode, space, in, layer, comp);
		const float m = mask ? mask[n] : 1.0f;
		const float in_alpha = in[3];

		if (!subtractive && p_composite == COMPOSITE_UNION) {
			const float layer_alpha = layer[3] * opacity * m;
			const float new_alpha = layer_alpha + (1.0f - layer_alpha) * in_alpha;
			if (layer_alpha == 0.0f || new_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			} else if (in_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = layer[k];
				}
			} else {
				const float ratio = layer_alpha / new_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = ratio * (in_alpha * (comp[k] - layer[k]) + layer[k] - in[k]) + in[k];
				}
			}
			out[3] = new_alpha;
		} else if (!subtractive) {
			const float layer_alpha = comp[3] * opacity * m;
			if (in_alpha == 0.0f || layer_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			} else {
				for (int k = 0; k < 3; k++) {
					out[k] = comp[k] * layer_alpha + in[k] * (1.0f - layer_alpha);
				}
			}
			out[3] = in_alpha;
		} else if (p_composite == COMPOSITE_UNION) {
			const float layer_alpha = layer[3] * opacity * m;
			const float comp_alpha = comp[3];
			const float new_alpha = in_alpha + layer_alpha - (2.0f - comp_alpha) * in_alpha * layer_alpha;
			if (layer_alpha == 0.0f || new_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			} else if (in_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = layer[k];
				}
			} else {
				const float ratio = in_alpha / new_alpha;
				const float layer_coeff = 1.0f / in_alpha - 1.0f;
				for (int k = 0; k < 3; k++) {
					out[k] = ratio * (layer_alpha * (comp_alpha * comp[k] + layer_coeff * layer[k] - in[k]) + in[k]);
				}
			}
			out[3] = new_alpha;
		} else {
			const float layer_alpha = layer[3] * opacity * m;
			const float comp_alpha = comp[3] * layer_alpha;
			const float new_alpha = 1.0f - layer_alpha + comp_alpha;
			if (in_alpha == 0.0f || comp_alpha == 0.0f) {
				for (int k = 0; k < 3; k++) {
					out[k] = in[k];
				}
			} else {
				const float ratio = comp_alpha / new_alpha;
				for (int k = 0; k < 3; k++) {
					out[k] = comp[k] * ratio + in[k] * (1.0f - ratio);
				}
			}
			out[3] = new_alpha * in_alpha;
		}
	}
}

void process(int p_mode, int p_composite, const float *p_in, const float *p_layer, const float *p_mask, float *p_out, float p_opacity, int p_count, int p_x, int p_y) {
	switch (p_mode) {
		case MODE_NORMAL:
			process_normal(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			break;
		case MODE_DISSOLVE:
			process_dissolve(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count, p_x, p_y);
			break;
		case MODE_BEHIND:
			process_behind(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			break;
		case MODE_ERASE:
			process_erase(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			break;
		case MODE_MERGE:
			process_merge(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			break;
		case MODE_SPLIT:
			process_split(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			break;
		default:
			if (p_mode < 0 || p_mode >= MODE_MAX) {
				process_normal(p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			} else {
				process_blend(p_mode, p_composite, p_in, p_layer, p_mask, p_out, p_opacity, p_count);
			}
			break;
	}
}

void process_replace(const float *in, const float *layer, const float *mask, float *out, float opacity, int count) {
	for (int n = 0; n < count; n++, in += 4, layer += 4, out += 4) {
		float opacity_value = opacity;
		if (mask) {
			opacity_value *= mask[n];
		}
		const float new_alpha = (layer[3] - in[3]) * opacity_value + in[3];
		float ratio = opacity_value;
		if (new_alpha) {
			ratio *= layer[3] / new_alpha;
		}
		for (int k = 0; k < 3; k++) {
			out[k] = (layer[k] - in[k]) * ratio + in[k];
		}
		out[3] = new_alpha;
	}
}

} // namespace TetraLayerModes
