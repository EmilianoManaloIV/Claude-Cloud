#include "tetra_video_timeline.h"

#include "../tetra_video_project.h"
#include "tetra_video_bin.h"
#include "tetra_video_thumbnails.h"
#include "tetra_video_timecode.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/scroll_bar.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/font.h"
#include "scene/resources/style_box_flat.h"

const char *TetraVideoTimeline::EFFECT_DRAG_TYPE = "tetra_video_effect";

static const Color VIDEO_TRACK_COLOR(0.33, 0.55, 0.82);
static const Color AUDIO_TRACK_COLOR(0.30, 0.68, 0.48);
static const Color PLAYHEAD_COLOR(1.0, 0.42, 0.32);
static const Color MARKER_COLOR(0.96, 0.80, 0.26);
static const Color REFUSED_COLOR(0.92, 0.30, 0.28);

real_t TetraVideoTimeline::_header_width() const {
	return Math::round(132 * EDSCALE);
}

real_t TetraVideoTimeline::_ruler_height() const {
	return Math::round(26 * EDSCALE);
}

real_t TetraVideoTimeline::_track_height(int p_track) const {
	const bool audio = project.is_valid() && p_track >= 0 && p_track < project->get_track_count() && project->get_track_kind(p_track) == TetraVideoProject::TRACK_AUDIO;
	return Math::round((audio ? 34 : 44) * EDSCALE);
}

real_t TetraVideoTimeline::_tracks_height() const {
	real_t height = 0;
	const int count = project.is_valid() ? project->get_track_count() : 0;
	for (int i = 0; i < count; i++) {
		height += _track_height(i);
	}
	return height;
}

real_t TetraVideoTimeline::_track_y(int p_track) const {
	real_t y = _ruler_height() - v_scroll->get_value();
	const int count = project.is_valid() ? project->get_track_count() : 0;
	for (int i = count - 1; i > p_track; i--) {
		y += _track_height(i);
	}
	return y;
}

real_t TetraVideoTimeline::_lanes_width() const {
	return MAX(real_t(1), get_size().x - _header_width() - (v_scroll->is_visible() ? v_scroll->get_combined_minimum_size().x : 0));
}

real_t TetraVideoTimeline::_lanes_height() const {
	return MAX(real_t(1), get_size().y - _ruler_height() - h_scroll->get_combined_minimum_size().y);
}

int TetraVideoTimeline::_track_at(real_t p_y) const {
	if (project.is_null() || p_y < _ruler_height()) {
		return -1;
	}
	real_t y = _ruler_height() - v_scroll->get_value();
	for (int i = project->get_track_count() - 1; i >= 0; i--) {
		const real_t height = _track_height(i);
		if (p_y >= y && p_y < y + height) {
			return i;
		}
		y += height;
	}
	return -1;
}

Rect2 TetraVideoTimeline::_clip_rect(const ClipView &p_clip) const {
	const real_t margin = Math::round(2 * EDSCALE);
	return Rect2(frame_to_x(p_clip.position), _track_y(p_clip.track) + margin, MAX(real_t(2), p_clip.length * zoom), _track_height(p_clip.track) - margin * 2 - 1);
}

Rect2 TetraVideoTimeline::_header_button_rect(int p_track, int p_button) const {
	const real_t icon = Math::round(16 * EDSCALE);
	const real_t gap = Math::round(8 * EDSCALE);
	const real_t x = _header_width() - gap - icon - (1 - p_button) * (icon + gap);
	const real_t y = _track_y(p_track) + Math::floor((_track_height(p_track) - icon) * 0.5);
	return Rect2(x, y, icon, icon);
}

int TetraVideoTimeline::_hit_clip(const Point2 &p_position, ClipEdge *r_edge) const {
	if (r_edge) {
		*r_edge = EDGE_NONE;
	}
	if (p_position.x < _header_width() || p_position.y < _ruler_height()) {
		return -1;
	}
	_ensure_clips();
	for (const ClipView &clip : clips) {
		const Rect2 rect = _clip_rect(clip);
		if (!rect.has_point(p_position)) {
			continue;
		}
		if (r_edge) {
			const real_t grip = MIN(real_t(7 * EDSCALE), rect.size.x * real_t(0.25));
			if (p_position.x - rect.position.x <= grip) {
				*r_edge = EDGE_START;
			} else if (rect.position.x + rect.size.x - p_position.x <= grip) {
				*r_edge = EDGE_END;
			}
		}
		return clip.id;
	}
	return -1;
}

Color TetraVideoTimeline::_clip_color(const ClipView &p_clip) const {
	if (p_clip.on_audio_track) {
		return Color(0.20, 0.47, 0.36);
	}
	switch (p_clip.kind) {
		case TetraVideoProject::CLIP_IMAGE:
			return Color(0.47, 0.35, 0.60);
		case TetraVideoProject::CLIP_COLOR:
			return Color(0.34, 0.36, 0.44);
		case TetraVideoProject::CLIP_TEXT:
			return Color(0.62, 0.43, 0.22);
		default:
			return Color(0.23, 0.40, 0.60);
	}
}

void TetraVideoTimeline::_ensure_clips() const {
	if (project.is_null()) {
		clips.clear();
		clip_index_by_id.clear();
		clips_revision = UINT64_MAX;
		return;
	}
	if (clips_revision == project->get_revision()) {
		return;
	}
	clips_revision = project->get_revision();
	clips.clear();
	clip_index_by_id.clear();
	HashMap<int, Dictionary> bin_clips;
	const PackedInt32Array ids = project->get_clip_ids();
	for (int i = 0; i < ids.size(); i++) {
		const Dictionary payload = project->get_clip(ids[i]);
		ClipView clip;
		clip.id = payload["id"];
		clip.bin_id = payload["bin_id"];
		clip.track = payload["track"];
		clip.position = payload["position"];
		clip.length = payload["length"];
		clip.in = payload["in"];
		clip.link = int(payload["link"]) > 0 ? int(payload["link"]) : -1;
		clip.fade_in = payload["fade_in"];
		clip.fade_out = payload["fade_out"];
		clip.effect_count = Array(payload["effects"]).size();
		if (!bin_clips.has(clip.bin_id)) {
			bin_clips[clip.bin_id] = project->get_bin_clip(clip.bin_id);
		}
		clip.bin_clip = bin_clips[clip.bin_id];
		clip.name = clip.bin_clip.get("name", String());
		clip.kind = clip.bin_clip.get("kind", 0);
		clip.on_audio_track = project->get_track_kind(clip.track) == TetraVideoProject::TRACK_AUDIO;
		clip_index_by_id[clip.id] = clips.size();
		clips.push_back(clip);
	}
}

const TetraVideoTimeline::ClipView *TetraVideoTimeline::_find_clip(int p_id) const {
	_ensure_clips();
	const HashMap<int, int>::ConstIterator found = clip_index_by_id.find(p_id);
	return found ? &clips[found->value] : nullptr;
}

void TetraVideoTimeline::_update_scroll_bars() {
	updating_scroll = true;
	const real_t lanes_height = _lanes_height();
	const real_t tracks_height = _tracks_height();
	v_scroll->set_visible(tracks_height > lanes_height);
	v_scroll->set_max(MAX(tracks_height, lanes_height));
	v_scroll->set_page(lanes_height);

	// Room to drop clips after the last one: the content always ends more than half a view past the last frame.
	const real_t lanes_width = _lanes_width();
	const int duration = project.is_valid() ? project->get_duration() : 0;
	h_scroll->set_max(MAX(duration, playhead) * zoom + lanes_width * 0.6);
	h_scroll->set_page(lanes_width);
	updating_scroll = false;
	_layout_scroll_bars();
}

void TetraVideoTimeline::_layout_scroll_bars() {
	const Size2 size = get_size();
	const Size2 h_min = h_scroll->get_combined_minimum_size();
	const Size2 v_min = v_scroll->get_combined_minimum_size();
	const real_t right = v_scroll->is_visible() ? v_min.x : 0;
	h_scroll->set_position(Point2(_header_width(), size.y - h_min.y));
	h_scroll->set_size(Size2(MAX(real_t(0), size.x - _header_width() - right), h_min.y));
	v_scroll->set_position(Point2(size.x - v_min.x, _ruler_height()));
	v_scroll->set_size(Size2(v_min.x, MAX(real_t(0), size.y - _ruler_height() - h_min.y)));
}

void TetraVideoTimeline::_scroll_changed(double p_value) {
	if (!updating_scroll) {
		queue_redraw();
	}
}

void TetraVideoTimeline::_project_changed() {
	// Clips that no longer exist leave the selection.
	PackedInt32Array kept;
	for (int i = 0; i < selection.size(); i++) {
		if (project.is_valid() && project->has_clip(selection[i])) {
			kept.push_back(selection[i]);
		}
	}
	if (kept.size() != selection.size()) {
		selection = kept;
		emit_signal(SNAME("selection_changed"));
	}
	if (project.is_valid() && active_track >= project->get_track_count()) {
		active_track = -1;
	}
	if (drag_mode != DRAG_NONE && drag_mode != DRAG_SEEK) {
		_cancel_drag();
	}
	_update_scroll_bars();
	queue_redraw();
}

void TetraVideoTimeline::_thumbnail_ready() {
	queue_redraw();
}

int TetraVideoTimeline::_snap_frame(int p_frame, const PackedInt32Array &p_ignore, bool *r_snapped) const {
	if (r_snapped) {
		*r_snapped = false;
	}
	if (!snapping || project.is_null()) {
		return p_frame;
	}
	const int tolerance = MAX(1, int(Math::round(10.0 * EDSCALE / zoom)));
	int best = project->find_snap(p_frame, tolerance, p_ignore);
	// The playhead belongs to the view, so the document does not list it among its snap points.
	if (Math::abs(playhead - p_frame) <= tolerance && (best < 0 || Math::abs(playhead - p_frame) < Math::abs(best - p_frame))) {
		best = playhead;
	}
	if (best < 0) {
		return p_frame;
	}
	if (r_snapped) {
		*r_snapped = true;
	}
	return best;
}

int TetraVideoTimeline::_snap_move(int p_start, int p_length, const PackedInt32Array &p_ignore, int &r_indicator) const {
	r_indicator = -1;
	bool start_snapped = false;
	bool end_snapped = false;
	const int start = _snap_frame(p_start, p_ignore, &start_snapped);
	const int end = _snap_frame(p_start + p_length, p_ignore, &end_snapped);
	if (start_snapped && (!end_snapped || Math::abs(start - p_start) <= Math::abs(end - p_start - p_length))) {
		r_indicator = start;
		return start;
	}
	if (end_snapped && end - p_length >= 0) {
		r_indicator = end;
		return end - p_length;
	}
	return p_start;
}

void TetraVideoTimeline::_begin_drag() {
	const ClipView *clip = _find_clip(drag_clip);
	if (!clip) {
		drag_mode = DRAG_NONE;
		return;
	}
	if (project->is_track_locked(clip->track)) {
		drag_mode = DRAG_NONE;
		_refuse(vformat(TTR("Track %s is locked."), project->get_track_name(clip->track)));
		return;
	}
	drag_ids.clear();
	if (drag_edge != EDGE_NONE) {
		drag_mode = drag_edge == EDGE_START ? DRAG_TRIM_START : DRAG_TRIM_END;
		drag_ids.push_back(clip->id);
		if (clip->link > 0) {
			drag_ids.push_back(clip->link);
		}
	} else {
		drag_mode = DRAG_MOVE;
		// The whole selection moves, and every clip takes its linked partner along.
		PackedInt32Array moving = selection;
		if (!moving.has(clip->id)) {
			moving.clear();
			moving.push_back(clip->id);
		}
		for (int i = 0; i < moving.size(); i++) {
			const ClipView *member = _find_clip(moving[i]);
			if (!member) {
				continue;
			}
			if (!drag_ids.has(member->id)) {
				drag_ids.push_back(member->id);
			}
			if (member->link > 0 && !drag_ids.has(member->link)) {
				drag_ids.push_back(member->link);
			}
		}
	}
	drag_delta = 0;
	drag_target_track = clip->track;
	drag_edge_frame = drag_edge == EDGE_START ? clip->position : clip->position + clip->length;
}

void TetraVideoTimeline::_update_move(const Point2 &p_position) {
	const ClipView *primary = _find_clip(drag_clip);
	if (!primary) {
		_cancel_drag();
		return;
	}
	int lowest_start = primary->position;
	for (int i = 0; i < drag_ids.size(); i++) {
		const ClipView *member = _find_clip(drag_ids[i]);
		if (member) {
			lowest_start = MIN(lowest_start, member->position);
		}
	}
	const int mouse_frame = int(Math::floor((p_position.x - _header_width() + h_scroll->get_value()) / zoom));
	int delta = MAX(mouse_frame - drag_grab_frame, -lowest_start);
	delta = MAX(_snap_move(primary->position + delta, primary->length, drag_ids, snap_indicator) - primary->position, -lowest_start);

	// Only one clip (with its partner) can change track; a group keeps its tracks.
	const bool single = drag_ids.size() == 1 || (drag_ids.size() == 2 && primary->link > 0 && drag_ids.has(primary->link));
	int target_track = primary->track;
	if (single) {
		const int hovered = _track_at(p_position.y);
		if (hovered >= 0 && project->get_track_kind(hovered) == project->get_track_kind(primary->track)) {
			target_track = hovered;
		}
	}
	drag_delta = delta;
	drag_target_track = target_track;

	ghosts.clear();
	ghosts_valid = true;
	ghost_hint = String();
	for (int i = 0; i < drag_ids.size(); i++) {
		const ClipView *member = _find_clip(drag_ids[i]);
		if (!member) {
			continue;
		}
		Ghost ghost;
		ghost.track = member->track;
		ghost.position = member->position + delta;
		ghost.length = member->length;
		ghost.name = member->name;
		if (single && target_track != primary->track) {
			ghost.track = member->id == primary->id ? target_track : project->get_paired_track(target_track);
		}
		if (ghost.track < 0) {
			ghosts_valid = false;
			ghost_hint = TTR("There is no paired track for the linked clip.");
			ghost.track = member->track;
		} else if (project->is_track_locked(ghost.track)) {
			ghosts_valid = false;
			ghost_hint = vformat(TTR("Track %s is locked."), project->get_track_name(ghost.track));
		} else if (!project->is_range_free(ghost.track, ghost.position, ghost.position + ghost.length, drag_ids)) {
			ghosts_valid = false;
			ghost_hint = TTR("Clips cannot overlap.");
		}
		ghosts.push_back(ghost);
	}
	queue_redraw();
}

void TetraVideoTimeline::_update_trim(const Point2 &p_position) {
	const ClipView *clip = _find_clip(drag_clip);
	if (!clip) {
		_cancel_drag();
		return;
	}
	bool snapped = false;
	int frame = _snap_frame(x_to_frame(p_position.x), drag_ids, &snapped);
	const int end = clip->position + clip->length;
	if (drag_mode == DRAG_TRIM_START) {
		frame = CLAMP(frame, 0, end - 1);
	} else {
		frame = MAX(frame, clip->position + 1);
	}
	snap_indicator = snapped ? frame : -1;
	drag_edge_frame = frame;

	ghosts.clear();
	ghosts_valid = true;
	ghost_hint = String();
	for (int i = 0; i < drag_ids.size(); i++) {
		const ClipView *member = _find_clip(drag_ids[i]);
		// A partner with other edges is trimmed by the project's rules; only an aligned one is previewed.
		if (!member || member->position != clip->position || member->length != clip->length) {
			continue;
		}
		Ghost ghost;
		ghost.track = member->track;
		ghost.name = member->name;
		ghost.position = drag_mode == DRAG_TRIM_START ? frame : clip->position;
		ghost.length = drag_mode == DRAG_TRIM_START ? end - frame : frame - clip->position;
		ghosts.push_back(ghost);
	}
	queue_redraw();
}

void TetraVideoTimeline::_finish_drag() {
	const DragMode mode = drag_mode;
	const PackedInt32Array ids = drag_ids;
	const int delta = drag_delta;
	const int target_track = drag_target_track;
	const int edge_frame = drag_edge_frame;
	const ClipView *found = _find_clip(drag_clip);
	_cancel_drag();
	if (!found || project.is_null() || mode == DRAG_SEEK || mode == DRAG_CLICK || mode == DRAG_NONE) {
		return;
	}
	// The project's change signal rebuilds the clip list, so nothing is read through the pointer after an edit.
	const ClipView primary = *found;

	switch (mode) {
		case DRAG_MOVE: {
			if (delta == 0 && target_track == primary.track) {
				break;
			}
			const bool single = ids.size() == 1 || (ids.size() == 2 && primary.link > 0 && ids.has(primary.link));
			if (single) {
				if (!project->move_clip(primary.id, target_track, primary.position + delta)) {
					_refuse(project->get_last_error());
				}
				break;
			}
			// A group moves clip by clip, leading edge first so that its members never run into each other.
			struct Member {
				int id = 0;
				int track = 0;
				int position = 0;
			};
			Vector<Member> members;
			for (int i = 0; i < ids.size(); i++) {
				const ClipView *clip = _find_clip(ids[i]);
				if (clip) {
					Member member;
					member.id = clip->id;
					member.track = clip->track;
					member.position = clip->position;
					members.push_back(member);
				}
			}
			struct MemberSort {
				bool forward = true;
				bool operator()(const Member &p_a, const Member &p_b) const {
					return forward ? p_a.position > p_b.position : p_a.position < p_b.position;
				}
			};
			MemberSort sorter;
			sorter.forward = delta > 0;
			members.sort_custom<MemberSort>(sorter);

			project->begin_undo_group(TTR("Move Clips"));
			int moved = 0;
			bool refused = false;
			String reason;
			for (; moved < members.size(); moved++) {
				const Member &member = members[moved];
				// Already there when it followed its linked partner.
				if (int(project->get_clip(member.id)["position"]) == member.position + delta) {
					continue;
				}
				if (!project->move_clip(member.id, member.track, member.position + delta)) {
					refused = true;
					reason = project->get_last_error();
					break;
				}
			}
			if (refused) {
				for (int i = moved - 1; i >= 0; i--) {
					if (int(project->get_clip(members[i].id)["position"]) != members[i].position) {
						project->move_clip(members[i].id, members[i].track, members[i].position);
					}
				}
			}
			project->end_undo_group();
			if (refused) {
				_refuse(reason);
			}
		} break;
		case DRAG_TRIM_START: {
			if (edge_frame != primary.position && !project->trim_clip_start(primary.id, edge_frame)) {
				_refuse(project->get_last_error());
			}
		} break;
		case DRAG_TRIM_END: {
			if (edge_frame != primary.position + primary.length && !project->trim_clip_end(primary.id, edge_frame)) {
				_refuse(project->get_last_error());
			}
		} break;
		default:
			break;
	}
}

void TetraVideoTimeline::_cancel_drag() {
	drag_mode = DRAG_NONE;
	drag_ids.clear();
	ghosts.clear();
	ghosts_valid = true;
	ghost_hint = String();
	snap_indicator = -1;
	queue_redraw();
}

void TetraVideoTimeline::_click_clip(int p_id, bool p_add) {
	const ClipView *clip = _find_clip(p_id);
	if (!clip) {
		return;
	}
	PackedInt32Array next;
	if (p_add) {
		next = selection;
		if (next.has(p_id)) {
			next.erase(p_id);
			if (clip->link > 0) {
				next.erase(clip->link);
			}
			set_selection(next);
			return;
		}
	}
	// The clicked clip goes first: it is the one the effect stack shows.
	PackedInt32Array ordered;
	ordered.push_back(p_id);
	if (clip->link > 0) {
		ordered.push_back(clip->link);
	}
	for (int i = 0; i < next.size(); i++) {
		if (!ordered.has(next[i])) {
			ordered.push_back(next[i]);
		}
	}
	set_selection(ordered);
}

void TetraVideoTimeline::_razor_at(const Point2 &p_position) {
	const int hit = _hit_clip(p_position);
	const ClipView *clip = _find_clip(hit);
	if (!clip) {
		return;
	}
	if (project->is_track_locked(clip->track)) {
		_refuse(vformat(TTR("Track %s is locked."), project->get_track_name(clip->track)));
		return;
	}
	if (clip->length < 2) {
		_refuse(TTR("A clip of one frame cannot be cut."));
		return;
	}
	const int frame = CLAMP(_snap_frame(x_to_frame(p_position.x), PackedInt32Array()), clip->position + 1, clip->position + clip->length - 1);
	if (project->split_clip(hit, frame) < 0) {
		_refuse(project->get_last_error());
	}
}

void TetraVideoTimeline::_header_pressed(const Point2 &p_position) {
	const int track = _track_at(p_position.y);
	if (track < 0) {
		return;
	}
	const real_t reach = Math::round(4 * EDSCALE);
	if (_header_button_rect(track, 0).grow(reach).has_point(p_position)) {
		if (project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO) {
			project->set_track_muted(track, !project->is_track_muted(track));
		} else {
			project->set_track_hidden(track, !project->is_track_hidden(track));
		}
	} else if (_header_button_rect(track, 1).grow(reach).has_point(p_position)) {
		project->set_track_locked(track, !project->is_track_locked(track));
	} else {
		set_active_track(track);
	}
}

void TetraVideoTimeline::_refuse(const String &p_message) {
	const String message = p_message.is_empty() ? TTR("This edit is not possible here.") : p_message;
	emit_signal(SNAME("edit_refused"), message);
	_show_hint(message, hover_position);
}

void TetraVideoTimeline::_show_hint(const String &p_text, const Point2 &p_position) {
	hint_text = p_text;
	hint_position = p_position;
	hint_serial++;
	if (is_inside_tree()) {
		get_tree()->create_timer(3.0)->connect("timeout", callable_mp(this, &TetraVideoTimeline::_hint_timeout).bind(hint_serial));
	}
	queue_redraw();
}

void TetraVideoTimeline::_hint_timeout(uint64_t p_serial) {
	if (p_serial == hint_serial) {
		hint_text = String();
		queue_redraw();
	}
}

bool TetraVideoTimeline::_evaluate_bin_drop(const Point2 &p_position, const PackedInt32Array &p_bin_ids, Vector<Ghost> &r_ghosts, Vector<int> &r_bin_ids, Vector<int> &r_positions, int &r_track, int &r_indicator, String &r_reason) const {
	r_indicator = -1;
	r_track = -1;
	if (project.is_null()) {
		return false;
	}
	const int track = _track_at(p_position.y);
	if (track < 0 || p_position.x < _header_width()) {
		r_reason = TTR("Drop the clip on a track.");
		return false;
	}
	r_track = track;
	int total_length = 0;
	for (int i = 0; i < p_bin_ids.size(); i++) {
		if (project->has_bin_clip(p_bin_ids[i])) {
			total_length += int(project->get_bin_clip(p_bin_ids[i])["length"]);
		}
	}
	int cursor = MAX(0, int(Math::floor((p_position.x - _header_width() + h_scroll->get_value()) / zoom)));
	cursor = MAX(0, _snap_move(cursor, total_length, PackedInt32Array(), r_indicator));

	const bool audio_track = project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO;
	const int partner = project->get_paired_track(track);
	for (int i = 0; i < p_bin_ids.size(); i++) {
		if (!project->has_bin_clip(p_bin_ids[i])) {
			continue;
		}
		const Dictionary bin = project->get_bin_clip(p_bin_ids[i]);
		const bool with_video = bin["has_video"];
		const bool with_audio = bin["has_audio"];
		Ghost ghost;
		ghost.track = track;
		ghost.position = cursor;
		ghost.length = bin["length"];
		ghost.name = bin["name"];
		r_ghosts.push_back(ghost);
		if (audio_track && !with_audio) {
			r_reason = TTR("This clip has no sound. Drop it on a video track.");
		} else if (!audio_track && !with_video) {
			r_reason = TTR("This clip has no picture. Drop it on an audio track.");
		} else if (project->is_track_locked(track)) {
			r_reason = vformat(TTR("Track %s is locked."), project->get_track_name(track));
		} else if (!project->is_range_free(track, cursor, cursor + ghost.length)) {
			r_reason = TTR("There is not enough room here.");
		} else if (with_video && with_audio && partner >= 0) {
			// The other half of an AV clip lands on the paired track and needs the same room there.
			Ghost other = ghost;
			other.track = partner;
			r_ghosts.push_back(other);
			if (project->is_track_locked(partner)) {
				r_reason = vformat(TTR("The paired track %s is locked."), project->get_track_name(partner));
			} else if (!project->is_range_free(partner, cursor, cursor + ghost.length)) {
				r_reason = vformat(TTR("The paired track %s has no room here."), project->get_track_name(partner));
			}
		}
		r_bin_ids.push_back(p_bin_ids[i]);
		r_positions.push_back(cursor);
		cursor += ghost.length;
	}
	return r_reason.is_empty() && !r_bin_ids.is_empty();
}

void TetraVideoTimeline::_clear_drop_feedback() {
	if (drop_hovering) {
		drop_hovering = false;
		drop_effect_clip = -1;
		ghosts.clear();
		ghosts_valid = true;
		ghost_hint = String();
		snap_indicator = -1;
		queue_redraw();
	}
}

void TetraVideoTimeline::_draw_clip(const Rect2 &p_rect, const Color &p_color, const String &p_name, bool p_selected, const Ref<Texture2D> &p_thumbnail, real_t p_alpha) {
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	Color fill = p_color;
	fill.a = p_alpha;
	clip_style->set_bg_color(fill);
	clip_style->set_border_width_all(p_selected ? MAX(2, int(Math::round(2 * EDSCALE))) : 1);
	Color border = p_selected ? Color(1, 1, 1) : p_color.lightened(0.25);
	border.a = p_alpha;
	clip_style->set_border_color(border);
	draw_style_box(clip_style, p_rect);

	const real_t pad = Math::round(3 * EDSCALE);
	// The label stays in view while the clip's start is scrolled out to the left.
	real_t content_x = MAX(p_rect.position.x, _header_width()) + pad;
	const real_t content_end = p_rect.position.x + p_rect.size.x - pad;
	if (p_thumbnail.is_valid()) {
		const real_t thumb_height = p_rect.size.y - pad * 2;
		const real_t thumb_width = Math::round(thumb_height * p_thumbnail->get_width() / MAX(1, p_thumbnail->get_height()));
		if (p_rect.size.x >= thumb_width + pad * 2) {
			draw_texture_rect(p_thumbnail, Rect2(p_rect.position.x + pad, p_rect.position.y + pad, thumb_width, thumb_height), false, Color(1, 1, 1, p_alpha));
			content_x = MAX(content_x, p_rect.position.x + pad * 2 + thumb_width);
		}
	}
	if (content_end - content_x > 14 * EDSCALE) {
		const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
		const int font_size = MAX(8, get_theme_font_size(SceneStringName(font_size), SNAME("Label")) - 1);
		draw_string(font, Point2(content_x, p_rect.position.y + pad + font->get_ascent(font_size)), p_name, HORIZONTAL_ALIGNMENT_LEFT, content_end - content_x, font_size, Color(1, 1, 1, 0.95 * p_alpha));
	}
	if (p_selected) {
		Color tint = accent;
		tint.a = 0.18;
		draw_rect(p_rect.grow(-1), tint);
	}
}

void TetraVideoTimeline::_draw_hint(const String &p_text, const Point2 &p_position) {
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Size2 text_size = font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
	const real_t pad = Math::round(6 * EDSCALE);
	const Size2 box = text_size + Size2(pad * 2, pad);
	Point2 corner = p_position + Point2(14, 14) * EDSCALE;
	corner.x = CLAMP(corner.x, real_t(2), MAX(real_t(2), get_size().x - box.x - 2));
	corner.y = CLAMP(corner.y, _ruler_height(), MAX(_ruler_height(), get_size().y - box.y - h_scroll->get_size().y - 2));
	draw_rect(Rect2(corner, box), Color(0.12, 0.05, 0.05, 0.94));
	draw_rect(Rect2(corner, box), REFUSED_COLOR, false, 1);
	draw_string(font, corner + Point2(pad, pad * 0.5 + font->get_ascent(font_size)), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 0.9, 0.88));
}

void TetraVideoTimeline::_draw_timeline() {
	_ensure_clips();
	const Size2 size = get_size();
	const real_t header_width = _header_width();
	const real_t ruler_height = _ruler_height();
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const int small_size = MAX(8, font_size - 2);
	const Color base = get_theme_color(SNAME("base_color"), EditorStringName(Editor));
	const Color dark = get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor));
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
	const double fps = project.is_valid() ? project->get_fps() : 25.0;
	const int track_count = project.is_valid() ? project->get_track_count() : 0;

	draw_rect(Rect2(Point2(), size), dark);

	// Lanes.
	for (int track = 0; track < track_count; track++) {
		const real_t y = _track_y(track);
		const real_t height = _track_height(track);
		if (y + height < ruler_height || y > size.y) {
			continue;
		}
		const bool audio = project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO;
		Color lane = audio ? base.lerp(AUDIO_TRACK_COLOR, 0.06) : base.lerp(VIDEO_TRACK_COLOR, 0.06);
		if (track == active_track) {
			lane = lane.lightened(0.05);
		}
		draw_rect(Rect2(header_width, y, size.x - header_width, height - 1), lane);
	}

	// Clips. The ones a drag is moving stay where they are, faded, until the project accepts the move.
	const bool dragging_clips = drag_mode == DRAG_MOVE || drag_mode == DRAG_TRIM_START || drag_mode == DRAG_TRIM_END;
	const real_t aspect = project.is_valid() ? real_t(project->get_width()) / real_t(MAX(1, project->get_height())) : real_t(16.0 / 9.0);
	for (const ClipView &clip : clips) {
		const Rect2 rect = _clip_rect(clip);
		if (rect.position.x + rect.size.x < header_width || rect.position.x > size.x || rect.position.y + rect.size.y < ruler_height || rect.position.y > size.y) {
			continue;
		}
		const bool selected = selection.has(clip.id);
		const real_t alpha = dragging_clips && drag_ids.has(clip.id) ? 0.35 : 1.0;
		Ref<Texture2D> thumbnail;
		if (!clip.on_audio_track && thumbnails.is_valid()) {
			const int thumb_height = int(rect.size.y - Math::round(3 * EDSCALE) * 2);
			thumbnail = thumbnails->get_thumbnail(clip.bin_clip, clip.in, Vector2i(int(Math::round(thumb_height * aspect)), thumb_height));
		}
		_draw_clip(rect, _clip_color(clip), clip.name, selected, thumbnail, alpha);

		// Fades: a darker wedge over the part of the clip that is faded.
		if (clip.fade_in > 0) {
			const real_t width = MIN(real_t(clip.fade_in * zoom), rect.size.x);
			draw_colored_polygon({ rect.position, rect.position + Point2(width, 0), rect.position + Point2(0, rect.size.y) }, Color(0, 0, 0, 0.4));
		}
		if (clip.fade_out > 0) {
			const real_t width = MIN(real_t(clip.fade_out * zoom), rect.size.x);
			const Point2 corner = rect.position + Point2(rect.size.x, 0);
			draw_colored_polygon({ corner, corner + Point2(0, rect.size.y), corner + Point2(-width, 0) }, Color(0, 0, 0, 0.4));
		}
		// A badge for clips that carry effects.
		if (clip.effect_count > 0 && rect.size.x > 46 * EDSCALE) {
			const String badge = clip.effect_count > 1 ? vformat("fx %d", clip.effect_count) : String("fx");
			const Size2 badge_size = font->get_string_size(badge, HORIZONTAL_ALIGNMENT_LEFT, -1, small_size) + Size2(8, 2) * EDSCALE;
			const Rect2 badge_rect(rect.position + Point2(rect.size.x - badge_size.x - 4 * EDSCALE, rect.size.y - badge_size.y - 3 * EDSCALE), badge_size);
			draw_rect(badge_rect, Color(0.08, 0.08, 0.1, 0.78));
			draw_string(font, badge_rect.position + Point2(4 * EDSCALE, EDSCALE + font->get_ascent(small_size)), badge, HORIZONTAL_ALIGNMENT_LEFT, -1, small_size, MARKER_COLOR);
		}
		if (clip.id == drop_effect_clip) {
			draw_rect(rect.grow(1), accent, false, MAX(real_t(2), 2 * EDSCALE));
		}
	}

	// Tracks that do not take part: locked ones are hatched darker, hidden or muted ones dimmed.
	for (int track = 0; track < track_count; track++) {
		const Rect2 lane(header_width, _track_y(track), size.x - header_width, _track_height(track) - 1);
		const bool silent = project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO ? project->is_track_muted(track) : project->is_track_hidden(track);
		if (silent) {
			draw_rect(lane, Color(dark.r, dark.g, dark.b, 0.55));
		}
		if (project->is_track_locked(track)) {
			draw_rect(lane, Color(0, 0, 0, 0.28));
		}
	}

	// What a drag or a drop would do.
	for (const Ghost &ghost : ghosts) {
		ClipView as_clip;
		as_clip.track = ghost.track;
		as_clip.position = ghost.position;
		as_clip.length = MAX(1, ghost.length);
		const Rect2 rect = _clip_rect(as_clip);
		const bool audio = project.is_valid() && ghost.track < track_count && project->get_track_kind(ghost.track) == TetraVideoProject::TRACK_AUDIO;
		const Color color = ghosts_valid ? (audio ? Color(0.20, 0.47, 0.36) : Color(0.23, 0.40, 0.60)) : REFUSED_COLOR.darkened(0.25);
		_draw_clip(rect, color, ghost.name, ghosts_valid, Ref<Texture2D>(), 0.8);
	}

	// Marker and snap lines across the lanes, then the playhead.
	if (project.is_valid()) {
		const TypedArray<Dictionary> markers = project->get_markers();
		for (int i = 0; i < markers.size(); i++) {
			const real_t x = frame_to_x(Dictionary(markers[i])["frame"]);
			if (x >= header_width && x <= size.x) {
				draw_line(Point2(x, ruler_height), Point2(x, size.y), Color(MARKER_COLOR.r, MARKER_COLOR.g, MARKER_COLOR.b, 0.35));
			}
		}
	}
	if (snap_indicator >= 0) {
		const real_t x = frame_to_x(snap_indicator);
		if (x >= header_width) {
			draw_line(Point2(x, ruler_height), Point2(x, size.y), accent, MAX(real_t(1), EDSCALE));
		}
	}
	if (tool == TOOL_RAZOR && hovering && drag_mode == DRAG_NONE && _hit_clip(hover_position) >= 0) {
		const int track = _track_at(hover_position.y);
		const real_t x = frame_to_x(_snap_frame(x_to_frame(hover_position.x), PackedInt32Array()));
		draw_dashed_line(Point2(x, _track_y(track)), Point2(x, _track_y(track) + _track_height(track)), Color(1, 1, 1, 0.9), MAX(real_t(1), EDSCALE), 4 * EDSCALE);
	}
	const real_t playhead_x = frame_to_x(playhead);
	if (playhead_x >= header_width && playhead_x <= size.x) {
		draw_line(Point2(playhead_x, ruler_height), Point2(playhead_x, size.y), PLAYHEAD_COLOR, MAX(real_t(1), EDSCALE));
	}

	// Track headers, over whatever the lanes drew under them.
	draw_rect(Rect2(0, ruler_height, header_width, size.y - ruler_height), dark);
	const Ref<Texture2D> icon_visible = get_editor_theme_icon(SNAME("GuiVisibilityVisible"));
	const Ref<Texture2D> icon_hidden = get_editor_theme_icon(SNAME("GuiVisibilityHidden"));
	const Ref<Texture2D> icon_sound = get_editor_theme_icon(SNAME("AudioStreamPlayer"));
	const Ref<Texture2D> icon_muted = get_editor_theme_icon(SNAME("AudioMute"));
	const Ref<Texture2D> icon_locked = get_editor_theme_icon(SNAME("Lock"));
	const Ref<Texture2D> icon_unlocked = get_editor_theme_icon(SNAME("Unlock"));
	for (int track = 0; track < track_count; track++) {
		const real_t y = _track_y(track);
		const real_t height = _track_height(track);
		if (y + height < ruler_height || y > size.y) {
			continue;
		}
		const bool audio = project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO;
		const Color kind_color = audio ? AUDIO_TRACK_COLOR : VIDEO_TRACK_COLOR;
		draw_rect(Rect2(0, y, header_width, height - 1), track == active_track ? base.lightened(0.10) : base);
		draw_rect(Rect2(0, y, Math::round(4 * EDSCALE), height - 1), kind_color);
		const real_t name_x = Math::round(12 * EDSCALE);
		const real_t name_width = _header_button_rect(track, 0).position.x - name_x - 4 * EDSCALE;
		draw_string(font, Point2(name_x, y + Math::floor((height - font->get_height(font_size)) * 0.5) + font->get_ascent(font_size)), project->get_track_name(track), HORIZONTAL_ALIGNMENT_LEFT, name_width, font_size, font_color);

		const bool off = audio ? project->is_track_muted(track) : project->is_track_hidden(track);
		const Ref<Texture2D> state_icon = audio ? (off ? icon_muted : icon_sound) : (off ? icon_hidden : icon_visible);
		draw_texture_rect(state_icon, _header_button_rect(track, 0), false, off ? MARKER_COLOR : Color(1, 1, 1, 0.75));
		const bool locked = project->is_track_locked(track);
		draw_texture_rect(locked ? icon_locked : icon_unlocked, _header_button_rect(track, 1), false, locked ? MARKER_COLOR : Color(1, 1, 1, 0.55));
	}

	// Ruler.
	draw_rect(Rect2(0, 0, size.x, ruler_height), base.darkened(0.12));
	const int base_frames = TetraVideoTimecode::frames_per_second(fps);
	const real_t label_width = font->get_string_size("00:00:00:00", HORIZONTAL_ALIGNMENT_LEFT, -1, small_size).x + 28 * EDSCALE;
	static const int SECOND_STEPS[] = { 1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600, 7200, 0 };
	static const int FRAME_STEPS[] = { 1, 2, 5, 10, 0 };
	int step = 0;
	for (int i = 0; FRAME_STEPS[i] && !step; i++) {
		if (FRAME_STEPS[i] < base_frames && FRAME_STEPS[i] * zoom >= label_width) {
			step = FRAME_STEPS[i];
		}
	}
	for (int i = 0; SECOND_STEPS[i] && !step; i++) {
		if (SECOND_STEPS[i] * base_frames * zoom >= label_width) {
			step = SECOND_STEPS[i] * base_frames;
		}
	}
	if (!step) {
		step = 7200 * base_frames;
	}
	// Four or five unlabelled ticks between two labels, where they are a whole number of frames apart.
	int minor = step % 5 == 0 ? step / 5 : (step % 4 == 0 ? step / 4 : (step % 2 == 0 ? step / 2 : step));
	if (minor * zoom < 4 * EDSCALE) {
		minor = step;
	}
	const int first = MAX(0, int(Math::floor(h_scroll->get_value() / zoom / minor)) * minor);
	const int last = int(Math::ceil((h_scroll->get_value() + size.x - header_width) / zoom));
	for (int frame = first; frame <= last; frame += minor) {
		const real_t x = frame_to_x(frame);
		if (x < header_width) {
			continue;
		}
		const bool major = frame % step == 0;
		draw_line(Point2(x, major ? ruler_height * 0.35 : ruler_height * 0.7), Point2(x, ruler_height), Color(font_color.r, font_color.g, font_color.b, major ? 0.6 : 0.3));
		if (major) {
			draw_string(font, Point2(x + 4 * EDSCALE, Math::round(3 * EDSCALE) + font->get_ascent(small_size)), TetraVideoTimecode::from_frames(frame, fps), HORIZONTAL_ALIGNMENT_LEFT, -1, small_size, Color(font_color.r, font_color.g, font_color.b, 0.8));
		}
	}
	if (zone_in >= 0 && zone_out >= zone_in) {
		const real_t from = MAX(frame_to_x(zone_in), header_width);
		const real_t to = frame_to_x(zone_out + 1);
		if (to > from) {
			draw_rect(Rect2(from, ruler_height - Math::round(5 * EDSCALE), to - from, Math::round(5 * EDSCALE)), Color(accent.r, accent.g, accent.b, 0.75));
		}
	}
	if (project.is_valid()) {
		const TypedArray<Dictionary> markers = project->get_markers();
		for (int i = 0; i < markers.size(); i++) {
			const Dictionary marker = markers[i];
			const real_t x = frame_to_x(marker["frame"]);
			if (x < header_width || x > size.x) {
				continue;
			}
			const real_t flag = Math::round(8 * EDSCALE);
			draw_colored_polygon({ Point2(x, ruler_height), Point2(x - flag * 0.5, ruler_height - flag), Point2(x + flag * 0.5, ruler_height - flag) }, MARKER_COLOR);
			const String text = marker["text"];
			if (!text.is_empty()) {
				draw_string(font, Point2(x + flag * 0.5 + 2 * EDSCALE, ruler_height - 3 * EDSCALE), text, HORIZONTAL_ALIGNMENT_LEFT, 120 * EDSCALE, small_size, MARKER_COLOR);
			}
		}
	}
	if (playhead_x >= header_width && playhead_x <= size.x) {
		const real_t head = Math::round(6 * EDSCALE);
		draw_colored_polygon({ Point2(playhead_x - head, 0), Point2(playhead_x + head, 0), Point2(playhead_x + head, ruler_height * 0.45), Point2(playhead_x, ruler_height * 0.8), Point2(playhead_x - head, ruler_height * 0.45) }, PLAYHEAD_COLOR);
		draw_line(Point2(playhead_x, ruler_height * 0.8), Point2(playhead_x, ruler_height), PLAYHEAD_COLOR, MAX(real_t(1), EDSCALE));
	}

	// The corner above the headers shows where the playhead is.
	draw_rect(Rect2(0, 0, header_width, ruler_height), base.darkened(0.12));
	draw_string(font, Point2(Math::round(12 * EDSCALE), Math::floor((ruler_height - font->get_height(font_size)) * 0.5) + font->get_ascent(font_size)), TetraVideoTimecode::from_frames(playhead, fps), HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, PLAYHEAD_COLOR.lightened(0.2));
	draw_line(Point2(header_width, 0), Point2(header_width, size.y), dark.darkened(0.3));
	draw_line(Point2(0, ruler_height), Point2(size.x, ruler_height), dark.darkened(0.3));

	if (track_count == 0) {
		draw_string(font, Point2(header_width, ruler_height + 30 * EDSCALE), TTR("No tracks. Use Timeline > Add Video Track."), HORIZONTAL_ALIGNMENT_CENTER, size.x - header_width, font_size, Color(font_color.r, font_color.g, font_color.b, 0.5));
	}
	if (!ghost_hint.is_empty() && !ghosts_valid) {
		_draw_hint(ghost_hint, hover_position);
	} else if (!hint_text.is_empty()) {
		_draw_hint(hint_text, hint_position);
	}
	if (has_focus()) {
		draw_rect(Rect2(Point2(), size).grow(-1), Color(accent.r, accent.g, accent.b, 0.35), false, 1);
	}
}

void TetraVideoTimeline::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid()) {
		const Point2 position = button->get_position();
		hover_position = position;
		if (!button->is_pressed()) {
			if (button->get_button_index() == MouseButton::LEFT && drag_mode != DRAG_NONE) {
				_finish_drag();
				accept_event();
			}
			return;
		}
		switch (button->get_button_index()) {
			case MouseButton::WHEEL_UP:
			case MouseButton::WHEEL_DOWN: {
				const bool up = button->get_button_index() == MouseButton::WHEEL_UP;
				if (button->is_command_or_control_pressed()) {
					set_zoom_at(zoom * (up ? 1.25 : 0.8), position.x);
				} else if (button->is_shift_pressed() || !v_scroll->is_visible()) {
					h_scroll->set_value(h_scroll->get_value() + (up ? -1 : 1) * h_scroll->get_page() * 0.12);
				} else {
					v_scroll->set_value(v_scroll->get_value() + (up ? -1 : 1) * _track_height(0));
				}
				accept_event();
			} break;
			case MouseButton::WHEEL_LEFT:
			case MouseButton::WHEEL_RIGHT: {
				h_scroll->set_value(h_scroll->get_value() + (button->get_button_index() == MouseButton::WHEEL_LEFT ? -1 : 1) * h_scroll->get_page() * 0.12);
				accept_event();
			} break;
			case MouseButton::LEFT: {
				grab_focus();
				accept_event();
				emit_signal(SNAME("interaction_started"));
				if (project.is_null()) {
					break;
				}
				if (position.y < _ruler_height()) {
					if (position.x >= _header_width()) {
						drag_mode = DRAG_SEEK;
						emit_signal(SNAME("seek_requested"), x_to_frame(position.x));
					}
					break;
				}
				if (position.x < _header_width()) {
					_header_pressed(position);
					break;
				}
				const int track = _track_at(position.y);
				if (track >= 0) {
					set_active_track(track);
				}
				if (tool == TOOL_RAZOR) {
					_razor_at(position);
					break;
				}
				ClipEdge edge = EDGE_NONE;
				const int hit = _hit_clip(position, &edge);
				if (hit < 0) {
					if (!button->is_shift_pressed()) {
						clear_selection();
					}
				} else if (button->is_shift_pressed()) {
					_click_clip(hit, true);
				} else {
					if (!selection.has(hit)) {
						_click_clip(hit, false);
					}
					if (button->is_double_click()) {
						emit_signal(SNAME("clip_activated"), hit);
					} else {
						drag_mode = DRAG_CLICK;
						drag_origin = position;
						drag_clip = hit;
						drag_edge = edge;
						drag_grab_frame = int(Math::floor((position.x - _header_width() + h_scroll->get_value()) / zoom));
					}
				}
			} break;
			default:
				break;
		}
		return;
	}

	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		hover_position = motion->get_position();
		hovering = true;
		switch (drag_mode) {
			case DRAG_SEEK: {
				emit_signal(SNAME("seek_requested"), x_to_frame(hover_position.x));
			} break;
			case DRAG_CLICK: {
				if (hover_position.distance_to(drag_origin) > 4 * EDSCALE) {
					_begin_drag();
					if (drag_mode == DRAG_MOVE) {
						_update_move(hover_position);
					} else if (drag_mode != DRAG_NONE) {
						_update_trim(hover_position);
					}
				}
			} break;
			case DRAG_MOVE: {
				_update_move(hover_position);
			} break;
			case DRAG_TRIM_START:
			case DRAG_TRIM_END: {
				_update_trim(hover_position);
			} break;
			default: {
				if (tool == TOOL_RAZOR) {
					queue_redraw();
				}
			} break;
		}
		return;
	}

	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == Key::ESCAPE && drag_mode != DRAG_NONE) {
		_cancel_drag();
		accept_event();
	}
}

bool TetraVideoTimeline::can_drop_data(const Point2 &p_point, const Variant &p_data) const {
	if (p_data.get_type() != Variant::DICTIONARY || project.is_null()) {
		return false;
	}
	// The feedback of a hovering drop is view state; this query is the only place that learns where the drop is.
	TetraVideoTimeline *self = const_cast<TetraVideoTimeline *>(this);
	const Dictionary payload = p_data;
	const String type = payload.get("type", String());
	if (type == TetraVideoBin::DRAG_TYPE) {
		Vector<Ghost> next_ghosts;
		Vector<int> bin_ids;
		Vector<int> positions;
		int track = -1;
		int indicator = -1;
		String reason;
		const bool possible = _evaluate_bin_drop(p_point, payload.get("bin_ids", PackedInt32Array()), next_ghosts, bin_ids, positions, track, indicator, reason);
		self->ghosts = next_ghosts;
		self->ghosts_valid = possible;
		self->ghost_hint = reason;
		self->snap_indicator = possible ? indicator : -1;
		self->hover_position = p_point;
		self->drop_hovering = true;
		self->queue_redraw();
		return possible;
	}
	if (type == EFFECT_DRAG_TYPE) {
		self->drop_effect_clip = _hit_clip(p_point);
		self->drop_hovering = true;
		self->queue_redraw();
		return drop_effect_clip >= 0;
	}
	return false;
}

void TetraVideoTimeline::drop_data(const Point2 &p_point, const Variant &p_data) {
	const Dictionary payload = p_data;
	const String type = payload.get("type", String());
	hover_position = p_point;
	if (type == EFFECT_DRAG_TYPE) {
		const int hit = _hit_clip(p_point);
		_clear_drop_feedback();
		if (hit >= 0) {
			grab_focus();
			emit_signal(SNAME("effect_dropped"), hit, String(payload.get("operation_id", String())));
		}
		return;
	}
	if (type != TetraVideoBin::DRAG_TYPE || project.is_null()) {
		return;
	}
	Vector<Ghost> next_ghosts;
	Vector<int> bin_ids;
	Vector<int> positions;
	int track = -1;
	int indicator = -1;
	String reason;
	const bool possible = _evaluate_bin_drop(p_point, payload.get("bin_ids", PackedInt32Array()), next_ghosts, bin_ids, positions, track, indicator, reason);
	drop_hovering = true;
	_clear_drop_feedback();
	if (!possible) {
		_refuse(reason);
		return;
	}
	grab_focus();
	emit_signal(SNAME("interaction_started"));
	PackedInt32Array inserted;
	String refusal;
	project->begin_undo_group(bin_ids.size() > 1 ? TTR("Insert Clips") : TTR("Insert Clip"));
	for (int i = 0; i < bin_ids.size(); i++) {
		const int id = project->insert_clip(bin_ids[i], track, positions[i]);
		if (id < 0) {
			refusal = project->get_last_error();
			break;
		}
		inserted.push_back(id);
		const int partner = project->get_linked_clip(id);
		if (partner > 0) {
			inserted.push_back(partner);
		}
	}
	project->end_undo_group();
	if (!refusal.is_empty()) {
		_refuse(refusal);
	}
	if (!inserted.is_empty()) {
		set_selection(inserted);
	}
}

Control::CursorShape TetraVideoTimeline::get_cursor_shape(const Point2 &p_pos) const {
	if (drag_mode == DRAG_TRIM_START || drag_mode == DRAG_TRIM_END) {
		return CURSOR_HSIZE;
	}
	if (drag_mode == DRAG_MOVE) {
		return ghosts_valid ? CURSOR_DRAG : CURSOR_FORBIDDEN;
	}
	if (p_pos.y >= _ruler_height() && p_pos.x >= _header_width()) {
		ClipEdge edge = EDGE_NONE;
		const int hit = _hit_clip(p_pos, &edge);
		if (tool == TOOL_RAZOR) {
			return hit >= 0 ? CURSOR_IBEAM : CURSOR_ARROW;
		}
		if (hit >= 0 && edge != EDGE_NONE) {
			return CURSOR_HSIZE;
		}
	}
	return Control::get_cursor_shape(p_pos);
}

String TetraVideoTimeline::get_tooltip(const Point2 &p_pos) const {
	if (project.is_null() || drag_mode != DRAG_NONE) {
		return String();
	}
	const int track = _track_at(p_pos.y);
	if (p_pos.x < _header_width()) {
		if (track < 0) {
			return String();
		}
		const real_t reach = Math::round(4 * EDSCALE);
		if (_header_button_rect(track, 0).grow(reach).has_point(p_pos)) {
			return project->get_track_kind(track) == TetraVideoProject::TRACK_AUDIO ? TTR("Mute this track") : TTR("Hide this track");
		}
		if (_header_button_rect(track, 1).grow(reach).has_point(p_pos)) {
			return TTR("Lock this track: its clips cannot be changed");
		}
		return String();
	}
	const ClipView *clip = _find_clip(_hit_clip(p_pos));
	if (!clip) {
		return String();
	}
	const double fps = project->get_fps();
	String text = clip->name + "\n" + vformat(TTR("Position %s, duration %s"), TetraVideoTimecode::from_frames(clip->position, fps), TetraVideoTimecode::from_frames(clip->length, fps));
	if (clip->effect_count > 0) {
		text += "\n" + vformat(TTR("%d effect(s)"), clip->effect_count);
	}
	return text;
}

void TetraVideoTimeline::set_project(const Ref<TetraVideoProject> &p_project) {
	if (project.is_valid()) {
		project->disconnect("changed", callable_mp(this, &TetraVideoTimeline::_project_changed));
	}
	_cancel_drag();
	project = p_project;
	clips_revision = UINT64_MAX;
	selection.clear();
	active_track = -1;
	zone_in = -1;
	zone_out = -1;
	playhead = 0;
	if (project.is_valid()) {
		project->connect("changed", callable_mp(this, &TetraVideoTimeline::_project_changed));
	}
	updating_scroll = true;
	h_scroll->set_value(0);
	v_scroll->set_value(0);
	updating_scroll = false;
	_update_scroll_bars();
	emit_signal(SNAME("selection_changed"));
	queue_redraw();
}

Ref<TetraVideoProject> TetraVideoTimeline::get_project() const {
	return project;
}

void TetraVideoTimeline::set_thumbnails(const Ref<TetraVideoThumbnails> &p_thumbnails) {
	if (thumbnails.is_valid()) {
		thumbnails->disconnect("thumbnail_ready", callable_mp(this, &TetraVideoTimeline::_thumbnail_ready));
	}
	thumbnails = p_thumbnails;
	if (thumbnails.is_valid()) {
		thumbnails->connect("thumbnail_ready", callable_mp(this, &TetraVideoTimeline::_thumbnail_ready));
	}
	queue_redraw();
}

void TetraVideoTimeline::set_playhead(int p_frame, bool p_follow) {
	const int frame = MAX(0, p_frame);
	if (frame == playhead && !p_follow) {
		return;
	}
	playhead = frame;
	_update_scroll_bars();
	if (p_follow) {
		const real_t x = frame_to_x(playhead);
		const real_t lanes_width = _lanes_width();
		if (x < _header_width() || x > _header_width() + lanes_width - 24 * EDSCALE) {
			h_scroll->set_value(playhead * zoom - lanes_width * 0.1);
		}
	}
	queue_redraw();
}

int TetraVideoTimeline::get_playhead() const {
	return playhead;
}

void TetraVideoTimeline::set_zoom(double p_pixels_per_frame) {
	set_zoom_at(p_pixels_per_frame, _header_width() + _lanes_width() * 0.5);
}

void TetraVideoTimeline::set_zoom_at(double p_pixels_per_frame, real_t p_view_x) {
	const double next = CLAMP(p_pixels_per_frame, get_zoom_min(), get_zoom_max());
	if (next == zoom) {
		return;
	}
	const double offset = MAX(0.0, double(p_view_x - _header_width()));
	const double frame = (h_scroll->get_value() + offset) / zoom;
	zoom = next;
	_update_scroll_bars();
	h_scroll->set_value(frame * zoom - offset);
	queue_redraw();
	emit_signal(SNAME("zoom_changed"), zoom);
}

double TetraVideoTimeline::get_zoom() const {
	return zoom;
}

double TetraVideoTimeline::get_zoom_min() const {
	return 0.02;
}

double TetraVideoTimeline::get_zoom_max() const {
	return 48.0;
}

void TetraVideoTimeline::zoom_to_fit() {
	int frames = project.is_valid() ? project->get_duration() : 0;
	if (frames <= 0) {
		// An empty timeline shows half a minute.
		frames = int(30 * (project.is_valid() ? project->get_fps() : 25.0));
	}
	const double next = CLAMP(double(_lanes_width() - 32 * EDSCALE) / frames, get_zoom_min(), get_zoom_max());
	zoom = next;
	_update_scroll_bars();
	h_scroll->set_value(0);
	queue_redraw();
	emit_signal(SNAME("zoom_changed"), zoom);
}

void TetraVideoTimeline::set_scroll_frame(int p_frame) {
	_update_scroll_bars();
	h_scroll->set_value(MAX(0, p_frame) * zoom);
	queue_redraw();
}

int TetraVideoTimeline::get_scroll_frame() const {
	return int(h_scroll->get_value() / zoom);
}

void TetraVideoTimeline::set_tool(Tool p_tool) {
	if (tool == p_tool) {
		return;
	}
	_cancel_drag();
	tool = p_tool;
	queue_redraw();
	emit_signal(SNAME("tool_changed"), int(tool));
}

TetraVideoTimeline::Tool TetraVideoTimeline::get_tool() const {
	return tool;
}

void TetraVideoTimeline::set_snapping(bool p_enabled) {
	snapping = p_enabled;
}

bool TetraVideoTimeline::is_snapping() const {
	return snapping;
}

void TetraVideoTimeline::set_selection(const PackedInt32Array &p_clip_ids) {
	PackedInt32Array next;
	for (int i = 0; i < p_clip_ids.size(); i++) {
		if (project.is_valid() && project->has_clip(p_clip_ids[i]) && !next.has(p_clip_ids[i])) {
			next.push_back(p_clip_ids[i]);
		}
	}
	if (next == selection) {
		return;
	}
	selection = next;
	queue_redraw();
	emit_signal(SNAME("selection_changed"));
}

PackedInt32Array TetraVideoTimeline::get_selection() const {
	return selection;
}

void TetraVideoTimeline::select_clip(int p_clip_id, bool p_add) {
	if (p_add && selection.has(p_clip_id)) {
		return;
	}
	_click_clip(p_clip_id, p_add);
}

void TetraVideoTimeline::clear_selection() {
	set_selection(PackedInt32Array());
}

void TetraVideoTimeline::set_active_track(int p_track) {
	const int track = project.is_valid() && p_track >= 0 && p_track < project->get_track_count() ? p_track : -1;
	if (track == active_track) {
		return;
	}
	active_track = track;
	queue_redraw();
	emit_signal(SNAME("active_track_changed"), active_track);
}

int TetraVideoTimeline::get_active_track() const {
	return active_track;
}

void TetraVideoTimeline::set_zone(int p_in, int p_out) {
	zone_in = p_in;
	zone_out = p_out;
	if (zone_in < 0 || zone_out < zone_in) {
		zone_in = -1;
		zone_out = -1;
	}
	queue_redraw();
}

int TetraVideoTimeline::get_zone_in() const {
	return zone_in;
}

int TetraVideoTimeline::get_zone_out() const {
	return zone_out;
}

real_t TetraVideoTimeline::frame_to_x(int p_frame) const {
	return _header_width() + p_frame * zoom - h_scroll->get_value();
}

int TetraVideoTimeline::x_to_frame(real_t p_x) const {
	return MAX(0, int(Math::round((p_x - _header_width() + h_scroll->get_value()) / zoom)));
}

Rect2 TetraVideoTimeline::get_clip_rect(int p_clip_id) const {
	const ClipView *clip = _find_clip(p_clip_id);
	return clip ? _clip_rect(*clip) : Rect2();
}

Rect2 TetraVideoTimeline::get_track_rect(int p_track) const {
	if (project.is_null() || p_track < 0 || p_track >= project->get_track_count()) {
		return Rect2();
	}
	return Rect2(_header_width(), _track_y(p_track), _lanes_width(), _track_height(p_track));
}

Rect2 TetraVideoTimeline::get_ruler_rect() const {
	return Rect2(_header_width(), 0, _lanes_width(), _ruler_height());
}

Rect2 TetraVideoTimeline::get_track_header_button_rect(int p_track, int p_button) const {
	if (project.is_null() || p_track < 0 || p_track >= project->get_track_count()) {
		return Rect2();
	}
	return _header_button_rect(p_track, CLAMP(p_button, 0, 1));
}

void TetraVideoTimeline::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			_draw_timeline();
		} break;
		case NOTIFICATION_RESIZED:
		case NOTIFICATION_THEME_CHANGED: {
			clip_style->set_corner_radius_all(int(Math::round(4 * EDSCALE)));
			_update_scroll_bars();
			queue_redraw();
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			_clear_drop_feedback();
			queue_redraw();
		} break;
		case NOTIFICATION_DRAG_END: {
			_clear_drop_feedback();
		} break;
		case NOTIFICATION_FOCUS_ENTER:
		case NOTIFICATION_FOCUS_EXIT: {
			queue_redraw();
		} break;
	}
}

void TetraVideoTimeline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_project"), &TetraVideoTimeline::get_project);
	ClassDB::bind_method(D_METHOD("set_playhead", "frame", "follow"), &TetraVideoTimeline::set_playhead, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_playhead"), &TetraVideoTimeline::get_playhead);
	ClassDB::bind_method(D_METHOD("set_zoom", "pixels_per_frame"), &TetraVideoTimeline::set_zoom);
	ClassDB::bind_method(D_METHOD("set_zoom_at", "pixels_per_frame", "view_x"), &TetraVideoTimeline::set_zoom_at);
	ClassDB::bind_method(D_METHOD("get_zoom"), &TetraVideoTimeline::get_zoom);
	ClassDB::bind_method(D_METHOD("get_zoom_min"), &TetraVideoTimeline::get_zoom_min);
	ClassDB::bind_method(D_METHOD("get_zoom_max"), &TetraVideoTimeline::get_zoom_max);
	ClassDB::bind_method(D_METHOD("zoom_to_fit"), &TetraVideoTimeline::zoom_to_fit);
	ClassDB::bind_method(D_METHOD("set_scroll_frame", "frame"), &TetraVideoTimeline::set_scroll_frame);
	ClassDB::bind_method(D_METHOD("get_scroll_frame"), &TetraVideoTimeline::get_scroll_frame);
	ClassDB::bind_method(D_METHOD("set_tool", "tool"), &TetraVideoTimeline::set_tool);
	ClassDB::bind_method(D_METHOD("get_tool"), &TetraVideoTimeline::get_tool);
	ClassDB::bind_method(D_METHOD("set_snapping", "enabled"), &TetraVideoTimeline::set_snapping);
	ClassDB::bind_method(D_METHOD("is_snapping"), &TetraVideoTimeline::is_snapping);
	ClassDB::bind_method(D_METHOD("set_selection", "clip_ids"), &TetraVideoTimeline::set_selection);
	ClassDB::bind_method(D_METHOD("get_selection"), &TetraVideoTimeline::get_selection);
	ClassDB::bind_method(D_METHOD("select_clip", "clip_id", "add"), &TetraVideoTimeline::select_clip, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_selection"), &TetraVideoTimeline::clear_selection);
	ClassDB::bind_method(D_METHOD("set_active_track", "track"), &TetraVideoTimeline::set_active_track);
	ClassDB::bind_method(D_METHOD("get_active_track"), &TetraVideoTimeline::get_active_track);
	ClassDB::bind_method(D_METHOD("set_zone", "in", "out"), &TetraVideoTimeline::set_zone);
	ClassDB::bind_method(D_METHOD("get_zone_in"), &TetraVideoTimeline::get_zone_in);
	ClassDB::bind_method(D_METHOD("get_zone_out"), &TetraVideoTimeline::get_zone_out);
	ClassDB::bind_method(D_METHOD("frame_to_x", "frame"), &TetraVideoTimeline::frame_to_x);
	ClassDB::bind_method(D_METHOD("x_to_frame", "x"), &TetraVideoTimeline::x_to_frame);
	ClassDB::bind_method(D_METHOD("get_clip_rect", "clip_id"), &TetraVideoTimeline::get_clip_rect);
	ClassDB::bind_method(D_METHOD("get_track_rect", "track"), &TetraVideoTimeline::get_track_rect);
	ClassDB::bind_method(D_METHOD("get_ruler_rect"), &TetraVideoTimeline::get_ruler_rect);
	ClassDB::bind_method(D_METHOD("get_track_header_button_rect", "track", "button"), &TetraVideoTimeline::get_track_header_button_rect);

	ADD_SIGNAL(MethodInfo("seek_requested", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("selection_changed"));
	ADD_SIGNAL(MethodInfo("zoom_changed", PropertyInfo(Variant::FLOAT, "pixels_per_frame")));
	ADD_SIGNAL(MethodInfo("tool_changed", PropertyInfo(Variant::INT, "tool")));
	ADD_SIGNAL(MethodInfo("active_track_changed", PropertyInfo(Variant::INT, "track")));
	ADD_SIGNAL(MethodInfo("edit_refused", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("clip_activated", PropertyInfo(Variant::INT, "clip_id")));
	ADD_SIGNAL(MethodInfo("effect_dropped", PropertyInfo(Variant::INT, "clip_id"), PropertyInfo(Variant::STRING, "operation_id")));
	ADD_SIGNAL(MethodInfo("interaction_started"));

	BIND_ENUM_CONSTANT(TOOL_SELECT);
	BIND_ENUM_CONSTANT(TOOL_RAZOR);
}

TetraVideoTimeline::TetraVideoTimeline() {
	set_focus_mode(FOCUS_ALL);
	set_clip_contents(true);
	set_custom_minimum_size(Size2(200, 110) * EDSCALE);

	clip_style.instantiate();
	clip_style->set_corner_radius_all(4);
	clip_style->set_anti_aliased(true);

	h_scroll = memnew(HScrollBar);
	h_scroll->connect(SceneStringName(value_changed), callable_mp(this, &TetraVideoTimeline::_scroll_changed));
	add_child(h_scroll);

	v_scroll = memnew(VScrollBar);
	v_scroll->connect(SceneStringName(value_changed), callable_mp(this, &TetraVideoTimeline::_scroll_changed));
	add_child(v_scroll);
}
