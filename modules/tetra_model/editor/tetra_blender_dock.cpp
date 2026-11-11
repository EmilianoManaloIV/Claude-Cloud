#include "tetra_blender_dock.h"

#include "tetra_blender_editor_plugin.h"
#include "tetra_blender_modifiers_tab.h"
#include "tetra_blender_operators_tab.h"
#include "tetra_blender_render_tab.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/texture_rect.h"

static constexpr const char *TAB_NAMES[] = { "modifiers", "operators", "render" };

void TetraBlenderDock::_update_status() {
	if (!is_inside_tree()) {
		return;
	}
	const TetraBlenderEditorPlugin::Status status = plugin->get_status();
	const bool busy = status == TetraBlenderEditorPlugin::STATUS_STARTING || status == TetraBlenderEditorPlugin::STATUS_WORKING;
	set_process(busy);

	status_label->set_text(plugin->get_status_text());
	status_label->set_tooltip_text(plugin->get_status_hint());
	// What to do is spelled out when Blender cannot be used; otherwise the
	// details are a tooltip.
	const bool explain = status == TetraBlenderEditorPlugin::STATUS_NOT_FOUND || status == TetraBlenderEditorPlugin::STATUS_FAILED;
	hint_label->set_text(explain ? plugin->get_status_hint() : String());
	hint_label->set_visible(explain);

	StringName icon;
	switch (status) {
		case TetraBlenderEditorPlugin::STATUS_NOT_FOUND:
			icon = SNAME("StatusWarning");
			break;
		case TetraBlenderEditorPlugin::STATUS_FAILED:
			icon = SNAME("StatusError");
			break;
		case TetraBlenderEditorPlugin::STATUS_READY:
			icon = SNAME("StatusSuccess");
			break;
		case TetraBlenderEditorPlugin::STATUS_NOT_STARTED:
			icon = SNAME("GuiRadioUnchecked");
			break;
		default:
			icon = "Progress" + itos(int(spinner_time * 10.0) % 8 + 1);
			break;
	}
	status_icon->set_texture(get_editor_theme_icon(icon));
}

void TetraBlenderDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_status();
		} break;
		case NOTIFICATION_PROCESS: {
			// Only runs while Blender starts or computes: turns the spinner.
			const int frame = int(spinner_time * 10.0);
			spinner_time += get_process_delta_time();
			if (int(spinner_time * 10.0) != frame) {
				_update_status();
			}
		} break;
	}
}

void TetraBlenderDock::show_tab(const String &p_tab) {
	for (int i = 0; i < 3; i++) {
		if (p_tab.to_lower() == TAB_NAMES[i]) {
			tabs->set_current_tab(i);
			return;
		}
	}
	ERR_FAIL_MSG(vformat("The Blender dock has no tab '%s'.", p_tab));
}

String TetraBlenderDock::get_current_tab() const {
	return TAB_NAMES[CLAMP(tabs->get_current_tab(), 0, 2)];
}

TetraBlenderModifiersTab *TetraBlenderDock::get_modifiers_tab() const {
	return modifiers_tab;
}

TetraBlenderOperatorsTab *TetraBlenderDock::get_operators_tab() const {
	return operators_tab;
}

TetraBlenderRenderTab *TetraBlenderDock::get_render_tab() const {
	return render_tab;
}

void TetraBlenderDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("show_tab", "tab"), &TetraBlenderDock::show_tab);
	ClassDB::bind_method(D_METHOD("get_current_tab"), &TetraBlenderDock::get_current_tab);
	ClassDB::bind_method(D_METHOD("get_modifiers_tab"), &TetraBlenderDock::get_modifiers_tab);
	ClassDB::bind_method(D_METHOD("get_operators_tab"), &TetraBlenderDock::get_operators_tab);
	ClassDB::bind_method(D_METHOD("get_render_tab"), &TetraBlenderDock::get_render_tab);
}

TetraBlenderDock::TetraBlenderDock(TetraBlenderEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_name(TTRC("Blender"));
	set_layout_key("TetraBlender");
	set_icon_name("MeshInstance3D");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_UL);
	set_available_layouts(EditorDock::DOCK_LAYOUT_VERTICAL | EditorDock::DOCK_LAYOUT_FLOATING);
	// Wide enough for the three tab titles and a parameter row.
	set_custom_minimum_size(Size2(320, 0) * EDSCALE);

	VBoxContainer *main = memnew(VBoxContainer);
	add_child(main);

	HBoxContainer *status_row = memnew(HBoxContainer);
	main->add_child(status_row);
	status_icon = memnew(TextureRect);
	status_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	status_icon->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
	status_row->add_child(status_icon);
	status_label = memnew(Label);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_label->set_mouse_filter(MOUSE_FILTER_PASS);
	status_row->add_child(status_label);

	hint_label = memnew(Label);
	hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint_label->hide();
	main->add_child(hint_label);

	tabs = memnew(TabContainer);
	tabs->set_v_size_flags(SIZE_EXPAND_FILL);
	main->add_child(tabs);

	modifiers_tab = memnew(TetraBlenderModifiersTab(plugin));
	modifiers_tab->set_name(TTRC("Modifiers"));
	tabs->add_child(modifiers_tab);
	operators_tab = memnew(TetraBlenderOperatorsTab(plugin));
	operators_tab->set_name(TTRC("Operators"));
	tabs->add_child(operators_tab);
	render_tab = memnew(TetraBlenderRenderTab(plugin));
	render_tab->set_name(TTRC("Render"));
	tabs->add_child(render_tab);

	plugin->connect("status_changed", callable_mp(this, &TetraBlenderDock::_update_status));
}
