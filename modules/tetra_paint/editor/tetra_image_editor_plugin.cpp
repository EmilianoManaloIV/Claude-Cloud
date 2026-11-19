#include "tetra_image_editor_plugin.h"

#include "tetra_image_workspace.h"

#include "core/os/keyboard.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/texture.h"

const Ref<Texture2D> TetraImageEditorPlugin::get_plugin_icon() const {
	return EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("TetraImage"), EditorStringName(EditorIcons));
}

void TetraImageEditorPlugin::make_visible(bool p_visible) {
	workspace->set_visible(p_visible);
}

bool TetraImageEditorPlugin::handles(Object *p_object) const {
	return !TetraImageWorkspace::get_editable_path(p_object).is_empty();
}

void TetraImageEditorPlugin::edit(Object *p_object) {
	// edit(nullptr) only means that something else is being edited now; the
	// open image stays.
	const String path = TetraImageWorkspace::get_editable_path(p_object);
	if (!path.is_empty()) {
		workspace->request_open_file(path);
	}
}

String TetraImageEditorPlugin::get_unsaved_status(const String &p_for_scene) const {
	if (!p_for_scene.is_empty() || !workspace->is_modified()) {
		return String();
	}
	const String path = workspace->get_document_path();
	if (!workspace->can_save_directly()) {
		// "Save" in the editor's dialog calls save_external_data(), which cannot
		// ask for a file name or a format, so the dialog would only come back.
		return vformat(TTR("The image \"%s\" in the Image workspace has unsaved changes, and saving it needs a file name or a choice of format.\nCancel and save it in the Image workspace, or continue without saving it."), path.is_empty() ? TTR("[Untitled]") : path.get_file());
	}
	return vformat(TTR("The image \"%s\" in the Image workspace has unsaved changes. Save it before closing?"), path.is_empty() ? TTR("[Untitled]") : path.get_file());
}

void TetraImageEditorPlugin::save_external_data() {
	// Saving everything also saves the image when that needs no question.
	if (workspace->is_modified() && workspace->can_save_directly()) {
		workspace->save();
	}
}

TetraImageEditorPlugin::TetraImageEditorPlugin() {
	// EditorMainScreen looks up "editor/editor_" + the lower-case plugin name.
	ED_SHORTCUT("editor/editor_image", TTRC("Open Image Workspace"), KeyModifierMask::CTRL | Key::F6);

	workspace = memnew(TetraImageWorkspace);
	workspace->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(workspace);
	workspace->hide();
}
