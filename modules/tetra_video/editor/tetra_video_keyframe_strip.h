#pragma once

#include "scene/gui/control.h"

class TetraVideoProject;

// The keyframes of one timeline clip, drawn against the clip's own time: one
// lane per animated effect parameter with a mark at each keyframe (a square
// for discrete, a diamond for linear, a circle for curved types) and the
// playhead as a line. A click selects a keyframe and asks for a seek to it, a
// drag moves it in time as one undo step, Delete removes it.
class TetraVideoKeyframeStrip : public Control {
	GDCLASS(TetraVideoKeyframeStrip, Control);

	struct Lane {
		int effect = 0;
		String param;
		String label;
		PackedInt32Array frames;
		PackedInt32Array types;
	};

	Ref<TetraVideoProject> project;
	int clip_id = -1;
	int clip_length = 1;
	// Frames from the clip start.
	int playhead = 0;
	Vector<Lane> lanes;
	int selected_effect = -1;
	String selected_param;
	int selected_frame = -1;
	bool dragging = false;
	bool seeking = false;
	bool group_open = false;

	float _lane_height() const;
	float _label_width() const;
	float _frame_to_x(int p_frame) const;
	int _find_lane(int p_effect, const String &p_param) const;
	bool _hit(const Point2 &p_position, int &r_lane, int &r_frame) const;
	void _end_drag();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	virtual Size2 get_minimum_size() const override;

	void set_project(const Ref<TetraVideoProject> &p_project);
	void set_clip(int p_clip_id);
	// Reads the keyframes of the clip again.
	void refresh();
	void set_playhead(int p_frame);
	int get_playhead() const;

	int get_lane_count() const;
	int get_lane_effect(int p_lane) const;
	String get_lane_param(int p_lane) const;
	String get_lane_label(int p_lane) const;
	PackedInt32Array get_lane_frames(int p_lane) const;
	// Where a frame of a lane is drawn, in the control's coordinates.
	Point2 get_keyframe_position(int p_lane, int p_frame) const;
	int get_frame_at(float p_x) const;

	void select_keyframe(int p_lane, int p_frame);
	int get_selected_lane() const;
	int get_selected_frame() const;
	bool delete_selected();

	TetraVideoKeyframeStrip();
};
