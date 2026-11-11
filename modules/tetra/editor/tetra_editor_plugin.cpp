#include "tetra_editor_plugin.h"

#include "../tetra_engine_registry.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/separator.h"

namespace {

const int ABOUT_WIDTH = 560;

// The upstream releases pinned in upstream/ (see docs/ARCHITECTURE.md). Godot's
// version is that of the running editor.
struct Upstream {
	const char *name;
	const char *version;
	const char *role;
};

const Upstream UPSTREAMS[] = {
	{ "Godot", GODOT_VERSION_NUMBER "-" GODOT_VERSION_STATUS, TTRC("The editor itself") },
	{ "GIMP", "3.2.8", TTRC("Image workspace, filters through GEGL") },
	{ "Kdenlive", "26.08.3", TTRC("Video workspace, playback and rendering through MLT") },
	{ "Blender", "5.3.0", TTRC("Mesh operators, modifiers and Cycles through the Blender bridge") },
};

} // namespace

TetraEditorPlugin *TetraEditorPlugin::singleton = nullptr;

TetraEditorPlugin *TetraEditorPlugin::get_singleton() {
	return singleton;
}

String TetraEditorPlugin::get_plugin_name() const {
	return "Tetra";
}

Label *TetraEditorPlugin::_add_cell(GridContainer *p_grid, const String &p_text, bool p_heading) {
	Label *cell = memnew(Label(p_text));
	cell->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	if (p_heading) {
		cell->set_modulate(Color(1, 1, 1, 0.6));
	}
	p_grid->add_child(cell);
	return cell;
}

void TetraEditorPlugin::_update_about() {
	while (engine_grid->get_child_count() > 0) {
		Node *child = engine_grid->get_child(0);
		engine_grid->remove_child(child);
		memdelete(child);
	}

	const TypedArray<Dictionary> engines = TetraEngineRegistry::get_engines();
	no_engines_label->set_visible(engines.is_empty());
	engine_grid->set_visible(!engines.is_empty());
	if (engines.is_empty()) {
		return;
	}

	_add_cell(engine_grid, TTR("Engine"), true);
	_add_cell(engine_grid, TTR("Version"), true);
	_add_cell(engine_grid, TTR("Status"), true);
	_add_cell(engine_grid, TTR("Details"), true);
	for (int i = 0; i < engines.size(); i++) {
		const Dictionary engine = engines[i];
		const bool available = engine["available"];
		const String version = engine["version"];
		const String details = engine["details"];

		_add_cell(engine_grid, engine["name"]);
		_add_cell(engine_grid, version.is_empty() ? String::utf8("–") : version);
		Label *status = _add_cell(engine_grid, available ? TTR("Available") : TTR("Not available"));
		status->add_theme_color_override(SceneStringName(font_color), about_dialog->get_theme_color(available ? SNAME("success_color") : SNAME("warning_color"), EditorStringName(Editor)));
		// Details are often a long path: they give way first and stay readable in the tooltip.
		Label *details_cell = _add_cell(engine_grid, details);
		details_cell->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		details_cell->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		details_cell->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		details_cell->set_tooltip_text(details);
		details_cell->set_tooltip_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	}
}

void TetraEditorPlugin::show_about() {
	_update_about();
	about_dialog->reset_size();
	about_dialog->popup_centered(Size2(ABOUT_WIDTH, 0) * EDSCALE);
}

AcceptDialog *TetraEditorPlugin::get_about_dialog() const {
	return about_dialog;
}

void TetraEditorPlugin::_bind_methods() {
	ClassDB::bind_static_method(SNAME("TetraEditorPlugin"), D_METHOD("get_singleton"), &TetraEditorPlugin::get_singleton);
	ClassDB::bind_method(D_METHOD("show_about"), &TetraEditorPlugin::show_about);
	ClassDB::bind_method(D_METHOD("get_about_dialog"), &TetraEditorPlugin::get_about_dialog);
}

TetraEditorPlugin::TetraEditorPlugin() {
	singleton = this;

	about_dialog = memnew(AcceptDialog);
	about_dialog->set_title(TTR("About Tetra Studio"));
	about_dialog->set_ok_button_text(TTR("Close"));
	EditorNode::get_singleton()->get_gui_base()->add_child(about_dialog);

	VBoxContainer *layout = memnew(VBoxContainer);
	about_dialog->add_child(layout);

	Label *summary = memnew(Label(TTR("Tetra Studio is one editor for games, images, video and 3D models: the Godot editor with an Image workspace, a Video workspace and a Blender bridge built in. The upstream engines do the work, so what GIMP's filters, Kdenlive's effects and Blender's operators can do upstream, they can do here.")));
	summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	summary->set_custom_minimum_size(Size2(ABOUT_WIDTH, 0) * EDSCALE);
	layout->add_child(summary);

	layout->add_child(memnew(HSeparator));

	Label *upstream_heading = memnew(Label(TTR("Built From")));
	upstream_heading->set_theme_type_variation("HeaderSmall");
	layout->add_child(upstream_heading);

	upstream_grid = memnew(GridContainer);
	upstream_grid->set_columns(3);
	upstream_grid->add_theme_constant_override("h_separation", 24 * EDSCALE);
	layout->add_child(upstream_grid);
	_add_cell(upstream_grid, TTR("Program"), true);
	_add_cell(upstream_grid, TTR("Version"), true);
	_add_cell(upstream_grid, TTR("Provides"), true);
	for (const Upstream &upstream : UPSTREAMS) {
		_add_cell(upstream_grid, upstream.name);
		_add_cell(upstream_grid, upstream.version);
		_add_cell(upstream_grid, TTR(upstream.role));
	}

	layout->add_child(memnew(HSeparator));

	Label *engine_heading = memnew(Label(TTR("Engines in This Build")));
	engine_heading->set_theme_type_variation("HeaderSmall");
	layout->add_child(engine_heading);

	engine_grid = memnew(GridContainer);
	engine_grid->set_columns(4);
	engine_grid->add_theme_constant_override("h_separation", 24 * EDSCALE);
	layout->add_child(engine_grid);

	no_engines_label = memnew(Label(TTR("No engine has been registered.")));
	no_engines_label->set_modulate(Color(1, 1, 1, 0.6));
	layout->add_child(no_engines_label);

	add_tool_menu_item(TTR("About Tetra Studio..."), callable_mp(this, &TetraEditorPlugin::show_about));
}

TetraEditorPlugin::~TetraEditorPlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
