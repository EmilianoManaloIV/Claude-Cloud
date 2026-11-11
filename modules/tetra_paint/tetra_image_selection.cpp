// The selection model follows GIMP: one canvas-sized float mask, combined
// with GIMP's channel-op rules (app/gegl/gimp-gegl-mask-combine.cc), and
// "select by colour" uses the seed-fill and pixel-difference rules of
// app/core/gimppickable-contiguous-region.cc.
// Copyright (C) 1995 Spencer Kimball and Peter Mattis. GPL-3.0-or-later.

#include "tetra_image_document.h"

#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

#include <cmath>
#include <cstring>

Rect2i TetraImageDocument::selection_bounds() const {
	if (selection_empty) {
		return Rect2i();
	}
	if (!selection_bounds_valid) {
		const float *mask = selection.ptr();
		int left = width, right = -1, top = height, bottom = -1;
		for (int y = 0; y < height; y++) {
			const float *row = mask + size_t(y) * width;
			int first = 0;
			while (first < width && row[first] <= 0.0f) {
				first++;
			}
			if (first == width) {
				continue;
			}
			int last = width - 1;
			while (row[last] <= 0.0f) {
				last--;
			}
			left = MIN(left, first);
			right = MAX(right, last);
			top = MIN(top, y);
			bottom = y;
		}
		selection_bounds_cache = right < 0 ? Rect2i() : Rect2i(left, top, right - left + 1, bottom - top + 1);
		selection_bounds_valid = true;
	}
	return selection_bounds_cache;
}

void TetraImageDocument::_selection_changed() {
	selection_bounds_valid = false;
	emit_signal(SNAME("selection_changed"));
}

void TetraImageDocument::_set_selection(const Vector<float> &p_mask, const String &p_name) {
	_undo_open(p_name);
	const Vector<float> before = selection;
	const bool before_empty = selection_empty;
	const Rect2i before_bounds = selection_bounds();

	bool any = false;
	const float *mask = p_mask.ptr();
	for (int64_t i = 0; i < p_mask.size() && !any; i++) {
		any = mask[i] > 0.0f;
	}
	selection_empty = !any;
	selection = any ? p_mask : Vector<float>();
	selection_bounds_valid = false;

	_undo_push_selection(before, before_empty, before_bounds);
	_undo_close();
	_selection_changed();
}

void TetraImageDocument::_combine_selection(const float *p_shape, SelectOp p_op, const String &p_name) {
	Vector<float> result;
	result.resize(int64_t(width) * height);
	float *out = result.ptrw();
	const float *old = _selection_ptr();
	tetra_parallel_rows(height, width, [&](int p_first, int p_end) {
		for (int64_t i = int64_t(p_first) * width; i < int64_t(p_end) * width; i++) {
			const float p = old ? old[i] : 0.0f;
			const float v = p_shape[i];
			switch (p_op) {
				case SELECT_REPLACE:
					out[i] = v;
					break;
				case SELECT_ADD:
					out[i] = MIN(p + v, 1.0f);
					break;
				case SELECT_SUBTRACT:
					out[i] = MAX(p - v, 0.0f);
					break;
				case SELECT_INTERSECT:
					out[i] = MIN(p, v);
					break;
			}
		}
	});
	_set_selection(result, p_name);
}

void TetraImageDocument::select_all() {
	TETRA_REQUIRE_IDLE();
	Vector<float> mask;
	mask.resize(int64_t(width) * height);
	mask.fill(1.0f);
	_set_selection(mask, TTR("Select All"));
}

void TetraImageDocument::select_none() {
	TETRA_REQUIRE_IDLE();
	_set_selection(Vector<float>(), TTR("Select None"));
}

void TetraImageDocument::select_rect(const Rect2i &p_rect, SelectOp p_op) {
	TETRA_REQUIRE_IDLE();
	LocalVector<float> shape;
	shape.resize_initialized(size_t(width) * height);
	const Rect2i rect = _clip_rect(p_rect);
	for (int y = rect.position.y; y < rect.position.y + rect.size.y; y++) {
		float *row = shape.ptr() + size_t(y) * width + rect.position.x;
		for (int x = 0; x < rect.size.x; x++) {
			row[x] = 1.0f;
		}
	}
	_combine_selection(shape.ptr(), p_op, TTR("Rectangle Select"));
}

// Adds p_weight times the coverage of the horizontal span [p_x0, p_x1) to a
// mask row. Antialiased spans count the covered fraction of each pixel; hard
// spans take the pixels whose centre lies inside.
static void add_span(float *p_row, int p_width, double p_x0, double p_x1, float p_weight, bool p_antialias) {
	if (!p_antialias) {
		const int first = MAX(int(ceil(p_x0 - 0.5)), 0);
		const int end = MIN(int(ceil(p_x1 - 0.5)), p_width);
		for (int x = first; x < end; x++) {
			p_row[x] += p_weight;
		}
		return;
	}
	const double x0 = MAX(p_x0, 0.0);
	const double x1 = MIN(p_x1, double(p_width));
	if (x1 <= x0) {
		return;
	}
	const int first = int(floor(x0));
	const int last = int(floor(x1));
	if (first == last) {
		p_row[first] += p_weight * float(x1 - x0);
		return;
	}
	p_row[first] += p_weight * float(first + 1 - x0);
	for (int x = first + 1; x < last; x++) {
		p_row[x] += p_weight;
	}
	if (last < p_width) {
		p_row[last] += p_weight * float(x1 - last);
	}
}

// Sub-scanlines per pixel row for antialiased shapes.
static const int SHAPE_SUBSAMPLES = 8;

void TetraImageDocument::select_ellipse(const Rect2i &p_rect, SelectOp p_op, bool p_antialias) {
	TETRA_REQUIRE_IDLE();
	LocalVector<float> shape;
	shape.resize_initialized(size_t(width) * height);
	if (p_rect.has_area()) {
		const double center_x = p_rect.position.x + p_rect.size.x * 0.5;
		const double center_y = p_rect.position.y + p_rect.size.y * 0.5;
		const double radius_x = p_rect.size.x * 0.5;
		const double radius_y = p_rect.size.y * 0.5;
		const int samples = p_antialias ? SHAPE_SUBSAMPLES : 1;
		const Rect2i rows = _clip_rect(p_rect);
		for (int y = rows.position.y; y < rows.position.y + rows.size.y; y++) {
			float *row = shape.ptr() + size_t(y) * width;
			for (int s = 0; s < samples; s++) {
				const double t = (y + (s + 0.5) / samples - center_y) / radius_y;
				if (t <= -1.0 || t >= 1.0) {
					continue;
				}
				const double half = radius_x * sqrt(1.0 - t * t);
				add_span(row, width, center_x - half, center_x + half, 1.0f / samples, p_antialias);
			}
			for (int x = rows.position.x; x < rows.position.x + rows.size.x; x++) {
				row[x] = MIN(row[x], 1.0f);
			}
		}
	}
	_combine_selection(shape.ptr(), p_op, TTR("Ellipse Select"));
}

void TetraImageDocument::select_polygon(const PackedVector2Array &p_points, SelectOp p_op, bool p_antialias) {
	TETRA_REQUIRE_IDLE();
	LocalVector<float> shape;
	shape.resize_initialized(size_t(width) * height);
	const int count = p_points.size();
	if (count >= 3) {
		real_t min_y = p_points[0].y, max_y = p_points[0].y;
		for (int i = 1; i < count; i++) {
			min_y = MIN(min_y, p_points[i].y);
			max_y = MAX(max_y, p_points[i].y);
		}
		const int first_row = MAX(int(floor(min_y)), 0);
		const int end_row = MIN(int(ceil(max_y)), height);
		const int samples = p_antialias ? SHAPE_SUBSAMPLES : 1;
		LocalVector<double> crossings;
		for (int y = first_row; y < end_row; y++) {
			float *row = shape.ptr() + size_t(y) * width;
			for (int s = 0; s < samples; s++) {
				const double scan_y = y + (s + 0.5) / samples;
				crossings.clear();
				for (int i = 0; i < count; i++) {
					const Vector2 &a = p_points[i];
					const Vector2 &b = p_points[(i + 1) % count];
					if ((a.y <= scan_y && b.y > scan_y) || (b.y <= scan_y && a.y > scan_y)) {
						crossings.push_back(a.x + (scan_y - a.y) * (b.x - a.x) / (b.y - a.y));
					}
				}
				crossings.sort();
				for (uint32_t i = 0; i + 1 < crossings.size(); i += 2) {
					add_span(row, width, crossings[i], crossings[i + 1], 1.0f / samples, p_antialias);
				}
			}
			for (int x = 0; x < width; x++) {
				row[x] = MIN(row[x], 1.0f);
			}
		}
	}
	_combine_selection(shape.ptr(), p_op, TTR("Free Select"));
}

Vector<float> TetraImageDocument::_sample_source(int p_layer_index, bool p_sample_merged) const {
	if (!p_sample_merged) {
		return layers[p_layer_index].pixels;
	}
	Vector<float> merged;
	merged.resize(int64_t(width) * height * 4);
	_composite_into(_canvas_rect(), merged.ptrw());
	return merged;
}

// Builds the mask of pixels whose colour is within p_threshold of the seed
// pixel: the largest difference of the sRGB-encoded R, G and B components
// decides, fully transparent pixels never match an opaque seed, and a fully
// transparent seed matches by alpha only.
void TetraImageDocument::_region_by_color(const float *p_pixels, const Point2i &p_seed, float p_threshold, bool p_contiguous, LocalVector<float> &r_mask) const {
	r_mask.resize_initialized(size_t(width) * height);
	const float *reference = p_pixels + (size_t(p_seed.y) * width + p_seed.x) * 4;
	const bool by_alpha = reference[3] == 0.0f;
	const float threshold = p_threshold + 1e-6f;

	// The encoded interval is mapped back to linear values once, so the scan
	// compares raw components instead of encoding every pixel.
	float low[3], high[3];
	for (int c = 0; c < 3; c++) {
		const float encoded = TetraLayerModes::linear_to_srgb(reference[c]);
		low[c] = MIN(TetraLayerModes::srgb_to_linear(encoded - threshold), reference[c]);
		high[c] = MAX(TetraLayerModes::srgb_to_linear(encoded + threshold), reference[c]);
	}
	auto matches = [&](const float *p_px) -> bool {
		if (by_alpha) {
			return Math::abs(p_px[3] - reference[3]) <= threshold;
		}
		if (p_px[3] == 0.0f) {
			return false;
		}
		return p_px[0] >= low[0] && p_px[0] <= high[0] && p_px[1] >= low[1] && p_px[1] <= high[1] && p_px[2] >= low[2] && p_px[2] <= high[2];
	};

	float *mask = r_mask.ptr();
	if (!p_contiguous) {
		tetra_parallel_rows(height, width, [&](int p_first, int p_end) {
			for (int64_t i = int64_t(p_first) * width; i < int64_t(p_end) * width; i++) {
				mask[i] = matches(p_pixels + i * 4) ? 1.0f : 0.0f;
			}
		});
		return;
	}

	LocalVector<Point2i> stack;
	stack.push_back(p_seed);
	while (!stack.is_empty()) {
		const Point2i point = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		const size_t row = size_t(point.y) * width;
		if (mask[row + point.x] != 0.0f || !matches(p_pixels + (row + point.x) * 4)) {
			continue;
		}
		int left = point.x;
		while (left > 0 && mask[row + left - 1] == 0.0f && matches(p_pixels + (row + left - 1) * 4)) {
			left--;
		}
		int right = point.x;
		while (right < width - 1 && mask[row + right + 1] == 0.0f && matches(p_pixels + (row + right + 1) * 4)) {
			right++;
		}
		for (int x = left; x <= right; x++) {
			mask[row + x] = 1.0f;
		}
		for (int dy = -1; dy <= 1; dy += 2) {
			const int ny = point.y + dy;
			if (ny < 0 || ny >= height) {
				continue;
			}
			const size_t next_row = size_t(ny) * width;
			bool in_run = false;
			for (int x = left; x <= right; x++) {
				const bool open = mask[next_row + x] == 0.0f && matches(p_pixels + (next_row + x) * 4);
				if (open && !in_run) {
					stack.push_back(Point2i(x, ny));
				}
				in_run = open;
			}
		}
	}
}

void TetraImageDocument::select_by_color(const Vector2i &p_point, float p_threshold, bool p_contiguous, bool p_sample_merged, SelectOp p_op) {
	TETRA_REQUIRE_IDLE();
	ERR_FAIL_COND(!_canvas_rect().has_point(p_point));
	const int index = _find_layer(active_layer);
	ERR_FAIL_COND_MSG(index < 0 && !p_sample_merged, "There is no active layer to sample.");
	const Vector<float> source = _sample_source(index, p_sample_merged);
	LocalVector<float> mask;
	_region_by_color(source.ptr(), p_point, p_threshold, p_contiguous, mask);
	_combine_selection(mask.ptr(), p_op, p_contiguous ? TTR("Fuzzy Select") : TTR("Select by Color"));
}

void TetraImageDocument::select_fuzzy(const Vector2i &p_point, float p_threshold, bool p_sample_merged, SelectOp p_op) {
	select_by_color(p_point, p_threshold, true, p_sample_merged, p_op);
}

void TetraImageDocument::select_mask(const Ref<Image> &p_mask, SelectOp p_op) {
	TETRA_REQUIRE_IDLE();
	ERR_FAIL_COND(p_mask.is_null());
	ERR_FAIL_COND_MSG(p_mask->get_width() != width || p_mask->get_height() != height, "The mask must have the size of the canvas.");
	Ref<Image> image = p_mask->duplicate();
	if (image->is_compressed()) {
		ERR_FAIL_COND(image->decompress() != OK);
	}
	if (image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	image->convert(Image::FORMAT_RF);
	LocalVector<float> shape;
	shape.resize(size_t(width) * height);
	const float *src = reinterpret_cast<const float *>(image->ptr());
	for (size_t i = 0; i < shape.size(); i++) {
		shape[i] = CLAMP(src[i], 0.0f, 1.0f);
	}
	_combine_selection(shape.ptr(), p_op, TTR("Selection from Mask"));
}

void TetraImageDocument::invert_selection() {
	TETRA_REQUIRE_IDLE();
	Vector<float> mask;
	mask.resize(int64_t(width) * height);
	float *out = mask.ptrw();
	const float *old = _selection_ptr();
	for (int64_t i = 0; i < mask.size(); i++) {
		out[i] = 1.0f - (old ? old[i] : 0.0f);
	}
	_set_selection(mask, TTR("Invert Selection"));
}

void TetraImageDocument::feather_selection(float p_radius) {
	TETRA_REQUIRE_IDLE();
	if (selection_empty || p_radius <= 0.0f) {
		return;
	}
	// GIMP feathers with a Gaussian of standard deviation radius / 3.5.
	const float sigma = p_radius / 3.5f;
	Vector<float> mask;
	mask.resize(int64_t(width) * height);
	tetra_gaussian_blur(selection.ptr(), mask.ptrw(), width, height, 1, sigma, sigma);
	_set_selection(mask, TTR("Feather Selection"));
}

// Circular maximum filter of radius p_radius. Samples outside the canvas
// count as p_outside. The horizontal maxima for growing half-widths are built
// incrementally and merged into every row offset that uses that half-width.
static void dilate_mask(const float *p_src, float *r_dst, int p_width, int p_height, int p_radius, float p_outside) {
	const size_t count = size_t(p_width) * p_height;
	LocalVector<float> horizontal;
	horizontal.resize(count);
	memcpy(horizontal.ptr(), p_src, sizeof(float) * count);
	memcpy(r_dst, p_src, sizeof(float) * count);
	for (int half = 0; half <= p_radius; half++) {
		if (half > 0) {
			tetra_parallel_rows(p_height, p_width, [&](int p_first, int p_end) {
				for (int y = p_first; y < p_end; y++) {
					const float *src = p_src + size_t(y) * p_width;
					float *row = horizontal.ptr() + size_t(y) * p_width;
					for (int x = 0; x < p_width; x++) {
						const float left = x - half >= 0 ? src[x - half] : p_outside;
						const float right = x + half < p_width ? src[x + half] : p_outside;
						row[x] = MAX(row[x], MAX(left, right));
					}
				}
			});
		}
		for (int dy = -p_radius; dy <= p_radius; dy++) {
			if (int(sqrt(double(p_radius * p_radius - dy * dy))) != half) {
				continue;
			}
			tetra_parallel_rows(p_height, p_width, [&](int p_first, int p_end) {
				for (int y = p_first; y < p_end; y++) {
					float *dst = r_dst + size_t(y) * p_width;
					const int sy = y + dy;
					if (sy < 0 || sy >= p_height) {
						for (int x = 0; x < p_width; x++) {
							dst[x] = MAX(dst[x], p_outside);
						}
						continue;
					}
					const float *src = horizontal.ptr() + size_t(sy) * p_width;
					for (int x = 0; x < p_width; x++) {
						dst[x] = MAX(dst[x], src[x]);
					}
				}
			});
		}
	}
}

void TetraImageDocument::grow_selection(int p_pixels) {
	TETRA_REQUIRE_IDLE();
	if (selection_empty || p_pixels <= 0) {
		return;
	}
	Vector<float> mask;
	mask.resize(int64_t(width) * height);
	dilate_mask(selection.ptr(), mask.ptrw(), width, height, p_pixels, 0.0f);
	_set_selection(mask, TTR("Grow Selection"));
}

void TetraImageDocument::shrink_selection(int p_pixels) {
	TETRA_REQUIRE_IDLE();
	if (selection_empty || p_pixels <= 0) {
		return;
	}
	// Shrinking is growing the complement; the area outside the canvas
	// counts as unselected, so the selection also retreats from the border.
	const int64_t count = int64_t(width) * height;
	LocalVector<float> inverted;
	inverted.resize(count);
	const float *old = selection.ptr();
	for (int64_t i = 0; i < count; i++) {
		inverted[i] = 1.0f - old[i];
	}
	Vector<float> mask;
	mask.resize(count);
	float *out = mask.ptrw();
	dilate_mask(inverted.ptr(), out, width, height, p_pixels, 1.0f);
	for (int64_t i = 0; i < count; i++) {
		out[i] = 1.0f - out[i];
	}
	_set_selection(mask, TTR("Shrink Selection"));
}

Ref<Image> TetraImageDocument::get_selection_mask() const {
	ERR_FAIL_COND_V(width == 0, Ref<Image>());
	Vector<uint8_t> data;
	data.resize_initialized(int64_t(width) * height * 4);
	if (!selection_empty) {
		memcpy(data.ptrw(), selection.ptr(), data.size());
	}
	return Image::create_from_data(width, height, false, Image::FORMAT_RF, data);
}

void TetraImageDocument::_bind_selection_methods() {
	ClassDB::bind_method(D_METHOD("select_all"), &TetraImageDocument::select_all);
	ClassDB::bind_method(D_METHOD("select_none"), &TetraImageDocument::select_none);
	ClassDB::bind_method(D_METHOD("select_rect", "rect", "op"), &TetraImageDocument::select_rect, DEFVAL(SELECT_REPLACE));
	ClassDB::bind_method(D_METHOD("select_ellipse", "rect", "op", "antialias"), &TetraImageDocument::select_ellipse, DEFVAL(SELECT_REPLACE), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_polygon", "points", "op", "antialias"), &TetraImageDocument::select_polygon, DEFVAL(SELECT_REPLACE), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_by_color", "point", "threshold", "contiguous", "sample_merged", "op"), &TetraImageDocument::select_by_color, DEFVAL(0.0588f), DEFVAL(false), DEFVAL(false), DEFVAL(SELECT_REPLACE));
	ClassDB::bind_method(D_METHOD("select_fuzzy", "point", "threshold", "sample_merged", "op"), &TetraImageDocument::select_fuzzy, DEFVAL(0.0588f), DEFVAL(false), DEFVAL(SELECT_REPLACE));
	ClassDB::bind_method(D_METHOD("select_mask", "mask", "op"), &TetraImageDocument::select_mask, DEFVAL(SELECT_REPLACE));
	ClassDB::bind_method(D_METHOD("invert_selection"), &TetraImageDocument::invert_selection);
	ClassDB::bind_method(D_METHOD("feather_selection", "radius"), &TetraImageDocument::feather_selection);
	ClassDB::bind_method(D_METHOD("grow_selection", "pixels"), &TetraImageDocument::grow_selection);
	ClassDB::bind_method(D_METHOD("shrink_selection", "pixels"), &TetraImageDocument::shrink_selection);
	ClassDB::bind_method(D_METHOD("get_selection_mask"), &TetraImageDocument::get_selection_mask);
	ClassDB::bind_method(D_METHOD("is_selection_empty"), &TetraImageDocument::is_selection_empty);
	ClassDB::bind_method(D_METHOD("selection_bounds"), &TetraImageDocument::selection_bounds);

	BIND_ENUM_CONSTANT(SELECT_REPLACE);
	BIND_ENUM_CONSTANT(SELECT_ADD);
	BIND_ENUM_CONSTANT(SELECT_SUBTRACT);
	BIND_ENUM_CONSTANT(SELECT_INTERSECT);
}
