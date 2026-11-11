#include "tetra_image_canvas.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "scene/main/timer.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"

// Black and white dashes that depend only on the screen position, so one
// uniform moves the whole outline without rebuilding its geometry.
static const char *OUTLINE_SHADER = R"(
shader_type canvas_item;
render_mode unshaded;
uniform float phase = 0.0;
void fragment() {
	float dash = mod(floor(FRAGCOORD.x) + floor(FRAGCOORD.y) + phase, 8.0);
	COLOR = dash < 4.0 ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(1.0, 1.0, 1.0, 1.0);
}
)";

void TetraImageCanvas::_message(const String &p_text) {
	emit_signal(SNAME("status_message"), p_text);
}

void TetraImageCanvas::set_document(const Ref<TetraImageDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (drag != DRAG_NONE) {
		tool_cancel();
	}
	if (document.is_valid()) {
		document->disconnect(SNAME("changed"), callable_mp(this, &TetraImageCanvas::_document_changed));
		document->disconnect(SNAME("size_changed"), callable_mp(this, &TetraImageCanvas::_document_resized));
		document->disconnect(SNAME("selection_changed"), callable_mp(this, &TetraImageCanvas::_selection_changed));
	}
	document = p_document;
	has_last_stroke_point = false;
	has_clone_source = false;
	has_clone_offset = false;
	if (document.is_valid()) {
		document->connect(SNAME("changed"), callable_mp(this, &TetraImageCanvas::_document_changed));
		document->connect(SNAME("size_changed"), callable_mp(this, &TetraImageCanvas::_document_resized));
		document->connect(SNAME("selection_changed"), callable_mp(this, &TetraImageCanvas::_selection_changed));
	}
	_rebuild_tiles();
	_update_outline();
	if (document.is_valid()) {
		zoom_to_fit();
	}
}

Ref<TetraImageDocument> TetraImageCanvas::get_document() const {
	return document;
}

void TetraImageCanvas::_rebuild_tiles() {
	tiles.clear();
	tile_count = Size2i();
	pending_dirty = Rect2i();
	if (document.is_null() || document->get_width() <= 0 || document->get_height() <= 0) {
		set_content_size(Size2(1, 1));
		queue_redraw();
		return;
	}
	const Size2i size = document->get_size();
	set_content_size(size);
	tile_count = Size2i((size.x + TILE_SIZE - 1) / TILE_SIZE, (size.y + TILE_SIZE - 1) / TILE_SIZE);
	tiles.resize(tile_count.x * tile_count.y);
	const Ref<Image> whole = document->composite();
	for (int ty = 0; ty < tile_count.y; ty++) {
		for (int tx = 0; tx < tile_count.x; tx++) {
			const Rect2i rect = Rect2i(tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE).intersection(Rect2i(Point2i(), size));
			Tile &tile = tiles[ty * tile_count.x + tx];
			tile.image = whole->get_region(rect);
			tile.texture = ImageTexture::create_from_image(tile.image);
			tile_update_count++;
		}
	}
	queue_redraw();
}

void TetraImageCanvas::_document_changed(const Rect2i &p_rect) {
	pending_dirty = pending_dirty.has_area() ? pending_dirty.merge(p_rect) : p_rect;
	if (!refresh_queued) {
		refresh_queued = true;
		callable_mp(this, &TetraImageCanvas::_refresh_tiles).call_deferred();
	}
}

void TetraImageCanvas::_refresh_tiles() {
	refresh_queued = false;
	if (document.is_null()) {
		return;
	}
	const Rect2i canvas(Point2i(), document->get_size());
	if (Size2i(get_content_size()) != canvas.size || tiles.is_empty()) {
		_rebuild_tiles();
		return;
	}
	const Rect2i dirty = pending_dirty.intersection(canvas);
	pending_dirty = Rect2i();
	if (!dirty.has_area()) {
		return;
	}
	// One composite of the changed rectangle, copied into the tiles it
	// touches; textures cannot be updated in part, so those tiles are uploaded whole.
	const Ref<Image> part = document->composite(dirty);
	ERR_FAIL_COND(part.is_null());
	const int first_x = dirty.position.x / TILE_SIZE;
	const int first_y = dirty.position.y / TILE_SIZE;
	const int last_x = (dirty.get_end().x - 1) / TILE_SIZE;
	const int last_y = (dirty.get_end().y - 1) / TILE_SIZE;
	for (int ty = first_y; ty <= last_y; ty++) {
		for (int tx = first_x; tx <= last_x; tx++) {
			Tile &tile = tiles[ty * tile_count.x + tx];
			const Rect2i tile_rect(tx * TILE_SIZE, ty * TILE_SIZE, tile.image->get_width(), tile.image->get_height());
			const Rect2i overlap = tile_rect.intersection(dirty);
			if (!overlap.has_area()) {
				continue;
			}
			tile.image->blit_rect(part, Rect2i(overlap.position - dirty.position, overlap.size), overlap.position - tile_rect.position);
			tile.texture->update(tile.image);
			tile_update_count++;
		}
	}
	queue_redraw();
}

void TetraImageCanvas::_document_resized() {
	if (drag != DRAG_NONE && drag != DRAG_MOVE) {
		drag = DRAG_NONE;
	}
	has_clone_source = false;
	has_clone_offset = false;
	has_last_stroke_point = false;
	_rebuild_tiles();
	zoom_to_fit();
}

void TetraImageCanvas::_selection_changed() {
	if (!outline_update_queued) {
		outline_update_queued = true;
		callable_mp(this, &TetraImageCanvas::_update_outline).call_deferred();
	}
}

void TetraImageCanvas::_update_outline() {
	outline_update_queued = false;
	outline_points.clear();
	if (document.is_valid() && !document->is_selection_empty()) {
		// The outline runs along the pixel edges where the mask crosses one half.
		const Ref<Image> mask = document->get_selection_mask();
		const Vector<uint8_t> mask_data = mask->get_data();
		const float *values = reinterpret_cast<const float *>(mask_data.ptr());
		const int w = mask->get_width();
		const int h = mask->get_height();
		const Rect2i bounds = document->selection_bounds().intersection(Rect2i(0, 0, w, h));
		const int x0 = bounds.position.x;
		const int x1 = bounds.get_end().x;
		const int y0 = bounds.position.y;
		const int y1 = bounds.get_end().y;

		// Horizontal edges between row y - 1 and row y, merged into runs.
		for (int y = y0; y <= y1 && outline_points.size() < MAX_OUTLINE_POINTS; y++) {
			const float *above = y > 0 ? values + size_t(y - 1) * w : nullptr;
			const float *below = y < h ? values + size_t(y) * w : nullptr;
			int run_start = -1;
			for (int x = x0; x <= x1; x++) {
				const bool edge = x < x1 && ((above && above[x] >= 0.5f) != (below && below[x] >= 0.5f));
				if (edge && run_start < 0) {
					run_start = x;
				} else if (!edge && run_start >= 0) {
					outline_points.push_back(Point2(run_start, y));
					outline_points.push_back(Point2(x, y));
					run_start = -1;
				}
			}
		}
		// Vertical edges between column x - 1 and column x, merged into runs down each column.
		LocalVector<int> run_starts;
		run_starts.resize(x1 - x0 + 1);
		for (int &start : run_starts) {
			start = -1;
		}
		for (int y = y0; y <= y1 && outline_points.size() < MAX_OUTLINE_POINTS; y++) {
			const float *row = y < y1 ? values + size_t(y) * w : nullptr;
			for (int x = x0; x <= x1; x++) {
				const bool edge = row && ((x > 0 && row[x - 1] >= 0.5f) != (x < w && row[x] >= 0.5f));
				int &start = run_starts[x - x0];
				if (edge && start < 0) {
					start = y;
				} else if (!edge && start >= 0) {
					outline_points.push_back(Point2(x, start));
					outline_points.push_back(Point2(x, y));
					start = -1;
				}
			}
		}
	}
	outline->queue_redraw();
	if (outline_points.is_empty()) {
		outline_timer->stop();
	} else if (outline_timer->is_stopped() && is_visible_in_tree()) {
		outline_timer->start();
	}
}

void TetraImageCanvas::_outline_tick() {
	outline_phase = (outline_phase + 7) % 8;
	outline_material->set_shader_parameter(SNAME("phase"), float(outline_phase));
}

void TetraImageCanvas::_draw_outline() {
	if (!outline_points.is_empty()) {
		outline->draw_multiline(outline_points, Color(1, 1, 1));
	}
}

void TetraImageCanvas::_view_changed() {
	// The outline is drawn once in document units and follows the view through its transform.
	outline->set_position(content_to_view(Point2()));
	outline->set_scale(Size2(get_zoom(), get_zoom()));
	set_texture_filter(get_zoom() > 1.0 ? TEXTURE_FILTER_NEAREST : TEXTURE_FILTER_LINEAR);
	overlay->queue_redraw();
}

void TetraImageCanvas::_canvas_draw() {
	draw_rect(Rect2(Point2(), get_size()), get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor)));
	if (document.is_null() || tiles.is_empty()) {
		return;
	}
	const Size2 size = get_content_size();
	draw_checkerboard(Rect2(Point2(), size));

	const Rect2 shown = get_visible_content_rect();
	if (shown.has_area()) {
		draw_set_transform_to_content();
		const int first_x = CLAMP(int(shown.position.x) / TILE_SIZE, 0, tile_count.x - 1);
		const int first_y = CLAMP(int(shown.position.y) / TILE_SIZE, 0, tile_count.y - 1);
		const int last_x = CLAMP(int(Math::ceil(shown.get_end().x)) / TILE_SIZE, 0, tile_count.x - 1);
		const int last_y = CLAMP(int(Math::ceil(shown.get_end().y)) / TILE_SIZE, 0, tile_count.y - 1);
		for (int ty = first_y; ty <= last_y; ty++) {
			for (int tx = first_x; tx <= last_x; tx++) {
				const Tile &tile = tiles[ty * tile_count.x + tx];
				draw_texture_rect(tile.texture, Rect2(Point2(tx * TILE_SIZE, ty * TILE_SIZE), tile.image->get_size()));
			}
		}
		draw_reset_transform();
	}

	const Rect2 border(content_to_view(Point2()), size * get_zoom());
	draw_rect(border.grow(0.5), Color(0, 0, 0, 0.7), false, 1.0);
}

// Closed outline of the ellipse inscribed in p_rect, as a polyline.
static Vector<Point2> ellipse_points(const Rect2 &p_rect) {
	Vector<Point2> points;
	const int count = CLAMP(int((p_rect.size.x + p_rect.size.y) / 4.0), 24, 192) & ~1;
	const Point2 center = p_rect.get_center();
	for (int i = 0; i <= count; i++) {
		const double angle = Math::TAU * i / count;
		points.push_back(center + Vector2(Math::cos(angle), Math::sin(angle)) * p_rect.size * 0.5);
	}
	return points;
}

void TetraImageCanvas::_draw_overlay() {
	if (document.is_null()) {
		return;
	}
	const Color white(1, 1, 1, 0.95);
	const Color black(0, 0, 0, 0.95);

	if (drag == DRAG_SELECT) {
		const Rect2 rect = Rect2(content_to_view(drag_start.round()), Size2()).expand(content_to_view(drag_current.round()));
		if (tool == TetraImageTools::TOOL_ELLIPSE_SELECT) {
			const Vector<Point2> points = ellipse_points(rect);
			overlay->draw_polyline(points, white, 1.0);
			Vector<Point2> dashes;
			for (int i = 0; i + 1 < points.size(); i += 2) {
				dashes.push_back(points[i]);
				dashes.push_back(points[i + 1]);
			}
			overlay->draw_multiline(dashes, black, 1.0);
		} else {
			overlay->draw_rect(rect, white, false, 1.0);
			const Point2 corners[5] = { rect.position, Point2(rect.get_end().x, rect.position.y), rect.get_end(), Point2(rect.position.x, rect.get_end().y), rect.position };
			for (int i = 0; i < 4; i++) {
				overlay->draw_dashed_line(corners[i], corners[i + 1], black, 1.0, 4.0);
			}
		}
	}

	if (drag == DRAG_GRADIENT) {
		const Point2 from = content_to_view(drag_start);
		const Point2 to = content_to_view(drag_current);
		overlay->draw_line(from, to, black, 3.0, true);
		overlay->draw_line(from, to, white, 1.0, true);
		for (const Point2 &end : { from, to }) {
			overlay->draw_rect(Rect2(end - Vector2(4, 4), Size2(8, 8)), black);
			overlay->draw_rect(Rect2(end - Vector2(3, 3), Size2(6, 6)), white);
		}
	}

	if (tool == TetraImageTools::TOOL_CLONE && has_clone_source) {
		// While a stroke runs the marker follows the point being copied from.
		Vector2 source = clone_source;
		if (drag == DRAG_STROKE && has_clone_offset && bool(tool_options.get("aligned", false))) {
			source = last_stroke_point + clone_offset;
		} else if (drag == DRAG_STROKE && !bool(tool_options.get("aligned", false))) {
			source = clone_source + (last_stroke_point - drag_start);
		}
		const Point2 at = content_to_view(source);
		for (int pass = 0; pass < 2; pass++) {
			const Color &color = pass == 0 ? black : white;
			const real_t width = pass == 0 ? 3.0 : 1.0;
			overlay->draw_line(at - Vector2(8, 0), at + Vector2(8, 0), color, width);
			overlay->draw_line(at - Vector2(0, 8), at + Vector2(0, 8), color, width);
		}
	}

	if (pointer_inside && !input_locked && !is_panning() && TetraImageTools::is_paint_tool(tool)) {
		const real_t radius = MAX(real_t(tool_options.get("size", 20.0)) * get_zoom() * 0.5, 0.5);
		if (radius < 3.0) {
			overlay->draw_line(pointer_view - Vector2(6, 0), pointer_view + Vector2(6, 0), white, 1.0);
			overlay->draw_line(pointer_view - Vector2(0, 6), pointer_view + Vector2(0, 6), white, 1.0);
		} else {
			const int count = CLAMP(int(radius * 1.5), 24, 256);
			overlay->draw_arc(pointer_view, radius + 1.0, 0.0, Math::TAU, count, black, 1.0, true);
			overlay->draw_arc(pointer_view, radius, 0.0, Math::TAU, count, white, 1.0, true);
		}
	}
}

void TetraImageCanvas::set_tool(TetraImageTools::Tool p_tool) {
	ERR_FAIL_INDEX(int(p_tool), int(TetraImageTools::TOOL_MAX));
	if (drag != DRAG_NONE) {
		tool_release();
	}
	tool = p_tool;
	set_default_cursor_shape(tool == TetraImageTools::TOOL_MOVE ? CURSOR_MOVE : (tool == TetraImageTools::TOOL_ZOOM ? CURSOR_ARROW : CURSOR_CROSS));
	overlay->queue_redraw();
}

void TetraImageCanvas::set_tool_options(const Dictionary &p_options) {
	tool_options = p_options;
	overlay->queue_redraw();
}

void TetraImageCanvas::set_colors(const Color &p_foreground, const Color &p_background) {
	foreground = p_foreground;
	background = p_background;
}

void TetraImageCanvas::set_input_locked(bool p_locked) {
	if (p_locked && drag != DRAG_NONE) {
		tool_release();
	}
	input_locked = p_locked;
	overlay->queue_redraw();
}

void TetraImageCanvas::set_clone_source(const Vector2 &p_position) {
	clone_source = p_position;
	has_clone_source = true;
	has_clone_offset = false;
	overlay->queue_redraw();
}

bool TetraImageCanvas::_layer_editable(int p_layer_id) {
	if (document->get_layer_index(p_layer_id) < 0) {
		_message(TTR("There is no active layer."));
		return false;
	}
	if (!document->layer_is_visible(p_layer_id)) {
		_message(TTR("The active layer is not visible."));
		return false;
	}
	return true;
}

Dictionary TetraImageCanvas::_stroke_settings(const Vector2 &p_position) {
	Dictionary settings;
	settings["color"] = foreground.srgb_to_linear();
	settings["size"] = tool_options.get("size", 20.0);
	settings["opacity"] = double(tool_options.get("opacity", 100.0)) / 100.0;
	settings["flow"] = double(tool_options.get("flow", 100.0)) / 100.0;
	settings["spacing"] = tool_options.get("spacing", 10.0);
	settings["pressure_size"] = tool_options.get("pressure_size", false);
	settings["pressure_opacity"] = tool_options.get("pressure_opacity", false);
	if (tool_options.has("hardness")) {
		settings["hardness"] = double(tool_options["hardness"]) / 100.0;
	}
	if (tool_options.has("mode")) {
		settings["mode"] = int(tool_options["mode"]);
	}
	if (tool_options.has("rate")) {
		settings["rate"] = double(tool_options["rate"]) / 100.0;
	}
	if (tool == TetraImageTools::TOOL_CLONE) {
		if (bool(tool_options.get("aligned", false))) {
			if (!has_clone_offset) {
				clone_offset = clone_source - p_position;
				has_clone_offset = true;
			}
			settings["source_offset"] = clone_offset;
		} else {
			settings["source_point"] = clone_source;
		}
	}
	return settings;
}

TetraImageDocument::SelectOp TetraImageCanvas::_select_op() const {
	if (drag_shift && drag_ctrl) {
		return TetraImageDocument::SELECT_INTERSECT;
	} else if (drag_shift) {
		return TetraImageDocument::SELECT_ADD;
	} else if (drag_ctrl) {
		return TetraImageDocument::SELECT_SUBTRACT;
	}
	return TetraImageDocument::SelectOp(CLAMP(int(tool_options.get("mode", 0)), 0, 3));
}

void TetraImageCanvas::_feather_and_close(const String &p_name) {
	const float feather = tool_options.get("feather", 0.0);
	if (feather > 0.0f && !document->is_selection_empty()) {
		document->feather_selection(feather);
	}
	document->end_undo_group();
	_message(document->is_selection_empty() ? TTR("Nothing is selected.") : vformat(TTR("%s: %d × %d px"), p_name, document->selection_bounds().size.x, document->selection_bounds().size.y));
}

void TetraImageCanvas::_pick(const Vector2 &p_position) {
	const Point2i point = p_position.floor();
	if (!Rect2i(Point2i(), document->get_size()).has_point(point)) {
		return;
	}
	const Color picked = document->pick_color(point, tool_options.get("sample_merged", true), tool_options.get("radius", 0));
	if (picked.a <= 0.0f) {
		_message(TTR("The pixel under the pointer is transparent."));
		return;
	}
	Color color = picked.linear_to_srgb();
	color.a = 1.0f;
	const bool to_background = (String(tool_options.get("target", "fg")) == "bg") != drag_ctrl;
	emit_signal(SNAME("color_picked"), color, to_background);
}

void TetraImageCanvas::tool_press(const Vector2 &p_position, float p_pressure, bool p_shift, bool p_ctrl) {
	_begin(p_position, p_pressure, p_shift, p_ctrl, false);
}

void TetraImageCanvas::_begin(const Vector2 &p_position, float p_pressure, bool p_shift, bool p_ctrl, bool p_defer_first_point) {
	if (document.is_null() || input_locked || drag != DRAG_NONE || document->is_stroke_active() || document->is_undo_group_open()) {
		return;
	}
	drag_start = p_position;
	drag_current = p_position;
	drag_shift = p_shift;
	drag_ctrl = p_ctrl;
	drag_layer = document->get_active_layer();
	const Point2i pixel = p_position.floor();
	const bool on_canvas = Rect2i(Point2i(), document->get_size()).has_point(pixel);

	if (TetraImageTools::is_paint_tool(tool)) {
		if (tool == TetraImageTools::TOOL_CLONE && p_ctrl) {
			set_clone_source(p_position);
			_message(TTR("Clone source set. Click and drag to paint from it."));
			return;
		}
		if (tool == TetraImageTools::TOOL_CLONE && !has_clone_source) {
			_message(TTR("Ctrl+click first to set the clone source."));
			return;
		}
		if (!_layer_editable(drag_layer)) {
			return;
		}
		// The other end of a pen is its eraser.
		const TetraImageDocument::PaintTool paint_tool = pen_inverted ? TetraImageDocument::TOOL_ERASER : TetraImageDocument::PaintTool(tool);
		if (document->stroke_begin(drag_layer, paint_tool, _stroke_settings(p_position)) != OK) {
			return;
		}
		drag = DRAG_STROKE;
		last_pressure = p_pressure;
		first_point_pending = false;
		if (p_shift && has_last_stroke_point) {
			document->stroke_add_point(last_stroke_point, p_pressure);
			document->stroke_add_point(p_position, p_pressure);
		} else if (p_defer_first_point) {
			// A pen reports its pressure with the first motion; a dab at full
			// pressure now would leave a blob at the start of the stroke.
			first_point_pending = true;
		} else {
			document->stroke_add_point(p_position, p_pressure);
		}
		last_stroke_point = p_position;
		has_last_stroke_point = true;
		if (tool == TetraImageTools::TOOL_AIRBRUSH) {
			airbrush_timer->start();
		}
		overlay->queue_redraw();
		return;
	}

	switch (tool) {
		case TetraImageTools::TOOL_BUCKET_FILL: {
			if (on_canvas && _layer_editable(drag_layer)) {
				const Color color = (String(tool_options.get("fill", "fg")) == "bg" ? background : foreground).srgb_to_linear();
				document->bucket_fill(drag_layer, pixel, color, double(tool_options.get("threshold", 15.0)) / 255.0, tool_options.get("contiguous", true), tool_options.get("sample_merged", false));
			}
		} break;
		case TetraImageTools::TOOL_GRADIENT: {
			if (_layer_editable(drag_layer)) {
				drag = DRAG_GRADIENT;
			}
		} break;
		case TetraImageTools::TOOL_COLOR_PICKER: {
			drag = DRAG_PICK;
			_pick(p_position);
		} break;
		case TetraImageTools::TOOL_RECT_SELECT:
		case TetraImageTools::TOOL_ELLIPSE_SELECT: {
			drag = DRAG_SELECT;
		} break;
		case TetraImageTools::TOOL_FUZZY_SELECT: {
			if (on_canvas) {
				document->begin_undo_group(TTR("Fuzzy Select"));
				document->select_fuzzy(pixel, double(tool_options.get("threshold", 15.0)) / 255.0, tool_options.get("sample_merged", false), _select_op());
				_feather_and_close(TTR("Fuzzy Select"));
			}
		} break;
		case TetraImageTools::TOOL_MOVE: {
			if (_layer_editable(drag_layer)) {
				drag = DRAG_MOVE;
				move_offset = Vector2i();
				move_group_open = false;
			}
		} break;
		case TetraImageTools::TOOL_ZOOM: {
			const bool zoom_out = (String(tool_options.get("direction", "in")) == "out") != p_ctrl;
			set_zoom(get_zoom() * (zoom_out ? 0.5 : 2.0), content_to_view(p_position));
		} break;
		default:
			break;
	}
	overlay->queue_redraw();
}

void TetraImageCanvas::tool_drag(const Vector2 &p_position, float p_pressure) {
	if (document.is_null() || drag == DRAG_NONE) {
		return;
	}
	drag_current = p_position;
	switch (drag) {
		case DRAG_STROKE: {
			if (first_point_pending) {
				document->stroke_add_point(drag_start, p_pressure);
				first_point_pending = false;
			}
			document->stroke_add_point(p_position, p_pressure);
			last_stroke_point = p_position;
			last_pressure = p_pressure;
		} break;
		case DRAG_PICK: {
			_pick(p_position);
		} break;
		case DRAG_MOVE: {
			// The layer is shown where it would land: the move so far is taken
			// back and redone from the original pixels, so nothing is lost at
			// the canvas edge while dragging back and forth.
			const Vector2i offset = Vector2i((p_position - drag_start).round());
			if (offset != move_offset) {
				if (move_group_open) {
					document->cancel_undo_group();
					move_group_open = false;
				}
				if (offset != Vector2i()) {
					document->begin_undo_group(TTR("Move Layer"));
					document->translate_layer(drag_layer, offset);
					move_group_open = true;
				}
				move_offset = offset;
				_message(vformat(TTR("Move: %d, %d px"), offset.x, offset.y));
			}
		} break;
		default:
			break;
	}
	overlay->queue_redraw();
}

void TetraImageCanvas::tool_release() {
	if (document.is_null() || drag == DRAG_NONE) {
		return;
	}
	const DragMode finished = drag;
	drag = DRAG_NONE;
	switch (finished) {
		case DRAG_STROKE: {
			airbrush_timer->stop();
			if (first_point_pending) {
				document->stroke_add_point(drag_start, 1.0);
				first_point_pending = false;
			}
			document->stroke_end();
		} break;
		case DRAG_GRADIENT: {
			if (drag_start.distance_to(drag_current) >= 1.0) {
				const float opacity = double(tool_options.get("opacity", 100.0)) / 100.0;
				Color from = foreground.srgb_to_linear();
				Color to = background.srgb_to_linear();
				from.a = opacity;
				to.a = opacity;
				if (bool(tool_options.get("reverse", false))) {
					SWAP(from, to);
				}
				document->gradient_fill(drag_layer, drag_start, drag_current, from, to, TetraImageDocument::GradientType(int(tool_options.get("type", 0))), TetraImageDocument::GradientRepeat(int(tool_options.get("repeat", 0))));
			}
		} break;
		case DRAG_SELECT: {
			const Point2i from = drag_start.round();
			const Point2i to = drag_current.round();
			const Rect2i rect = Rect2i(from, Size2i()).expand(to);
			const TetraImageDocument::SelectOp op = _select_op();
			const bool ellipse = tool == TetraImageTools::TOOL_ELLIPSE_SELECT;
			const String name = ellipse ? TTR("Ellipse Select") : TTR("Rectangle Select");
			if (rect.has_area()) {
				document->begin_undo_group(name);
				if (ellipse) {
					document->select_ellipse(rect, op, tool_options.get("antialias", true));
				} else {
					document->select_rect(rect, op);
				}
				_feather_and_close(name);
			} else if (op == TetraImageDocument::SELECT_REPLACE && !document->is_selection_empty()) {
				// A click without a drag removes the selection, as in GIMP.
				document->select_none();
				_message(TTR("Nothing is selected."));
			}
		} break;
		case DRAG_MOVE: {
			if (move_group_open) {
				document->end_undo_group();
				move_group_open = false;
			}
		} break;
		default:
			break;
	}
	overlay->queue_redraw();
}

void TetraImageCanvas::tool_cancel() {
	if (document.is_null() || drag == DRAG_NONE) {
		return;
	}
	if (drag == DRAG_STROKE) {
		airbrush_timer->stop();
		first_point_pending = false;
		document->stroke_cancel();
	} else if (drag == DRAG_MOVE && move_group_open) {
		document->cancel_undo_group();
		move_group_open = false;
	}
	drag = DRAG_NONE;
	overlay->queue_redraw();
}

void TetraImageCanvas::_airbrush_tick() {
	if (drag == DRAG_STROKE && document.is_valid() && document->is_stroke_active() && !first_point_pending) {
		document->stroke_add_point(last_stroke_point, last_pressure);
	}
}

bool TetraImageCanvas::_canvas_gui_input(const Ref<InputEvent> &p_event) {
	if (document.is_null()) {
		return false;
	}

	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid()) {
		if (button->get_button_index() == MouseButton::LEFT) {
			if (button->is_pressed()) {
				_begin(view_to_content(button->get_position()), 1.0, button->is_shift_pressed(), button->is_command_or_control_pressed(), pen_detected);
			} else {
				tool_release();
			}
			return true;
		}
		if (button->get_button_index() == MouseButton::RIGHT && button->is_pressed() && drag != DRAG_NONE) {
			tool_cancel();
			return true;
		}
		return false;
	}

	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		pointer_view = motion->get_position();
		pointer_inside = true;
		float pressure = motion->get_pressure();
		if (pressure > 0.0f && pressure < 1.0f) {
			pen_detected = true;
		}
		if (pressure <= 0.0f) {
			pressure = 1.0f;
		}
		pen_inverted = motion->get_pen_inverted();
		const Vector2 position = view_to_content(pointer_view);
		emit_signal(SNAME("pointer_moved"), position);
		if (drag != DRAG_NONE) {
			tool_drag(position, pressure);
			return true;
		}
		overlay->queue_redraw();
		return false;
	}

	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == Key::ESCAPE && drag != DRAG_NONE) {
		tool_cancel();
		return true;
	}
	return false;
}

void TetraImageCanvas::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_EXIT: {
			pointer_inside = false;
			overlay->queue_redraw();
			emit_signal(SNAME("pointer_exited"));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				if (drag != DRAG_NONE) {
					tool_release();
				}
				outline_timer->stop();
			} else if (!outline_points.is_empty()) {
				outline_timer->start();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (drag != DRAG_NONE) {
				tool_release();
			}
		} break;
	}
}

void TetraImageCanvas::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &TetraImageCanvas::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &TetraImageCanvas::get_document);
	ClassDB::bind_method(D_METHOD("set_tool_options", "options"), &TetraImageCanvas::set_tool_options);
	ClassDB::bind_method(D_METHOD("get_tool_options"), &TetraImageCanvas::get_tool_options);
	ClassDB::bind_method(D_METHOD("set_colors", "foreground", "background"), &TetraImageCanvas::set_colors);
	ClassDB::bind_method(D_METHOD("set_input_locked", "locked"), &TetraImageCanvas::set_input_locked);
	ClassDB::bind_method(D_METHOD("is_input_locked"), &TetraImageCanvas::is_input_locked);
	ClassDB::bind_method(D_METHOD("tool_press", "position", "pressure", "shift", "ctrl"), &TetraImageCanvas::tool_press, DEFVAL(1.0), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("tool_drag", "position", "pressure"), &TetraImageCanvas::tool_drag, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("tool_release"), &TetraImageCanvas::tool_release);
	ClassDB::bind_method(D_METHOD("tool_cancel"), &TetraImageCanvas::tool_cancel);
	ClassDB::bind_method(D_METHOD("is_tool_active"), &TetraImageCanvas::is_tool_active);
	ClassDB::bind_method(D_METHOD("set_clone_source", "position"), &TetraImageCanvas::set_clone_source);
	ClassDB::bind_method(D_METHOD("has_clone_source_point"), &TetraImageCanvas::has_clone_source_point);
	ClassDB::bind_method(D_METHOD("get_tile_update_count"), &TetraImageCanvas::get_tile_update_count);
	ClassDB::bind_method(D_METHOD("get_tile_count"), &TetraImageCanvas::get_tile_count);
	ClassDB::bind_method(D_METHOD("get_outline_point_count"), &TetraImageCanvas::get_outline_point_count);

	ADD_SIGNAL(MethodInfo("pointer_moved", PropertyInfo(Variant::VECTOR2, "position")));
	ADD_SIGNAL(MethodInfo("pointer_exited"));
	ADD_SIGNAL(MethodInfo("color_picked", PropertyInfo(Variant::COLOR, "color"), PropertyInfo(Variant::BOOL, "background")));
	ADD_SIGNAL(MethodInfo("status_message", PropertyInfo(Variant::STRING, "text")));
}

TetraImageCanvas::TetraImageCanvas() {
	set_default_cursor_shape(CURSOR_CROSS);

	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(OUTLINE_SHADER);
	outline_material.instantiate();
	outline_material->set_shader(shader);

	// Both feedback layers sit below the scroll bars of the base class.
	outline = memnew(Control);
	outline->set_mouse_filter(MOUSE_FILTER_IGNORE);
	outline->set_material(outline_material);
	outline->connect(SceneStringName(draw), callable_mp(this, &TetraImageCanvas::_draw_outline));
	add_child(outline);
	move_child(outline, 0);

	overlay = memnew(Control);
	overlay->set_mouse_filter(MOUSE_FILTER_IGNORE);
	overlay->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	overlay->connect(SceneStringName(draw), callable_mp(this, &TetraImageCanvas::_draw_overlay));
	add_child(overlay);
	move_child(overlay, 1);

	outline_timer = memnew(Timer);
	outline_timer->set_wait_time(0.12);
	outline_timer->connect("timeout", callable_mp(this, &TetraImageCanvas::_outline_tick));
	add_child(outline_timer);

	// The airbrush keeps spraying while the pointer rests.
	airbrush_timer = memnew(Timer);
	airbrush_timer->set_wait_time(0.04);
	airbrush_timer->connect("timeout", callable_mp(this, &TetraImageCanvas::_airbrush_tick));
	add_child(airbrush_timer);

	connect(SNAME("view_changed"), callable_mp(this, &TetraImageCanvas::_view_changed));
}
