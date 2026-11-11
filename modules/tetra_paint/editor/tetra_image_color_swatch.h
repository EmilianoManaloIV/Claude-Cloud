#pragma once

#include "scene/gui/control.h"

// GIMP's colour area: the foreground square overlapping the background
// square, a corner that swaps the two and a corner that resets them to black
// and white. It only shows the colours and reports clicks; the workspace
// owns the colours.
class TetraImageColorSwatch : public Control {
	GDCLASS(TetraImageColorSwatch, Control);

	enum Area {
		AREA_NONE,
		AREA_FOREGROUND,
		AREA_BACKGROUND,
		AREA_SWAP,
		AREA_DEFAULT,
	};

	Color foreground = Color(0, 0, 0, 1);
	Color background = Color(1, 1, 1, 1);

	Rect2 _get_area_rect(Area p_area) const;
	Area _get_area_at(const Point2 &p_position) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

	virtual void gui_input(const Ref<InputEvent> &p_event) override;

public:
	virtual Size2 get_minimum_size() const override;
	virtual String get_tooltip(const Point2 &p_pos) const override;

	void set_colors(const Color &p_foreground, const Color &p_background);
	Color get_foreground() const { return foreground; }
	Color get_background() const { return background; }

	TetraImageColorSwatch();
};
