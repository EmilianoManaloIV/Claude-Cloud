#pragma once

#include "core/math/math_funcs.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

// Non-drop-frame timecode (HH:MM:SS:FF) as the monitor and the timeline ruler
// show it. One second counts as many frames as the rounded frame rate, so
// 29.97 fps counts 30 frames per timecode second, as Kdenlive does.
namespace TetraVideoTimecode {

inline int frames_per_second(double p_fps) {
	return MAX(1, int(Math::round(p_fps)));
}

inline String from_frames(int p_frame, double p_fps) {
	const int base = frames_per_second(p_fps);
	const int frame = MAX(0, p_frame);
	const int seconds = frame / base;
	return vformat("%02d:%02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60, frame % base);
}

// Accepts "HH:MM:SS:FF" with any number of leading fields left out ("12:05" is
// 12 seconds and 5 frames) and a plain frame number. Returns -1 for anything else.
inline int to_frames(const String &p_text, double p_fps) {
	const String text = p_text.strip_edges().replace_char(';', ':').replace_char('.', ':');
	if (text.is_empty()) {
		return -1;
	}
	const PackedStringArray fields = text.split(":");
	if (fields.size() > 4) {
		return -1;
	}
	for (const String &field : fields) {
		if (field.is_empty() || !field.is_valid_int() || field.to_int() < 0) {
			return -1;
		}
	}
	if (fields.size() == 1) {
		return fields[0].to_int();
	}
	const int base = frames_per_second(p_fps);
	int64_t seconds = 0;
	for (int i = 0; i < fields.size() - 1; i++) {
		seconds = seconds * 60 + fields[i].to_int();
	}
	return int(seconds * base + fields[fields.size() - 1].to_int());
}

} // namespace TetraVideoTimecode
