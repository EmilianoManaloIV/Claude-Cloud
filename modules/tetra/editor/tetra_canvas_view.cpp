#include "tetra_canvas_view.h"

#include "core/input/input_event.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/view_panner.h"
#include "scene/resources/image_texture.h"

namespace {

// Zoom change of one wheel notch.
const real_t WHEEL_ZOOM_FACTOR = 1.2;
// View pixels scrolled by one wheel notch, before the editor scale.
const real_t WHEEL_SCROLL_STEP = 48.0;
// View pixels left free around the content by zoom_to_fit(), before the editor scale.
const real_t FIT_MARGIN = 16.0;
// The checkerboard icon is repeated into a texture of about this size, so that a view is covered by few draws.
const int CHECKERBOARD_TEXTURE_SIZE = 256;

} // namespace

void TetraCanvasView::_setup_panner() {
	// Outside the editor (a headless script) there are no settings; middle-drag panning still works.
	if (EditorSettings::get_singleton()) {
		panner->setup(ViewPanner::SCROLL_ZOOMS, ED_GET_SHORTCUT("canvas_item_editor/pan_view"), bool(EDITOR_GET("editors/panning/simple_panning")));
		panner->setup_warped_panning(this, EDITOR_GET("editors/panning/warped_mouse_panning"));
	}
}

void TetraCanvasView::_update_checkerboard() {
	checkerboard.unref();
	const Ref<Texture2D> icon = get_editor_theme_icon(SNAME("Checkerboard"));
	if (icon.is_null()) {
		return;
	}
	const Ref<Image> tile = icon->get_image();
	if (tile.is_null() || tile->is_empty()) {
		return;
	}
	const int repeats_x = MAX(1, CHECKERBOARD_TEXTURE_SIZE / tile->get_width());
	const int repeats_y = MAX(1, CHECKERBOARD_TEXTURE_SIZE / tile->get_height());
	Ref<Image> sheet = Image::create_empty(tile->get_width() * repeats_x, tile->get_height() * repeats_y, false, tile->get_format());
	const Rect2i tile_rect(0, 0, tile->get_width(), tile->get_height());
	for (int y = 0; y < repeats_y; y++) {
		for (int x = 0; x < repeats_x; x++) {
			sheet->blit_rect(tile, tile_rect, Point2i(x * tile->get_width(), y * tile->get_height()));
		}
	}
	checkerboard = ImageTexture::create_from_image(sheet);
}

void TetraCanvasView::_get_offset_limits(Vector2 &r_min, Vector2 &r_max) const {
	// The view centre may be anywhere over the content.
	const Vector2 half_view = get_size() / zoom * 0.5;
	r_min = -half_view;
	r_max = content_size - half_view;
}

void TetraCanvasView::_view_updated() {
	Vector2 offset_min;
	Vector2 offset_max;
	_get_offset_limits(offset_min, offset_max);
	view_offset = view_offset.clamp(offset_min, offset_max);
	// Whole view pixels, so that content drawn at integer positions stays sharp.
	view_offset = (view_offset * zoom).round() / zoom;

	const Size2 view_size = get_size();
	const Size2 page = view_size / zoom;
	updating_scroll_bars = true;
	h_scroll->set_min(offset_min.x);
	h_scroll->set_max(offset_max.x + page.x);
	h_scroll->set_page(page.x);
	h_scroll->set_value(view_offset.x);
	h_scroll->set_visible(content_size.x * zoom > view_size.x);
	v_scroll->set_min(offset_min.y);
	v_scroll->set_max(offset_max.y + page.y);
	v_scroll->set_page(page.y);
	v_scroll->set_value(view_offset.y);
	v_scroll->set_visible(content_size.y * zoom > view_size.y);
	// Each bar ends where the other begins.
	h_scroll->set_anchor_and_offset(SIDE_RIGHT, ANCHOR_END, v_scroll->is_visible() ? -v_scroll->get_combined_minimum_size().width : 0);
	v_scroll->set_anchor_and_offset(SIDE_BOTTOM, ANCHOR_END, h_scroll->is_visible() ? -h_scroll->get_combined_minimum_size().height : 0);
	updating_scroll_bars = false;

	queue_redraw();
	emit_signal(SNAME("view_changed"));
}

void TetraCanvasView::_scroll_bar_changed(double p_value) {
	if (updating_scroll_bars) {
		return;
	}
	fit_when_sized = false;
	view_offset = Vector2(h_scroll->get_value(), v_scroll->get_value());
	_view_updated();
}

void TetraCanvasView::_pan_callback(Vector2 p_scroll_vec, Ref<InputEvent> p_event) {
	set_view_offset(view_offset - p_scroll_vec / zoom);
}

void TetraCanvasView::_zoom_callback(float p_zoom_factor, Vector2 p_origin, Ref<InputEvent> p_event) {
	set_zoom(zoom * p_zoom_factor, p_origin);
}

bool TetraCanvasView::_wheel_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_null() || !button->is_pressed()) {
		return false;
	}
	Vector2 direction;
	switch (button->get_button_index()) {
		case MouseButton::WHEEL_UP: {
			direction.y = -1;
		} break;
		case MouseButton::WHEEL_DOWN: {
			direction.y = 1;
		} break;
		case MouseButton::WHEEL_LEFT: {
			direction.x = -1;
		} break;
		case MouseButton::WHEEL_RIGHT: {
			direction.x = 1;
		} break;
		default: {
			return false;
		}
	}
	// Smooth-scrolling devices send fractions of a notch.
	const real_t amount = button->get_factor() > 0.0 ? button->get_factor() : 1.0;

	const bool zooms = direction.y != 0 && (button->is_command_or_control_pressed() || (!wheel_scrolls && !button->is_shift_pressed()));
	if (zooms) {
		set_zoom(zoom * Math::pow(WHEEL_ZOOM_FACTOR, -direction.y * amount), button->get_position());
		return true;
	}
	if (button->is_shift_pressed() && direction.x == 0) {
		direction = Vector2(direction.y, 0);
	}
	set_view_offset(view_offset + direction * amount * WHEEL_SCROLL_STEP * EDSCALE / zoom);
	return true;
}

void TetraCanvasView::gui_input(const Ref<InputEvent> &p_event) {
	ERR_FAIL_COND(p_event.is_null());

	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->is_pressed() && !has_focus()) {
		// The pan key only reaches a focused control.
		grab_focus();
	}

	// An armed or running pan owns the mouse, so that Space+drag does not also paint.
	if (panner->is_panning() && panner->gui_input(p_event, get_global_rect())) {
		accept_event();
		return;
	}
	if (_canvas_gui_input(p_event)) {
		accept_event();
		return;
	}
	if (_wheel_input(p_event) || panner->gui_input(p_event, get_global_rect())) {
		accept_event();
	}
}

void TetraCanvasView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_setup_panner();
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->check_changed_settings_in_group("editors/panning")) {
				_setup_panner();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			_update_checkerboard();
			queue_redraw();
		} break;

		case NOTIFICATION_RESIZED: {
			if (fit_when_sized && get_size().x > 0 && get_size().y > 0) {
				zoom_to_fit();
			} else {
				_view_updated();
			}
		} break;

		case NOTIFICATION_FOCUS_EXIT: {
			panner->release_pan_key();
		} break;

		case NOTIFICATION_DRAW: {
			_canvas_draw();
		} break;
	}
}

void TetraCanvasView::draw_set_transform_to_content() {
	draw_set_transform(-view_offset * zoom, 0, Size2(zoom, zoom));
}

void TetraCanvasView::draw_reset_transform() {
	draw_set_transform(Point2(), 0, Size2(1, 1));
}

void TetraCanvasView::draw_checkerboard(const Rect2 &p_content_rect) {
	draw_reset_transform();
	if (checkerboard.is_null()) {
		return;
	}
	const Rect2 target(content_to_view(p_content_rect.position).round(), (p_content_rect.size * zoom).round());
	const Rect2 shown_area = target.intersection(Rect2(Point2(), get_size()));
	if (!shown_area.has_area()) {
		return;
	}
	// The pattern is anchored to the corner of the rectangle: it moves with the content but does not scale.
	const Size2 tile = checkerboard->get_size();
	const int first_x = int(Math::floor((shown_area.position.x - target.position.x) / tile.x));
	const int first_y = int(Math::floor((shown_area.position.y - target.position.y) / tile.y));
	const int last_x = int(Math::ceil((shown_area.get_end().x - target.position.x) / tile.x));
	const int last_y = int(Math::ceil((shown_area.get_end().y - target.position.y) / tile.y));
	for (int y = first_y; y < last_y; y++) {
		for (int x = first_x; x < last_x; x++) {
			const Rect2 cell(target.position + Vector2(x, y) * tile, tile);
			const Rect2 shown = cell.intersection(shown_area);
			if (shown.has_area()) {
				draw_texture_rect_region(checkerboard, shown, Rect2(shown.position - cell.position, shown.size));
			}
		}
	}
}

void TetraCanvasView::set_content_size(const Size2 &p_size) {
	ERR_FAIL_COND_MSG(p_size.x <= 0 || p_size.y <= 0, "The content size must be positive.");
	if (content_size == p_size) {
		return;
	}
	content_size = p_size;
	if (fit_when_sized && get_size().x > 0 && get_size().y > 0) {
		zoom_to_fit();
	} else {
		_view_updated();
	}
}

Size2 TetraCanvasView::get_content_size() const {
	return content_size;
}

void TetraCanvasView::set_zoom(real_t p_zoom, const Vector2 &p_view_pivot) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_zoom) || p_zoom <= 0, "The zoom must be a positive number.");
	fit_when_sized = false;
	const Vector2 pivot = view_to_content(p_view_pivot);
	zoom = CLAMP(p_zoom, zoom_min, zoom_max);
	view_offset = pivot - p_view_pivot / zoom;
	_view_updated();
}

void TetraCanvasView::set_zoom_centered(real_t p_zoom) {
	set_zoom(p_zoom, get_size() * 0.5);
}

real_t TetraCanvasView::get_zoom() const {
	return zoom;
}

void TetraCanvasView::set_zoom_limits(real_t p_min, real_t p_max) {
	ERR_FAIL_COND_MSG(p_min <= 0 || p_max < p_min, "The zoom limits must be positive and ordered.");
	zoom_min = p_min;
	zoom_max = p_max;
	if (zoom < zoom_min || zoom > zoom_max) {
		zoom = CLAMP(zoom, zoom_min, zoom_max);
		_view_updated();
	}
}

real_t TetraCanvasView::get_zoom_min() const {
	return zoom_min;
}

real_t TetraCanvasView::get_zoom_max() const {
	return zoom_max;
}

void TetraCanvasView::zoom_to_fit() {
	const Size2 view_size = get_size();
	if (view_size.x <= 0 || view_size.y <= 0) {
		fit_when_sized = true;
		return;
	}
	fit_when_sized = false;
	const Size2 available = (view_size - Size2(2, 2) * FIT_MARGIN * EDSCALE).max(Size2(1, 1));
	zoom = CLAMP(MIN(available.x / content_size.x, available.y / content_size.y), zoom_min, zoom_max);
	view_offset = content_size * 0.5 - view_size / zoom * 0.5;
	_view_updated();
}

void TetraCanvasView::center_content() {
	fit_when_sized = false;
	view_offset = content_size * 0.5 - get_size() / zoom * 0.5;
	_view_updated();
}

void TetraCanvasView::set_view_offset(const Vector2 &p_offset) {
	fit_when_sized = false;
	view_offset = p_offset;
	_view_updated();
}

Vector2 TetraCanvasView::get_view_offset() const {
	return view_offset;
}

Vector2 TetraCanvasView::view_to_content(const Vector2 &p_view_position) const {
	return p_view_position / zoom + view_offset;
}

Vector2 TetraCanvasView::content_to_view(const Vector2 &p_content_position) const {
	return (p_content_position - view_offset) * zoom;
}

Rect2 TetraCanvasView::get_visible_content_rect() const {
	return Rect2(view_offset, get_size() / zoom).intersection(Rect2(Point2(), content_size));
}

void TetraCanvasView::set_wheel_scrolls(bool p_scrolls) {
	wheel_scrolls = p_scrolls;
}

bool TetraCanvasView::is_wheel_scrolling() const {
	return wheel_scrolls;
}

bool TetraCanvasView::is_panning() const {
	return panner->is_panning();
}

void TetraCanvasView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_content_size", "size"), &TetraCanvasView::set_content_size);
	ClassDB::bind_method(D_METHOD("get_content_size"), &TetraCanvasView::get_content_size);
	ClassDB::bind_method(D_METHOD("set_zoom", "zoom", "view_pivot"), &TetraCanvasView::set_zoom);
	ClassDB::bind_method(D_METHOD("set_zoom_centered", "zoom"), &TetraCanvasView::set_zoom_centered);
	ClassDB::bind_method(D_METHOD("get_zoom"), &TetraCanvasView::get_zoom);
	ClassDB::bind_method(D_METHOD("set_zoom_limits", "min", "max"), &TetraCanvasView::set_zoom_limits);
	ClassDB::bind_method(D_METHOD("get_zoom_min"), &TetraCanvasView::get_zoom_min);
	ClassDB::bind_method(D_METHOD("get_zoom_max"), &TetraCanvasView::get_zoom_max);
	ClassDB::bind_method(D_METHOD("zoom_to_fit"), &TetraCanvasView::zoom_to_fit);
	ClassDB::bind_method(D_METHOD("center_content"), &TetraCanvasView::center_content);
	ClassDB::bind_method(D_METHOD("set_view_offset", "offset"), &TetraCanvasView::set_view_offset);
	ClassDB::bind_method(D_METHOD("get_view_offset"), &TetraCanvasView::get_view_offset);
	ClassDB::bind_method(D_METHOD("view_to_content", "view_position"), &TetraCanvasView::view_to_content);
	ClassDB::bind_method(D_METHOD("content_to_view", "content_position"), &TetraCanvasView::content_to_view);
	ClassDB::bind_method(D_METHOD("get_visible_content_rect"), &TetraCanvasView::get_visible_content_rect);
	ClassDB::bind_method(D_METHOD("set_wheel_scrolls", "scrolls"), &TetraCanvasView::set_wheel_scrolls);
	ClassDB::bind_method(D_METHOD("is_wheel_scrolling"), &TetraCanvasView::is_wheel_scrolling);
	ClassDB::bind_method(D_METHOD("is_panning"), &TetraCanvasView::is_panning);
	ClassDB::bind_method(D_METHOD("draw_set_transform_to_content"), &TetraCanvasView::draw_set_transform_to_content);
	ClassDB::bind_method(D_METHOD("draw_reset_transform"), &TetraCanvasView::draw_reset_transform);
	ClassDB::bind_method(D_METHOD("draw_checkerboard", "content_rect"), &TetraCanvasView::draw_checkerboard);

	ADD_SIGNAL(MethodInfo("view_changed"));
}

TetraCanvasView::TetraCanvasView() {
	set_clip_contents(true);
	set_focus_mode(FOCUS_ALL);

	panner.instantiate();
	panner->set_callbacks(callable_mp(this, &TetraCanvasView::_pan_callback), callable_mp(this, &TetraCanvasView::_zoom_callback));

	h_scroll = memnew(HScrollBar);
	h_scroll->set_anchors_and_offsets_preset(PRESET_BOTTOM_WIDE);
	h_scroll->set_step(0.001);
	h_scroll->hide();
	h_scroll->connect(SceneStringName(value_changed), callable_mp(this, &TetraCanvasView::_scroll_bar_changed));
	add_child(h_scroll);

	v_scroll = memnew(VScrollBar);
	v_scroll->set_anchors_and_offsets_preset(PRESET_RIGHT_WIDE);
	v_scroll->set_step(0.001);
	v_scroll->hide();
	v_scroll->connect(SceneStringName(value_changed), callable_mp(this, &TetraCanvasView::_scroll_bar_changed));
	add_child(v_scroll);
}
