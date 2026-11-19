#include "tetra_video_effect_stack.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_assets.h"
#include "../tetra_video_project.h"

#include "tetra_video_keyframe_strip.h"

#include "tetra/editor/tetra_param_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
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
		const Dictionary keyframes = Dictionary(effects[selected_effect])["keyframes"];
		const Dictionary shown = panel->get_values();
		for (const KeyValue<Variant, Variant> &entry : values) {
			// Values the panel already shows are left alone, so a slider being dragged is not disturbed.
			// Animated parameters show the value at the playhead instead (_update_animation).
			if (panel->has_param(entry.key) && !keyframes.has(entry.key) && shown.get(entry.key, Variant()) != entry.value) {
				panel->set_value(entry.key, entry.value);
			}
		}
	}
	strip->set_clip(clip_id);
	_update_animation();
	updating = false;

	if (clip_id < 0) {
		hint->set_text(TTR("Select a clip in the timeline to see its effects."));
	} else if (effects.is_empty()) {
		hint->set_text(TTR("This clip has no effects.\nDouble-click an effect in the Effects tab, or drag one onto the clip."));
	}
	hint->set_visible(clip_id < 0 || effects.is_empty());
	tree->set_visible(!hint->is_visible());
	panel->set_visible(!hint->is_visible());
	strip->set_visible(!hint->is_visible() && strip->get_lane_count() > 0);
	keyframe_scroll->set_visible(!hint->is_visible() && !keyframe_rows.is_empty());
}

void TetraVideoEffectStack::_show_selected_effect() {
	if (clip_id < 0 || selected_effect < 0 || selected_effect >= project->get_clip_effect_count(clip_id)) {
		panel->set_operation(Dictionary());
		_build_keyframe_rows(Dictionary());
		return;
	}
	const Dictionary effect = project->get_clip_effects(clip_id)[selected_effect];
	const Dictionary operation = _operation_of(effect);
	panel->set_operation(operation);
	if (!operation.is_empty()) {
		panel->set_values(effect["values"]);
	}
	_build_keyframe_rows(operation);
	_update_animation();
	keyframe_scroll->set_visible(!keyframe_rows.is_empty());
}

// One row of keyframe controls per parameter of the shown effect that MLT can animate.
void TetraVideoEffectStack::_build_keyframe_rows(const Dictionary &p_operation) {
	for (int i = keyframe_box->get_child_count() - 1; i >= 0; i--) {
		Node *child = keyframe_box->get_child(i);
		keyframe_box->remove_child(child);
		child->queue_free();
	}
	keyframe_rows.clear();
	const Array params = p_operation.get("params", Array());
	for (const Variant &entry : params) {
		const Dictionary param = entry;
		if (!bool(param.get("animatable", false))) {
			continue;
		}
		KeyframeRow row;
		row.name = param["name"];
		row.integer = String(param.get("type", "float")) == "int";
		const String label = param.get("label", row.name);

		HBoxContainer *line = memnew(HBoxContainer);
		line->add_theme_constant_override("separation", 0);
		keyframe_box->add_child(line);

		row.toggle = memnew(Button);
		row.toggle->set_toggle_mode(true);
		row.toggle->set_flat(true);
		row.toggle->set_text(label);
		row.toggle->set_button_icon(get_editor_theme_icon(SNAME("KeyValue")));
		row.toggle->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		row.toggle->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row.toggle->set_h_size_flags(SIZE_EXPAND_FILL);
		row.toggle->set_tooltip_text(vformat(TTR("Animate \"%s\" with keyframes.\nWhile this is on, changing the value sets a keyframe at the playhead."), label));
		row.toggle->connect(SceneStringName(toggled), callable_mp(this, &TetraVideoEffectStack::_keyframe_toggled).bind(row.name));
		line->add_child(row.toggle);

		row.controls = memnew(HBoxContainer);
		row.controls->add_theme_constant_override("separation", 0);
		line->add_child(row.controls);

		row.previous = memnew(Button);
		row.previous->set_flat(true);
		row.previous->set_button_icon(get_editor_theme_icon(SNAME("ArrowLeft")));
		row.previous->set_tooltip_text(TTR("Go to the previous keyframe"));
		row.previous->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoEffectStack::_keyframe_step).bind(row.name, -1));
		row.controls->add_child(row.previous);

		row.key = memnew(Button);
		row.key->set_flat(true);
		row.key->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoEffectStack::_keyframe_pressed).bind(row.name));
		row.controls->add_child(row.key);

		row.next = memnew(Button);
		row.next->set_flat(true);
		row.next->set_button_icon(get_editor_theme_icon(SNAME("ArrowRight")));
		row.next->set_tooltip_text(TTR("Go to the next keyframe"));
		row.next->connect(SceneStringName(pressed), callable_mp(this, &TetraVideoEffectStack::_keyframe_step).bind(row.name, 1));
		row.controls->add_child(row.next);

		row.type = memnew(OptionButton);
		row.type->set_fit_to_longest_item(false);
		row.type->set_clip_text(true);
		row.type->set_custom_minimum_size(Size2(80 * EDSCALE, 0));
		row.type->set_tooltip_text(TTR("How the value moves from the keyframe before the playhead to the next one"));
		for (int type = 0; type < TetraVideoProject::KEYFRAME_TYPE_MAX; type++) {
			row.type->add_item(TetraVideoProject::get_keyframe_type_name(TetraVideoProject::KeyframeType(type)), type);
		}
		row.type->connect(SceneStringName(item_selected), callable_mp(this, &TetraVideoEffectStack::_keyframe_type_selected).bind(row.name));
		row.controls->add_child(row.type);

		keyframe_rows.push_back(row);
	}
	// Up to three rows show at once; more scroll.
	const float row_height = keyframe_rows.is_empty() ? 0.0f : keyframe_rows[0].toggle->get_combined_minimum_size().y + keyframe_box->get_theme_constant(SNAME("separation"));
	keyframe_scroll->set_custom_minimum_size(Size2(0, row_height * MIN(3, keyframe_rows.size())));
}

// Brings the keyframe rows, the strip and the animated values of the form in line with the project and the playhead.
void TetraVideoEffectStack::_update_animation() {
	const int frame = get_frame_in_clip();
	strip->set_playhead(frame);
	const bool has_effect = project.is_valid() && clip_id >= 0 && selected_effect >= 0 && selected_effect < project->get_clip_effect_count(clip_id);
	const bool was_updating = updating;
	updating = true;
	for (const KeyframeRow &row : keyframe_rows) {
		const TypedArray<Dictionary> keys = has_effect ? project->get_effect_keyframes(clip_id, selected_effect, row.name) : TypedArray<Dictionary>();
		const bool animated = !keys.is_empty();
		row.toggle->set_pressed_no_signal(animated);
		row.controls->set_visible(animated);
		if (!animated) {
			continue;
		}
		bool on_key = false;
		bool before = false;
		bool after = false;
		int type = Dictionary(keys[0])["type"];
		for (int i = 0; i < keys.size(); i++) {
			const Dictionary key = keys[i];
			const int at = key["frame"];
			on_key = on_key || at == frame;
			before = before || at < frame;
			after = after || at > frame;
			if (at <= frame) {
				type = key["type"];
			}
		}
		row.previous->set_disabled(!before);
		row.next->set_disabled(!after);
		row.key->set_button_icon(get_editor_theme_icon(on_key ? SNAME("Remove") : SNAME("Add")));
		row.key->set_tooltip_text(on_key ? TTR("Remove the keyframe at the playhead") : TTR("Add a keyframe at the playhead"));
		if (row.type->get_selected_id() != type) {
			row.type->select(row.type->get_item_index(type));
		}
		if (panel->has_param(row.name)) {
			const double value = project->get_effect_value_at(clip_id, selected_effect, row.name, frame);
			const Variant shown = row.integer ? Variant(int64_t(Math::round(value))) : Variant(value);
			if (panel->get_value(row.name) != shown) {
				panel->set_value(row.name, shown);
			}
		}
	}
	updating = was_updating;
}

int TetraVideoEffectStack::_find_keyframe_row(const String &p_name) const {
	for (int i = 0; i < keyframe_rows.size(); i++) {
		if (keyframe_rows[i].name == p_name) {
			return i;
		}
	}
	return -1;
}

void TetraVideoEffectStack::_keyframe_toggled(bool p_on, const String &p_name) {
	if (!updating) {
		set_param_animated(p_name, p_on);
		// A refused change (a locked track) leaves the button as the project says.
		_update_animation();
	}
}

void TetraVideoEffectStack::_keyframe_step(const String &p_name, int p_direction) {
	if (clip_id < 0 || selected_effect < 0) {
		return;
	}
	const int frame = get_frame_in_clip();
	int target = -1;
	const TypedArray<Dictionary> keys = project->get_effect_keyframes(clip_id, selected_effect, p_name);
	for (int i = 0; i < keys.size(); i++) {
		const int at = Dictionary(keys[i])["frame"];
		if (p_direction < 0 && at < frame) {
			target = at;
		} else if (p_direction > 0 && at > frame && target < 0) {
			target = at;
		}
	}
	if (target >= 0) {
		_strip_seek(target);
	}
}

void TetraVideoEffectStack::_keyframe_pressed(const String &p_name) {
	if (clip_id < 0 || selected_effect < 0) {
		return;
	}
	commit_pending();
	const int frame = get_frame_in_clip();
	bool on_key = false;
	const TypedArray<Dictionary> keys = project->get_effect_keyframes(clip_id, selected_effect, p_name);
	for (int i = 0; i < keys.size(); i++) {
		on_key = on_key || int(Dictionary(keys[i])["frame"]) == frame;
	}
	if (on_key) {
		project->remove_effect_keyframe(clip_id, selected_effect, p_name, frame);
	} else {
		project->add_effect_keyframe(clip_id, selected_effect, p_name, frame);
	}
}

// The type belongs to the keyframe the stretch under the playhead starts at.
void TetraVideoEffectStack::_keyframe_type_selected(int p_index, const String &p_name) {
	const int row = _find_keyframe_row(p_name);
	if (updating || row < 0 || clip_id < 0 || selected_effect < 0) {
		return;
	}
	commit_pending();
	const int frame = get_frame_in_clip();
	const TypedArray<Dictionary> keys = project->get_effect_keyframes(clip_id, selected_effect, p_name);
	if (keys.is_empty()) {
		return;
	}
	int target = Dictionary(keys[0])["frame"];
	for (int i = 0; i < keys.size(); i++) {
		const int at = Dictionary(keys[i])["frame"];
		if (at <= frame) {
			target = at;
		}
	}
	project->set_effect_keyframe_type(clip_id, selected_effect, p_name, target, TetraVideoProject::KeyframeType(keyframe_rows[row].type->get_item_id(p_index)));
	_update_animation();
}

void TetraVideoEffectStack::_strip_seek(int p_frame) {
	if (project.is_valid() && project->has_clip(clip_id)) {
		emit_signal(SNAME("seek_requested"), int(project->get_clip(clip_id)["position"]) + p_frame);
	}
}

void TetraVideoEffectStack::set_playhead(int p_frame) {
	if (p_frame == playhead) {
		return;
	}
	playhead = p_frame;
	if (project.is_valid() && clip_id >= 0) {
		_update_animation();
	}
}

int TetraVideoEffectStack::get_playhead() const {
	return playhead;
}

int TetraVideoEffectStack::get_frame_in_clip() const {
	if (project.is_null() || !project->has_clip(clip_id)) {
		return 0;
	}
	const Dictionary clip = project->get_clip(clip_id);
	return CLAMP(playhead - int(clip["position"]), 0, int(clip["length"]) - 1);
}

bool TetraVideoEffectStack::set_param_animated(const String &p_name, bool p_animated) {
	if (project.is_null() || clip_id < 0 || selected_effect < 0 || p_animated == is_param_animated(p_name)) {
		return false;
	}
	commit_pending();
	if (p_animated) {
		// The first keyframe sits on the first frame of the clip, as in Kdenlive.
		return project->add_effect_keyframe(clip_id, selected_effect, p_name, 0);
	}
	return project->clear_effect_keyframes(clip_id, selected_effect, p_name, get_frame_in_clip());
}

bool TetraVideoEffectStack::is_param_animated(const String &p_name) const {
	return project.is_valid() && clip_id >= 0 && selected_effect >= 0 && project->has_effect_keyframes(clip_id, selected_effect, p_name);
}

PackedStringArray TetraVideoEffectStack::get_animatable_params() const {
	PackedStringArray names;
	for (const KeyframeRow &row : keyframe_rows) {
		names.push_back(row.name);
	}
	return names;
}

Control *TetraVideoEffectStack::get_keyframe_control(const String &p_name, const String &p_which) const {
	const int row = _find_keyframe_row(p_name);
	if (row < 0) {
		return nullptr;
	}
	if (p_which == "toggle") {
		return keyframe_rows[row].toggle;
	} else if (p_which == "previous") {
		return keyframe_rows[row].previous;
	} else if (p_which == "key") {
		return keyframe_rows[row].key;
	} else if (p_which == "next") {
		return keyframe_rows[row].next;
	} else if (p_which == "type") {
		return keyframe_rows[row].type;
	}
	return nullptr;
}

TetraVideoKeyframeStrip *TetraVideoEffectStack::get_keyframe_strip() const {
	return strip;
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
	if (project->has_effect_keyframes(clip_id, selected_effect, p_name)) {
		// An animated parameter: the edit sets the keyframe at the playhead, or makes one there.
		project->add_effect_keyframe(clip_id, selected_effect, p_name, get_frame_in_clip(), p_value);
	} else {
		project->set_clip_effect_value(clip_id, selected_effect, p_name, p_value);
	}
	group_timer->start();
}

void TetraVideoEffectStack::set_project(const Ref<TetraVideoProject> &p_project) {
	commit_pending();
	if (project.is_valid()) {
		project->disconnect("changed", callable_mp(this, &TetraVideoEffectStack::_project_changed));
	}
	project = p_project;
	strip->set_project(project);
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
	ClassDB::bind_method(D_METHOD("set_playhead", "frame"), &TetraVideoEffectStack::set_playhead);
	ClassDB::bind_method(D_METHOD("get_playhead"), &TetraVideoEffectStack::get_playhead);
	ClassDB::bind_method(D_METHOD("get_frame_in_clip"), &TetraVideoEffectStack::get_frame_in_clip);
	ClassDB::bind_method(D_METHOD("set_param_animated", "name", "animated"), &TetraVideoEffectStack::set_param_animated);
	ClassDB::bind_method(D_METHOD("is_param_animated", "name"), &TetraVideoEffectStack::is_param_animated);
	ClassDB::bind_method(D_METHOD("get_animatable_params"), &TetraVideoEffectStack::get_animatable_params);
	ClassDB::bind_method(D_METHOD("get_keyframe_control", "name", "which"), &TetraVideoEffectStack::get_keyframe_control);
	ClassDB::bind_method(D_METHOD("get_keyframe_strip"), &TetraVideoEffectStack::get_keyframe_strip);

	// A timeline frame the keyframe controls want the playhead on.
	ADD_SIGNAL(MethodInfo("seek_requested", PropertyInfo(Variant::INT, "frame")));
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

	strip = memnew(TetraVideoKeyframeStrip);
	strip->hide();
	strip->connect("seek_requested", callable_mp(this, &TetraVideoEffectStack::_strip_seek));
	strip->connect("edit_started", callable_mp(this, &TetraVideoEffectStack::commit_pending));
	add_child(strip);

	keyframe_scroll = memnew(ScrollContainer);
	keyframe_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	keyframe_scroll->hide();
	add_child(keyframe_scroll);
	keyframe_box = memnew(VBoxContainer);
	keyframe_box->set_h_size_flags(SIZE_EXPAND_FILL);
	keyframe_scroll->add_child(keyframe_box);

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
