#pragma once

#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

class TetraVideoProject;

// Renders a project to a media file by running MLT's command-line player,
// melt, on the project's XML, exactly as Kdenlive does. Encoding presets come
// from Kdenlive's profiles.xml plus three built-in ones.
//
// start() returns at once; a reader thread follows melt's progress output and
// reports with the progress and finished signals on the main thread. Keep a
// reference to the renderer while it runs: dropping it cancels the render.
class TetraVideoRenderer : public RefCounted {
	GDCLASS(TetraVideoRenderer, RefCounted);

	Thread thread;
	Mutex mutex;
	SafeFlag running;
	SafeFlag cancel_requested;
	int64_t process_id = 0;
	Ref<FileAccess> process_output;
	Ref<FileAccess> process_errors;
	String xml_path;
	String output_path;
	String command_line;
	String log_tail;
	String last_error;
	int first_frame = 0;
	int frame_count = 1;
	int current_frame = 0;
	int reported_frame = -1;
	double current_percent = 0.0;

	static void _thread_main(void *p_self);
	void _follow_process();
	void _read_progress();
	void _emit_progress(int p_frame, double p_percent);
	void _emit_finished(bool p_ok, const String &p_message);

protected:
	static void _bind_methods();

public:
	static TypedArray<Dictionary> get_presets();
	static Dictionary get_preset(const String &p_id);
	static String find_melt();
	static PackedStringArray expand_preset_arguments(const Dictionary &p_preset, const Dictionary &p_options);

	Error start(const Ref<TetraVideoProject> &p_project, const String &p_output_path, const String &p_preset_id, const Dictionary &p_options = Dictionary());
	void cancel();
	bool is_running() const;
	int get_frame() const;
	double get_percent() const;
	String get_command_line() const;
	String get_log() const;
	String get_last_error() const;

	~TetraVideoRenderer();
};
