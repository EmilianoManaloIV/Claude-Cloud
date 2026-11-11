#include "tetra_video_effect_stack.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_assets.h"
#include "../tetra_video_project.h"

#include "tetra/editor/tetra_param_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"
#include "scene/main/timer.h"

Dictionary TetraVideoEffectStack::_operation_of(const Dictionary &p_effect) const {
	const int width = project.is_valid() ? project->get_width() : 1920;
	const int height = project.is_valid() ? project->get_height() : 1080;
	Dictionary operation = TetraVideoAssets::get_operation(p_effect.get("operation", String()), width, height);
	if (operation.is_empty()) {
		// An effect of a file written by another program: MLT's own description of the filter, if it has one.
		operation = TetraMltEngine::get_service_operation("filter", p_effect.get("service", String()));
	}
	return operation;
}

void TetraVideoEffectStack::_update() {
	if (project.is_null() || !project->has_clip(clip_id)) {
		clip_id = -1;
	}
	TypedArray<Dictionary> effects;
	String signature = itos(clip_id);
	if (clip_id >= 0) {
		effects = project->get_clip_effects(clip_id);
		for (int i = 0; i < effects.size(); i++) {
			signature += "|" + String(Dictionary(effects[i])["operation"]) + ":" + String(Dictionary(effects[i])["service"]);
		}
	}
	updating = true;
	if (signature != shown_signature) {
		shown_signature = signature;
		tree->clear();
		TreeItem *root = tree->create_item();
		if (clip_id >= 0) {
			const Dictionary clip = project->get_clip(clip_id);
			const Dictionary bin = project->get_bin_clip(clip["bin_id"]);
			title->set_text(vformat(TTR("%s on %s"), String(bin.get("name", String())), project->get_track_name(clip["track"])));
		} else {
			title->set_text(TTR("No clip selected"));
		}
		for (int i = 0; i < effects.size(); i++) {
			const Dictionary effect = effects[i];
			const Dictionary operation = _operation_of(effect);
			TreeItem *item = tree->create_item(root);
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_editable(0, true);
			item->set_text(0, operation.is_empty() ? String(effect["service"]) : String(operation["label"]));
			item->set_tooltip_text(0, operation.get("description", String()));
			item->add_button(0, get_editor_theme_icon(SNAME("MoveUp")), BUTTON_UP, i == 0, TTR("Move Up: apply this effect earlier"));
			item->add_button(0, get_editor_theme_icon(SNAME("MoveDown")), BUTTON_DOWN, i == effects.size() - 1, TTR("Move Down: apply this effect later"));
			item->add_button(0, get_editor_theme_icon(SNAME("Remove")), BUTTON_REMOVE, false, TTR("Remove Effect"));
		}
		selected_effect = effects.is_empty() ? -1 : CLAMP(selected_effect, 0, effects.size() - 1);
		if (selected_effect >= 0) {
			tree->get_root()->get_child(selected_effect)->select(0);
		}
		_show_selected_effect();
	}
	// What can change without the list changing: the enable boxes and the values.
	TreeItem *root = tree->get_root();
	for (int i = 0; root && i < effects.size() && i < root->get_child_count(); i++) {
		root->get_child(i)->set_checked(0, Dictionary(effects[i])["enabled"]);
	}
	if (selected_effect >= 0 && selected_effect < effects.size()) {
		const Dictionary values = Dictionary(effects[selected_effect])["values"];
		const Dictionary shown = panel->get_values();
		for (const KeyValue<Variant, Variant> &entry : values) {
			// Values the panel already shows are left alone, so a slider being dragged is not disturbed.
			if (panel->has_param(entry.key) && shown.get(entry.key, Variant()) != entry.value) {
				panel->set_value(entry.key, entry.value);
			}
		}
	}
	updating = false;

	if (clip_id < 0) {
		hint->set_text(TTR("Select a clip in the timeline to see its effects."));
	} else if (effects.is_empty()) {
		hint->set_text(TTR("This clip has no effects.\nDouble-click an effect in the Effects tab, or drag one onto the clip."));
	}
	hint->set_visible(clip_id < 0 || effects.is_empty());
	tree->set_visible(!hint->is_visible());
	panel->set_visible(!hint->is_visible());
}

void TetraVideoEffectStack::_show_selected_effect() {
	if (clip_id < 0 || selected_effect < 0 || selected_effect >= project->get_clip_effect_count(clip_id)) {
		panel->set_operation(Dictionary());
		return;
	}
	const Dictionary effect = project->get_clip_effects(clip_id)[selected_effect];
	const Dictionary operation = _operation_of(effect);
	panel->set_operation(operation);
	if (!operation.is_empty()) {
		panel->set_values(effect["values"]);
	}
}

void TetraVideoEffectStack::_project_changed() {
	_update();
}

void TetraVideoEffectStack::_item_selected() {
	if (updating) {
		return;
	}
	TreeItem *item = tree->get_selected();
	if (!item || item->get_index() == selected_effect) {
		return;
	}
	commit_pending();
	selected_effect = item->get_index();
	updating = true;
	_show_selected_effect();
	updating = false;
}

void TetraVideoEffectStack::_item_edited() {
	TreeItem *item = tree->get_edited();
	if (updating || !item || clip_id < 0) {
		return;
	}
	commit_pending();
	project->set_clip_effect_enabled(clip_id, item->get_index(), item->is_checked(0));
}

void TetraVideoEffectStack::_button_clicked(Object *p_item, int p_column, int p_id, int p_mouse_button) {
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	if (!item || clip_id < 0 || p_mouse_button != int(MouseButton::LEFT)) {
		return;
	}
	commit_pending();
	const int index = item->get_index();
	switch (p_id) {
		case BUTTON_UP: {
			if (project->move_clip_effect(clip_id, index, index - 1) && selected_effect == index) {
				select_effect(index - 1);
			}
		} break;
		case BUTTON_DOWN: {
			if (project->move_clip_effect(clip_id, index, index + 1) && selected_effect == index) {
				select_effect(index + 1);
			}
		} break;
		case BUTTON_REMOVE: {
			project->remove_clip_effect(clip_id, index);
		} break;
	}
}

void TetraVideoEffectStack::_param_changed(const String &p_name, const Variant &p_value) {
	if (updating || clip_id < 0 || selected_effect < 0) {
		return;
	}
	if (!group_open) {
		project->begin_undo_group(TTR("Change Effect Parameter"));
		group_open = true;
	}
	project->set_clip_effect_value(clip_id, selected_effect, p_name, p_value);
	group_timer->start();
}

void TetraVideoEffectStack::set_project(const Ref<TetraVideoProject> &p_project) {
	commit_pending();
	if (project.is_valid()) {
		project->disconnect("changed", callable_mp(this, &TetraVideoEffectStack::_project_changed));
	}
	project = p_project;
	clip_id = -1;
	selected_effect = -1;
	shown_signature = String();
	if (project.is_valid()) {
		project->connect("changed", callable_mp(this, &TetraVideoEffectStack::_project_changed));
	}
	_update();
}

void TetraVideoEffectStack::set_clip(int p_clip_id) {
	if (p_clip_id == clip_id) {
		return;
	}
	commit_pending();
	clip_id = p_clip_id;
	selected_effect = 0;
	_update();
}

int TetraVideoEffectStack::get_clip() const {
	return clip_id;
}

void TetraVideoEffectStack::select_effect(int p_index) {
	if (clip_id < 0 || p_index < 0 || p_index >= project->get_clip_effect_count(clip_id)) {
		return;
	}
	commit_pending();
	selected_effect = p_index;
	// The list may still show the effects in their old order; rebuilding it selects the right row.
	shown_signature = String();
	_update();
}

int TetraVideoEffectStack::get_selected_effect() const {
	return selected_effect;
}

int TetraVideoEffectStack::get_effect_count() const {
	return clip_id >= 0 ? project->get_clip_effect_count(clip_id) : 0;
}

TetraParamPanel *TetraVideoEffectStack::get_param_panel() const {
	return panel;
}

void TetraVideoEffectStack::commit_pending() {
	if (group_open) {
		group_open = false;
		group_timer->stop();
		if (project.is_valid()) {
			project->end_undo_group();
		}
	}
}

void TetraVideoEffectStack::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_clip", "clip_id"), &TetraVideoEffectStack::set_clip);
	ClassDB::bind_method(D_METHOD("get_clip"), &TetraVideoEffectStack::get_clip);
	ClassDB::bind_method(D_METHOD("select_effect", "index"), &TetraVideoEffectStack::select_effect);
	ClassDB::bind_method(D_METHOD("get_selected_effect"), &TetraVideoEffectStack::get_selected_effect);
	ClassDB::bind_method(D_METHOD("get_effect_count"), &TetraVideoEffectStack::get_effect_count);
	ClassDB::bind_method(D_METHOD("get_param_panel"), &TetraVideoEffectStack::get_param_panel);
	ClassDB::bind_method(D_METHOD("commit_pending"), &TetraVideoEffectStack::commit_pending);
}

TetraVideoEffectStack::TetraVideoEffectStack() {
	title = memnew(Label);
	title->set_text(TTR("No clip selected"));
	title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title->set_theme_type_variation("HeaderSmall");
	add_child(title);

	hint = memnew(Label);
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->set_modulate(Color(1, 1, 1, 0.6));
	hint->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(hint);

	tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_columns(1);
	tree->set_custom_minimum_size(Size2(0, 84) * EDSCALE);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_stretch_ratio(0.6);
	tree->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoEffectStack::_item_selected));
	tree->connect("item_edited", callable_mp(this, &TetraVideoEffectStack::_item_edited));
	tree->connect("button_clicked", callable_mp(this, &TetraVideoEffectStack::_button_clicked));
	add_child(tree);

	panel = memnew(TetraParamPanel);
	panel->set_v_size_flags(SIZE_EXPAND_FILL);
	panel->connect("value_changed", callable_mp(this, &TetraVideoEffectStack::_param_changed));
	add_child(panel);

	// Closes the undo step of a run of parameter edits once the values have rested.
	group_timer = memnew(Timer);
	group_timer->set_one_shot(true);
	group_timer->set_wait_time(0.6);
	group_timer->connect("timeout", callable_mp(this, &TetraVideoEffectStack::commit_pending));
	add_child(group_timer);
}
