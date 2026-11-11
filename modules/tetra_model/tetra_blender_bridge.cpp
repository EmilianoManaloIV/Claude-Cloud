#include "tetra_blender_bridge.h"

#include "bridge/tetra_blender_server_script.h"
#include "tetra_blender_mesh.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/material.h"

#ifdef TOOLS_ENABLED
#include "editor/settings/editor_settings.h"
#endif

#if __has_include("modules/tetra/tetra_engine_registry.h")
#include "modules/tetra/tetra_engine_registry.h"
#define TETRA_ENGINE_REGISTRY_AVAILABLE
#endif

namespace {

const char *SETTING_PYTHON_PATH = "tetra/blender/python_path";
const char *SETTING_EXECUTABLE_PATH = "tetra/blender/executable_path";
const char *SETTING_GODOT_BLENDER_PATH = "filesystem/import/blender/blender_path";

// The version of the last Blender any bridge talked to, for the engine registry.
Mutex known_version_mutex;
String known_version;

Dictionary make_error(const String &p_message) {
	Dictionary result;
	result["ok"] = false;
	result["error"] = p_message;
	return result;
}

bool is_ok(const Dictionary &p_result) {
	return bool(p_result.get("ok", false));
}

// A deep copy in which the Godot types JSON has no notation for become number arrays.
Variant to_json(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::FLOAT: {
			return Math::is_finite(double(p_value)) ? p_value : Variant(0.0);
		}
		case Variant::STRING_NAME: {
			return String(p_value);
		}
		case Variant::VECTOR2:
		case Variant::VECTOR2I: {
			const Vector2 v = p_value;
			Array array;
			array.push_back(v.x);
			array.push_back(v.y);
			return array;
		}
		case Variant::VECTOR3:
		case Variant::VECTOR3I: {
			const Vector3 v = p_value;
			Array array;
			array.push_back(v.x);
			array.push_back(v.y);
			array.push_back(v.z);
			return array;
		}
		case Variant::COLOR: {
			const Color c = p_value;
			Array array;
			array.push_back(c.r);
			array.push_back(c.g);
			array.push_back(c.b);
			array.push_back(c.a);
			return array;
		}
		case Variant::DICTIONARY: {
			const Dictionary source = p_value;
			Dictionary copy;
			const Array keys = source.keys();
			for (int i = 0; i < keys.size(); i++) {
				copy[String(keys[i])] = to_json(source[keys[i]]);
			}
			return copy;
		}
		case Variant::ARRAY: {
			const Array source = p_value;
			Array copy;
			copy.resize(source.size());
			for (int i = 0; i < source.size(); i++) {
				copy[i] = to_json(source[i]);
			}
			return copy;
		}
		default: {
			return p_value;
		}
	}
}

// Godot colours are sRGB-encoded, Blender's shader inputs are linear.
Array linear_color(const Variant &p_value) {
	if (p_value.get_type() == Variant::ARRAY) {
		return p_value;
	}
	const Color encoded = p_value;
	const Color linear = encoded.srgb_to_linear();
	Array array;
	array.push_back(linear.r);
	array.push_back(linear.g);
	array.push_back(linear.b);
	array.push_back(linear.a);
	return array;
}

Dictionary material_to_json(const Dictionary &p_material) {
	Dictionary json = to_json(p_material);
	for (const char *key : { "base_color", "emission" }) {
		if (p_material.has(key)) {
			json[key] = linear_color(p_material[key]);
		}
	}
	return json;
}

Dictionary material_from_resource(const Ref<Material> &p_material) {
	Dictionary json;
	const Ref<BaseMaterial3D> material = p_material;
	if (material.is_null()) {
		return json;
	}
	json["base_color"] = linear_color(material->get_albedo());
	json["metallic"] = material->get_metallic();
	json["roughness"] = material->get_roughness();
	if (material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
		json["emission"] = linear_color(material->get_emission());
		json["emission_strength"] = material->get_emission_energy_multiplier();
	}
	return json;
}

Ref<StandardMaterial3D> material_from_description(const Dictionary &p_description) {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_name(String(p_description.get("name", String())));
	const Array base = p_description.get("base_color", Array());
	if (base.size() >= 3) {
		material->set_albedo(Color(float(base[0]), float(base[1]), float(base[2]), base.size() > 3 ? float(base[3]) : 1.0f).linear_to_srgb());
	}
	material->set_metallic(float(p_description.get("metallic", 0.0)));
	material->set_roughness(float(p_description.get("roughness", 0.5)));
	const Array emission = p_description.get("emission", Array());
	if (emission.size() >= 3) {
		const Color color = Color(float(emission[0]), float(emission[1]), float(emission[2]));
		const float strength = MAX(color.r, MAX(color.g, color.b));
		if (strength > 0.0f) {
			// Blender's emission is unbounded; keep the hue in the colour and the rest in the energy.
			const float energy = MAX(strength, 1.0f);
			material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
			material->set_emission(Color(color.r / energy, color.g / energy, color.b / energy).linear_to_srgb());
			material->set_emission_energy_multiplier(energy);
		}
	}
	return material;
}

// JSON has one number type and no vectors: give schema defaults the Variant
// types the operation schema asks for.
Array convert_schemas(const Variant &p_schemas) {
	if (p_schemas.get_type() != Variant::ARRAY) {
		return Array();
	}
	Array schemas = p_schemas;
	for (int i = 0; i < schemas.size(); i++) {
		if (schemas[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary schema = schemas[i];
		const Array params = schema.get("params", Array());
		for (int j = 0; j < params.size(); j++) {
			if (params[j].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary param = params[j];
			const String type = param.get("type", String());
			const Variant value = param.get("default", Variant());
			if (type == "int" || type == "seed") {
				param["default"] = int64_t(value);
			} else if (value.get_type() == Variant::ARRAY) {
				const Array numbers = value;
				if (type == "vec2" && numbers.size() >= 2) {
					param["default"] = Vector2(float(numbers[0]), float(numbers[1]));
				} else if (type == "vec3" && numbers.size() >= 3) {
					param["default"] = Vector3(float(numbers[0]), float(numbers[1]), float(numbers[2]));
				} else if (type == "color" && numbers.size() >= 3) {
					param["default"] = Color(float(numbers[0]), float(numbers[1]), float(numbers[2]), numbers.size() > 3 ? float(numbers[3]) : 1.0f);
				}
			}
		}
	}
	return schemas;
}

void integerize(Dictionary &r_result, const char *p_key) {
	if (r_result.has(p_key) && r_result[p_key].get_type() == Variant::FLOAT) {
		r_result[p_key] = int64_t(r_result[p_key]);
	}
}

String editor_setting(const String &p_name) {
#ifdef TOOLS_ENABLED
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting(p_name)) {
		return String(EditorSettings::get_singleton()->get_setting(p_name)).strip_edges();
	}
#endif
	return String();
}

String project_setting(const String &p_name) {
	if (ProjectSettings::get_singleton() && ProjectSettings::get_singleton()->has_setting(p_name)) {
		return String(ProjectSettings::get_singleton()->get_setting_with_override(p_name)).strip_edges();
	}
	return String();
}

String cache_dir() {
	return OS::get_singleton()->get_cache_path().path_join("tetra").path_join("blender");
}

// The embedded server script as a file Blender can run. Empty on failure.
String ensure_server_script() {
	const String override_path = OS::get_singleton()->get_environment("TETRA_BLENDER_SERVER_SCRIPT");
	if (!override_path.is_empty() && FileAccess::exists(override_path)) {
		return override_path;
	}
	const String dir = cache_dir();
	DirAccess::make_dir_recursive_absolute(dir);
	const String path = dir.path_join("tetra_blender_server.py");
	const String script = String::utf8(tetra_blender_server_script());
	if (FileAccess::exists(path) && FileAccess::get_file_as_string(path) == script) {
		return path;
	}
	// Written under another name first so that a second editor starting at
	// the same moment never runs a half-written script.
	const String partial = path + vformat(".%d.tmp", OS::get_singleton()->get_process_id());
	Ref<FileAccess> file = FileAccess::open(partial, FileAccess::WRITE);
	if (file.is_null()) {
		return String();
	}
	file->store_string(script);
	file->close();
	if (DirAccess::rename_absolute(partial, path) != OK) {
		DirAccess::remove_absolute(partial);
		return String();
	}
	return path;
}

void remove_dir_recursive(const String &p_dir) {
	if (!p_dir.contains("tetra") || !DirAccess::dir_exists_absolute(p_dir)) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_valid()) {
		dir->erase_contents_recursive();
	}
	dir.unref();
	DirAccess::remove_absolute(p_dir);
}

} // namespace

Ref<TetraBlenderBridge> TetraBlenderBridge::shared;

Ref<TetraBlenderBridge> TetraBlenderBridge::get_shared() {
	if (shared.is_null()) {
		shared.instantiate();
	}
	return shared;
}

void TetraBlenderBridge::free_shared() {
	shared.unref();
}

Dictionary TetraBlenderBridge::find_launcher() {
	Dictionary launcher;
	auto accept = [&launcher](const String &p_kind, const String &p_path, const String &p_origin) -> bool {
		if (p_path.is_empty()) {
			return false;
		}
		String path = p_path;
		if (path.begins_with("res://") || path.begins_with("user://")) {
			path = ProjectSettings::get_singleton()->globalize_path(path);
		}
		if (!FileAccess::exists(path)) {
			return false;
		}
		launcher["kind"] = p_kind;
		launcher["path"] = path;
		launcher["description"] = p_kind == "python" ? vformat("bpy module of %s (%s)", path, p_origin) : vformat("Blender executable %s (%s)", path, p_origin);
		return true;
	};

	if (accept("python", editor_setting(SETTING_PYTHON_PATH), "editor setting tetra/blender/python_path")) {
		return launcher;
	}
	if (accept("python", project_setting(SETTING_PYTHON_PATH), "project setting tetra/blender/python_path")) {
		return launcher;
	}
	if (accept("python", OS::get_singleton()->get_environment("TETRA_BLENDER_PYTHON"), "environment variable TETRA_BLENDER_PYTHON")) {
		return launcher;
	}
	if (accept("blender", editor_setting(SETTING_EXECUTABLE_PATH), "editor setting tetra/blender/executable_path")) {
		return launcher;
	}
	if (accept("blender", editor_setting(SETTING_GODOT_BLENDER_PATH), "editor setting filesystem/import/blender/blender_path")) {
		return launcher;
	}
#ifdef WINDOWS_ENABLED
	const String separator = ";";
	const String executable = "blender.exe";
#else
	const String separator = ":";
	const String executable = "blender";
#endif
	const PackedStringArray search_path = OS::get_singleton()->get_environment("PATH").split(separator, false);
	for (const String &dir : search_path) {
		if (accept("blender", dir.path_join(executable), "found on PATH")) {
			return launcher;
		}
	}
	launcher["kind"] = "";
	launcher["path"] = "";
	launcher["description"] = TTR("No Blender found. Set the editor setting tetra/blender/python_path to a Python that has the bpy module (scripts/setup-blender-bridge.sh creates one), or tetra/blender/executable_path to a Blender executable.");
	return launcher;
}

bool TetraBlenderBridge::is_available() {
	return !String(find_launcher()["kind"]).is_empty();
}

void TetraBlenderBridge::update_engine_registry() {
#ifdef TETRA_ENGINE_REGISTRY_AVAILABLE
	const Dictionary launcher = find_launcher();
	String version;
	{
		MutexLock lock(known_version_mutex);
		version = known_version;
	}
	TetraEngineRegistry::add_engine("Blender", version, !String(launcher["kind"]).is_empty(), launcher["description"]);
#endif
}

void TetraBlenderBridge::_reader_thread_func(void *p_self) {
	static_cast<TetraBlenderBridge *>(p_self)->_reader_loop();
}

void TetraBlenderBridge::_reader_loop() {
	Vector<uint8_t> out_buffer;
	Vector<uint8_t> err_buffer;
	uint64_t last_activity = OS::get_singleton()->get_ticks_usec();
	while (!reader_exit.is_set()) {
		bool active = _drain(child_stdio, out_buffer, true);
		active = _drain(child_stderr, err_buffer, false) || active;
		if (active) {
			last_activity = OS::get_singleton()->get_ticks_usec();
			continue;
		}
		bool exited = false;
		{
			// Not while _kill_process() is reaping the child: asking about a
			// process that was already waited for is an error.
			MutexLock lock(kill_mutex);
			exited = process_reaped.is_set() || !OS::get_singleton()->is_process_running(child_pid.get());
			if (exited) {
				process_reaped.set();
			}
		}
		if (exited) {
			// Whatever the process wrote before it ended is still in the pipes.
			while (_drain(child_stdio, out_buffer, true) || _drain(child_stderr, err_buffer, false)) {
			}
			process_alive.clear();
			MutexLock lock(state_mutex);
			pending_stopped = true;
			_schedule_flush();
			break;
		}
		// The pipes are polled: often while an answer is awaited, rarely once
		// Blender has been quiet for a while.
		if (request_in_flight.is_set()) {
			OS::get_singleton()->delay_usec(200);
		} else {
			OS::get_singleton()->delay_usec(OS::get_singleton()->get_ticks_usec() - last_activity < 1000000 ? 2000 : 20000);
		}
	}
}

bool TetraBlenderBridge::_drain(const Ref<FileAccess> &p_pipe, Vector<uint8_t> &r_buffer, bool p_protocol) {
	uint64_t available = p_pipe->get_length();
	if (available == 0) {
		return false;
	}
	available = MIN(available, uint64_t(65536));
	const int64_t start = r_buffer.size();
	r_buffer.resize(start + int64_t(available));
	const uint64_t received = p_pipe->get_buffer(r_buffer.ptrw() + start, available);
	r_buffer.resize(start + int64_t(received));
	if (received == 0) {
		return false;
	}

	int64_t line_start = 0;
	const uint8_t *bytes = r_buffer.ptr();
	const int64_t size = r_buffer.size();
	for (int64_t i = start; i < size; i++) {
		if (bytes[i] != '\n') {
			continue;
		}
		const String line = String::utf8((const char *)bytes + line_start, int(i - line_start)).strip_edges();
		line_start = i + 1;
		if (line.is_empty()) {
			continue;
		}
		if (p_protocol && line.begins_with("{")) {
			_handle_protocol_line(line);
		} else {
			_handle_log_line(line);
		}
	}
	if (line_start > 0) {
		const int64_t rest = size - line_start;
		if (rest > 0) {
			memmove(r_buffer.ptrw(), r_buffer.ptr() + line_start, rest);
		}
		r_buffer.resize(rest);
	}
	return true;
}

void TetraBlenderBridge::_handle_protocol_line(const String &p_line) {
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(p_line) != OK || json->get_data().get_type() != Variant::DICTIONARY) {
		_handle_log_line(p_line);
		return;
	}
	const Dictionary message = json->get_data();
	if (message.has("event")) {
		if (String(message["event"]) == "ready") {
			server_ready.set();
			return;
		}
		MutexLock lock(state_mutex);
		// The main thread may be blocked in a long request and not flushing.
		if (pending_events.size() >= 2000) {
			pending_events.pop_front();
		}
		pending_events.push_back(message);
		_schedule_flush();
		return;
	}
	MutexLock lock(state_mutex);
	response = message;
	has_response = true;
}

void TetraBlenderBridge::_handle_log_line(const String &p_line) {
	MutexLock lock(state_mutex);
	if (stderr_tail.size() >= 30) {
		stderr_tail.remove_at(0);
	}
	stderr_tail.push_back(p_line);
	if (pending_log.size() >= 2000) {
		pending_log.remove_at(0);
	}
	pending_log.push_back(p_line);
	_schedule_flush();
}

// Call with state_mutex held.
void TetraBlenderBridge::_schedule_flush() {
	if (flush_scheduled || destroying.is_set()) {
		return;
	}
	flush_scheduled = true;
	callable_mp_static(&TetraBlenderBridge::_flush_events_static).call_deferred(get_instance_id());
}

void TetraBlenderBridge::_flush_events_static(ObjectID p_bridge) {
	// The bridge may be gone by the time the main thread gets here.
	Ref<TetraBlenderBridge> bridge = ObjectDB::get_ref<TetraBlenderBridge>(p_bridge);
	if (bridge.is_valid()) {
		bridge->_flush_events();
	}
}

void TetraBlenderBridge::_flush_events() {
	PackedStringArray lines;
	Array events;
	bool started = false;
	bool stopped = false;
	{
		MutexLock lock(state_mutex);
		lines = pending_log;
		pending_log = PackedStringArray();
		events = pending_events;
		pending_events = Array();
		started = pending_started;
		stopped = pending_stopped;
		pending_started = false;
		pending_stopped = false;
		flush_scheduled = false;
	}
	if (started) {
		update_engine_registry();
		emit_signal(SNAME("started"));
	}
	for (const String &line : lines) {
		emit_signal(SNAME("log_line"), line);
	}
	for (int i = 0; i < events.size(); i++) {
		emit_signal(SNAME("progress"), events[i]);
	}
	if (stopped) {
		emit_signal(SNAME("stopped"));
	}
}

String TetraBlenderBridge::_stderr_summary() const {
	MutexLock lock(state_mutex);
	if (stderr_tail.is_empty()) {
		return String();
	}
	const int first = MAX(0, stderr_tail.size() - 6);
	return String("\n").join(stderr_tail.slice(first));
}

void TetraBlenderBridge::_set_last_error(const Dictionary &p_result) {
	MutexLock lock(state_mutex);
	last_error = is_ok(p_result) ? String() : String(p_result.get("error", "Unknown error"));
}

void TetraBlenderBridge::_kill_process() {
	MutexLock lock(kill_mutex);
	const int64_t pid = child_pid.get();
	if (pid != 0 && !process_reaped.is_set()) {
		process_reaped.set();
		OS::get_singleton()->kill(pid);
	}
}

// Call with request_mutex held. Leaves the bridge as if Blender had never been started.
void TetraBlenderBridge::_cleanup_process() {
	if (!reader_thread.is_started()) {
		return;
	}
	const bool was_alive = process_alive.is_set();
	_kill_process();
	reader_exit.set();
	reader_thread.wait_to_finish();
	reader_exit.clear();
	child_stdio.unref();
	child_stderr.unref();
	child_pid.set(0);
	process_alive.clear();
	server_ready.clear();
	MutexLock lock(state_mutex);
	has_response = false;
	response = Dictionary();
	if (was_alive) {
		pending_stopped = true;
		_schedule_flush();
	}
}

bool TetraBlenderBridge::_write_line(const CharString &p_line) {
	// The pipe is non-blocking; writes of up to PIPE_BUF bytes either happen
	// completely or not at all, so a refused chunk can simply be retried.
	const uint8_t *data = (const uint8_t *)p_line.get_data();
	int64_t remaining = p_line.length();
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	while (remaining > 0) {
		const int64_t chunk = MIN(remaining, int64_t(4096));
		if (child_stdio->store_buffer(data, chunk)) {
			data += chunk;
			remaining -= chunk;
			continue;
		}
		if (!process_alive.is_set() || OS::get_singleton()->get_ticks_usec() - started > 30000000) {
			return false;
		}
		OS::get_singleton()->delay_usec(500);
	}
	return true;
}

// Call with request_mutex held and the process started.
Dictionary TetraBlenderBridge::_transact(const Dictionary &p_command, double p_timeout_sec, bool p_kill_on_timeout) {
	const int64_t id = next_request_id++;
	Dictionary command = to_json(p_command);
	command["id"] = id;
	const String name = command.get("cmd", String());
	const CharString line = (JSON::stringify(command, "", false, true) + "\n").utf8();
	{
		MutexLock lock(state_mutex);
		has_response = false;
		response = Dictionary();
	}

	request_in_flight.set();
	Dictionary answer;
	if (!_write_line(line)) {
		answer = make_error(vformat("Could not send '%s' to Blender. %s", name, _stderr_summary()));
	} else {
		const uint64_t started = OS::get_singleton()->get_ticks_usec();
		uint32_t pause = 50;
		while (true) {
			// Read before looking for the answer: the reader stores everything
			// the process wrote before it clears this flag.
			const bool alive = process_alive.is_set();
			{
				MutexLock lock(state_mutex);
				if (has_response) {
					const Dictionary candidate = response;
					has_response = false;
					response = Dictionary();
					// The server answers a line it could not parse without an id.
					const Variant candidate_id = candidate.get("id", Variant());
					if (candidate_id.get_type() == Variant::NIL || int64_t(candidate_id) == id) {
						answer = candidate;
						break;
					}
				}
			}
			if (!alive) {
				answer = make_error(vformat("Blender exited while running '%s'. %s", name, _stderr_summary()));
				break;
			}
			if (p_timeout_sec > 0.0 && double(OS::get_singleton()->get_ticks_usec() - started) > p_timeout_sec * 1000000.0) {
				if (p_kill_on_timeout) {
					_kill_process();
					// Let the reader notice, so that the next request starts a fresh Blender.
					for (int i = 0; i < 2000 && process_alive.is_set(); i++) {
						OS::get_singleton()->delay_usec(500);
					}
				}
				answer = make_error(vformat("Blender did not answer '%s' within %s seconds.", name, String::num(p_timeout_sec, 1)));
				break;
			}
			OS::get_singleton()->delay_usec(pause);
			pause = MIN(pause * 2, 2000u);
		}
	}
	request_in_flight.clear();
	if (!answer.has("ok")) {
		answer["ok"] = false;
	}
	answer.erase("id");
	return answer;
}

// Call with request_mutex held.
Error TetraBlenderBridge::_start_locked() {
	if (process_alive.is_set() && server_ready.is_set()) {
		return OK;
	}
	_cleanup_process();

	auto fail = [this](Error p_error, const String &p_message) -> Error {
		MutexLock lock(state_mutex);
		last_error = p_message;
		return p_error;
	};

	const Dictionary launcher = find_launcher();
	const String kind = launcher["kind"];
	{
		MutexLock lock(state_mutex);
		launch_description = launcher["description"];
	}
	if (kind.is_empty()) {
		return fail(ERR_FILE_NOT_FOUND, launcher["description"]);
	}
	const String script = ensure_server_script();
	if (script.is_empty()) {
		return fail(ERR_FILE_CANT_WRITE, vformat("Could not write the Blender bridge script to %s.", cache_dir()));
	}

	List<String> arguments;
	if (kind == "blender") {
		arguments.push_back("--background");
		arguments.push_back("--factory-startup");
		arguments.push_back("--python-exit-code");
		arguments.push_back("1");
		arguments.push_back("--python");
		arguments.push_back(script);
		arguments.push_back("--");
	} else {
		arguments.push_back(script);
	}
	arguments.push_back("--temp-dir");
	arguments.push_back(get_exchange_dir());

	const Dictionary info = OS::get_singleton()->execute_with_pipe(launcher["path"], arguments, false);
	if (info.is_empty()) {
		return fail(ERR_CANT_FORK, vformat("Could not start %s.", String(launcher["path"])));
	}
	child_stdio = info["stdio"];
	child_stderr = info["stderr"];
	child_pid.set(int64_t(info["pid"]));
	{
		MutexLock lock(state_mutex);
		stderr_tail.clear();
		has_response = false;
	}
	reader_exit.clear();
	server_ready.clear();
	process_reaped.clear();
	process_alive.set();
	request_in_flight.set();
	reader_thread.start(_reader_thread_func, this);

	// Importing bpy takes from half a second to well over ten when the disk cache is cold.
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	while (!server_ready.is_set()) {
		if (!process_alive.is_set()) {
			const String summary = _stderr_summary();
			_cleanup_process();
			request_in_flight.clear();
			return fail(ERR_CANT_CONNECT, vformat("Blender exited while starting (%s). %s", String(launcher["description"]), summary));
		}
		if (destroying.is_set() || double(OS::get_singleton()->get_ticks_usec() - started) > start_timeout * 1000000.0) {
			_cleanup_process();
			request_in_flight.clear();
			return fail(ERR_TIMEOUT, vformat("Blender did not become ready within %s seconds (%s).", String::num(start_timeout, 0), String(launcher["description"])));
		}
		OS::get_singleton()->delay_usec(1000);
	}

	Dictionary hello;
	hello["cmd"] = "hello";
	const Dictionary answer = _transact(hello, 60.0, true);
	if (!is_ok(answer)) {
		_cleanup_process();
		return fail(ERR_CANT_CONNECT, String(answer.get("error", "Blender did not answer.")));
	}
	const String version = answer.get("blender_version", String());
	{
		MutexLock lock(known_version_mutex);
		known_version = version;
	}
	MutexLock lock(state_mutex);
	if (version != blender_version) {
		schema_cache.clear();
	}
	blender_version = version;
	server_info = answer;
	last_error = String();
	pending_started = true;
	_schedule_flush();
	return OK;
}

Error TetraBlenderBridge::start() {
	MutexLock lock(request_mutex);
	if (destroying.is_set()) {
		return ERR_UNAVAILABLE;
	}
	return _start_locked();
}

void TetraBlenderBridge::stop() {
	_cancel_queued_jobs(TTR("The Blender bridge was stopped."));
	if (request_mutex.try_lock()) {
		// Nothing is in flight, so Blender can be asked to leave by itself.
		if (process_alive.is_set() && server_ready.is_set()) {
			Dictionary command;
			command["cmd"] = "shutdown";
			_transact(command, 2.0, false);
			const uint64_t started = OS::get_singleton()->get_ticks_usec();
			while (process_alive.is_set() && OS::get_singleton()->get_ticks_usec() - started < 3000000) {
				OS::get_singleton()->delay_usec(1000);
			}
		}
		_cleanup_process();
		request_mutex.unlock();
		return;
	}
	// Another thread is waiting for an answer; ending the process is the only way to interrupt it.
	_kill_process();
	MutexLock lock(request_mutex);
	_cleanup_process();
}

bool TetraBlenderBridge::is_running() const {
	return process_alive.is_set() && server_ready.is_set();
}

bool TetraBlenderBridge::is_busy() const {
	return request_in_flight.is_set() || async_pending.get() > 0;
}

String TetraBlenderBridge::get_launch_description() const {
	{
		MutexLock lock(state_mutex);
		if (!launch_description.is_empty()) {
			return launch_description;
		}
	}
	return find_launcher()["description"];
}

String TetraBlenderBridge::get_blender_version() const {
	MutexLock lock(state_mutex);
	return blender_version;
}

Dictionary TetraBlenderBridge::get_server_info() const {
	MutexLock lock(state_mutex);
	return server_info.duplicate(true);
}

String TetraBlenderBridge::get_exchange_dir() {
	MutexLock lock(state_mutex);
	if (exchange_dir.is_empty()) {
		exchange_dir = cache_dir().path_join(vformat("exchange-%d-%d", OS::get_singleton()->get_process_id(), uint64_t(get_instance_id())));
	}
	if (!DirAccess::dir_exists_absolute(exchange_dir)) {
		DirAccess::make_dir_recursive_absolute(exchange_dir);
	}
	return exchange_dir;
}

String TetraBlenderBridge::get_last_error() const {
	MutexLock lock(state_mutex);
	return last_error;
}

void TetraBlenderBridge::set_start_timeout(double p_seconds) {
	start_timeout = MAX(p_seconds, 1.0);
}

double TetraBlenderBridge::get_start_timeout() const {
	return start_timeout;
}

void TetraBlenderBridge::set_join_triangles(bool p_enabled) {
	join_triangles = p_enabled;
}

bool TetraBlenderBridge::get_join_triangles() const {
	return join_triangles;
}

Dictionary TetraBlenderBridge::request(const Dictionary &p_command, double p_timeout_sec) {
	if (!p_command.has("cmd")) {
		return make_error("A request needs a \"cmd\".");
	}
	MutexLock lock(request_mutex);
	if (destroying.is_set()) {
		return make_error("The Blender bridge is shutting down.");
	}
	if (_start_locked() != OK) {
		return make_error(get_last_error());
	}
	return _transact(p_command, p_timeout_sec, true);
}

void TetraBlenderBridge::request_async(const Dictionary &p_command, const Callable &p_done, double p_timeout_sec) {
	_queue_job(JOB_REQUEST, p_command.has("cmd") ? p_command : make_error("A request needs a \"cmd\"."), PackedStringArray(), Variant(), p_done, p_timeout_sec);
}

String TetraBlenderBridge::_temp_path(const String &p_suffix, PackedStringArray &r_temp_files) {
	const String path = get_exchange_dir().path_join(vformat("%d-%s", temp_counter.postincrement(), p_suffix));
	r_temp_files.push_back(path);
	return path;
}

Dictionary TetraBlenderBridge::_prepare_mesh_command(const String &p_cmd, const Ref<Mesh> &p_mesh, PackedStringArray &r_temp_files) {
	if (p_mesh.is_null()) {
		return make_error("No mesh was given.");
	}
	const String input = _temp_path("in.tmsh", r_temp_files);
	if (TetraBlenderMesh::write_file(p_mesh, input) != OK) {
		return make_error(vformat("Could not write the mesh exchange file %s.", input));
	}
	Dictionary command;
	command["cmd"] = p_cmd;
	command["mesh"] = input;
	command["output"] = _temp_path("out.tmsh", r_temp_files);
	command["join_triangles"] = join_triangles;
	return command;
}

Dictionary TetraBlenderBridge::_prepare_apply_modifiers(const Ref<Mesh> &p_mesh, const Array &p_stack, PackedStringArray &r_temp_files) {
	Dictionary command = _prepare_mesh_command("apply_modifiers", p_mesh, r_temp_files);
	if (!command.has("cmd")) {
		return command;
	}
	Array modifiers;
	for (int i = 0; i < p_stack.size(); i++) {
		if (p_stack[i].get_type() != Variant::DICTIONARY) {
			return make_error(vformat("Modifier %d is not a Dictionary.", i));
		}
		const Dictionary entry = p_stack[i];
		Dictionary modifier;
		modifier["type"] = entry.get("type", entry.get("id", String()));
		modifier["values"] = entry.get("values", Dictionary());
		modifier["enabled"] = entry.get("enabled", true);
		const Dictionary objects = entry.get("objects", Dictionary());
		Dictionary object_files;
		const Array names = objects.keys();
		for (int j = 0; j < names.size(); j++) {
			const Variant target = objects[names[j]];
			Ref<Mesh> other = target;
			Transform3D transform;
			if (target.get_type() == Variant::DICTIONARY) {
				const Dictionary description = target;
				other = description.get("mesh", Variant());
				transform = description.get("transform", Transform3D());
			}
			if (other.is_null()) {
				return make_error(vformat("Modifier %d: '%s' needs a Mesh.", i, String(names[j])));
			}
			const String file = _temp_path("object.tmsh", r_temp_files);
			if (TetraBlenderMesh::write_file(other, file) != OK) {
				return make_error(vformat("Could not write the mesh exchange file %s.", file));
			}
			Dictionary object;
			object["mesh"] = file;
			object["transform"] = TetraBlenderMesh::transform_to_matrix(transform, true);
			object_files[String(names[j])] = object;
		}
		if (!object_files.is_empty()) {
			modifier["objects"] = object_files;
		}
		modifiers.push_back(modifier);
	}
	command["modifiers"] = modifiers;
	return command;
}

Dictionary TetraBlenderBridge::_prepare_run_operators(const Ref<Mesh> &p_mesh, const Array &p_operators, const Dictionary &p_options, PackedStringArray &r_temp_files) {
	Dictionary command = _prepare_mesh_command("run_operators", p_mesh, r_temp_files);
	if (!command.has("cmd")) {
		return command;
	}
	for (int i = 0; i < p_operators.size(); i++) {
		if (p_operators[i].get_type() != Variant::DICTIONARY) {
			return make_error(vformat("Operator %d is not a Dictionary.", i));
		}
	}
	command["operators"] = p_operators;
	command["select"] = p_options.get("select", "ALL");
	if (p_options.has("select_mode")) {
		command["select_mode"] = p_options["select_mode"];
	}
	if (p_options.has("join_triangles")) {
		command["join_triangles"] = bool(p_options["join_triangles"]);
	}
	return command;
}

Dictionary TetraBlenderBridge::_prepare_create_primitive(const String &p_id, const Dictionary &p_values, PackedStringArray &r_temp_files) {
	Dictionary command;
	command["cmd"] = "create_primitive";
	command["primitive"] = p_id;
	command["values"] = p_values;
	command["output"] = _temp_path("out.tmsh", r_temp_files);
	return command;
}

// Writes the meshes of a scene description; returns an error answer or an empty Dictionary.
Dictionary TetraBlenderBridge::_prepare_scene_meshes(const Array &p_meshes, Array &r_entries, PackedStringArray &r_temp_files) {
	for (int i = 0; i < p_meshes.size(); i++) {
		if (p_meshes[i].get_type() != Variant::DICTIONARY) {
			return make_error(vformat("Mesh %d is not a Dictionary.", i));
		}
		const Dictionary source = p_meshes[i];
		const Ref<Mesh> mesh = source.get("mesh", Variant());
		if (mesh.is_null()) {
			return make_error(vformat("Mesh %d has no \"mesh\".", i));
		}
		const String file = _temp_path("scene.tmsh", r_temp_files);
		if (TetraBlenderMesh::write_file(mesh, file) != OK) {
			return make_error(vformat("Could not write the mesh exchange file %s.", file));
		}
		Dictionary entry;
		entry["file"] = file;
		entry["name"] = source.get("name", mesh->get_name().is_empty() ? vformat("Mesh%d", i) : mesh->get_name());
		entry["transform"] = TetraBlenderMesh::transform_to_matrix(source.get("transform", Transform3D()), true);
		if (source.has("material")) {
			entry["material"] = material_to_json(source["material"]);
		} else {
			// One description per surface: the surface's own material where it
			// is a BaseMaterial3D, otherwise what the caller listed.
			const Array listed = source.get("materials", Array());
			Array materials;
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				if (surface < listed.size() && listed[surface].get_type() == Variant::DICTIONARY) {
					materials.push_back(material_to_json(listed[surface]));
				} else {
					materials.push_back(material_from_resource(mesh->surface_get_material(surface)));
				}
			}
			entry["materials"] = materials;
		}
		r_entries.push_back(entry);
	}
	return Dictionary();
}

Dictionary TetraBlenderBridge::_prepare_render(const Dictionary &p_scene, const Dictionary &p_settings, PackedStringArray &r_temp_files) {
	Array meshes;
	const Dictionary failure = _prepare_scene_meshes(p_scene.get("meshes", Array()), meshes, r_temp_files);
	if (!failure.is_empty()) {
		return failure;
	}
	Dictionary scene;
	scene["meshes"] = meshes;

	const Dictionary camera_source = p_scene.get("camera", Dictionary());
	Dictionary camera = to_json(camera_source);
	camera["transform"] = TetraBlenderMesh::transform_to_matrix(camera_source.get("transform", Transform3D()), false);
	scene["camera"] = camera;

	const Array light_sources = p_scene.get("lights", Array());
	Array lights;
	for (int i = 0; i < light_sources.size(); i++) {
		if (light_sources[i].get_type() != Variant::DICTIONARY) {
			return make_error(vformat("Light %d is not a Dictionary.", i));
		}
		const Dictionary source = light_sources[i];
		Dictionary light = to_json(source);
		light["transform"] = TetraBlenderMesh::transform_to_matrix(source.get("transform", Transform3D()), false);
		if (source.has("color")) {
			light["color"] = linear_color(source["color"]);
		}
		lights.push_back(light);
	}
	scene["lights"] = lights;

	const Dictionary world_source = p_scene.get("world", Dictionary());
	Dictionary world = to_json(world_source);
	if (world_source.has("color")) {
		world["color"] = linear_color(world_source["color"]);
	}
	scene["world"] = world;

	Dictionary command;
	command["cmd"] = "render";
	command["scene"] = scene;
	command["settings"] = p_settings;
	command["output"] = _temp_path("render.png", r_temp_files);
	return command;
}

Dictionary TetraBlenderBridge::_prepare_export(const String &p_path, const Array &p_meshes, PackedStringArray &r_temp_files) {
	Array meshes;
	const Dictionary failure = _prepare_scene_meshes(p_meshes, meshes, r_temp_files);
	if (!failure.is_empty()) {
		return failure;
	}
	Dictionary command;
	command["cmd"] = "export_meshes";
	command["path"] = ProjectSettings::get_singleton()->globalize_path(p_path);
	command["meshes"] = meshes;
	return command;
}

// The part of a job that may run on any thread: the request itself and
// turning the files Blender wrote into plain data.
Dictionary TetraBlenderBridge::_execute(const Job &p_job) {
	Dictionary result;
	if (!p_job.command.has("cmd")) {
		result = p_job.command;
	} else if (p_job.kind == JOB_LIST) {
		const String what = p_job.context;
		bool cached = false;
		{
			MutexLock lock(state_mutex);
			if (schema_cache.has(what)) {
				result["ok"] = true;
				result["operations"] = schema_cache[what].duplicate(true);
				cached = true;
			}
		}
		if (!cached) {
			result = request(p_job.command, p_job.timeout);
			if (is_ok(result)) {
				const Array operations = convert_schemas(result.get(what, Variant()));
				result.erase(what);
				result["operations"] = operations;
				MutexLock lock(state_mutex);
				schema_cache[what] = operations.duplicate(true);
			}
		}
	} else {
		result = request(p_job.command, p_job.timeout);
	}

	if (is_ok(result)) {
		switch (p_job.kind) {
			case JOB_MESH: {
				Error error = OK;
				const Array surfaces = TetraBlenderMesh::read_surfaces(result.get("output", String()), &error);
				if (error != OK) {
					result = make_error("Could not read the mesh Blender wrote.");
				} else {
					result["surfaces"] = surfaces;
				}
			} break;
			case JOB_IMPORT: {
				const Array meshes = result.get("meshes", Array());
				const String directory = result.get("directory", String());
				for (int i = 0; i < meshes.size(); i++) {
					Dictionary entry = meshes[i];
					const String file = entry.get("file", String());
					Error error = OK;
					entry["surfaces"] = TetraBlenderMesh::read_surfaces(file, &error);
					DirAccess::remove_absolute(file);
					if (error != OK) {
						result = make_error(vformat("Could not read the imported mesh '%s'.", String(entry.get("name", String()))));
						break;
					}
				}
				if (!directory.is_empty() && directory.begins_with(get_exchange_dir())) {
					remove_dir_recursive(directory);
				}
			} break;
			case JOB_RENDER: {
				const Ref<Image> image = Image::load_from_file(result.get("output", String()));
				if (image.is_null() || image->is_empty()) {
					result = make_error("Could not read the image Blender rendered.");
				} else {
					result["image"] = image;
				}
			} break;
			default: {
			} break;
		}
	}
	for (const String &file : p_job.temp_files) {
		if (FileAccess::exists(file)) {
			DirAccess::remove_absolute(file);
		}
	}
	return result;
}

// The part of a job that creates resources; the asynchronous calls run it on the main thread.
Dictionary TetraBlenderBridge::_finalize(JobKind p_kind, const Dictionary &p_result, const Variant &p_context) {
	Dictionary result = p_result;
	if (!result.has("error")) {
		result["error"] = String();
	}
	if (!is_ok(result)) {
		return result;
	}
	integerize(result, "vertex_count");
	integerize(result, "polygon_count");
	integerize(result, "triangle_count");
	switch (p_kind) {
		case JOB_MESH: {
			result["mesh"] = TetraBlenderMesh::surfaces_to_mesh(result.get("surfaces", Array()), Ref<Mesh>(p_context));
			result.erase("surfaces");
			result.erase("output");
		} break;
		case JOB_IMPORT: {
			const Array entries = result.get("meshes", Array());
			Array meshes;
			for (int i = 0; i < entries.size(); i++) {
				const Dictionary entry = entries[i];
				const Array surfaces = entry.get("surfaces", Array());
				const Ref<ArrayMesh> mesh = TetraBlenderMesh::surfaces_to_mesh(surfaces);
				mesh->set_name(String(entry.get("name", String())));
				const Array descriptions = entry.get("materials", Array());
				LocalVector<Ref<StandardMaterial3D>> materials;
				materials.resize(descriptions.size());
				for (int surface = 0; surface < mesh->get_surface_count() && surface < surfaces.size(); surface++) {
					const int slot = int(Dictionary(surfaces[surface]).get("material_index", -1));
					if (slot < 0 || slot >= descriptions.size() || String(Dictionary(descriptions[slot]).get("name", String())).is_empty()) {
						continue;
					}
					if (materials[slot].is_null()) {
						materials[slot] = material_from_description(descriptions[slot]);
					}
					mesh->surface_set_material(surface, materials[slot]);
				}
				Dictionary item;
				item["name"] = entry.get("name", String());
				item["parent"] = entry.get("parent", String());
				const PackedFloat64Array matrix = entry.get("transform", Array());
				item["transform"] = TetraBlenderMesh::matrix_to_transform(matrix, true);
				item["mesh"] = mesh;
				meshes.push_back(item);
			}
			result["meshes"] = meshes;
			result.erase("directory");
		} break;
		case JOB_RENDER: {
			result.erase("output");
		} break;
		default: {
		} break;
	}
	return result;
}

Dictionary TetraBlenderBridge::_run_now(JobKind p_kind, const Dictionary &p_command, const PackedStringArray &p_temp_files, const Variant &p_context) {
	Job job;
	job.kind = p_kind;
	job.command = p_command;
	job.temp_files = p_temp_files;
	job.context = p_context;
	const Dictionary result = _finalize(p_kind, _execute(job), p_context);
	_set_last_error(result);
	return result;
}

void TetraBlenderBridge::_queue_job(JobKind p_kind, const Dictionary &p_command, const PackedStringArray &p_temp_files, const Variant &p_context, const Callable &p_done, double p_timeout) {
	Job job;
	job.kind = p_kind;
	job.command = p_command;
	job.temp_files = p_temp_files;
	job.context = p_context;
	job.done = p_done;
	job.timeout = p_timeout;
	{
		MutexLock lock(async_mutex);
		async_queue.push_back(job);
		async_pending.increment();
		if (!async_thread.is_started()) {
			async_thread.start(_async_thread_func, this);
		}
	}
	async_semaphore.post();
}

void TetraBlenderBridge::_cancel_queued_jobs(const String &p_reason) {
	List<Job> cancelled;
	{
		MutexLock lock(async_mutex);
		for (const Job &job : async_queue) {
			cancelled.push_back(job);
			async_pending.decrement();
		}
		async_queue.clear();
	}
	for (const Job &job : cancelled) {
		for (const String &file : job.temp_files) {
			if (FileAccess::exists(file)) {
				DirAccess::remove_absolute(file);
			}
		}
		if (!destroying.is_set()) {
			callable_mp_static(&TetraBlenderBridge::_async_finish_static).call_deferred(get_instance_id(), job.done, int(job.kind), make_error(p_reason), job.context);
		}
	}
}

void TetraBlenderBridge::_async_thread_func(void *p_self) {
	static_cast<TetraBlenderBridge *>(p_self)->_async_loop();
}

void TetraBlenderBridge::_async_loop() {
	while (true) {
		async_semaphore.wait();
		if (async_exit.is_set()) {
			break;
		}
		Job job;
		{
			MutexLock lock(async_mutex);
			if (async_queue.is_empty()) {
				continue;
			}
			job = async_queue.front()->get();
			async_queue.pop_front();
		}
		const Dictionary result = _execute(job);
		async_pending.decrement();
		if (!destroying.is_set()) {
			callable_mp_static(&TetraBlenderBridge::_async_finish_static).call_deferred(get_instance_id(), job.done, int(job.kind), result, job.context);
		}
	}
}

void TetraBlenderBridge::_async_finish_static(ObjectID p_bridge, const Callable &p_done, int p_kind, const Dictionary &p_result, const Variant &p_context) {
	Ref<TetraBlenderBridge> bridge = ObjectDB::get_ref<TetraBlenderBridge>(p_bridge);
	if (bridge.is_null()) {
		return;
	}
	const Dictionary result = bridge->_finalize(JobKind(p_kind), p_result, p_context);
	bridge->_set_last_error(result);
	if (p_done.is_valid()) {
		p_done.call(result);
	}
}

Array TetraBlenderBridge::_list(const String &p_what) {
	Dictionary command;
	command["cmd"] = "list_" + p_what;
	return _run_now(JOB_LIST, command, PackedStringArray(), p_what).get("operations", Array());
}

void TetraBlenderBridge::_list_async(const String &p_what, const Callable &p_done) {
	Dictionary command;
	command["cmd"] = "list_" + p_what;
	_queue_job(JOB_LIST, command, PackedStringArray(), p_what, p_done);
}

Array TetraBlenderBridge::list_operators() {
	return _list("operators");
}

Array TetraBlenderBridge::list_modifiers() {
	return _list("modifiers");
}

Array TetraBlenderBridge::list_primitives() {
	return _list("primitives");
}

void TetraBlenderBridge::list_operators_async(const Callable &p_done) {
	_list_async("operators", p_done);
}

void TetraBlenderBridge::list_modifiers_async(const Callable &p_done) {
	_list_async("modifiers", p_done);
}

void TetraBlenderBridge::list_primitives_async(const Callable &p_done) {
	_list_async("primitives", p_done);
}

Ref<ArrayMesh> TetraBlenderBridge::apply_modifier_stack(const Ref<Mesh> &p_mesh, const Array &p_stack) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_apply_modifiers(p_mesh, p_stack, temp_files);
	return _run_now(JOB_MESH, command, temp_files, p_mesh).get("mesh", Variant());
}

void TetraBlenderBridge::apply_modifier_stack_async(const Ref<Mesh> &p_mesh, const Array &p_stack, const Callable &p_done) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_apply_modifiers(p_mesh, p_stack, temp_files);
	_queue_job(JOB_MESH, command, temp_files, p_mesh, p_done);
}

Ref<ArrayMesh> TetraBlenderBridge::run_operators(const Ref<Mesh> &p_mesh, const Array &p_operators, const Dictionary &p_options) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_run_operators(p_mesh, p_operators, p_options, temp_files);
	return _run_now(JOB_MESH, command, temp_files, p_mesh).get("mesh", Variant());
}

void TetraBlenderBridge::run_operators_async(const Ref<Mesh> &p_mesh, const Array &p_operators, const Callable &p_done, const Dictionary &p_options) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_run_operators(p_mesh, p_operators, p_options, temp_files);
	_queue_job(JOB_MESH, command, temp_files, p_mesh, p_done);
}

Ref<ArrayMesh> TetraBlenderBridge::create_primitive(const String &p_id, const Dictionary &p_values) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_create_primitive(p_id, p_values, temp_files);
	return _run_now(JOB_MESH, command, temp_files, Variant()).get("mesh", Variant());
}

void TetraBlenderBridge::create_primitive_async(const String &p_id, const Dictionary &p_values, const Callable &p_done) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_create_primitive(p_id, p_values, temp_files);
	_queue_job(JOB_MESH, command, temp_files, Variant(), p_done);
}

Array TetraBlenderBridge::import_meshes(const String &p_path) {
	Dictionary command;
	command["cmd"] = "import_meshes";
	command["path"] = ProjectSettings::get_singleton()->globalize_path(p_path);
	return _run_now(JOB_IMPORT, command, PackedStringArray(), Variant()).get("meshes", Array());
}

void TetraBlenderBridge::import_meshes_async(const String &p_path, const Callable &p_done) {
	Dictionary command;
	command["cmd"] = "import_meshes";
	command["path"] = ProjectSettings::get_singleton()->globalize_path(p_path);
	_queue_job(JOB_IMPORT, command, PackedStringArray(), Variant(), p_done);
}

Error TetraBlenderBridge::export_meshes(const String &p_path, const Array &p_meshes) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_export(p_path, p_meshes, temp_files);
	return is_ok(_run_now(JOB_REQUEST, command, temp_files, Variant())) ? OK : FAILED;
}

void TetraBlenderBridge::export_meshes_async(const String &p_path, const Array &p_meshes, const Callable &p_done) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_export(p_path, p_meshes, temp_files);
	_queue_job(JOB_REQUEST, command, temp_files, Variant(), p_done);
}

Ref<Image> TetraBlenderBridge::render_scene(const Dictionary &p_scene, const Dictionary &p_settings) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_render(p_scene, p_settings, temp_files);
	return _run_now(JOB_RENDER, command, temp_files, Variant()).get("image", Variant());
}

void TetraBlenderBridge::render_scene_async(const Dictionary &p_scene, const Dictionary &p_settings, const Callable &p_done) {
	PackedStringArray temp_files;
	const Dictionary command = _prepare_render(p_scene, p_settings, temp_files);
	_queue_job(JOB_RENDER, command, temp_files, Variant(), p_done);
}

void TetraBlenderBridge::_bind_methods() {
	ClassDB::bind_static_method("TetraBlenderBridge", D_METHOD("get_shared"), &TetraBlenderBridge::get_shared);
	ClassDB::bind_static_method("TetraBlenderBridge", D_METHOD("find_launcher"), &TetraBlenderBridge::find_launcher);
	ClassDB::bind_static_method("TetraBlenderBridge", D_METHOD("is_available"), &TetraBlenderBridge::is_available);

	ClassDB::bind_method(D_METHOD("start"), &TetraBlenderBridge::start);
	ClassDB::bind_method(D_METHOD("stop"), &TetraBlenderBridge::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &TetraBlenderBridge::is_running);
	ClassDB::bind_method(D_METHOD("is_busy"), &TetraBlenderBridge::is_busy);
	ClassDB::bind_method(D_METHOD("get_launch_description"), &TetraBlenderBridge::get_launch_description);
	ClassDB::bind_method(D_METHOD("get_blender_version"), &TetraBlenderBridge::get_blender_version);
	ClassDB::bind_method(D_METHOD("get_server_info"), &TetraBlenderBridge::get_server_info);
	ClassDB::bind_method(D_METHOD("get_exchange_dir"), &TetraBlenderBridge::get_exchange_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &TetraBlenderBridge::get_last_error);
	ClassDB::bind_method(D_METHOD("set_start_timeout", "seconds"), &TetraBlenderBridge::set_start_timeout);
	ClassDB::bind_method(D_METHOD("get_start_timeout"), &TetraBlenderBridge::get_start_timeout);
	ClassDB::bind_method(D_METHOD("set_join_triangles", "enabled"), &TetraBlenderBridge::set_join_triangles);
	ClassDB::bind_method(D_METHOD("get_join_triangles"), &TetraBlenderBridge::get_join_triangles);

	ClassDB::bind_method(D_METHOD("request", "command", "timeout_sec"), &TetraBlenderBridge::request, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("request_async", "command", "done", "timeout_sec"), &TetraBlenderBridge::request_async, DEFVAL(0.0));

	ClassDB::bind_method(D_METHOD("list_operators"), &TetraBlenderBridge::list_operators);
	ClassDB::bind_method(D_METHOD("list_modifiers"), &TetraBlenderBridge::list_modifiers);
	ClassDB::bind_method(D_METHOD("list_primitives"), &TetraBlenderBridge::list_primitives);
	ClassDB::bind_method(D_METHOD("apply_modifier_stack", "mesh", "stack"), &TetraBlenderBridge::apply_modifier_stack);
	ClassDB::bind_method(D_METHOD("run_operators", "mesh", "operators", "options"), &TetraBlenderBridge::run_operators, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("create_primitive", "id", "values"), &TetraBlenderBridge::create_primitive, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("import_meshes", "path"), &TetraBlenderBridge::import_meshes);
	ClassDB::bind_method(D_METHOD("export_meshes", "path", "meshes"), &TetraBlenderBridge::export_meshes);
	ClassDB::bind_method(D_METHOD("render_scene", "scene", "settings"), &TetraBlenderBridge::render_scene, DEFVAL(Dictionary()));

	ClassDB::bind_method(D_METHOD("list_operators_async", "done"), &TetraBlenderBridge::list_operators_async);
	ClassDB::bind_method(D_METHOD("list_modifiers_async", "done"), &TetraBlenderBridge::list_modifiers_async);
	ClassDB::bind_method(D_METHOD("list_primitives_async", "done"), &TetraBlenderBridge::list_primitives_async);
	ClassDB::bind_method(D_METHOD("apply_modifier_stack_async", "mesh", "stack", "done"), &TetraBlenderBridge::apply_modifier_stack_async);
	ClassDB::bind_method(D_METHOD("run_operators_async", "mesh", "operators", "done", "options"), &TetraBlenderBridge::run_operators_async, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("create_primitive_async", "id", "values", "done"), &TetraBlenderBridge::create_primitive_async);
	ClassDB::bind_method(D_METHOD("import_meshes_async", "path", "done"), &TetraBlenderBridge::import_meshes_async);
	ClassDB::bind_method(D_METHOD("export_meshes_async", "path", "meshes", "done"), &TetraBlenderBridge::export_meshes_async);
	ClassDB::bind_method(D_METHOD("render_scene_async", "scene", "settings", "done"), &TetraBlenderBridge::render_scene_async);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_timeout"), "set_start_timeout", "get_start_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "join_triangles"), "set_join_triangles", "get_join_triangles");

	ADD_SIGNAL(MethodInfo("started"));
	ADD_SIGNAL(MethodInfo("stopped"));
	ADD_SIGNAL(MethodInfo("progress", PropertyInfo(Variant::DICTIONARY, "event")));
	ADD_SIGNAL(MethodInfo("log_line", PropertyInfo(Variant::STRING, "line")));
}

TetraBlenderBridge::TetraBlenderBridge() {
}

TetraBlenderBridge::~TetraBlenderBridge() {
	destroying.set();
	async_exit.set();
	_cancel_queued_jobs(String());
	// Ends a request the worker thread may be waiting on.
	_kill_process();
	if (async_thread.is_started()) {
		async_semaphore.post();
		async_thread.wait_to_finish();
	}
	{
		MutexLock lock(request_mutex);
		_cleanup_process();
	}
	if (!exchange_dir.is_empty()) {
		remove_dir_recursive(exchange_dir);
	}
}
