#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

// The timeline document of the Video workspace: profile, bin, tracks, clips,
// effects and markers, edited with Kdenlive's rules (clips on a track never
// overlap, locked tracks reject edits, an operation that cannot be done is
// refused as a whole).
//
// The whole document lives in one value type made of copy-on-write vectors.
// Every operation edits a copy and commits it only when the result is legal,
// and the undo stack stores the document before and after each step, which
// makes undo and redo exact by construction.
class TetraVideoProject : public RefCounted {
	GDCLASS(TetraVideoProject, RefCounted);

public:
	enum ClipKind {
		CLIP_VIDEO,
		CLIP_AUDIO,
		CLIP_IMAGE,
		CLIP_COLOR,
		CLIP_TEXT,
	};

	enum TrackKind {
		TRACK_VIDEO,
		TRACK_AUDIO,
	};

	// How a value travels from one keyframe to the next. The numbers are MLT's mlt_keyframe_type.
	enum KeyframeType {
		KEYFRAME_DISCRETE,
		KEYFRAME_LINEAR,
		KEYFRAME_SMOOTH,
		KEYFRAME_SMOOTH_NATURAL,
		KEYFRAME_SMOOTH_TIGHT,
		KEYFRAME_SINUSOIDAL_IN,
		KEYFRAME_SINUSOIDAL_OUT,
		KEYFRAME_SINUSOIDAL_IN_OUT,
		KEYFRAME_QUADRATIC_IN,
		KEYFRAME_QUADRATIC_OUT,
		KEYFRAME_QUADRATIC_IN_OUT,
		KEYFRAME_CUBIC_IN,
		KEYFRAME_CUBIC_OUT,
		KEYFRAME_CUBIC_IN_OUT,
		KEYFRAME_QUARTIC_IN,
		KEYFRAME_QUARTIC_OUT,
		KEYFRAME_QUARTIC_IN_OUT,
		KEYFRAME_QUINTIC_IN,
		KEYFRAME_QUINTIC_OUT,
		KEYFRAME_QUINTIC_IN_OUT,
		KEYFRAME_EXPONENTIAL_IN,
		KEYFRAME_EXPONENTIAL_OUT,
		KEYFRAME_EXPONENTIAL_IN_OUT,
		KEYFRAME_CIRCULAR_IN,
		KEYFRAME_CIRCULAR_OUT,
		KEYFRAME_CIRCULAR_IN_OUT,
		KEYFRAME_BACK_IN,
		KEYFRAME_BACK_OUT,
		KEYFRAME_BACK_IN_OUT,
		KEYFRAME_ELASTIC_IN,
		KEYFRAME_ELASTIC_OUT,
		KEYFRAME_ELASTIC_IN_OUT,
		KEYFRAME_BOUNCE_IN,
		KEYFRAME_BOUNCE_OUT,
		KEYFRAME_BOUNCE_IN_OUT,
		KEYFRAME_TYPE_MAX,
	};

private:
	struct Keyframe {
		// Frames from the start of the clip.
		int frame = 0;
		// In the parameter's display units, like the constant values.
		double value = 0.0;
		// Governs the stretch from this keyframe to the next one.
		KeyframeType type = KEYFRAME_LINEAR;
	};

	// The keyframes of one animated parameter, ordered by frame, never empty.
	struct KeyframeTrack {
		String param;
		Vector<Keyframe> keys;
	};

	struct Effect {
		String operation;
		String service;
		Dictionary values;
		bool enabled = true;
		// Parameters that follow keyframes instead of their entry in values.
		Vector<KeyframeTrack> keyframes;
	};

	struct BinClip {
		int id = 0;
		ClipKind kind = CLIP_VIDEO;
		String name;
		// File path for media and images, "#rrggbbaa" for colours, the text for titles.
		String source;
		int length = 0;
		bool has_video = true;
		bool has_audio = false;
		// Extra MLT producer properties (font of a title, stream indexes...).
		Dictionary properties;
	};

	struct Track {
		TrackKind kind = TRACK_VIDEO;
		String name;
		bool hidden = false;
		bool muted = false;
		bool locked = false;
		// Identifies the track while this project object lives, whatever its index becomes. Not saved.
		int uid = 0;
	};

	struct Clip {
		int id = 0;
		int bin_id = 0;
		int track = 0;
		int position = 0;
		int in = 0;
		int out = 0;
		// Id of the clip this one moves, trims, splits and dies with; 0 when unlinked.
		int link = 0;
		int fade_in = 0;
		int fade_out = 0;
		Vector<Effect> effects;

		int length() const { return out - in + 1; }
		int end() const { return position + out - in + 1; }
	};

	struct Marker {
		int frame = 0;
		String text;
	};

	struct State {
		int width = 1920;
		int height = 1080;
		int fps_num = 25;
		int fps_den = 1;
		int sample_rate = 48000;
		Vector<BinClip> bin;
		Vector<Track> tracks;
		Vector<Clip> clips;
		Vector<Marker> markers;
		int next_bin_id = 1;
		int next_clip_id = 1;
	};

	struct UndoStep {
		String name;
		State before;
		State after;
	};

	State state;
	Vector<UndoStep> undo_stack;
	int undo_position = 0;
	int max_undo_steps = 200;
	int group_depth = 0;
	bool group_changed = false;
	String group_name;
	State group_before;
	uint64_t revision = 0;
	String last_error;
	int next_track_uid = 1;
	// Loaded from MLT XML of another program, which the model only keeps in part.
	bool foreign = false;

	static int _find_clip(const State &p_state, int p_id);
	static int _find_bin(const State &p_state, int p_id);
	static bool _bin_is_still(const BinClip &p_bin);
	static void _rescale_frames(State &r_state, int64_t p_num, int64_t p_den);
	static bool _layout_ok(const State &p_state);
	static bool _track_accepts(const State &p_state, int p_track, const BinClip &p_bin);
	static int _paired_track(const State &p_state, int p_track);
	static int _split_raw(State &r_state, int p_clip_index, int p_frame);
	static void _relink_split(State &r_state, const Vector<int> &p_originals, const Vector<int> &p_new_ids);
	static void _remove_clip_at(State &r_state, int p_index);
	static void _clamp_fades(Clip &r_clip);
	static String _check_state(const State &p_state);
	static Vector<int> _sorted_track_clips(const State &p_state, int p_track);
	bool _clip_editable(const State &p_state, int p_clip_index) const;
	bool _resolve_tracks(const PackedInt32Array &p_tracks, Vector<int> &r_tracks) const;
	bool _edit_effect(int p_clip_id, int p_index, State &r_next, int &r_clip_index) const;

	// Keyframes (tetra_video_keyframes.cpp).
	static double _keyframes_value_at(const Vector<Keyframe> &p_keys, int p_frame);
	static int _keyframe_index(const Vector<Keyframe> &p_keys, int p_frame);
	static int _keyframe_before(const Vector<Keyframe> &p_keys, int p_frame);
	static String _keyframes_to_mlt(const Vector<Keyframe> &p_keys, double p_factor, bool p_integer);
	static bool _keyframes_from_mlt(const String &p_text, double p_factor, double p_fps, Vector<Keyframe> &r_keys);
	static Array _keyframes_to_array(const Vector<Keyframe> &p_keys);
	static bool _keyframes_from_array(const Array &p_array, Vector<Keyframe> &r_keys);
	static Dictionary _effect_keyframes_to_dictionary(const Effect &p_effect);
	static void _effect_keyframes_from_dictionary(const Dictionary &p_dictionary, Effect &r_effect);
	static int _find_keyframe_track(const Effect &p_effect, const String &p_param);
	static void _fit_keyframes(Clip &r_clip, int p_first, int p_length);
	static void _rescale_keyframes(Clip &r_clip, int64_t p_num, int64_t p_den);
	static Dictionary _effect_operation(const String &p_operation, const String &p_service, int p_width, int p_height);
	Dictionary _effect_param_schema(const Effect &p_effect, const String &p_param) const;
	const Vector<Keyframe> *_get_keyframes(int p_clip_id, int p_index, const String &p_param) const;
	bool _edit_keyframes(int p_clip_id, int p_index, const String &p_param, bool p_must_exist, State &r_next, int &r_clip_index, Vector<Keyframe> &r_keys);
	void _commit_keyframes(const String &p_name, State &r_next, int p_clip_index, int p_index, const String &p_param, const Vector<Keyframe> &p_keys);

	void _commit(const String &p_name, const State &p_next);
	void _apply_state(const State &p_next);
	bool _fail(const String &p_reason);

	Dictionary _bin_to_dictionary(const BinClip &p_bin) const;
	Dictionary _clip_to_dictionary(const Clip &p_clip) const;

	Error _load_xml(const String &p_xml, State &r_state, String &r_error, bool &r_foreign) const;

protected:
	static void _bind_methods();

public:
	// Profile.
	bool set_profile(int p_width, int p_height, int p_fps_num, int p_fps_den, int p_sample_rate);
	bool set_profile_preset(const String &p_name);
	static PackedStringArray get_profile_presets();
	int get_width() const { return state.width; }
	int get_height() const { return state.height; }
	int get_fps_num() const { return state.fps_num; }
	int get_fps_den() const { return state.fps_den; }
	double get_fps() const { return double(state.fps_num) / double(state.fps_den); }
	int get_sample_rate() const { return state.sample_rate; }

	// Bin.
	int add_bin_clip(ClipKind p_kind, const String &p_name, const String &p_source, int p_length, bool p_has_video = true, bool p_has_audio = false, const Dictionary &p_properties = Dictionary());
	int add_bin_color(const String &p_name, const Color &p_color, int p_length = 125);
	int add_bin_text(const String &p_name, const String &p_text, int p_length = 125, const Dictionary &p_properties = Dictionary());
	bool remove_bin_clip(int p_id, bool p_cascade = false);
	bool set_bin_clip_name(int p_id, const String &p_name);
	bool set_bin_clip_source(int p_id, const String &p_source);
	bool set_bin_clip_properties(int p_id, const Dictionary &p_properties);
	bool has_bin_clip(int p_id) const;
	Dictionary get_bin_clip(int p_id) const;
	PackedInt32Array get_bin_clip_ids() const;
	int get_bin_clip_use_count(int p_id) const;

	// Tracks, ordered bottom to top.
	int add_track(TrackKind p_kind, const String &p_name = String(), int p_index = -1);
	bool remove_track(int p_index);
	bool move_track(int p_from, int p_to);
	bool set_track_name(int p_index, const String &p_name);
	bool set_track_hidden(int p_index, bool p_hidden);
	bool set_track_muted(int p_index, bool p_muted);
	bool set_track_locked(int p_index, bool p_locked);
	int get_track_count() const { return state.tracks.size(); }
	Dictionary get_track(int p_index) const;
	TrackKind get_track_kind(int p_index) const;
	String get_track_name(int p_index) const;
	bool is_track_hidden(int p_index) const;
	bool is_track_muted(int p_index) const;
	bool is_track_locked(int p_index) const;
	int get_paired_track(int p_index) const;
	int get_track_uid(int p_index) const;
	int find_track_by_uid(int p_uid) const;

	// Timeline clips.
	int insert_clip(int p_bin_id, int p_track, int p_position, int p_in = -1, int p_out = -1, bool p_with_linked = true);
	bool move_clip(int p_id, int p_track, int p_position);
	bool trim_clip_start(int p_id, int p_frame);
	bool trim_clip_end(int p_id, int p_frame);
	int split_clip(int p_id, int p_frame);
	PackedInt32Array split_all_at(int p_frame);
	bool delete_clip(int p_id);
	bool delete_clips(const PackedInt32Array &p_ids);
	bool ripple_delete(int p_id);
	bool lift_range(int p_from, int p_to, const PackedInt32Array &p_tracks = PackedInt32Array());
	bool extract_range(int p_from, int p_to, const PackedInt32Array &p_tracks = PackedInt32Array());
	bool insert_space(int p_position, int p_length, int p_track = -1);
	int remove_space(int p_position, int p_track = -1);
	int duplicate_clip(int p_id, int p_track = -1, int p_position = -1);
	bool link_clips(int p_a, int p_b);
	bool unlink_clip(int p_id);
	int get_linked_clip(int p_id) const;
	bool set_clip_fades(int p_id, int p_fade_in, int p_fade_out);

	// Effects.
	int add_clip_effect(int p_clip_id, const String &p_operation, const String &p_service, const Dictionary &p_values = Dictionary(), int p_index = -1);
	bool remove_clip_effect(int p_clip_id, int p_index);
	bool move_clip_effect(int p_clip_id, int p_from, int p_to);
	bool set_clip_effect_enabled(int p_clip_id, int p_index, bool p_enabled);
	bool set_clip_effect_values(int p_clip_id, int p_index, const Dictionary &p_values);
	bool set_clip_effect_value(int p_clip_id, int p_index, const String &p_name, const Variant &p_value);
	int get_clip_effect_count(int p_clip_id) const;
	TypedArray<Dictionary> get_clip_effects(int p_clip_id) const;

	// Keyframes: an animatable numeric parameter of an effect can follow keyframes. Frames count from the clip start.
	bool is_effect_param_animatable(int p_clip_id, int p_index, const String &p_param) const;
	bool has_effect_keyframes(int p_clip_id, int p_index, const String &p_param) const;
	PackedStringArray get_effect_keyframe_params(int p_clip_id, int p_index) const;
	TypedArray<Dictionary> get_effect_keyframes(int p_clip_id, int p_index, const String &p_param) const;
	bool add_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_frame, const Variant &p_value = Variant(), int p_type = -1);
	bool remove_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_frame);
	bool move_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_from, int p_to);
	bool set_effect_keyframe_value(int p_clip_id, int p_index, const String &p_param, int p_frame, double p_value);
	bool set_effect_keyframe_type(int p_clip_id, int p_index, const String &p_param, int p_frame, KeyframeType p_type);
	bool clear_effect_keyframes(int p_clip_id, int p_index, const String &p_param, int p_frame = 0);
	double get_effect_value_at(int p_clip_id, int p_index, const String &p_param, int p_frame) const;
	bool clip_has_keyframes(int p_clip_id) const;
	static double keyframes_value_at(const Array &p_keyframes, int p_frame);
	static String keyframes_to_mlt(const Array &p_keyframes, double p_factor = 1.0, bool p_integer = false);
	static Array keyframes_from_mlt(const String &p_text, double p_factor = 1.0, double p_fps = 25.0);
	static String get_keyframe_type_name(KeyframeType p_type);
	static String get_keyframe_type_marker(KeyframeType p_type);

	// Markers.
	bool add_marker(int p_frame, const String &p_text = String());
	bool remove_marker(int p_frame);
	bool move_marker(int p_from, int p_to);
	bool has_marker(int p_frame) const;
	TypedArray<Dictionary> get_markers() const;

	// Queries.
	bool has_clip(int p_id) const;
	Dictionary get_clip(int p_id) const;
	PackedInt32Array get_clip_ids() const;
	int get_clip_at(int p_track, int p_frame) const;
	PackedInt32Array get_clips_in_track(int p_track) const;
	int get_duration() const;
	PackedInt32Array get_snap_points(const PackedInt32Array &p_ignore_ids = PackedInt32Array()) const;
	int find_snap(int p_frame, int p_tolerance, const PackedInt32Array &p_ignore_ids = PackedInt32Array()) const;
	bool is_range_free(int p_track, int p_from, int p_to, const PackedInt32Array &p_ignore_ids = PackedInt32Array()) const;
	String check_consistency() const;
	uint64_t get_revision() const { return revision; }
	String get_last_error() const { return last_error; }
	Dictionary get_state() const;

	// Undo.
	bool undo();
	bool redo();
	bool can_undo() const { return undo_position > 0; }
	bool can_redo() const { return undo_position < undo_stack.size(); }
	String get_undo_name() const;
	String get_redo_name() const;
	int get_undo_count() const { return undo_position; }
	void clear_undo_history();
	void begin_undo_group(const String &p_name);
	void end_undo_group();
	void set_max_undo_steps(int p_steps);
	int get_max_undo_steps() const { return max_undo_steps; }

	// MLT XML, which is also the project file format.
	String to_mlt_xml(const Dictionary &p_options = Dictionary()) const;
	Error save(const String &p_path);
	Error load(const String &p_path);
	Error load_from_xml(const String &p_xml);
	bool is_foreign() const { return foreign; }
	void clear();

	// Converts a parameter value to the text MLT expects in a property.
	static String value_to_mlt(const Variant &p_value);
};

VARIANT_ENUM_CAST(TetraVideoProject::ClipKind);
VARIANT_ENUM_CAST(TetraVideoProject::TrackKind);
VARIANT_ENUM_CAST(TetraVideoProject::KeyframeType);
