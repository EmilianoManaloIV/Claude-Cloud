#pragma once

#include "editor/plugins/editor_plugin.h"

class AcceptDialog;
class GridContainer;
class Label;

// The part of Tetra Studio that belongs to no workspace: it adds
// "About Tetra Studio..." to Project > Tools. The dialog names the upstream
// programs Tetra is built from and lists the engines announced through
// TetraEngineRegistry with their version and whether they work.
class TetraEditorPlugin : public EditorPlugin {
	GDCLASS(TetraEditorPlugin, EditorPlugin);

	static TetraEditorPlugin *singleton;

	AcceptDialog *about_dialog = nullptr;
	GridContainer *upstream_grid = nullptr;
	GridContainer *engine_grid = nullptr;
	Label *no_engines_label = nullptr;

	void _update_about();
	Label *_add_cell(GridContainer *p_grid, const String &p_text, bool p_heading = false);

protected:
	static void _bind_methods();

public:
	static TetraEditorPlugin *get_singleton();

	virtual String get_plugin_name() const override;

	void show_about();
	AcceptDialog *get_about_dialog() const;

	TetraEditorPlugin();
	~TetraEditorPlugin();
};
