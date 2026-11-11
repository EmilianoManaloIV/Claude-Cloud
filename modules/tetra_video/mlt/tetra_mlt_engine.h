#pragma once

#include "core/io/image.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "servers/audio/effects/audio_stream_generator.h"

class TetraVideoProject;

// The bridge to MLT, the engine that plays and renders Kdenlive's timelines.
//
// The static methods describe the installation (services, metadata, media
// probing). An instance holds one producer graph built from MLT XML and gives
// access to its frames: synchronously with render_frame(), or through a
// playback thread that publishes frames with the frame_ready signal and
// feeds the frame's sound to Godot's AudioServer.
//
// Every call into MLT, from any instance or thread, is serialised by one
// mutex, so all methods may be called from any thread. Built without
// libmlt++ every method still exists, is_available() is false and every call
// fails cleanly.
class TetraMltEngine : public RefCounted {
	GDCLASS(TetraMltEngine, RefCounted);

	struct Graph;

	// The graph and the thumbnail sources are only touched under the MLT mutex.
	Graph *graph = nullptr;
	HashMap<String, Graph *> thumbnail_graphs;
	Vector<String> thumbnail_order;
	int profile_width = 1920;
	int profile_height = 1080;
	int profile_fps_num = 25;
	int profile_fps_den = 1;

	// Playback state shared between the callers and the playback thread.
	Mutex control_mutex;
	Thread thread;
	SafeFlag exit_requested;
	SafeFlag frame_signal_pending;
	bool playing = false;
	bool loop = false;
	bool refresh_requested = false;
	double speed = 1.0;
	double volume = 1.0;
	double fps = 25.0;
	int position = 0;
	int length = 0;
	uint64_t generation = 0;
	Vector2i preview_size;
	Ref<Image> current_image;
	int current_frame = -1;
	SafeNumeric<int64_t> audio_frames_pushed;
	SafeNumeric<int64_t> dropped_frames;
	StringName audio_bus = "Master";
	// The playback keeps a plain pointer to its generator, so both are held.
	Ref<AudioStreamGenerator> audio_generator;
	Ref<AudioStreamGeneratorPlayback> audio_playback;

	static void _thread_main(void *p_self);
	void _run_playback();
	void _ensure_thread();
	void _start_audio();
	void _stop_audio();
	void _publish(const Ref<Image> &p_image, int p_frame, uint64_t p_generation);
	void _finish_playback(int p_frame, uint64_t p_generation);
	void _emit_frame_ready();
	void _emit_playback_finished();
	void _drop_graph();
	void _drop_thumbnails();

protected:
	static void _bind_methods();

public:
	static bool is_available();
	static String get_version();
	static PackedStringArray list_services(const String &p_kind);
	static bool has_service(const String &p_kind, const String &p_name);
	static String get_preferred_service(const String &p_kind, const String &p_candidates);
	static Dictionary get_service_operation(const String &p_kind, const String &p_name);
	static Dictionary probe(const String &p_path, const Ref<TetraVideoProject> &p_project = Ref<TetraVideoProject>());
	static int import_media(const Ref<TetraVideoProject> &p_project, const String &p_path);
	static PackedFloat32Array get_audio_peaks(const String &p_path, int p_samples_per_second = 50);

	Error set_project(const Ref<TetraVideoProject> &p_project);
	Error set_xml(const String &p_xml);
	bool has_graph() const;
	int get_length() const;
	Ref<Image> render_frame(int p_frame, const Vector2i &p_max_size = Vector2i());
	Ref<Image> get_thumbnail(const Variant &p_source, int p_frame, const Vector2i &p_size);

	void play();
	void pause();
	void seek(int p_frame);
	void set_speed(double p_speed);
	double get_speed() const;
	int get_position() const;
	bool is_playing() const;
	void set_loop(bool p_loop);
	bool get_loop() const;
	void set_preview_size(const Vector2i &p_size);
	Vector2i get_preview_size() const;
	void set_volume(double p_volume);
	double get_volume() const;
	void set_audio_bus(const StringName &p_bus);
	StringName get_audio_bus() const;
	Ref<Image> get_current_frame_image() const;
	int get_current_frame() const;
	int64_t get_audio_frames_pushed() const;
	int64_t get_dropped_frames() const;

	TetraMltEngine();
	~TetraMltEngine();
};
