#pragma once

#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"

class FilterLineEdit;
class RichTextLabel;
class Tree;
class TreeItem;

// A searchable list of the operations of the operation schema
// (docs/ARCHITECTURE.md). Without a search text the operations are shown in
// folders that follow their slash-separated "category"; with one, the matches
// are listed flat, best first. The description of the selected operation is
// shown below the list.
class TetraOperationBrowser : public VBoxContainer {
	GDCLASS(TetraOperationBrowser, VBoxContainer);

	Array operations;
	HashMap<String, int> index_by_id;
	String selected_id;
	// Folders the user opened or closed by hand, by category path; true when collapsed.
	HashMap<String, bool> folder_collapsed;
	// Set while this class changes the tree, so that the tree's signals are not taken for user actions.
	bool updating_tree = false;

	FilterLineEdit *search_edit = nullptr;
	Tree *tree = nullptr;
	RichTextLabel *description = nullptr;

	void _update_tree();
	void _add_category(TreeItem *p_parent, const Dictionary &p_category, bool p_collapsed_by_default, TreeItem *&r_selected);
	TreeItem *_add_operation(TreeItem *p_parent, int p_index, bool p_with_category);
	void _update_description();
	void _set_selected_from_user(const String &p_id);

	void _search_text_changed(const String &p_text);
	void _search_text_submitted(const String &p_text);
	void _item_selected();
	void _item_activated();
	void _item_collapsed(Object *p_item);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// An Array of operation Dictionaries. The selection is kept if its id still exists.
	void set_operations(const Array &p_operations);
	Array get_operations() const;

	// The id of the selected operation, or an empty String.
	String get_selected_id() const;
	Dictionary get_selected_operation() const;
	// Selects and reveals an operation without emitting operation_selected. An
	// unknown or empty id clears the selection.
	void select_operation(const String &p_id);

	void set_filter_text(const String &p_text);
	String get_filter_text() const;

	void set_description_visible(bool p_visible);
	bool is_description_visible() const;

	// Moves keyboard focus to the search field; arrow keys there move through the list.
	void focus_search();

	TetraOperationBrowser();
};
