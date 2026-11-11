#include "tetra_video_project.h"

#include "core/object/class_db.h"

#include <climits>

namespace {

struct ProfilePreset {
	const char *name;
	int width;
	int height;
	int fps_num;
	int fps_den;
};

// Square-pixel progressive profiles, named like MLT's own profile files.
const ProfilePreset profile_presets[] = {
	{ "atsc_1080p_25", 1920, 1080, 25, 1 },
	{ "atsc_1080p_24", 1920, 1080, 24, 1 },
	{ "atsc_1080p_2398", 1920, 1080, 24000, 1001 },
	{ "atsc_1080p_2997", 1920, 1080, 30000, 1001 },
	{ "atsc_1080p_30", 1920, 1080, 30, 1 },
	{ "atsc_1080p_50", 1920, 1080, 50, 1 },
	{ "atsc_1080p_5994", 1920, 1080, 60000, 1001 },
	{ "atsc_1080p_60", 1920, 1080, 60, 1 },
	{ "atsc_720p_25", 1280, 720, 25, 1 },
	{ "atsc_720p_30", 1280, 720, 30, 1 },
	{ "atsc_720p_50", 1280, 720, 50, 1 },
	{ "atsc_720p_60", 1280, 720, 60, 1 },
	{ "uhd_2160p_25", 3840, 2160, 25, 1 },
	{ "uhd_2160p_30", 3840, 2160, 30, 1 },
	{ "uhd_2160p_50", 3840, 2160, 50, 1 },
	{ "uhd_2160p_60", 3840, 2160, 60, 1 },
	{ "vertical_hd_30", 1080, 1920, 30, 1 },
	{ "vertical_hd_60", 1080, 1920, 60, 1 },
	{ "square_1080p_30", 1080, 1080, 30, 1 },
	{ "square_1080p_60", 1080, 1080, 60, 1 },
};

struct SpanSort {
	// x = track, y = first frame, z = one past the last frame.
	bool operator()(const Vector3i &p_a, const Vector3i &p_b) const {
		return p_a.x != p_b.x ? p_a.x < p_b.x : p_a.y < p_b.y;
	}
};

} // namespace

int TetraVideoProject::_find_clip(const State &p_state, int p_id) {
	for (int i = 0; i < p_state.clips.size(); i++) {
		if (p_state.clips[i].id == p_id) {
			return i;
		}
	}
	return -1;
}

int TetraVideoProject::_find_bin(const State &p_state, int p_id) {
	for (int i = 0; i < p_state.bin.size(); i++) {
		if (p_state.bin[i].id == p_id) {
			return i;
		}
	}
	return -1;
}

bool TetraVideoProject::_bin_is_still(const BinClip &p_bin) {
	return p_bin.kind == CLIP_IMAGE || p_bin.kind == CLIP_COLOR || p_bin.kind == CLIP_TEXT;
}

bool TetraVideoProject::_layout_ok(const State &p_state) {
	Vector<Vector3i> spans;
	spans.resize(p_state.clips.size());
	for (int i = 0; i < p_state.clips.size(); i++) {
		const Clip &clip = p_state.clips[i];
		if (clip.track < 0 || clip.track >= p_state.tracks.size() || clip.position < 0 || clip.in < 0 || clip.out < clip.in) {
			return false;
		}
		spans.write[i] = Vector3i(clip.track, clip.position, clip.end());
	}
	spans.sort_custom<SpanSort>();
	for (int i = 1; i < spans.size(); i++) {
		if (spans[i].x == spans[i - 1].x && spans[i].y < spans[i - 1].z) {
			return false;
		}
	}
	return true;
}

bool TetraVideoProject::_track_accepts(const State &p_state, int p_track, const BinClip &p_bin) {
	if (p_track < 0 || p_track >= p_state.tracks.size()) {
		return false;
	}
	return p_state.tracks[p_track].kind == TRACK_VIDEO ? p_bin.has_video : p_bin.has_audio;
}

// Kdenlive's mirror rule: the n-th video track counted from the lowest one is
// paired with the n-th audio track counted from the highest one, so that in
// the usual A2 A1 V1 V2 stack V1 goes with A1.
int TetraVideoProject::_paired_track(const State &p_state, int p_track) {
	if (p_track < 0 || p_track >= p_state.tracks.size()) {
		return -1;
	}
	const int count = p_state.tracks.size();
	int rank = 0;
	if (p_state.tracks[p_track].kind == TRACK_VIDEO) {
		for (int i = 0; i < p_track; i++) {
			rank += p_state.tracks[i].kind == TRACK_VIDEO ? 1 : 0;
		}
		for (int i = count - 1; i >= 0; i--) {
			if (p_state.tracks[i].kind == TRACK_AUDIO && rank-- == 0) {
				return i;
			}
		}
	} else {
		for (int i = p_track + 1; i < count; i++) {
			rank += p_state.tracks[i].kind == TRACK_AUDIO ? 1 : 0;
		}
		for (int i = 0; i < count; i++) {
			if (p_state.tracks[i].kind == TRACK_VIDEO && rank-- == 0) {
				return i;
			}
		}
	}
	return -1;
}

void TetraVideoProject::_clamp_fades(Clip &r_clip) {
	r_clip.fade_in = CLAMP(r_clip.fade_in, 0, r_clip.length());
	r_clip.fade_out = CLAMP(r_clip.fade_out, 0, r_clip.length());
}

// Cuts one clip in two at a timeline frame strictly inside it, ignoring links.
// The left part keeps the id and the fade in, the new right part gets the fade
// out, and both keep the effects.
int TetraVideoProject::_split_raw(State &r_state, int p_clip_index, int p_frame) {
	Clip left = r_state.clips[p_clip_index];
	const int offset = p_frame - left.position;
	Clip right = left;
	right.id = r_state.next_clip_id++;
	right.position = p_frame;
	right.in = left.in + offset;
	right.fade_in = 0;
	right.link = 0;
	left.out = left.in + offset - 1;
	left.fade_out = 0;
	_clamp_fades(left);
	_clamp_fades(right);
	r_state.clips.write[p_clip_index] = left;
	r_state.clips.push_back(right);
	return right.id;
}

// After several clips were cut at the same frame, the right parts of two
// linked clips become a linked pair of their own.
void TetraVideoProject::_relink_split(State &r_state, const Vector<int> &p_originals, const Vector<int> &p_new_ids) {
	for (int i = 0; i < p_originals.size(); i++) {
		const int original_link = r_state.clips[_find_clip(r_state, p_originals[i])].link;
		const int partner = original_link != 0 ? p_originals.find(original_link) : -1;
		if (partner >= 0) {
			r_state.clips.write[_find_clip(r_state, p_new_ids[i])].link = p_new_ids[partner];
		}
	}
}

void TetraVideoProject::_remove_clip_at(State &r_state, int p_index) {
	const int link = r_state.clips[p_index].link;
	r_state.clips.remove_at(p_index);
	if (link != 0) {
		const int partner = _find_clip(r_state, link);
		if (partner >= 0) {
			r_state.clips.write[partner].link = 0;
		}
	}
}

Vector<int> TetraVideoProject::_sorted_track_clips(const State &p_state, int p_track) {
	Vector<Vector3i> spans;
	for (int i = 0; i < p_state.clips.size(); i++) {
		if (p_state.clips[i].track == p_track) {
			spans.push_back(Vector3i(p_track, p_state.clips[i].position, i));
		}
	}
	spans.sort_custom<SpanSort>();
	Vector<int> indices;
	indices.resize(spans.size());
	for (int i = 0; i < spans.size(); i++) {
		indices.write[i] = spans[i].z;
	}
	return indices;
}

String TetraVideoProject::_check_state(const State &p_state) {
	if (p_state.width < 1 || p_state.height < 1 || p_state.fps_num < 1 || p_state.fps_den < 1 || p_state.sample_rate < 1) {
		return "profile has a non-positive value";
	}
	for (int i = 0; i < p_state.bin.size(); i++) {
		const BinClip &bin = p_state.bin[i];
		if (bin.id < 1 || bin.id >= p_state.next_bin_id) {
			return vformat("bin clip %d has an id outside the allocated range", bin.id);
		}
		if (bin.length < 1) {
			return vformat("bin clip %d has no length", bin.id);
		}
		if (!bin.has_video && !bin.has_audio) {
			return vformat("bin clip %d has neither video nor audio", bin.id);
		}
		for (int j = 0; j < i; j++) {
			if (p_state.bin[j].id == bin.id) {
				return vformat("bin clip id %d is used twice", bin.id);
			}
		}
	}
	HashMap<int, int> seen;
	for (int i = 0; i < p_state.clips.size(); i++) {
		const Clip &clip = p_state.clips[i];
		if (clip.id < 1 || clip.id >= p_state.next_clip_id) {
			return vformat("clip %d has an id outside the allocated range", clip.id);
		}
		if (seen.has(clip.id)) {
			return vformat("clip id %d is used twice", clip.id);
		}
		seen[clip.id] = i;
		const int bin_index = _find_bin(p_state, clip.bin_id);
		if (bin_index < 0) {
			return vformat("clip %d refers to missing bin clip %d", clip.id, clip.bin_id);
		}
		const BinClip &bin = p_state.bin[bin_index];
		if (clip.track < 0 || clip.track >= p_state.tracks.size()) {
			return vformat("clip %d is on missing track %d", clip.id, clip.track);
		}
		if (!_track_accepts(p_state, clip.track, bin)) {
			return vformat("clip %d is on a track of the wrong kind", clip.id);
		}
		if (clip.position < 0 || clip.in < 0 || clip.out < clip.in) {
			return vformat("clip %d has an illegal position or in/out", clip.id);
		}
		if (!_bin_is_still(bin) && clip.out > bin.length - 1) {
			return vformat("clip %d ends after its source (out %d, source length %d)", clip.id, clip.out, bin.length);
		}
		if (clip.fade_in < 0 || clip.fade_out < 0 || clip.fade_in > clip.length() || clip.fade_out > clip.length()) {
			return vformat("clip %d has a fade longer than itself", clip.id);
		}
	}
	for (int i = 0; i < p_state.clips.size(); i++) {
		const Clip &clip = p_state.clips[i];
		if (clip.link == 0) {
			continue;
		}
		if (clip.link == clip.id || !seen.has(clip.link)) {
			return vformat("clip %d is linked to missing clip %d", clip.id, clip.link);
		}
		const Clip &partner = p_state.clips[seen[clip.link]];
		if (partner.link != clip.id) {
			return vformat("link of clip %d is not mutual", clip.id);
		}
		if (partner.track == clip.track) {
			return vformat("clip %d is linked to a clip on its own track", clip.id);
		}
	}
	if (!_layout_ok(p_state)) {
		return "clips overlap on a track";
	}
	for (int i = 0; i < p_state.markers.size(); i++) {
		if (p_state.markers[i].frame < 0 || (i > 0 && p_state.markers[i].frame <= p_state.markers[i - 1].frame)) {
			return "markers are not sorted, unique and non-negative";
		}
	}
	return String();
}

bool TetraVideoProject::_clip_editable(const State &p_state, int p_clip_index) const {
	return !p_state.tracks[p_state.clips[p_clip_index].track].locked;
}

bool TetraVideoProject::_fail(const String &p_reason) {
	last_error = p_reason;
	return false;
}

void TetraVideoProject::_apply_state(const State &p_next) {
	const bool structure = state.tracks.ptr() != p_next.tracks.ptr() || state.bin.ptr() != p_next.bin.ptr() ||
			state.width != p_next.width || state.height != p_next.height || state.fps_num != p_next.fps_num ||
			state.fps_den != p_next.fps_den || state.sample_rate != p_next.sample_rate;
	state = p_next;
	revision++;
	if (structure) {
		emit_signal(SNAME("structure_changed"));
	}
	emit_signal(SNAME("changed"));
}

void TetraVideoProject::_commit(const String &p_name, const State &p_next) {
	last_error = String();
	if (group_depth > 0) {
		group_changed = true;
		_apply_state(p_next);
		return;
	}
	undo_stack.resize(undo_position);
	UndoStep step;
	step.name = p_name;
	step.before = state;
	step.after = p_next;
	undo_stack.push_back(step);
	if (undo_stack.size() > max_undo_steps) {
		undo_stack.remove_at(0);
	}
	undo_position = undo_stack.size();
	_apply_state(p_next);
}

// Profile.

bool TetraVideoProject::set_profile(int p_width, int p_height, int p_fps_num, int p_fps_den, int p_sample_rate) {
	if (p_width < 1 || p_height < 1 || p_fps_num < 1 || p_fps_den < 1 || p_sample_rate < 1) {
		return _fail("Profile values must be positive.");
	}
	if (p_width == state.width && p_height == state.height && p_fps_num == state.fps_num && p_fps_den == state.fps_den && p_sample_rate == state.sample_rate) {
		return true;
	}
	State next = state;
	next.width = p_width;
	next.height = p_height;
	next.fps_num = p_fps_num;
	next.fps_den = p_fps_den;
	next.sample_rate = p_sample_rate;
	_commit(TTR("Change Profile"), next);
	return true;
}

bool TetraVideoProject::set_profile_preset(const String &p_name) {
	for (const ProfilePreset &preset : profile_presets) {
		if (p_name == preset.name) {
			return set_profile(preset.width, preset.height, preset.fps_num, preset.fps_den, state.sample_rate);
		}
	}
	return _fail(vformat("Unknown profile preset \"%s\".", p_name));
}

PackedStringArray TetraVideoProject::get_profile_presets() {
	PackedStringArray names;
	for (const ProfilePreset &preset : profile_presets) {
		names.push_back(preset.name);
	}
	return names;
}

// Bin.

int TetraVideoProject::add_bin_clip(ClipKind p_kind, const String &p_name, const String &p_source, int p_length, bool p_has_video, bool p_has_audio, const Dictionary &p_properties) {
	if (p_length < 1) {
		_fail("A bin clip needs a length of at least one frame.");
		return -1;
	}
	BinClip bin;
	bin.kind = p_kind;
	bin.name = p_name;
	bin.source = p_source;
	bin.length = p_length;
	bin.properties = p_properties.duplicate(true);
	switch (p_kind) {
		case CLIP_VIDEO:
			bin.has_video = p_has_video;
			bin.has_audio = p_has_audio;
			break;
		case CLIP_AUDIO:
			bin.has_video = false;
			bin.has_audio = true;
			break;
		default:
			bin.has_video = true;
			bin.has_audio = false;
			break;
	}
	if (!bin.has_video && !bin.has_audio) {
		_fail("A media clip needs video or audio.");
		return -1;
	}
	State next = state;
	bin.id = next.next_bin_id++;
	next.bin.push_back(bin);
	_commit(TTR("Add Bin Clip"), next);
	return bin.id;
}

int TetraVideoProject::add_bin_color(const String &p_name, const Color &p_color, int p_length) {
	return add_bin_clip(CLIP_COLOR, p_name, "#" + p_color.to_html(true), p_length);
}

int TetraVideoProject::add_bin_text(const String &p_name, const String &p_text, int p_length, const Dictionary &p_properties) {
	return add_bin_clip(CLIP_TEXT, p_name, p_text, p_length, true, false, p_properties);
}

bool TetraVideoProject::remove_bin_clip(int p_id, bool p_cascade) {
	const int index = _find_bin(state, p_id);
	if (index < 0) {
		return _fail("No such bin clip.");
	}
	State next = state;
	for (int i = next.clips.size() - 1; i >= 0; i--) {
		if (next.clips[i].bin_id != p_id) {
			continue;
		}
		if (!p_cascade) {
			return _fail("The bin clip is used on the timeline.");
		}
		if (!_clip_editable(next, i)) {
			return _fail("The bin clip is used on a locked track.");
		}
		_remove_clip_at(next, i);
	}
	next.bin.remove_at(index);
	_commit(TTR("Remove Bin Clip"), next);
	return true;
}

bool TetraVideoProject::set_bin_clip_name(int p_id, const String &p_name) {
	const int index = _find_bin(state, p_id);
	if (index < 0) {
		return _fail("No such bin clip.");
	}
	if (state.bin[index].name == p_name) {
		return true;
	}
	State next = state;
	next.bin.write[index].name = p_name;
	_commit(TTR("Rename Bin Clip"), next);
	return true;
}

bool TetraVideoProject::set_bin_clip_source(int p_id, const String &p_source) {
	const int index = _find_bin(state, p_id);
	if (index < 0) {
		return _fail("No such bin clip.");
	}
	if (state.bin[index].source == p_source) {
		return true;
	}
	State next = state;
	next.bin.write[index].source = p_source;
	_commit(TTR("Change Bin Clip Source"), next);
	return true;
}

bool TetraVideoProject::set_bin_clip_properties(int p_id, const Dictionary &p_properties) {
	const int index = _find_bin(state, p_id);
	if (index < 0) {
		return _fail("No such bin clip.");
	}
	State next = state;
	next.bin.write[index].properties = p_properties.duplicate(true);
	_commit(TTR("Change Bin Clip Properties"), next);
	return true;
}

bool TetraVideoProject::has_bin_clip(int p_id) const {
	return _find_bin(state, p_id) >= 0;
}

Dictionary TetraVideoProject::_bin_to_dictionary(const BinClip &p_bin) const {
	Dictionary d;
	d["id"] = p_bin.id;
	d["kind"] = p_bin.kind;
	d["name"] = p_bin.name;
	d["source"] = p_bin.source;
	d["length"] = p_bin.length;
	d["has_video"] = p_bin.has_video;
	d["has_audio"] = p_bin.has_audio;
	d["properties"] = p_bin.properties.duplicate(true);
	return d;
}

Dictionary TetraVideoProject::get_bin_clip(int p_id) const {
	const int index = _find_bin(state, p_id);
	return index < 0 ? Dictionary() : _bin_to_dictionary(state.bin[index]);
}

PackedInt32Array TetraVideoProject::get_bin_clip_ids() const {
	PackedInt32Array ids;
	for (const BinClip &bin : state.bin) {
		ids.push_back(bin.id);
	}
	return ids;
}

int TetraVideoProject::get_bin_clip_use_count(int p_id) const {
	int count = 0;
	for (const Clip &clip : state.clips) {
		count += clip.bin_id == p_id ? 1 : 0;
	}
	return count;
}

// Tracks.

int TetraVideoProject::add_track(TrackKind p_kind, const String &p_name, int p_index) {
	State next = state;
	// Video tracks stack upwards and audio tracks downwards, which keeps V1 next to A1.
	int index = p_index < 0 ? (p_kind == TRACK_VIDEO ? next.tracks.size() : 0) : MIN(p_index, next.tracks.size());
	Track track;
	track.kind = p_kind;
	track.name = p_name;
	if (track.name.is_empty()) {
		int same_kind = 1;
		for (const Track &other : next.tracks) {
			same_kind += other.kind == p_kind ? 1 : 0;
		}
		track.name = (p_kind == TRACK_VIDEO ? "V" : "A") + itos(same_kind);
	}
	next.tracks.insert(index, track);
	for (int i = 0; i < next.clips.size(); i++) {
		if (next.clips[i].track >= index) {
			next.clips.write[i].track++;
		}
	}
	_commit(TTR("Add Track"), next);
	return index;
}

bool TetraVideoProject::remove_track(int p_index) {
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return _fail("No such track.");
	}
	if (state.tracks[p_index].locked) {
		return _fail("The track is locked.");
	}
	State next = state;
	for (int i = next.clips.size() - 1; i >= 0; i--) {
		if (next.clips[i].track == p_index) {
			_remove_clip_at(next, i);
		}
	}
	for (int i = 0; i < next.clips.size(); i++) {
		if (next.clips[i].track > p_index) {
			next.clips.write[i].track--;
		}
	}
	next.tracks.remove_at(p_index);
	_commit(TTR("Remove Track"), next);
	return true;
}

bool TetraVideoProject::move_track(int p_from, int p_to) {
	const int count = state.tracks.size();
	if (p_from < 0 || p_from >= count || p_to < 0 || p_to >= count) {
		return _fail("No such track.");
	}
	if (p_from == p_to) {
		return true;
	}
	State next = state;
	const Track moved = next.tracks[p_from];
	next.tracks.remove_at(p_from);
	next.tracks.insert(p_to, moved);
	for (int i = 0; i < next.clips.size(); i++) {
		const int track = next.clips[i].track;
		if (track == p_from) {
			next.clips.write[i].track = p_to;
		} else if (p_from < p_to && track > p_from && track <= p_to) {
			next.clips.write[i].track = track - 1;
		} else if (p_to < p_from && track >= p_to && track < p_from) {
			next.clips.write[i].track = track + 1;
		}
	}
	_commit(TTR("Move Track"), next);
	return true;
}

bool TetraVideoProject::set_track_name(int p_index, const String &p_name) {
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return _fail("No such track.");
	}
	if (state.tracks[p_index].name == p_name) {
		return true;
	}
	State next = state;
	next.tracks.write[p_index].name = p_name;
	_commit(TTR("Rename Track"), next);
	return true;
}

bool TetraVideoProject::set_track_hidden(int p_index, bool p_hidden) {
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return _fail("No such track.");
	}
	if (state.tracks[p_index].hidden == p_hidden) {
		return true;
	}
	State next = state;
	next.tracks.write[p_index].hidden = p_hidden;
	_commit(p_hidden ? TTR("Hide Track") : TTR("Show Track"), next);
	return true;
}

bool TetraVideoProject::set_track_muted(int p_index, bool p_muted) {
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return _fail("No such track.");
	}
	if (state.tracks[p_index].muted == p_muted) {
		return true;
	}
	State next = state;
	next.tracks.write[p_index].muted = p_muted;
	_commit(p_muted ? TTR("Mute Track") : TTR("Unmute Track"), next);
	return true;
}

bool TetraVideoProject::set_track_locked(int p_index, bool p_locked) {
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return _fail("No such track.");
	}
	if (state.tracks[p_index].locked == p_locked) {
		return true;
	}
	State next = state;
	next.tracks.write[p_index].locked = p_locked;
	_commit(p_locked ? TTR("Lock Track") : TTR("Unlock Track"), next);
	return true;
}

Dictionary TetraVideoProject::get_track(int p_index) const {
	Dictionary d;
	if (p_index < 0 || p_index >= state.tracks.size()) {
		return d;
	}
	const Track &track = state.tracks[p_index];
	d["kind"] = track.kind;
	d["name"] = track.name;
	d["hidden"] = track.hidden;
	d["muted"] = track.muted;
	d["locked"] = track.locked;
	return d;
}

TetraVideoProject::TrackKind TetraVideoProject::get_track_kind(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, state.tracks.size(), TRACK_VIDEO);
	return state.tracks[p_index].kind;
}

String TetraVideoProject::get_track_name(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, state.tracks.size(), String());
	return state.tracks[p_index].name;
}

bool TetraVideoProject::is_track_hidden(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, state.tracks.size(), false);
	return state.tracks[p_index].hidden;
}

bool TetraVideoProject::is_track_muted(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, state.tracks.size(), false);
	return state.tracks[p_index].muted;
}

bool TetraVideoProject::is_track_locked(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, state.tracks.size(), false);
	return state.tracks[p_index].locked;
}

int TetraVideoProject::get_paired_track(int p_index) const {
	return _paired_track(state, p_index);
}

// Timeline clips.

int TetraVideoProject::insert_clip(int p_bin_id, int p_track, int p_position, int p_in, int p_out, bool p_with_linked) {
	const int bin_index = _find_bin(state, p_bin_id);
	if (bin_index < 0) {
		_fail("No such bin clip.");
		return -1;
	}
	const BinClip &bin = state.bin[bin_index];
	if (p_track < 0 || p_track >= state.tracks.size()) {
		_fail("No such track.");
		return -1;
	}
	if (!_track_accepts(state, p_track, bin)) {
		_fail("The clip does not fit this kind of track.");
		return -1;
	}
	const int in = p_in < 0 ? 0 : p_in;
	const int out = p_out < 0 ? MAX(in, bin.length - 1) : p_out;
	if (p_position < 0 || out < in || (!_bin_is_still(bin) && out > bin.length - 1)) {
		_fail("Illegal position or in/out points.");
		return -1;
	}
	if (state.tracks[p_track].locked) {
		_fail("The track is locked.");
		return -1;
	}
	State next = state;
	Clip clip;
	clip.id = next.next_clip_id++;
	clip.bin_id = p_bin_id;
	clip.track = p_track;
	clip.position = p_position;
	clip.in = in;
	clip.out = out;
	const int paired = p_with_linked && bin.has_video && bin.has_audio ? _paired_track(next, p_track) : -1;
	if (paired >= 0) {
		if (next.tracks[paired].locked) {
			_fail("The paired track is locked.");
			return -1;
		}
		Clip partner = clip;
		partner.id = next.next_clip_id++;
		partner.track = paired;
		partner.link = clip.id;
		clip.link = partner.id;
		next.clips.push_back(clip);
		next.clips.push_back(partner);
	} else {
		next.clips.push_back(clip);
	}
	if (!_layout_ok(next)) {
		_fail("The clip would overlap another clip.");
		return -1;
	}
	_commit(TTR("Insert Clip"), next);
	return clip.id;
}

bool TetraVideoProject::move_clip(int p_id, int p_track, int p_position) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	const Clip &clip = state.clips[index];
	if (p_track < 0 || p_track >= state.tracks.size() || p_position < 0) {
		return _fail("Illegal track or position.");
	}
	if (p_track == clip.track && p_position == clip.position) {
		return false;
	}
	if (!_clip_editable(state, index) || state.tracks[p_track].locked) {
		return _fail("The track is locked.");
	}
	if (!_track_accepts(state, p_track, state.bin[_find_bin(state, clip.bin_id)])) {
		return _fail("The clip does not fit this kind of track.");
	}
	State next = state;
	next.clips.write[index].track = p_track;
	next.clips.write[index].position = p_position;
	if (clip.link != 0) {
		const int partner_index = _find_clip(state, clip.link);
		const Clip &partner = state.clips[partner_index];
		int partner_track = partner.track;
		if (p_track != clip.track) {
			// An audio/video pair follows to the mirror track; two clips of the same kind keep their distance.
			if (state.tracks[partner.track].kind != state.tracks[clip.track].kind) {
				partner_track = _paired_track(state, p_track);
			} else {
				partner_track = partner.track + (p_track - clip.track);
			}
		}
		if (!_track_accepts(state, partner_track, state.bin[_find_bin(state, partner.bin_id)]) || partner_track == p_track) {
			return _fail("The linked clip has no track to move to.");
		}
		if (!_clip_editable(state, partner_index) || state.tracks[partner_track].locked) {
			return _fail("The linked clip's track is locked.");
		}
		next.clips.write[partner_index].track = partner_track;
		next.clips.write[partner_index].position = partner.position + (p_position - clip.position);
	}
	if (!_layout_ok(next)) {
		return _fail("The clip would overlap another clip.");
	}
	_commit(TTR("Move Clip"), next);
	return true;
}

bool TetraVideoProject::trim_clip_start(int p_id, int p_frame) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	Vector<int> group;
	group.push_back(index);
	if (state.clips[index].link != 0) {
		group.push_back(_find_clip(state, state.clips[index].link));
	}
	int delta = p_frame - state.clips[index].position;
	for (int member : group) {
		const Clip &clip = state.clips[member];
		if (!_clip_editable(state, member)) {
			return _fail("The track is locked.");
		}
		int previous_end = 0;
		for (const Clip &other : state.clips) {
			if (other.track == clip.track && other.position < clip.position) {
				previous_end = MAX(previous_end, other.end());
			}
		}
		int lowest = previous_end - clip.position;
		if (!_bin_is_still(state.bin[_find_bin(state, clip.bin_id)])) {
			lowest = MAX(lowest, -clip.in);
		}
		delta = CLAMP(delta, lowest, clip.length() - 1);
	}
	if (delta == 0) {
		return false;
	}
	State next = state;
	for (int member : group) {
		Clip clip = next.clips[member];
		const int new_length = clip.length() - delta;
		clip.position += delta;
		// A still has no real source frames: growing it leftwards past frame 0 just makes it longer.
		clip.in = MAX(0, clip.in + delta);
		clip.out = clip.in + new_length - 1;
		_clamp_fades(clip);
		next.clips.write[member] = clip;
	}
	_commit(TTR("Trim Clip Start"), next);
	return true;
}

bool TetraVideoProject::trim_clip_end(int p_id, int p_frame) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	Vector<int> group;
	group.push_back(index);
	if (state.clips[index].link != 0) {
		group.push_back(_find_clip(state, state.clips[index].link));
	}
	int delta = p_frame - state.clips[index].end();
	for (int member : group) {
		const Clip &clip = state.clips[member];
		if (!_clip_editable(state, member)) {
			return _fail("The track is locked.");
		}
		int next_start = INT_MAX;
		for (const Clip &other : state.clips) {
			if (other.track == clip.track && other.position > clip.position) {
				next_start = MIN(next_start, other.position);
			}
		}
		int highest = next_start - clip.end();
		const BinClip &bin = state.bin[_find_bin(state, clip.bin_id)];
		if (!_bin_is_still(bin)) {
			highest = MIN(highest, bin.length - 1 - clip.out);
		}
		delta = CLAMP(delta, -(clip.length() - 1), highest);
	}
	if (delta == 0) {
		return false;
	}
	State next = state;
	for (int member : group) {
		Clip clip = next.clips[member];
		clip.out += delta;
		_clamp_fades(clip);
		next.clips.write[member] = clip;
	}
	_commit(TTR("Trim Clip End"), next);
	return true;
}

int TetraVideoProject::split_clip(int p_id, int p_frame) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		_fail("No such clip.");
		return -1;
	}
	const Clip &clip = state.clips[index];
	if (p_frame <= clip.position || p_frame >= clip.end()) {
		_fail("The frame is not inside the clip.");
		return -1;
	}
	if (!_clip_editable(state, index)) {
		_fail("The track is locked.");
		return -1;
	}
	State next = state;
	Vector<int> originals;
	Vector<int> new_ids;
	originals.push_back(p_id);
	new_ids.push_back(_split_raw(next, index, p_frame));
	if (clip.link != 0) {
		const int partner_index = _find_clip(state, clip.link);
		const Clip &partner = state.clips[partner_index];
		if (p_frame > partner.position && p_frame < partner.end()) {
			if (!_clip_editable(state, partner_index)) {
				_fail("The linked clip's track is locked.");
				return -1;
			}
			originals.push_back(partner.id);
			new_ids.push_back(_split_raw(next, partner_index, p_frame));
		}
	}
	_relink_split(next, originals, new_ids);
	_commit(TTR("Split Clip"), next);
	return new_ids[0];
}

PackedInt32Array TetraVideoProject::split_all_at(int p_frame) {
	State next = state;
	Vector<int> originals;
	Vector<int> new_ids;
	const int count = next.clips.size();
	for (int i = 0; i < count; i++) {
		const Clip &clip = next.clips[i];
		if (p_frame > clip.position && p_frame < clip.end() && _clip_editable(next, i)) {
			originals.push_back(clip.id);
			new_ids.push_back(_split_raw(next, i, p_frame));
		}
	}
	PackedInt32Array result;
	if (new_ids.is_empty()) {
		return result;
	}
	_relink_split(next, originals, new_ids);
	_commit(TTR("Split All Clips"), next);
	for (int id : new_ids) {
		result.push_back(id);
	}
	return result;
}

bool TetraVideoProject::delete_clip(int p_id) {
	PackedInt32Array ids;
	ids.push_back(p_id);
	return delete_clips(ids);
}

bool TetraVideoProject::delete_clips(const PackedInt32Array &p_ids) {
	if (p_ids.is_empty()) {
		return false;
	}
	State next = state;
	for (int id : p_ids) {
		const int index = _find_clip(next, id);
		if (index < 0) {
			// Already removed as the linked partner of an earlier id, unless it never existed.
			if (_find_clip(state, id) < 0) {
				return _fail("No such clip.");
			}
			continue;
		}
		const int link = next.clips[index].link;
		if (!_clip_editable(next, index)) {
			return _fail("The track is locked.");
		}
		_remove_clip_at(next, index);
		if (link != 0) {
			const int partner_index = _find_clip(next, link);
			if (!_clip_editable(next, partner_index)) {
				return _fail("The linked clip's track is locked.");
			}
			_remove_clip_at(next, partner_index);
		}
	}
	_commit(p_ids.size() == 1 ? TTR("Delete Clip") : TTR("Delete Clips"), next);
	return true;
}

bool TetraVideoProject::ripple_delete(int p_id) {
	int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	State next = state;
	const int link = next.clips[index].link;
	for (int pass = 0; pass < 2 && index >= 0; pass++) {
		if (!_clip_editable(next, index)) {
			return _fail("The track is locked.");
		}
		const Clip removed = next.clips[index];
		_remove_clip_at(next, index);
		for (int i = 0; i < next.clips.size(); i++) {
			if (next.clips[i].track == removed.track && next.clips[i].position >= removed.position) {
				next.clips.write[i].position -= removed.length();
			}
		}
		index = pass == 0 && link != 0 ? _find_clip(next, link) : -1;
	}
	if (!_layout_ok(next)) {
		return _fail("The gap cannot be closed.");
	}
	_commit(TTR("Ripple Delete"), next);
	return true;
}

bool TetraVideoProject::_resolve_tracks(const PackedInt32Array &p_tracks, Vector<int> &r_tracks) const {
	if (p_tracks.is_empty()) {
		for (int i = 0; i < state.tracks.size(); i++) {
			if (!state.tracks[i].locked) {
				r_tracks.push_back(i);
			}
		}
		return true;
	}
	for (int track : p_tracks) {
		if (track < 0 || track >= state.tracks.size() || state.tracks[track].locked) {
			return false;
		}
		if (!r_tracks.has(track)) {
			r_tracks.push_back(track);
		}
	}
	return true;
}

bool TetraVideoProject::lift_range(int p_from, int p_to, const PackedInt32Array &p_tracks) {
	Vector<int> tracks;
	if (p_from < 0 || p_to <= p_from) {
		return _fail("Illegal frame range.");
	}
	if (!_resolve_tracks(p_tracks, tracks)) {
		return _fail("A track is missing or locked.");
	}
	State next = state;
	const int cuts[2] = { p_from, p_to };
	for (int cut : cuts) {
		Vector<int> originals;
		Vector<int> new_ids;
		const int count = next.clips.size();
		for (int i = 0; i < count; i++) {
			const Clip &clip = next.clips[i];
			if (tracks.has(clip.track) && cut > clip.position && cut < clip.end()) {
				originals.push_back(clip.id);
				new_ids.push_back(_split_raw(next, i, cut));
			}
		}
		_relink_split(next, originals, new_ids);
	}
	bool removed = false;
	for (int i = next.clips.size() - 1; i >= 0; i--) {
		const Clip &clip = next.clips[i];
		if (tracks.has(clip.track) && clip.position >= p_from && clip.end() <= p_to) {
			_remove_clip_at(next, i);
			removed = true;
		}
	}
	if (!removed) {
		return false;
	}
	_commit(TTR("Lift Range"), next);
	return true;
}

bool TetraVideoProject::extract_range(int p_from, int p_to, const PackedInt32Array &p_tracks) {
	Vector<int> tracks;
	if (p_from < 0 || p_to <= p_from) {
		return _fail("Illegal frame range.");
	}
	if (!_resolve_tracks(p_tracks, tracks)) {
		return _fail("A track is missing or locked.");
	}
	begin_undo_group(TTR("Extract Range"));
	bool changed = lift_range(p_from, p_to, p_tracks);
	State next = state;
	bool moved = false;
	for (int i = 0; i < next.clips.size(); i++) {
		if (tracks.has(next.clips[i].track) && next.clips[i].position >= p_to) {
			next.clips.write[i].position -= p_to - p_from;
			moved = true;
		}
	}
	if (moved) {
		_commit(TTR("Extract Range"), next);
		changed = true;
	}
	end_undo_group();
	return changed;
}

bool TetraVideoProject::insert_space(int p_position, int p_length, int p_track) {
	if (p_position < 0 || p_length < 1) {
		return _fail("Illegal position or length.");
	}
	PackedInt32Array requested;
	if (p_track >= 0) {
		requested.push_back(p_track);
	}
	Vector<int> tracks;
	if (!_resolve_tracks(requested, tracks)) {
		return _fail("The track is missing or locked.");
	}
	State next = state;
	bool moved = false;
	for (int i = 0; i < next.clips.size(); i++) {
		if (tracks.has(next.clips[i].track) && next.clips[i].position >= p_position) {
			next.clips.write[i].position += p_length;
			moved = true;
		}
	}
	if (!moved) {
		return false;
	}
	_commit(TTR("Insert Space"), next);
	return true;
}

int TetraVideoProject::remove_space(int p_position, int p_track) {
	PackedInt32Array requested;
	if (p_track >= 0) {
		requested.push_back(p_track);
	}
	Vector<int> tracks;
	if (p_position < 0 || !_resolve_tracks(requested, tracks)) {
		_fail("The track is missing or locked.");
		return 0;
	}
	// Each track has a gap around the position; the narrowest one bounds how far everything can close up.
	int amount = INT_MAX;
	for (int track : tracks) {
		int gap_start = 0;
		int gap_end = INT_MAX;
		for (const Clip &clip : state.clips) {
			if (clip.track != track) {
				continue;
			}
			if (clip.position <= p_position && clip.end() > p_position) {
				_fail("The position is not in a gap.");
				return 0;
			}
			if (clip.end() <= p_position) {
				gap_start = MAX(gap_start, clip.end());
			} else {
				gap_end = MIN(gap_end, clip.position);
			}
		}
		if (gap_end != INT_MAX) {
			amount = MIN(amount, gap_end - gap_start);
		}
	}
	if (amount == INT_MAX || amount < 1) {
		return 0;
	}
	State next = state;
	for (int i = 0; i < next.clips.size(); i++) {
		if (tracks.has(next.clips[i].track) && next.clips[i].position > p_position) {
			next.clips.write[i].position -= amount;
		}
	}
	_commit(TTR("Remove Space"), next);
	return amount;
}

int TetraVideoProject::duplicate_clip(int p_id, int p_track, int p_position) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		_fail("No such clip.");
		return -1;
	}
	const Clip &clip = state.clips[index];
	const int track = p_track < 0 ? clip.track : p_track;
	const int position = p_position < 0 ? clip.end() : p_position;
	if (track >= state.tracks.size() || !_track_accepts(state, track, state.bin[_find_bin(state, clip.bin_id)])) {
		_fail("The clip does not fit this track.");
		return -1;
	}
	if (state.tracks[track].locked) {
		_fail("The track is locked.");
		return -1;
	}
	State next = state;
	Clip copy = clip;
	copy.id = next.next_clip_id++;
	copy.track = track;
	copy.position = position;
	copy.link = 0;
	if (clip.link != 0) {
		const Clip &partner = state.clips[_find_clip(state, clip.link)];
		int partner_track = partner.track;
		if (track != clip.track) {
			if (state.tracks[partner.track].kind != state.tracks[clip.track].kind) {
				partner_track = _paired_track(state, track);
			} else {
				partner_track = partner.track + (track - clip.track);
			}
		}
		if (!_track_accepts(state, partner_track, state.bin[_find_bin(state, partner.bin_id)]) || partner_track == track || state.tracks[partner_track].locked) {
			_fail("The linked clip has no track to be copied to.");
			return -1;
		}
		Clip partner_copy = partner;
		partner_copy.id = next.next_clip_id++;
		partner_copy.track = partner_track;
		partner_copy.position = partner.position + (position - clip.position);
		partner_copy.link = copy.id;
		copy.link = partner_copy.id;
		next.clips.push_back(copy);
		next.clips.push_back(partner_copy);
	} else {
		next.clips.push_back(copy);
	}
	if (!_layout_ok(next)) {
		_fail("The copy would overlap another clip.");
		return -1;
	}
	_commit(TTR("Duplicate Clip"), next);
	return copy.id;
}

bool TetraVideoProject::link_clips(int p_a, int p_b) {
	const int a = _find_clip(state, p_a);
	const int b = _find_clip(state, p_b);
	if (a < 0 || b < 0 || a == b) {
		return _fail("No such clips.");
	}
	if (state.clips[a].link != 0 || state.clips[b].link != 0) {
		return _fail("A clip is already linked.");
	}
	if (state.clips[a].track == state.clips[b].track) {
		return _fail("Clips on the same track cannot be linked.");
	}
	State next = state;
	next.clips.write[a].link = p_b;
	next.clips.write[b].link = p_a;
	_commit(TTR("Link Clips"), next);
	return true;
}

bool TetraVideoProject::unlink_clip(int p_id) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	if (state.clips[index].link == 0) {
		return false;
	}
	State next = state;
	const int partner = _find_clip(next, next.clips[index].link);
	next.clips.write[index].link = 0;
	next.clips.write[partner].link = 0;
	_commit(TTR("Unlink Clips"), next);
	return true;
}

int TetraVideoProject::get_linked_clip(int p_id) const {
	const int index = _find_clip(state, p_id);
	return index < 0 || state.clips[index].link == 0 ? -1 : state.clips[index].link;
}

bool TetraVideoProject::set_clip_fades(int p_id, int p_fade_in, int p_fade_out) {
	const int index = _find_clip(state, p_id);
	if (index < 0) {
		return _fail("No such clip.");
	}
	if (!_clip_editable(state, index)) {
		return _fail("The track is locked.");
	}
	Clip clip = state.clips[index];
	clip.fade_in = p_fade_in;
	clip.fade_out = p_fade_out;
	_clamp_fades(clip);
	if (clip.fade_in == state.clips[index].fade_in && clip.fade_out == state.clips[index].fade_out) {
		return true;
	}
	State next = state;
	next.clips.write[index] = clip;
	_commit(TTR("Change Fades"), next);
	return true;
}

// Effects.

int TetraVideoProject::add_clip_effect(int p_clip_id, const String &p_operation, const String &p_service, const Dictionary &p_values, int p_index) {
	const int index = _find_clip(state, p_clip_id);
	if (index < 0 || p_service.is_empty()) {
		_fail("No such clip, or no MLT service given.");
		return -1;
	}
	if (!_clip_editable(state, index)) {
		_fail("The track is locked.");
		return -1;
	}
	State next = state;
	Effect effect;
	effect.operation = p_operation.is_empty() ? "mlt:" + p_service : p_operation;
	effect.service = p_service;
	effect.values = p_values.duplicate(true);
	Vector<Effect> effects = next.clips[index].effects;
	const int at = p_index < 0 ? effects.size() : MIN(p_index, effects.size());
	effects.insert(at, effect);
	next.clips.write[index].effects = effects;
	_commit(TTR("Add Effect"), next);
	return at;
}

bool TetraVideoProject::_edit_effect(int p_clip_id, int p_index, State &r_next, int &r_clip_index) const {
	r_clip_index = _find_clip(state, p_clip_id);
	if (r_clip_index < 0 || p_index < 0 || p_index >= state.clips[r_clip_index].effects.size()) {
		return false;
	}
	if (!_clip_editable(state, r_clip_index)) {
		return false;
	}
	r_next = state;
	return true;
}

bool TetraVideoProject::remove_clip_effect(int p_clip_id, int p_index) {
	State next;
	int index;
	if (!_edit_effect(p_clip_id, p_index, next, index)) {
		return _fail("No such effect, or the track is locked.");
	}
	Vector<Effect> effects = next.clips[index].effects;
	effects.remove_at(p_index);
	next.clips.write[index].effects = effects;
	_commit(TTR("Remove Effect"), next);
	return true;
}

bool TetraVideoProject::move_clip_effect(int p_clip_id, int p_from, int p_to) {
	State next;
	int index;
	if (!_edit_effect(p_clip_id, p_from, next, index) || p_to < 0 || p_to >= state.clips[index].effects.size()) {
		return _fail("No such effect, or the track is locked.");
	}
	if (p_from == p_to) {
		return true;
	}
	Vector<Effect> effects = next.clips[index].effects;
	const Effect moved = effects[p_from];
	effects.remove_at(p_from);
	effects.insert(p_to, moved);
	next.clips.write[index].effects = effects;
	_commit(TTR("Move Effect"), next);
	return true;
}

bool TetraVideoProject::set_clip_effect_enabled(int p_clip_id, int p_index, bool p_enabled) {
	State next;
	int index;
	if (!_edit_effect(p_clip_id, p_index, next, index)) {
		return _fail("No such effect, or the track is locked.");
	}
	if (state.clips[index].effects[p_index].enabled == p_enabled) {
		return true;
	}
	Vector<Effect> effects = next.clips[index].effects;
	effects.write[p_index].enabled = p_enabled;
	next.clips.write[index].effects = effects;
	_commit(p_enabled ? TTR("Enable Effect") : TTR("Disable Effect"), next);
	return true;
}

bool TetraVideoProject::set_clip_effect_values(int p_clip_id, int p_index, const Dictionary &p_values) {
	State next;
	int index;
	if (!_edit_effect(p_clip_id, p_index, next, index)) {
		return _fail("No such effect, or the track is locked.");
	}
	Vector<Effect> effects = next.clips[index].effects;
	// The stored dictionary is shared with the undo history, so it is replaced, never edited in place.
	effects.write[p_index].values = p_values.duplicate(true);
	next.clips.write[index].effects = effects;
	_commit(TTR("Change Effect"), next);
	return true;
}

bool TetraVideoProject::set_clip_effect_value(int p_clip_id, int p_index, const String &p_name, const Variant &p_value) {
	const int index = _find_clip(state, p_clip_id);
	if (index < 0 || p_index < 0 || p_index >= state.clips[index].effects.size()) {
		return _fail("No such effect.");
	}
	Dictionary values = state.clips[index].effects[p_index].values.duplicate(true);
	values[p_name] = p_value;
	return set_clip_effect_values(p_clip_id, p_index, values);
}

int TetraVideoProject::get_clip_effect_count(int p_clip_id) const {
	const int index = _find_clip(state, p_clip_id);
	return index < 0 ? 0 : state.clips[index].effects.size();
}

TypedArray<Dictionary> TetraVideoProject::get_clip_effects(int p_clip_id) const {
	TypedArray<Dictionary> result;
	const int index = _find_clip(state, p_clip_id);
	if (index < 0) {
		return result;
	}
	for (const Effect &effect : state.clips[index].effects) {
		Dictionary d;
		d["operation"] = effect.operation;
		d["service"] = effect.service;
		d["values"] = effect.values.duplicate(true);
		d["enabled"] = effect.enabled;
		result.push_back(d);
	}
	return result;
}

// Markers.

bool TetraVideoProject::add_marker(int p_frame, const String &p_text) {
	if (p_frame < 0) {
		return _fail("Illegal frame.");
	}
	State next = state;
	int at = 0;
	while (at < next.markers.size() && next.markers[at].frame < p_frame) {
		at++;
	}
	Marker marker;
	marker.frame = p_frame;
	marker.text = p_text;
	if (at < next.markers.size() && next.markers[at].frame == p_frame) {
		if (next.markers[at].text == p_text) {
			return true;
		}
		next.markers.write[at] = marker;
		_commit(TTR("Edit Marker"), next);
	} else {
		next.markers.insert(at, marker);
		_commit(TTR("Add Marker"), next);
	}
	return true;
}

bool TetraVideoProject::remove_marker(int p_frame) {
	for (int i = 0; i < state.markers.size(); i++) {
		if (state.markers[i].frame == p_frame) {
			State next = state;
			next.markers.remove_at(i);
			_commit(TTR("Remove Marker"), next);
			return true;
		}
	}
	return _fail("No marker at this frame.");
}

bool TetraVideoProject::move_marker(int p_from, int p_to) {
	if (!has_marker(p_from) || p_to < 0 || (p_to != p_from && has_marker(p_to))) {
		return _fail("No marker to move, or the target frame is taken.");
	}
	if (p_from == p_to) {
		return true;
	}
	String text;
	for (const Marker &marker : state.markers) {
		if (marker.frame == p_from) {
			text = marker.text;
		}
	}
	begin_undo_group(TTR("Move Marker"));
	remove_marker(p_from);
	add_marker(p_to, text);
	end_undo_group();
	return true;
}

bool TetraVideoProject::has_marker(int p_frame) const {
	for (const Marker &marker : state.markers) {
		if (marker.frame == p_frame) {
			return true;
		}
	}
	return false;
}

TypedArray<Dictionary> TetraVideoProject::get_markers() const {
	TypedArray<Dictionary> result;
	for (const Marker &marker : state.markers) {
		Dictionary d;
		d["frame"] = marker.frame;
		d["text"] = marker.text;
		result.push_back(d);
	}
	return result;
}

// Queries.

bool TetraVideoProject::has_clip(int p_id) const {
	return _find_clip(state, p_id) >= 0;
}

Dictionary TetraVideoProject::_clip_to_dictionary(const Clip &p_clip) const {
	Dictionary d;
	d["id"] = p_clip.id;
	d["bin_id"] = p_clip.bin_id;
	d["track"] = p_clip.track;
	d["position"] = p_clip.position;
	d["in"] = p_clip.in;
	d["out"] = p_clip.out;
	d["length"] = p_clip.length();
	d["link"] = p_clip.link;
	d["fade_in"] = p_clip.fade_in;
	d["fade_out"] = p_clip.fade_out;
	d["effects"] = get_clip_effects(p_clip.id);
	return d;
}

Dictionary TetraVideoProject::get_clip(int p_id) const {
	const int index = _find_clip(state, p_id);
	return index < 0 ? Dictionary() : _clip_to_dictionary(state.clips[index]);
}

PackedInt32Array TetraVideoProject::get_clip_ids() const {
	PackedInt32Array ids;
	for (const Clip &clip : state.clips) {
		ids.push_back(clip.id);
	}
	ids.sort();
	return ids;
}

int TetraVideoProject::get_clip_at(int p_track, int p_frame) const {
	for (const Clip &clip : state.clips) {
		if (clip.track == p_track && p_frame >= clip.position && p_frame < clip.end()) {
			return clip.id;
		}
	}
	return -1;
}

PackedInt32Array TetraVideoProject::get_clips_in_track(int p_track) const {
	PackedInt32Array ids;
	for (int index : _sorted_track_clips(state, p_track)) {
		ids.push_back(state.clips[index].id);
	}
	return ids;
}

int TetraVideoProject::get_duration() const {
	int duration = 0;
	for (const Clip &clip : state.clips) {
		duration = MAX(duration, clip.end());
	}
	return duration;
}

PackedInt32Array TetraVideoProject::get_snap_points(const PackedInt32Array &p_ignore_ids) const {
	PackedInt32Array points;
	points.push_back(0);
	for (const Clip &clip : state.clips) {
		if (!p_ignore_ids.has(clip.id)) {
			points.push_back(clip.position);
			points.push_back(clip.end());
		}
	}
	for (const Marker &marker : state.markers) {
		points.push_back(marker.frame);
	}
	points.sort();
	PackedInt32Array unique;
	for (int point : points) {
		if (unique.is_empty() || unique[unique.size() - 1] != point) {
			unique.push_back(point);
		}
	}
	return unique;
}

int TetraVideoProject::find_snap(int p_frame, int p_tolerance, const PackedInt32Array &p_ignore_ids) const {
	int best = -1;
	for (int point : get_snap_points(p_ignore_ids)) {
		if (Math::abs(point - p_frame) <= p_tolerance && (best < 0 || Math::abs(point - p_frame) < Math::abs(best - p_frame))) {
			best = point;
		}
	}
	return best;
}

bool TetraVideoProject::is_range_free(int p_track, int p_from, int p_to, const PackedInt32Array &p_ignore_ids) const {
	if (p_track < 0 || p_track >= state.tracks.size() || p_from < 0 || p_to < p_from) {
		return false;
	}
	for (const Clip &clip : state.clips) {
		if (clip.track == p_track && clip.position < p_to && clip.end() > p_from && !p_ignore_ids.has(clip.id)) {
			return false;
		}
	}
	return true;
}

String TetraVideoProject::check_consistency() const {
	return _check_state(state);
}

Dictionary TetraVideoProject::get_state() const {
	Dictionary d;
	Dictionary profile;
	profile["width"] = state.width;
	profile["height"] = state.height;
	profile["fps_num"] = state.fps_num;
	profile["fps_den"] = state.fps_den;
	profile["sample_rate"] = state.sample_rate;
	d["profile"] = profile;
	Array bin;
	for (const BinClip &clip : state.bin) {
		bin.push_back(_bin_to_dictionary(clip));
	}
	d["bin"] = bin;
	Array tracks;
	for (int i = 0; i < state.tracks.size(); i++) {
		tracks.push_back(get_track(i));
	}
	d["tracks"] = tracks;
	Array clips;
	for (int id : get_clip_ids()) {
		clips.push_back(get_clip(id));
	}
	d["clips"] = clips;
	d["markers"] = get_markers();
	d["next_bin_id"] = state.next_bin_id;
	d["next_clip_id"] = state.next_clip_id;
	return d;
}

// Undo.

bool TetraVideoProject::undo() {
	if (!can_undo() || group_depth > 0) {
		return false;
	}
	undo_position--;
	_apply_state(undo_stack[undo_position].before);
	return true;
}

bool TetraVideoProject::redo() {
	if (!can_redo() || group_depth > 0) {
		return false;
	}
	_apply_state(undo_stack[undo_position].after);
	undo_position++;
	return true;
}

String TetraVideoProject::get_undo_name() const {
	return can_undo() ? undo_stack[undo_position - 1].name : String();
}

String TetraVideoProject::get_redo_name() const {
	return can_redo() ? undo_stack[undo_position].name : String();
}

void TetraVideoProject::clear_undo_history() {
	undo_stack.clear();
	undo_position = 0;
}

// Operations between begin and end are recorded as a single undo step, which
// is how a drag that calls move_clip() many times stays one step.
void TetraVideoProject::begin_undo_group(const String &p_name) {
	if (group_depth++ == 0) {
		group_name = p_name;
		group_before = state;
		group_changed = false;
	}
}

void TetraVideoProject::end_undo_group() {
	ERR_FAIL_COND(group_depth == 0);
	if (--group_depth > 0 || !group_changed) {
		return;
	}
	undo_stack.resize(undo_position);
	UndoStep step;
	step.name = group_name;
	step.before = group_before;
	step.after = state;
	undo_stack.push_back(step);
	if (undo_stack.size() > max_undo_steps) {
		undo_stack.remove_at(0);
	}
	undo_position = undo_stack.size();
	group_before = State();
}

void TetraVideoProject::set_max_undo_steps(int p_steps) {
	max_undo_steps = MAX(1, p_steps);
	while (undo_stack.size() > max_undo_steps) {
		undo_stack.remove_at(0);
		undo_position = MAX(0, undo_position - 1);
	}
}

void TetraVideoProject::clear() {
	clear_undo_history();
	_apply_state(State());
}

void TetraVideoProject::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_profile", "width", "height", "fps_num", "fps_den", "sample_rate"), &TetraVideoProject::set_profile);
	ClassDB::bind_method(D_METHOD("set_profile_preset", "name"), &TetraVideoProject::set_profile_preset);
	ClassDB::bind_static_method("TetraVideoProject", D_METHOD("get_profile_presets"), &TetraVideoProject::get_profile_presets);
	ClassDB::bind_method(D_METHOD("get_width"), &TetraVideoProject::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &TetraVideoProject::get_height);
	ClassDB::bind_method(D_METHOD("get_fps_num"), &TetraVideoProject::get_fps_num);
	ClassDB::bind_method(D_METHOD("get_fps_den"), &TetraVideoProject::get_fps_den);
	ClassDB::bind_method(D_METHOD("get_fps"), &TetraVideoProject::get_fps);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &TetraVideoProject::get_sample_rate);

	ClassDB::bind_method(D_METHOD("add_bin_clip", "kind", "name", "source", "length", "has_video", "has_audio", "properties"), &TetraVideoProject::add_bin_clip, DEFVAL(true), DEFVAL(false), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("add_bin_color", "name", "color", "length"), &TetraVideoProject::add_bin_color, DEFVAL(125));
	ClassDB::bind_method(D_METHOD("add_bin_text", "name", "text", "length", "properties"), &TetraVideoProject::add_bin_text, DEFVAL(125), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("remove_bin_clip", "id", "cascade"), &TetraVideoProject::remove_bin_clip, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_bin_clip_name", "id", "name"), &TetraVideoProject::set_bin_clip_name);
	ClassDB::bind_method(D_METHOD("set_bin_clip_source", "id", "source"), &TetraVideoProject::set_bin_clip_source);
	ClassDB::bind_method(D_METHOD("set_bin_clip_properties", "id", "properties"), &TetraVideoProject::set_bin_clip_properties);
	ClassDB::bind_method(D_METHOD("has_bin_clip", "id"), &TetraVideoProject::has_bin_clip);
	ClassDB::bind_method(D_METHOD("get_bin_clip", "id"), &TetraVideoProject::get_bin_clip);
	ClassDB::bind_method(D_METHOD("get_bin_clip_ids"), &TetraVideoProject::get_bin_clip_ids);
	ClassDB::bind_method(D_METHOD("get_bin_clip_use_count", "id"), &TetraVideoProject::get_bin_clip_use_count);

	ClassDB::bind_method(D_METHOD("add_track", "kind", "name", "index"), &TetraVideoProject::add_track, DEFVAL(String()), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_track", "index"), &TetraVideoProject::remove_track);
	ClassDB::bind_method(D_METHOD("move_track", "from", "to"), &TetraVideoProject::move_track);
	ClassDB::bind_method(D_METHOD("set_track_name", "index", "name"), &TetraVideoProject::set_track_name);
	ClassDB::bind_method(D_METHOD("set_track_hidden", "index", "hidden"), &TetraVideoProject::set_track_hidden);
	ClassDB::bind_method(D_METHOD("set_track_muted", "index", "muted"), &TetraVideoProject::set_track_muted);
	ClassDB::bind_method(D_METHOD("set_track_locked", "index", "locked"), &TetraVideoProject::set_track_locked);
	ClassDB::bind_method(D_METHOD("get_track_count"), &TetraVideoProject::get_track_count);
	ClassDB::bind_method(D_METHOD("get_track", "index"), &TetraVideoProject::get_track);
	ClassDB::bind_method(D_METHOD("get_track_kind", "index"), &TetraVideoProject::get_track_kind);
	ClassDB::bind_method(D_METHOD("get_track_name", "index"), &TetraVideoProject::get_track_name);
	ClassDB::bind_method(D_METHOD("is_track_hidden", "index"), &TetraVideoProject::is_track_hidden);
	ClassDB::bind_method(D_METHOD("is_track_muted", "index"), &TetraVideoProject::is_track_muted);
	ClassDB::bind_method(D_METHOD("is_track_locked", "index"), &TetraVideoProject::is_track_locked);
	ClassDB::bind_method(D_METHOD("get_paired_track", "index"), &TetraVideoProject::get_paired_track);

	ClassDB::bind_method(D_METHOD("insert_clip", "bin_id", "track", "position", "in", "out", "with_linked"), &TetraVideoProject::insert_clip, DEFVAL(-1), DEFVAL(-1), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("move_clip", "id", "track", "position"), &TetraVideoProject::move_clip);
	ClassDB::bind_method(D_METHOD("trim_clip_start", "id", "frame"), &TetraVideoProject::trim_clip_start);
	ClassDB::bind_method(D_METHOD("trim_clip_end", "id", "frame"), &TetraVideoProject::trim_clip_end);
	ClassDB::bind_method(D_METHOD("split_clip", "id", "frame"), &TetraVideoProject::split_clip);
	ClassDB::bind_method(D_METHOD("split_all_at", "frame"), &TetraVideoProject::split_all_at);
	ClassDB::bind_method(D_METHOD("delete_clip", "id"), &TetraVideoProject::delete_clip);
	ClassDB::bind_method(D_METHOD("delete_clips", "ids"), &TetraVideoProject::delete_clips);
	ClassDB::bind_method(D_METHOD("ripple_delete", "id"), &TetraVideoProject::ripple_delete);
	ClassDB::bind_method(D_METHOD("lift_range", "from", "to", "tracks"), &TetraVideoProject::lift_range, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("extract_range", "from", "to", "tracks"), &TetraVideoProject::extract_range, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("insert_space", "position", "length", "track"), &TetraVideoProject::insert_space, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_space", "position", "track"), &TetraVideoProject::remove_space, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("duplicate_clip", "id", "track", "position"), &TetraVideoProject::duplicate_clip, DEFVAL(-1), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("link_clips", "a", "b"), &TetraVideoProject::link_clips);
	ClassDB::bind_method(D_METHOD("unlink_clip", "id"), &TetraVideoProject::unlink_clip);
	ClassDB::bind_method(D_METHOD("get_linked_clip", "id"), &TetraVideoProject::get_linked_clip);
	ClassDB::bind_method(D_METHOD("set_clip_fades", "id", "fade_in", "fade_out"), &TetraVideoProject::set_clip_fades);

	ClassDB::bind_method(D_METHOD("add_clip_effect", "clip_id", "operation", "service", "values", "index"), &TetraVideoProject::add_clip_effect, DEFVAL(Dictionary()), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_clip_effect", "clip_id", "index"), &TetraVideoProject::remove_clip_effect);
	ClassDB::bind_method(D_METHOD("move_clip_effect", "clip_id", "from", "to"), &TetraVideoProject::move_clip_effect);
	ClassDB::bind_method(D_METHOD("set_clip_effect_enabled", "clip_id", "index", "enabled"), &TetraVideoProject::set_clip_effect_enabled);
	ClassDB::bind_method(D_METHOD("set_clip_effect_values", "clip_id", "index", "values"), &TetraVideoProject::set_clip_effect_values);
	ClassDB::bind_method(D_METHOD("set_clip_effect_value", "clip_id", "index", "name", "value"), &TetraVideoProject::set_clip_effect_value);
	ClassDB::bind_method(D_METHOD("get_clip_effect_count", "clip_id"), &TetraVideoProject::get_clip_effect_count);
	ClassDB::bind_method(D_METHOD("get_clip_effects", "clip_id"), &TetraVideoProject::get_clip_effects);

	ClassDB::bind_method(D_METHOD("add_marker", "frame", "text"), &TetraVideoProject::add_marker, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("remove_marker", "frame"), &TetraVideoProject::remove_marker);
	ClassDB::bind_method(D_METHOD("move_marker", "from", "to"), &TetraVideoProject::move_marker);
	ClassDB::bind_method(D_METHOD("has_marker", "frame"), &TetraVideoProject::has_marker);
	ClassDB::bind_method(D_METHOD("get_markers"), &TetraVideoProject::get_markers);

	ClassDB::bind_method(D_METHOD("has_clip", "id"), &TetraVideoProject::has_clip);
	ClassDB::bind_method(D_METHOD("get_clip", "id"), &TetraVideoProject::get_clip);
	ClassDB::bind_method(D_METHOD("get_clip_ids"), &TetraVideoProject::get_clip_ids);
	ClassDB::bind_method(D_METHOD("get_clip_at", "track", "frame"), &TetraVideoProject::get_clip_at);
	ClassDB::bind_method(D_METHOD("get_clips_in_track", "track"), &TetraVideoProject::get_clips_in_track);
	ClassDB::bind_method(D_METHOD("get_duration"), &TetraVideoProject::get_duration);
	ClassDB::bind_method(D_METHOD("get_snap_points", "ignore_ids"), &TetraVideoProject::get_snap_points, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("find_snap", "frame", "tolerance", "ignore_ids"), &TetraVideoProject::find_snap, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("is_range_free", "track", "from", "to", "ignore_ids"), &TetraVideoProject::is_range_free, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("check_consistency"), &TetraVideoProject::check_consistency);
	ClassDB::bind_method(D_METHOD("get_revision"), &TetraVideoProject::get_revision);
	ClassDB::bind_method(D_METHOD("get_last_error"), &TetraVideoProject::get_last_error);
	ClassDB::bind_method(D_METHOD("get_state"), &TetraVideoProject::get_state);

	ClassDB::bind_method(D_METHOD("undo"), &TetraVideoProject::undo);
	ClassDB::bind_method(D_METHOD("redo"), &TetraVideoProject::redo);
	ClassDB::bind_method(D_METHOD("can_undo"), &TetraVideoProject::can_undo);
	ClassDB::bind_method(D_METHOD("can_redo"), &TetraVideoProject::can_redo);
	ClassDB::bind_method(D_METHOD("get_undo_name"), &TetraVideoProject::get_undo_name);
	ClassDB::bind_method(D_METHOD("get_redo_name"), &TetraVideoProject::get_redo_name);
	ClassDB::bind_method(D_METHOD("get_undo_count"), &TetraVideoProject::get_undo_count);
	ClassDB::bind_method(D_METHOD("clear_undo_history"), &TetraVideoProject::clear_undo_history);
	ClassDB::bind_method(D_METHOD("begin_undo_group", "name"), &TetraVideoProject::begin_undo_group);
	ClassDB::bind_method(D_METHOD("end_undo_group"), &TetraVideoProject::end_undo_group);
	ClassDB::bind_method(D_METHOD("set_max_undo_steps", "steps"), &TetraVideoProject::set_max_undo_steps);
	ClassDB::bind_method(D_METHOD("get_max_undo_steps"), &TetraVideoProject::get_max_undo_steps);

	ClassDB::bind_method(D_METHOD("to_mlt_xml", "options"), &TetraVideoProject::to_mlt_xml, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("save", "path"), &TetraVideoProject::save);
	ClassDB::bind_method(D_METHOD("load", "path"), &TetraVideoProject::load);
	ClassDB::bind_method(D_METHOD("load_from_xml", "xml"), &TetraVideoProject::load_from_xml);
	ClassDB::bind_method(D_METHOD("clear"), &TetraVideoProject::clear);
	ClassDB::bind_static_method("TetraVideoProject", D_METHOD("value_to_mlt", "value"), &TetraVideoProject::value_to_mlt);

	ADD_SIGNAL(MethodInfo("changed"));
	ADD_SIGNAL(MethodInfo("structure_changed"));

	BIND_ENUM_CONSTANT(CLIP_VIDEO);
	BIND_ENUM_CONSTANT(CLIP_AUDIO);
	BIND_ENUM_CONSTANT(CLIP_IMAGE);
	BIND_ENUM_CONSTANT(CLIP_COLOR);
	BIND_ENUM_CONSTANT(CLIP_TEXT);
	BIND_ENUM_CONSTANT(TRACK_VIDEO);
	BIND_ENUM_CONSTANT(TRACK_AUDIO);
}
