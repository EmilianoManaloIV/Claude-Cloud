#pragma once

#include "editor/plugins/editor_plugin.h"

class TetraImageWorkspace;

// The "Image" main screen: Tetra's GIMP. The plugin owns the workspace and
// routes image files from the FileSystem dock into it.
class TetraImageEditorPlugin : public EditorPlugin {
	GDCLASS(TetraImageEditorPlugin, EditorPlugin);

	TetraImageWorkspace *workspace = nullptr;

public:
	virtual String get_plugin_name() const override { return TTRC("Image"); }
	virtual const Ref<Texture2D> get_plugin_icon() const override;
	virtual bool has_main_screen() const override { return true; }
	virtual void make_visible(bool p_visible) override;
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual String get_unsaved_status(const String &p_for_scene = "") const override;
	virtual void save_external_data() override;

	TetraImageWorkspace *get_workspace() const { return workspace; }

	TetraImageEditorPlugin();
};
