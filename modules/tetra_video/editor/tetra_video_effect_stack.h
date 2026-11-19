#pragma once

#include "scene/gui/box_container.h"

class Button;
class HBoxContainer;
class Label;
class OptionButton;
class ScrollContainer;
class TetraParamPanel;
class TetraVideoKeyframeStrip;
class TetraVideoProject;
class Timer;
class Tree;
class TreeItem;

// The effects of one timeline clip: a list with an enable box, move and remove
// buttons per effect, and the parameters of the selected effect below it.
// Every change goes to the project. The edits of one slider drag are gathered
// in a single undo step: the step is closed when no value has changed for a
// moment, or by commit_pending() before anything else touches the project.
//
// Parameters MLT can animate get a row of keyframe controls between the list
// and the form: a toggle that makes the parameter follow keyframes, buttons to
// go to the previous or next keyframe and to add or remove one at the
// playhead, and the interpolation of the stretch the playhead is in. While a
// parameter is animated its control in the form shows the value at the
// playhead, and editing it sets the keyframe there, creating one if needed.
// The strip under the list shows the keyframes of the whole clip.
class TetraVideoEffectStack : public VBoxContainer {
	GDCLASS(TetraVideoEffectStack, VBoxContainer);

	enum {
		BUTTON_UP,
		BUTTON_DOWN,
		BUTTON_REMOVE,
	};

	struct KeyframeRow {
		String name;
		bool integer = false;
		Button *toggle = nullptr;
		HBoxContainer *controls = nullptr;
		Button *previous = nullptr;
		Button *key = nullptr;
		Button *next = nullptr;
		OptionButton *type = nullptr;
	};

	Ref<TetraVideoProject> project;
	int clip_id = -1;
	// Timeline frame.
	int playhead = 0;
	Vector<KeyframeRow> keyframe_rows;
	int selected_effect = -1;
	// Clip id and operation ids of what the list shows; a different one means the list must be rebuilt.
	String shown_signature;
	bool updating = false;
	bool group_open = false;

	Label *title = nullptr;
	Tree *tree = nullptr;
	Label *hint = nullptr;
	TetraVideoKeyframeStrip *strip = nullptr;
	ScrollContainer *keyframe_scroll = nullptr;
	VBoxContainer *keyframe_box = nullptr;
	TetraParamPanel *panel = nullptr;
	Timer *group_timer = nullptr;

	Dictionary _operation_of(const Dictionary &p_effect) const;
	void _update();
	void _show_selected_effect();
	void _project_changed();
	void _item_selected();
	void _item_edited();
	void _button_clicked(Object *p_item, int p_column, int p_id, int p_mouse_button);
	void _param_changed(const String &p_name, const Variant &p_value);
	void _build_keyframe_rows(const Dictionary &p_operation);
	void _update_animation();
	int _find_keyframe_row(const String &p_name) const;
	void _keyframe_toggled(bool p_on, const String &p_name);
	void _keyframe_step(const String &p_name, int p_direction);
	void _keyframe_pressed(const String &p_name);
	void _keyframe_type_selected(int p_type, const String &p_name);
	void _strip_seek(int p_frame);

protected:
	static void _bind_methods();

public:
	void set_project(const Ref<TetraVideoProject> &p_project);
	// The timeline clip whose effects are shown; -1 for none.
	void set_clip(int p_clip_id);
	int get_clip() const;
	void select_effect(int p_index);
	int get_selected_effect() const;
	int get_effect_count() const;
	TetraParamPanel *get_param_panel() const;
	// Closes the undo step of a run of parameter edits.
	void commit_pending();

	// The timeline frame the keyframe controls work at.
	void set_playhead(int p_frame);
	int get_playhead() const;
	// The playhead in frames from the start of the shown clip, kept inside it.
	int get_frame_in_clip() const;
	// Makes a parameter of the selected effect follow keyframes, or a constant again.
	bool set_param_animated(const String &p_name, bool p_animated);
	bool is_param_animated(const String &p_name) const;
	PackedStringArray get_animatable_params() const;
	// "toggle", "previous", "key", "next" or "type" of a parameter's keyframe row.
	Control *get_keyframe_control(const String &p_name, const String &p_which) const;
	TetraVideoKeyframeStrip *get_keyframe_strip() const;

	TetraVideoEffectStack();
};
