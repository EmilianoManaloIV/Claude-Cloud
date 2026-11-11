#pragma once

#include "editor/docks/editor_dock.h"

class Label;
class TabContainer;
class TetraBlenderEditorPlugin;
class TetraBlenderModifiersTab;
class TetraBlenderOperatorsTab;
class TetraBlenderRenderTab;
class TextureRect;

// The "Blender" dock: the state of the Blender process on top, and below it
// the Modifiers, Operators and Render tabs. It is a view of
// TetraBlenderEditorPlugin, which does the work.
class TetraBlenderDock : public EditorDock {
	GDCLASS(TetraBlenderDock, EditorDock);

	TetraBlenderEditorPlugin *plugin = nullptr;

	TextureRect *status_icon = nullptr;
	Label *status_label = nullptr;
	Label *hint_label = nullptr;
	TabContainer *tabs = nullptr;
	TetraBlenderModifiersTab *modifiers_tab = nullptr;
	TetraBlenderOperatorsTab *operators_tab = nullptr;
	TetraBlenderRenderTab *render_tab = nullptr;

	double spinner_time = 0.0;

	void _update_status();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// "modifiers", "operators" or "render".
	void show_tab(const String &p_tab);
	String get_current_tab() const;
	TetraBlenderModifiersTab *get_modifiers_tab() const;
	TetraBlenderOperatorsTab *get_operators_tab() const;
	TetraBlenderRenderTab *get_render_tab() const;

	TetraBlenderDock(TetraBlenderEditorPlugin *p_plugin);
};
