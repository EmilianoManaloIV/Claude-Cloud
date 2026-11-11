#pragma once

#include "scene/gui/dialogs.h"

class Button;
class EditorFileDialog;
class Label;
class LineEdit;
class OptionButton;
class ProgressBar;
class SpinBox;
class TetraVideoProject;
class TetraVideoRenderer;
class Tree;

// The Render dialog: the encoding presets by group, the output file, the range
// and the size, and the progress of the running render with a way to cancel
// it. Closing the dialog does not stop a render; it goes on in the background
// and render_finished reports its end.
class TetraVideoRenderDialog : public ConfirmationDialog {
	GDCLASS(TetraVideoRenderDialog, ConfirmationDialog);

public:
	enum RangeMode {
		RANGE_PROJECT,
		RANGE_ZONE,
	};

private:
	Ref<TetraVideoProject> project;
	Ref<TetraVideoRenderer> renderer;
	String preset_id = "tetra/mp4";
	bool updating = false;

	Tree *preset_tree = nullptr;
	Label *preset_label = nullptr;
	LineEdit *path_edit = nullptr;
	Button *browse_button = nullptr;
	OptionButton *range_option = nullptr;
	SpinBox *in_spin = nullptr;
	SpinBox *out_spin = nullptr;
	OptionButton *scale_option = nullptr;
	ProgressBar *progress_bar = nullptr;
	Button *cancel_render_button = nullptr;
	Label *status_label = nullptr;
	EditorFileDialog *file_dialog = nullptr;

	void _update_presets();
	void _update_preset_label();
	void _update_state();
	void _apply_extension();
	void _preset_selected();
	void _range_selected(int p_index);
	void _browse_pressed();
	void _file_selected(const String &p_path);
	void _render_progress(int p_frame, double p_percent);
	void _render_finished(bool p_ok, const String &p_message);

protected:
	virtual void ok_pressed() override;
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Opens the dialog for a project. The zone (inclusive frames, -1 when unset) is offered as the range to render.
	void popup_render(const Ref<TetraVideoProject> &p_project, int p_zone_in = -1, int p_zone_out = -1);

	void set_preset(const String &p_preset_id);
	String get_preset() const;
	void set_output_path(const String &p_path);
	String get_output_path() const;
	void set_range_mode(RangeMode p_mode);
	RangeMode get_range_mode() const;
	void set_range(int p_in, int p_out);
	// 1 renders at the profile's size, 2 at half, 4 at a quarter.
	void set_scale_divisor(int p_divisor);
	int get_scale_divisor() const;

	Error start_render();
	void cancel_render();
	bool is_rendering() const;
	Ref<TetraVideoRenderer> get_renderer() const;

	TetraVideoRenderDialog();
};

VARIANT_ENUM_CAST(TetraVideoRenderDialog::RangeMode);
