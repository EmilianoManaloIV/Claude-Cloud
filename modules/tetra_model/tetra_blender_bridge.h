#pragma once

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "scene/resources/mesh.h"

// A long-lived Blender process driven over the JSON Lines protocol of
// bridge/tetra_blender_server.py.
//
// Blender is started lazily by the first request. It is found, in this order,
// through: editor setting then project setting "tetra/blender/python_path" (a
// Python with the bpy module), the TETRA_BLENDER_PYTHON environment variable,
// editor setting "tetra/blender/executable_path", Godot's own
// "filesystem/import/blender/blender_path", and a "blender" on PATH.
//
// Threading: every method may be called from any thread. One command is in
// flight at a time; request() blocks its caller until the answer arrives and
// further callers queue behind it. The *_async variants return at once, run
// on the bridge's worker thread and call their Callable on the main thread.
// Signals are always emitted on the main thread. Start-up takes seconds, so
// editor UI should only use the asynchronous calls.
//
// Failures never raise: requests answer {"ok": false, "error": "..."},
// the object-returning calls return null (or an empty Array) and leave the
// message in get_last_error().
class TetraBlenderBridge : public RefCounted {
	GDCLASS(TetraBlenderBridge, RefCounted);

	enum JobKind {
		JOB_REQUEST,
		JOB_LIST,
		JOB_MESH,
		JOB_IMPORT,
		JOB_RENDER,
	};

	struct Job {
		JobKind kind = JOB_REQUEST;
		// Without a "cmd" the preparation failed and this already is the answer.
		Dictionary command;
		PackedStringArray temp_files;
		Variant context;
		Callable done;
		double timeout = 0.0;
	};

	static Ref<TetraBlenderBridge> shared;

	// Held for the whole of a request, which is what serialises them.
	Mutex request_mutex;
	// Guards what the reader thread shares with everyone else.
	mutable Mutex state_mutex;

	Ref<FileAccess> child_stdio;
	Ref<FileAccess> child_stderr;
	SafeNumeric<int64_t> child_pid;
	Thread reader_thread;
	SafeFlag reader_exit;
	SafeFlag process_alive;
	// Serialises the two places that may wait for the child process.
	Mutex kill_mutex;
	SafeFlag process_reaped;
	SafeFlag server_ready;
	SafeFlag request_in_flight;
	SafeFlag destroying;

	bool has_response = false;
	Dictionary response;
	PackedStringArray stderr_tail;
	PackedStringArray pending_log;
	Array pending_events;
	bool pending_started = false;
	bool pending_stopped = false;
	bool flush_scheduled = false;

	int64_t next_request_id = 1;
	double start_timeout = 180.0;
	bool join_triangles = false;
	String exchange_dir;
	SafeNumeric<uint32_t> temp_counter;
	String launch_description;
	String blender_version;
	Dictionary server_info;
	String last_error;
	HashMap<String, Array> schema_cache;

	Thread async_thread;
	Mutex async_mutex;
	List<Job> async_queue;
	Semaphore async_semaphore;
	SafeFlag async_exit;
	SafeNumeric<uint32_t> async_pending;

	static void _reader_thread_func(void *p_self);
	static void _async_thread_func(void *p_self);
	static void _flush_events_static(ObjectID p_bridge);
	static void _async_finish_static(ObjectID p_bridge, const Callable &p_done, int p_kind, const Dictionary &p_result, const Variant &p_context);

	void _reader_loop();
	bool _drain(const Ref<FileAccess> &p_pipe, Vector<uint8_t> &r_buffer, bool p_protocol);
	void _handle_protocol_line(const String &p_line);
	void _handle_log_line(const String &p_line);
	void _schedule_flush();
	void _flush_events();

	Error _start_locked();
	void _kill_process();
	void _cleanup_process();
	bool _write_line(const CharString &p_line);
	Dictionary _transact(const Dictionary &p_command, double p_timeout_sec, bool p_kill_on_timeout);
	String _stderr_summary() const;
	void _set_last_error(const Dictionary &p_result);

	String _temp_path(const String &p_suffix, PackedStringArray &r_temp_files);
	Dictionary _prepare_mesh_command(const String &p_cmd, const Ref<Mesh> &p_mesh, PackedStringArray &r_temp_files);
	Dictionary _prepare_apply_modifiers(const Ref<Mesh> &p_mesh, const Array &p_stack, PackedStringArray &r_temp_files);
	Dictionary _prepare_run_operators(const Ref<Mesh> &p_mesh, const Array &p_operators, const Dictionary &p_options, PackedStringArray &r_temp_files);
	Dictionary _prepare_create_primitive(const String &p_id, const Dictionary &p_values, PackedStringArray &r_temp_files);
	Dictionary _prepare_scene_meshes(const Array &p_meshes, Array &r_entries, PackedStringArray &r_temp_files);
	Dictionary _prepare_render(const Dictionary &p_scene, const Dictionary &p_settings, PackedStringArray &r_temp_files);
	Dictionary _prepare_export(const String &p_path, const Array &p_meshes, PackedStringArray &r_temp_files);

	Dictionary _execute(const Job &p_job);
	Dictionary _finalize(JobKind p_kind, const Dictionary &p_result, const Variant &p_context);
	Dictionary _run_now(JobKind p_kind, const Dictionary &p_command, const PackedStringArray &p_temp_files, const Variant &p_context);
	void _queue_job(JobKind p_kind, const Dictionary &p_command, const PackedStringArray &p_temp_files, const Variant &p_context, const Callable &p_done, double p_timeout = 0.0);
	void _cancel_queued_jobs(const String &p_reason);
	void _async_loop();
	Array _list(const String &p_what);
	void _list_async(const String &p_what, const Callable &p_done);

protected:
	static void _bind_methods();

public:
	// The bridge the editor UI shares, created on first use.
	static Ref<TetraBlenderBridge> get_shared();
	static void free_shared();

	// {kind: "python" | "blender" | "", path: String, description: String}.
	// kind is empty when no Blender can be launched; description then says
	// what to configure.
	static Dictionary find_launcher();
	static bool is_available();
	// Announces Blender in TetraEngineRegistry with what is known right now. Main thread only.
	static void update_engine_registry();

	Error start();
	// Ends the Blender process; a request in flight answers with an error and
	// queued asynchronous jobs are cancelled. The next request starts it again.
	void stop();
	bool is_running() const;
	// True while a request is in flight or asynchronous jobs are queued.
	bool is_busy() const;

	String get_launch_description() const;
	// Empty until Blender has been started once.
	String get_blender_version() const;
	// The server's "hello" answer: versions, cycles, denoise, file extensions.
	Dictionary get_server_info() const;
	String get_exchange_dir();
	String get_last_error() const;
	void set_start_timeout(double p_seconds);
	double get_start_timeout() const;
	// When set, apply_modifier_stack and run_operators first merge the
	// triangle pairs of the mesh back into quads (Godot meshes are always
	// triangles), where no material border, sharp edge or UV seam separates
	// them. run_operators' "join_triangles" option overrides it per call.
	void set_join_triangles(bool p_enabled);
	bool get_join_triangles() const;

	// Sends one protocol command ({"cmd": ..., ...}) and returns the answer.
	// Vector2/Vector3/Color values are sent as number arrays. With a positive
	// p_timeout_sec an unanswered request kills Blender, since it cannot be
	// interrupted any other way.
	Dictionary request(const Dictionary &p_command, double p_timeout_sec = 0.0);
	// p_done receives the answer Dictionary.
	void request_async(const Dictionary &p_command, const Callable &p_done, double p_timeout_sec = 0.0);

	// Operation schemas (docs/ARCHITECTURE.md), cached after the first call.
	// Operators carry "mode": "EDIT" | "OBJECT"; modifiers that take another
	// object carry "object_params": [{name, label, description}].
	Array list_operators();
	Array list_modifiers();
	Array list_primitives();

	// p_stack: [{type: "SUBSURF" (or id: "blender:modifier.SUBSURF"), values: Dictionary,
	// enabled: bool = true, objects: {param: Mesh | {mesh: Mesh, transform: Transform3D}}}].
	Ref<ArrayMesh> apply_modifier_stack(const Ref<Mesh> &p_mesh, const Array &p_stack);
	// p_operators: [{id: "blender:mesh.subdivide", values: Dictionary, select: "ALL" | "NONE" | "INVERT" (optional)}].
	// p_options: {select: "ALL" (default) | "NONE", select_mode: "VERT" | "EDGE" | "FACE"}.
	Ref<ArrayMesh> run_operators(const Ref<Mesh> &p_mesh, const Array &p_operators, const Dictionary &p_options = Dictionary());
	Ref<ArrayMesh> create_primitive(const String &p_id, const Dictionary &p_values);
	// [{name: String, transform: Transform3D, mesh: ArrayMesh, parent: String}].
	Array import_meshes(const String &p_path);
	// p_meshes: [{mesh: Mesh, name: String, transform: Transform3D}]. The format follows the extension.
	Error export_meshes(const String &p_path, const Array &p_meshes);
	// See README.md for the scene and settings dictionaries.
	Ref<Image> render_scene(const Dictionary &p_scene, const Dictionary &p_settings);

	// Asynchronous variants. p_done receives one Dictionary: {ok: bool,
	// error: String} plus, on success, the result under "operations", "mesh",
	// "meshes" or "image" and the statistics the server reported.
	void list_operators_async(const Callable &p_done);
	void list_modifiers_async(const Callable &p_done);
	void list_primitives_async(const Callable &p_done);
	void apply_modifier_stack_async(const Ref<Mesh> &p_mesh, const Array &p_stack, const Callable &p_done);
	void run_operators_async(const Ref<Mesh> &p_mesh, const Array &p_operators, const Callable &p_done, const Dictionary &p_options = Dictionary());
	void create_primitive_async(const String &p_id, const Dictionary &p_values, const Callable &p_done);
	void import_meshes_async(const String &p_path, const Callable &p_done);
	void export_meshes_async(const String &p_path, const Array &p_meshes, const Callable &p_done);
	void render_scene_async(const Dictionary &p_scene, const Dictionary &p_settings, const Callable &p_done);

	TetraBlenderBridge();
	~TetraBlenderBridge();
};
