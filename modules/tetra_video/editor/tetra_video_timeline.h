#pragma once

#include "core/templates/hash_map.h"
#include "scene/gui/control.h"

class HScrollBar;
class StyleBoxFlat;
class TetraVideoProject;
class TetraVideoThumbnails;
class Texture2D;
class VScrollBar;

// The multitrack timeline: a ruler with the playhead and the markers, one
// header per track and the clips of a TetraVideoProject as blocks that can be
// selected, moved, trimmed and cut. Tracks are listed as in Kdenlive, the top
// video track first and the audio tracks below the video tracks.
//
// The view never changes clips itself. While the mouse drags, it only shows
// where the clip would go; on release it asks the project, whose rules decide.
// A refused edit leaves the clip where it was and is reported by edit_refused.
class TetraVideoTimeline : public Control {
	GDCLASS(TetraVideoTimeline, Control);

public:
	enum Tool {
		TOOL_SELECT,
		TOOL_RAZOR,
	};

private:
	// What the view needs to know about a timeline clip, read once per project revision.
	struct ClipView {
		int id = 0;
		int bin_id = 0;
		int track = 0;
		int position = 0;
		int length = 0;
		int in = 0;
		int link = -1;
		int fade_in = 0;
		int fade_out = 0;
		int effect_count = 0;
		int kind = 0;
		bool on_audio_track = false;
		String name;
		Dictionary bin_clip;
	};

	// A clip drawn where an unfinished drag or drop would put it.
	struct Ghost {
		int track = 0;
		int position = 0;
		int length = 0;
		String name;
	};

	enum DragMode {
		DRAG_NONE,
		DRAG_SEEK,
		// The button is down on a clip but the mouse has not moved far enough to mean a drag.
		DRAG_CLICK,
		DRAG_MOVE,
		DRAG_TRIM_START,
		DRAG_TRIM_END,
	};

	enum ClipEdge {
		EDGE_NONE,
		EDGE_START,
		EDGE_END,
	};

	Ref<TetraVideoProject> project;
	Ref<TetraVideoThumbnails> thumbnails;
	// Filled on demand by _ensure_clips(), also from const queries.
	mutable Vector<ClipView> clips;
	mutable HashMap<int, int> clip_index_by_id;
	mutable uint64_t clips_revision = UINT64_MAX;

	// Pixels per frame.
	double zoom = 4.0;
	int playhead = 0;
	Tool tool = TOOL_SELECT;
	bool snapping = true;
	PackedInt32Array selection;
	int active_track = -1;
	int zone_in = -1;
	int zone_out = -1;

	HScrollBar *h_scroll = nullptr;
	VScrollBar *v_scroll = nullptr;
	bool updating_scroll = false;
	Ref<StyleBoxFlat> clip_style;

	DragMode drag_mode = DRAG_NONE;
	Point2 drag_origin;
	int drag_clip = -1;
	ClipEdge drag_edge = EDGE_NONE;
	// Frame under the mouse when the drag began.
	int drag_grab_frame = 0;
	int drag_delta = 0;
	int drag_target_track = -1;
	int drag_edge_frame = 0;
	bool drag_valid = true;
	PackedInt32Array drag_ids;
	Vector<Ghost> ghosts;
	bool ghosts_valid = true;
	String ghost_hint;
	// Frame of the snap point the drag is held by, or -1.
	int snap_indicator = -1;

	// A drop from the bin or the effect list hovering over the view.
	bool drop_hovering = false;
	int drop_effect_clip = -1;

	bool hovering = false;
	Point2 hover_position;
	String hint_text;
	Point2 hint_position;
	uint64_t hint_serial = 0;

	real_t _header_width() const;
	real_t _ruler_height() const;
	real_t _track_height(int p_track) const;
	real_t _tracks_height() const;
	real_t _track_y(int p_track) const;
	real_t _lanes_width() const;
	real_t _lanes_height() const;
	int _track_at(real_t p_y) const;
	Rect2 _clip_rect(const ClipView &p_clip) const;
	Rect2 _header_button_rect(int p_track, int p_button) const;
	int _hit_clip(const Point2 &p_position, ClipEdge *r_edge = nullptr) const;
	Color _clip_color(const ClipView &p_clip) const;

	void _ensure_clips() const;
	const ClipView *_find_clip(int p_id) const;
	void _update_scroll_bars();
	void _layout_scroll_bars();
	void _scroll_changed(double p_value);
	void _project_changed();
	void _thumbnail_ready();

	int _snap_frame(int p_frame, const PackedInt32Array &p_ignore, bool *r_snapped = nullptr) const;
	int _snap_move(int p_start, int p_length, const PackedInt32Array &p_ignore, int &r_indicator) const;
	void _begin_drag();
	void _update_move(const Point2 &p_position);
	void _update_trim(const Point2 &p_position);
	void _finish_drag();
	void _cancel_drag();
	void _click_clip(int p_id, bool p_add);
	void _razor_at(const Point2 &p_position);
	void _header_pressed(const Point2 &p_position);
	void _refuse(const String &p_message);
	void _show_hint(const String &p_text, const Point2 &p_position);
	void _hint_timeout(uint64_t p_serial);
	// Works out where the bin clips dragged to p_position would go. Returns whether the drop is possible; when it is
	// not, r_reason says why. The ghosts are filled in either way.
	bool _evaluate_bin_drop(const Point2 &p_position, const PackedInt32Array &p_bin_ids, Vector<Ghost> &r_ghosts, Vector<int> &r_bin_ids, Vector<int> &r_positions, int &r_track, int &r_indicator, String &r_reason) const;
	void _clear_drop_feedback();

	void _draw_timeline();
	void _draw_clip(const Rect2 &p_rect, const Color &p_color, const String &p_name, bool p_selected, const Ref<Texture2D> &p_thumbnail, real_t p_alpha);
	void _draw_hint(const String &p_text, const Point2 &p_position);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The "type" of the drag data of an effect: { type, operation_id }.
	static const char *EFFECT_DRAG_TYPE;

	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	virtual bool can_drop_data(const Point2 &p_point, const Variant &p_data) const override;
	virtual void drop_data(const Point2 &p_point, const Variant &p_data) override;
	virtual CursorShape get_cursor_shape(const Point2 &p_pos = Point2()) const override;
	virtual String get_tooltip(const Point2 &p_pos) const override;

	void set_project(const Ref<TetraVideoProject> &p_project);
	Ref<TetraVideoProject> get_project() const;
	void set_thumbnails(const Ref<TetraVideoThumbnails> &p_thumbnails);

	// Moves the playhead without asking for a seek. With p_follow the view scrolls to keep it visible.
	void set_playhead(int p_frame, bool p_follow = false);
	int get_playhead() const;

	void set_zoom(double p_pixels_per_frame);
	// Zooms keeping the frame under the given view x in place.
	void set_zoom_at(double p_pixels_per_frame, real_t p_view_x);
	double get_zoom() const;
	double get_zoom_min() const;
	double get_zoom_max() const;
	void zoom_to_fit();
	void set_scroll_frame(int p_frame);
	int get_scroll_frame() const;

	void set_tool(Tool p_tool);
	Tool get_tool() const;
	void set_snapping(bool p_enabled);
	bool is_snapping() const;

	void set_selection(const PackedInt32Array &p_clip_ids);
	PackedInt32Array get_selection() const;
	// Selects a clip and its linked partner; with p_add the selection is extended instead of replaced.
	void select_clip(int p_clip_id, bool p_add = false);
	void clear_selection();
	void set_active_track(int p_track);
	int get_active_track() const;

	// Frames of the render zone, inclusive; -1 when unset.
	void set_zone(int p_in, int p_out);
	int get_zone_in() const;
	int get_zone_out() const;

	// Geometry in the view's own coordinates, for tools and tests that click.
	real_t frame_to_x(int p_frame) const;
	int x_to_frame(real_t p_x) const;
	Rect2 get_clip_rect(int p_clip_id) const;
	Rect2 get_track_rect(int p_track) const;
	Rect2 get_ruler_rect() const;
	Rect2 get_track_header_button_rect(int p_track, int p_button) const;

	TetraVideoTimeline();
};

VARIANT_ENUM_CAST(TetraVideoTimeline::Tool);
