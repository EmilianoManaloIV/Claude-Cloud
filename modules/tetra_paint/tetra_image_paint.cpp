// The paint engine follows GIMP's brush core: the parametric brush falloff is
// ported from app/core/gimpbrushgenerated.c, the per-stroke coverage rules
// from app/paint/gimppaintcore-loops.cc and the smudge blend from
// app/gegl/gimp-gegl-loops.cc.
// Copyright (C) 1995 Spencer Kimball and Peter Mattis; 2013 Daniel Sabo;
// 2018 Ell. GPL-3.0-or-later.

#include "tetra_image_document.h"

#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

#include <cmath>
#include <cstring>

// Samples per pixel of the radial brush profile, as in GIMP.
static const int PROFILE_OVERSAMPLING = 4;

static inline double profile_gauss(double p_f) {
	if (p_f < -0.5) {
		p_f = -1.0 - p_f;
		return 2.0 * p_f * p_f;
	}
	if (p_f < 0.5) {
		return 1.0 - 2.0 * p_f * p_f;
	}
	p_f = 1.0 - p_f;
	return 2.0 * p_f * p_f;
}

// Builds the lookup table of brush opacity by distance from the centre: the
// hardness-shaped falloff averaged over one pixel, which antialiases the rim.
void TetraImageDocument::_stroke_update_profile(float p_radius, float p_hardness) {
	if (stroke.profile_radius == p_radius && stroke.profile_hardness == p_hardness) {
		return;
	}
	stroke.profile_radius = p_radius;
	stroke.profile_hardness = p_hardness;

	const double radius = p_radius;
	const int length = PROFILE_OVERSAMPLING * int(ceil(1.0 + sqrt(2.0 * Math::pow(ceil(radius + 1.0), 2.0))));
	stroke.profile.resize(length);
	const double exponent = (1.0 - p_hardness) < 0.0000004 ? 1000000.0 : 0.4 / (1.0 - p_hardness);

	double buffer[PROFILE_OVERSAMPLING];
	double sum = 0.0;
	double d = 0.0;
	for (int i = 0; i < PROFILE_OVERSAMPLING; i++) {
		d = Math::abs((i + 0.5) / PROFILE_OVERSAMPLING - 0.5);
		buffer[i] = d > radius ? 0.0 : profile_gauss(pow(d / radius, exponent));
		sum += buffer[i];
	}
	int x = 0;
	for (; (d < radius || sum > 0.00001) && x < length; d += 1.0 / PROFILE_OVERSAMPLING) {
		sum -= buffer[x % PROFILE_OVERSAMPLING];
		buffer[x % PROFILE_OVERSAMPLING] = d > radius ? 0.0 : profile_gauss(pow(d / radius, exponent));
		sum += buffer[x % PROFILE_OVERSAMPLING];
		stroke.profile[x++] = float(sum / PROFILE_OVERSAMPLING);
	}
	while (x < length) {
		stroke.profile[x++] = 0.0f;
	}
}

Dictionary TetraImageDocument::get_default_stroke_settings(PaintTool p_tool) {
	Dictionary settings;
	settings["color"] = Color(0, 0, 0, 1);
	settings["size"] = 20.0;
	settings["hardness"] = p_tool == TOOL_PENCIL ? 1.0 : 0.5;
	settings["opacity"] = 1.0;
	settings["flow"] = p_tool == TOOL_AIRBRUSH ? 0.1 : 1.0;
	settings["spacing"] = 10.0;
	settings["mode"] = LAYER_MODE_NORMAL;
	settings["pressure_size"] = false;
	settings["pressure_opacity"] = false;
	if (p_tool == TOOL_SMUDGE) {
		settings["rate"] = 0.5;
	}
	if (p_tool == TOOL_CLONE) {
		settings["source_offset"] = Vector2();
	}
	return settings;
}

Error TetraImageDocument::stroke_begin(int p_layer_id, PaintTool p_tool, const Dictionary &p_settings) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_INDEX_V(int(p_tool), int(TOOL_MAX), ERR_INVALID_PARAMETER);
	const int index = _find_layer(p_layer_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);

	Dictionary settings = get_default_stroke_settings(p_tool);
	settings.merge(p_settings, true);

	stroke = Stroke();
	stroke.layer_id = p_layer_id;
	stroke.tool = p_tool;
	const Color color = settings["color"];
	stroke.color[0] = color.r;
	stroke.color[1] = color.g;
	stroke.color[2] = color.b;
	stroke.color[3] = color.a;
	stroke.size = MAX(float(settings["size"]), 1.0f);
	stroke.hardness = CLAMP(float(settings["hardness"]), 0.0f, 1.0f);
	stroke.opacity = CLAMP(float(settings["opacity"]), 0.0f, 1.0f);
	stroke.flow = CLAMP(float(settings["flow"]), 0.0f, 1.0f);
	stroke.spacing = CLAMP(float(settings["spacing"]), 1.0f, 5000.0f);
	stroke.mode = CLAMP(int(settings["mode"]), 0, int(LAYER_MODE_MAX) - 1);
	stroke.pressure_size = settings["pressure_size"];
	stroke.pressure_opacity = settings["pressure_opacity"];
	stroke.rate = CLAMP(float(settings.get("rate", 0.5)), 0.0f, 1.0f);
	if (p_settings.has("source_point")) {
		stroke.has_source_point = true;
		stroke.source_point = p_settings["source_point"];
	} else {
		stroke.has_source_offset = true;
		stroke.source_offset = settings.get("source_offset", Vector2());
	}

	stroke.before = layers[index].pixels;
	stroke.coverage.resize_initialized(size_t(width) * height);
	stroke.active = true;
	return OK;
}

// Stamps the brush once. Paintbrush, pencil, eraser, airbrush and clone add
// the dab to the stroke's coverage and re-composite the covered pixels from
// the pre-stroke layer, so overlapping dabs of one stroke never exceed the
// stroke opacity. Smudge works on the current pixels instead.
void TetraImageDocument::_stroke_dab(const Vector2 &p_position, float p_pressure, Rect2i &r_dirty) {
	const int index = _find_layer(stroke.layer_id);
	Layer &layer = layers[index];

	const float radius = MAX(stroke.size * 0.5f * (stroke.pressure_size ? p_pressure : 1.0f), 0.5f);
	const float paint_opacity = MIN(stroke.flow * (stroke.pressure_opacity ? p_pressure : 1.0f), 1.0f);
	if (paint_opacity <= 0.0f) {
		return;
	}
	const bool hard = stroke.tool == TOOL_PENCIL;
	// The pencil snaps to the pixel grid; the other tools place the dab with
	// sub-pixel precision by evaluating the profile from the true centre.
	const double center_x = hard ? floor(p_position.x) + 0.5 : double(p_position.x);
	const double center_y = hard ? floor(p_position.y) + 0.5 : double(p_position.y);
	const int left = int(floor(center_x - radius - 1.0));
	const int top = int(floor(center_y - radius - 1.0));
	const int right = int(ceil(center_x + radius + 1.0));
	const int bottom = int(ceil(center_y + radius + 1.0));
	Rect2i rect = _clip_rect(Rect2i(left, top, right - left, bottom - top));
	if (!selection_empty) {
		rect = rect.intersection(selection_bounds());
	}
	if (!hard) {
		_stroke_update_profile(radius, stroke.hardness);
	}

	const float *mask_px = _selection_ptr();
	const float image_opacity = stroke.opacity * stroke.color[3];
	const int w = rect.size.x;

	if (stroke.tool == TOOL_SMUDGE && !stroke.smudge_ready) {
		// The first dab only loads the brush with the pixels under it.
		stroke.smudge_side = 2 * int(ceil(stroke.size * 0.5f + 1.0f)) + 3;
		stroke.smudge.resize_initialized(size_t(stroke.smudge_side) * stroke.smudge_side * 4);
		const int half = stroke.smudge_side / 2;
		const int origin_x = int(floor(center_x)) - half;
		const int origin_y = int(floor(center_y)) - half;
		const float *src = layer.pixels.ptr();
		for (int ay = 0; ay < stroke.smudge_side; ay++) {
			for (int ax = 0; ax < stroke.smudge_side; ax++) {
				const int px = origin_x + ax;
				const int py = origin_y + ay;
				if (px >= 0 && px < width && py >= 0 && py < height) {
					memcpy(stroke.smudge.ptr() + (size_t(ay) * stroke.smudge_side + ax) * 4, src + (size_t(py) * width + px) * 4, sizeof(float) * 4);
				}
			}
		}
		stroke.smudge_ready = true;
		return;
	}
	if (!rect.has_area()) {
		return;
	}

	float *dst = layer.pixels.ptrw();
	const float *before = stroke.before.ptr();
	LocalVector<float> mask, paint, result;
	mask.resize(w);
	paint.resize(w * 4);
	result.resize(w * 4);

	int mode = stroke.mode;
	int composite_mode = TetraLayerModes::get_info(mode).paint_composite;
	if (stroke.tool == TOOL_ERASER) {
		mode = TetraLayerModes::MODE_ERASE;
		composite_mode = TetraLayerModes::COMPOSITE_CLIP_TO_BACKDROP;
	}
	const int source_dx = int(Math::round(stroke.source_offset.x));
	const int source_dy = int(Math::round(stroke.source_offset.y));
	const int smudge_half = stroke.smudge_side / 2;
	const int smudge_origin_x = int(floor(center_x)) - smudge_half;
	const int smudge_origin_y = int(floor(center_y)) - smudge_half;

	for (int y = rect.position.y; y < rect.position.y + rect.size.y; y++) {
		const size_t offset = size_t(y) * width + rect.position.x;
		const double dy = y + 0.5 - center_y;
		for (int col = 0; col < w; col++) {
			const double dx = rect.position.x + col + 0.5 - center_x;
			const double distance = sqrt(dx * dx + dy * dy);
			if (hard) {
				mask[col] = distance <= radius ? 1.0f : 0.0f;
			} else {
				const uint32_t slot = uint32_t(Math::round(distance * PROFILE_OVERSAMPLING));
				mask[col] = (distance < radius + 1.0 && slot < stroke.profile.size()) ? stroke.profile[slot] : 0.0f;
			}
		}

		if (stroke.tool == TOOL_SMUDGE) {
			for (int col = 0; col < w; col++) {
				const int ax = rect.position.x + col - smudge_origin_x;
				const int ay = y - smudge_origin_y;
				if (ax < 0 || ax >= stroke.smudge_side || ay < 0 || ay >= stroke.smudge_side) {
					continue;
				}
				float *carried = stroke.smudge.ptr() + (size_t(ay) * stroke.smudge_side + ax) * 4;
				float *current = dst + (offset + col) * 4;
				const float carried_alpha = stroke.rate * carried[3];
				const float current_alpha = (1.0f - stroke.rate) * current[3];
				const float alpha = carried_alpha + current_alpha;
				if (alpha == 0.0f) {
					carried[0] = carried[1] = carried[2] = carried[3] = 0.0f;
				} else {
					for (int c = 0; c < 3; c++) {
						carried[c] = (carried[c] * carried_alpha + current[c] * current_alpha) / alpha;
					}
					carried[3] = alpha;
				}
				const float coverage = mask[col] * paint_opacity * (mask_px ? mask_px[offset + col] : 1.0f);
				float out[4];
				TetraLayerModes::process_replace(current, carried, &coverage, out, image_opacity, 1);
				if (layer.lock_alpha) {
					if (current[3] == 0.0f) {
						continue;
					}
					out[3] = current[3];
				}
				memcpy(current, out, sizeof(float) * 4);
			}
			continue;
		}

		float *coverage = stroke.coverage.ptr() + offset;
		for (int col = 0; col < w; col++) {
			float c = coverage[col];
			if (stroke.tool == TOOL_AIRBRUSH) {
				c += (1.0f - c) * mask[col] * paint_opacity;
			} else if (paint_opacity > c) {
				c += (paint_opacity - c) * mask[col] * paint_opacity;
			}
			coverage[col] = c;
			mask[col] = c * (mask_px ? mask_px[offset + col] : 1.0f);

			float *px = paint.ptr() + col * 4;
			if (stroke.tool == TOOL_CLONE) {
				const int sx = rect.position.x + col + source_dx;
				const int sy = y + source_dy;
				if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
					memcpy(px, before + (size_t(sy) * width + sx) * 4, sizeof(float) * 4);
				} else {
					px[0] = px[1] = px[2] = px[3] = 0.0f;
				}
			} else {
				px[0] = stroke.color[0];
				px[1] = stroke.color[1];
				px[2] = stroke.color[2];
				px[3] = 1.0f;
			}
		}
		const float *source = before + offset * 4;
		TetraLayerModes::process(mode, composite_mode, source, paint.ptr(), mask.ptr(), result.ptr(), image_opacity, w, rect.position.x, y);
		if (layer.lock_alpha) {
			for (int col = 0; col < w; col++) {
				if (source[col * 4 + 3] == 0.0f) {
					memcpy(result.ptr() + col * 4, source + col * 4, sizeof(float) * 4);
				} else {
					result[col * 4 + 3] = source[col * 4 + 3];
				}
			}
		}
		memcpy(dst + offset * 4, result.ptr(), sizeof(float) * 4 * w);
	}
	r_dirty = r_dirty.has_area() ? r_dirty.merge(rect) : rect;
}

void TetraImageDocument::stroke_add_point(const Vector2 &p_position, float p_pressure) {
	ERR_FAIL_COND_MSG(!stroke.active, "stroke_add_point() without stroke_begin().");
	const float pressure = CLAMP(p_pressure, 0.0f, 1.0f);
	Rect2i dirty;
	if (!stroke.has_last) {
		if (stroke.has_source_point) {
			stroke.source_offset = stroke.source_point - p_position;
		}
		_stroke_dab(p_position, pressure, dirty);
		stroke.distance_since_dab = 0.0;
	} else {
		const Vector2 delta = p_position - stroke.last_position;
		const double length = delta.length();
		if (length == 0.0) {
			// A resting airbrush keeps spraying; the other tools need motion.
			if (stroke.tool == TOOL_AIRBRUSH) {
				_stroke_dab(p_position, pressure, dirty);
			}
		} else {
			// Dabs are spaced by a percentage of the brush size, but never
			// closer than one pixel along the dominant axis.
			const double pixel_step = length / MAX(Math::abs(delta.x), Math::abs(delta.y));
			const double step = MAX(double(stroke.spacing) * 0.01 * stroke.size, pixel_step);
			double travelled = MAX(step - stroke.distance_since_dab, 0.0);
			double last_dab = -stroke.distance_since_dab;
			// The tolerance keeps the dab that lands exactly on the end point.
			while (travelled <= length + 1e-6) {
				const float f = MIN(float(travelled / length), 1.0f);
				_stroke_dab(stroke.last_position + delta * f, stroke.last_pressure + (pressure - stroke.last_pressure) * f, dirty);
				last_dab = travelled;
				travelled += step;
			}
			stroke.distance_since_dab = length - last_dab;
		}
	}
	stroke.last_position = p_position;
	stroke.last_pressure = pressure;
	stroke.has_last = true;
	if (dirty.has_area()) {
		stroke.bounds = stroke.bounds.has_area() ? stroke.bounds.merge(dirty) : dirty;
		_mark_dirty(dirty);
	}
}

void TetraImageDocument::stroke_end() {
	ERR_FAIL_COND_MSG(!stroke.active, "stroke_end() without stroke_begin().");
	static const char *tool_names[TOOL_MAX] = { "Paintbrush", "Pencil", "Eraser", "Airbrush", "Smudge", "Clone" };
	const bool painted = stroke.bounds.has_area();
	if (painted) {
		_undo_open(TTR(tool_names[stroke.tool]));
		_undo_push_pixels(stroke.layer_id, stroke.bounds, stroke.before.ptr());
	}
	stroke = Stroke();
	if (painted) {
		_undo_close();
	}
}

void TetraImageDocument::stroke_cancel() {
	ERR_FAIL_COND_MSG(!stroke.active, "stroke_cancel() without stroke_begin().");
	const Rect2i bounds = stroke.bounds;
	const int index = _find_layer(stroke.layer_id);
	if (bounds.has_area() && index >= 0) {
		float *dst = layers[index].pixels.ptrw();
		const float *before = stroke.before.ptr();
		for (int y = bounds.position.y; y < bounds.position.y + bounds.size.y; y++) {
			const size_t offset = (size_t(y) * width + bounds.position.x) * 4;
			memcpy(dst + offset, before + offset, sizeof(float) * 4 * bounds.size.x);
		}
	}
	stroke = Stroke();
	_mark_dirty(bounds);
}

Error TetraImageDocument::stroke_line(int p_layer_id, PaintTool p_tool, const Dictionary &p_settings, const Vector2 &p_from, const Vector2 &p_to) {
	const Error err = stroke_begin(p_layer_id, p_tool, p_settings);
	if (err != OK) {
		return err;
	}
	stroke_add_point(p_from);
	stroke_add_point(p_to);
	stroke_end();
	return OK;
}

void TetraImageDocument::_bind_paint_methods() {
	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("get_default_stroke_settings", "tool"), &TetraImageDocument::get_default_stroke_settings);
	ClassDB::bind_method(D_METHOD("stroke_begin", "layer_id", "tool", "settings"), &TetraImageDocument::stroke_begin);
	ClassDB::bind_method(D_METHOD("stroke_add_point", "position", "pressure"), &TetraImageDocument::stroke_add_point, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("stroke_end"), &TetraImageDocument::stroke_end);
	ClassDB::bind_method(D_METHOD("stroke_cancel"), &TetraImageDocument::stroke_cancel);
	ClassDB::bind_method(D_METHOD("is_stroke_active"), &TetraImageDocument::is_stroke_active);
	ClassDB::bind_method(D_METHOD("stroke_line", "layer_id", "tool", "settings", "from", "to"), &TetraImageDocument::stroke_line);

	BIND_ENUM_CONSTANT(TOOL_PAINTBRUSH);
	BIND_ENUM_CONSTANT(TOOL_PENCIL);
	BIND_ENUM_CONSTANT(TOOL_ERASER);
	BIND_ENUM_CONSTANT(TOOL_AIRBRUSH);
	BIND_ENUM_CONSTANT(TOOL_SMUDGE);
	BIND_ENUM_CONSTANT(TOOL_CLONE);
}
