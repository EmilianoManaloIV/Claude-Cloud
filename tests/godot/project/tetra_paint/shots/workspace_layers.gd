extends RefCounted
## Picture (a): the Image workspace with a three-layer painting.
##
##   scripts/screenshot.sh build/screens/image-workspace.png Image res://tetra_paint/shots/workspace_layers.gd


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var ws: Object = TetraImageWorkspace.get_singleton()
	var doc: TetraImageDocument = load("res://tetra_paint/shots/demo_scene.gd").build(ws)
	await p_tree.create_timer(0.5).timeout
	print("screenshot layers: %s, undo steps: %d, title '%s'" % [ws.get_layers_panel().get_listed_names(), doc.get_undo_names().size(), ws.get_title_text()])
