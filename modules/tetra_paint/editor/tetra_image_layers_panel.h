#pragma once

#include "../tetra_image_document.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/gui/box_container.h"

class Button;
class EditorSpinSlider;
class ImageTexture;
class OptionButton;
class Timer;
class Tree;
class TreeItem;

// The Layers panel of the Image workspace: the layer stack with the top layer
// first, as in GIMP. Each row has an eye, a thumbnail, the name (double-click
// renames) and the alpha lock; above the list are the mode and opacity of the
// active layer, below it the buttons that change the stack.
//
// Thumbnails are made lazily, a moment after the last change, and only for
// the layers that may have changed.
class TetraImageLayersPanel : public VBoxContainer {
	GDCLASS(TetraImageLayersPanel, VBoxContainer);

	enum {
		COLUMN_VISIBLE,
		COLUMN_NAME,
		COLUMN_LOCK,
	};

	Ref<TetraImageDocument> document;

	OptionButton *mode = nullptr;
	EditorSpinSlider *opacity = nullptr;
	Tree *tree = nullptr;
	Button *new_button = nullptr;
	Button *duplicate_button = nullptr;
	Button *raise_button = nullptr;
	Button *lower_button = nullptr;
	Button *merge_button = nullptr;
	Button *delete_button = nullptr;

	HashMap<int, Ref<ImageTexture>> thumbnails;
	HashSet<int> dirty_thumbnails;
	Timer *thumbnail_timer = nullptr;
	bool list_update_queued = false;
	bool updating = false;
	bool opacity_group_open = false;
	int redo_count = 0;

	void _queue_list_update();
	void _update_list();
	void _update_thumbnails();
	Ref<ImageTexture> _make_thumbnail(int p_layer_id) const;
	void _mark_thumbnail_dirty(int p_layer_id);
	void _mark_all_thumbnails_dirty();

	void _pixels_changed(const Rect2i &p_rect);
	void _undo_changed();
	void _item_selected();
	void _item_activated();
	void _item_edited();
	void _button_clicked(TreeItem *p_item, int p_column, int p_id, int p_mouse_button);
	void _mode_selected(int p_index);
	void _opacity_changed(double p_value);
	void _opacity_grabbed();
	void _opacity_ungrabbed();
	Button *_make_button(const String &p_tooltip, const Callable &p_action);
	String _unique_name(const String &p_base) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_document(const Ref<TetraImageDocument> &p_document);
	Ref<TetraImageDocument> get_document() const;

	// The stack operations of the buttons, on the active layer. Each is one undo step.
	void new_layer();
	void duplicate_layer();
	void raise_layer();
	void lower_layer();
	void merge_down();
	void delete_layer();

	// Layer names as listed, top layer first.
	PackedStringArray get_listed_names() const;
	Tree *get_tree() const { return tree; }

	TetraImageLayersPanel();
};
