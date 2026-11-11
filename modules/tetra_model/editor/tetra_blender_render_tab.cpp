#include "tetra_blender_render_tab.h"

#include "tetra_blender_editor_plugin.h"

#include "tetra/editor/tetra_param_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/progress_bar.h"
#include "scene/resources/image_texture.h"

void TetraBlenderRenderView::_canvas_draw() {
	if (texture.is_null()) {
		return;
	}
	// The checkerboard shows through a render with a transparent background.
	draw_checkerboard(Rect2(Point2(), get_content_size()));
	draw_set_transform_to_content();
	draw_texture(texture, Point2());
	draw_reset_transform();
}

void TetraBlenderRenderView::set_image(const Ref<Image> &p_image) {
	if (p_image.is_null()) {
		texture.unref();
		queue_redraw();
		return;
	}
	// The picture is usually shown smaller than it is: mipmaps keep it smooth.
	Ref<Image> display = p_image->duplicate();
	display->generate_mipmaps();
	texture = ImageTexture::create_from_image(display);
	set_content_size(p_image->get_size());
	zoom_to_fit();
	queue_redraw();
}

bool TetraBlenderRenderView::has_image() const {
	return texture.is_valid();
}

TetraBlenderRenderView::TetraBlenderRenderView() {
	set_texture_filter(TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
	set_clip_contents(true);
}

void TetraBlenderRenderTab::_render_pressed() {
	render();
}

void TetraBlenderRenderTab::render() {
	if (!plugin->render_current_view(param_panel->get_values())) {
		_set_status(plugin->get_last_error(), true);
	}
}

void TetraBlenderRenderTab::_cancel_pressed() {
	plugin->cancel_render();
}

void TetraBlenderRenderTab::_save_pressed() {
	if (save_dialog->get_current_file().is_empty()) {
		save_dialog->set_current_file("render.png");
	}
	save_dialog->popup_file_dialog();
}

void TetraBlenderRenderTab::_save_file_selected(const String &p_path) {
	const Error error = plugin->save_render(p_path);
	if (error == OK) {
		_set_status(vformat(TTR("Saved %s"), p_path));
	} else {
		_set_status(vformat(TTR("Could not write %s."), p_path), true);
	}
}

void TetraBlenderRenderTab::_render_started() {
	progress_bar->set_value(0.0);
	_set_status(TTR("Sending the scene to Blender…"));
	_update_buttons();
}

void TetraBlenderRenderTab::_render_progress(double p_fraction, int p_sample, int p_samples) {
	progress_bar->set_value(p_fraction);
	_set_status(vformat(TTR("Cycles: sample %d of %d"), p_sample, p_samples));
}

void TetraBlenderRenderTab::_render_finished(bool p_ok, const String &p_message) {
	if (p_ok) {
		view->set_image(plugin->get_last_render());
		const Dictionary info = plugin->get_last_render_info();
		_set_status(vformat(TTR("%d × %d, %d samples%s, %s s"), int(info.get("width", 0)), int(info.get("height", 0)), int(info.get("samples", 0)), bool(info.get("denoised", false)) ? TTR(", denoised") : String(), String::num(double(info.get("render_time", 0.0)), 1)));
	} else {
		_set_status(p_message.is_empty() ? TTR("The render failed.") : p_message, true);
	}
	_update_buttons();
}

void TetraBlenderRenderTab::_set_status(const String &p_text, bool p_error) {
	status_label->set_text(p_text);
	status_label->set_tooltip_text(p_text);
	if (p_error && is_inside_tree()) {
		status_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
	} else {
		status_label->remove_theme_color_override(SceneStringName(font_color));
	}
}

void TetraBlenderRenderTab::_update_buttons() {
	const bool rendering = plugin->is_rendering();
	render_button->set_disabled(rendering);
	cancel_button->set_visible(rendering);
	progress_bar->set_visible(rendering);
	save_button->set_disabled(plugin->get_last_render().is_null());
	fit_button->set_disabled(!view->has_image());
}

void TetraBlenderRenderTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			render_button->set_button_icon(get_editor_theme_icon(SNAME("Camera3D")));
			save_button->set_button_icon(get_editor_theme_icon(SNAME("Save")));
		} break;
	}
}

TetraParamPanel *TetraBlenderRenderTab::get_param_panel() const {
	return param_panel;
}

TetraBlenderRenderView *TetraBlenderRenderTab::get_view() const {
	return view;
}

void TetraBlenderRenderTab::_bind_methods() {
	ClassDB::bind_method(D_METHOD("render"), &TetraBlenderRenderTab::render);
	ClassDB::bind_method(D_METHOD("get_param_panel"), &TetraBlenderRenderTab::get_param_panel);
	ClassDB::bind_method(D_METHOD("get_view"), &TetraBlenderRenderTab::get_view);
}

TetraBlenderRenderTab::TetraBlenderRenderTab(TetraBlenderEditorPlugin *p_plugin) {
	plugin = p_plugin;

	param_panel = memnew(TetraParamPanel);
	// The settings are few: they are shown in full and the picture gets the rest.
	param_panel->set_scrollable(false);
	param_panel->set_operation(plugin->get_render_schema());
	add_child(param_panel);

	HBoxContainer *buttons = memnew(HBoxContainer);
	add_child(buttons);
	render_button = memnew(Button);
	render_button->set_text(TTRC("Render"));
	render_button->set_h_size_flags(SIZE_EXPAND_FILL);
	render_button->set_tooltip_text(TTRC("Render the edited scene with Cycles, Blender's path tracer."));
	render_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderRenderTab::_render_pressed));
	buttons->add_child(render_button);
	cancel_button = memnew(Button);
	cancel_button->set_text(TTRC("Cancel"));
	cancel_button->set_tooltip_text(TTRC("Stop the render. Blender is restarted for the next request."));
	cancel_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderRenderTab::_cancel_pressed));
	buttons->add_child(cancel_button);

	progress_bar = memnew(ProgressBar);
	progress_bar->set_min(0.0);
	progress_bar->set_max(1.0);
	progress_bar->set_step(0.001);
	add_child(progress_bar);

	status_label = memnew(Label);
	status_label->set_text(TTRC("Nothing rendered yet."));
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_label->set_mouse_filter(MOUSE_FILTER_PASS);
	add_child(status_label);

	view = memnew(TetraBlenderRenderView);
	view->set_v_size_flags(SIZE_EXPAND_FILL);
	view->set_custom_minimum_size(Size2(0, 120) * EDSCALE);
	add_child(view);

	HBoxContainer *image_buttons = memnew(HBoxContainer);
	add_child(image_buttons);
	save_button = memnew(Button);
	save_button->set_text(TTRC("Save to Project..."));
	save_button->set_h_size_flags(SIZE_EXPAND_FILL);
	save_button->set_tooltip_text(TTRC("Save the picture as a PNG inside the project."));
	save_button->connect(SceneStringName(pressed), callable_mp(this, &TetraBlenderRenderTab::_save_pressed));
	image_buttons->add_child(save_button);
	fit_button = memnew(Button);
	fit_button->set_text(TTRC("Fit"));
	fit_button->set_tooltip_text(TTRC("Show the whole picture. The wheel zooms, the middle button pans."));
	fit_button->connect(SceneStringName(pressed), callable_mp((TetraCanvasView *)view, &TetraCanvasView::zoom_to_fit));
	image_buttons->add_child(fit_button);

	save_dialog = memnew(EditorFileDialog);
	save_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	save_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	save_dialog->set_title(TTRC("Save Render"));
	save_dialog->add_filter("*.png", TTRC("PNG Image"));
	save_dialog->connect("file_selected", callable_mp(this, &TetraBlenderRenderTab::_save_file_selected));
	add_child(save_dialog);

	plugin->connect("render_started", callable_mp(this, &TetraBlenderRenderTab::_render_started));
	plugin->connect("render_progress", callable_mp(this, &TetraBlenderRenderTab::_render_progress));
	plugin->connect("render_finished", callable_mp(this, &TetraBlenderRenderTab::_render_finished));
	_update_buttons();
}
