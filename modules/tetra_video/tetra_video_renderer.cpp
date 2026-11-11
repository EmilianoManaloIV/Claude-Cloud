#include "tetra_video_renderer.h"

#include "tetra_video_assets.h"
#include "tetra_video_project.h"
#include "tetra_video_xml.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

namespace {

Dictionary _make_preset(const String &p_id, const String &p_group, const String &p_name, const String &p_extension, const String &p_args, const String &p_type) {
	Dictionary preset;
	preset["id"] = p_id;
	preset["group"] = p_group;
	preset["name"] = p_name;
	preset["extension"] = p_extension;
	preset["args"] = p_args;
	preset["type"] = p_type;
	preset["builtin"] = false;
	preset["speeds"] = PackedStringArray();
	preset["default_speed_index"] = 0;
	return preset;
}

// Presets that do not depend on Kdenlive's data. The Theora one produces
// files Godot's own VideoStreamPlayer plays.
void _add_builtin_presets(TypedArray<Dictionary> &r_presets) {
	Dictionary mp4 = _make_preset("tetra/mp4", "Tetra Studio", "MP4 (H.264/AAC)", "mp4", "f=mp4 movflags=+faststart vcodec=libx264 preset=veryfast crf=%quality g=15 acodec=aac ab=%audiobitrate+'k'", "av");
	mp4["default_quality"] = 23;
	mp4["default_audio_bitrate"] = 160;
	Dictionary webm = _make_preset("tetra/webm", "Tetra Studio", "WebM (VP9/Opus)", "webm", "f=webm vcodec=libvpx-vp9 crf=%quality vb=0 g=15 row-mt=1 acodec=libopus ab=%audiobitrate+'k'", "av");
	webm["default_quality"] = 32;
	webm["default_audio_bitrate"] = 128;
	Dictionary ogv = _make_preset("tetra/ogv", "Tetra Studio", "Ogg (Theora/Vorbis), plays in Godot", "ogv", "f=ogg vcodec=libtheora qscale=%quality g=15 acodec=libvorbis aq=%audioquality", "av");
	ogv["default_quality"] = 7;
	ogv["default_audio_quality"] = 5;
	for (Dictionary preset : { mp4, webm, ogv }) {
		preset["builtin"] = true;
		r_presets.push_back(preset);
	}
}

// Kdenlive's profiles.xml: <group id><name/><profile name extension args .../></group>.
void _add_kdenlive_presets(TypedArray<Dictionary> &r_presets) {
	const String data_dir = TetraVideoAssets::get_data_dir();
	String path;
	for (const String &candidate : { data_dir.path_join("profiles.xml"), data_dir.path_join("export/profiles.xml") }) {
		if (!data_dir.is_empty() && path.is_empty() && FileAccess::exists(candidate)) {
			path = candidate;
		}
	}
	Vector<TetraXmlNode> nodes;
	String message;
	if (path.is_empty() || tetra_parse_xml(FileAccess::get_file_as_string(path), nodes, message) != OK) {
		return;
	}
	const int root = tetra_xml_find_child(nodes, 0, "profiles");
	if (root < 0) {
		return;
	}
	for (int group : nodes[root].children) {
		if (nodes[group].name != "group") {
			continue;
		}
		const int name_node = tetra_xml_find_child(nodes, group, "name");
		const String group_id = nodes[group].get_attribute("id");
		const String group_name = name_node < 0 ? group_id : nodes[name_node].text.strip_edges();
		for (int child : nodes[group].children) {
			const TetraXmlNode &node = nodes[child];
			if (node.name != "profile" || node.get_attribute("name").is_empty()) {
				continue;
			}
			Dictionary preset = _make_preset(group_id + "/" + node.get_attribute("name"), group_name, node.get_attribute("name"),
					node.get_attribute("extension").to_lower(), node.get_attribute("args"), nodes[group].get_attribute("type", "av"));
			if (node.attributes.has("defaultquality")) {
				preset["default_quality"] = node.get_attribute("defaultquality").to_int();
			}
			if (node.attributes.has("defaultbitrate")) {
				preset["default_bitrate"] = node.get_attribute("defaultbitrate").to_int();
			}
			if (node.attributes.has("defaultaudiobitrate")) {
				preset["default_audio_bitrate"] = node.get_attribute("defaultaudiobitrate").to_int();
			}
			if (node.attributes.has("defaultaudioquality")) {
				preset["default_audio_quality"] = node.get_attribute("defaultaudioquality").to_int();
			}
			if (node.attributes.has("speeds")) {
				preset["speeds"] = node.get_attribute("speeds").split(";");
				preset["default_speed_index"] = node.get_attribute("defaultspeedindex", "0").to_int();
			}
			r_presets.push_back(preset);
		}
	}
}

} // namespace

TypedArray<Dictionary> TetraVideoRenderer::get_presets() {
	TypedArray<Dictionary> presets;
	_add_builtin_presets(presets);
	_add_kdenlive_presets(presets);
	return presets;
}

Dictionary TetraVideoRenderer::get_preset(const String &p_id) {
	const TypedArray<Dictionary> presets = get_presets();
	for (int i = 0; i < presets.size(); i++) {
		const Dictionary preset = presets[i];
		if (String(preset["id"]) == p_id) {
			return preset;
		}
	}
	return Dictionary();
}

// TETRA_MELT names the executable explicitly; otherwise PATH is searched for melt-7 and melt.
String TetraVideoRenderer::find_melt() {
	const String configured = OS::get_singleton()->get_environment("TETRA_MELT");
	if (!configured.is_empty() && FileAccess::exists(configured)) {
		return configured;
	}
	for (const String &dir : OS::get_singleton()->get_environment("PATH").split(":", false)) {
		for (const char *name : { "melt-7", "melt" }) {
			if (FileAccess::exists(dir.path_join(name))) {
				return dir.path_join(name);
			}
		}
	}
	return String();
}

// Fills the placeholders of a preset's argument string the way Kdenlive
// does and splits it into melt consumer properties.
PackedStringArray TetraVideoRenderer::expand_preset_arguments(const Dictionary &p_preset, const Dictionary &p_options) {
	String args = p_preset.get("args", String());
	const int quality = p_options.get("quality", p_options.get("crf", p_preset.get("default_quality", 23)));
	const int audio_quality = p_options.get("audio_quality", p_preset.get("default_audio_quality", 5));
	const int audio_bitrate = p_options.get("audio_bitrate", p_preset.get("default_audio_bitrate", 160));
	const int bitrate = p_options.get("bitrate", p_preset.get("default_bitrate", 6000));
	args = args.replace("%audiobitrate+'k'", itos(audio_bitrate) + "k")
				   .replace("%bitrate+'k'", itos(bitrate) + "k")
				   .replace("%audiobitrate", itos(audio_bitrate))
				   .replace("%bitrate", itos(bitrate))
				   .replace("%audioquality", itos(audio_quality))
				   .replace("%quality", itos(quality));
	const PackedStringArray speeds = p_preset.get("speeds", PackedStringArray());
	if (!speeds.is_empty()) {
		const int index = p_options.get("speed_index", p_preset.get("default_speed_index", 0));
		args += " " + speeds[CLAMP(index, 0, speeds.size() - 1)];
	}
	args += " " + String(p_options.get("extra_args", String()));
	PackedStringArray result;
	for (const String &argument : args.split(" ", false)) {
		// A placeholder nobody filled would be passed to the encoder as a literal.
		if (!argument.contains_char('%')) {
			result.push_back(argument);
		}
	}
	const int threads = p_options.get("threads", 0);
	if (threads > 0) {
		result.push_back("threads=" + itos(threads));
	}
	// A negative real_time lets MLT use that many processing threads without ever dropping a frame.
	result.push_back("real_time=-" + itos(MAX(1, threads)));
	return result;
}

Error TetraVideoRenderer::start(const Ref<TetraVideoProject> &p_project, const String &p_output_path, const String &p_preset_id, const Dictionary &p_options) {
	ERR_FAIL_COND_V(p_project.is_null(), ERR_INVALID_PARAMETER);
	if (running.is_set()) {
		last_error = "A render is already running.";
		return ERR_BUSY;
	}
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	const Dictionary preset = get_preset(p_preset_id);
	if (preset.is_empty()) {
		last_error = vformat("Unknown render preset \"%s\".", p_preset_id);
		return ERR_DOES_NOT_EXIST;
	}
	const String melt = find_melt();
	if (melt.is_empty()) {
		last_error = "The melt executable was not found. Install MLT's melt or set TETRA_MELT.";
		return ERR_FILE_NOT_FOUND;
	}
	const int duration = p_project->get_duration();
	if (duration < 1) {
		last_error = "The timeline is empty.";
		return ERR_INVALID_DATA;
	}
	const int in = CLAMP(int(p_options.get("in", 0)), 0, duration - 1);
	const int out = CLAMP(int(p_options.get("out", duration - 1)), in, duration - 1);

	String output = p_output_path;
	if (output.begins_with("res://") || output.begins_with("user://")) {
		output = ProjectSettings::get_singleton()->globalize_path(output);
	}
	if (DirAccess::make_dir_recursive_absolute(output.get_base_dir()) != OK) {
		last_error = vformat("Cannot create the folder of \"%s\".", output);
		return ERR_CANT_CREATE;
	}

	Dictionary xml_options;
	xml_options["tetra_properties"] = false;
	if (p_options.has("width") && p_options.has("height")) {
		xml_options["width"] = p_options["width"];
		xml_options["height"] = p_options["height"];
	}
	const String scratch = OS::get_singleton()->get_cache_path().path_join("tetra-studio");
	DirAccess::make_dir_recursive_absolute(scratch);
	const String xml_file = scratch.path_join(vformat("render-%d-%d.mlt", OS::get_singleton()->get_process_id(), OS::get_singleton()->get_ticks_usec()));
	Ref<FileAccess> file = FileAccess::open(xml_file, FileAccess::WRITE);
	if (file.is_null()) {
		last_error = vformat("Cannot write \"%s\".", xml_file);
		return ERR_FILE_CANT_WRITE;
	}
	file->store_string(p_project->to_mlt_xml(xml_options));
	file.unref();

	List<String> arguments;
	arguments.push_back(xml_file);
	arguments.push_back("in=" + itos(in));
	arguments.push_back("out=" + itos(out));
	arguments.push_back("-progress");
	arguments.push_back("-consumer");
	arguments.push_back("avformat:" + output);
	for (const String &argument : expand_preset_arguments(preset, p_options)) {
		arguments.push_back(argument);
	}
	String line = melt;
	for (const String &argument : arguments) {
		line += " " + argument;
	}

	const Dictionary process = OS::get_singleton()->execute_with_pipe(melt, arguments, false);
	if (process.is_empty()) {
		DirAccess::remove_absolute(xml_file);
		last_error = "melt could not be started.";
		return ERR_CANT_FORK;
	}
	{
		MutexLock lock(mutex);
		process_id = process["pid"];
		process_output = process["stdio"];
		process_errors = process["stderr"];
		xml_path = xml_file;
		output_path = output;
		command_line = line;
		log_tail = String();
		last_error = String();
		first_frame = in;
		frame_count = out - in + 1;
		current_frame = in;
		reported_frame = -1;
		current_percent = 0.0;
	}
	cancel_requested.clear();
	running.set();
	thread.start(_thread_main, this);
	return OK;
}

void TetraVideoRenderer::_thread_main(void *p_self) {
	static_cast<TetraVideoRenderer *>(p_self)->_follow_process();
}

// melt -progress rewrites one line on stderr: "Current Frame:  12, percentage:  48".
void TetraVideoRenderer::_read_progress() {
	// The child's stdout is drained too, or it would block once the pipe is full.
	uint64_t available = process_output->get_length();
	if (available > 0) {
		Vector<uint8_t> discard;
		discard.resize(available);
		process_output->get_buffer(discard.ptrw(), available);
	}
	available = process_errors->get_length();
	if (available == 0) {
		return;
	}
	Vector<uint8_t> chunk;
	chunk.resize(available);
	const uint64_t got = process_errors->get_buffer(chunk.ptrw(), available);
	const String text = String::utf8((const char *)chunk.ptr(), got);
	int frame = -1;
	{
		MutexLock lock(mutex);
		log_tail = (log_tail + text).right(4000);
		const int marker = log_tail.rfind("Current Frame:");
		const int comma = marker < 0 ? -1 : log_tail.find_char(',', marker);
		if (comma > marker) {
			frame = log_tail.substr(marker + 14, comma - marker - 14).strip_edges().to_int();
			if (frame == reported_frame) {
				frame = -1;
			} else {
				reported_frame = frame;
				current_frame = frame;
				current_percent = CLAMP(double(frame - first_frame + 1) * 100.0 / frame_count, 0.0, 100.0);
			}
		}
	}
	if (frame >= 0) {
		call_deferred(SNAME("_emit_progress"), frame, get_percent());
	}
}

void TetraVideoRenderer::_follow_process() {
	OS *os = OS::get_singleton();
	bool killed = false;
	while (os->is_process_running(process_id)) {
		_read_progress();
		if (cancel_requested.is_set()) {
			// kill() also reaps the child, after which the process must not be asked about again.
			os->kill(process_id);
			killed = true;
			break;
		}
		os->delay_usec(20000);
	}
	_read_progress();
	const int exit_code = killed ? -1 : os->get_process_exit_code(process_id);
	String message;
	bool ok = false;
	int last_frame = 0;
	{
		MutexLock lock(mutex);
		DirAccess::remove_absolute(xml_path);
		process_output.unref();
		process_errors.unref();
		if (killed) {
			DirAccess::remove_absolute(output_path);
			message = "The render was cancelled.";
		} else if (exit_code != 0) {
			message = vformat("melt failed with exit code %d.", exit_code);
		} else if (!FileAccess::exists(output_path)) {
			message = "melt finished but wrote no file.";
		} else {
			ok = true;
			message = output_path;
			current_frame = first_frame + frame_count - 1;
			current_percent = 100.0;
		}
		last_error = ok ? String() : message;
		last_frame = current_frame;
	}
	running.clear();
	if (ok) {
		// A short render can end between two of melt's progress lines; the bar still has to reach the end.
		call_deferred(SNAME("_emit_progress"), last_frame, 100.0);
	}
	call_deferred(SNAME("_emit_finished"), ok, message);
}

void TetraVideoRenderer::_emit_progress(int p_frame, double p_percent) {
	emit_signal(SNAME("progress"), p_frame, p_percent);
}

void TetraVideoRenderer::_emit_finished(bool p_ok, const String &p_message) {
	emit_signal(SNAME("finished"), p_ok, p_message);
}

void TetraVideoRenderer::cancel() {
	if (running.is_set()) {
		cancel_requested.set();
	}
}

bool TetraVideoRenderer::is_running() const {
	return running.is_set();
}

int TetraVideoRenderer::get_frame() const {
	MutexLock lock(mutex);
	return current_frame;
}

double TetraVideoRenderer::get_percent() const {
	MutexLock lock(mutex);
	return current_percent;
}

String TetraVideoRenderer::get_command_line() const {
	MutexLock lock(mutex);
	return command_line;
}

String TetraVideoRenderer::get_log() const {
	MutexLock lock(mutex);
	return log_tail;
}

String TetraVideoRenderer::get_last_error() const {
	MutexLock lock(mutex);
	return last_error;
}

TetraVideoRenderer::~TetraVideoRenderer() {
	cancel();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
}

void TetraVideoRenderer::_bind_methods() {
	ClassDB::bind_static_method("TetraVideoRenderer", D_METHOD("get_presets"), &TetraVideoRenderer::get_presets);
	ClassDB::bind_static_method("TetraVideoRenderer", D_METHOD("get_preset", "id"), &TetraVideoRenderer::get_preset);
	ClassDB::bind_static_method("TetraVideoRenderer", D_METHOD("find_melt"), &TetraVideoRenderer::find_melt);
	ClassDB::bind_static_method("TetraVideoRenderer", D_METHOD("expand_preset_arguments", "preset", "options"), &TetraVideoRenderer::expand_preset_arguments);

	ClassDB::bind_method(D_METHOD("start", "project", "output_path", "preset_id", "options"), &TetraVideoRenderer::start, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("cancel"), &TetraVideoRenderer::cancel);
	ClassDB::bind_method(D_METHOD("is_running"), &TetraVideoRenderer::is_running);
	ClassDB::bind_method(D_METHOD("get_frame"), &TetraVideoRenderer::get_frame);
	ClassDB::bind_method(D_METHOD("get_percent"), &TetraVideoRenderer::get_percent);
	ClassDB::bind_method(D_METHOD("get_command_line"), &TetraVideoRenderer::get_command_line);
	ClassDB::bind_method(D_METHOD("get_log"), &TetraVideoRenderer::get_log);
	ClassDB::bind_method(D_METHOD("get_last_error"), &TetraVideoRenderer::get_last_error);
	ClassDB::bind_method(D_METHOD("_emit_progress", "frame", "percent"), &TetraVideoRenderer::_emit_progress);
	ClassDB::bind_method(D_METHOD("_emit_finished", "ok", "message"), &TetraVideoRenderer::_emit_finished);

	ADD_SIGNAL(MethodInfo("progress", PropertyInfo(Variant::INT, "frame"), PropertyInfo(Variant::FLOAT, "percent")));
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::STRING, "message")));
}
