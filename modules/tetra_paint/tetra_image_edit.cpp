// Fills and transforms. The gradient shape and repeat formulas are ported
// from GIMP (app/operations/gimpoperationgradient.c, Copyright (C) 1995
// Spencer Kimball and Peter Mattis), GPL-3.0-or-later.

#include "tetra_image_document.h"

#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

#include <cmath>
#include <cstring>

Error TetraImageDocument::fill(int p_layer_id, const Color &p_color) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const Rect2i rect = _edit_rect();
	if (!rect.has_area()) {
		return OK;
	}
	const float color[4] = { p_color.r, p_color.g, p_color.b, p_color.a };
	LocalVector<float> buffer;
	buffer.resize(size_t(rect.size.x) * rect.size.y * 4);
	for (size_t i = 0; i < buffer.size(); i += 4) {
		memcpy(buffer.ptr() + i, color, sizeof(color));
	}
	_undo_open(TTR("Fill"));
	_commit_pixels(index, rect, buffer.ptr(), rect.size.x, rect.position);
	_undo_close();
	return OK;
}

Error TetraImageDocument::clear(int p_layer_id) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const Rect2i rect = _edit_rect();
	if (!rect.has_area() || layers[index].lock_alpha) {
		return OK;
	}
	LocalVector<float> buffer;
	buffer.resize_initialized(size_t(rect.size.x) * rect.size.y * 4);
	_undo_open(TTR("Clear"));
	_commit_pixels(index, rect, buffer.ptr(), rect.size.x, rect.position);
	_undo_close();
	return OK;
}

Error TetraImageDocument::bucket_fill(int p_layer_id, const Vector2i &p_point, const Color &p_color, float p_threshold, bool p_contiguous, bool p_sample_merged) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(!_canvas_rect().has_point(p_point), ERR_INVALID_PARAMETER);

	const Vector<float> source = _sample_source(index, p_sample_merged);
	LocalVector<float> mask;
	_region_by_color(source.ptr(), p_point, p_threshold, p_contiguous, mask);

	Rect2i rect;
	for (int y = 0; y < height; y++) {
		const float *row = mask.ptr() + size_t(y) * width;
		int first = 0;
		while (first < width && row[first] == 0.0f) {
			first++;
		}
		if (first == width) {
			continue;
		}
		int last = width - 1;
		while (row[last] == 0.0f) {
			last--;
		}
		const Rect2i span(first, y, last - first + 1, 1);
		rect = rect.has_area() ? rect.merge(span) : span;
	}
	if (!rect.has_area()) {
		return OK;
	}

	// The colour is painted over the region in Normal mode; _commit_pixels
	// then limits the result to the selection.
	const float color[4] = { p_color.r, p_color.g, p_color.b, 1.0f };
	const int w = rect.size.x;
	LocalVector<float> buffer;
	buffer.resize(size_t(w) * rect.size.y * 4);
	const float *layer_px = layers[index].pixels.ptr();
	tetra_parallel_rows(rect.size.y, w, [&](int p_first, int p_end) {
		LocalVector<float> paint;
		paint.resize(w * 4);
		for (int x = 0; x < w; x++) {
			memcpy(paint.ptr() + x * 4, color, sizeof(color));
		}
		for (int row = p_first; row < p_end; row++) {
			const size_t offset = size_t(rect.position.y + row) * width + rect.position.x;
			TetraLayerModes::process(TetraLayerModes::MODE_NORMAL, TetraLayerModes::COMPOSITE_UNION, layer_px + offset * 4, paint.ptr(), mask.ptr() + offset, buffer.ptr() + size_t(row) * w * 4, p_color.a, w, rect.position.x, rect.position.y + row);
		}
	});
	_undo_open(TTR("Bucket Fill"));
	_commit_pixels(index, rect, buffer.ptr(), w, rect.position);
	_undo_close();
	return OK;
}

static inline double gradient_factor(TetraImageDocument::GradientType p_type, double p_x, double p_y, double p_dist, const Vector2 &p_axis) {
	if (p_dist == 0.0) {
		return 0.0;
	}
	switch (p_type) {
		case TetraImageDocument::GRADIENT_LINEAR:
			return (p_axis.x * p_x + p_axis.y * p_y) / p_dist;
		case TetraImageDocument::GRADIENT_BILINEAR:
			return Math::abs((p_axis.x * p_x + p_axis.y * p_y) / p_dist);
		case TetraImageDocument::GRADIENT_RADIAL:
			return sqrt(p_x * p_x + p_y * p_y) / p_dist;
		case TetraImageDocument::GRADIENT_SQUARE:
			return MAX(Math::abs(p_x), Math::abs(p_y)) / p_dist;
		case TetraImageDocument::GRADIENT_CONICAL: {
			if (p_x == 0.0 && p_y == 0.0) {
				return 0.5;
			}
			const double length = sqrt(p_x * p_x + p_y * p_y);
			const double cosine = CLAMP((p_axis.x * p_x + p_axis.y * p_y) / length, -1.0, 1.0);
			return CLAMP(acos(cosine) / Math::PI, 0.0, 1.0);
		}
		case TetraImageDocument::GRADIENT_CONICAL_ASYMMETRIC: {
			if (p_x == 0.0 && p_y == 0.0) {
				return 0.5;
			}
			double angle = atan2(p_x, p_y) - atan2(double(p_axis.x), double(p_axis.y));
			if (angle < 0.0) {
				angle += Math::TAU;
			}
			return CLAMP(angle / Math::TAU, 0.0, 1.0);
		}
		case TetraImageDocument::GRADIENT_SPIRAL_CLOCKWISE:
		case TetraImageDocument::GRADIENT_SPIRAL_ANTICLOCKWISE: {
			if (p_x == 0.0 && p_y == 0.0) {
				return 0.5;
			}
			const double axis_angle = atan2(double(p_axis.x), double(p_axis.y));
			const double point_angle = atan2(p_x, p_y);
			double angle = p_type == TetraImageDocument::GRADIENT_SPIRAL_CLOCKWISE ? point_angle - axis_angle : axis_angle - point_angle;
			if (angle < 0.0) {
				angle += Math::TAU;
			}
			return fmod(angle / Math::TAU + sqrt(p_x * p_x + p_y * p_y) / p_dist, 1.0);
		}
	}
	return 0.0;
}

Error TetraImageDocument::gradient_fill(int p_layer_id, const Vector2 &p_start, const Vector2 &p_end, const Color &p_color_a, const Color &p_color_b, GradientType p_type, GradientRepeat p_repeat) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const Rect2i rect = _edit_rect();
	if (!rect.has_area()) {
		return OK;
	}
	const double dist = p_start.distance_to(p_end);
	const Vector2 axis = dist > 0.0 ? (p_end - p_start) / dist : Vector2();

	// Like GIMP's default, the two colours are interpolated in perceptual
	// (sRGB-encoded) RGB and the alpha linearly.
	float encoded_a[3], encoded_b[3];
	for (int c = 0; c < 3; c++) {
		encoded_a[c] = TetraLayerModes::linear_to_srgb(p_color_a[c]);
		encoded_b[c] = TetraLayerModes::linear_to_srgb(p_color_b[c]);
	}

	const int w = rect.size.x;
	LocalVector<float> buffer;
	buffer.resize(size_t(w) * rect.size.y * 4);
	const float *layer_px = layers[index].pixels.ptr();
	tetra_parallel_rows(rect.size.y, w * 8, [&](int p_first_row, int p_end_row) {
		LocalVector<float> paint;
		paint.resize(w * 4);
		for (int row = p_first_row; row < p_end_row; row++) {
			const int y = rect.position.y + row;
			for (int col = 0; col < w; col++) {
				double factor = gradient_factor(p_type, rect.position.x + col + 0.5 - p_start.x, y + 0.5 - p_start.y, dist, axis);
				switch (p_repeat) {
					case REPEAT_NONE:
						factor = CLAMP(factor, 0.0, 1.0);
						break;
					case REPEAT_SAWTOOTH:
						factor = factor - floor(factor);
						break;
					case REPEAT_TRIANGULAR: {
						factor = Math::abs(factor);
						const unsigned int whole = (unsigned int)factor;
						factor -= floor(factor);
						if (whole & 1) {
							factor = 1.0 - factor;
						}
					} break;
				}
				const float f = float(factor);
				float *px = paint.ptr() + col * 4;
				for (int c = 0; c < 3; c++) {
					px[c] = TetraLayerModes::srgb_to_linear(encoded_a[c] + (encoded_b[c] - encoded_a[c]) * f);
				}
				px[3] = p_color_a.a + (p_color_b.a - p_color_a.a) * f;
			}
			const size_t offset = size_t(y) * width + rect.position.x;
			TetraLayerModes::process(TetraLayerModes::MODE_NORMAL, TetraLayerModes::COMPOSITE_UNION, layer_px + offset * 4, paint.ptr(), nullptr, buffer.ptr() + size_t(row) * w * 4, 1.0f, w, rect.position.x, y);
		}
	});
	_undo_open(TTR("Gradient"));
	_commit_pixels(index, rect, buffer.ptr(), w, rect.position);
	_undo_close();
	return OK;
}

Color TetraImageDocument::pick_color(const Vector2i &p_point, bool p_sample_merged, int p_radius) const {
	ERR_FAIL_COND_V(!_canvas_rect().has_point(p_point), Color(0, 0, 0, 0));
	const int index = _find_layer(active_layer);
	ERR_FAIL_COND_V_MSG(index < 0 && !p_sample_merged, Color(0, 0, 0, 0), "There is no active layer to sample.");
	const int radius = MAX(p_radius, 0);
	const Rect2i rect = _clip_rect(Rect2i(p_point.x - radius, p_point.y - radius, radius * 2 + 1, radius * 2 + 1));
	LocalVector<float> pixels;
	pixels.resize(size_t(rect.size.x) * rect.size.y * 4);
	if (p_sample_merged) {
		_composite_into(rect, pixels.ptr());
	} else {
		for (int y = 0; y < rect.size.y; y++) {
			memcpy(pixels.ptr() + size_t(y) * rect.size.x * 4, layers[index].pixels.ptr() + (size_t(rect.position.y + y) * width + rect.position.x) * 4, sizeof(float) * 4 * rect.size.x);
		}
	}
	const int count = rect.size.x * rect.size.y;
	if (count == 1) {
		return Color(pixels[0], pixels[1], pixels[2], pixels[3]);
	}
	// Average with alpha weighting so transparent pixels do not tint the result.
	double sum[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < count; i++) {
		const float *px = pixels.ptr() + i * 4;
		sum[0] += px[0] * px[3];
		sum[1] += px[1] * px[3];
		sum[2] += px[2] * px[3];
		sum[3] += px[3];
	}
	if (sum[3] <= 0.0) {
		return Color(0, 0, 0, 0);
	}
	return Color(sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3], sum[3] / count);
}

enum RemapKind {
	REMAP_FLIP_HORIZONTAL,
	REMAP_FLIP_VERTICAL,
	REMAP_ROTATE_90,
	REMAP_ROTATE_180,
	REMAP_ROTATE_270,
};

// Flips or rotates (clockwise) an interleaved buffer. Quarter turns swap the
// width and height of the result.
static void remap_buffer(const float *p_src, float *r_dst, int p_src_width, int p_src_height, int p_channels, RemapKind p_kind) {
	const bool swap = p_kind == REMAP_ROTATE_90 || p_kind == REMAP_ROTATE_270;
	const int dst_width = swap ? p_src_height : p_src_width;
	const int dst_height = swap ? p_src_width : p_src_height;
	tetra_parallel_rows(dst_height, dst_width, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			float *dst = r_dst + size_t(y) * dst_width * p_channels;
			for (int x = 0; x < dst_width; x++) {
				int sx = x, sy = y;
				switch (p_kind) {
					case REMAP_FLIP_HORIZONTAL:
						sx = p_src_width - 1 - x;
						break;
					case REMAP_FLIP_VERTICAL:
						sy = p_src_height - 1 - y;
						break;
					case REMAP_ROTATE_90:
						sx = y;
						sy = p_src_height - 1 - x;
						break;
					case REMAP_ROTATE_180:
						sx = p_src_width - 1 - x;
						sy = p_src_height - 1 - y;
						break;
					case REMAP_ROTATE_270:
						sx = p_src_width - 1 - y;
						sy = x;
						break;
				}
				memcpy(dst + x * p_channels, p_src + (size_t(sy) * p_src_width + sx) * p_channels, sizeof(float) * p_channels);
			}
		}
	});
}

// Copies a buffer into a new frame of another size; p_offset is where the
// old top-left corner lands in the new frame. Uncovered area is zero.
static Vector<float> reframe_buffer(const Vector<float> &p_src, int p_src_width, int p_src_height, int p_dst_width, int p_dst_height, const Point2i &p_offset, int p_channels) {
	Vector<float> result;
	result.resize_initialized(int64_t(p_dst_width) * p_dst_height * p_channels);
	if (p_src.is_empty()) {
		return result;
	}
	const Rect2i covered = Rect2i(p_offset, Size2i(p_src_width, p_src_height)).intersection(Rect2i(0, 0, p_dst_width, p_dst_height));
	float *dst = result.ptrw();
	for (int y = covered.position.y; y < covered.position.y + covered.size.y; y++) {
		memcpy(dst + (size_t(y) * p_dst_width + covered.position.x) * p_channels,
				p_src.ptr() + (size_t(y - p_offset.y) * p_src_width + (covered.position.x - p_offset.x)) * p_channels,
				sizeof(float) * p_channels * covered.size.x);
	}
	return result;
}

struct ResampleTap {
	int first = 0;
	LocalVector<float> weights;
};

static inline float resample_kernel(TetraImageDocument::Interpolation p_interpolation, float p_t) {
	const float t = Math::abs(p_t);
	if (p_interpolation == TetraImageDocument::INTERPOLATION_LINEAR) {
		return t < 1.0f ? 1.0f - t : 0.0f;
	}
	// Catmull-Rom cubic.
	if (t < 1.0f) {
		return (1.5f * t - 2.5f) * t * t + 1.0f;
	}
	if (t < 2.0f) {
		return ((-0.5f * t + 2.5f) * t - 4.0f) * t + 2.0f;
	}
	return 0.0f;
}

// Filter taps for resampling one axis. When shrinking, the kernel is widened
// by the scale factor so every source pixel contributes (no aliasing).
static void build_resample_taps(int p_src_length, int p_dst_length, TetraImageDocument::Interpolation p_interpolation, LocalVector<ResampleTap> &r_taps) {
	r_taps.resize(p_dst_length);
	const double scale = double(p_src_length) / p_dst_length;
	for (int i = 0; i < p_dst_length; i++) {
		ResampleTap &tap = r_taps[i];
		if (p_interpolation == TetraImageDocument::INTERPOLATION_NEAREST) {
			tap.first = MIN(int((i + 0.5) * scale), p_src_length - 1);
			tap.weights.push_back(1.0f);
			continue;
		}
		const double stretch = MAX(scale, 1.0);
		const double support = (p_interpolation == TetraImageDocument::INTERPOLATION_LINEAR ? 1.0 : 2.0) * stretch;
		const double center = (i + 0.5) * scale - 0.5;
		const int first = int(floor(center - support)) + 1;
		const int last = int(floor(center + support));
		tap.first = first;
		float sum = 0.0f;
		for (int j = first; j <= last; j++) {
			const float weight = resample_kernel(p_interpolation, float((j - center) / stretch));
			tap.weights.push_back(weight);
			sum += weight;
		}
		for (float &weight : tap.weights) {
			weight /= sum;
		}
	}
}

static Vector<float> resample_buffer(const Vector<float> &p_src, int p_src_width, int p_src_height, int p_dst_width, int p_dst_height, int p_channels, TetraImageDocument::Interpolation p_interpolation) {
	Vector<float> result;
	result.resize(int64_t(p_dst_width) * p_dst_height * p_channels);
	if (p_src.is_empty()) {
		result.fill(0.0f);
		return result;
	}
	LocalVector<ResampleTap> taps_x, taps_y;
	build_resample_taps(p_src_width, p_dst_width, p_interpolation, taps_x);
	build_resample_taps(p_src_height, p_dst_height, p_interpolation, taps_y);
	const bool has_alpha = p_channels == 4;
	const float *src = p_src.ptr();

	// Horizontal pass on premultiplied values, so transparent pixels do not
	// bleed their colour into their neighbours.
	LocalVector<float> horizontal;
	horizontal.resize(size_t(p_dst_width) * p_src_height * p_channels);
	tetra_parallel_rows(p_src_height, p_dst_width * 4, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			const float *row = src + size_t(y) * p_src_width * p_channels;
			float *out = horizontal.ptr() + size_t(y) * p_dst_width * p_channels;
			for (int x = 0; x < p_dst_width; x++) {
				const ResampleTap &tap = taps_x[x];
				float sum[4] = { 0, 0, 0, 0 };
				for (uint32_t k = 0; k < tap.weights.size(); k++) {
					const float *px = row + CLAMP(tap.first + int(k), 0, p_src_width - 1) * p_channels;
					const float weight = tap.weights[k];
					if (has_alpha) {
						const float weighted_alpha = px[3] * weight;
						sum[0] += px[0] * weighted_alpha;
						sum[1] += px[1] * weighted_alpha;
						sum[2] += px[2] * weighted_alpha;
						sum[3] += weighted_alpha;
					} else {
						sum[0] += px[0] * weight;
					}
				}
				memcpy(out + x * p_channels, sum, sizeof(float) * p_channels);
			}
		}
	});

	float *dst = result.ptrw();
	tetra_parallel_rows(p_dst_height, p_dst_width * 4, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			const ResampleTap &tap = taps_y[y];
			float *out = dst + size_t(y) * p_dst_width * p_channels;
			memset(out, 0, sizeof(float) * p_dst_width * p_channels);
			for (uint32_t k = 0; k < tap.weights.size(); k++) {
				const float *row = horizontal.ptr() + size_t(CLAMP(tap.first + int(k), 0, p_src_height - 1)) * p_dst_width * p_channels;
				const float weight = tap.weights[k];
				for (int i = 0; i < p_dst_width * p_channels; i++) {
					out[i] += row[i] * weight;
				}
			}
			for (int x = 0; x < p_dst_width; x++) {
				float *px = out + x * p_channels;
				if (!has_alpha) {
					px[0] = CLAMP(px[0], 0.0f, 1.0f);
				} else if (px[3] > 1e-6f) {
					const float alpha = MIN(px[3], 1.0f);
					px[0] /= px[3];
					px[1] /= px[3];
					px[2] /= px[3];
					px[3] = alpha;
				} else {
					px[0] = px[1] = px[2] = px[3] = 0.0f;
				}
			}
		}
	});
	return result;
}

void TetraImageDocument::_set_layer_buffer(int p_index, const Vector<float> &p_pixels, const String &p_name) {
	const Rect2i changed = _diff_bounds(layers[p_index].pixels.ptr(), p_pixels.ptr());
	if (!changed.has_area()) {
		return;
	}
	_undo_open(p_name);
	_undo_push_pixels(layers[p_index].id, changed, layers[p_index].pixels.ptr());
	layers[p_index].pixels = p_pixels;
	_undo_close();
	_mark_dirty(changed);
}

void TetraImageDocument::_replace_canvas(int p_width, int p_height, const LocalVector<Vector<float>> &p_layer_pixels, const Vector<float> &p_selection, const String &p_name) {
	_undo_open(p_name);
	DocState before = _capture_state();
	const bool resized = p_width != width || p_height != height;
	width = p_width;
	height = p_height;
	for (uint32_t i = 0; i < layers.size(); i++) {
		layers[i].pixels = p_layer_pixels[i];
	}
	bool any = false;
	for (int64_t i = 0; i < p_selection.size() && !any; i++) {
		any = p_selection[i] > 0.0f;
	}
	selection_empty = !any;
	selection = any ? p_selection : Vector<float>();
	selection_bounds_valid = false;
	dirty_rect = Rect2i();
	_undo_push_state(before);
	_undo_close();
	if (resized) {
		emit_signal(SNAME("size_changed"));
	}
	emit_signal(SNAME("layers_changed"));
	emit_signal(SNAME("selection_changed"));
	_mark_dirty(_canvas_rect());
}

Error TetraImageDocument::flip_layer(int p_layer_id, bool p_vertical) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	Vector<float> pixels;
	pixels.resize(int64_t(width) * height * 4);
	remap_buffer(layers[index].pixels.ptr(), pixels.ptrw(), width, height, 4, p_vertical ? REMAP_FLIP_VERTICAL : REMAP_FLIP_HORIZONTAL);
	_set_layer_buffer(index, pixels, TTR("Flip Layer"));
	return OK;
}

Error TetraImageDocument::rotate_layer(int p_layer_id, int p_degrees) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const int degrees = ((p_degrees % 360) + 360) % 360;
	ERR_FAIL_COND_V_MSG(degrees % 90 != 0, ERR_INVALID_PARAMETER, "Layers rotate in steps of 90 degrees.");
	if (degrees == 0) {
		return OK;
	}
	Vector<float> pixels;
	pixels.resize_initialized(int64_t(width) * height * 4);
	const float *src = layers[index].pixels.ptr();
	if (degrees == 180) {
		remap_buffer(src, pixels.ptrw(), width, height, 4, REMAP_ROTATE_180);
	} else {
		// A quarter turn about the canvas centre; what leaves the canvas is
		// cut off because layers always have the size of the canvas.
		float *dst = pixels.ptrw();
		const double cx = width * 0.5;
		const double cy = height * 0.5;
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				const double dx = x + 0.5 - cx;
				const double dy = y + 0.5 - cy;
				const double source_x = degrees == 90 ? cx + dy : cx - dy;
				const double source_y = degrees == 90 ? cy - dx : cy + dx;
				const int sx = int(floor(source_x - 0.25));
				const int sy = int(floor(source_y - 0.25));
				if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
					memcpy(dst + (size_t(y) * width + x) * 4, src + (size_t(sy) * width + sx) * 4, sizeof(float) * 4);
				}
			}
		}
	}
	_set_layer_buffer(index, pixels, TTR("Rotate Layer"));
	return OK;
}

Error TetraImageDocument::translate_layer(int p_layer_id, const Vector2i &p_offset) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const Vector<float> pixels = reframe_buffer(layers[index].pixels, width, height, width, height, p_offset, 4);
	_set_layer_buffer(index, pixels, TTR("Move Layer"));
	return OK;
}

Error TetraImageDocument::flip_image(bool p_vertical) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	const RemapKind kind = p_vertical ? REMAP_FLIP_VERTICAL : REMAP_FLIP_HORIZONTAL;
	LocalVector<Vector<float>> buffers;
	for (const Layer &layer : layers) {
		Vector<float> pixels;
		pixels.resize(layer.pixels.size());
		remap_buffer(layer.pixels.ptr(), pixels.ptrw(), width, height, 4, kind);
		buffers.push_back(pixels);
	}
	Vector<float> mask;
	if (!selection_empty) {
		mask.resize(selection.size());
		remap_buffer(selection.ptr(), mask.ptrw(), width, height, 1, kind);
	}
	_replace_canvas(width, height, buffers, mask, TTR("Flip Image"));
	return OK;
}

Error TetraImageDocument::rotate_image(int p_degrees) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	const int degrees = ((p_degrees % 360) + 360) % 360;
	ERR_FAIL_COND_V_MSG(degrees % 90 != 0, ERR_INVALID_PARAMETER, "Images rotate in steps of 90 degrees.");
	if (degrees == 0) {
		return OK;
	}
	const RemapKind kind = degrees == 90 ? REMAP_ROTATE_90 : (degrees == 180 ? REMAP_ROTATE_180 : REMAP_ROTATE_270);
	LocalVector<Vector<float>> buffers;
	for (const Layer &layer : layers) {
		Vector<float> pixels;
		pixels.resize(layer.pixels.size());
		remap_buffer(layer.pixels.ptr(), pixels.ptrw(), width, height, 4, kind);
		buffers.push_back(pixels);
	}
	Vector<float> mask;
	if (!selection_empty) {
		mask.resize(selection.size());
		remap_buffer(selection.ptr(), mask.ptrw(), width, height, 1, kind);
	}
	const bool swap = degrees != 180;
	_replace_canvas(swap ? height : width, swap ? width : height, buffers, mask, TTR("Rotate Image"));
	return OK;
}

Error TetraImageDocument::scale_image(const Vector2i &p_size, Interpolation p_interpolation) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(p_size.x < 1 || p_size.y < 1 || p_size.x > 16384 || p_size.y > 16384, ERR_INVALID_PARAMETER);
	if (p_size == get_size()) {
		return OK;
	}
	LocalVector<Vector<float>> buffers;
	for (const Layer &layer : layers) {
		buffers.push_back(resample_buffer(layer.pixels, width, height, p_size.x, p_size.y, 4, p_interpolation));
	}
	Vector<float> mask;
	if (!selection_empty) {
		mask = resample_buffer(selection, width, height, p_size.x, p_size.y, 1, p_interpolation == INTERPOLATION_NEAREST ? INTERPOLATION_NEAREST : INTERPOLATION_LINEAR);
	}
	_replace_canvas(p_size.x, p_size.y, buffers, mask, TTR("Scale Image"));
	return OK;
}

Error TetraImageDocument::resize_canvas(const Vector2i &p_size, const Vector2i &p_offset) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(p_size.x < 1 || p_size.y < 1 || p_size.x > 16384 || p_size.y > 16384, ERR_INVALID_PARAMETER);
	if (p_size == get_size() && p_offset == Vector2i()) {
		return OK;
	}
	LocalVector<Vector<float>> buffers;
	for (const Layer &layer : layers) {
		buffers.push_back(reframe_buffer(layer.pixels, width, height, p_size.x, p_size.y, p_offset, 4));
	}
	Vector<float> mask;
	if (!selection_empty) {
		mask = reframe_buffer(selection, width, height, p_size.x, p_size.y, p_offset, 1);
	}
	_replace_canvas(p_size.x, p_size.y, buffers, mask, TTR("Canvas Size"));
	return OK;
}

Error TetraImageDocument::crop(const Rect2i &p_rect) {
	const Rect2i rect = _clip_rect(p_rect);
	ERR_FAIL_COND_V_MSG(!rect.has_area(), ERR_INVALID_PARAMETER, "The crop rectangle does not intersect the canvas.");
	return resize_canvas(rect.size, -rect.position);
}

Error TetraImageDocument::crop_to_selection() {
	ERR_FAIL_COND_V_MSG(selection_empty, ERR_UNCONFIGURED, "There is no selection to crop to.");
	return crop(selection_bounds());
}

void TetraImageDocument::_bind_edit_methods() {
	ClassDB::bind_method(D_METHOD("fill", "layer_id", "color"), &TetraImageDocument::fill);
	ClassDB::bind_method(D_METHOD("clear", "layer_id"), &TetraImageDocument::clear);
	ClassDB::bind_method(D_METHOD("bucket_fill", "layer_id", "point", "color", "threshold", "contiguous", "sample_merged"), &TetraImageDocument::bucket_fill, DEFVAL(0.0588f), DEFVAL(true), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("gradient_fill", "layer_id", "start", "end", "color_a", "color_b", "type", "repeat"), &TetraImageDocument::gradient_fill, DEFVAL(GRADIENT_LINEAR), DEFVAL(REPEAT_NONE));
	ClassDB::bind_method(D_METHOD("pick_color", "point", "sample_merged", "radius"), &TetraImageDocument::pick_color, DEFVAL(false), DEFVAL(0));

	ClassDB::bind_method(D_METHOD("flip_layer", "layer_id", "vertical"), &TetraImageDocument::flip_layer, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("rotate_layer", "layer_id", "degrees"), &TetraImageDocument::rotate_layer);
	ClassDB::bind_method(D_METHOD("translate_layer", "layer_id", "offset"), &TetraImageDocument::translate_layer);
	ClassDB::bind_method(D_METHOD("flip_image", "vertical"), &TetraImageDocument::flip_image, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("rotate_image", "degrees"), &TetraImageDocument::rotate_image);
	ClassDB::bind_method(D_METHOD("scale_image", "size", "interpolation"), &TetraImageDocument::scale_image, DEFVAL(INTERPOLATION_CUBIC));
	ClassDB::bind_method(D_METHOD("resize_canvas", "size", "offset"), &TetraImageDocument::resize_canvas, DEFVAL(Vector2i()));
	ClassDB::bind_method(D_METHOD("crop", "rect"), &TetraImageDocument::crop);
	ClassDB::bind_method(D_METHOD("crop_to_selection"), &TetraImageDocument::crop_to_selection);

	BIND_ENUM_CONSTANT(GRADIENT_LINEAR);
	BIND_ENUM_CONSTANT(GRADIENT_BILINEAR);
	BIND_ENUM_CONSTANT(GRADIENT_RADIAL);
	BIND_ENUM_CONSTANT(GRADIENT_SQUARE);
	BIND_ENUM_CONSTANT(GRADIENT_CONICAL);
	BIND_ENUM_CONSTANT(GRADIENT_CONICAL_ASYMMETRIC);
	BIND_ENUM_CONSTANT(GRADIENT_SPIRAL_CLOCKWISE);
	BIND_ENUM_CONSTANT(GRADIENT_SPIRAL_ANTICLOCKWISE);

	BIND_ENUM_CONSTANT(REPEAT_NONE);
	BIND_ENUM_CONSTANT(REPEAT_SAWTOOTH);
	BIND_ENUM_CONSTANT(REPEAT_TRIANGULAR);

	BIND_ENUM_CONSTANT(INTERPOLATION_NEAREST);
	BIND_ENUM_CONSTANT(INTERPOLATION_LINEAR);
	BIND_ENUM_CONSTANT(INTERPOLATION_CUBIC);
}
