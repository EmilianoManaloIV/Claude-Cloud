#include "tetra_mlt_engine.h"

#include "../tetra_video_project.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/audio/audio_server.h"
#include "servers/audio/effects/audio_stream_generator.h"

#ifdef TETRA_MLT_ENABLED

#include <Mlt.h>
#include <framework/mlt_audio.h>
#include <framework/mlt_log.h>
#include <framework/mlt_version.h>

#include <clocale>

struct TetraMltEngine::Graph {
	Mlt::Profile profile;
	Mlt::Producer *producer = nullptr;
	int length = 0;

	~Graph() {
		delete producer;
	}
};

namespace {

// Every call into MLT, from any engine instance or thread, holds this mutex.
Mutex mlt_mutex;
Mlt::Repository *repository = nullptr;
bool init_attempted = false;
HashMap<String, PackedStringArray> service_lists;
HashMap<String, HashSet<String>> service_sets;

void _collect_services(const String &p_kind, Mlt::Properties *p_services) {
	PackedStringArray names;
	HashSet<String> set;
	if (p_services) {
		for (int i = 0; i < p_services->count(); i++) {
			const String name = String::utf8(p_services->get_name(i));
			names.push_back(name);
			set.insert(name);
		}
		delete p_services;
	}
	names.sort();
	service_lists[p_kind] = names;
	service_sets[p_kind] = set;
}

// Starts MLT once. Its plug-ins may switch the process locale to the user's,
// which would make both MLT and Godot print decimal commas, so the numeric
// locale is pinned back to "C" afterwards (and again after building graphs,
// because toolkit-based producers are loaded lazily).
bool _ensure_mlt() {
	MutexLock lock(mlt_mutex);
	if (!init_attempted) {
		init_attempted = true;
		mlt_log_set_level(MLT_LOG_ERROR);
		repository = Mlt::Factory::init();
		setlocale(LC_NUMERIC, "C");
		if (repository) {
			_collect_services("producer", repository->producers());
			_collect_services("filter", repository->filters());
			_collect_services("transition", repository->transitions());
			_collect_services("consumer", repository->consumers());
		}
	}
	return repository != nullptr;
}

String _globalize(const String &p_path) {
	if (p_path.begins_with("res://") || p_path.begins_with("user://")) {
		return ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return p_path;
}

String _text(Mlt::Properties &p_properties, const char *p_name) {
	const char *value = p_properties.get(p_name);
	return value ? String::utf8(value) : String();
}

// MLT writes colours as "#rrggbb", "#aarrggbb", "0xrrggbbaa" or a few names.
Color _parse_color(const String &p_text) {
	const String text = p_text.strip_edges().to_lower();
	if (text.begins_with("0x") && text.length() == 10) {
		return Color::html(text.substr(2));
	}
	if (text.begins_with("#") && text.length() == 9) {
		return Color::html(text.substr(3) + text.substr(1, 2));
	}
	return Color::from_string(text, Color(0, 0, 0, 1));
}

Vector2i _fit_size(int p_width, int p_height, const Vector2i &p_max) {
	if (p_max.x <= 0 || p_max.y <= 0) {
		return Vector2i(p_width, p_height);
	}
	const double scale = MIN(1.0, MIN(double(p_max.x) / p_width, double(p_max.y) / p_height));
	// MLT's scalers and chroma-subsampled filters want even sizes.
	return Vector2i(MAX(2, int(Math::round(p_width * scale)) & ~1), MAX(2, int(Math::round(p_height * scale)) & ~1));
}

// Pulls the picture of a frame as RGBA at the profile size fitted into
// p_max_size. The "consumer." properties are what a real MLT consumer sets on
// every frame; the rescale normaliser of each producer obeys them.
Ref<Image> _frame_image(Mlt::Frame &p_frame, Mlt::Profile &p_profile, const Vector2i &p_max_size) {
	const Vector2i size = _fit_size(p_profile.width(), p_profile.height(), p_max_size);
	p_frame.set("consumer.rescale", "bilinear");
	p_frame.set("consumer.progressive", 1);
	p_frame.set("consumer.deinterlacer", "onefield");
	mlt_image_format format = mlt_image_rgba;
	int width = size.x;
	int height = size.y;
	const uint8_t *data = p_frame.get_image(format, width, height, 0);
	if (!data || format != mlt_image_rgba || width < 1 || height < 1) {
		return Ref<Image>();
	}
	Vector<uint8_t> pixels;
	pixels.resize(width * height * 4);
	memcpy(pixels.ptrw(), data, pixels.size());
	Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, pixels);
	if (width != size.x || height != size.y) {
		image->resize(size.x, size.y, Image::INTERPOLATE_BILINEAR);
	}
	return image;
}

Ref<Image> _grab(Mlt::Producer &p_producer, Mlt::Profile &p_profile, int p_frame, const Vector2i &p_max_size) {
	p_producer.seek(p_frame);
	Mlt::Frame *frame = p_producer.get_frame();
	if (!frame) {
		return Ref<Image>();
	}
	Ref<Image> image = _frame_image(*frame, p_profile, p_max_size);
	delete frame;
	return image;
}

// Opens a media file through MLT's loader, which picks the producer and adds
// the normalising filters. Stills are sent to the producer the timeline uses
// for them instead of whatever the loader would try first.
Mlt::Producer *_open_file(Mlt::Profile &r_profile, const String &p_path) {
	static const char *still_extensions[] = { "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "svg", "tga" };
	String resource = p_path;
	const String extension = p_path.get_extension().to_lower();
	for (const char *still : still_extensions) {
		if (extension == still) {
			const String service = TetraMltEngine::get_preferred_service("producer", "pixbuf,qimage");
			if (TetraMltEngine::has_service("producer", service)) {
				resource = service + ":" + p_path;
			}
		}
	}
	return new Mlt::Producer(r_profile, nullptr, resource.utf8().get_data());
}

void _set_profile(Mlt::Profile &r_profile, int p_width, int p_height, int p_fps_num, int p_fps_den) {
	r_profile.set_width(p_width);
	r_profile.set_height(p_height);
	r_profile.set_frame_rate(p_fps_num, p_fps_den);
	r_profile.set_sample_aspect(1, 1);
	r_profile.set_display_aspect(p_width, p_height);
	r_profile.set_progressive(1);
	r_profile.set_explicit(1);
}

// One entry of MLT's YAML service metadata as a parameter of the operation schema.
Dictionary _metadata_parameter(Mlt::Properties &p_parameter) {
	Dictionary param;
	const String identifier = _text(p_parameter, "identifier");
	const String mlt_type = _text(p_parameter, "type");
	const String widget = _text(p_parameter, "widget");
	if (identifier.is_empty() || _text(p_parameter, "readonly") == "yes") {
		return param;
	}
	Array options;
	if (mlt_properties values = mlt_properties(p_parameter.get_data("values"))) {
		Mlt::Properties list(values);
		for (int i = 0; i < list.count(); i++) {
			Dictionary option;
			const String value = String::utf8(list.get(i));
			option["value"] = mlt_type == "integer" ? Variant(value.to_int()) : Variant(value);
			option["label"] = value;
			bool repeated = false;
			for (int j = 0; j < options.size(); j++) {
				repeated = repeated || Dictionary(options[j])["value"] == option["value"];
			}
			if (!repeated) {
				options.push_back(option);
			}
		}
	}
	const String default_text = _text(p_parameter, "default");
	String type = "string";
	Variant default_value = default_text;
	const bool has_default = p_parameter.get("default") != nullptr;
	if (!options.is_empty() && (mlt_type == "string" || (mlt_type == "integer" && has_default))) {
		type = "enum";
		default_value = mlt_type == "integer" ? Variant(default_text.to_int()) : Variant(default_text);
		bool listed = false;
		for (int i = 0; i < options.size(); i++) {
			listed = listed || Dictionary(options[i])["value"] == default_value;
		}
		if (!listed) {
			// Either the metadata names no default (the empty choice leaves the property unset)
			// or it gives one in another notation than the listed keywords.
			Dictionary option;
			option["value"] = default_value;
			option["label"] = has_default ? default_text : String("(default)");
			options.push_front(option);
		}
	} else if (mlt_type == "integer") {
		type = "int";
		default_value = default_text.to_int();
	} else if (mlt_type == "float") {
		type = "float";
		default_value = default_text.to_float();
	} else if (mlt_type == "boolean") {
		type = "bool";
		default_value = default_text == "1" || default_text == "yes" || default_text == "true";
	} else if (mlt_type == "color") {
		type = "color";
		default_value = _parse_color(default_text);
	} else if (widget == "fileopen" || widget == "filesave") {
		type = "file";
	} else if (widget == "textbox") {
		type = "text";
	}
	param["name"] = identifier;
	param["label"] = _text(p_parameter, "title").is_empty() ? identifier.capitalize() : _text(p_parameter, "title");
	param["description"] = _text(p_parameter, "description");
	param["type"] = type;
	param["default"] = default_value;
	if (!has_default) {
		// The service has its own built-in default; the value is only sent once it is changed.
		param["no_default"] = true;
	}
	if (type == "int" || type == "float") {
		// Some generated metadata (LADSPA) states a default outside its own range; the range gives way.
		const double number = default_value;
		if (p_parameter.get("minimum")) {
			param["min"] = has_default ? MIN(number, _text(p_parameter, "minimum").to_float()) : _text(p_parameter, "minimum").to_float();
		}
		if (p_parameter.get("maximum")) {
			param["max"] = has_default ? MAX(number, _text(p_parameter, "maximum").to_float()) : _text(p_parameter, "maximum").to_float();
		}
	}
	if (!_text(p_parameter, "unit").is_empty()) {
		param["unit"] = _text(p_parameter, "unit");
	}
	if (type == "enum") {
		param["options"] = options;
	}
	if (_text(p_parameter, "animation") == "yes") {
		param["animated"] = true;
	}
	return param;
}

} // namespace

bool TetraMltEngine::is_available() {
	return _ensure_mlt();
}

String TetraMltEngine::get_version() {
	return _ensure_mlt() ? String::utf8(mlt_version_get_string()) : String();
}

PackedStringArray TetraMltEngine::list_services(const String &p_kind) {
	if (!_ensure_mlt() || !service_lists.has(p_kind)) {
		return PackedStringArray();
	}
	return service_lists[p_kind];
}

bool TetraMltEngine::has_service(const String &p_kind, const String &p_name) {
	return _ensure_mlt() && service_sets.has(p_kind) && service_sets[p_kind].has(p_name);
}

Dictionary TetraMltEngine::get_service_operation(const String &p_kind, const String &p_name) {
	Dictionary operation;
	if (!has_service(p_kind, p_name)) {
		return operation;
	}
	mlt_service_type type = mlt_service_filter_type;
	if (p_kind == "producer") {
		type = mlt_service_producer_type;
	} else if (p_kind == "transition") {
		type = mlt_service_transition_type;
	} else if (p_kind == "consumer") {
		type = mlt_service_consumer_type;
	}
	MutexLock lock(mlt_mutex);
	Array params;
	String label = p_name;
	String description;
	bool audio = false;
	bool hidden = false;
	Mlt::Properties *metadata = repository->metadata(type, p_name.utf8().get_data());
	const bool has_metadata = metadata && metadata->is_valid() && metadata->count() > 0;
	if (has_metadata) {
		if (!_text(*metadata, "title").is_empty()) {
			label = _text(*metadata, "title");
		}
		description = _text(*metadata, "description");
		if (mlt_properties tags = mlt_properties(metadata->get_data("tags"))) {
			Mlt::Properties list(tags);
			bool video = false;
			for (int i = 0; i < list.count(); i++) {
				const String tag = String::utf8(list.get(i));
				audio = audio || tag == "Audio";
				video = video || tag == "Video";
				hidden = hidden || tag == "Hidden";
			}
			audio = audio && !video;
		}
		if (mlt_properties parameters = mlt_properties(metadata->get_data("parameters"))) {
			Mlt::Properties list(parameters);
			for (int i = 0; i < list.count(); i++) {
				mlt_properties entry = mlt_properties(list.get_data(list.get_name(i)));
				if (!entry) {
					continue;
				}
				Mlt::Properties parameter(entry);
				const Dictionary param = _metadata_parameter(parameter);
				if (!param.is_empty()) {
					params.push_back(param);
				}
			}
		}
	}
	delete metadata;
	operation["id"] = "mlt:" + p_name;
	operation["label"] = label;
	operation["description"] = description;
	operation["category"] = p_kind == "filter" ? (audio ? "Audio/MLT" : "Video/MLT") : p_kind.capitalize();
	operation["source"] = "mlt";
	operation["params"] = params;
	operation["service"] = p_name;
	operation["kind"] = p_kind == "filter" ? (audio ? "audio_effect" : "video_effect") : p_kind;
	operation["has_metadata"] = has_metadata;
	operation["hidden"] = hidden;
	return operation;
}

Dictionary TetraMltEngine::probe(const String &p_path, const Ref<TetraVideoProject> &p_project) {
	Dictionary info;
	const String path = _globalize(p_path);
	if (!_ensure_mlt() || !FileAccess::exists(path)) {
		return info;
	}
	MutexLock lock(mlt_mutex);
	Mlt::Profile profile;
	if (p_project.is_valid()) {
		_set_profile(profile, p_project->get_width(), p_project->get_height(), p_project->get_fps_num(), p_project->get_fps_den());
	}
	Mlt::Producer *producer = _open_file(profile, path);
	if (producer->is_valid() && p_project.is_null()) {
		// Without a project the length is counted in the file's own frames.
		profile.from_producer(*producer);
		profile.set_explicit(1);
		delete producer;
		producer = _open_file(profile, path);
	}
	setlocale(LC_NUMERIC, "C");
	if (!producer->is_valid()) {
		delete producer;
		return info;
	}
	const String service = _text(*producer, "mlt_service");
	const bool media = service.begins_with("avformat");
	const bool still = service == "pixbuf" || service == "qimage";
	const int audio_index = producer->get_int("audio_index");
	const int video_index = producer->get_int("video_index");
	const bool has_video = media ? video_index >= 0 : true;
	const bool has_audio = media && audio_index >= 0;
	const double fps = profile.fps();
	int length = producer->get_length();
	if (still) {
		length = int(Math::round(5.0 * fps));
	}
	double source_fps = fps;
	if (media && has_video && producer->get_int("meta.media.frame_rate_den") > 0) {
		source_fps = producer->get_double("meta.media.frame_rate_num") / producer->get_double("meta.media.frame_rate_den");
	}
	info["service"] = service;
	info["kind"] = still ? TetraVideoProject::CLIP_IMAGE : (has_video ? TetraVideoProject::CLIP_VIDEO : TetraVideoProject::CLIP_AUDIO);
	info["length"] = length;
	info["duration"] = length / fps;
	info["fps"] = source_fps;
	// avformat describes each stream when the file is opened; "meta.media.width" only appears with the first frame.
	int width = producer->get_int("meta.media.width");
	int height = producer->get_int("meta.media.height");
	if (media && has_video && (width < 1 || height < 1)) {
		width = producer->get_int(vformat("meta.media.%d.codec.width", video_index).utf8().get_data());
		height = producer->get_int(vformat("meta.media.%d.codec.height", video_index).utf8().get_data());
	}
	info["width"] = has_video ? width : 0;
	info["height"] = has_video ? height : 0;
	info["has_video"] = has_video;
	info["has_audio"] = has_audio;
	info["sample_rate"] = has_audio ? producer->get_int(vformat("meta.media.%d.codec.sample_rate", audio_index).utf8().get_data()) : 0;
	info["channels"] = has_audio ? producer->get_int(vformat("meta.media.%d.codec.channels", audio_index).utf8().get_data()) : 0;
	delete producer;
	return info;
}

PackedFloat32Array TetraMltEngine::get_audio_peaks(const String &p_path, int p_samples_per_second) {
	PackedFloat32Array peaks;
	const String path = _globalize(p_path);
	if (!_ensure_mlt() || p_samples_per_second < 1 || !FileAccess::exists(path)) {
		return peaks;
	}
	// The producer is private to this call; the lock is taken per frame so that playback keeps running meanwhile.
	Mlt::Profile *profile = nullptr;
	Mlt::Producer *producer = nullptr;
	int length = 0;
	double fps = 25.0;
	{
		MutexLock lock(mlt_mutex);
		profile = new Mlt::Profile();
		producer = new Mlt::Producer(*profile, nullptr, path.utf8().get_data());
		setlocale(LC_NUMERIC, "C");
		if (producer->is_valid()) {
			length = producer->get_length();
			fps = profile->fps();
		}
	}
	const int frequency = 16000;
	const double samples_per_peak = double(frequency) / p_samples_per_second;
	int64_t sample_position = 0;
	for (int i = 0; i < length; i++) {
		MutexLock lock(mlt_mutex);
		Mlt::Frame *frame = producer->get_frame();
		if (!frame) {
			break;
		}
		mlt_audio_format format = mlt_audio_f32le;
		int rate = frequency;
		int channels = 1;
		int samples = mlt_audio_calculate_frame_samples(float(fps), frequency, i);
		const float *data = (const float *)frame->get_audio(format, rate, channels, samples);
		if (data && format == mlt_audio_f32le && channels > 0) {
			for (int s = 0; s < samples; s++) {
				const int bucket = int((sample_position + s) / samples_per_peak);
				if (bucket >= peaks.size()) {
					peaks.resize(bucket + 1);
					peaks.write[bucket] = 0.0;
				}
				peaks.write[bucket] = MAX(peaks[bucket], Math::abs(data[s * channels]));
			}
		}
		sample_position += samples;
		delete frame;
	}
	MutexLock lock(mlt_mutex);
	delete producer;
	delete profile;
	return peaks;
}

Error TetraMltEngine::set_xml(const String &p_xml) {
	if (!_ensure_mlt()) {
		return ERR_UNAVAILABLE;
	}
	const CharString utf8 = p_xml.utf8();
	Graph *next = memnew(Graph);
	double next_fps = 25.0;
	int next_length = 0;
	{
		MutexLock lock(mlt_mutex);
		next->producer = new Mlt::Producer(next->profile, "xml-string", utf8.get_data());
		setlocale(LC_NUMERIC, "C");
		if (!next->producer->is_valid()) {
			memdelete(next);
			return ERR_INVALID_DATA;
		}
		next->length = next->producer->get_length();
		next_length = next->length;
		next_fps = next->profile.fps();
		if (graph) {
			memdelete(graph);
		}
		graph = next;
	}
	MutexLock lock(control_mutex);
	length = next_length;
	fps = next_fps;
	position = CLAMP(position, 0, MAX(0, length - 1));
	generation++;
	refresh_requested = thread.is_started();
	return OK;
}

bool TetraMltEngine::has_graph() const {
	MutexLock lock(mlt_mutex);
	return graph != nullptr;
}

Ref<Image> TetraMltEngine::render_frame(int p_frame, const Vector2i &p_max_size) {
	MutexLock lock(mlt_mutex);
	if (!graph) {
		return Ref<Image>();
	}
	return _grab(*graph->producer, graph->profile, CLAMP(p_frame, 0, MAX(0, graph->length - 1)), p_max_size);
}

Ref<Image> TetraMltEngine::get_thumbnail(const Variant &p_source, int p_frame, const Vector2i &p_size) {
	if (!_ensure_mlt()) {
		return Ref<Image>();
	}
	const bool from_bin = p_source.get_type() == Variant::DICTIONARY;
	const String key = (from_bin ? "bin:" : "file:") + p_source.operator String();
	String xml;
	{
		MutexLock lock(mlt_mutex);
		if (!thumbnail_graphs.has(key) && from_bin) {
			// A one-clip project goes through the same XML writer as the timeline, so a title or a colour looks
			// the same here. It is written before MLT is locked for good, because the writer consults the catalog.
			const Dictionary bin = p_source;
			Ref<TetraVideoProject> single;
			single.instantiate();
			single->set_profile(profile_width, profile_height, profile_fps_num, profile_fps_den, 48000);
			const int id = single->add_bin_clip(TetraVideoProject::ClipKind(int(bin.get("kind", 0))), bin.get("name", String()), bin.get("source", String()),
					bin.get("length", 1), bin.get("has_video", true), false, bin.get("properties", Dictionary()));
			single->add_track(TetraVideoProject::TRACK_VIDEO);
			if (id > 0 && single->insert_clip(id, 0, 0) > 0) {
				Dictionary options;
				options["tetra_properties"] = false;
				xml = single->to_mlt_xml(options);
			}
		}
	}
	MutexLock lock(mlt_mutex);
	Graph *source = thumbnail_graphs.has(key) ? thumbnail_graphs[key] : nullptr;
	if (!source) {
		source = memnew(Graph);
		_set_profile(source->profile, profile_width, profile_height, profile_fps_num, profile_fps_den);
		if (from_bin) {
			if (!xml.is_empty()) {
				source->producer = new Mlt::Producer(source->profile, "xml-string", xml.utf8().get_data());
			}
		} else {
			const String path = _globalize(p_source);
			if (FileAccess::exists(path)) {
				source->producer = _open_file(source->profile, path);
			}
		}
		setlocale(LC_NUMERIC, "C");
		if (!source->producer || !source->producer->is_valid()) {
			memdelete(source);
			return Ref<Image>();
		}
		source->length = source->producer->get_length();
		thumbnail_graphs[key] = source;
		thumbnail_order.push_back(key);
		// Keep a handful of sources open: thumbnails of one clip are asked for in runs.
		if (thumbnail_order.size() > 8) {
			memdelete(thumbnail_graphs[thumbnail_order[0]]);
			thumbnail_graphs.erase(thumbnail_order[0]);
			thumbnail_order.remove_at(0);
		}
	}
	return _grab(*source->producer, source->profile, CLAMP(p_frame, 0, MAX(0, source->length - 1)), p_size);
}

void TetraMltEngine::_drop_graph() {
	MutexLock lock(mlt_mutex);
	if (graph) {
		memdelete(graph);
		graph = nullptr;
	}
}

void TetraMltEngine::_drop_thumbnails() {
	MutexLock lock(mlt_mutex);
	for (KeyValue<String, Graph *> &source : thumbnail_graphs) {
		memdelete(source.value);
	}
	thumbnail_graphs.clear();
	thumbnail_order.clear();
}

// The playback thread.
//
// At normal speed frames are decoded in order a little ahead of the clock.
// Their sound goes straight into the audio ring buffer and their picture
// waits in a short queue until its time, so the sound that is heard and the
// picture that is shown belong together. When decoding cannot keep up,
// pictures are skipped (never sound) and, if that is not enough, playback
// jumps ahead to where the clock is. At any other speed the frame under the
// clock is rendered, silently.
void TetraMltEngine::_run_playback() {
	struct Queued {
		int64_t virtual_frame = 0;
		int frame = 0;
		Ref<Image> image;
	};
	LocalVector<Queued> queue;
	OS *os = OS::get_singleton();
	uint64_t seen_generation = UINT64_MAX;
	// Looping counts frames past the end ("virtual" frames) so that the clock never jumps.
	double anchor_frame = 0.0;
	uint64_t anchor_time = 0;
	int64_t next_decode = 0;
	int last_shown = -1;
	int last_skips = -1;
	PackedVector2Array audio_buffer;

	while (!exit_requested.is_set()) {
		control_mutex.lock();
		const bool now_playing = playing;
		const bool now_loop = loop;
		const double now_speed = speed;
		const double now_volume = volume;
		const double now_fps = fps;
		const uint64_t now_generation = generation;
		const int start = position;
		const int total = length;
		const Vector2i size = preview_size;
		const bool refresh = refresh_requested;
		refresh_requested = false;
		Ref<AudioStreamGeneratorPlayback> audio = audio_playback;
		control_mutex.unlock();

		if (now_generation != seen_generation) {
			seen_generation = now_generation;
			queue.clear();
			anchor_frame = start;
			anchor_time = os->get_ticks_usec();
			next_decode = start;
			last_shown = -1;
			last_skips = -1;
		}
		if (total < 1 || (!now_playing && !refresh)) {
			os->delay_usec(2000);
			continue;
		}
		if (!now_playing) {
			Ref<Image> image;
			{
				MutexLock lock(mlt_mutex);
				if (graph) {
					image = _grab(*graph->producer, graph->profile, start, size);
				}
			}
			_publish(image, start, now_generation);
			continue;
		}

		const double clock = anchor_frame + double(int64_t(os->get_ticks_usec() - anchor_time)) * now_fps * now_speed / 1000000.0;

		if (now_speed != 1.0) {
			const int frame = int(Math::floor(clock));
			if (frame < 0 || frame >= total) {
				if (now_loop) {
					anchor_frame += frame < 0 ? total : -total;
				} else {
					_finish_playback(frame < 0 ? 0 : total - 1, now_generation);
				}
				continue;
			}
			if (frame == last_shown) {
				os->delay_usec(1000);
				continue;
			}
			Ref<Image> image;
			{
				MutexLock lock(mlt_mutex);
				if (graph) {
					image = _grab(*graph->producer, graph->profile, frame, size);
				}
			}
			if (last_shown >= 0) {
				dropped_frames.add(MAX(0, Math::abs(frame - last_shown) - 1));
			}
			last_shown = frame;
			_publish(image, frame, now_generation);
			continue;
		}

		Ref<Image> due_image;
		int due_frame = -1;
		while (!queue.is_empty() && double(queue[0].virtual_frame) <= clock) {
			due_image = queue[0].image;
			due_frame = queue[0].frame;
			queue.remove_at(0);
		}
		if (due_frame >= 0) {
			_publish(due_image, due_frame, now_generation);
		}
		if (!now_loop && next_decode >= total) {
			if (queue.is_empty()) {
				_finish_playback(total - 1, now_generation);
			} else {
				os->delay_usec(1000);
			}
			continue;
		}

		const int ahead = MAX(2, int(Math::ceil(now_fps * 0.15)));
		if (double(next_decode) - clock >= ahead || int(queue.size()) > ahead) {
			os->delay_usec(1000);
			continue;
		}
		int64_t behind = int64_t(Math::floor(clock)) - next_decode;
		if (behind > ahead) {
			if (!now_loop) {
				behind = MIN(behind, int64_t(total - 1) - next_decode);
			}
			dropped_frames.add(behind);
			next_decode += behind;
			behind = 0;
		}
		const bool late = behind > 0;
		const int frame = int(next_decode % total);
		Ref<Image> image;
		{
			MutexLock lock(mlt_mutex);
			if (graph) {
				if (graph->producer->position() != frame) {
					graph->producer->seek(frame);
				}
				Mlt::Frame *decoded = graph->producer->get_frame();
				if (decoded && audio.is_valid()) {
					mlt_audio_format format = mlt_audio_f32le;
					int frequency = int(AudioServer::get_singleton()->get_mix_rate());
					int channels = 2;
					int samples = mlt_audio_calculate_frame_samples(float(now_fps), frequency, next_decode);
					const float *data = (const float *)decoded->get_audio(format, frequency, channels, samples);
					if (data && format == mlt_audio_f32le && samples > 0 && channels > 0) {
						audio_buffer.resize(samples);
						Vector2 *out = audio_buffer.ptrw();
						for (int i = 0; i < samples; i++) {
							const float left = data[i * channels];
							const float right = channels > 1 ? data[i * channels + 1] : left;
							out[i] = Vector2(left, right) * now_volume;
						}
						// The ring ran dry (or playback just started): what is pushed now is heard after
						// the output latency, and that is when this frame's picture is due.
						if (audio->get_skips() != last_skips) {
							anchor_frame = double(next_decode);
							anchor_time = os->get_ticks_usec() + uint64_t(AudioServer::get_singleton()->get_output_latency() * 1000000.0);
						}
						if (audio->can_push_buffer(samples) && audio->push_buffer(audio_buffer)) {
							audio_frames_pushed.add(samples);
						}
						last_skips = audio->get_skips();
					}
				}
				if (decoded && !late) {
					image = _frame_image(*decoded, graph->profile, size);
				}
				delete decoded;
			}
		}
		if (late) {
			dropped_frames.increment();
		} else {
			Queued queued;
			queued.virtual_frame = next_decode;
			queued.frame = frame;
			queued.image = image;
			queue.push_back(queued);
		}
		next_decode++;
	}
}

#else // TETRA_MLT_ENABLED

struct TetraMltEngine::Graph {};

bool TetraMltEngine::is_available() {
	return false;
}

String TetraMltEngine::get_version() {
	return String();
}

PackedStringArray TetraMltEngine::list_services(const String &p_kind) {
	return PackedStringArray();
}

bool TetraMltEngine::has_service(const String &p_kind, const String &p_name) {
	return false;
}

Dictionary TetraMltEngine::get_service_operation(const String &p_kind, const String &p_name) {
	return Dictionary();
}

Dictionary TetraMltEngine::probe(const String &p_path, const Ref<TetraVideoProject> &p_project) {
	return Dictionary();
}

PackedFloat32Array TetraMltEngine::get_audio_peaks(const String &p_path, int p_samples_per_second) {
	return PackedFloat32Array();
}

Error TetraMltEngine::set_xml(const String &p_xml) {
	return ERR_UNAVAILABLE;
}

bool TetraMltEngine::has_graph() const {
	return false;
}

Ref<Image> TetraMltEngine::render_frame(int p_frame, const Vector2i &p_max_size) {
	return Ref<Image>();
}

Ref<Image> TetraMltEngine::get_thumbnail(const Variant &p_source, int p_frame, const Vector2i &p_size) {
	return Ref<Image>();
}

void TetraMltEngine::_drop_graph() {
}

void TetraMltEngine::_drop_thumbnails() {
}

void TetraMltEngine::_run_playback() {
}

#endif // TETRA_MLT_ENABLED

String TetraMltEngine::get_preferred_service(const String &p_kind, const String &p_candidates) {
	const PackedStringArray candidates = p_candidates.split(",", false);
	for (const String &candidate : candidates) {
		if (has_service(p_kind, candidate)) {
			return candidate;
		}
	}
	return candidates.is_empty() ? String() : candidates[0];
}

int TetraMltEngine::import_media(const Ref<TetraVideoProject> &p_project, const String &p_path) {
	ERR_FAIL_COND_V(p_project.is_null(), -1);
	const Dictionary info = probe(p_path, p_project);
	if (info.is_empty() || int(info["length"]) < 1) {
		return -1;
	}
	return p_project->add_bin_clip(TetraVideoProject::ClipKind(int(info["kind"])), p_path.get_file(), p_path, info["length"], info["has_video"], info["has_audio"]);
}

Error TetraMltEngine::set_project(const Ref<TetraVideoProject> &p_project) {
	ERR_FAIL_COND_V(p_project.is_null(), ERR_INVALID_PARAMETER);
	if (!is_available()) {
		return ERR_UNAVAILABLE;
	}
	if (profile_width != p_project->get_width() || profile_height != p_project->get_height() || profile_fps_num != p_project->get_fps_num() || profile_fps_den != p_project->get_fps_den()) {
		// Thumbnail sources count frames in the old profile.
		_drop_thumbnails();
		profile_width = p_project->get_width();
		profile_height = p_project->get_height();
		profile_fps_num = p_project->get_fps_num();
		profile_fps_den = p_project->get_fps_den();
	}
	return set_xml(p_project->to_mlt_xml());
}

int TetraMltEngine::get_length() const {
	MutexLock lock(control_mutex);
	return length;
}

void TetraMltEngine::_thread_main(void *p_self) {
	static_cast<TetraMltEngine *>(p_self)->_run_playback();
}

void TetraMltEngine::_ensure_thread() {
	MutexLock lock(control_mutex);
	if (!thread.is_started() && length > 0) {
		exit_requested.clear();
		thread.start(_thread_main, this);
	}
}

// The sound goes to the AudioServer through a generator stream that the
// engine owns, so no AudioStreamPlayer node is needed. Every start gets a
// fresh playback, and with it an empty ring buffer: sound queued before a
// pause or a seek must not be heard afterwards.
void TetraMltEngine::_start_audio() {
	_stop_audio();
	MutexLock lock(control_mutex);
	if (!AudioServer::get_singleton()) {
		return;
	}
	if (audio_generator.is_null()) {
		audio_generator.instantiate();
		audio_generator->set_mix_rate_mode(AudioStreamGenerator::MIX_RATE_OUTPUT);
		audio_generator->set_buffer_length(1.0);
	}
	audio_playback = audio_generator->instantiate_playback();
	Vector<AudioFrame> volumes;
	volumes.resize(AudioServer::MAX_CHANNELS_PER_BUS);
	for (int i = 0; i < volumes.size(); i++) {
		volumes.write[i] = i == 0 ? AudioFrame(1, 1) : AudioFrame(0, 0);
	}
	AudioServer::get_singleton()->start_playback_stream(audio_playback, audio_bus, volumes);
}

void TetraMltEngine::_stop_audio() {
	MutexLock lock(control_mutex);
	if (audio_playback.is_valid() && AudioServer::get_singleton()) {
		AudioServer::get_singleton()->stop_playback_stream(audio_playback);
	}
	audio_playback.unref();
}

void TetraMltEngine::_publish(const Ref<Image> &p_image, int p_frame, uint64_t p_generation) {
	{
		MutexLock lock(control_mutex);
		if (generation != p_generation) {
			return;
		}
		current_image = p_image;
		current_frame = p_frame;
		position = p_frame;
	}
	// One pending notification is enough: the handler reads the latest frame.
	if (!frame_signal_pending.is_set()) {
		frame_signal_pending.set();
		call_deferred(SNAME("_emit_frame_ready"));
	}
}

void TetraMltEngine::_finish_playback(int p_frame, uint64_t p_generation) {
	{
		MutexLock lock(control_mutex);
		if (generation != p_generation) {
			return;
		}
		playing = false;
		position = p_frame;
		generation++;
		refresh_requested = true;
	}
	call_deferred(SNAME("_emit_playback_finished"));
}

void TetraMltEngine::_emit_frame_ready() {
	frame_signal_pending.clear();
	emit_signal(SNAME("frame_ready"), get_current_frame());
}

void TetraMltEngine::_emit_playback_finished() {
	emit_signal(SNAME("playback_finished"));
}

void TetraMltEngine::play() {
	bool with_sound = false;
	{
		MutexLock lock(control_mutex);
		if (length < 1 || playing) {
			return;
		}
		// Play at the end starts over, like every player.
		if (!loop && speed > 0 && position >= length - 1) {
			position = 0;
		} else if (!loop && speed < 0 && position <= 0) {
			position = length - 1;
		}
		playing = true;
		generation++;
		with_sound = speed == 1.0;
	}
	if (with_sound) {
		_start_audio();
	}
	_ensure_thread();
}

void TetraMltEngine::pause() {
	{
		MutexLock lock(control_mutex);
		if (playing) {
			playing = false;
			generation++;
		}
	}
	_stop_audio();
}

void TetraMltEngine::seek(int p_frame) {
	bool with_sound = false;
	{
		MutexLock lock(control_mutex);
		position = CLAMP(p_frame, 0, MAX(0, length - 1));
		generation++;
		refresh_requested = true;
		with_sound = playing && speed == 1.0;
	}
	if (with_sound) {
		_start_audio();
	} else {
		_stop_audio();
	}
	_ensure_thread();
}

void TetraMltEngine::set_speed(double p_speed) {
	if (p_speed == 0.0) {
		pause();
		return;
	}
	bool with_sound = false;
	{
		MutexLock lock(control_mutex);
		if (speed == p_speed) {
			return;
		}
		speed = p_speed;
		generation++;
		with_sound = playing && speed == 1.0;
	}
	if (with_sound) {
		_start_audio();
	} else {
		_stop_audio();
	}
}

double TetraMltEngine::get_speed() const {
	MutexLock lock(control_mutex);
	return speed;
}

int TetraMltEngine::get_position() const {
	MutexLock lock(control_mutex);
	return position;
}

bool TetraMltEngine::is_playing() const {
	MutexLock lock(control_mutex);
	return playing;
}

void TetraMltEngine::set_loop(bool p_loop) {
	MutexLock lock(control_mutex);
	loop = p_loop;
}

bool TetraMltEngine::get_loop() const {
	MutexLock lock(control_mutex);
	return loop;
}

void TetraMltEngine::set_preview_size(const Vector2i &p_size) {
	MutexLock lock(control_mutex);
	if (preview_size != p_size) {
		preview_size = p_size;
		refresh_requested = thread.is_started() && !playing;
	}
}

Vector2i TetraMltEngine::get_preview_size() const {
	MutexLock lock(control_mutex);
	return preview_size;
}

void TetraMltEngine::set_volume(double p_volume) {
	MutexLock lock(control_mutex);
	volume = MAX(0.0, p_volume);
}

double TetraMltEngine::get_volume() const {
	MutexLock lock(control_mutex);
	return volume;
}

void TetraMltEngine::set_audio_bus(const StringName &p_bus) {
	MutexLock lock(control_mutex);
	audio_bus = p_bus;
}

StringName TetraMltEngine::get_audio_bus() const {
	MutexLock lock(control_mutex);
	return audio_bus;
}

Ref<Image> TetraMltEngine::get_current_frame_image() const {
	MutexLock lock(control_mutex);
	return current_image;
}

int TetraMltEngine::get_current_frame() const {
	MutexLock lock(control_mutex);
	return current_frame;
}

int64_t TetraMltEngine::get_audio_frames_pushed() const {
	return audio_frames_pushed.get();
}

int64_t TetraMltEngine::get_dropped_frames() const {
	return dropped_frames.get();
}

TetraMltEngine::TetraMltEngine() {
}

TetraMltEngine::~TetraMltEngine() {
	exit_requested.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	Ref<AudioStreamGeneratorPlayback> last_playback = audio_playback;
	_stop_audio();
	// The playback points at its generator; the generator may only go once the mixer has let go of the playback.
	for (int i = 0; i < 200 && last_playback.is_valid() && AudioServer::get_singleton() && AudioServer::get_singleton()->is_playback_active(last_playback); i++) {
		OS::get_singleton()->delay_usec(1000);
	}
	last_playback.unref();
	_drop_graph();
	_drop_thumbnails();
}

void TetraMltEngine::_bind_methods() {
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("is_available"), &TetraMltEngine::is_available);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_version"), &TetraMltEngine::get_version);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("list_services", "kind"), &TetraMltEngine::list_services);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("has_service", "kind", "name"), &TetraMltEngine::has_service);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_preferred_service", "kind", "candidates"), &TetraMltEngine::get_preferred_service);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_service_operation", "kind", "name"), &TetraMltEngine::get_service_operation);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("probe", "path", "project"), &TetraMltEngine::probe, DEFVAL(Ref<TetraVideoProject>()));
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("import_media", "project", "path"), &TetraMltEngine::import_media);
	ClassDB::bind_static_method("TetraMltEngine", D_METHOD("get_audio_peaks", "path", "samples_per_second"), &TetraMltEngine::get_audio_peaks, DEFVAL(50));

	ClassDB::bind_method(D_METHOD("set_project", "project"), &TetraMltEngine::set_project);
	ClassDB::bind_method(D_METHOD("set_xml", "xml"), &TetraMltEngine::set_xml);
	ClassDB::bind_method(D_METHOD("has_graph"), &TetraMltEngine::has_graph);
	ClassDB::bind_method(D_METHOD("get_length"), &TetraMltEngine::get_length);
	ClassDB::bind_method(D_METHOD("render_frame", "frame", "max_size"), &TetraMltEngine::render_frame, DEFVAL(Vector2i()));
	ClassDB::bind_method(D_METHOD("get_thumbnail", "source", "frame", "size"), &TetraMltEngine::get_thumbnail);

	ClassDB::bind_method(D_METHOD("play"), &TetraMltEngine::play);
	ClassDB::bind_method(D_METHOD("pause"), &TetraMltEngine::pause);
	ClassDB::bind_method(D_METHOD("seek", "frame"), &TetraMltEngine::seek);
	ClassDB::bind_method(D_METHOD("set_speed", "speed"), &TetraMltEngine::set_speed);
	ClassDB::bind_method(D_METHOD("get_speed"), &TetraMltEngine::get_speed);
	ClassDB::bind_method(D_METHOD("get_position"), &TetraMltEngine::get_position);
	ClassDB::bind_method(D_METHOD("is_playing"), &TetraMltEngine::is_playing);
	ClassDB::bind_method(D_METHOD("set_loop", "loop"), &TetraMltEngine::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &TetraMltEngine::get_loop);
	ClassDB::bind_method(D_METHOD("set_preview_size", "size"), &TetraMltEngine::set_preview_size);
	ClassDB::bind_method(D_METHOD("get_preview_size"), &TetraMltEngine::get_preview_size);
	ClassDB::bind_method(D_METHOD("set_volume", "volume"), &TetraMltEngine::set_volume);
	ClassDB::bind_method(D_METHOD("get_volume"), &TetraMltEngine::get_volume);
	ClassDB::bind_method(D_METHOD("set_audio_bus", "bus"), &TetraMltEngine::set_audio_bus);
	ClassDB::bind_method(D_METHOD("get_audio_bus"), &TetraMltEngine::get_audio_bus);
	ClassDB::bind_method(D_METHOD("get_current_frame_image"), &TetraMltEngine::get_current_frame_image);
	ClassDB::bind_method(D_METHOD("get_current_frame"), &TetraMltEngine::get_current_frame);
	ClassDB::bind_method(D_METHOD("get_audio_frames_pushed"), &TetraMltEngine::get_audio_frames_pushed);
	ClassDB::bind_method(D_METHOD("get_dropped_frames"), &TetraMltEngine::get_dropped_frames);
	ClassDB::bind_method(D_METHOD("_emit_frame_ready"), &TetraMltEngine::_emit_frame_ready);
	ClassDB::bind_method(D_METHOD("_emit_playback_finished"), &TetraMltEngine::_emit_playback_finished);

	ADD_SIGNAL(MethodInfo("frame_ready", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("playback_finished"));
}
