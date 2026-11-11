#include "tetra_image_document.h"

#include "tetra_paint_utils.h"

#include "core/object/class_db.h"

#include <cstring>

int TetraImageDocument::_find_layer(int p_id) const {
	for (uint32_t i = 0; i < layers.size(); i++) {
		if (layers[i].id == p_id) {
			return int(i);
		}
	}
	return -1;
}

Rect2i TetraImageDocument::_clip_rect(const Rect2i &p_rect) const {
	return p_rect.intersection(_canvas_rect());
}

Rect2i TetraImageDocument::_edit_rect() const {
	return selection_empty ? _canvas_rect() : selection_bounds();
}

void TetraImageDocument::_mark_dirty(const Rect2i &p_rect) {
	const Rect2i rect = _clip_rect(p_rect);
	if (!rect.has_area()) {
		return;
	}
	dirty_rect = dirty_rect.has_area() ? dirty_rect.merge(rect) : rect;
	emit_signal(SNAME("changed"), rect);
}

Rect2i TetraImageDocument::get_and_clear_dirty_rect() {
	const Rect2i rect = dirty_rect;
	dirty_rect = Rect2i();
	return rect;
}

void TetraImageDocument::_reset(int p_width, int p_height) {
	width = p_width;
	height = p_height;
	layers.clear();
	active_layer = -1;
	selection = Vector<float>();
	selection_empty = true;
	selection_bounds_valid = false;
	dirty_rect = Rect2i();
}

Vector<float> TetraImageDocument::_new_buffer(const float *p_fill) const {
	Vector<float> buffer;
	const int64_t count = int64_t(width) * height;
	buffer.resize_initialized(count * 4);
	if (p_fill) {
		float *w = buffer.ptrw();
		for (int64_t i = 0; i < count; i++) {
			memcpy(w + i * 4, p_fill, sizeof(float) * 4);
		}
	}
	return buffer;
}

int TetraImageDocument::_append_layer(const String &p_name, const Vector<float> &p_pixels) {
	Layer layer;
	layer.id = next_layer_id++;
	layer.name = p_name;
	layer.pixels = p_pixels;
	layers.push_back(layer);
	return layer.id;
}

Error TetraImageDocument::create(int p_width, int p_height, const Color &p_fill) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(p_width < 1 || p_height < 1 || p_width > 16384 || p_height > 16384, ERR_INVALID_PARAMETER);
	_reset(p_width, p_height);
	const float fill[4] = { p_fill.r, p_fill.g, p_fill.b, p_fill.a };
	active_layer = _append_layer(TTR("Background"), _new_buffer(p_fill.a > 0.0f ? fill : nullptr));
	_clear_history();
	emit_signal(SNAME("size_changed"));
	emit_signal(SNAME("layers_changed"));
	emit_signal(SNAME("selection_changed"));
	_mark_dirty(_canvas_rect());
	return OK;
}

// Composites one row of the visible layers, bottom to top, the way GIMP's
// projection does: the bottom visible layer is shown as it is (only its
// opacity applies) and every layer above goes through its mode.
void TetraImageDocument::_composite_row(int p_y, int p_x, int p_count, float *r_row) const {
	memset(r_row, 0, sizeof(float) * 4 * p_count);
	bool bottom = true;
	for (const Layer &layer : layers) {
		if (!layer.visible) {
			continue;
		}
		const float *src = layer.pixels.ptr() + (size_t(p_y) * width + p_x) * 4;
		if (bottom && layer.mode != LAYER_MODE_DISSOLVE) {
			memcpy(r_row, src, sizeof(float) * 4 * p_count);
			if (layer.opacity != 1.0f) {
				for (int x = 0; x < p_count; x++) {
					r_row[x * 4 + 3] *= layer.opacity;
				}
			}
		} else {
			TetraLayerModes::process(layer.mode, TetraLayerModes::get_info(layer.mode).layer_composite, r_row, src, nullptr, r_row, layer.opacity, p_count, p_x, p_y);
		}
		bottom = false;
	}
}

void TetraImageDocument::_composite_into(const Rect2i &p_rect, float *r_out) const {
	const int w = p_rect.size.x;
	tetra_parallel_rows(p_rect.size.y, w * int(layers.size()), [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			_composite_row(p_rect.position.y + y, p_rect.position.x, w, r_out + size_t(y) * w * 4);
		}
	});
}

Ref<Image> TetraImageDocument::composite_float(const Rect2i &p_rect) const {
	const Rect2i rect = _clip_rect(p_rect.has_area() ? p_rect : _canvas_rect());
	if (!rect.has_area()) {
		return Ref<Image>();
	}
	Vector<uint8_t> data;
	data.resize(int64_t(rect.size.x) * rect.size.y * 16);
	_composite_into(rect, reinterpret_cast<float *>(data.ptrw()));
	return Image::create_from_data(rect.size.x, rect.size.y, false, Image::FORMAT_RGBAF, data);
}

Ref<Image> TetraImageDocument::composite(const Rect2i &p_rect) const {
	const Rect2i rect = _clip_rect(p_rect.has_area() ? p_rect : _canvas_rect());
	if (!rect.has_area()) {
		return Ref<Image>();
	}
	const int w = rect.size.x;
	Vector<uint8_t> data;
	data.resize(int64_t(w) * rect.size.y * 4);
	uint8_t *out = data.ptrw();
	tetra_parallel_rows(rect.size.y, w * int(layers.size()), [&](int p_first, int p_end) {
		LocalVector<float> row;
		row.resize(w * 4);
		for (int y = p_first; y < p_end; y++) {
			_composite_row(rect.position.y + y, rect.position.x, w, row.ptr());
			uint8_t *dst = out + size_t(y) * w * 4;
			for (int x = 0; x < w; x++) {
				const float *px = row.ptr() + x * 4;
				dst[x * 4 + 0] = tetra_linear_to_srgb8(px[0]);
				dst[x * 4 + 1] = tetra_linear_to_srgb8(px[1]);
				dst[x * 4 + 2] = tetra_linear_to_srgb8(px[2]);
				const float alpha = px[3] < 0.0f ? 0.0f : (px[3] > 1.0f ? 1.0f : px[3]);
				dst[x * 4 + 3] = uint8_t(alpha * 255.0f + 0.5f);
			}
		}
	});
	return Image::create_from_data(w, rect.size.y, false, Image::FORMAT_RGBA8, data);
}

void TetraImageDocument::_mix_into(float *p_dest, const Rect2i &p_rect, const float *p_src, int p_src_width, const Point2i &p_src_origin, bool p_lock_alpha, bool p_use_selection) const {
	const float *mask = p_use_selection ? _selection_ptr() : nullptr;
	const int w = p_rect.size.x;
	tetra_parallel_rows(p_rect.size.y, w, [&](int p_first, int p_end) {
		for (int row = p_first; row < p_end; row++) {
			const int y = p_rect.position.y + row;
			float *d = p_dest + (size_t(y) * width + p_rect.position.x) * 4;
			const float *s = p_src + (size_t(y - p_src_origin.y) * p_src_width + (p_rect.position.x - p_src_origin.x)) * 4;
			const float *m = mask ? mask + size_t(y) * width + p_rect.position.x : nullptr;
			if (!m && !p_lock_alpha) {
				memcpy(d, s, sizeof(float) * 4 * w);
				continue;
			}
			for (int x = 0; x < w; x++, d += 4, s += 4) {
				float result[4];
				if (m) {
					TetraLayerModes::process_replace(d, s, m + x, result, 1.0f, 1);
				} else {
					memcpy(result, s, sizeof(float) * 4);
				}
				if (p_lock_alpha) {
					if (d[3] == 0.0f) {
						continue;
					}
					result[3] = d[3];
				}
				memcpy(d, result, sizeof(float) * 4);
			}
		}
	});
}

void TetraImageDocument::_commit_pixels(int p_index, const Rect2i &p_rect, const float *p_src, int p_src_width, const Point2i &p_src_origin, bool p_use_selection) {
	Rect2i rect = _clip_rect(p_rect);
	if (p_use_selection && !selection_empty) {
		rect = rect.intersection(selection_bounds());
	}
	if (!rect.has_area()) {
		return;
	}
	Layer &layer = layers[p_index];
	_undo_push_pixels(layer.id, rect, layer.pixels.ptr());
	_mix_into(layer.pixels.ptrw(), rect, p_src, p_src_width, p_src_origin, layer.lock_alpha, p_use_selection);
	_mark_dirty(rect);
}

Rect2i TetraImageDocument::_diff_bounds(const float *p_a, const float *p_b) const {
	const size_t row_bytes = sizeof(float) * 4 * width;
	int top = 0;
	while (top < height && memcmp(p_a + size_t(top) * width * 4, p_b + size_t(top) * width * 4, row_bytes) == 0) {
		top++;
	}
	if (top == height) {
		return Rect2i();
	}
	int bottom = height - 1;
	while (bottom > top && memcmp(p_a + size_t(bottom) * width * 4, p_b + size_t(bottom) * width * 4, row_bytes) == 0) {
		bottom--;
	}
	int left = width;
	int right = -1;
	for (int y = top; y <= bottom; y++) {
		const float *a = p_a + size_t(y) * width * 4;
		const float *b = p_b + size_t(y) * width * 4;
		for (int x = 0; x < left; x++) {
			if (memcmp(a + x * 4, b + x * 4, sizeof(float) * 4) != 0) {
				left = x;
				break;
			}
		}
		for (int x = width - 1; x > right; x--) {
			if (memcmp(a + x * 4, b + x * 4, sizeof(float) * 4) != 0) {
				right = x;
				break;
			}
		}
	}
	return Rect2i(left, top, right - left + 1, bottom - top + 1);
}

Vector<float> TetraImageDocument::_image_to_linear(const Ref<Image> &p_image, bool p_force_linearise) {
	Vector<float> pixels;
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), pixels);
	Ref<Image> image = p_image->duplicate();
	if (image->is_compressed()) {
		ERR_FAIL_COND_V(image->decompress() != OK, pixels);
	}
	if (image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	const Image::Format format = image->get_format();
	const bool is_float = format >= Image::FORMAT_RF && format <= Image::FORMAT_RGBE9995;
	image->convert(Image::FORMAT_RGBAF);
	const int w = image->get_width();
	const int h = image->get_height();
	pixels.resize(int64_t(w) * h * 4);
	float *dst = pixels.ptrw();
	memcpy(dst, image->ptr(), sizeof(float) * 4 * size_t(w) * h);
	if (!is_float || p_force_linearise) {
		tetra_parallel_rows(h, w, [&](int p_first, int p_end) {
			for (int64_t i = int64_t(p_first) * w; i < int64_t(p_end) * w; i++) {
				for (int c = 0; c < 3; c++) {
					dst[i * 4 + c] = TetraLayerModes::srgb_to_linear(dst[i * 4 + c]);
				}
			}
		});
	}
	return pixels;
}

// Layers.

void TetraImageDocument::_finish_structure_change(DocState &p_before, bool p_redraw) {
	_undo_push_state(p_before);
	_undo_close();
	emit_signal(SNAME("layers_changed"));
	if (p_redraw) {
		_mark_dirty(_canvas_rect());
	}
}

int TetraImageDocument::add_layer(const String &p_name, int p_index) {
	TETRA_REQUIRE_IDLE_V(-1);
	ERR_FAIL_COND_V_MSG(width == 0, -1, "Create or load a document first.");
	_undo_open(TTR("Add Layer"));
	DocState before = _capture_state();
	Layer layer;
	layer.id = next_layer_id++;
	layer.name = p_name;
	layer.pixels = _new_buffer();
	const int index = (p_index < 0 || p_index > int(layers.size())) ? int(layers.size()) : p_index;
	layers.insert(index, layer);
	active_layer = layer.id;
	_finish_structure_change(before, false);
	return layer.id;
}

Error TetraImageDocument::remove_layer(int p_id) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	_undo_open(TTR("Remove Layer"));
	DocState before = _capture_state();
	layers.remove_at(index);
	if (active_layer == p_id) {
		active_layer = layers.is_empty() ? -1 : layers[index > 0 ? index - 1 : 0].id;
	}
	_finish_structure_change(before, true);
	return OK;
}

int TetraImageDocument::duplicate_layer(int p_id) {
	TETRA_REQUIRE_IDLE_V(-1);
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, -1);
	_undo_open(TTR("Duplicate Layer"));
	DocState before = _capture_state();
	Layer layer = layers[index];
	layer.id = next_layer_id++;
	layer.name = vformat(TTR("%s copy"), layer.name);
	layers.insert(index + 1, layer);
	active_layer = layer.id;
	_finish_structure_change(before, true);
	return layer.id;
}

Error TetraImageDocument::move_layer(int p_id, int p_new_index) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	const int target = CLAMP(p_new_index, 0, int(layers.size()) - 1);
	if (target == index) {
		return OK;
	}
	_undo_open(TTR("Reorder Layer"));
	DocState before = _capture_state();
	const Layer layer = layers[index];
	layers.remove_at(index);
	layers.insert(target, layer);
	_finish_structure_change(before, true);
	return OK;
}

Error TetraImageDocument::merge_down(int p_id) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V_MSG(index == 0, ERR_INVALID_PARAMETER, "There is no layer below to merge into.");
	_undo_open(TTR("Merge Down"));
	DocState before = _capture_state();

	// The lower layer's opacity is baked into its pixels and the upper layer
	// is composited over it with its own mode and opacity; the result keeps
	// the lower layer's name and mode at full opacity.
	const Layer &upper = layers[index];
	Layer &lower = layers[index - 1];
	Vector<float> merged;
	merged.resize(int64_t(width) * height * 4);
	float *out = merged.ptrw();
	const float *lower_px = lower.pixels.ptr();
	const float *upper_px = upper.pixels.ptr();
	const float lower_opacity = lower.opacity;
	const int composite_mode = TetraLayerModes::get_info(upper.mode).layer_composite;
	tetra_parallel_rows(height, width, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			float *row = out + size_t(y) * width * 4;
			memcpy(row, lower_px + size_t(y) * width * 4, sizeof(float) * 4 * width);
			if (lower_opacity != 1.0f) {
				for (int x = 0; x < width; x++) {
					row[x * 4 + 3] *= lower_opacity;
				}
			}
			TetraLayerModes::process(upper.mode, composite_mode, row, upper_px + size_t(y) * width * 4, nullptr, row, upper.opacity, width, 0, y);
		}
	});
	lower.pixels = merged;
	lower.opacity = 1.0f;
	active_layer = lower.id;
	layers.remove_at(index);
	_finish_structure_change(before, true);
	return OK;
}

Error TetraImageDocument::flatten() {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	ERR_FAIL_COND_V(layers.is_empty(), ERR_UNCONFIGURED);
	_undo_open(TTR("Flatten Image"));
	DocState before = _capture_state();
	Vector<float> merged;
	merged.resize(int64_t(width) * height * 4);
	_composite_into(_canvas_rect(), merged.ptrw());
	String name = layers[0].name;
	for (const Layer &layer : layers) {
		if (layer.visible) {
			name = layer.name;
			break;
		}
	}
	layers.clear();
	active_layer = _append_layer(name, merged);
	_finish_structure_change(before, true);
	return OK;
}

PackedInt32Array TetraImageDocument::get_layer_ids() const {
	PackedInt32Array ids;
	for (const Layer &layer : layers) {
		ids.push_back(layer.id);
	}
	return ids;
}

void TetraImageDocument::set_active_layer(int p_id) {
	ERR_FAIL_COND(_find_layer(p_id) < 0);
	if (active_layer != p_id) {
		active_layer = p_id;
		emit_signal(SNAME("layers_changed"));
	}
}

String TetraImageDocument::layer_get_name(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, String());
	return layers[index].name;
}

void TetraImageDocument::layer_set_name(int p_id, const String &p_name) {
	TETRA_REQUIRE_IDLE();
	const int index = _find_layer(p_id);
	ERR_FAIL_COND(index < 0);
	if (layers[index].name == p_name) {
		return;
	}
	_undo_open(TTR("Rename Layer"));
	DocState before = _capture_state();
	layers[index].name = p_name;
	_finish_structure_change(before, false);
}

bool TetraImageDocument::layer_is_visible(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, false);
	return layers[index].visible;
}

void TetraImageDocument::layer_set_visible(int p_id, bool p_visible) {
	TETRA_REQUIRE_IDLE();
	const int index = _find_layer(p_id);
	ERR_FAIL_COND(index < 0);
	if (layers[index].visible == p_visible) {
		return;
	}
	_undo_open(TTR("Layer Visibility"));
	DocState before = _capture_state();
	layers[index].visible = p_visible;
	_finish_structure_change(before, true);
}

float TetraImageDocument::layer_get_opacity(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, 0.0f);
	return layers[index].opacity;
}

void TetraImageDocument::layer_set_opacity(int p_id, float p_opacity) {
	TETRA_REQUIRE_IDLE();
	const int index = _find_layer(p_id);
	ERR_FAIL_COND(index < 0);
	const float opacity = CLAMP(p_opacity, 0.0f, 1.0f);
	if (layers[index].opacity == opacity) {
		return;
	}
	_undo_open(TTR("Layer Opacity"));
	DocState before = _capture_state();
	layers[index].opacity = opacity;
	_finish_structure_change(before, true);
}

TetraImageDocument::LayerMode TetraImageDocument::layer_get_mode(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, LAYER_MODE_NORMAL);
	return layers[index].mode;
}

void TetraImageDocument::layer_set_mode(int p_id, LayerMode p_mode) {
	TETRA_REQUIRE_IDLE();
	const int index = _find_layer(p_id);
	ERR_FAIL_COND(index < 0);
	ERR_FAIL_INDEX(int(p_mode), int(LAYER_MODE_MAX));
	if (layers[index].mode == p_mode) {
		return;
	}
	_undo_open(TTR("Layer Mode"));
	DocState before = _capture_state();
	layers[index].mode = p_mode;
	_finish_structure_change(before, true);
}

bool TetraImageDocument::layer_get_lock_alpha(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, false);
	return layers[index].lock_alpha;
}

void TetraImageDocument::layer_set_lock_alpha(int p_id, bool p_lock) {
	TETRA_REQUIRE_IDLE();
	const int index = _find_layer(p_id);
	ERR_FAIL_COND(index < 0);
	if (layers[index].lock_alpha == p_lock) {
		return;
	}
	_undo_open(TTR("Lock Alpha"));
	DocState before = _capture_state();
	layers[index].lock_alpha = p_lock;
	_finish_structure_change(before, false);
}

Ref<Image> TetraImageDocument::layer_get_image(int p_id) const {
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, Ref<Image>());
	Vector<uint8_t> data;
	data.resize(int64_t(width) * height * 16);
	memcpy(data.ptrw(), layers[index].pixels.ptr(), data.size());
	return Image::create_from_data(width, height, false, Image::FORMAT_RGBAF, data);
}

Error TetraImageDocument::layer_set_image(int p_id, const Ref<Image> &p_image, const Vector2i &p_offset) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	const int index = _find_layer(p_id);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), ERR_INVALID_PARAMETER);
	const Vector<float> source = _image_to_linear(p_image);
	ERR_FAIL_COND_V(source.is_empty(), ERR_INVALID_DATA);
	const int source_width = p_image->get_width();
	const Rect2i placed = _clip_rect(Rect2i(p_offset, p_image->get_size()));

	Vector<float> pixels = _new_buffer();
	float *dst = pixels.ptrw();
	for (int y = placed.position.y; y < placed.position.y + placed.size.y; y++) {
		memcpy(dst + (size_t(y) * width + placed.position.x) * 4,
				source.ptr() + (size_t(y - p_offset.y) * source_width + (placed.position.x - p_offset.x)) * 4,
				sizeof(float) * 4 * placed.size.x);
	}

	// Only the area that really differs is recorded and repainted, so a
	// filter committed through a small selection stays a small undo step.
	const Rect2i changed = _diff_bounds(layers[index].pixels.ptr(), dst);
	if (!changed.has_area()) {
		return OK;
	}
	_undo_open(TTR("Set Layer Pixels"));
	_undo_push_pixels(p_id, changed, layers[index].pixels.ptr());
	layers[index].pixels = pixels;
	_undo_close();
	_mark_dirty(changed);
	return OK;
}

PackedStringArray TetraImageDocument::get_layer_mode_names() {
	PackedStringArray names;
	for (int i = 0; i < LAYER_MODE_MAX; i++) {
		names.push_back(TTR(TetraLayerModes::get_info(i).name));
	}
	return names;
}

Color TetraImageDocument::blend_colors(LayerMode p_mode, const Color &p_backdrop, const Color &p_layer, float p_opacity) {
	const float in[4] = { p_backdrop.r, p_backdrop.g, p_backdrop.b, p_backdrop.a };
	const float layer[4] = { p_layer.r, p_layer.g, p_layer.b, p_layer.a };
	float out[4];
	TetraLayerModes::process(p_mode, TetraLayerModes::get_info(p_mode).layer_composite, in, layer, nullptr, out, p_opacity, 1, 0, 0);
	return Color(out[0], out[1], out[2], out[3]);
}

// Undo.

void TetraImageDocument::_undo_open(const String &p_name) {
	if (group_depth == 0) {
		open_group = UndoGroup();
		open_group.name = p_name;
	}
	group_depth++;
}

void TetraImageDocument::_undo_close() {
	ERR_FAIL_COND(group_depth == 0);
	group_depth--;
	if (group_depth > 0 || open_group.steps.is_empty()) {
		return;
	}
	for (const UndoStep &step : open_group.steps) {
		open_group.bytes += uint64_t(step.data.size()) * sizeof(float);
		const DocState *states[2] = { &step.before, &step.after };
		for (const DocState *state : states) {
			for (const Layer &layer : state->layers) {
				open_group.bytes += uint64_t(layer.pixels.size()) * sizeof(float);
			}
			open_group.bytes += uint64_t(state->selection.size()) * sizeof(float);
		}
	}
	undo_stack.push_back(open_group);
	open_group = UndoGroup();
	redo_stack.clear();
	_trim_undo();
	emit_signal(SNAME("undo_changed"));
}

void TetraImageDocument::_undo_push_pixels(int p_layer_id, const Rect2i &p_rect, const float *p_before_full) {
	UndoStep step;
	step.type = UndoStep::TYPE_PIXELS;
	step.layer_id = p_layer_id;
	step.rect = p_rect;
	step.data.resize(int64_t(p_rect.size.x) * p_rect.size.y * 4);
	float *dst = step.data.ptrw();
	for (int y = 0; y < p_rect.size.y; y++) {
		memcpy(dst + size_t(y) * p_rect.size.x * 4, p_before_full + (size_t(p_rect.position.y + y) * width + p_rect.position.x) * 4, sizeof(float) * 4 * p_rect.size.x);
	}
	open_group.steps.push_back(step);
}

void TetraImageDocument::_undo_push_selection(const Vector<float> &p_before, bool p_before_empty, const Rect2i &p_before_bounds) {
	const Rect2i after_bounds = selection_empty ? Rect2i() : selection_bounds();
	const Rect2i before_bounds = p_before_empty ? Rect2i() : p_before_bounds;
	Rect2i rect = before_bounds;
	if (after_bounds.has_area()) {
		rect = rect.has_area() ? rect.merge(after_bounds) : after_bounds;
	}
	if (!rect.has_area()) {
		return;
	}
	UndoStep step;
	step.type = UndoStep::TYPE_SELECTION;
	step.rect = rect;
	step.data.resize_initialized(int64_t(rect.size.x) * rect.size.y);
	if (!p_before_empty && !p_before.is_empty()) {
		float *dst = step.data.ptrw();
		for (int y = 0; y < rect.size.y; y++) {
			memcpy(dst + size_t(y) * rect.size.x, p_before.ptr() + size_t(rect.position.y + y) * width + rect.position.x, sizeof(float) * rect.size.x);
		}
	}
	open_group.steps.push_back(step);
}

TetraImageDocument::DocState TetraImageDocument::_capture_state() const {
	DocState state;
	state.width = width;
	state.height = height;
	state.active_layer = active_layer;
	state.layers = layers;
	state.has_pixels.resize(layers.size());
	for (bool &flag : state.has_pixels) {
		flag = true;
	}
	state.selection = selection;
	state.selection_empty = selection_empty;
	return state;
}

void TetraImageDocument::_undo_push_state(DocState &p_before) {
	UndoStep step;
	step.type = UndoStep::TYPE_STATE;
	step.after = _capture_state();
	DocState &after = step.after;
	for (uint32_t i = 0; i < p_before.layers.size(); i++) {
		for (uint32_t j = 0; j < after.layers.size(); j++) {
			if (p_before.layers[i].id == after.layers[j].id && p_before.layers[i].pixels.ptr() == after.layers[j].pixels.ptr()) {
				p_before.layers[i].pixels = Vector<float>();
				p_before.has_pixels[i] = false;
				after.layers[j].pixels = Vector<float>();
				after.has_pixels[j] = false;
			}
		}
	}
	if (p_before.selection.ptr() == after.selection.ptr() && p_before.selection_empty == after.selection_empty) {
		p_before.has_selection = false;
		p_before.selection = Vector<float>();
		after.has_selection = false;
		after.selection = Vector<float>();
	}
	step.before = p_before;
	open_group.steps.push_back(step);
}

void TetraImageDocument::_restore_state(const DocState &p_state) {
	LocalVector<Layer> restored(p_state.layers);
	for (uint32_t i = 0; i < restored.size(); i++) {
		if (!p_state.has_pixels[i]) {
			const int current = _find_layer(restored[i].id);
			ERR_CONTINUE(current < 0);
			restored[i].pixels = layers[current].pixels;
		}
	}
	const bool resized = width != p_state.width || height != p_state.height;
	layers = restored;
	width = p_state.width;
	height = p_state.height;
	active_layer = p_state.active_layer;
	if (p_state.has_selection) {
		selection = p_state.selection;
		selection_empty = p_state.selection_empty;
		selection_bounds_valid = false;
		emit_signal(SNAME("selection_changed"));
	}
	if (resized) {
		dirty_rect = Rect2i();
		emit_signal(SNAME("size_changed"));
	}
	emit_signal(SNAME("layers_changed"));
	_mark_dirty(_canvas_rect());
}

void TetraImageDocument::_apply_group(UndoGroup &p_group, bool p_undo) {
	const int count = int(p_group.steps.size());
	for (int n = 0; n < count; n++) {
		UndoStep &step = p_group.steps[p_undo ? count - 1 - n : n];
		const Rect2i &rect = step.rect;
		switch (step.type) {
			case UndoStep::TYPE_PIXELS: {
				const int index = _find_layer(step.layer_id);
				ERR_CONTINUE(index < 0);
				float *layer_px = layers[index].pixels.ptrw();
				float *saved = step.data.ptrw();
				LocalVector<float> row;
				row.resize(rect.size.x * 4);
				const size_t row_bytes = sizeof(float) * 4 * rect.size.x;
				for (int y = 0; y < rect.size.y; y++) {
					float *a = layer_px + (size_t(rect.position.y + y) * width + rect.position.x) * 4;
					float *b = saved + size_t(y) * rect.size.x * 4;
					memcpy(row.ptr(), a, row_bytes);
					memcpy(a, b, row_bytes);
					memcpy(b, row.ptr(), row_bytes);
				}
				_mark_dirty(rect);
			} break;
			case UndoStep::TYPE_SELECTION: {
				if (selection.is_empty()) {
					selection.resize_initialized(int64_t(width) * height);
				}
				float *mask = selection.ptrw();
				float *saved = step.data.ptrw();
				bool any = false;
				for (int y = 0; y < rect.size.y; y++) {
					float *a = mask + size_t(rect.position.y + y) * width + rect.position.x;
					float *b = saved + size_t(y) * rect.size.x;
					for (int x = 0; x < rect.size.x; x++) {
						SWAP(a[x], b[x]);
						any = any || a[x] > 0.0f;
					}
				}
				selection_empty = !any;
				if (selection_empty) {
					selection = Vector<float>();
				}
				selection_bounds_valid = false;
				emit_signal(SNAME("selection_changed"));
			} break;
			case UndoStep::TYPE_STATE: {
				_restore_state(p_undo ? step.before : step.after);
			} break;
		}
	}
}

void TetraImageDocument::_trim_undo() {
	uint64_t total = 0;
	for (const UndoGroup &group : undo_stack) {
		total += group.bytes;
	}
	while (total > undo_memory_limit && undo_stack.size() > 1) {
		total -= undo_stack[0].bytes;
		undo_stack.remove_at(0);
	}
}

void TetraImageDocument::_clear_history() {
	undo_stack.clear();
	redo_stack.clear();
	open_group = UndoGroup();
	group_depth = 0;
	emit_signal(SNAME("undo_changed"));
}

void TetraImageDocument::begin_undo_group(const String &p_name) {
	TETRA_REQUIRE_IDLE();
	_undo_open(p_name);
}

void TetraImageDocument::end_undo_group() {
	ERR_FAIL_COND_MSG(group_depth == 0, "end_undo_group() without begin_undo_group().");
	_undo_close();
}

bool TetraImageDocument::undo() {
	if (stroke.active || group_depth > 0 || undo_stack.is_empty()) {
		return false;
	}
	UndoGroup group = undo_stack[undo_stack.size() - 1];
	undo_stack.remove_at(undo_stack.size() - 1);
	_apply_group(group, true);
	redo_stack.push_back(group);
	emit_signal(SNAME("undo_changed"));
	return true;
}

bool TetraImageDocument::redo() {
	if (stroke.active || group_depth > 0 || redo_stack.is_empty()) {
		return false;
	}
	UndoGroup group = redo_stack[redo_stack.size() - 1];
	redo_stack.remove_at(redo_stack.size() - 1);
	_apply_group(group, false);
	undo_stack.push_back(group);
	emit_signal(SNAME("undo_changed"));
	return true;
}

PackedStringArray TetraImageDocument::get_undo_names() const {
	PackedStringArray names;
	for (const UndoGroup &group : undo_stack) {
		names.push_back(group.name);
	}
	return names;
}

PackedStringArray TetraImageDocument::get_redo_names() const {
	PackedStringArray names;
	for (const UndoGroup &group : redo_stack) {
		names.push_back(group.name);
	}
	return names;
}

void TetraImageDocument::clear_undo_history() {
	TETRA_REQUIRE_IDLE();
	_clear_history();
}

void TetraImageDocument::set_undo_memory_limit(int64_t p_bytes) {
	undo_memory_limit = uint64_t(MAX(p_bytes, int64_t(0)));
	_trim_undo();
	emit_signal(SNAME("undo_changed"));
}

int64_t TetraImageDocument::get_undo_memory_usage() const {
	uint64_t total = 0;
	for (const UndoGroup &group : undo_stack) {
		total += group.bytes;
	}
	for (const UndoGroup &group : redo_stack) {
		total += group.bytes;
	}
	return int64_t(total);
}

void TetraImageDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "width", "height", "fill"), &TetraImageDocument::create, DEFVAL(Color(1, 1, 1, 1)));
	ClassDB::bind_method(D_METHOD("get_size"), &TetraImageDocument::get_size);
	ClassDB::bind_method(D_METHOD("get_width"), &TetraImageDocument::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &TetraImageDocument::get_height);

	ClassDB::bind_method(D_METHOD("add_layer", "name", "index"), &TetraImageDocument::add_layer, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_layer", "id"), &TetraImageDocument::remove_layer);
	ClassDB::bind_method(D_METHOD("duplicate_layer", "id"), &TetraImageDocument::duplicate_layer);
	ClassDB::bind_method(D_METHOD("move_layer", "id", "new_index"), &TetraImageDocument::move_layer);
	ClassDB::bind_method(D_METHOD("merge_down", "id"), &TetraImageDocument::merge_down);
	ClassDB::bind_method(D_METHOD("flatten"), &TetraImageDocument::flatten);
	ClassDB::bind_method(D_METHOD("get_layer_ids"), &TetraImageDocument::get_layer_ids);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &TetraImageDocument::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer_index", "id"), &TetraImageDocument::get_layer_index);
	ClassDB::bind_method(D_METHOD("get_active_layer"), &TetraImageDocument::get_active_layer);
	ClassDB::bind_method(D_METHOD("set_active_layer", "id"), &TetraImageDocument::set_active_layer);

	ClassDB::bind_method(D_METHOD("layer_get_name", "id"), &TetraImageDocument::layer_get_name);
	ClassDB::bind_method(D_METHOD("layer_set_name", "id", "name"), &TetraImageDocument::layer_set_name);
	ClassDB::bind_method(D_METHOD("layer_is_visible", "id"), &TetraImageDocument::layer_is_visible);
	ClassDB::bind_method(D_METHOD("layer_set_visible", "id", "visible"), &TetraImageDocument::layer_set_visible);
	ClassDB::bind_method(D_METHOD("layer_get_opacity", "id"), &TetraImageDocument::layer_get_opacity);
	ClassDB::bind_method(D_METHOD("layer_set_opacity", "id", "opacity"), &TetraImageDocument::layer_set_opacity);
	ClassDB::bind_method(D_METHOD("layer_get_mode", "id"), &TetraImageDocument::layer_get_mode);
	ClassDB::bind_method(D_METHOD("layer_set_mode", "id", "mode"), &TetraImageDocument::layer_set_mode);
	ClassDB::bind_method(D_METHOD("layer_get_lock_alpha", "id"), &TetraImageDocument::layer_get_lock_alpha);
	ClassDB::bind_method(D_METHOD("layer_set_lock_alpha", "id", "lock"), &TetraImageDocument::layer_set_lock_alpha);
	ClassDB::bind_method(D_METHOD("layer_get_image", "id"), &TetraImageDocument::layer_get_image);
	ClassDB::bind_method(D_METHOD("layer_set_image", "id", "image", "offset"), &TetraImageDocument::layer_set_image, DEFVAL(Vector2i()));

	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("get_layer_mode_names"), &TetraImageDocument::get_layer_mode_names);
	ClassDB::bind_static_method("TetraImageDocument", D_METHOD("blend_colors", "mode", "backdrop", "layer", "opacity"), &TetraImageDocument::blend_colors, DEFVAL(1.0f));

	ClassDB::bind_method(D_METHOD("composite", "rect"), &TetraImageDocument::composite, DEFVAL(Rect2i()));
	ClassDB::bind_method(D_METHOD("composite_float", "rect"), &TetraImageDocument::composite_float, DEFVAL(Rect2i()));
	ClassDB::bind_method(D_METHOD("get_and_clear_dirty_rect"), &TetraImageDocument::get_and_clear_dirty_rect);

	ClassDB::bind_method(D_METHOD("begin_undo_group", "name"), &TetraImageDocument::begin_undo_group);
	ClassDB::bind_method(D_METHOD("end_undo_group"), &TetraImageDocument::end_undo_group);
	ClassDB::bind_method(D_METHOD("undo"), &TetraImageDocument::undo);
	ClassDB::bind_method(D_METHOD("redo"), &TetraImageDocument::redo);
	ClassDB::bind_method(D_METHOD("can_undo"), &TetraImageDocument::can_undo);
	ClassDB::bind_method(D_METHOD("can_redo"), &TetraImageDocument::can_redo);
	ClassDB::bind_method(D_METHOD("get_undo_names"), &TetraImageDocument::get_undo_names);
	ClassDB::bind_method(D_METHOD("get_redo_names"), &TetraImageDocument::get_redo_names);
	ClassDB::bind_method(D_METHOD("clear_undo_history"), &TetraImageDocument::clear_undo_history);
	ClassDB::bind_method(D_METHOD("set_undo_memory_limit", "bytes"), &TetraImageDocument::set_undo_memory_limit);
	ClassDB::bind_method(D_METHOD("get_undo_memory_limit"), &TetraImageDocument::get_undo_memory_limit);
	ClassDB::bind_method(D_METHOD("get_undo_memory_usage"), &TetraImageDocument::get_undo_memory_usage);

	ADD_SIGNAL(MethodInfo("changed", PropertyInfo(Variant::RECT2I, "rect")));
	ADD_SIGNAL(MethodInfo("layers_changed"));
	ADD_SIGNAL(MethodInfo("selection_changed"));
	ADD_SIGNAL(MethodInfo("size_changed"));
	ADD_SIGNAL(MethodInfo("undo_changed"));

	BIND_ENUM_CONSTANT(LAYER_MODE_NORMAL);
	BIND_ENUM_CONSTANT(LAYER_MODE_DISSOLVE);
	BIND_ENUM_CONSTANT(LAYER_MODE_BEHIND);
	BIND_ENUM_CONSTANT(LAYER_MODE_COLOR_ERASE);
	BIND_ENUM_CONSTANT(LAYER_MODE_ERASE);
	BIND_ENUM_CONSTANT(LAYER_MODE_MERGE);
	BIND_ENUM_CONSTANT(LAYER_MODE_SPLIT);
	BIND_ENUM_CONSTANT(LAYER_MODE_LIGHTEN_ONLY);
	BIND_ENUM_CONSTANT(LAYER_MODE_LUMA_LIGHTEN_ONLY);
	BIND_ENUM_CONSTANT(LAYER_MODE_SCREEN);
	BIND_ENUM_CONSTANT(LAYER_MODE_DODGE);
	BIND_ENUM_CONSTANT(LAYER_MODE_ADDITION);
	BIND_ENUM_CONSTANT(LAYER_MODE_DARKEN_ONLY);
	BIND_ENUM_CONSTANT(LAYER_MODE_LUMA_DARKEN_ONLY);
	BIND_ENUM_CONSTANT(LAYER_MODE_MULTIPLY);
	BIND_ENUM_CONSTANT(LAYER_MODE_BURN);
	BIND_ENUM_CONSTANT(LAYER_MODE_LINEAR_BURN);
	BIND_ENUM_CONSTANT(LAYER_MODE_OVERLAY);
	BIND_ENUM_CONSTANT(LAYER_MODE_SOFT_LIGHT);
	BIND_ENUM_CONSTANT(LAYER_MODE_HARD_LIGHT);
	BIND_ENUM_CONSTANT(LAYER_MODE_VIVID_LIGHT);
	BIND_ENUM_CONSTANT(LAYER_MODE_PIN_LIGHT);
	BIND_ENUM_CONSTANT(LAYER_MODE_LINEAR_LIGHT);
	BIND_ENUM_CONSTANT(LAYER_MODE_HARD_MIX);
	BIND_ENUM_CONSTANT(LAYER_MODE_DIFFERENCE);
	BIND_ENUM_CONSTANT(LAYER_MODE_EXCLUSION);
	BIND_ENUM_CONSTANT(LAYER_MODE_SUBTRACT);
	BIND_ENUM_CONSTANT(LAYER_MODE_GRAIN_EXTRACT);
	BIND_ENUM_CONSTANT(LAYER_MODE_GRAIN_MERGE);
	BIND_ENUM_CONSTANT(LAYER_MODE_DIVIDE);
	BIND_ENUM_CONSTANT(LAYER_MODE_HSV_HUE);
	BIND_ENUM_CONSTANT(LAYER_MODE_HSV_SATURATION);
	BIND_ENUM_CONSTANT(LAYER_MODE_HSL_COLOR);
	BIND_ENUM_CONSTANT(LAYER_MODE_HSV_VALUE);
	BIND_ENUM_CONSTANT(LAYER_MODE_LCH_HUE);
	BIND_ENUM_CONSTANT(LAYER_MODE_LCH_CHROMA);
	BIND_ENUM_CONSTANT(LAYER_MODE_LCH_COLOR);
	BIND_ENUM_CONSTANT(LAYER_MODE_LCH_LIGHTNESS);
	BIND_ENUM_CONSTANT(LAYER_MODE_LUMINANCE);
	BIND_ENUM_CONSTANT(LAYER_MODE_MAX);

	_bind_selection_methods();
	_bind_paint_methods();
	_bind_edit_methods();
}
