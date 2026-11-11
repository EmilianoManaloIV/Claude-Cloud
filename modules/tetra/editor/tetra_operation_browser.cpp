#include "tetra_operation_browser.h"

#include "../tetra_schema.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/filter_line_edit.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/tree.h"

namespace {

// With no more operations than this every folder starts open.
const int EXPAND_ALL_LIMIT = 40;

// A search lists this many of its best matches; laying out thousands of rows on every keystroke would lag.
const int MAX_SEARCH_RESULTS = 200;

const char *CATEGORY_META = "tetra_category";

String get_string(const Dictionary &p_dictionary, const char *p_key) {
	const Variant value = p_dictionary.get(p_key, Variant());
	return value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME ? String(value) : String();
}

String get_label(const Dictionary &p_operation) {
	const String label = get_string(p_operation, "label");
	return label.is_empty() ? get_string(p_operation, "id") : label;
}

} // namespace

void TetraOperationBrowser::_update_tree(bool p_select_best_match) {
	updating_tree = true;
	tree->clear();
	TreeItem *root = tree->create_item();
	TreeItem *selected = nullptr;

	const String filter = search_edit->get_text().strip_edges();
	if (filter.is_empty()) {
		tree->set_columns(1);
		const Dictionary categories = TetraSchema::category_tree(operations);
		const Array folders = categories["categories"];
		const bool collapsed_by_default = int(categories["count"]) > EXPAND_ALL_LIMIT && folders.size() > 1;
		_add_category(root, categories, collapsed_by_default, selected);
	} else {
		tree->set_columns(2);
		tree->set_column_expand(0, true);
		tree->set_column_expand_ratio(0, 3);
		tree->set_column_clip_content(0, true);
		tree->set_column_expand(1, true);
		tree->set_column_expand_ratio(1, 2);
		tree->set_column_clip_content(1, true);
		const PackedInt32Array matches = TetraSchema::search_indices(operations, filter);
		const int listed = MIN(int(matches.size()), MAX_SEARCH_RESULTS);
		if (p_select_best_match) {
			selected_id = listed > 0 ? get_string(operations[matches[0]], "id") : String();
		}
		for (int i = 0; i < listed; i++) {
			TreeItem *item = _add_operation(root, matches[i], true);
			if (!selected && get_string(operations[matches[i]], "id") == selected_id) {
				selected = item;
			}
		}
		if (matches.size() > listed) {
			TreeItem *more = tree->create_item(root);
			more->set_text(0, vformat(TTR("%d more matches. Type more to narrow the search."), int(matches.size()) - listed));
			more->set_custom_color(0, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
			more->set_selectable(0, false);
			more->set_selectable(1, false);
		}
	}

	if (selected) {
		// Folders on the way to the selection are opened whatever their remembered state.
		for (TreeItem *parent = selected->get_parent(); parent; parent = parent->get_parent()) {
			parent->set_collapsed(false);
		}
		selected->select(0);
		tree->scroll_to_item(selected, true);
	} else {
		selected_id = String();
	}
	updating_tree = false;
	_update_description();
}

void TetraOperationBrowser::_add_category(TreeItem *p_parent, const Dictionary &p_category, bool p_collapsed_by_default, TreeItem *&r_selected) {
	const Array categories = p_category["categories"];
	for (int i = 0; i < categories.size(); i++) {
		const Dictionary category = categories[i];
		const String path = category["path"];

		TreeItem *folder = tree->create_item(p_parent);
		folder->set_text(0, category["name"]);
		folder->set_auto_translate_mode(0, AUTO_TRANSLATE_MODE_DISABLED);
		folder->set_icon(0, get_editor_theme_icon(SNAME("Folder")));
		folder->set_icon_modulate(0, get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog")));
		// A folder is not a choice: a click anywhere on its row opens or closes it instead.
		folder->set_selectable(0, false);
		folder->set_meta(CATEGORY_META, path);
		_add_category(folder, category, p_collapsed_by_default, r_selected);

		const HashMap<String, bool>::ConstIterator remembered = folder_collapsed.find(path);
		folder->set_collapsed(remembered ? remembered->value : p_collapsed_by_default);
	}

	const Array ids = p_category["operations"];
	for (int i = 0; i < ids.size(); i++) {
		const String id = ids[i];
		const HashMap<String, int>::ConstIterator found = index_by_id.find(id);
		if (!found) {
			continue;
		}
		TreeItem *item = _add_operation(p_parent, found->value, false);
		if (!r_selected && id == selected_id) {
			r_selected = item;
		}
	}
}

TreeItem *TetraOperationBrowser::_add_operation(TreeItem *p_parent, int p_index, bool p_with_category) {
	const Dictionary operation = operations[p_index];
	TreeItem *item = tree->create_item(p_parent);
	item->set_text(0, get_label(operation));
	item->set_auto_translate_mode(0, AUTO_TRANSLATE_MODE_DISABLED);
	item->set_metadata(0, get_string(operation, "id"));
	item->set_tooltip_text(0, get_string(operation, "description"));
	if (p_with_category) {
		item->set_text(1, get_string(operation, "category"));
		item->set_auto_translate_mode(1, AUTO_TRANSLATE_MODE_DISABLED);
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
		item->set_text_overrun_behavior(1, TextServer::OVERRUN_TRIM_ELLIPSIS);
		item->set_custom_color(1, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
	}
	return item;
}

void TetraOperationBrowser::_update_description() {
	description->clear();
	const Color dimmed = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));
	const HashMap<String, int>::ConstIterator found = index_by_id.find(selected_id);
	if (selected_id.is_empty() || !found) {
		description->push_color(dimmed);
		description->add_text(operations.is_empty() ? TTR("No operations are available.") : TTR("Select an operation to see what it does."));
		description->pop();
		return;
	}
	const Dictionary operation = operations[found->value];
	description->push_bold();
	description->add_text(get_label(operation));
	description->pop();
	description->add_newline();

	String origin = selected_id;
	const String category = get_string(operation, "category");
	if (!category.is_empty()) {
		origin += String::utf8("  ·  ") + category;
	}
	description->push_color(dimmed);
	description->add_text(origin);
	description->pop();

	const String text = get_string(operation, "description");
	if (!text.is_empty()) {
		description->add_newline();
		description->add_text(text);
	}
}

void TetraOperationBrowser::_set_selected_from_user(const String &p_id) {
	if (p_id == selected_id) {
		return;
	}
	selected_id = p_id;
	_update_description();
	if (!selected_id.is_empty()) {
		emit_signal(SNAME("operation_selected"), selected_id);
	}
}

void TetraOperationBrowser::_search_text_changed(const String &p_text) {
	const String previous = selected_id;
	// While searching the best match becomes the selection, so that Enter runs it.
	_update_tree(true);
	if (selected_id != previous && !selected_id.is_empty()) {
		emit_signal(SNAME("operation_selected"), selected_id);
	}
}

void TetraOperationBrowser::_search_text_submitted(const String &p_text) {
	if (!selected_id.is_empty()) {
		emit_signal(SNAME("operation_activated"), selected_id);
	}
}

void TetraOperationBrowser::_item_selected() {
	if (updating_tree) {
		return;
	}
	const TreeItem *item = tree->get_selected();
	if (item && item->get_metadata(0).get_type() == Variant::STRING) {
		_set_selected_from_user(item->get_metadata(0));
	}
}

void TetraOperationBrowser::_item_activated() {
	const TreeItem *item = tree->get_selected();
	if (item && item->get_metadata(0).get_type() == Variant::STRING) {
		const String id = item->get_metadata(0);
		_set_selected_from_user(id);
		emit_signal(SNAME("operation_activated"), id);
	}
}

void TetraOperationBrowser::_item_collapsed(Object *p_item) {
	if (updating_tree) {
		return;
	}
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	if (item && item->has_meta(CATEGORY_META)) {
		folder_collapsed[item->get_meta(CATEGORY_META)] = item->is_collapsed();
	}
}

void TetraOperationBrowser::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			search_edit->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			// Folder icons and dimmed colours come from the theme.
			_update_tree();
		} break;
	}
}

void TetraOperationBrowser::set_operations(const Array &p_operations) {
	operations = p_operations;
	index_by_id.clear();
	for (int i = 0; i < operations.size(); i++) {
		if (operations[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const String id = get_string(operations[i], "id");
		if (!id.is_empty() && !index_by_id.has(id)) {
			index_by_id[id] = i;
		}
	}
	if (!index_by_id.has(selected_id)) {
		selected_id = String();
	}
	_update_tree();
}

Array TetraOperationBrowser::get_operations() const {
	return operations;
}

String TetraOperationBrowser::get_selected_id() const {
	return selected_id;
}

Dictionary TetraOperationBrowser::get_selected_operation() const {
	const HashMap<String, int>::ConstIterator found = index_by_id.find(selected_id);
	return found ? Dictionary(operations[found->value]) : Dictionary();
}

void TetraOperationBrowser::select_operation(const String &p_id) {
	selected_id = index_by_id.has(p_id) ? p_id : String();
	// An operation the current search does not list can only be shown by ending the search.
	const String filter = search_edit->get_text().strip_edges();
	if (!selected_id.is_empty() && !filter.is_empty()) {
		const PackedInt32Array matches = TetraSchema::search_indices(operations, filter);
		bool listed = false;
		for (int i = 0; i < MIN(int(matches.size()), MAX_SEARCH_RESULTS) && !listed; i++) {
			listed = get_string(operations[matches[i]], "id") == selected_id;
		}
		if (!listed) {
			search_edit->set_text(String());
		}
	}
	_update_tree();
}

void TetraOperationBrowser::set_filter_text(const String &p_text) {
	if (search_edit->get_text() == p_text) {
		return;
	}
	search_edit->set_text(p_text);
	_update_tree(true);
}

String TetraOperationBrowser::get_filter_text() const {
	return search_edit->get_text();
}

void TetraOperationBrowser::set_description_visible(bool p_visible) {
	description->set_visible(p_visible);
}

bool TetraOperationBrowser::is_description_visible() const {
	return description->is_visible();
}

void TetraOperationBrowser::focus_search() {
	search_edit->grab_focus();
	search_edit->select_all();
}

void TetraOperationBrowser::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_operations", "operations"), &TetraOperationBrowser::set_operations);
	ClassDB::bind_method(D_METHOD("get_operations"), &TetraOperationBrowser::get_operations);
	ClassDB::bind_method(D_METHOD("get_selected_id"), &TetraOperationBrowser::get_selected_id);
	ClassDB::bind_method(D_METHOD("get_selected_operation"), &TetraOperationBrowser::get_selected_operation);
	ClassDB::bind_method(D_METHOD("select_operation", "id"), &TetraOperationBrowser::select_operation);
	ClassDB::bind_method(D_METHOD("set_filter_text", "text"), &TetraOperationBrowser::set_filter_text);
	ClassDB::bind_method(D_METHOD("get_filter_text"), &TetraOperationBrowser::get_filter_text);
	ClassDB::bind_method(D_METHOD("set_description_visible", "visible"), &TetraOperationBrowser::set_description_visible);
	ClassDB::bind_method(D_METHOD("is_description_visible"), &TetraOperationBrowser::is_description_visible);
	ClassDB::bind_method(D_METHOD("focus_search"), &TetraOperationBrowser::focus_search);

	ADD_SIGNAL(MethodInfo("operation_selected", PropertyInfo(Variant::STRING, "id")));
	ADD_SIGNAL(MethodInfo("operation_activated", PropertyInfo(Variant::STRING, "id")));
}

TetraOperationBrowser::TetraOperationBrowser() {
	search_edit = memnew(FilterLineEdit);
	search_edit->set_placeholder(TTR("Search Operations"));
	search_edit->set_accessibility_name(TTR("Search Operations"));
	search_edit->set_clear_button_enabled(true);
	search_edit->connect(SceneStringName(text_changed), callable_mp(this, &TetraOperationBrowser::_search_text_changed));
	search_edit->connect(SceneStringName(text_submitted), callable_mp(this, &TetraOperationBrowser::_search_text_submitted));
	add_child(search_edit);

	tree = memnew(Tree);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_custom_minimum_size(Size2(0, 96 * EDSCALE));
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_ROW);
	tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	tree->connect(SceneStringName(item_selected), callable_mp(this, &TetraOperationBrowser::_item_selected));
	tree->connect("item_activated", callable_mp(this, &TetraOperationBrowser::_item_activated));
	tree->connect("item_collapsed", callable_mp(this, &TetraOperationBrowser::_item_collapsed));
	add_child(tree);
	search_edit->set_forward_control(tree);

	description = memnew(RichTextLabel);
	description->set_custom_minimum_size(Size2(0, 72 * EDSCALE));
	description->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	description->set_selection_enabled(true);
	description->set_focus_mode(FOCUS_NONE);
	add_child(description);
}
