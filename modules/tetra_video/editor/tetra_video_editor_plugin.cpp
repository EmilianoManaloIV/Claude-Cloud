#include "tetra_video_editor_plugin.h"

#include "../tetra_video_file.h"
#include "../tetra_video_project.h"
#include "tetra_video_workspace.h"

#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/video_stream.h"

TetraVideoEditorPlugin *TetraVideoEditorPlugin::singleton = nullptr;

// The file behind a resource this plugin opens, or "" for anything else.
static String _video_file_of(Object *p_object) {
	const Resource *resource = Object::cast_to<Resource>(p_object);
	if (!resource) {
		return String();
	}
	String path;
	if (Object::cast_to<TetraVideoProjectFile>(p_object) || Object::cast_to<TetraVideoMediaFile>(p_object)) {
		path = resource->get_path();
	} else if (const VideoStream *stream = Object::cast_to<VideoStream>(p_object)) {
		// A stream of the project (Ogg Theora): the file it was loaded from.
		path = resource->get_path().is_resource_file() ? resource->get_path() : stream->get_file();
	}
	return path.is_resource_file() || path.is_absolute_path() ? path : String();
}

TetraVideoEditorPlugin *TetraVideoEditorPlugin::get_singleton() {
	return singleton;
}

TetraVideoWorkspace *TetraVideoEditorPlugin::get_workspace() const {
	return workspace;
}

String TetraVideoEditorPlugin::get_plugin_name() const {
	return TTRC("Video");
}

const Ref<Texture2D> TetraVideoEditorPlugin::get_plugin_icon() const {
	return EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("TetraVideo"), EditorStringName(EditorIcons));
}

void TetraVideoEditorPlugin::make_visible(bool p_visible) {
	workspace->set_visible(p_visible);
	if (p_visible) {
		// The transport and tool keys work at once, without a click into the timeline first.
		callable_mp(workspace, &TetraVideoWorkspace::focus_timeline).call_deferred();
	}
}

bool TetraVideoEditorPlugin::handles(Object *p_object) const {
	return !_video_file_of(p_object).is_empty();
}

void TetraVideoEditorPlugin::edit(Object *p_object) {
	// edit(nullptr) only means that something else is being edited now; the project stays open.
	const String path = _video_file_of(p_object);
	if (path.is_empty()) {
		return;
	}
	if (Object::cast_to<TetraVideoProjectFile>(p_object)) {
		if (path != workspace->get_project_path()) {
			workspace->request_open_project(path);
		}
	} else {
		workspace->import_and_show(path);
	}
}

String TetraVideoEditorPlugin::get_unsaved_status(const String &p_for_scene) const {
	// A video project belongs to no scene.
	if (!p_for_scene.is_empty() || !workspace->is_modified()) {
		return String();
	}
	const String path = workspace->get_project_path();
	if (path.is_empty()) {
		// An untouched or emptied new project is not worth a question.
		const Ref<TetraVideoProject> project = workspace->get_project();
		if (project->get_bin_clip_ids().is_empty() && project->get_clip_ids().is_empty()) {
			return String();
		}
		return TTR("Save the new video project before closing?");
	}
	return vformat(TTR("Save changes to the video project %s before closing?"), path.get_file());
}

void TetraVideoEditorPlugin::save_external_data() {
	if (get_unsaved_status().is_empty()) {
		return;
	}
	if (!workspace->get_project_path().is_empty()) {
		workspace->save_project();
		return;
	}
	// "Save" was chosen for a project that has no file yet: it gets the first free default name.
	String path = "res://video_project.mlt";
	for (int i = 2; FileAccess::exists(path); i++) {
		path = vformat("res://video_project_%d.mlt", i);
	}
	workspace->save_project(path);
}

void TetraVideoEditorPlugin::apply_changes() {
	workspace->commit_pending_edits();
}

void TetraVideoEditorPlugin::set_window_layout(Ref<ConfigFile> p_layout) {
	// The project that was open when the editor was closed comes back, unless work has begun already.
	const String path = p_layout->get_value("TetraVideo", "project_path", String());
	if (!path.is_empty() && FileAccess::exists(path) && workspace->get_project_path().is_empty() && !workspace->is_modified()) {
		workspace->open_project(path);
	}
}

void TetraVideoEditorPlugin::get_window_layout(Ref<ConfigFile> p_layout) {
	p_layout->set_value("TetraVideo", "project_path", workspace->get_project_path());
}

void TetraVideoEditorPlugin::_bind_methods() {
	ClassDB::bind_static_method("TetraVideoEditorPlugin", D_METHOD("get_singleton"), &TetraVideoEditorPlugin::get_singleton);
	ClassDB::bind_method(D_METHOD("get_workspace"), &TetraVideoEditorPlugin::get_workspace);
}

TetraVideoEditorPlugin::TetraVideoEditorPlugin() {
	singleton = this;

	// The main-screen button looks this shortcut up by name when the plugin is added.
	ED_SHORTCUT("editor/editor_video", TTRC("Open Video Workspace"), KeyModifierMask::CTRL | Key::F7);

	workspace = memnew(TetraVideoWorkspace);
	workspace->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(workspace);
	workspace->hide();
}

TetraVideoEditorPlugin::~TetraVideoEditorPlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
