// Keyframed effect parameters of TetraVideoProject.
//
// The interpolation functions and the animation string syntax are ported from
// MLT, so that a value shown in the editor is the value the engine renders:
//   src/framework/mlt_animation.c
//   Copyright (C) 2004-2026 Meltytech, LLC (LGPL-2.1-or-later)
// The easing functions there are based on Robert Penner's easing functions.
// What happens to keyframes when a clip is trimmed or cut follows Kdenlive:
//   src/assets/keyframes/model/keyframemodel.cpp, keyframemodellist.cpp
//   Copyright (C) 2017 Nicolas Carion (GPL-3.0-or-later)

#include "tetra_video_project.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_assets.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"

namespace {

enum Ease {
	EASE_IN,
	EASE_OUT,
	EASE_IN_OUT,
};

double _distance(double p_x0, double p_y0, double p_x1, double p_y1) {
	return Math::sqrt((p_x1 - p_x0) * (p_x1 - p_x0) + (p_y1 - p_y0) * (p_y1 - p_y0));
}

// Catmull-Rom spline through (x1, y1) and (x2, y2). alpha 0 is the uniform
// spline, 0.5 the centripetal one; a tension of 0 or less only keeps a slope
// at a keyframe that lies between its neighbours, so the curve cannot overshoot.
double _catmull_rom(double p_x0, double p_y0, double p_x1, double p_y1, double p_x2, double p_y2, double p_x3, double p_y3, double p_t, double p_alpha, double p_tension) {
	// A duplicated end point stands for one far away, which makes the end segment leave horizontally.
	if (p_x0 == p_x1) {
		p_x0 -= 10000;
	}
	if (p_x3 == p_x2) {
		p_x3 += 10000;
	}
	double m1 = 0;
	double m2 = 0;
	const double t12 = Math::pow(_distance(p_x1, p_y1, p_x2, p_y2), p_alpha);
	if (p_tension > 0.0 || (p_y1 < p_y0 && p_y1 > p_y2) || (p_y1 > p_y0 && p_y1 < p_y2)) {
		const double t01 = Math::pow(_distance(p_x0, p_y0, p_x1, p_y1), p_alpha);
		m1 = Math::abs(p_tension) * (p_y2 - p_y1 + t12 * ((p_y1 - p_y0) / t01 - (p_y2 - p_y0) / (t01 + t12)));
	}
	if (p_tension > 0.0 || (p_y2 < p_y1 && p_y2 > p_y3) || (p_y2 > p_y1 && p_y2 < p_y3)) {
		const double t23 = Math::pow(_distance(p_x2, p_y2, p_x3, p_y3), p_alpha);
		m2 = Math::abs(p_tension) * (p_y2 - p_y1 + t12 * ((p_y3 - p_y2) / t23 - (p_y3 - p_y1) / (t12 + t23)));
	}
	const double a = 2.0 * (p_y1 - p_y2) + m1 + m2;
	const double b = -3.0 * (p_y1 - p_y2) - m1 - m1 - m2;
	return a * p_t * p_t * p_t + b * p_t * p_t + m1 * p_t + p_y1;
}

double _sinusoidal(double p_t, Ease p_ease) {
	switch (p_ease) {
		case EASE_IN:
			return Math::sin((p_t - 1) * Math::PI / 2) + 1;
		case EASE_OUT:
			return Math::sin(p_t * Math::PI / 2);
		default:
			return 0.5 * (1 - Math::cos(p_t * Math::PI));
	}
}

double _power(double p_t, double p_order, Ease p_ease) {
	switch (p_ease) {
		case EASE_IN:
			return Math::pow(p_t, p_order);
		case EASE_OUT:
			return 1 - Math::pow(1 - p_t, p_order);
		default:
			return p_t < 0.5 ? Math::pow(2.0, p_order) * Math::pow(p_t, p_order) / 2 : 1.0 - Math::pow(-2 * p_t + 2, p_order) / 2;
	}
}

double _exponential(double p_t, Ease p_ease) {
	if (p_t == 0.0) {
		return 0;
	}
	if (p_t == 1.0) {
		return 1;
	}
	switch (p_ease) {
		case EASE_IN:
			return Math::pow(2.0, 10 * p_t - 10);
		case EASE_OUT:
			return 1.0 - Math::pow(2.0, -10 * p_t);
		default:
			return p_t < 0.5 ? Math::pow(2.0, 20 * p_t - 10) / 2 : (2 - Math::pow(2.0, -20 * p_t + 10)) / 2;
	}
}

double _circular(double p_t, Ease p_ease) {
	switch (p_ease) {
		case EASE_IN:
			return 1.0 - Math::sqrt(1.0 - p_t * p_t);
		case EASE_OUT:
			return Math::sqrt(1.0 - (p_t - 1.0) * (p_t - 1.0));
		default:
			return p_t < 0.5 ? 0.5 * (1 - Math::sqrt(1 - 4 * (p_t * p_t))) : 0.5 * (Math::sqrt(-((2 * p_t) - 3) * ((2 * p_t) - 1)) + 1);
	}
}

double _back_in(double p_f) {
	return p_f * p_f * p_f - p_f * Math::sin(p_f * Math::PI);
}

double _back(double p_t, Ease p_ease) {
	switch (p_ease) {
		case EASE_IN:
			return _back_in(p_t);
		case EASE_OUT:
			return 1 - _back_in(1 - p_t);
		default:
			return p_t < 0.5 ? 0.5 * _back_in(2 * p_t) : 0.5 * (1 - _back_in(1 - (2 * p_t - 1))) + 0.5;
	}
}

double _elastic(double p_t, Ease p_ease) {
	const double half_turns = 13 * Math::PI / 2;
	switch (p_ease) {
		case EASE_IN:
			return Math::sin(half_turns * p_t) * Math::pow(2.0, 10 * (p_t - 1));
		case EASE_OUT:
			return Math::sin(-half_turns * (p_t + 1)) * Math::pow(2.0, -10 * p_t) + 1;
		default:
			if (p_t < 0.5) {
				return 0.5 * Math::sin(half_turns * (2 * p_t)) * Math::pow(2.0, 10 * ((2 * p_t) - 1));
			}
			return 0.5 * (Math::sin(-half_turns * ((2 * p_t - 1) + 1)) * Math::pow(2.0, -10 * (2 * p_t - 1)) + 2);
	}
}

double _bounce_out(double p_t) {
	if (p_t < 4 / 11.0) {
		return (121 * p_t * p_t) / 16.0;
	} else if (p_t < 8 / 11.0) {
		return (363 / 40.0 * p_t * p_t) - (99 / 10.0 * p_t) + 17 / 5.0;
	} else if (p_t < 9 / 10.0) {
		return (4356 / 361.0 * p_t * p_t) - (35442 / 1805.0 * p_t) + 16061 / 1805.0;
	}
	return (54 / 5.0 * p_t * p_t) - (513 / 25.0 * p_t) + 268 / 25.0;
}

double _bounce(double p_t, Ease p_ease) {
	switch (p_ease) {
		case EASE_IN:
			return 1.0 - _bounce_out(1.0 - p_t);
		case EASE_OUT:
			return _bounce_out(p_t);
		default:
			return p_t < 0.5 ? 0.5 * (1.0 - _bounce_out(1.0 - p_t * 2)) : 0.5 * _bounce_out(2.0 * p_t - 1.0) + 0.5;
	}
}

// The fraction of the way from one keyframe to the next at progress p_t, for the easing types.
double _ease_factor(int p_type, double p_t) {
	const int family = (p_type - TetraVideoProject::KEYFRAME_SINUSOIDAL_IN) / 3;
	const Ease ease = Ease((p_type - TetraVideoProject::KEYFRAME_SINUSOIDAL_IN) % 3);
	switch (family) {
		case 0:
			return _sinusoidal(p_t, ease);
		case 1:
		case 2:
		case 3:
		case 4:
			return _power(p_t, family + 1, ease);
		case 5:
			return _exponential(p_t, ease);
		case 6:
			return _circular(p_t, ease);
		case 7:
			return _back(p_t, ease);
		case 8:
			return _elastic(p_t, ease);
		default:
			return _bounce(p_t, ease);
	}
}

// The character between the frame and "=" in an animation string, in mlt_keyframe_type order.
const char *type_markers[TetraVideoProject::KEYFRAME_TYPE_MAX] = {
	"|", "", "~", "$", "-",
	"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
	"p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "A", "B", "C", "D"
};

const char *type_names[TetraVideoProject::KEYFRAME_TYPE_MAX] = {
	TTRC("Discrete"), TTRC("Linear"), TTRC("Smooth"), TTRC("Smooth Natural"), TTRC("Smooth Tight"),
	TTRC("Sinusoidal In"), TTRC("Sinusoidal Out"), TTRC("Sinusoidal In/Out"),
	TTRC("Quadratic In"), TTRC("Quadratic Out"), TTRC("Quadratic In/Out"),
	TTRC("Cubic In"), TTRC("Cubic Out"), TTRC("Cubic In/Out"),
	TTRC("Quartic In"), TTRC("Quartic Out"), TTRC("Quartic In/Out"),
	TTRC("Quintic In"), TTRC("Quintic Out"), TTRC("Quintic In/Out"),
	TTRC("Exponential In"), TTRC("Exponential Out"), TTRC("Exponential In/Out"),
	TTRC("Circular In"), TTRC("Circular Out"), TTRC("Circular In/Out"),
	TTRC("Back In"), TTRC("Back Out"), TTRC("Back In/Out"),
	TTRC("Elastic In"), TTRC("Elastic Out"), TTRC("Elastic In/Out"),
	TTRC("Bounce In"), TTRC("Bounce Out"), TTRC("Bounce In/Out")
};

bool _is_integer_param(const Dictionary &p_schema) {
	return String(p_schema.get("type", "float")) == "int";
}

} // namespace

// Index of the keyframe at p_frame, or -1.
int TetraVideoProject::_keyframe_index(const Vector<Keyframe> &p_keys, int p_frame) {
	for (int i = 0; i < p_keys.size(); i++) {
		if (p_keys[i].frame == p_frame) {
			return i;
		}
	}
	return -1;
}

// Index of the last keyframe at or before p_frame, or -1.
int TetraVideoProject::_keyframe_before(const Vector<Keyframe> &p_keys, int p_frame) {
	int found = -1;
	for (int i = 0; i < p_keys.size() && p_keys[i].frame <= p_frame; i++) {
		found = i;
	}
	return found;
}

// mlt_animation_get_item() and interpolate_value(): the value holds before the
// first and after the last keyframe, and between two keyframes the type of the
// earlier one decides the curve.
double TetraVideoProject::_keyframes_value_at(const Vector<Keyframe> &p_keys, int p_frame) {
	if (p_keys.is_empty()) {
		return 0.0;
	}
	const int at = _keyframe_before(p_keys, p_frame);
	if (at < 0) {
		return p_keys[0].value;
	}
	if (p_keys[at].frame == p_frame || at == p_keys.size() - 1) {
		return p_keys[at].value;
	}
	const Keyframe &p1 = p_keys[at];
	const Keyframe &p2 = p_keys[at + 1];
	const Keyframe &p0 = at > 0 ? p_keys[at - 1] : p1;
	const Keyframe &p3 = at + 2 < p_keys.size() ? p_keys[at + 2] : p2;
	const double t = double(p_frame - p1.frame) / double(p2.frame - p1.frame);
	switch (p1.type) {
		case KEYFRAME_DISCRETE:
			return p1.value;
		case KEYFRAME_LINEAR:
			return p1.value + (p2.value - p1.value) * t;
		case KEYFRAME_SMOOTH:
			return _catmull_rom(p0.frame, p0.value, p1.frame, p1.value, p2.frame, p2.value, p3.frame, p3.value, t, 0.0, 1.0);
		case KEYFRAME_SMOOTH_NATURAL:
			return _catmull_rom(p0.frame, p0.value, p1.frame, p1.value, p2.frame, p2.value, p3.frame, p3.value, t, 0.5, -1.0);
		case KEYFRAME_SMOOTH_TIGHT:
			return _catmull_rom(p0.frame, p0.value, p1.frame, p1.value, p2.frame, p2.value, p3.frame, p3.value, t, 0.5, 0.0);
		default:
			return p1.value + (p2.value - p1.value) * _ease_factor(p1.type, t);
	}
}

// "frame=value;frame~=value": MLT's animation string. Display values are divided by the factor, as constants are.
String TetraVideoProject::_keyframes_to_mlt(const Vector<Keyframe> &p_keys, double p_factor, bool p_integer) {
	String text;
	for (const Keyframe &key : p_keys) {
		if (!text.is_empty()) {
			text += ";";
		}
		const double number = key.value / (p_factor == 0.0 ? 1.0 : p_factor);
		text += itos(key.frame) + type_markers[key.type] + "=";
		text += p_integer && p_factor == 1.0 ? itos(int64_t(Math::round(number))) : value_to_mlt(number);
	}
	return text;
}

// Reads an animation string of plain numbers. Keyframe times may be frames or
// clock values (hh:mm:ss.mmm). Returns false for anything else: rectangles,
// colours, times counted from the end.
bool TetraVideoProject::_keyframes_from_mlt(const String &p_text, double p_factor, double p_fps, Vector<Keyframe> &r_keys) {
	r_keys.clear();
	for (const String &item : p_text.split(";", false)) {
		const int equals = item.find_char('=');
		if (equals < 1) {
			return false;
		}
		String time = item.substr(0, equals).strip_edges();
		String value = item.substr(equals + 1).strip_edges();
		Keyframe key;
		const char32_t last = time[time.length() - 1];
		if (!is_digit(last)) {
			int type = -1;
			for (int i = 0; i < KEYFRAME_TYPE_MAX; i++) {
				if (type_markers[i][0] != 0 && char32_t(type_markers[i][0]) == last) {
					type = i;
				}
			}
			if (last == '!') {
				type = KEYFRAME_DISCRETE;
			}
			if (type < 0) {
				return false;
			}
			key.type = KeyframeType(type);
			time = time.substr(0, time.length() - 1);
		}
		if (time.contains_char(':')) {
			const PackedStringArray parts = time.split(":");
			double seconds = 0.0;
			for (const String &part : parts) {
				if (!part.is_valid_float() || part.begins_with("-")) {
					return false;
				}
				seconds = seconds * 60.0 + part.to_float();
			}
			key.frame = int(Math::round(seconds * p_fps));
		} else if (time.is_valid_int() && !time.begins_with("-")) {
			key.frame = time.to_int();
		} else {
			return false;
		}
		if (value.length() >= 2 && value.begins_with("\"") && value.ends_with("\"")) {
			value = value.substr(1, value.length() - 2);
		}
		if (!value.is_valid_float()) {
			return false;
		}
		key.value = value.to_float() * p_factor;
		// Keyframes are kept in time order; a repeated time replaces the earlier entry, as in MLT.
		int at = r_keys.size();
		while (at > 0 && r_keys[at - 1].frame > key.frame) {
			at--;
		}
		if (at > 0 && r_keys[at - 1].frame == key.frame) {
			r_keys.write[at - 1] = key;
		} else {
			r_keys.insert(at, key);
		}
	}
	return !r_keys.is_empty();
}

Array TetraVideoProject::_keyframes_to_array(const Vector<Keyframe> &p_keys) {
	Array array;
	for (const Keyframe &key : p_keys) {
		Dictionary d;
		d["frame"] = key.frame;
		d["value"] = key.value;
		d["type"] = int(key.type);
		array.push_back(d);
	}
	return array;
}

// Accepts what _keyframes_to_array() writes, in any order. Returns false when an entry is malformed.
bool TetraVideoProject::_keyframes_from_array(const Array &p_array, Vector<Keyframe> &r_keys) {
	r_keys.clear();
	for (const Variant &entry : p_array) {
		if (entry.get_type() != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary d = entry;
		const Variant frame = d.get("frame", Variant());
		const Variant value = d.get("value", Variant());
		const int type = d.get("type", int(KEYFRAME_LINEAR));
		if (!frame.is_num() || !value.is_num() || type < 0 || type >= KEYFRAME_TYPE_MAX || !Math::is_finite(double(value))) {
			return false;
		}
		Keyframe key;
		key.frame = frame;
		key.value = value;
		key.type = KeyframeType(type);
		int at = r_keys.size();
		while (at > 0 && r_keys[at - 1].frame > key.frame) {
			at--;
		}
		if (at > 0 && r_keys[at - 1].frame == key.frame) {
			r_keys.write[at - 1] = key;
		} else {
			r_keys.insert(at, key);
		}
	}
	return true;
}

Dictionary TetraVideoProject::_effect_keyframes_to_dictionary(const Effect &p_effect) {
	Dictionary d;
	for (const KeyframeTrack &track : p_effect.keyframes) {
		d[track.param] = _keyframes_to_array(track.keys);
	}
	return d;
}

void TetraVideoProject::_effect_keyframes_from_dictionary(const Dictionary &p_dictionary, Effect &r_effect) {
	r_effect.keyframes.clear();
	for (const KeyValue<Variant, Variant> &entry : p_dictionary) {
		KeyframeTrack track;
		track.param = entry.key;
		if (entry.value.get_type() == Variant::ARRAY && !track.param.is_empty() && _keyframes_from_array(entry.value, track.keys) && !track.keys.is_empty()) {
			r_effect.keyframes.push_back(track);
		}
	}
}

int TetraVideoProject::_find_keyframe_track(const Effect &p_effect, const String &p_param) {
	for (int i = 0; i < p_effect.keyframes.size(); i++) {
		if (p_effect.keyframes[i].param == p_param) {
			return i;
		}
	}
	return -1;
}

// Keeps the stretch of p_length frames that starts at p_first and makes its
// first frame frame 0. Keyframes outside are dropped, as Kdenlive does when a
// clip is resized or cut, after a keyframe with the interpolated value was
// put on the boundary they were beyond. Between linear and discrete keyframes
// that leaves the motion exactly as it was.
void TetraVideoProject::_fit_keyframes(Clip &r_clip, int p_first, int p_length) {
	const int last = p_first + p_length - 1;
	for (Effect &effect : r_clip.effects) {
		for (KeyframeTrack &track : effect.keyframes) {
			const Vector<Keyframe> old = track.keys;
			Vector<Keyframe> keys;
			for (int boundary = 0; boundary < 2; boundary++) {
				const int frame = boundary == 0 ? p_first : last;
				const bool beyond = boundary == 0 ? old[0].frame < frame : old[old.size() - 1].frame > frame;
				if (!beyond || _keyframe_index(old, frame) >= 0 || (boundary == 1 && last == p_first)) {
					continue;
				}
				Keyframe key;
				key.frame = frame;
				key.value = _keyframes_value_at(old, frame);
				key.type = old[MAX(0, _keyframe_before(old, frame))].type;
				keys.push_back(key);
			}
			for (const Keyframe &key : old) {
				if (key.frame >= p_first && key.frame <= last) {
					int at = keys.size();
					while (at > 0 && keys[at - 1].frame > key.frame) {
						at--;
					}
					keys.insert(at, key);
				}
			}
			for (Keyframe &key : keys) {
				key.frame -= p_first;
			}
			track.keys = keys;
		}
		// Only a clip without a single frame leaves a track empty.
		for (int i = effect.keyframes.size() - 1; i >= 0; i--) {
			if (effect.keyframes[i].keys.is_empty()) {
				effect.keyframes.remove_at(i);
			}
		}
	}
}

// A change of frame rate moves keyframes with the pictures they were set on.
void TetraVideoProject::_rescale_keyframes(Clip &r_clip, int64_t p_num, int64_t p_den) {
	for (Effect &effect : r_clip.effects) {
		for (KeyframeTrack &track : effect.keyframes) {
			Vector<Keyframe> keys;
			for (Keyframe key : track.keys) {
				key.frame = int(CLAMP((int64_t(key.frame) * p_num + p_den / 2) / p_den, int64_t(0), int64_t(INT_MAX)));
				if (!keys.is_empty() && keys[keys.size() - 1].frame == key.frame) {
					keys.write[keys.size() - 1] = key;
				} else {
					keys.push_back(key);
				}
			}
			track.keys = keys;
		}
	}
	_fit_keyframes(r_clip, 0, r_clip.length());
}

// The schema of an effect: the catalog entry, or for a filter the catalog
// leaves out (Kdenlive describes the service under another id) MLT's own metadata.
Dictionary TetraVideoProject::_effect_operation(const String &p_operation, const String &p_service, int p_width, int p_height) {
	Dictionary operation = TetraVideoAssets::get_operation(p_operation, p_width, p_height);
	if (operation.is_empty() && p_operation == "mlt:" + p_service) {
		operation = TetraMltEngine::get_service_operation("filter", p_service);
	}
	return operation;
}

Dictionary TetraVideoProject::_effect_param_schema(const Effect &p_effect, const String &p_param) const {
	const Dictionary operation = _effect_operation(p_effect.operation, p_effect.service, state.width, state.height);
	const Array params = operation.get("params", Array());
	for (const Variant &entry : params) {
		const Dictionary param = entry;
		if (String(param.get("name", String())) == p_param) {
			return param;
		}
	}
	return Dictionary();
}

const Vector<TetraVideoProject::Keyframe> *TetraVideoProject::_get_keyframes(int p_clip_id, int p_index, const String &p_param) const {
	const int index = _find_clip(state, p_clip_id);
	if (index < 0 || p_index < 0 || p_index >= state.clips[index].effects.size()) {
		return nullptr;
	}
	const Effect &effect = state.clips[index].effects[p_index];
	const int track = _find_keyframe_track(effect, p_param);
	return track < 0 ? nullptr : &effect.keyframes[track].keys;
}

bool TetraVideoProject::_edit_keyframes(int p_clip_id, int p_index, const String &p_param, bool p_must_exist, State &r_next, int &r_clip_index, Vector<Keyframe> &r_keys) {
	if (!_edit_effect(p_clip_id, p_index, r_next, r_clip_index)) {
		return _fail("No such effect, or the track is locked.");
	}
	const Effect &effect = state.clips[r_clip_index].effects[p_index];
	const int track = _find_keyframe_track(effect, p_param);
	if (track < 0 && p_must_exist) {
		return _fail("The parameter has no keyframes.");
	}
	r_keys = track < 0 ? Vector<Keyframe>() : effect.keyframes[track].keys;
	return true;
}

// Stores the keyframes of one parameter; an empty list ends its animation.
void TetraVideoProject::_commit_keyframes(const String &p_name, State &r_next, int p_clip_index, int p_index, const String &p_param, const Vector<Keyframe> &p_keys) {
	Vector<Effect> effects = r_next.clips[p_clip_index].effects;
	Vector<KeyframeTrack> tracks = effects[p_index].keyframes;
	const int track = _find_keyframe_track(effects[p_index], p_param);
	if (p_keys.is_empty()) {
		if (track >= 0) {
			tracks.remove_at(track);
		}
	} else if (track >= 0) {
		tracks.write[track].keys = p_keys;
	} else {
		KeyframeTrack added;
		added.param = p_param;
		added.keys = p_keys;
		tracks.push_back(added);
	}
	effects.write[p_index].keyframes = tracks;
	r_next.clips.write[p_clip_index].effects = effects;
	_commit(p_name, r_next);
}

bool TetraVideoProject::is_effect_param_animatable(int p_clip_id, int p_index, const String &p_param) const {
	const int index = _find_clip(state, p_clip_id);
	if (index < 0 || p_index < 0 || p_index >= state.clips[index].effects.size()) {
		return false;
	}
	return bool(_effect_param_schema(state.clips[index].effects[p_index], p_param).get("animatable", false));
}

bool TetraVideoProject::has_effect_keyframes(int p_clip_id, int p_index, const String &p_param) const {
	return _get_keyframes(p_clip_id, p_index, p_param) != nullptr;
}

PackedStringArray TetraVideoProject::get_effect_keyframe_params(int p_clip_id, int p_index) const {
	PackedStringArray names;
	const int index = _find_clip(state, p_clip_id);
	if (index >= 0 && p_index >= 0 && p_index < state.clips[index].effects.size()) {
		for (const KeyframeTrack &track : state.clips[index].effects[p_index].keyframes) {
			names.push_back(track.param);
		}
	}
	return names;
}

TypedArray<Dictionary> TetraVideoProject::get_effect_keyframes(int p_clip_id, int p_index, const String &p_param) const {
	TypedArray<Dictionary> result;
	const Vector<Keyframe> *keys = _get_keyframes(p_clip_id, p_index, p_param);
	if (keys) {
		for (const Variant &key : _keyframes_to_array(*keys)) {
			result.push_back(key);
		}
	}
	return result;
}

// Adds a keyframe, or changes the one already at p_frame. Without a value the
// keyframe takes the value the parameter has at that frame, and without a type
// the type of the keyframe before it, so adding one does not change the motion
// between linear keyframes.
bool TetraVideoProject::add_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_frame, const Variant &p_value, int p_type) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, false, next, index, keys)) {
		return false;
	}
	const Clip &clip = state.clips[index];
	const Effect &effect = clip.effects[p_index];
	const Dictionary schema = _effect_param_schema(effect, p_param);
	if (!bool(schema.get("animatable", false))) {
		return _fail("The parameter cannot be animated.");
	}
	if (p_frame < 0 || p_frame >= clip.length()) {
		return _fail("The keyframe is outside the clip.");
	}
	if (p_type >= KEYFRAME_TYPE_MAX || (p_value.get_type() != Variant::NIL && (!p_value.is_num() || !Math::is_finite(double(p_value))))) {
		return _fail("Illegal keyframe value or type.");
	}
	Keyframe key;
	key.frame = p_frame;
	const int existing = _keyframe_index(keys, p_frame);
	if (p_value.get_type() != Variant::NIL) {
		key.value = p_value;
	} else if (!keys.is_empty()) {
		key.value = _keyframes_value_at(keys, p_frame);
	} else {
		const Variant constant = effect.values.get(p_param, schema.get("default", 0.0));
		key.value = constant.is_num() ? double(constant) : String(constant).to_float();
	}
	if (p_type >= 0) {
		key.type = KeyframeType(p_type);
	} else if (existing >= 0) {
		key.type = keys[existing].type;
	} else if (!keys.is_empty()) {
		key.type = keys[MAX(0, _keyframe_before(keys, p_frame))].type;
	}
	if (existing >= 0) {
		if (keys[existing].value == key.value && keys[existing].type == key.type) {
			return false;
		}
		keys.write[existing] = key;
	} else {
		keys.insert(_keyframe_before(keys, p_frame) + 1, key);
	}
	_commit_keyframes(existing >= 0 ? TTR("Change Keyframe") : TTR("Add Keyframe"), next, index, p_index, p_param, keys);
	return true;
}

// Removing the only keyframe ends the animation; the parameter keeps that keyframe's value.
bool TetraVideoProject::remove_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_frame) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, true, next, index, keys)) {
		return false;
	}
	if (keys.size() == 1 && keys[0].frame == p_frame) {
		return clear_effect_keyframes(p_clip_id, p_index, p_param, p_frame);
	}
	const int at = _keyframe_index(keys, p_frame);
	if (at < 0) {
		return _fail("No keyframe at that frame.");
	}
	keys.remove_at(at);
	_commit_keyframes(TTR("Remove Keyframe"), next, index, p_index, p_param, keys);
	return true;
}

// Moves a keyframe in time. It cannot land on another keyframe or leave the clip.
bool TetraVideoProject::move_effect_keyframe(int p_clip_id, int p_index, const String &p_param, int p_from, int p_to) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, true, next, index, keys)) {
		return false;
	}
	const int at = _keyframe_index(keys, p_from);
	if (at < 0) {
		return _fail("No keyframe at that frame.");
	}
	if (p_to == p_from) {
		return false;
	}
	if (p_to < 0 || p_to >= state.clips[index].length()) {
		return _fail("The keyframe would leave the clip.");
	}
	if (_keyframe_index(keys, p_to) >= 0) {
		return _fail("There is already a keyframe at that frame.");
	}
	Keyframe key = keys[at];
	key.frame = p_to;
	keys.remove_at(at);
	keys.insert(_keyframe_before(keys, p_to) + 1, key);
	_commit_keyframes(TTR("Move Keyframe"), next, index, p_index, p_param, keys);
	return true;
}

bool TetraVideoProject::set_effect_keyframe_value(int p_clip_id, int p_index, const String &p_param, int p_frame, double p_value) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, true, next, index, keys)) {
		return false;
	}
	const int at = _keyframe_index(keys, p_frame);
	if (at < 0 || !Math::is_finite(p_value)) {
		return _fail("No keyframe at that frame, or an illegal value.");
	}
	if (keys[at].value == p_value) {
		return false;
	}
	keys.write[at].value = p_value;
	_commit_keyframes(TTR("Change Keyframe"), next, index, p_index, p_param, keys);
	return true;
}

bool TetraVideoProject::set_effect_keyframe_type(int p_clip_id, int p_index, const String &p_param, int p_frame, KeyframeType p_type) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, true, next, index, keys)) {
		return false;
	}
	const int at = _keyframe_index(keys, p_frame);
	if (at < 0 || p_type < 0 || p_type >= KEYFRAME_TYPE_MAX) {
		return _fail("No keyframe at that frame, or an unknown type.");
	}
	if (keys[at].type == p_type) {
		return false;
	}
	keys.write[at].type = p_type;
	_commit_keyframes(TTR("Change Keyframe Type"), next, index, p_index, p_param, keys);
	return true;
}

// Ends the animation of a parameter: it becomes a constant again, with the value it had at p_frame.
bool TetraVideoProject::clear_effect_keyframes(int p_clip_id, int p_index, const String &p_param, int p_frame) {
	State next;
	int index;
	Vector<Keyframe> keys;
	if (!_edit_keyframes(p_clip_id, p_index, p_param, true, next, index, keys)) {
		return false;
	}
	const Effect &effect = state.clips[index].effects[p_index];
	const double value = _keyframes_value_at(keys, p_frame);
	Dictionary values = effect.values.duplicate(true);
	if (_is_integer_param(_effect_param_schema(effect, p_param))) {
		values[p_param] = int64_t(Math::round(value));
	} else {
		values[p_param] = value;
	}
	Vector<Effect> effects = next.clips[index].effects;
	effects.write[p_index].values = values;
	next.clips.write[index].effects = effects;
	_commit_keyframes(TTR("Remove Keyframes"), next, index, p_index, p_param, Vector<Keyframe>());
	return true;
}

// The value of a numeric parameter p_frame frames into the clip: interpolated when it is animated, its constant otherwise.
double TetraVideoProject::get_effect_value_at(int p_clip_id, int p_index, const String &p_param, int p_frame) const {
	const Vector<Keyframe> *keys = _get_keyframes(p_clip_id, p_index, p_param);
	if (keys) {
		return _keyframes_value_at(*keys, p_frame);
	}
	const int index = _find_clip(state, p_clip_id);
	if (index < 0 || p_index < 0 || p_index >= state.clips[index].effects.size()) {
		return 0.0;
	}
	const Effect &effect = state.clips[index].effects[p_index];
	Variant constant = effect.values.get(p_param, Variant());
	if (constant.get_type() == Variant::NIL) {
		constant = _effect_param_schema(effect, p_param).get("default", 0.0);
	}
	return constant.is_num() ? double(constant) : String(constant).to_float();
}

bool TetraVideoProject::clip_has_keyframes(int p_clip_id) const {
	const int index = _find_clip(state, p_clip_id);
	if (index >= 0) {
		for (const Effect &effect : state.clips[index].effects) {
			if (!effect.keyframes.is_empty()) {
				return true;
			}
		}
	}
	return false;
}

double TetraVideoProject::keyframes_value_at(const Array &p_keyframes, int p_frame) {
	Vector<Keyframe> keys;
	return _keyframes_from_array(p_keyframes, keys) ? _keyframes_value_at(keys, p_frame) : 0.0;
}

String TetraVideoProject::keyframes_to_mlt(const Array &p_keyframes, double p_factor, bool p_integer) {
	Vector<Keyframe> keys;
	return _keyframes_from_array(p_keyframes, keys) ? _keyframes_to_mlt(keys, p_factor, p_integer) : String();
}

Array TetraVideoProject::keyframes_from_mlt(const String &p_text, double p_factor, double p_fps) {
	Vector<Keyframe> keys;
	return _keyframes_from_mlt(p_text, p_factor, p_fps, keys) ? _keyframes_to_array(keys) : Array();
}

String TetraVideoProject::get_keyframe_type_name(KeyframeType p_type) {
	return p_type >= 0 && p_type < KEYFRAME_TYPE_MAX ? TTRGET(type_names[p_type]) : String();
}

String TetraVideoProject::get_keyframe_type_marker(KeyframeType p_type) {
	return p_type >= 0 && p_type < KEYFRAME_TYPE_MAX ? String(type_markers[p_type]) : String();
}
