#pragma once

#include "tetra/editor/tetra_canvas_view.h"

#include "core/io/image.h"
#include "scene/gui/box_container.h"

class Button;
class EditorFileDialog;
class ImageTexture;
class Label;
class ProgressBar;
class TetraBlenderEditorPlugin;
class TetraParamPanel;

// Shows a rendered picture, zoomable and pannable.
class TetraBlenderRenderView : public TetraCanvasView {
	GDCLASS(TetraBlenderRenderView, TetraCanvasView);

	Ref<ImageTexture> texture;

protected:
	virtual void _canvas_draw() override;

public:
	void set_image(const Ref<Image> &p_image);
	bool has_image() const;

	TetraBlenderRenderView();
};

// The Render tab of the Blender dock: renders the edited scene with Cycles
// through the camera of the 3D viewport, shows the picture and saves it into
// the project.
class TetraBlenderRenderTab : public VBoxContainer {
	GDCLASS(TetraBlenderRenderTab, VBoxContainer);

	TetraBlenderEditorPlugin *plugin = nullptr;

	TetraParamPanel *param_panel = nullptr;
	Button *render_button = nullptr;
	Button *cancel_button = nullptr;
	ProgressBar *progress_bar = nullptr;
	Label *status_label = nullptr;
	TetraBlenderRenderView *view = nullptr;
	Button *save_button = nullptr;
	Button *fit_button = nullptr;
	EditorFileDialog *save_dialog = nullptr;

	void _render_pressed();
	void _cancel_pressed();
	void _save_pressed();
	void _save_file_selected(const String &p_path);
	void _render_started();
	void _render_progress(double p_fraction, int p_sample, int p_samples);
	void _render_finished(bool p_ok, const String &p_message);
	void _set_status(const String &p_text, bool p_error = false);
	void _update_buttons();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Starts a render with the settings of the panel.
	void render();
	TetraParamPanel *get_param_panel() const;
	TetraBlenderRenderView *get_view() const;

	TetraBlenderRenderTab(TetraBlenderEditorPlugin *p_plugin);
};
