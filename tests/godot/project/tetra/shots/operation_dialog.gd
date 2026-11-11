extends RefCounted
## Screenshot driver: a TetraOperationDialog for an operation that uses every
## parameter type, with the advanced section open.

const ShotData := preload("res://tetra/shots/shot_data.gd")


func drive(p_tree: SceneTree) -> void:
	# Dialogs are separate OS windows by default; embedded they are part of the captured viewport.
	p_tree.root.gui_embed_subwindows = true
	var op := ShotData.kitchen_sink()
	var problems := TetraSchema.validate_operation(op)
	if problems != "":
		printerr("SCRIPT ERROR: the made-up operation is invalid: " + problems)

	var dialog := TetraOperationDialog.new()
	dialog.set_preview_enabled(true)
	EditorInterface.get_base_control().add_child(dialog)
	dialog.get_param_panel().set_advanced_folded(false)
	dialog.popup_operation(op, {"iterations": 7, "radius": 250.0})
	await p_tree.create_timer(0.5).timeout
	print("screenshot dialog size %s, values %s" % [dialog.size, dialog.get_values()])
