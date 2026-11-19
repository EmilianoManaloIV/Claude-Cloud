#pragma once

#include "editor/plugins/editor_plugin.h"

class TetraVideoWorkspace;

// Adds the "Video" workspace to the editor's main screen, next to 2D, 3D and
// Script. The workspace itself is a TetraVideoWorkspace; this class connects
// it to the editor: the main-screen button, files opened from the FileSystem
// dock, the "unsaved changes" question on quit and the editor layout.
class TetraVideoEditorPlugin : public EditorPlugin {
	GDCLASS(TetraVideoEditorPlugin, EditorPlugin);

	static TetraVideoEditorPlugin *singleton;

	TetraVideoWorkspace *workspace = nullptr;

protected:
	static void _bind_methods();

public:
	// The plugin of the running editor; null outside the editor.
	static TetraVideoEditorPlugin *get_singleton();

	TetraVideoWorkspace *get_workspace() const;

	virtual String get_plugin_name() const override;
	virtual const Ref<Texture2D> get_plugin_icon() const override;
	virtual bool has_main_screen() const override { return true; }
	virtual void make_visible(bool p_visible) override;
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual String get_unsaved_status(const String &p_for_scene = "") const override;
	virtual void save_external_data() override;
	virtual void apply_changes() override;
	virtual void set_window_layout(Ref<ConfigFile> p_layout) override;
	virtual void get_window_layout(Ref<ConfigFile> p_layout) override;

	TetraVideoEditorPlugin();
	~TetraVideoEditorPlugin();
};
