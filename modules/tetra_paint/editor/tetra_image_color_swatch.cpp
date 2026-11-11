#include "tetra_image_color_swatch.h"

#include "core/input/input_event.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"

Rect2 TetraImageColorSwatch::_get_area_rect(Area p_area) const {
	const real_t s = EDSCALE;
	switch (p_area) {
		case AREA_FOREGROUND:
			return Rect2(2 * s, 2 * s, 34 * s, 34 * s);
		case AREA_BACKGROUND:
			return Rect2(22 * s, 22 * s, 34 * s, 34 * s);
		case AREA_SWAP:
			return Rect2(40 * s, 2 * s, 16 * s, 16 * s);
		case AREA_DEFAULT:
			return Rect2(2 * s, 40 * s, 16 * s, 16 * s);
		default:
			return Rect2();
	}
}

TetraImageColorSwatch::Area TetraImageColorSwatch::_get_area_at(const Point2 &p_position) const {
	// In drawing order from the top: the foreground covers a corner of the background.
	for (const Area area : { AREA_FOREGROUND, AREA_BACKGROUND, AREA_SWAP, AREA_DEFAULT }) {
		if (_get_area_rect(area).has_point(p_position)) {
			return area;
		}
	}
	return AREA_NONE;
}

Size2 TetraImageColorSwatch::get_minimum_size() const {
	return Size2(58, 58) * EDSCALE;
}

String TetraImageColorSwatch::get_tooltip(const Point2 &p_pos) const {
	switch (_get_area_at(p_pos)) {
		case AREA_FOREGROUND:
			return TTR("Foreground colour. Click to change it.");
		case AREA_BACKGROUND:
			return TTR("Background colour. Click to change it.");
		case AREA_SWAP:
			return TTR("Swap the foreground and background colours (X).");
		case AREA_DEFAULT:
			return TTR("Reset to black and white (D).");
		default:
			return String();
	}
}

void TetraImageColorSwatch::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_null() || !button->is_pressed() || button->get_button_index() != MouseButton::LEFT) {
		return;
	}
	switch (_get_area_at(button->get_position())) {
		case AREA_FOREGROUND:
			emit_signal(SNAME("foreground_pressed"));
			break;
		case AREA_BACKGROUND:
			emit_signal(SNAME("background_pressed"));
			break;
		case AREA_SWAP:
			emit_signal(SNAME("swap_pressed"));
			break;
		case AREA_DEFAULT:
			emit_signal(SNAME("default_pressed"));
			break;
		default:
			return;
	}
	accept_event();
}

void TetraImageColorSwatch::set_colors(const Color &p_foreground, const Color &p_background) {
	foreground = p_foreground;
	background = p_background;
	queue_redraw();
}

void TetraImageColorSwatch::_notification(int p_what) {
	if (p_what != NOTIFICATION_DRAW) {
		return;
	}
	const real_t s = EDSCALE;
	const Color dark(0, 0, 0, 0.9);
	const Color light(1, 1, 1, 0.9);
	const Color line = get_theme_color(SceneStringName(font_color), SNAME("Label"));

	for (const Area area : { AREA_BACKGROUND, AREA_FOREGROUND }) {
		const Rect2 rect = _get_area_rect(area);
		draw_rect(rect, area == AREA_FOREGROUND ? foreground : background);
		draw_rect(rect.grow(-0.5), light, false, 1.0);
		draw_rect(rect.grow(0.5), dark, false, 1.0);
	}

	// A bent double arrow between the two squares.
	const Rect2 swap = _get_area_rect(AREA_SWAP);
	const Point2 a = swap.position + Vector2(3, 5) * s;
	const Point2 corner = swap.position + Vector2(11, 5) * s;
	const Point2 b = swap.position + Vector2(11, 13) * s;
	draw_polyline({ a, corner, b }, line, s);
	draw_polyline({ a + Vector2(3, -3) * s, a, a + Vector2(3, 3) * s }, line, s);
	draw_polyline({ b + Vector2(-3, -3) * s, b, b + Vector2(3, -3) * s }, line, s);

	// Black over white in miniature.
	const Rect2 reset = _get_area_rect(AREA_DEFAULT);
	const Rect2 small_bg(reset.position + Vector2(6, 6) * s, Size2(9, 9) * s);
	const Rect2 small_fg(reset.position + Vector2(1, 1) * s, Size2(9, 9) * s);
	draw_rect(small_bg, Color(1, 1, 1));
	draw_rect(small_bg.grow(0.5), dark, false, 1.0);
	draw_rect(small_fg, Color(0, 0, 0));
	draw_rect(small_fg.grow(0.5), light, false, 1.0);
}

void TetraImageColorSwatch::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_colors", "foreground", "background"), &TetraImageColorSwatch::set_colors);
	ClassDB::bind_method(D_METHOD("get_foreground"), &TetraImageColorSwatch::get_foreground);
	ClassDB::bind_method(D_METHOD("get_background"), &TetraImageColorSwatch::get_background);

	ADD_SIGNAL(MethodInfo("foreground_pressed"));
	ADD_SIGNAL(MethodInfo("background_pressed"));
	ADD_SIGNAL(MethodInfo("swap_pressed"));
	ADD_SIGNAL(MethodInfo("default_pressed"));
}

TetraImageColorSwatch::TetraImageColorSwatch() {
	set_mouse_filter(MOUSE_FILTER_STOP);
}
