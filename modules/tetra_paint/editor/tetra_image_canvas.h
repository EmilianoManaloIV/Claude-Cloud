#pragma once

#include "tetra_image_tools.h"

#include "../tetra_image_document.h"

#include "tetra/editor/tetra_canvas_view.h"

#include "core/templates/local_vector.h"

class ImageTexture;
class ShaderMaterial;
class Timer;

// The canvas of the Image workspace. It shows a TetraImageDocument and turns
// pointer input into edits with the active tool.
//
// Display: the composite is kept in a grid of 256 px textures. A change of
// the document re-composites only the changed rectangle, once per frame, and
// uploads only the tiles it touches. The selection outline ("marching ants")
// and the tool feedback (brush outline, gradient line, rubber band) live in
// two child controls so that neither repaints the tiles.
//
// Input: the mouse handlers call tool_press(), tool_drag() and
// tool_release(), which are also bound, so a script can perform exactly what
// a pointer gesture does, in document coordinates.
class TetraImageCanvas : public TetraCanvasView {
	GDCLASS(TetraImageCanvas, TetraCanvasView);

	enum {
		TILE_SIZE = 256,
		// Beyond this many outline vertices the rest of a very ragged selection edge is not drawn.
		MAX_OUTLINE_POINTS = 600000,
	};

	enum DragMode {
		DRAG_NONE,
		DRAG_STROKE,
		DRAG_GRADIENT,
		DRAG_SELECT,
		DRAG_MOVE,
		DRAG_PICK,
	};

	struct Tile {
		Ref<Image> image;
		Ref<ImageTexture> texture;
	};

	Ref<TetraImageDocument> document;

	LocalVector<Tile> tiles;
	Size2i tile_count;
	Rect2i pending_dirty;
	bool refresh_queued = false;
	int tile_update_count = 0;

	Control *outline = nullptr;
	Ref<ShaderMaterial> outline_material;
	Timer *outline_timer = nullptr;
	Vector<Point2> outline_points;
	int outline_phase = 0;
	bool outline_update_queued = false;

	Control *overlay = nullptr;
	Vector2 pointer_view;
	bool pointer_inside = false;

	TetraImageTools::Tool tool = TetraImageTools::TOOL_PAINTBRUSH;
	Dictionary tool_options;
	Color foreground = Color(0, 0, 0, 1);
	Color background = Color(1, 1, 1, 1);
	bool input_locked = false;

	DragMode drag = DRAG_NONE;
	Vector2 drag_start;
	Vector2 drag_current;
	bool drag_shift = false;
	bool drag_ctrl = false;
	int drag_layer = -1;
	Vector2i move_offset;
	bool move_group_open = false;

	bool first_point_pending = false;
	bool has_last_stroke_point = false;
	Vector2 last_stroke_point;
	float last_pressure = 1.0f;
	Timer *airbrush_timer = nullptr;
	// Set once a pointer reports intermediate pressure, which only pens do.
	bool pen_detected = false;
	bool pen_inverted = false;

	bool has_clone_source = false;
	Vector2 clone_source;
	bool has_clone_offset = false;
	Vector2 clone_offset;

	void _rebuild_tiles();
	void _refresh_tiles();
	void _document_changed(const Rect2i &p_rect);
	void _document_resized();
	void _selection_changed();
	void _update_outline();
	void _outline_tick();
	void _draw_outline();
	void _draw_overlay();
	void _view_changed();
	void _airbrush_tick();

	bool _layer_editable(int p_layer_id);
	Dictionary _stroke_settings(const Vector2 &p_position);
	TetraImageDocument::SelectOp _select_op() const;
	void _feather_and_close(const String &p_name);
	void _pick(const Vector2 &p_position);
	void _begin(const Vector2 &p_position, float p_pressure, bool p_shift, bool p_ctrl, bool p_defer_first_point);
	void _message(const String &p_text);

protected:
	void _notification(int p_what);
	static void _bind_methods();

	virtual void _canvas_draw() override;
	virtual bool _canvas_gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_document(const Ref<TetraImageDocument> &p_document);
	Ref<TetraImageDocument> get_document() const;

	void set_tool(TetraImageTools::Tool p_tool);
	TetraImageTools::Tool get_tool() const { return tool; }
	// The values of the active tool's option form (see TetraImageTools::get_options_schema()).
	void set_tool_options(const Dictionary &p_options);
	Dictionary get_tool_options() const { return tool_options; }
	// Display (sRGB) colours.
	void set_colors(const Color &p_foreground, const Color &p_background);

	// While locked the pointer does not edit; navigation still works.
	void set_input_locked(bool p_locked);
	bool is_input_locked() const { return input_locked; }

	// One pointer gesture with the active tool, in document pixels.
	void tool_press(const Vector2 &p_position, float p_pressure = 1.0f, bool p_shift = false, bool p_ctrl = false);
	void tool_drag(const Vector2 &p_position, float p_pressure = 1.0f);
	void tool_release();
	void tool_cancel();
	bool is_tool_active() const { return drag != DRAG_NONE; }

	void set_clone_source(const Vector2 &p_position);
	bool has_clone_source_point() const { return has_clone_source; }

	// Counters for tests: textures uploaded so far, and vertices of the selection outline.
	int get_tile_update_count() const { return tile_update_count; }
	int get_tile_count() const { return tiles.size(); }
	int get_outline_point_count() const { return outline_points.size(); }

	TetraImageCanvas();
};
