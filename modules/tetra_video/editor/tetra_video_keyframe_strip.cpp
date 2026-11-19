#include "tetra_video_keyframe_strip.h"

#include "../tetra_video_assets.h"
#include "../tetra_video_project.h"

#include "core/input/input_event.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/font.h"

float TetraVideoKeyframeStrip::_lane_height() const {
	return Math::round(20 * EDSCALE);
}

float TetraVideoKeyframeStrip::_label_width() const {
	return Math::round(MIN(110 * EDSCALE, get_size().x * 0.4));
}

float TetraVideoKeyframeStrip::_frame_to_x(int p_frame) const {
	const float pad = Math::round(8 * EDSCALE);
	const float left = _label_width() + pad;
	const float width = MAX(1.0f, get_size().x - left - pad);
	return left + (clip_length > 1 ? width * float(p_frame) / float(clip_length - 1) : 0.0f);
}

int TetraVideoKeyframeStrip::get_frame_at(float p_x) const {
	const float pad = Math::round(8 * EDSCALE);
	const float left = _label_width() + pad;
	const float width = MAX(1.0f, get_size().x - left - pad);
	return CLAMP(int(Math::round((p_x - left) / width * float(clip_length - 1))), 0, clip_length - 1);
}

int TetraVideoKeyframeStrip::_find_lane(int p_effect, const String &p_param) const {
	for (int i = 0; i < lanes.size(); i++) {
		if (lanes[i].effect == p_effect && lanes[i].param == p_param) {
			return i;
		}
	}
	return -1;
}

// The keyframe mark under a position, the nearest one within reach.
bool TetraVideoKeyframeStrip::_hit(const Point2 &p_position, int &r_lane, int &r_frame) const {
	const int lane = int(Math::floor(p_position.y / _lane_height()));
	if (lane < 0 || lane >= lanes.size() || p_position.x < _label_width()) {
		return false;
	}
	float best = 7 * EDSCALE;
	bool found = false;
	for (int frame : lanes[lane].frames) {
		const float distance = Math::abs(_frame_to_x(frame) - p_position.x);
		if (distance <= best) {
			best = distance;
			r_lane = lane;
			r_frame = frame;
			found = true;
		}
	}
	return found;
}

void TetraVideoKeyframeStrip::_end_drag() {
	dragging = false;
	seeking = false;
	if (group_open) {
		group_open = false;
		if (project.is_valid()) {
			project->end_undo_group();
		}
	}
}

void TetraVideoKeyframeStrip::gui_input(const Ref<InputEvent> &p_event) {
	if (project.is_null() || clip_id < 0) {
		return;
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT) {
		if (button->is_pressed()) {
			grab_focus();
			int lane = -1;
			int frame = -1;
			if (_hit(button->get_position(), lane, frame)) {
				select_keyframe(lane, frame);
				dragging = true;
				emit_signal(SNAME("seek_requested"), frame);
			} else if (button->get_position().x >= _label_width()) {
				select_keyframe(-1, -1);
				seeking = true;
				emit_signal(SNAME("seek_requested"), get_frame_at(button->get_position().x));
			}
		} else {
			_end_drag();
		}
		accept_event();
		return;
	}
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && seeking) {
		emit_signal(SNAME("seek_requested"), get_frame_at(motion->get_position().x));
		accept_event();
		return;
	}
	if (motion.is_valid() && dragging && selected_frame >= 0) {
		const int target = get_frame_at(motion->get_position().x);
		const int lane = get_selected_lane();
		if (lane >= 0 && target != selected_frame && !lanes[lane].frames.has(target)) {
			if (!group_open) {
				emit_signal(SNAME("edit_started"));
				project->begin_undo_group(TTR("Move Keyframe"));
				group_open = true;
			}
			const int from = selected_frame;
			// Set first: the project's change signal redraws with the new selection.
			selected_frame = target;
			if (!project->move_effect_keyframe(clip_id, selected_effect, selected_param, from, target)) {
				selected_frame = from;
			} else {
				emit_signal(SNAME("seek_requested"), target);
			}
		}
		accept_event();
		return;
	}
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo() && (key->get_keycode() == Key::KEY_DELETE || key->get_keycode() == Key::BACKSPACE)) {
		if (delete_selected()) {
			accept_event();
		}
	}
}

Size2 TetraVideoKeyframeStrip::get_minimum_size() const {
	return Size2(120 * EDSCALE, _lane_height() * MAX(1, lanes.size()) + Math::round(2 * EDSCALE));
}

void TetraVideoKeyframeStrip::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			const Size2 size = get_size();
			const float lane_height = _lane_height();
			const float label_width = _label_width();
			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = MAX(8, get_theme_font_size(SceneStringName(font_size), SNAME("Label")) - 2);
			const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
			const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
			const Color dark = get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor));
			const Color mark_color = Color(0.93, 0.78, 0.3);
			draw_rect(Rect2(Point2(), size), dark);
			const float first_x = _frame_to_x(0);
			const float last_x = _frame_to_x(clip_length - 1);
			for (int i = 0; i < lanes.size(); i++) {
				const Lane &lane = lanes[i];
				const float y = i * lane_height;
				const float middle = Math::round(y + lane_height * 0.5);
				if (i % 2 == 1) {
					draw_rect(Rect2(0, y, size.x, lane_height), Color(1, 1, 1, 0.03));
				}
				draw_string(font, Point2(4 * EDSCALE, y + Math::floor((lane_height - font->get_height(font_size)) * 0.5) + font->get_ascent(font_size)), lane.label, HORIZONTAL_ALIGNMENT_LEFT, label_width - 8 * EDSCALE, font_size, Color(font_color.r, font_color.g, font_color.b, 0.85));
				draw_line(Point2(first_x, middle), Point2(last_x, middle), Color(1, 1, 1, 0.18), Math::round(EDSCALE));
				const float r = Math::round(5 * EDSCALE);
				for (int k = 0; k < lane.frames.size(); k++) {
					const Point2 at = Point2(Math::round(_frame_to_x(lane.frames[k])), middle);
					const bool selected = lane.effect == selected_effect && lane.param == selected_param && lane.frames[k] == selected_frame;
					const Color color = selected ? accent.lightened(0.3) : mark_color;
					const int type = lane.types[k];
					if (type == TetraVideoProject::KEYFRAME_DISCRETE) {
						draw_rect(Rect2(at - Point2(r - EDSCALE, r - EDSCALE), Size2(2 * (r - EDSCALE), 2 * (r - EDSCALE))), color);
					} else if (type == TetraVideoProject::KEYFRAME_LINEAR) {
						draw_colored_polygon({ at + Point2(0, -r), at + Point2(r, 0), at + Point2(0, r), at + Point2(-r, 0) }, color);
					} else {
						draw_circle(at, r - 0.5 * EDSCALE, color);
					}
					if (selected) {
						draw_arc(at, r + 2 * EDSCALE, 0, Math::TAU, 24, Color(1, 1, 1, 0.9), Math::round(EDSCALE), true);
					}
				}
			}
			draw_line(Point2(label_width, 0), Point2(label_width, size.y), Color(0, 0, 0, 0.4), Math::round(EDSCALE));
			const float playhead_x = Math::round(_frame_to_x(CLAMP(playhead, 0, clip_length - 1)));
			draw_line(Point2(playhead_x, 0), Point2(playhead_x, size.y), Color(0.95, 0.3, 0.25), Math::round(MAX(1.0, 1.5 * EDSCALE)));
			if (has_focus()) {
				draw_rect(Rect2(Point2(), size), Color(accent.r, accent.g, accent.b, 0.6), false, Math::round(EDSCALE));
			}
		} break;
		case NOTIFICATION_FOCUS_ENTER:
		case NOTIFICATION_FOCUS_EXIT:
		case NOTIFICATION_THEME_CHANGED: {
			queue_redraw();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_end_drag();
		} break;
	}
}

void TetraVideoKeyframeStrip::set_project(const Ref<TetraVideoProject> &p_project) {
	_end_drag();
	project = p_project;
	clip_id = -1;
	refresh();
}

void TetraVideoKeyframeStrip::set_clip(int p_clip_id) {
	if (p_clip_id != clip_id) {
		_end_drag();
		clip_id = p_clip_id;
		selected_frame = -1;
	}
	refresh();
}

void TetraVideoKeyframeStrip::refresh() {
	const int old_count = lanes.size();
	lanes.clear();
	clip_length = 1;
	if (project.is_valid() && project->has_clip(clip_id)) {
		clip_length = MAX(1, int(project->get_clip(clip_id)["length"]));
		const TypedArray<Dictionary> effects = project->get_clip_effects(clip_id);
		for (int e = 0; e < effects.size(); e++) {
			const Dictionary effect = effects[e];
			const Dictionary keyframes = effect["keyframes"];
			if (keyframes.is_empty()) {
				continue;
			}
			Dictionary operation = TetraVideoAssets::get_operation(effect["operation"], project->get_width(), project->get_height());
			const Array params = operation.get("params", Array());
			for (const KeyValue<Variant, Variant> &entry : keyframes) {
				Lane lane;
				lane.effect = e;
				lane.param = entry.key;
				lane.label = lane.param;
				for (const Variant &param : params) {
					if (String(Dictionary(param)["name"]) == lane.param) {
						lane.label = Dictionary(param).get("label", lane.param);
					}
				}
				if (effects.size() > 1) {
					lane.label = String(operation.get("label", effect["service"])) + ": " + lane.label;
				}
				const Array keys = entry.value;
				for (const Variant &key : keys) {
					lane.frames.push_back(Dictionary(key)["frame"]);
					lane.types.push_back(Dictionary(key)["type"]);
				}
				lanes.push_back(lane);
			}
		}
	}
	const int lane = _find_lane(selected_effect, selected_param);
	if (lane < 0 || !lanes[lane].frames.has(selected_frame)) {
		selected_frame = -1;
		if (lane < 0) {
			selected_effect = -1;
			selected_param = String();
		}
	}
	if (lanes.size() != old_count) {
		update_minimum_size();
	}
	queue_redraw();
}

void TetraVideoKeyframeStrip::set_playhead(int p_frame) {
	if (p_frame != playhead) {
		playhead = p_frame;
		queue_redraw();
	}
}

int TetraVideoKeyframeStrip::get_playhead() const {
	return playhead;
}

int TetraVideoKeyframeStrip::get_lane_count() const {
	return lanes.size();
}

int TetraVideoKeyframeStrip::get_lane_effect(int p_lane) const {
	ERR_FAIL_INDEX_V(p_lane, lanes.size(), -1);
	return lanes[p_lane].effect;
}

String TetraVideoKeyframeStrip::get_lane_param(int p_lane) const {
	ERR_FAIL_INDEX_V(p_lane, lanes.size(), String());
	return lanes[p_lane].param;
}

String TetraVideoKeyframeStrip::get_lane_label(int p_lane) const {
	ERR_FAIL_INDEX_V(p_lane, lanes.size(), String());
	return lanes[p_lane].label;
}

PackedInt32Array TetraVideoKeyframeStrip::get_lane_frames(int p_lane) const {
	ERR_FAIL_INDEX_V(p_lane, lanes.size(), PackedInt32Array());
	return lanes[p_lane].frames;
}

Point2 TetraVideoKeyframeStrip::get_keyframe_position(int p_lane, int p_frame) const {
	return Point2(Math::round(_frame_to_x(p_frame)), Math::round((p_lane + 0.5) * _lane_height()));
}

void TetraVideoKeyframeStrip::select_keyframe(int p_lane, int p_frame) {
	if (p_lane >= 0 && p_lane < lanes.size() && lanes[p_lane].frames.has(p_frame)) {
		selected_effect = lanes[p_lane].effect;
		selected_param = lanes[p_lane].param;
		selected_frame = p_frame;
		emit_signal(SNAME("keyframe_selected"), selected_effect, selected_param, selected_frame);
	} else {
		selected_frame = -1;
	}
	queue_redraw();
}

int TetraVideoKeyframeStrip::get_selected_lane() const {
	return selected_frame < 0 ? -1 : _find_lane(selected_effect, selected_param);
}

int TetraVideoKeyframeStrip::get_selected_frame() const {
	return selected_frame;
}

bool TetraVideoKeyframeStrip::delete_selected() {
	if (project.is_null() || get_selected_lane() < 0) {
		return false;
	}
	_end_drag();
	emit_signal(SNAME("edit_started"));
	const int frame = selected_frame;
	selected_frame = -1;
	return project->remove_effect_keyframe(clip_id, selected_effect, selected_param, frame);
}

void TetraVideoKeyframeStrip::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_playhead", "frame"), &TetraVideoKeyframeStrip::set_playhead);
	ClassDB::bind_method(D_METHOD("get_playhead"), &TetraVideoKeyframeStrip::get_playhead);
	ClassDB::bind_method(D_METHOD("refresh"), &TetraVideoKeyframeStrip::refresh);
	ClassDB::bind_method(D_METHOD("get_lane_count"), &TetraVideoKeyframeStrip::get_lane_count);
	ClassDB::bind_method(D_METHOD("get_lane_effect", "lane"), &TetraVideoKeyframeStrip::get_lane_effect);
	ClassDB::bind_method(D_METHOD("get_lane_param", "lane"), &TetraVideoKeyframeStrip::get_lane_param);
	ClassDB::bind_method(D_METHOD("get_lane_label", "lane"), &TetraVideoKeyframeStrip::get_lane_label);
	ClassDB::bind_method(D_METHOD("get_lane_frames", "lane"), &TetraVideoKeyframeStrip::get_lane_frames);
	ClassDB::bind_method(D_METHOD("get_keyframe_position", "lane", "frame"), &TetraVideoKeyframeStrip::get_keyframe_position);
	ClassDB::bind_method(D_METHOD("get_frame_at", "x"), &TetraVideoKeyframeStrip::get_frame_at);
	ClassDB::bind_method(D_METHOD("select_keyframe", "lane", "frame"), &TetraVideoKeyframeStrip::select_keyframe);
	ClassDB::bind_method(D_METHOD("get_selected_lane"), &TetraVideoKeyframeStrip::get_selected_lane);
	ClassDB::bind_method(D_METHOD("get_selected_frame"), &TetraVideoKeyframeStrip::get_selected_frame);
	ClassDB::bind_method(D_METHOD("delete_selected"), &TetraVideoKeyframeStrip::delete_selected);

	// Frames count from the start of the clip.
	ADD_SIGNAL(MethodInfo("seek_requested", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("keyframe_selected", PropertyInfo(Variant::INT, "effect"), PropertyInfo(Variant::STRING, "param"), PropertyInfo(Variant::INT, "frame")));
	// Emitted before the strip changes the project, so that a pending run of other edits can be closed.
	ADD_SIGNAL(MethodInfo("edit_started"));
}

TetraVideoKeyframeStrip::TetraVideoKeyframeStrip() {
	set_focus_mode(FOCUS_ALL);
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_tooltip_text(TTR("Keyframes of the clip.\nClick a keyframe to select it and go there, drag to move it in time, Delete removes it."));
}
