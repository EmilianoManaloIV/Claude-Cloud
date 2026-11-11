#include "tetra_image_layers_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/tree.h"
#include "scene/main/timer.h"
#include "scene/resources/image_texture.h"

void TetraImageLayersPanel::set_document(const Ref<TetraImageDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (document.is_valid()) {
		if (opacity_group_open) {
			document->end_undo_group();
			opacity_group_open = false;
		}
		document->disconnect(SNAME("layers_changed"), callable_mp(this, &TetraImageLayersPanel::_queue_list_update));
		document->disconnect(SNAME("changed"), callable_mp(this, &TetraImageLayersPanel::_pixels_changed));
		document->disconnect(SNAME("size_changed"), callable_mp(this, &TetraImageLayersPanel::_mark_all_thumbnails_dirty));
		document->disconnect(SNAME("undo_changed"), callable_mp(this, &TetraImageLayersPanel::_undo_changed));
	}
	document = p_document;
	thumbnails.clear();
	dirty_thumbnails.clear();
	redo_count = 0;
	if (document.is_valid()) {
		document->connect(SNAME("layers_changed"), callable_mp(this, &TetraImageLayersPanel::_queue_list_update));
		document->connect(SNAME("changed"), callable_mp(this, &TetraImageLayersPanel::_pixels_changed));
		document->connect(SNAME("size_changed"), callable_mp(this, &TetraImageLayersPanel::_mark_all_thumbnails_dirty));
		document->connect(SNAME("undo_changed"), callable_mp(this, &TetraImageLayersPanel::_undo_changed));
	}
	_update_list();
}

Ref<TetraImageDocument> TetraImageLayersPanel::get_document() const {
	return document;
}

void TetraImageLayersPanel::_queue_list_update() {
	if (!list_update_queued) {
		list_update_queued = true;
		callable_mp(this, &TetraImageLayersPanel::_update_list).call_deferred();
	}
}

void TetraImageLayersPanel::_update_list() {
	list_update_queued = false;
	updating = true;
	tree->clear();
	TreeItem *root = tree->create_item();
	const bool has_document = document.is_valid() && document->get_layer_count() > 0;
	if (has_document) {
		const PackedInt32Array ids = document->get_layer_ids();
		const int active = document->get_active_layer();
		const Ref<Texture2D> eye_open = get_editor_theme_icon(SNAME("GuiVisibilityVisible"));
		const Ref<Texture2D> eye_closed = get_editor_theme_icon(SNAME("GuiVisibilityHidden"));
		const Ref<Texture2D> locked = get_editor_theme_icon(SNAME("Lock"));
		const Ref<Texture2D> unlocked = get_editor_theme_icon(SNAME("Unlock"));
		for (int i = ids.size() - 1; i >= 0; i--) {
			const int id = ids[i];
			TreeItem *item = tree->create_item(root);
			item->set_metadata(COLUMN_NAME, id);
			const bool layer_visible = document->layer_is_visible(id);
			item->add_button(COLUMN_VISIBLE, layer_visible ? eye_open : eye_closed, COLUMN_VISIBLE, false, TTR("Toggle the visibility of the layer."));
			item->set_selectable(COLUMN_VISIBLE, false);
			item->set_text(COLUMN_NAME, document->layer_get_name(id));
			item->set_icon_max_width(COLUMN_NAME, Math::round(48 * EDSCALE));
			if (thumbnails.has(id)) {
				item->set_icon(COLUMN_NAME, thumbnails[id]);
			} else {
				dirty_thumbnails.insert(id);
			}
			if (!layer_visible) {
				item->set_custom_color(COLUMN_NAME, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
			}
			const bool lock = document->layer_get_lock_alpha(id);
			item->add_button(COLUMN_LOCK, lock ? locked : unlocked, COLUMN_LOCK, false, TTR("Lock the alpha channel: painting keeps the transparency of the layer."));
			if (!lock) {
				item->set_button_color(COLUMN_LOCK, 0, Color(1, 1, 1, 0.3));
			}
			item->set_selectable(COLUMN_LOCK, false);
			if (id == active) {
				item->select(COLUMN_NAME);
			}
		}
		if (document->get_layer_index(active) >= 0) {
			mode->select(int(document->layer_get_mode(active)));
			opacity->set_value_no_signal(document->layer_get_opacity(active) * 100.0);
		}
		const int index = document->get_layer_index(active);
		raise_button->set_disabled(index < 0 || index >= ids.size() - 1);
		lower_button->set_disabled(index <= 0);
		merge_button->set_disabled(index <= 0);
		delete_button->set_disabled(index < 0 || ids.size() <= 1);
		duplicate_button->set_disabled(index < 0);
		if (!dirty_thumbnails.is_empty() && thumbnail_timer->is_stopped()) {
			thumbnail_timer->start();
		}
	}
	new_button->set_disabled(!has_document);
	mode->set_disabled(!has_document);
	opacity->set_read_only(!has_document);
	if (!has_document) {
		for (Button *button : { duplicate_button, raise_button, lower_button, merge_button, delete_button }) {
			button->set_disabled(true);
		}
	}
	updating = false;
}

Ref<ImageTexture> TetraImageLayersPanel::_make_thumbnail(int p_layer_id) const {
	Ref<Image> image = document->layer_get_image(p_layer_id);
	if (image.is_null() || image->is_empty()) {
		return Ref<ImageTexture>();
	}
	const int box = MAX(8, int(Math::round(40 * EDSCALE)));
	const Size2 full = image->get_size();
	const real_t scale = MIN(box / full.x, box / full.y);
	const int w = MAX(1, int(Math::round(full.x * scale)));
	const int h = MAX(1, int(Math::round(full.y * scale)));
	// Twice the size first and a box filter after it, so thin strokes of a large layer survive.
	if (full.x > w * 2 && full.y > h * 2) {
		image->resize(w * 2, h * 2, Image::INTERPOLATE_BILINEAR);
		image->shrink_x2();
	} else {
		image->resize(w, h, Image::INTERPOLATE_BILINEAR);
	}
	// Shown over a small checkerboard so transparent layers read as such.
	Ref<Image> shown = Image::create_empty(image->get_width(), image->get_height(), false, Image::FORMAT_RGBA8);
	for (int y = 0; y < shown->get_height(); y++) {
		for (int x = 0; x < shown->get_width(); x++) {
			const Color pixel = image->get_pixel(x, y);
			const float alpha = CLAMP(pixel.a, 0.0f, 1.0f);
			const float check = ((x / 5 + y / 5) & 1) ? 0.4f : 0.6f;
			const Color color = Color(pixel.r, pixel.g, pixel.b).clamp().linear_to_srgb();
			shown->set_pixel(x, y, Color(Math::lerp(check, color.r, alpha), Math::lerp(check, color.g, alpha), Math::lerp(check, color.b, alpha), 1.0f));
		}
	}
	return ImageTexture::create_from_image(shown);
}

void TetraImageLayersPanel::_update_thumbnails() {
	if (document.is_null()) {
		dirty_thumbnails.clear();
		return;
	}
	if (document->is_stroke_active()) {
		// Not while painting: wait for the stroke to finish.
		thumbnail_timer->start();
		return;
	}
	for (const int id : dirty_thumbnails) {
		if (document->get_layer_index(id) >= 0) {
			thumbnails[id] = _make_thumbnail(id);
		} else {
			thumbnails.erase(id);
		}
	}
	dirty_thumbnails.clear();
	for (TreeItem *item = tree->get_root() ? tree->get_root()->get_first_child() : nullptr; item; item = item->get_next()) {
		const int id = item->get_metadata(COLUMN_NAME);
		if (thumbnails.has(id)) {
			item->set_icon(COLUMN_NAME, thumbnails[id]);
		}
	}
}

void TetraImageLayersPanel::_mark_thumbnail_dirty(int p_layer_id) {
	dirty_thumbnails.insert(p_layer_id);
	thumbnail_timer->start();
}

void TetraImageLayersPanel::_mark_all_thumbnails_dirty() {
	if (document.is_null()) {
		return;
	}
	for (const int id : document->get_layer_ids()) {
		dirty_thumbnails.insert(id);
	}
	thumbnail_timer->start();
}

void TetraImageLayersPanel::_pixels_changed(const Rect2i &p_rect) {
	// Edits go to the active layer; undo and redo, which may touch any layer, are caught in _undo_changed().
	_mark_thumbnail_dirty(document->get_active_layer());
}

void TetraImageLayersPanel::_undo_changed() {
	const int redo_now = document->get_redo_names().size();
	if (redo_now != redo_count) {
		redo_count = redo_now;
		_mark_all_thumbnails_dirty();
	}
}

void TetraImageLayersPanel::_item_selected() {
	TreeItem *item = tree->get_selected();
	if (updating || !item || document.is_null()) {
		return;
	}
	document->set_active_layer(item->get_metadata(COLUMN_NAME));
}

void TetraImageLayersPanel::_item_activated() {
	TreeItem *item = tree->get_selected();
	if (!item) {
		return;
	}
	item->set_editable(COLUMN_NAME, true);
	tree->edit_selected(true);
}

void TetraImageLayersPanel::_item_edited() {
	TreeItem *item = tree->get_edited();
	if (updating || !item || document.is_null()) {
		return;
	}
	item->set_editable(COLUMN_NAME, false);
	const int id = item->get_metadata(COLUMN_NAME);
	const String name = item->get_text(COLUMN_NAME).strip_edges();
	if (name.is_empty() || name == document->layer_get_name(id)) {
		item->set_text(COLUMN_NAME, document->layer_get_name(id));
		return;
	}
	document->layer_set_name(id, name);
}

void TetraImageLayersPanel::_button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button) {
	if (document.is_null() || p_mouse_button != int(MouseButton::LEFT)) {
		return;
	}
	const int id = p_item->get_metadata(COLUMN_NAME);
	if (p_id == COLUMN_VISIBLE) {
		document->layer_set_visible(id, !document->layer_is_visible(id));
	} else if (p_id == COLUMN_LOCK) {
		document->layer_set_lock_alpha(id, !document->layer_get_lock_alpha(id));
	}
}

void TetraImageLayersPanel::_mode_selected(int p_index) {
	if (updating || document.is_null()) {
		return;
	}
	document->layer_set_mode(document->get_active_layer(), TetraImageDocument::LayerMode(p_index));
}

void TetraImageLayersPanel::_opacity_changed(double p_value) {
	if (updating || document.is_null()) {
		return;
	}
	document->layer_set_opacity(document->get_active_layer(), p_value / 100.0);
}

void TetraImageLayersPanel::_opacity_grabbed() {
	// One drag of the slider is one undo step.
	if (document.is_valid() && !document->is_stroke_active() && !document->is_undo_group_open()) {
		document->begin_undo_group(TTR("Layer Opacity"));
		opacity_group_open = true;
	}
}

void TetraImageLayersPanel::_opacity_ungrabbed() {
	if (opacity_group_open && document.is_valid()) {
		document->end_undo_group();
	}
	opacity_group_open = false;
}

String TetraImageLayersPanel::_unique_name(const String &p_base) const {
	HashSet<String> used;
	for (const int id : document->get_layer_ids()) {
		used.insert(document->layer_get_name(id));
	}
	if (!used.has(p_base)) {
		return p_base;
	}
	for (int n = 2;; n++) {
		const String name = vformat("%s %d", p_base, n);
		if (!used.has(name)) {
			return name;
		}
	}
}

void TetraImageLayersPanel::new_layer() {
	if (document.is_null()) {
		return;
	}
	const int index = document->get_layer_index(document->get_active_layer());
	document->add_layer(_unique_name(TTR("Layer")), index < 0 ? -1 : index + 1);
}

void TetraImageLayersPanel::duplicate_layer() {
	if (document.is_valid()) {
		document->duplicate_layer(document->get_active_layer());
	}
}

void TetraImageLayersPanel::raise_layer() {
	if (document.is_null()) {
		return;
	}
	const int index = document->get_layer_index(document->get_active_layer());
	if (index >= 0 && index < document->get_layer_count() - 1) {
		document->move_layer(document->get_active_layer(), index + 1);
	}
}

void TetraImageLayersPanel::lower_layer() {
	if (document.is_null()) {
		return;
	}
	const int index = document->get_layer_index(document->get_active_layer());
	if (index > 0) {
		document->move_layer(document->get_active_layer(), index - 1);
	}
}

void TetraImageLayersPanel::merge_down() {
	if (document.is_valid() && document->get_layer_index(document->get_active_layer()) > 0) {
		document->merge_down(document->get_active_layer());
	}
}

void TetraImageLayersPanel::delete_layer() {
	if (document.is_valid() && document->get_layer_count() > 1) {
		document->remove_layer(document->get_active_layer());
	}
}

PackedStringArray TetraImageLayersPanel::get_listed_names() const {
	PackedStringArray names;
	for (TreeItem *item = tree->get_root() ? tree->get_root()->get_first_child() : nullptr; item; item = item->get_next()) {
		names.push_back(item->get_text(COLUMN_NAME));
	}
	return names;
}

Button *TetraImageLayersPanel::_make_button(const String &p_tooltip, const Callable &p_action) {
	Button *button = memnew(Button);
	button->set_theme_type_variation(SceneStringName(FlatButton));
	button->set_tooltip_text(p_tooltip);
	button->set_focus_mode(FOCUS_NONE);
	button->connect(SceneStringName(pressed), p_action);
	return button;
}

void TetraImageLayersPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			new_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			duplicate_button->set_button_icon(get_editor_theme_icon(SNAME("Duplicate")));
			raise_button->set_button_icon(get_editor_theme_icon(SNAME("MoveUp")));
			lower_button->set_button_icon(get_editor_theme_icon(SNAME("MoveDown")));
			merge_button->set_button_icon(get_editor_theme_icon(SNAME("TetraLayerMergeDown")));
			delete_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
			tree->set_column_custom_minimum_width(COLUMN_VISIBLE, Math::round(30 * EDSCALE));
			tree->set_column_custom_minimum_width(COLUMN_LOCK, Math::round(30 * EDSCALE));
			_queue_list_update();
		} break;
	}
}

void TetraImageLayersPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &TetraImageLayersPanel::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &TetraImageLayersPanel::get_document);
	ClassDB::bind_method(D_METHOD("new_layer"), &TetraImageLayersPanel::new_layer);
	ClassDB::bind_method(D_METHOD("duplicate_layer"), &TetraImageLayersPanel::duplicate_layer);
	ClassDB::bind_method(D_METHOD("raise_layer"), &TetraImageLayersPanel::raise_layer);
	ClassDB::bind_method(D_METHOD("lower_layer"), &TetraImageLayersPanel::lower_layer);
	ClassDB::bind_method(D_METHOD("merge_down"), &TetraImageLayersPanel::merge_down);
	ClassDB::bind_method(D_METHOD("delete_layer"), &TetraImageLayersPanel::delete_layer);
	ClassDB::bind_method(D_METHOD("get_listed_names"), &TetraImageLayersPanel::get_listed_names);
	ClassDB::bind_method(D_METHOD("get_tree"), &TetraImageLayersPanel::get_tree);
}

TetraImageLayersPanel::TetraImageLayersPanel() {
	mode = memnew(OptionButton);
	mode->set_tooltip_text(TTR("How the active layer is combined with the layers below it."));
	mode->set_fit_to_longest_item(false);
	mode->set_clip_text(true);
	const PackedStringArray mode_names = TetraImageDocument::get_layer_mode_names();
	for (const String &name : mode_names) {
		mode->add_item(name);
	}
	mode->connect(SceneStringName(item_selected), callable_mp(this, &TetraImageLayersPanel::_mode_selected));
	add_child(mode);

	opacity = memnew(EditorSpinSlider);
	opacity->set_label(TTR("Opacity"));
	opacity->set_min(0.0);
	opacity->set_max(100.0);
	opacity->set_step(0.1);
	opacity->set_suffix("%");
	opacity->set_value(100.0);
	opacity->set_tooltip_text(TTR("Opacity of the active layer."));
	opacity->connect(SceneStringName(value_changed), callable_mp(this, &TetraImageLayersPanel::_opacity_changed));
	opacity->connect("grabbed", callable_mp(this, &TetraImageLayersPanel::_opacity_grabbed));
	opacity->connect("ungrabbed", callable_mp(this, &TetraImageLayersPanel::_opacity_ungrabbed));
	add_child(opacity);

	tree = memnew(Tree);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_custom_minimum_size(Size2(0, 120) * EDSCALE);
	tree->set_hide_root(true);
	tree->set_hide_folding(true);
	tree->set_columns(3);
	tree->set_column_expand(COLUMN_VISIBLE, false);
	tree->set_column_expand(COLUMN_NAME, true);
	tree->set_column_clip_content(COLUMN_NAME, true);
	tree->set_column_expand(COLUMN_LOCK, false);
	tree->set_select_mode(Tree::SELECT_ROW);
	// Letters are tool shortcuts in the workspace, not a search in the list.
	tree->set_allow_search(false);
	tree->connect(SceneStringName(item_selected), callable_mp(this, &TetraImageLayersPanel::_item_selected));
	tree->connect("item_activated", callable_mp(this, &TetraImageLayersPanel::_item_activated));
	tree->connect("item_edited", callable_mp(this, &TetraImageLayersPanel::_item_edited));
	tree->connect("button_clicked", callable_mp(this, &TetraImageLayersPanel::_button_clicked));
	add_child(tree);

	HBoxContainer *buttons = memnew(HBoxContainer);
	add_child(buttons);
	new_button = _make_button(TTR("New layer above the active one."), callable_mp(this, &TetraImageLayersPanel::new_layer));
	buttons->add_child(new_button);
	duplicate_button = _make_button(TTR("Duplicate the active layer."), callable_mp(this, &TetraImageLayersPanel::duplicate_layer));
	buttons->add_child(duplicate_button);
	raise_button = _make_button(TTR("Raise the active layer."), callable_mp(this, &TetraImageLayersPanel::raise_layer));
	buttons->add_child(raise_button);
	lower_button = _make_button(TTR("Lower the active layer."), callable_mp(this, &TetraImageLayersPanel::lower_layer));
	buttons->add_child(lower_button);
	merge_button = _make_button(TTR("Merge the active layer into the one below it."), callable_mp(this, &TetraImageLayersPanel::merge_down));
	buttons->add_child(merge_button);
	buttons->add_spacer();
	delete_button = _make_button(TTR("Delete the active layer."), callable_mp(this, &TetraImageLayersPanel::delete_layer));
	buttons->add_child(delete_button);

	thumbnail_timer = memnew(Timer);
	thumbnail_timer->set_one_shot(true);
	thumbnail_timer->set_wait_time(0.3);
	thumbnail_timer->connect("timeout", callable_mp(this, &TetraImageLayersPanel::_update_thumbnails));
	add_child(thumbnail_timer);
}
