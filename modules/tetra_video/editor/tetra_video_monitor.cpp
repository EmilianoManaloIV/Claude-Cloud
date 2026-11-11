#include "tetra_video_monitor.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_project.h"
#include "tetra_video_timecode.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/resources/image_texture.h"

Ref<TetraMltEngine> TetraVideoMonitor::_active_engine() const {
	return clip_mode ? clip_engine : project_engine;
}

Ref<TetraVideoProject> TetraVideoMonitor::_active_project() const {
	return clip_mode ? clip_project : project;
}

int TetraVideoMonitor::_active_length() const {
	const Ref<TetraVideoProject> active = _active_project();
	return active.is_valid() ? active->get_duration() : 0;
}

void TetraVideoMonitor::_set_active_position(int p_frame) {
	if (clip_mode) {
		clip_position = p_frame;
	} else {
		project_position = p_frame;
	}
}

void TetraVideoMonitor::_apply_preview_size() {
	if (project.is_null()) {
		return;
	}
	const Vector2i size(MAX(2, project->get_width() / preview_divisor), MAX(2, project->get_height() / preview_divisor));
	if (project_engine.is_valid()) {
		project_engine->set_preview_size(size);
	}
	clip_engine->set_preview_size(size);
}

void TetraVideoMonitor::_update_timecode() {
	const double fps = project.is_valid() ? project->get_fps() : 25.0;
	if (!timecode_edit->has_focus()) {
		timecode_edit->set_text(TetraVideoTimecode::from_frames(get_position(), fps));
	}
	duration_label->set_text("/ " + TetraVideoTimecode::from_frames(_active_length(), fps));
}

void TetraVideoMonitor::_update_icons() {
	if (!is_inside_tree()) {
		return;
	}
	start_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoToStart")));
	step_back_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoStepBack")));
	stop_button->set_button_icon(get_editor_theme_icon(SNAME("Stop")));
	step_forward_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoStepForward")));
	end_button->set_button_icon(get_editor_theme_icon(SNAME("TetraVideoToEnd")));
	loop_button->set_button_icon(get_editor_theme_icon(SNAME("Loop")));
	_update_transport();
}

void TetraVideoMonitor::_update_transport() {
	if (!is_inside_tree()) {
		return;
	}
	const bool playing = is_playing();
	play_button->set_button_icon(get_editor_theme_icon(playing ? SNAME("Pause") : SNAME("Play")));
	play_button->set_tooltip_text(playing ? TTR("Pause (Space, K)") : TTR("Play (Space, L)"));
	project_mode_button->set_pressed_no_signal(!clip_mode);
	clip_mode_button->set_pressed_no_signal(clip_mode);
	clip_mode_button->set_disabled(clip_bin_id < 0);
	if (clip_mode && clip_project.is_valid() && clip_project->get_bin_clip_ids().size() > 0) {
		source_label->set_text(String(clip_project->get_bin_clip(clip_project->get_bin_clip_ids()[0])["name"]));
	} else {
		source_label->set_text(String());
	}
}

void TetraVideoMonitor::_draw_view() {
	const Size2 size = view->get_size();
	view->draw_rect(Rect2(Point2(), size), Color(0, 0, 0));
	if (has_frame && texture.is_valid() && project.is_valid()) {
		// The picture may be a reduced preview; its shape on screen is always the profile's.
		const real_t aspect = real_t(project->get_width()) / real_t(MAX(1, project->get_height()));
		Size2 fitted(size.x, size.x / aspect);
		if (fitted.y > size.y) {
			fitted = Size2(size.y * aspect, size.y);
		}
		view->draw_texture_rect(texture, Rect2(((size - fitted) * 0.5).floor(), fitted.floor()));
		return;
	}
	String hint;
	if (!TetraMltEngine::is_available()) {
		hint = TTR("The MLT engine is not available in this build.");
	} else if (_active_length() == 0) {
		hint = clip_mode ? TTR("This clip has no frames.") : TTR("The timeline is empty.\nDrag clips from the Project Bin onto a track.");
	}
	if (!hint.is_empty()) {
		const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
		const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
		const PackedStringArray lines = hint.split("\n");
		const real_t line_height = font->get_height(font_size);
		real_t y = (size.y - line_height * lines.size()) * 0.5 + font->get_ascent(font_size);
		for (const String &line : lines) {
			view->draw_string(font, Point2(0, y), line, HORIZONTAL_ALIGNMENT_CENTER, size.x, font_size, Color(1, 1, 1, 0.45));
			y += line_height;
		}
	}
}

void TetraVideoMonitor::_engine_frame_ready(int p_frame, bool p_from_clip_engine) {
	if (p_from_clip_engine != clip_mode) {
		return;
	}
	const Ref<TetraMltEngine> engine = _active_engine();
	const Ref<Image> image = engine->get_current_frame_image();
	if (image.is_null() || image->is_empty() || _active_length() == 0) {
		return;
	}
	if (texture.is_valid() && texture->get_size() == Size2(image->get_size()) && texture->get_format() == image->get_format()) {
		texture->update(image);
	} else {
		texture = ImageTexture::create_from_image(image);
	}
	has_frame = true;
	view->queue_redraw();
	if (engine->is_playing()) {
		// While paused the position is whatever was asked for last; a frame of an earlier seek must not move it back.
		_set_active_position(p_frame);
		if (!clip_mode) {
			emit_signal(SNAME("position_changed"), p_frame);
		}
	}
	_update_timecode();
}

void TetraVideoMonitor::_engine_finished(bool p_from_clip_engine) {
	if (p_from_clip_engine != clip_mode) {
		return;
	}
	const Ref<TetraMltEngine> engine = _active_engine();
	_set_active_position(engine->get_position());
	if (!clip_mode) {
		emit_signal(SNAME("position_changed"), project_position);
	}
	_update_transport();
	_update_timecode();
}

void TetraVideoMonitor::_mode_pressed(bool p_clip_mode) {
	if (p_clip_mode && clip_bin_id >= 0) {
		show_clip(clip_bin_id);
	} else {
		show_project();
	}
}

void TetraVideoMonitor::_loop_toggled(bool p_pressed) {
	set_loop(p_pressed);
}

void TetraVideoMonitor::_timecode_submitted(const String &p_text) {
	const int frame = TetraVideoTimecode::to_frames(p_text, project.is_valid() ? project->get_fps() : 25.0);
	timecode_edit->release_focus();
	if (frame >= 0) {
		seek(frame);
	}
	_update_timecode();
}

void TetraVideoMonitor::_timecode_focus_exited() {
	_update_timecode();
}

void TetraVideoMonitor::_preview_selected(int p_index) {
	set_preview_divisor(preview_option->get_item_id(p_index));
}

void TetraVideoMonitor::_step_pressed(int p_delta) {
	step(p_delta);
}

void TetraVideoMonitor::set_project(const Ref<TetraVideoProject> &p_project) {
	pause();
	project = p_project;
	clip_engine->pause();
	clip_project.unref();
	clip_bin_id = -1;
	clip_mode = false;
	project_position = 0;
	clip_position = 0;
	has_frame = false;
	_apply_preview_size();
	_update_transport();
	_update_timecode();
	view->queue_redraw();
}

void TetraVideoMonitor::set_project_engine(const Ref<TetraMltEngine> &p_engine) {
	if (project_engine.is_valid()) {
		project_engine->disconnect("frame_ready", callable_mp(this, &TetraVideoMonitor::_engine_frame_ready));
		project_engine->disconnect("playback_finished", callable_mp(this, &TetraVideoMonitor::_engine_finished));
	}
	project_engine = p_engine;
	if (project_engine.is_valid()) {
		project_engine->connect("frame_ready", callable_mp(this, &TetraVideoMonitor::_engine_frame_ready).bind(false));
		project_engine->connect("playback_finished", callable_mp(this, &TetraVideoMonitor::_engine_finished).bind(false));
		project_engine->set_loop(loop);
	}
	_apply_preview_size();
}

Ref<TetraMltEngine> TetraVideoMonitor::get_clip_engine() const {
	return clip_engine;
}

void TetraVideoMonitor::show_clip(int p_bin_id) {
	if (project.is_null() || !project->has_bin_clip(p_bin_id)) {
		return;
	}
	pause();
	const Dictionary bin = project->get_bin_clip(p_bin_id);
	const bool with_video = bin["has_video"];
	const bool with_audio = bin["has_audio"];

	Ref<TetraVideoProject> single;
	single.instantiate();
	single->set_profile(project->get_width(), project->get_height(), project->get_fps_num(), project->get_fps_den(), project->get_sample_rate());
	const int id = single->add_bin_clip(TetraVideoProject::ClipKind(int(bin["kind"])), bin["name"], bin["source"], bin["length"], with_video, with_audio, bin["properties"]);
	// Audio below video, so that an AV clip put on the video track finds its paired audio track.
	if (with_audio) {
		single->add_track(TetraVideoProject::TRACK_AUDIO);
	}
	if (with_video) {
		single->add_track(TetraVideoProject::TRACK_VIDEO);
	}
	if (id < 0 || single->insert_clip(id, single->get_track_count() - 1, 0) < 0) {
		return;
	}
	clip_project = single;
	clip_bin_id = p_bin_id;
	clip_mode = true;
	clip_position = 0;
	has_frame = false;
	clip_engine->set_project(clip_project);
	clip_engine->seek(0);
	_update_transport();
	_update_timecode();
	view->queue_redraw();
	emit_signal(SNAME("mode_changed"), true);
}

void TetraVideoMonitor::show_project() {
	if (clip_mode) {
		clip_engine->pause();
		clip_mode = false;
		has_frame = false;
		emit_signal(SNAME("mode_changed"), false);
	}
	_update_transport();
	_update_timecode();
	view->queue_redraw();
	refresh();
}

bool TetraVideoMonitor::is_clip_mode() const {
	return clip_mode;
}

int TetraVideoMonitor::get_clip_bin_id() const {
	return clip_bin_id;
}

void TetraVideoMonitor::seek(int p_frame) {
	const int frame = CLAMP(p_frame, 0, MAX(0, _active_length() - 1));
	_set_active_position(frame);
	const Ref<TetraMltEngine> engine = _active_engine();
	if (engine.is_valid() && _active_length() > 0) {
		engine->seek(frame);
	}
	if (!clip_mode) {
		emit_signal(SNAME("position_changed"), frame);
	}
	_update_timecode();
}

int TetraVideoMonitor::get_position() const {
	return clip_mode ? clip_position : project_position;
}

void TetraVideoMonitor::play() {
	const Ref<TetraMltEngine> engine = _active_engine();
	if (engine.is_null() || _active_length() == 0) {
		return;
	}
	engine->set_speed(1.0);
	engine->play();
	_update_transport();
}

void TetraVideoMonitor::pause() {
	const Ref<TetraMltEngine> engine = _active_engine();
	if (engine.is_null()) {
		return;
	}
	if (engine->is_playing()) {
		engine->pause();
		_set_active_position(engine->get_position());
		if (!clip_mode) {
			emit_signal(SNAME("position_changed"), project_position);
		}
	}
	_update_transport();
	_update_timecode();
}

void TetraVideoMonitor::toggle_play() {
	if (is_playing()) {
		pause();
	} else {
		play();
	}
}

void TetraVideoMonitor::stop() {
	pause();
	seek(0);
}

bool TetraVideoMonitor::is_playing() const {
	const Ref<TetraMltEngine> engine = _active_engine();
	return engine.is_valid() && engine->is_playing();
}

void TetraVideoMonitor::step(int p_delta) {
	pause();
	seek(get_position() + p_delta);
}

void TetraVideoMonitor::go_to_start() {
	pause();
	seek(0);
}

void TetraVideoMonitor::go_to_end() {
	pause();
	seek(_active_length() - 1);
}

void TetraVideoMonitor::shuttle(int p_direction) {
	const Ref<TetraMltEngine> engine = _active_engine();
	if (engine.is_null() || _active_length() == 0) {
		return;
	}
	if (p_direction == 0) {
		pause();
		return;
	}
	// Pressing the same direction again doubles the speed, up to 8x, as in Kdenlive.
	double speed = p_direction > 0 ? 1.0 : -1.0;
	if (engine->is_playing() && engine->get_speed() * speed > 0.0) {
		speed = CLAMP(engine->get_speed() * 2.0, -8.0, 8.0);
	}
	engine->set_speed(speed);
	engine->play();
	_update_transport();
}

void TetraVideoMonitor::set_loop(bool p_loop) {
	loop = p_loop;
	if (project_engine.is_valid()) {
		project_engine->set_loop(loop);
	}
	clip_engine->set_loop(loop);
	loop_button->set_pressed_no_signal(loop);
}

bool TetraVideoMonitor::is_loop() const {
	return loop;
}

void TetraVideoMonitor::set_preview_divisor(int p_divisor) {
	const int divisor = p_divisor >= 4 ? 4 : (p_divisor >= 2 ? 2 : 1);
	if (divisor == preview_divisor) {
		return;
	}
	preview_divisor = divisor;
	preview_option->select(preview_option->get_item_index(divisor));
	_apply_preview_size();
	if (!is_playing()) {
		seek(get_position());
	}
}

int TetraVideoMonitor::get_preview_divisor() const {
	return preview_divisor;
}

void TetraVideoMonitor::refresh() {
	if (_active_length() == 0) {
		has_frame = false;
		view->queue_redraw();
		_update_timecode();
		return;
	}
	const Ref<TetraMltEngine> engine = _active_engine();
	if (engine.is_valid() && !engine->is_playing()) {
		const int frame = MIN(get_position(), _active_length() - 1);
		_set_active_position(frame);
		engine->seek(frame);
		if (!clip_mode) {
			emit_signal(SNAME("position_changed"), frame);
		}
	}
	_update_timecode();
}

void TetraVideoMonitor::project_changed() {
	if (clip_bin_id >= 0 && (project.is_null() || !project->has_bin_clip(clip_bin_id))) {
		clip_bin_id = -1;
		if (clip_mode) {
			show_project();
		}
	}
	_apply_preview_size();
	_update_transport();
	_update_timecode();
	view->queue_redraw();
}

Ref<Image> TetraVideoMonitor::get_frame_image() const {
	return has_frame && texture.is_valid() ? texture->get_image() : Ref<Image>();
}

void TetraVideoMonitor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			_update_icons();
		} break;
	}
}

void TetraVideoMonitor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("show_clip", "bin_id"), &TetraVideoMonitor::show_clip);
	ClassDB::bind_method(D_METHOD("show_project"), &TetraVideoMonitor::show_project);
	ClassDB::bind_method(D_METHOD("is_clip_mode"), &TetraVideoMonitor::is_clip_mode);
	ClassDB::bind_method(D_METHOD("get_clip_bin_id"), &TetraVideoMonitor::get_clip_bin_id);
	ClassDB::bind_method(D_METHOD("get_clip_engine"), &TetraVideoMonitor::get_clip_engine);
	ClassDB::bind_method(D_METHOD("seek", "frame"), &TetraVideoMonitor::seek);
	ClassDB::bind_method(D_METHOD("get_position"), &TetraVideoMonitor::get_position);
	ClassDB::bind_method(D_METHOD("play"), &TetraVideoMonitor::play);
	ClassDB::bind_method(D_METHOD("pause"), &TetraVideoMonitor::pause);
	ClassDB::bind_method(D_METHOD("toggle_play"), &TetraVideoMonitor::toggle_play);
	ClassDB::bind_method(D_METHOD("stop"), &TetraVideoMonitor::stop);
	ClassDB::bind_method(D_METHOD("is_playing"), &TetraVideoMonitor::is_playing);
	ClassDB::bind_method(D_METHOD("step", "delta"), &TetraVideoMonitor::step);
	ClassDB::bind_method(D_METHOD("go_to_start"), &TetraVideoMonitor::go_to_start);
	ClassDB::bind_method(D_METHOD("go_to_end"), &TetraVideoMonitor::go_to_end);
	ClassDB::bind_method(D_METHOD("shuttle", "direction"), &TetraVideoMonitor::shuttle);
	ClassDB::bind_method(D_METHOD("set_loop", "loop"), &TetraVideoMonitor::set_loop);
	ClassDB::bind_method(D_METHOD("is_loop"), &TetraVideoMonitor::is_loop);
	ClassDB::bind_method(D_METHOD("set_preview_divisor", "divisor"), &TetraVideoMonitor::set_preview_divisor);
	ClassDB::bind_method(D_METHOD("get_preview_divisor"), &TetraVideoMonitor::get_preview_divisor);
	ClassDB::bind_method(D_METHOD("refresh"), &TetraVideoMonitor::refresh);
	ClassDB::bind_method(D_METHOD("get_frame_image"), &TetraVideoMonitor::get_frame_image);

	ADD_SIGNAL(MethodInfo("position_changed", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("mode_changed", PropertyInfo(Variant::BOOL, "clip_mode")));
}

TetraVideoMonitor::TetraVideoMonitor() {
	clip_engine.instantiate();
	clip_engine->connect("frame_ready", callable_mp(this, &TetraVideoMonitor::_engine_frame_ready).bind(true));
	clip_engine->connect("playback_finished", callable_mp(this, &TetraVideoMonitor::_engine_finished).bind(true));

	HBoxContainer *header = memnew(HBoxContainer);
	add_child(header);

	project_mode_button = memnew(Button(TTR("Project Monitor")));
	project_mode_button->set_toggle_mode(true);
	project_mode_button->set_pressed(true);
	project_mode_button->set_theme_type_variation(SceneStringName(FlatButton));
	project_mode_button->set_tooltip_text(TTR("Show the timeline at the playhead."));
	project_mode_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::_mode_pressed).bind(false));
	header->add_child(project_mode_button);

	clip_mode_button = memnew(Button(TTR("Clip Monitor")));
	clip_mode_button->set_toggle_mode(true);
	clip_mode_button->set_disabled(true);
	clip_mode_button->set_theme_type_variation(SceneStringName(FlatButton));
	clip_mode_button->set_tooltip_text(TTR("Show the bin clip that was opened last (double-click a clip in the Project Bin)."));
	clip_mode_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::_mode_pressed).bind(true));
	header->add_child(clip_mode_button);

	source_label = memnew(Label);
	source_label->set_h_size_flags(SIZE_EXPAND_FILL);
	source_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	source_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	source_label->set_modulate(Color(1, 1, 1, 0.7));
	header->add_child(source_label);

	view = memnew(Control);
	view->set_v_size_flags(SIZE_EXPAND_FILL);
	view->set_custom_minimum_size(Size2(160, 90) * EDSCALE);
	view->set_clip_contents(true);
	view->connect(SceneStringName(draw), callable_mp(this, &TetraVideoMonitor::_draw_view));
	add_child(view);

	HBoxContainer *transport = memnew(HBoxContainer);
	add_child(transport);

	start_button = memnew(Button);
	start_button->set_theme_type_variation(SceneStringName(FlatButton));
	start_button->set_tooltip_text(TTR("Go to Start (Home)"));
	start_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::go_to_start));
	transport->add_child(start_button);

	step_back_button = memnew(Button);
	step_back_button->set_theme_type_variation(SceneStringName(FlatButton));
	step_back_button->set_tooltip_text(TTR("Previous Frame (Left)"));
	step_back_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::_step_pressed).bind(-1));
	transport->add_child(step_back_button);

	play_button = memnew(Button);
	play_button->set_theme_type_variation(SceneStringName(FlatButton));
	play_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::toggle_play));
	transport->add_child(play_button);

	stop_button = memnew(Button);
	stop_button->set_theme_type_variation(SceneStringName(FlatButton));
	stop_button->set_tooltip_text(TTR("Stop and return to the start"));
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::stop));
	transport->add_child(stop_button);

	step_forward_button = memnew(Button);
	step_forward_button->set_theme_type_variation(SceneStringName(FlatButton));
	step_forward_button->set_tooltip_text(TTR("Next Frame (Right)"));
	step_forward_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::_step_pressed).bind(1));
	transport->add_child(step_forward_button);

	end_button = memnew(Button);
	end_button->set_theme_type_variation(SceneStringName(FlatButton));
	end_button->set_tooltip_text(TTR("Go to End (End)"));
	end_button->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoMonitor::go_to_end));
	transport->add_child(end_button);

	loop_button = memnew(Button);
	loop_button->set_toggle_mode(true);
	loop_button->set_theme_type_variation(SceneStringName(FlatButton));
	loop_button->set_tooltip_text(TTR("Loop Playback"));
	loop_button->connect(SceneStringName(toggled), callable_mp(this, &TetraVideoMonitor::_loop_toggled));
	transport->add_child(loop_button);

	transport->add_child(memnew(VSeparator));

	timecode_edit = memnew(LineEdit);
	timecode_edit->set_text("00:00:00:00");
	timecode_edit->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	timecode_edit->set_custom_minimum_size(Size2(96, 0) * EDSCALE);
	timecode_edit->set_select_all_on_focus(true);
	timecode_edit->set_tooltip_text(TTR("Position (HH:MM:SS:FF). Type a timecode or a frame number and press Enter to go there."));
	timecode_edit->connect(SceneStringName(text_submitted), callable_mp(this, &TetraVideoMonitor::_timecode_submitted));
	timecode_edit->connect(SceneStringName(focus_exited), callable_mp(this, &TetraVideoMonitor::_timecode_focus_exited));
	transport->add_child(timecode_edit);

	duration_label = memnew(Label);
	duration_label->set_text("/ 00:00:00:00");
	duration_label->set_modulate(Color(1, 1, 1, 0.7));
	duration_label->set_h_size_flags(SIZE_EXPAND_FILL);
	duration_label->set_clip_text(true);
	transport->add_child(duration_label);

	preview_option = memnew(OptionButton);
	preview_option->set_flat(true);
	preview_option->add_item(TTR("Full"), 1);
	preview_option->add_item(TTR("1/2"), 2);
	preview_option->add_item(TTR("1/4"), 4);
	preview_option->select(1);
	preview_option->set_tooltip_text(TTR("Preview Resolution"));
	preview_option->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoMonitor::_preview_selected));
	transport->add_child(preview_option);
}
