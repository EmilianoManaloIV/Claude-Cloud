extends RefCounted
## The Render dialog over the Video workspace.
##
##   scripts/screenshot.sh build/screens/video-render.png Video res://tetra_video/shots/render_dialog.gd


func drive(p_tree: SceneTree) -> void:
	p_tree.root.gui_embed_subwindows = true
	var common: RefCounted = load("res://tetra_video/shots/video_common.gd").new()
	await common.setup(p_tree)
	var workspace: Object = common.workspace
	workspace.get_timeline().zoom_to_fit()
	workspace.get_timeline().set_zone(50, 200)
	workspace.seek(60)
	workspace.open_render_dialog()
	await common.settle(p_tree, 1.0)
