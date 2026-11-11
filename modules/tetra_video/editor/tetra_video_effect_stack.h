#pragma once

#include "scene/gui/box_container.h"

class Label;
class TetraParamPanel;
class TetraVideoProject;
class Timer;
class Tree;
class TreeItem;

// The effects of one timeline clip: a list with an enable box, move and remove
// buttons per effect, and the parameters of the selected effect below it.
// Every change goes to the project. The edits of one slider drag are gathered
// in a single undo step: the step is closed when no value has changed for a
// moment, or by commit_pending() before anything else touches the project.
class TetraVideoEffectStack : public VBoxContainer {
	GDCLASS(TetraVideoEffectStack, VBoxContainer);

	enum {
		BUTTON_UP,
		BUTTON_DOWN,
		BUTTON_REMOVE,
	};

	Ref<TetraVideoProject> project;
	int clip_id = -1;
	int selected_effect = -1;
	// Clip id and operation ids of what the list shows; a different one means the list must be rebuilt.
	String shown_signature;
	bool updating = false;
	bool group_open = false;

	Label *title = nullptr;
	Tree *tree = nullptr;
	Label *hint = nullptr;
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

	TetraVideoEffectStack();
};
