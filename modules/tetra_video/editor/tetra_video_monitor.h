#pragma once

#include "scene/gui/box_container.h"

class Button;
class Image;
class ImageTexture;
class Label;
class LineEdit;
class OptionButton;
class TetraMltEngine;
class TetraVideoProject;

// The monitor: the picture of the engine at the playhead with the transport
// below it. It shows either the timeline (project mode) or one bin clip (clip
// mode, Kdenlive's clip monitor), each through an engine of its own, and keeps
// a position for each.
class TetraVideoMonitor : public VBoxContainer {
	GDCLASS(TetraVideoMonitor, VBoxContainer);

	Ref<TetraVideoProject> project;
	Ref<TetraMltEngine> project_engine;
	Ref<TetraMltEngine> clip_engine;
	// A one-clip timeline in the project's profile, shown in clip mode.
	Ref<TetraVideoProject> clip_project;
	int clip_bin_id = -1;
	bool clip_mode = false;
	int project_position = 0;
	int clip_position = 0;
	int preview_divisor = 2;
	bool loop = false;
	// Picture of the active engine; has_frame is false while there is nothing to show.
	Ref<ImageTexture> texture;
	bool has_frame = false;

	Button *project_mode_button = nullptr;
	Button *clip_mode_button = nullptr;
	Label *source_label = nullptr;
	Control *view = nullptr;
	Button *start_button = nullptr;
	Button *step_back_button = nullptr;
	Button *play_button = nullptr;
	Button *stop_button = nullptr;
	Button *step_forward_button = nullptr;
	Button *end_button = nullptr;
	Button *loop_button = nullptr;
	LineEdit *timecode_edit = nullptr;
	Label *duration_label = nullptr;
	OptionButton *preview_option = nullptr;

	Ref<TetraMltEngine> _active_engine() const;
	Ref<TetraVideoProject> _active_project() const;
	int _active_length() const;
	void _set_active_position(int p_frame);
	void _apply_preview_size();
	void _update_transport();
	void _update_timecode();
	void _update_icons();
	void _draw_view();
	void _engine_frame_ready(int p_frame, bool p_from_clip_engine);
	void _engine_finished(bool p_from_clip_engine);
	void _mode_pressed(bool p_clip_mode);
	void _loop_toggled(bool p_pressed);
	void _timecode_submitted(const String &p_text);
	void _timecode_focus_exited();
	void _preview_selected(int p_index);
	void _step_pressed(int p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_project(const Ref<TetraVideoProject> &p_project);
	void set_project_engine(const Ref<TetraMltEngine> &p_engine);
	Ref<TetraMltEngine> get_clip_engine() const;

	// Shows a bin clip of the project (clip mode); show_project() returns to the timeline.
	void show_clip(int p_bin_id);
	void show_project();
	bool is_clip_mode() const;
	int get_clip_bin_id() const;

	// Transport of whatever is shown.
	void seek(int p_frame);
	int get_position() const;
	void play();
	void pause();
	void toggle_play();
	void stop();
	bool is_playing() const;
	void step(int p_delta);
	void go_to_start();
	void go_to_end();
	// J / K / L: -1 plays backwards (faster each time), 0 pauses, 1 plays forwards (faster each time).
	void shuttle(int p_direction);
	void set_loop(bool p_loop);
	bool is_loop() const;
	// 1 = full resolution, 2 = half, 4 = quarter.
	void set_preview_divisor(int p_divisor);
	int get_preview_divisor() const;

	// Asks the engine for the current frame again (after the timeline changed).
	void refresh();
	// Called when the project's profile, bin or duration changed.
	void project_changed();
	Ref<Image> get_frame_image() const;

	TetraVideoMonitor();
};
