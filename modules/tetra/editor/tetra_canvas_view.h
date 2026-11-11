#pragma once

#include "scene/gui/control.h"

class HScrollBar;
class ImageTexture;
class VScrollBar;
class ViewPanner;

// Base class of 2D document views such as the paint canvas: a content area
// measured in document units that is shown zoomed and panned inside the
// control.
//
//   view position = (content position - view offset) * zoom
//
// The base handles navigation. The mouse wheel zooms about the cursor (or
// scrolls, see set_wheel_scrolls), Ctrl+wheel and pinch gestures zoom, and
// middle-drag or Space+left-drag pan. Scroll bars appear on the axes where
// the zoomed content is larger than the view. The view centre always stays
// over the content, so small content can be moved about and large content can
// be scrolled half a view past its edges.
//
// A subclass draws in _canvas_draw() and takes input in _canvas_gui_input().
class TetraCanvasView : public Control {
	GDCLASS(TetraCanvasView, Control);

	Size2 content_size = Size2(1, 1);
	real_t zoom = 1.0;
	real_t zoom_min = 1.0 / 64.0;
	real_t zoom_max = 256.0;
	Vector2 view_offset;
	bool wheel_scrolls = false;
	// Set until the control has a size, so that a view fitted before it is laid out still ends up fitted.
	bool fit_when_sized = true;
	bool updating_scroll_bars = false;

	HScrollBar *h_scroll = nullptr;
	VScrollBar *v_scroll = nullptr;
	Ref<ViewPanner> panner;
	Ref<ImageTexture> checkerboard;

	void _setup_panner();
	void _update_checkerboard();
	// Limits the offset, updates the scroll bars, redraws and emits view_changed.
	void _view_updated();
	void _get_offset_limits(Vector2 &r_min, Vector2 &r_max) const;
	void _scroll_bar_changed(double p_value);
	void _pan_callback(Vector2 p_scroll_vec, Ref<InputEvent> p_event);
	void _zoom_callback(float p_zoom_factor, Vector2 p_origin, Ref<InputEvent> p_event);
	bool _wheel_input(const Ref<InputEvent> &p_event);

protected:
	void _notification(int p_what);
	static void _bind_methods();

	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	// Called during NOTIFICATION_DRAW with the identity transform. Use
	// draw_set_transform_to_content() to draw in document units.
	virtual void _canvas_draw() {}
	// Offered every input event before the base uses it for navigation; return
	// true to consume it. While a pan is under way (the pan key is held or a
	// drag has begun) the events go to the navigation first instead.
	virtual bool _canvas_gui_input(const Ref<InputEvent> &p_event) { return false; }

	// For use in _canvas_draw(). After draw_set_transform_to_content() the draw_*
	// functions take content coordinates; draw_reset_transform() goes back to view pixels.
	void draw_set_transform_to_content();
	void draw_reset_transform();
	// Fills p_content_rect with the editor's transparency checkerboard. The
	// squares keep their size on screen at every zoom. Resets the draw transform.
	void draw_checkerboard(const Rect2 &p_content_rect);

public:
	void set_content_size(const Size2 &p_size);
	Size2 get_content_size() const;

	// Changes the zoom keeping the content under p_view_pivot (in view pixels) in place.
	void set_zoom(real_t p_zoom, const Vector2 &p_view_pivot);
	// Changes the zoom about the centre of the view.
	void set_zoom_centered(real_t p_zoom);
	real_t get_zoom() const;
	void set_zoom_limits(real_t p_min, real_t p_max);
	real_t get_zoom_min() const;
	real_t get_zoom_max() const;
	// Shows the whole content, as large as fits, in the middle of the view.
	void zoom_to_fit();
	// Moves the middle of the content to the middle of the view without changing the zoom.
	void center_content();

	// The content position shown at the top-left corner of the view.
	void set_view_offset(const Vector2 &p_offset);
	Vector2 get_view_offset() const;

	Vector2 view_to_content(const Vector2 &p_view_position) const;
	Vector2 content_to_view(const Vector2 &p_content_position) const;
	// The part of the content that is inside the view, in content coordinates.
	Rect2 get_visible_content_rect() const;

	// When set, the plain wheel scrolls vertically and Shift+wheel horizontally;
	// zooming is then Ctrl+wheel. Otherwise the plain wheel zooms.
	void set_wheel_scrolls(bool p_scrolls);
	bool is_wheel_scrolling() const;

	// True while the pan key is held or a pan drag is under way.
	bool is_panning() const;

	TetraCanvasView();
};
