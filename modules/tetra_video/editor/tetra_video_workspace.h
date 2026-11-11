#pragma once

#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "scene/gui/box_container.h"

class AcceptDialog;
class Button;
class CheckBox;
class ConfirmationDialog;
class EditorFileDialog;
class HSlider;
class Label;
class MenuButton;
class OptionButton;
class SpinBox;
class TabContainer;
class TetraMltEngine;
class TetraOperationBrowser;
class TetraVideoBin;
class TetraVideoEffectStack;
class TetraVideoMonitor;
class TetraVideoProject;
class TetraVideoRenderDialog;
class TetraVideoThumbnails;
class TetraVideoTimeline;
class Timer;

// The Video workspace: Kdenlive's editing layout inside the editor's main
// screen. Project Bin, Monitor and the Effects / Effect Stack tabs sit above
// the Timeline; a menu bar is on top and a status line at the bottom.
//
// The workspace owns the document (a TetraVideoProject) and the MLT engine
// that shows it. Every edit goes through the project; when the project
// changes, its MLT graph is rebuilt on a worker thread and the monitor shows
// the frame under the playhead again. The bound methods are the scripting
// surface: tool scripts and the screenshot drivers use them.
class TetraVideoWorkspace : public VBoxContainer {
	GDCLASS(TetraVideoWorkspace, VBoxContainer);

public:
	enum Tool {
		TOOL_SELECT,
		TOOL_RAZOR,
	};

private:
	enum MenuOption {
		PROJECT_NEW,
		PROJECT_OPEN,
		PROJECT_SAVE,
		PROJECT_SAVE_AS,
		PROJECT_SETTINGS,
		PROJECT_RENDER,
		EDIT_UNDO,
		EDIT_REDO,
		EDIT_DELETE,
		EDIT_RIPPLE_DELETE,
		EDIT_SELECT_ALL,
		TIMELINE_ADD_VIDEO_TRACK,
		TIMELINE_ADD_AUDIO_TRACK,
		TIMELINE_DELETE_TRACK,
		TIMELINE_SPLIT,
		TIMELINE_INSERT_SPACE,
		TIMELINE_REMOVE_SPACE,
		TIMELINE_ADD_MARKER,
		TIMELINE_DELETE_MARKER,
		TIMELINE_ZONE_IN,
		TIMELINE_ZONE_OUT,
		TIMELINE_ZONE_CLEAR,
	};

	// What waits for the answer of the "discard changes?" question.
	enum PendingAction {
		PENDING_NONE,
		PENDING_NEW,
		PENDING_OPEN,
	};

	static TetraVideoWorkspace *singleton;

	Ref<TetraVideoProject> project;
	Ref<TetraMltEngine> engine;
	Ref<TetraVideoThumbnails> thumbnails;
	String project_path;
	uint64_t saved_revision = 0;
	// Project path the render dialog's output file was last derived from.
	String render_path_source = "-";
	PendingAction pending_action = PENDING_NONE;
	String pending_path;

	// The MLT graph of the timeline is rebuilt here, off the main thread; only the newest request counts.
	Thread graph_thread;
	Mutex graph_mutex;
	Semaphore graph_semaphore;
	SafeFlag graph_quit;
	String graph_pending_xml;
	bool graph_has_pending = false;
	Timer *graph_timer = nullptr;

	MenuButton *project_menu = nullptr;
	MenuButton *edit_menu = nullptr;
	MenuButton *timeline_menu = nullptr;
	Button *undo_button = nullptr;
	Button *redo_button = nullptr;
	Button *render_button = nullptr;
	Label *title_label = nullptr;

	TetraVideoBin *bin = nullptr;
	TetraVideoMonitor *monitor = nullptr;
	TabContainer *side_tabs = nullptr;
	TetraOperationBrowser *effects_browser = nullptr;
	TetraVideoEffectStack *effect_stack = nullptr;

	Button *select_tool_button = nullptr;
	Button *razor_tool_button = nullptr;
	Button *snap_button = nullptr;
	Button *split_button = nullptr;
	Button *delete_button = nullptr;
	Button *ripple_button = nullptr;
	Button *marker_button = nullptr;
	Button *zoom_out_button = nullptr;
	Button *zoom_in_button = nullptr;
	Button *zoom_fit_button = nullptr;
	HSlider *zoom_slider = nullptr;
	TetraVideoTimeline *timeline = nullptr;

	Label *status_label = nullptr;
	Label *info_label = nullptr;
	Timer *status_timer = nullptr;

	EditorFileDialog *open_dialog = nullptr;
	EditorFileDialog *save_dialog = nullptr;
	ConfirmationDialog *discard_dialog = nullptr;
	AcceptDialog *error_dialog = nullptr;
	ConfirmationDialog *settings_dialog = nullptr;
	OptionButton *settings_preset = nullptr;
	SpinBox *settings_width = nullptr;
	SpinBox *settings_height = nullptr;
	SpinBox *settings_fps_num = nullptr;
	SpinBox *settings_fps_den = nullptr;
	ConfirmationDialog *space_dialog = nullptr;
	SpinBox *space_seconds = nullptr;
	CheckBox *space_active_only = nullptr;
	TetraVideoRenderDialog *render_dialog = nullptr;

	static void _graph_thread_main(void *p_self);
	void _run_graph_thread();
	void _rebuild_graph();
	void _graph_rebuilt();

	Ref<TetraVideoProject> _make_default_project() const;
	void _set_project(const Ref<TetraVideoProject> &p_project, const String &p_path);
	void _project_changed();
	void _project_structure_changed();
	void _update_title();
	void _update_info();
	void _update_actions();
	void _update_icons();
	void _update_effects_list();
	void _set_status(const String &p_text, bool p_warning = false);
	void _status_timeout();
	void _show_error(const String &p_text);

	void _menu_option(int p_option);
	void _ask_discard(PendingAction p_action, const String &p_path = String());
	void _discard_confirmed();
	void _open_file_selected(const String &p_path);
	void _save_file_selected(const String &p_path);
	void _settings_preset_selected(int p_index);
	void _settings_confirmed();
	void _space_confirmed();

	void _bin_clip_activated(int p_bin_id);
	void _bin_import_requested(const PackedStringArray &p_paths);
	void _monitor_position_changed(int p_frame);
	void _timeline_seek_requested(int p_frame);
	void _timeline_selection_changed();
	void _timeline_zoom_changed(double p_zoom);
	void _timeline_tool_changed(int p_tool);
	void _timeline_edit_refused(const String &p_message);
	void _timeline_clip_activated(int p_clip_id);
	void _timeline_effect_dropped(int p_clip_id, const String &p_operation_id);
	void _tool_button_pressed(int p_tool);
	void _snap_toggled(bool p_pressed);
	void _zoom_slider_changed(double p_value);
	void _zoom_step(int p_direction);
	void _effect_activated(const String &p_id);
	Variant _effects_get_drag_data(const Point2 &p_point, Control *p_from);
	void _render_finished(bool p_ok, const String &p_message);
	int _add_effect_to_clip(int p_clip_id, const Dictionary &p_operation);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

	// The workspace of the running editor; null outside the editor.
	static TetraVideoWorkspace *get_singleton();

	Ref<TetraVideoProject> get_project() const;
	Ref<TetraMltEngine> get_engine() const;
	TetraVideoBin *get_bin() const;
	TetraVideoMonitor *get_monitor() const;
	TetraVideoTimeline *get_timeline() const;
	TetraOperationBrowser *get_effects_browser() const;
	TetraVideoEffectStack *get_effect_stack() const;
	TetraVideoRenderDialog *get_render_dialog() const;

	// Project file. These act at once, without asking about unsaved changes.
	void new_project();
	Error open_project(const String &p_path);
	Error save_project(const String &p_path = String());
	String get_project_path() const;
	bool is_modified() const;

	// Adds a media file to the bin (or finds it there) and returns its bin id, -1 when it cannot be read.
	int import_file(const String &p_path);
	int add_color_clip(const String &p_name, const Color &p_color, int p_length);
	int add_title_clip(const String &p_name, const String &p_text, int p_length);

	void seek(int p_frame);
	int get_playhead() const;
	void play();
	void pause();
	bool is_playing() const;

	void set_tool(Tool p_tool);
	Tool get_tool() const;
	void select_clip(int p_clip_id, bool p_add = false);
	void clear_selection();
	PackedInt32Array get_selected_clips() const;

	// Adds an effect of the catalog to the selected clips it fits (video effects to clips on video tracks, audio
	// effects to clips on audio tracks). Returns how many clips received it.
	int add_effect_to_selection(const String &p_operation_id);
	bool delete_selection(bool p_ripple = false);
	// Cuts the selected clips under the playhead, or every clip under it when nothing is selected.
	int split_at_playhead();
	bool add_marker(const String &p_text = String());
	bool undo();
	bool redo();

	void show_effects();
	void show_effect_stack();
	void open_render_dialog();
	void open_project_settings();
	void focus_timeline();
	// Closes the undo step of parameter edits that are still being gathered.
	void commit_pending_edits();
	// True while a rebuild of the engine's graph is waiting or running.
	bool is_graph_pending() const;

	TetraVideoWorkspace();
	~TetraVideoWorkspace();
};

VARIANT_ENUM_CAST(TetraVideoWorkspace::Tool);
