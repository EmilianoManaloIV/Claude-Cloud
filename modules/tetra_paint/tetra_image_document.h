#pragma once

#include "tetra_layer_modes.h"

#include "core/io/image.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

// A layered raster image: Tetra's counterpart of a GIMP image.
//
// Storage is 32-bit float RGBA in linear light with straight (unassociated)
// alpha, one full-canvas buffer per layer. That is simple and fast enough for
// canvases up to about 4096 x 4096 (256 MB per layer at that size); there is
// no tiling and layers have no offset or size of their own.
//
// Every mutating method records an undo step and reports the changed area
// through the `changed` signal and the dirty rectangle. The document is not
// thread-safe: call it from one thread. The only methods meant for worker
// threads are the static filter_image() and get_operations().
class TetraImageDocument : public RefCounted {
	GDCLASS(TetraImageDocument, RefCounted);

public:
	enum LayerMode {
		LAYER_MODE_NORMAL = TetraLayerModes::MODE_NORMAL,
		LAYER_MODE_DISSOLVE = TetraLayerModes::MODE_DISSOLVE,
		LAYER_MODE_BEHIND = TetraLayerModes::MODE_BEHIND,
		LAYER_MODE_COLOR_ERASE = TetraLayerModes::MODE_COLOR_ERASE,
		LAYER_MODE_ERASE = TetraLayerModes::MODE_ERASE,
		LAYER_MODE_MERGE = TetraLayerModes::MODE_MERGE,
		LAYER_MODE_SPLIT = TetraLayerModes::MODE_SPLIT,
		LAYER_MODE_LIGHTEN_ONLY = TetraLayerModes::MODE_LIGHTEN_ONLY,
		LAYER_MODE_LUMA_LIGHTEN_ONLY = TetraLayerModes::MODE_LUMA_LIGHTEN_ONLY,
		LAYER_MODE_SCREEN = TetraLayerModes::MODE_SCREEN,
		LAYER_MODE_DODGE = TetraLayerModes::MODE_DODGE,
		LAYER_MODE_ADDITION = TetraLayerModes::MODE_ADDITION,
		LAYER_MODE_DARKEN_ONLY = TetraLayerModes::MODE_DARKEN_ONLY,
		LAYER_MODE_LUMA_DARKEN_ONLY = TetraLayerModes::MODE_LUMA_DARKEN_ONLY,
		LAYER_MODE_MULTIPLY = TetraLayerModes::MODE_MULTIPLY,
		LAYER_MODE_BURN = TetraLayerModes::MODE_BURN,
		LAYER_MODE_LINEAR_BURN = TetraLayerModes::MODE_LINEAR_BURN,
		LAYER_MODE_OVERLAY = TetraLayerModes::MODE_OVERLAY,
		LAYER_MODE_SOFT_LIGHT = TetraLayerModes::MODE_SOFT_LIGHT,
		LAYER_MODE_HARD_LIGHT = TetraLayerModes::MODE_HARD_LIGHT,
		LAYER_MODE_VIVID_LIGHT = TetraLayerModes::MODE_VIVID_LIGHT,
		LAYER_MODE_PIN_LIGHT = TetraLayerModes::MODE_PIN_LIGHT,
		LAYER_MODE_LINEAR_LIGHT = TetraLayerModes::MODE_LINEAR_LIGHT,
		LAYER_MODE_HARD_MIX = TetraLayerModes::MODE_HARD_MIX,
		LAYER_MODE_DIFFERENCE = TetraLayerModes::MODE_DIFFERENCE,
		LAYER_MODE_EXCLUSION = TetraLayerModes::MODE_EXCLUSION,
		LAYER_MODE_SUBTRACT = TetraLayerModes::MODE_SUBTRACT,
		LAYER_MODE_GRAIN_EXTRACT = TetraLayerModes::MODE_GRAIN_EXTRACT,
		LAYER_MODE_GRAIN_MERGE = TetraLayerModes::MODE_GRAIN_MERGE,
		LAYER_MODE_DIVIDE = TetraLayerModes::MODE_DIVIDE,
		LAYER_MODE_HSV_HUE = TetraLayerModes::MODE_HSV_HUE,
		LAYER_MODE_HSV_SATURATION = TetraLayerModes::MODE_HSV_SATURATION,
		LAYER_MODE_HSL_COLOR = TetraLayerModes::MODE_HSL_COLOR,
		LAYER_MODE_HSV_VALUE = TetraLayerModes::MODE_HSV_VALUE,
		LAYER_MODE_LCH_HUE = TetraLayerModes::MODE_LCH_HUE,
		LAYER_MODE_LCH_CHROMA = TetraLayerModes::MODE_LCH_CHROMA,
		LAYER_MODE_LCH_COLOR = TetraLayerModes::MODE_LCH_COLOR,
		LAYER_MODE_LCH_LIGHTNESS = TetraLayerModes::MODE_LCH_LIGHTNESS,
		LAYER_MODE_LUMINANCE = TetraLayerModes::MODE_LUMINANCE,
		LAYER_MODE_MAX = TetraLayerModes::MODE_MAX,
	};

	enum SelectOp {
		SELECT_REPLACE,
		SELECT_ADD,
		SELECT_SUBTRACT,
		SELECT_INTERSECT,
	};

	enum PaintTool {
		TOOL_PAINTBRUSH,
		TOOL_PENCIL,
		TOOL_ERASER,
		TOOL_AIRBRUSH,
		TOOL_SMUDGE,
		TOOL_CLONE,
		TOOL_MAX,
	};

	enum GradientType {
		GRADIENT_LINEAR,
		GRADIENT_BILINEAR,
		GRADIENT_RADIAL,
		GRADIENT_SQUARE,
		GRADIENT_CONICAL,
		GRADIENT_CONICAL_ASYMMETRIC,
		GRADIENT_SPIRAL_CLOCKWISE,
		GRADIENT_SPIRAL_ANTICLOCKWISE,
	};

	enum GradientRepeat {
		REPEAT_NONE,
		REPEAT_SAWTOOTH,
		REPEAT_TRIANGULAR,
	};

	enum Interpolation {
		INTERPOLATION_NEAREST,
		INTERPOLATION_LINEAR,
		INTERPOLATION_CUBIC,
	};

private:
	struct Layer {
		int id = 0;
		String name;
		bool visible = true;
		float opacity = 1.0f;
		LayerMode mode = LAYER_MODE_NORMAL;
		bool lock_alpha = false;
		Vector<float> pixels; // width * height * 4, rows top to bottom.
	};

	// A snapshot of the document structure for undo. Pixel buffers and the
	// selection are only kept where they differ between the two snapshots of
	// a step; the rest is taken from the live document when restoring.
	struct DocState {
		int width = 0;
		int height = 0;
		int active_layer = -1;
		LocalVector<Layer> layers;
		LocalVector<bool> has_pixels;
		bool has_selection = true;
		Vector<float> selection;
		bool selection_empty = true;
	};

	struct UndoStep {
		enum Type {
			TYPE_PIXELS, // data = the other version of rect in layer_id; swapped on undo/redo.
			TYPE_SELECTION, // data = the other version of rect in the selection mask.
			TYPE_STATE, // before/after structure snapshots.
		};
		Type type = TYPE_PIXELS;
		int layer_id = -1;
		Rect2i rect;
		Vector<float> data;
		DocState before;
		DocState after;
	};

	struct UndoGroup {
		String name;
		LocalVector<UndoStep> steps;
		uint64_t bytes = 0;
	};

	struct Stroke {
		bool active = false;
		int layer_id = -1;
		PaintTool tool = TOOL_PAINTBRUSH;
		float color[4] = { 0, 0, 0, 1 };
		float size = 20.0f;
		float hardness = 0.5f;
		float opacity = 1.0f;
		float flow = 1.0f;
		float spacing = 10.0f;
		float rate = 0.5f;
		int mode = LAYER_MODE_NORMAL;
		bool pressure_size = false;
		bool pressure_opacity = false;
		bool has_source_offset = false;
		Vector2 source_offset;
		bool has_source_point = false;
		Vector2 source_point;

		Vector<float> before; // The layer's pixels when the stroke began.
		LocalVector<float> coverage; // Accumulated brush coverage, canvas-sized.
		Rect2i bounds;
		bool has_last = false;
		Vector2 last_position;
		float last_pressure = 1.0f;
		double distance_since_dab = 0.0;
		LocalVector<float> profile; // Radial brush falloff lookup table.
		float profile_radius = -1.0f;
		float profile_hardness = -1.0f;
		LocalVector<float> smudge; // Colour carried by the smudge brush.
		int smudge_side = 0;
		bool smudge_ready = false;
	};

	int width = 0;
	int height = 0;
	LocalVector<Layer> layers;
	int next_layer_id = 1;
	int active_layer = -1;

	Vector<float> selection; // Empty, or width * height values in 0..1.
	bool selection_empty = true;
	mutable bool selection_bounds_valid = false;
	mutable Rect2i selection_bounds_cache;

	Rect2i dirty_rect;

	LocalVector<UndoGroup> undo_stack;
	LocalVector<UndoGroup> redo_stack;
	UndoGroup open_group;
	int group_depth = 0;
	uint64_t undo_memory_limit = 1024ull * 1024ull * 1024ull;

	Stroke stroke;

	int _find_layer(int p_id) const;
	Rect2i _canvas_rect() const { return Rect2i(0, 0, width, height); }
	Rect2i _clip_rect(const Rect2i &p_rect) const;
	Rect2i _edit_rect() const;
	const float *_selection_ptr() const { return selection_empty ? nullptr : selection.ptr(); }
	void _mark_dirty(const Rect2i &p_rect);
	void _reset(int p_width, int p_height);
	int _append_layer(const String &p_name, const Vector<float> &p_pixels);
	Vector<float> _new_buffer(const float *p_fill = nullptr) const;
	void _composite_row(int p_y, int p_x, int p_count, float *r_row) const;
	void _composite_into(const Rect2i &p_rect, float *r_out) const;
	Rect2i _diff_bounds(const float *p_a, const float *p_b) const;
	void _finish_structure_change(DocState &p_before, bool p_redraw);
	void _mix_into(float *p_dest, const Rect2i &p_rect, const float *p_src, int p_src_width, const Point2i &p_src_origin, bool p_lock_alpha, bool p_use_selection) const;
	void _commit_pixels(int p_index, const Rect2i &p_rect, const float *p_src, int p_src_width, const Point2i &p_src_origin, bool p_use_selection = true);
	static Vector<float> _image_to_linear(const Ref<Image> &p_image, bool p_force_linearise = false);

	// Undo recording.
	void _undo_open(const String &p_name);
	void _undo_close();
	void _undo_push_pixels(int p_layer_id, const Rect2i &p_rect, const float *p_before_full);
	void _undo_push_selection(const Vector<float> &p_before, bool p_before_empty, const Rect2i &p_before_bounds);
	DocState _capture_state() const;
	void _undo_push_state(DocState &p_before);
	void _restore_state(const DocState &p_state);
	void _apply_group(UndoGroup &p_group, bool p_undo);
	void _trim_undo();
	void _clear_history();

	// Selection.
	void _selection_changed();
	void _combine_selection(const float *p_shape, SelectOp p_op, const String &p_name);
	void _set_selection(const Vector<float> &p_mask, const String &p_name);
	void _region_by_color(const float *p_pixels, const Point2i &p_seed, float p_threshold, bool p_contiguous, LocalVector<float> &r_mask) const;
	Vector<float> _sample_source(int p_layer_index, bool p_sample_merged) const;

	// Painting.
	void _stroke_dab(const Vector2 &p_position, float p_pressure, Rect2i &r_dirty);
	void _stroke_update_profile(float p_radius, float p_hardness);

	// Layer and whole-image transforms.
	void _set_layer_buffer(int p_index, const Vector<float> &p_pixels, const String &p_name);
	void _replace_canvas(int p_width, int p_height, const LocalVector<Vector<float>> &p_layer_pixels, const Vector<float> &p_selection, const String &p_name);

	// Operations.
	static Error _run_operation(const String &p_id, const Dictionary &p_values, const float *p_src, float *r_dst, int p_width, int p_height);

	// Files.
	Error _load_ora(const String &p_path);

	static void _bind_selection_methods();
	static void _bind_paint_methods();
	static void _bind_edit_methods();
	static void _bind_operation_methods();
	static void _bind_file_methods();

protected:
	static void _bind_methods();

public:
	// Document.
	Error create(int p_width, int p_height, const Color &p_fill = Color(1, 1, 1, 1));
	Vector2i get_size() const { return Vector2i(width, height); }
	int get_width() const { return width; }
	int get_height() const { return height; }

	// Layers. The stack is ordered bottom to top; ids are stable.
	int add_layer(const String &p_name, int p_index = -1);
	Error remove_layer(int p_id);
	int duplicate_layer(int p_id);
	Error move_layer(int p_id, int p_new_index);
	Error merge_down(int p_id);
	Error flatten();
	PackedInt32Array get_layer_ids() const;
	int get_layer_count() const { return layers.size(); }
	int get_layer_index(int p_id) const { return _find_layer(p_id); }
	int get_active_layer() const { return active_layer; }
	void set_active_layer(int p_id);

	String layer_get_name(int p_id) const;
	void layer_set_name(int p_id, const String &p_name);
	bool layer_is_visible(int p_id) const;
	void layer_set_visible(int p_id, bool p_visible);
	float layer_get_opacity(int p_id) const;
	void layer_set_opacity(int p_id, float p_opacity);
	LayerMode layer_get_mode(int p_id) const;
	void layer_set_mode(int p_id, LayerMode p_mode);
	bool layer_get_lock_alpha(int p_id) const;
	void layer_set_lock_alpha(int p_id, bool p_lock);
	Ref<Image> layer_get_image(int p_id) const;
	Error layer_set_image(int p_id, const Ref<Image> &p_image, const Vector2i &p_offset = Vector2i());

	static PackedStringArray get_layer_mode_names();
	static Color blend_colors(LayerMode p_mode, const Color &p_backdrop, const Color &p_layer, float p_opacity = 1.0f);

	// Compositing. An empty rectangle means the whole canvas.
	Ref<Image> composite(const Rect2i &p_rect = Rect2i()) const;
	Ref<Image> composite_float(const Rect2i &p_rect = Rect2i()) const;
	Rect2i get_and_clear_dirty_rect();

	// Undo.
	void begin_undo_group(const String &p_name);
	void end_undo_group();
	// Reverts everything recorded since the outermost begin_undo_group() and
	// drops the group: nothing joins the history and the redo stack is kept.
	// This is how a live preview or an interactive drag is taken back.
	void cancel_undo_group();
	bool is_undo_group_open() const { return group_depth > 0; }
	bool undo();
	bool redo();
	bool can_undo() const { return !undo_stack.is_empty(); }
	bool can_redo() const { return !redo_stack.is_empty(); }
	PackedStringArray get_undo_names() const;
	PackedStringArray get_redo_names() const;
	void clear_undo_history();
	void set_undo_memory_limit(int64_t p_bytes);
	int64_t get_undo_memory_limit() const { return int64_t(undo_memory_limit); }
	int64_t get_undo_memory_usage() const;

	// Selection.
	void select_all();
	void select_none();
	void select_rect(const Rect2i &p_rect, SelectOp p_op = SELECT_REPLACE);
	void select_ellipse(const Rect2i &p_rect, SelectOp p_op = SELECT_REPLACE, bool p_antialias = true);
	void select_polygon(const PackedVector2Array &p_points, SelectOp p_op = SELECT_REPLACE, bool p_antialias = true);
	void select_by_color(const Vector2i &p_point, float p_threshold = 0.0588f, bool p_contiguous = false, bool p_sample_merged = false, SelectOp p_op = SELECT_REPLACE);
	void select_fuzzy(const Vector2i &p_point, float p_threshold = 0.0588f, bool p_sample_merged = false, SelectOp p_op = SELECT_REPLACE);
	void select_mask(const Ref<Image> &p_mask, SelectOp p_op = SELECT_REPLACE);
	void invert_selection();
	void feather_selection(float p_radius);
	void grow_selection(int p_pixels);
	void shrink_selection(int p_pixels);
	Ref<Image> get_selection_mask() const;
	bool is_selection_empty() const { return selection_empty; }
	Rect2i selection_bounds() const;

	// Painting.
	static Dictionary get_default_stroke_settings(PaintTool p_tool);
	Error stroke_begin(int p_layer_id, PaintTool p_tool, const Dictionary &p_settings);
	void stroke_add_point(const Vector2 &p_position, float p_pressure = 1.0f);
	void stroke_end();
	void stroke_cancel();
	bool is_stroke_active() const { return stroke.active; }
	Error stroke_line(int p_layer_id, PaintTool p_tool, const Dictionary &p_settings, const Vector2 &p_from, const Vector2 &p_to);

	// Fills and edits.
	Error fill(int p_layer_id, const Color &p_color);
	Error clear(int p_layer_id);
	Error bucket_fill(int p_layer_id, const Vector2i &p_point, const Color &p_color, float p_threshold = 0.0588f, bool p_contiguous = true, bool p_sample_merged = false);
	Error gradient_fill(int p_layer_id, const Vector2 &p_start, const Vector2 &p_end, const Color &p_color_a, const Color &p_color_b, GradientType p_type = GRADIENT_LINEAR, GradientRepeat p_repeat = REPEAT_NONE);
	Color pick_color(const Vector2i &p_point, bool p_sample_merged = false, int p_radius = 0) const;

	// Transforms.
	Error flip_layer(int p_layer_id, bool p_vertical = false);
	Error rotate_layer(int p_layer_id, int p_degrees);
	Error translate_layer(int p_layer_id, const Vector2i &p_offset);
	Error flip_image(bool p_vertical = false);
	Error rotate_image(int p_degrees);
	Error scale_image(const Vector2i &p_size, Interpolation p_interpolation = INTERPOLATION_CUBIC);
	Error resize_canvas(const Vector2i &p_size, const Vector2i &p_offset = Vector2i());
	Error crop(const Rect2i &p_rect);
	Error crop_to_selection();

	// Operations (filters and colour adjustments) described with the shared
	// operation schema.
	static Array get_native_operations();
	static Array get_operations();
	static Ref<Image> filter_image(const Ref<Image> &p_image, const String &p_operation_id, const Dictionary &p_values);
	Ref<Image> compute_operation(int p_layer_id, const String &p_operation_id, const Dictionary &p_values) const;
	Error apply_operation(int p_layer_id, const String &p_operation_id, const Dictionary &p_values);
	// Commits the canvas-sized result of filter_image() on this layer, computed
	// elsewhere (for example on a worker thread), exactly as apply_operation()
	// would: through the selection and the alpha lock, as one undo step.
	Error apply_filtered_image(int p_layer_id, const Ref<Image> &p_filtered, const String &p_name);

	// Files.
	Error load(const String &p_path);
	Error save_ora(const String &p_path) const;
	Error export_image(const String &p_path, float p_quality = 0.9f) const;
};

VARIANT_ENUM_CAST(TetraImageDocument::LayerMode);
VARIANT_ENUM_CAST(TetraImageDocument::SelectOp);
VARIANT_ENUM_CAST(TetraImageDocument::PaintTool);
VARIANT_ENUM_CAST(TetraImageDocument::GradientType);
VARIANT_ENUM_CAST(TetraImageDocument::GradientRepeat);
VARIANT_ENUM_CAST(TetraImageDocument::Interpolation);
