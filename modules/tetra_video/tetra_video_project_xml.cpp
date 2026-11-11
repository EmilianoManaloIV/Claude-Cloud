#include "tetra_video_project.h"

#include "mlt/tetra_mlt_engine.h"
#include "tetra_video_assets.h"
#include "tetra_video_xml.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/variant/variant_utility.h"

// Writing and reading the project as MLT XML.
//
// The graph follows Kdenlive's layout: track 0 of the tractor is a black
// colour producer as long as the project, every timeline track is a playlist
// above it, every audio track is mixed and every video track is composited
// onto track 0 by an always-active transition. Everything MLT does not need
// to play the project is stored in "tetra:" properties, which MLT ignores.

namespace {

const int FORMAT_VERSION = 1;

String _globalize(const String &p_path) {
	if (p_path.begins_with("res://") || p_path.begins_with("user://")) {
		return ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return p_path;
}

String _mlt_color(const String &p_html) {
	const Color color = Color::html_is_valid(p_html) ? Color::html(p_html) : Color(0, 0, 0, 1);
	return "0x" + color.to_html(true);
}

int _gcd(int p_a, int p_b) {
	while (p_b != 0) {
		const int rest = p_a % p_b;
		p_a = p_b;
		p_b = rest;
	}
	return MAX(1, p_a);
}

class XmlWriter {
	String out;

public:
	void line(int p_indent, const String &p_text) {
		for (int i = 0; i < p_indent; i++) {
			out += " ";
		}
		out += p_text + "\n";
	}

	void property(int p_indent, const String &p_name, const String &p_value) {
		line(p_indent, "<property name=\"" + p_name.xml_escape(true) + "\">" + p_value.xml_escape() + "</property>");
	}

	void property(int p_indent, const String &p_name, int p_value) {
		property(p_indent, p_name, itos(p_value));
	}

	// Properties from a dictionary are written in key order so that the same
	// document always produces the same text.
	void properties(int p_indent, const Dictionary &p_values) {
		PackedStringArray keys;
		for (const Variant &key : p_values.keys()) {
			keys.push_back(key);
		}
		keys.sort();
		for (const String &key : keys) {
			property(p_indent, key, TetraVideoProject::value_to_mlt(p_values[key]));
		}
	}

	const String &get() const { return out; }
};

// Volume keyframes in dB that approximate a linear gain ramp between silence
// and unity over p_length frames starting at p_first.
String _audio_fade_animation(int p_first, int p_length, bool p_fade_in) {
	const double fractions[6] = { 0.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0 };
	String animation;
	int last_frame = -1;
	for (int i = 0; i < 6; i++) {
		const double fraction = p_fade_in ? fractions[i] : fractions[5 - i];
		const double at = p_fade_in ? fractions[i] : 1.0 - fractions[5 - i];
		const int frame = p_first + int(Math::round(at * p_length));
		if (frame == last_frame) {
			continue;
		}
		last_frame = frame;
		const double decibel = fraction <= 0.0 ? -70.0 : 20.0 * Math::log(fraction) / Math::log(10.0);
		if (!animation.is_empty()) {
			animation += ";";
		}
		animation += itos(frame) + "=" + String::num(decibel, 2);
	}
	return animation;
}

// The <property> children of a node, by name.
HashMap<String, String> _properties(const Vector<TetraXmlNode> &p_nodes, const TetraXmlNode &p_node) {
	HashMap<String, String> result;
	for (int child : p_node.children) {
		if (p_nodes[child].name == "property") {
			result[p_nodes[child].get_attribute("name")] = p_nodes[child].text;
		}
	}
	return result;
}

String _lookup(const HashMap<String, String> &p_map, const String &p_name, const String &p_default = String()) {
	const String *value = p_map.getptr(p_name);
	return value ? *value : p_default;
}

// MLT accepts plain frame counts and clock values ("00:00:02.500") for times.
int _parse_time(const String &p_text, double p_fps, int p_default) {
	const String text = p_text.strip_edges();
	if (text.is_empty()) {
		return p_default;
	}
	if (!text.contains_char(':')) {
		return text.is_valid_int() ? text.to_int() : p_default;
	}
	double seconds = 0.0;
	for (const String &part : text.replace_char(',', '.').split(":")) {
		seconds = seconds * 60.0 + part.to_float();
	}
	return int(Math::round(seconds * p_fps));
}

} // namespace

String TetraVideoProject::value_to_mlt(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			return bool(p_value) ? "1" : "0";
		case Variant::INT:
			return itos(p_value);
		case Variant::FLOAT:
			// "1" rather than "1.0": the shortest form MLT reads back as the same number.
			return String::num(double(p_value)).trim_suffix(".0");
		case Variant::COLOR:
			return "0x" + Color(p_value).to_html(true);
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			return String::num(v.x) + " " + String::num(v.y);
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			return String::num(v.x) + " " + String::num(v.y) + " " + String::num(v.z);
		}
		default:
			return p_value.operator String();
	}
}

String TetraVideoProject::to_mlt_xml(const Dictionary &p_options) const {
	const String compositing = p_options.get("compositing", TetraMltEngine::get_preferred_service("transition", "frei0r.cairoblend,qtblend,composite"));
	const String image_producer = p_options.get("image_producer", TetraMltEngine::get_preferred_service("producer", "pixbuf,qimage"));
	const String text_producer = p_options.get("text_producer", TetraMltEngine::get_preferred_service("producer", "pango,qtext"));
	const bool with_tetra = p_options.get("tetra_properties", true);
	const int width = p_options.get("width", state.width);
	const int height = p_options.get("height", state.height);
	const int duration = MAX(1, get_duration());
	const int aspect = _gcd(width, height);

	int lowest_video_track = -1;
	for (int i = 0; i < state.tracks.size() && lowest_video_track < 0; i++) {
		if (state.tracks[i].kind == TRACK_VIDEO) {
			lowest_video_track = i;
		}
	}

	XmlWriter xml;
	xml.line(0, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
	xml.line(0, "<mlt LC_NUMERIC=\"C\" version=\"7.0.0\" producer=\"main_tractor\">");
	xml.line(1, vformat("<profile description=\"%dx%d %s fps\" width=\"%d\" height=\"%d\" progressive=\"1\" sample_aspect_num=\"1\" sample_aspect_den=\"1\" display_aspect_num=\"%d\" display_aspect_den=\"%d\" frame_rate_num=\"%d\" frame_rate_den=\"%d\" colorspace=\"709\"/>",
						width, height, state.fps_den == 1 ? itos(state.fps_num) : String::num(get_fps(), 2), width, height, width / aspect, height / aspect, state.fps_num, state.fps_den));

	xml.line(1, vformat("<producer id=\"black\" in=\"0\" out=\"%d\">", duration - 1));
	xml.property(2, "length", duration);
	xml.property(2, "eof", "pause");
	xml.property(2, "resource", "black");
	xml.property(2, "aspect_ratio", 1);
	xml.property(2, "mlt_service", "color");
	xml.property(2, "set.test_audio", 0);
	xml.line(1, "</producer>");
	xml.line(1, "<playlist id=\"background\">");
	xml.line(2, vformat("<entry producer=\"black\" in=\"0\" out=\"%d\"/>", duration - 1));
	xml.line(1, "</playlist>");

	for (int track_index = 0; track_index < state.tracks.size(); track_index++) {
		const Track &track = state.tracks[track_index];
		const Vector<int> ordered = _sorted_track_clips(state, track_index);

		// One producer per use, so that two tracks never fight over one decoder.
		for (int clip_index : ordered) {
			const Clip &clip = state.clips[clip_index];
			const BinClip &bin = state.bin[_find_bin(state, clip.bin_id)];
			const int length = _bin_is_still(bin) ? MAX(bin.length, clip.out + 1) : bin.length;
			xml.line(1, vformat("<producer id=\"clip%d\" in=\"0\" out=\"%d\">", clip.id, length - 1));
			xml.property(2, "length", length);
			xml.property(2, "eof", "pause");
			Dictionary extra = bin.properties;
			switch (bin.kind) {
				case CLIP_COLOR:
					xml.property(2, "resource", _mlt_color(bin.source));
					xml.property(2, "aspect_ratio", 1);
					xml.property(2, "mlt_service", "color");
					break;
				case CLIP_IMAGE:
					xml.property(2, "resource", _globalize(bin.source));
					xml.property(2, "ttl", length);
					xml.property(2, "mlt_service", image_producer);
					break;
				case CLIP_TEXT: {
					xml.property(2, "mlt_service", text_producer);
					Dictionary title;
					title["text"] = bin.source;
					title["family"] = "Sans";
					// The text image is scaled to fit the frame, so the size sets its sharpness and the pad its margin.
					title["size"] = MAX(32, height / 4);
					title["weight"] = 700;
					title["fgcolour"] = "0xffffffff";
					title["bgcolour"] = "0x00000000";
					title["align"] = 1;
					title["pad"] = MAX(16, height / 8);
					title.merge(extra, true);
					extra = title;
				} break;
				default:
					xml.property(2, "resource", _globalize(bin.source));
					xml.property(2, "mlt_service", "avformat");
					// Kdenlive's rule: a video track never plays sound and an audio track never decodes pictures.
					if (track.kind == TRACK_VIDEO && bin.has_audio) {
						xml.property(2, "audio_index", -1);
					} else if (track.kind == TRACK_AUDIO && bin.has_video) {
						xml.property(2, "video_index", -1);
					}
					break;
			}
			xml.properties(2, extra);
			xml.line(1, "</producer>");
		}

		xml.line(1, vformat("<playlist id=\"track%d\">", track_index));
		if (with_tetra) {
			xml.property(2, "tetra:kind", track.kind == TRACK_VIDEO ? "video" : "audio");
			xml.property(2, "tetra:name", track.name);
			xml.property(2, "tetra:hidden", track.hidden ? 1 : 0);
			xml.property(2, "tetra:muted", track.muted ? 1 : 0);
			xml.property(2, "tetra:locked", track.locked ? 1 : 0);
		}
		int cursor = 0;
		for (int clip_index : ordered) {
			const Clip &clip = state.clips[clip_index];
			if (clip.position > cursor) {
				xml.line(2, vformat("<blank length=\"%d\"/>", clip.position - cursor));
			}
			cursor = clip.end();
			xml.line(2, vformat("<entry producer=\"clip%d\" in=\"%d\" out=\"%d\">", clip.id, clip.in, clip.out));
			if (with_tetra) {
				xml.property(3, "tetra:id", clip.id);
				xml.property(3, "tetra:bin_id", clip.bin_id);
				xml.property(3, "tetra:link", clip.link);
				xml.property(3, "tetra:fade_in", clip.fade_in);
				xml.property(3, "tetra:fade_out", clip.fade_out);
			}
			// Filters on a playlist entry see frame 0 at the start of the clip, which keeps animations clip-relative.
			for (int i = 0; i < clip.effects.size(); i++) {
				const Effect &effect = clip.effects[i];
				xml.line(3, vformat("<filter id=\"clip%d_effect%d\">", clip.id, i));
				xml.property(4, "mlt_service", effect.service);
				xml.properties(4, TetraVideoAssets::effect_to_mlt_properties(effect.operation, effect.values, width, height, clip.length()));
				if (!effect.enabled) {
					xml.property(4, "disable", 1);
				}
				if (with_tetra) {
					xml.property(4, "tetra:operation", effect.operation);
					xml.property(4, "tetra:values", VariantUtilityFunctions::var_to_str(effect.values));
				}
				xml.line(3, "</filter>");
			}
			const int length = clip.length();
			for (int fade = 0; fade < 2; fade++) {
				const bool fade_in = fade == 0;
				const int frames = fade_in ? clip.fade_in : clip.fade_out;
				if (frames < 1) {
					continue;
				}
				const int first = fade_in ? 0 : MAX(0, length - 1 - frames);
				xml.line(3, vformat("<filter id=\"clip%d_fade_%s\">", clip.id, fade_in ? "in" : "out"));
				if (track.kind == TRACK_AUDIO) {
					xml.property(4, "mlt_service", "volume");
					xml.property(4, "level", _audio_fade_animation(first, frames, fade_in));
				} else {
					// The lowest video track fades to black; the ones above fade to transparent and reveal what is below.
					xml.property(4, "mlt_service", "brightness");
					const String animation = fade_in ? vformat("%d=0;%d=1", first, first + frames) : vformat("%d=1;%d=0", first, first + frames);
					xml.property(4, track_index == lowest_video_track ? "level" : "alpha", animation);
				}
				if (with_tetra) {
					xml.property(4, "tetra:fade", fade_in ? "in" : "out");
				}
				xml.line(3, "</filter>");
			}
			xml.line(2, "</entry>");
		}
		xml.line(1, "</playlist>");
	}

	xml.line(1, vformat("<tractor id=\"main_tractor\" in=\"0\" out=\"%d\">", duration - 1));
	if (with_tetra) {
		xml.property(2, "tetra:version", FORMAT_VERSION);
		xml.property(2, "tetra:sample_rate", state.sample_rate);
		xml.property(2, "tetra:next_bin_id", state.next_bin_id);
		xml.property(2, "tetra:next_clip_id", state.next_clip_id);
		for (const BinClip &bin : state.bin) {
			xml.property(2, "tetra:bin." + itos(bin.id), VariantUtilityFunctions::var_to_str(_bin_to_dictionary(bin)));
		}
		for (const Marker &marker : state.markers) {
			xml.property(2, "tetra:marker." + itos(marker.frame), marker.text);
		}
	}
	xml.line(2, "<track producer=\"background\"/>");
	for (int i = 0; i < state.tracks.size(); i++) {
		const Track &track = state.tracks[i];
		const char *hide = track.kind == TRACK_VIDEO ? (track.hidden ? "both" : "audio") : (track.muted ? "both" : "video");
		xml.line(2, vformat("<track producer=\"track%d\" hide=\"%s\"/>", i, hide));
	}
	for (int i = 0; i < state.tracks.size(); i++) {
		const bool audio = state.tracks[i].kind == TRACK_AUDIO;
		xml.line(2, vformat("<transition id=\"%s%d\">", audio ? "mix" : "composite", i + 1));
		xml.property(3, "a_track", 0);
		xml.property(3, "b_track", i + 1);
		xml.property(3, "mlt_service", audio ? String("mix") : compositing);
		xml.property(3, "always_active", 1);
		if (audio) {
			xml.property(3, "sum", 1);
			xml.property(3, "accepts_blanks", 1);
		}
		xml.line(2, "</transition>");
	}
	xml.line(1, "</tractor>");
	xml.line(0, "</mlt>");
	return xml.get();
}

Error TetraVideoProject::save(const String &p_path) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		last_error = vformat("Cannot write \"%s\".", p_path);
		return error == OK ? ERR_FILE_CANT_WRITE : error;
	}
	file->store_string(to_mlt_xml());
	last_error = String();
	return OK;
}

Error TetraVideoProject::load(const String &p_path) {
	Error error = OK;
	const String xml = FileAccess::get_file_as_string(p_path, &error);
	if (error != OK) {
		last_error = vformat("Cannot read \"%s\".", p_path);
		return error;
	}
	return load_from_xml(xml);
}

Error TetraVideoProject::load_from_xml(const String &p_xml) {
	State loaded;
	String message;
	const Error error = _load_xml(p_xml, loaded, message);
	if (error != OK) {
		last_error = message;
		return error;
	}
	last_error = String();
	clear_undo_history();
	_apply_state(loaded);
	return OK;
}

Error TetraVideoProject::_load_xml(const String &p_xml, State &r_state, String &r_error) const {
	Vector<TetraXmlNode> nodes;
	const Error parse_error = tetra_parse_xml(p_xml, nodes, r_error);
	if (parse_error != OK) {
		return parse_error;
	}
	int root = -1;
	for (int child : nodes[0].children) {
		if (nodes[child].name == "mlt") {
			root = child;
		}
	}
	if (root < 0) {
		r_error = "Not an MLT XML document: there is no <mlt> element.";
		return ERR_FILE_UNRECOGNIZED;
	}

	HashMap<String, int> producers;
	HashMap<String, int> playlists;
	HashMap<String, int> tractors;
	Vector<int> playlist_order;
	int main_tractor = -1;
	State loaded;
	for (int child : nodes[root].children) {
		const TetraXmlNode &node = nodes[child];
		const String id = node.get_attribute("id");
		if (node.name == "profile") {
			loaded.width = node.get_attribute("width", "1920").to_int();
			loaded.height = node.get_attribute("height", "1080").to_int();
			loaded.fps_num = node.get_attribute("frame_rate_num", "25").to_int();
			loaded.fps_den = node.get_attribute("frame_rate_den", "1").to_int();
		} else if (node.name == "producer" || node.name == "chain") {
			producers[id] = child;
		} else if (node.name == "playlist") {
			playlists[id] = child;
			playlist_order.push_back(child);
		} else if (node.name == "tractor") {
			tractors[id] = child;
			main_tractor = child;
		}
	}
	if (loaded.width < 1 || loaded.height < 1 || loaded.fps_num < 1 || loaded.fps_den < 1) {
		r_error = "The profile has a non-positive size or frame rate.";
		return ERR_INVALID_DATA;
	}
	const double fps = double(loaded.fps_num) / double(loaded.fps_den);
	if (tractors.has(nodes[root].get_attribute("producer"))) {
		main_tractor = tractors[nodes[root].get_attribute("producer")];
	}

	// The playlists that make up each timeline track, bottom to top, and whether MLT hides the track's video.
	Vector<Vector<int>> track_playlists;
	Vector<bool> track_is_audio;
	if (main_tractor >= 0) {
		for (int child : nodes[main_tractor].children) {
			if (nodes[child].name != "track") {
				continue;
			}
			const String producer = nodes[child].get_attribute("producer");
			Vector<int> sources;
			bool audio = nodes[child].get_attribute("hide") == "video";
			if (playlists.has(producer)) {
				sources.push_back(playlists[producer]);
			} else if (tractors.has(producer)) {
				// Kdenlive wraps the two playlists of a track in a tractor of their own.
				const TetraXmlNode &sub = nodes[tractors[producer]];
				audio = audio || _lookup(_properties(nodes, sub), "kdenlive:audio_track") == "1";
				for (int sub_child : sub.children) {
					const String sub_producer = nodes[sub_child].get_attribute("producer");
					if (nodes[sub_child].name == "track" && playlists.has(sub_producer)) {
						sources.push_back(playlists[sub_producer]);
						audio = audio || nodes[sub_child].get_attribute("hide") == "video";
					}
				}
			}
			if (!sources.is_empty()) {
				track_playlists.push_back(sources);
				track_is_audio.push_back(audio);
			}
		}
	} else {
		for (int playlist : playlist_order) {
			Vector<int> sources;
			sources.push_back(playlist);
			track_playlists.push_back(sources);
			track_is_audio.push_back(false);
		}
	}
	if (track_playlists.is_empty()) {
		r_error = "The document has no playlist to load as a track.";
		return ERR_INVALID_DATA;
	}

	const HashMap<String, String> project_properties = main_tractor >= 0 ? _properties(nodes, nodes[main_tractor]) : HashMap<String, String>();
	const bool native = project_properties.has("tetra:version");
	if (native) {
		if (_lookup(project_properties, "tetra:version").to_int() > FORMAT_VERSION) {
			r_error = "The project was saved by a newer version of Tetra Studio.";
			return ERR_FILE_UNRECOGNIZED;
		}
		loaded.sample_rate = _lookup(project_properties, "tetra:sample_rate", "48000").to_int();
		for (const KeyValue<String, String> &property : project_properties) {
			if (property.key.begins_with("tetra:bin.")) {
				const Variant parsed = VariantUtilityFunctions::str_to_var(property.value);
				if (parsed.get_type() != Variant::DICTIONARY) {
					r_error = vformat("The bin entry \"%s\" cannot be read.", property.key);
					return ERR_INVALID_DATA;
				}
				const Dictionary d = parsed;
				BinClip bin;
				bin.id = d.get("id", 0);
				bin.kind = ClipKind(int(d.get("kind", 0)));
				bin.name = d.get("name", String());
				bin.source = d.get("source", String());
				bin.length = d.get("length", 0);
				bin.has_video = d.get("has_video", true);
				bin.has_audio = d.get("has_audio", false);
				bin.properties = Dictionary(d.get("properties", Dictionary())).duplicate(true);
				int at = 0;
				while (at < loaded.bin.size() && loaded.bin[at].id < bin.id) {
					at++;
				}
				loaded.bin.insert(at, bin);
			} else if (property.key.begins_with("tetra:marker.")) {
				Marker marker;
				marker.frame = property.key.trim_prefix("tetra:marker.").to_int();
				marker.text = property.value;
				int at = 0;
				while (at < loaded.markers.size() && loaded.markers[at].frame < marker.frame) {
					at++;
				}
				loaded.markers.insert(at, marker);
			}
		}
	}

	// Bin clips made up for the producers of a foreign document, by service and resource.
	HashMap<String, int> foreign_bin;
	for (int track_index = 0; track_index < track_playlists.size(); track_index++) {
		const Vector<int> &sources = track_playlists[track_index];
		const HashMap<String, String> track_properties = _properties(nodes, nodes[sources[0]]);
		Track track;
		Vector<Clip> track_clips;
		if (native) {
			track.kind = _lookup(track_properties, "tetra:kind") == "audio" ? TRACK_AUDIO : TRACK_VIDEO;
			track.name = _lookup(track_properties, "tetra:name");
			track.hidden = _lookup(track_properties, "tetra:hidden") == "1";
			track.muted = _lookup(track_properties, "tetra:muted") == "1";
			track.locked = _lookup(track_properties, "tetra:locked") == "1";
		} else {
			track.kind = track_is_audio[track_index] ? TRACK_AUDIO : TRACK_VIDEO;
		}
		bool only_background = !native;
		for (int source : sources) {
			int cursor = 0;
			for (int child : nodes[source].children) {
				const TetraXmlNode &node = nodes[child];
				if (node.name == "blank") {
					cursor += MAX(0, _parse_time(node.get_attribute("length"), fps, 0));
					continue;
				}
				if (node.name != "entry") {
					continue;
				}
				const String producer_id = node.get_attribute("producer");
				if (!producers.has(producer_id)) {
					r_error = vformat("A playlist entry refers to the missing producer \"%s\".", producer_id);
					return ERR_INVALID_DATA;
				}
				const TetraXmlNode &producer = nodes[producers[producer_id]];
				const HashMap<String, String> producer_properties = _properties(nodes, producer);
				const HashMap<String, String> entry_properties = _properties(nodes, node);
				const int producer_length = _parse_time(_lookup(producer_properties, "length"), fps, _parse_time(producer.get_attribute("out"), fps, -1) + 1);
				Clip clip;
				clip.track = loaded.tracks.size();
				clip.position = cursor;
				clip.in = _parse_time(node.get_attribute("in"), fps, 0);
				clip.out = _parse_time(node.get_attribute("out"), fps, MAX(clip.in, producer_length - 1));
				if (clip.in < 0 || clip.out < clip.in) {
					r_error = vformat("The entry of producer \"%s\" has illegal in/out points.", producer_id);
					return ERR_INVALID_DATA;
				}
				cursor += clip.length();

				const String service = _lookup(producer_properties, "mlt_service", producer.name == "chain" ? "avformat" : "");
				const String resource = _lookup(producer_properties, "resource");
				if (native) {
					clip.id = _lookup(entry_properties, "tetra:id").to_int();
					clip.bin_id = _lookup(entry_properties, "tetra:bin_id").to_int();
					clip.link = _lookup(entry_properties, "tetra:link").to_int();
					clip.fade_in = _lookup(entry_properties, "tetra:fade_in").to_int();
					clip.fade_out = _lookup(entry_properties, "tetra:fade_out").to_int();
				} else {
					const bool is_color = service == "color" || service == "colour";
					if (!(is_color && (resource == "black" || resource == "0" || producer_id.begins_with("black")))) {
						only_background = false;
					}
					const String key = service + "|" + resource + "|" + _lookup(producer_properties, "text");
					if (!foreign_bin.has(key)) {
						BinClip bin;
						bin.id = loaded.next_bin_id++;
						bin.name = resource.get_file().is_empty() ? producer_id : resource.get_file();
						bin.source = resource;
						bin.length = MAX(1, MAX(producer_length, clip.out + 1));
						bin.has_video = false;
						bin.has_audio = false;
						if (is_color) {
							bin.kind = CLIP_COLOR;
							const Color color = resource.begins_with("0x") && resource.length() == 10 ? Color::html(resource.substr(2)) : Color::from_string(resource, Color(0, 0, 0, 1));
							bin.source = "#" + color.to_html(true);
						} else if (service == "pixbuf" || service == "qimage") {
							bin.kind = CLIP_IMAGE;
						} else if (service == "pango" || service == "qtext" || service == "kdenlivetitle") {
							bin.kind = CLIP_TEXT;
							bin.source = _lookup(producer_properties, "text", resource);
						} else {
							bin.kind = CLIP_VIDEO;
						}
						foreign_bin[key] = loaded.bin.size();
						loaded.bin.push_back(bin);
					}
					BinClip &bin = loaded.bin.write[foreign_bin[key]];
					bin.length = MAX(bin.length, clip.out + 1);
					// A media file is taken to carry whatever the tracks it sits on play.
					if (track.kind == TRACK_VIDEO) {
						bin.has_video = true;
					} else if (bin.kind == CLIP_VIDEO) {
						bin.has_audio = true;
					} else {
						r_error = vformat("The still or title \"%s\" is on an audio track.", producer_id);
						return ERR_INVALID_DATA;
					}
					clip.id = loaded.next_clip_id++;
					clip.bin_id = bin.id;
				}

				for (int filter_index : node.children) {
					if (nodes[filter_index].name != "filter") {
						continue;
					}
					const HashMap<String, String> filter_properties = _properties(nodes, nodes[filter_index]);
					if (filter_properties.has("tetra:fade")) {
						continue;
					}
					Effect effect;
					effect.service = _lookup(filter_properties, "mlt_service");
					effect.enabled = _lookup(filter_properties, "disable") != "1";
					if (effect.service.is_empty()) {
						continue;
					}
					if (native && filter_properties.has("tetra:values")) {
						effect.operation = _lookup(filter_properties, "tetra:operation", "mlt:" + effect.service);
						const Variant parsed = VariantUtilityFunctions::str_to_var(_lookup(filter_properties, "tetra:values"));
						if (parsed.get_type() == Variant::DICTIONARY) {
							effect.values = parsed;
						}
					} else {
						effect.operation = "mlt:" + effect.service;
						for (const KeyValue<String, String> &property : filter_properties) {
							if (property.key != "mlt_service" && property.key != "disable" && !property.key.begins_with("_")) {
								effect.values[property.key] = property.value;
							}
						}
					}
					clip.effects.push_back(effect);
				}
				bool overlaps = false;
				for (const Clip &other : track_clips) {
					overlaps = overlaps || (clip.position < other.end() && clip.end() > other.position);
				}
				if (!overlaps) {
					track_clips.push_back(clip);
				}
			}
		}
		// The black background of Kdenlive, Shotcut and Tetra files is rebuilt on export, not loaded as a track.
		if (!native && track_index == 0 && only_background && track_playlists.size() > 1) {
			continue;
		}
		if (native && nodes[sources[0]].get_attribute("id") == "background") {
			continue;
		}
		if (track.name.is_empty()) {
			int same_kind = 1;
			for (const Track &other : loaded.tracks) {
				same_kind += other.kind == track.kind ? 1 : 0;
			}
			track.name = (track.kind == TRACK_VIDEO ? "V" : "A") + itos(same_kind);
		}
		for (Clip clip : track_clips) {
			clip.track = loaded.tracks.size();
			loaded.clips.push_back(clip);
		}
		loaded.tracks.push_back(track);
	}

	if (native) {
		loaded.next_bin_id = _lookup(project_properties, "tetra:next_bin_id", "1").to_int();
		loaded.next_clip_id = _lookup(project_properties, "tetra:next_clip_id", "1").to_int();
	} else {
		for (int i = loaded.bin.size() - 1; i >= 0; i--) {
			bool used = false;
			for (const Clip &clip : loaded.clips) {
				used = used || clip.bin_id == loaded.bin[i].id;
			}
			if (!used) {
				// Only the skipped background track referred to it.
				loaded.bin.remove_at(i);
			} else if (!loaded.bin[i].has_video && loaded.bin[i].has_audio) {
				// A media file that only ever sat on audio tracks is an audio clip.
				loaded.bin.write[i].kind = CLIP_AUDIO;
			}
		}
	}
	const String problem = _check_state(loaded);
	if (!problem.is_empty()) {
		r_error = "The document describes an inconsistent timeline: " + problem + ".";
		return ERR_INVALID_DATA;
	}
	r_state = loaded;
	return OK;
}
