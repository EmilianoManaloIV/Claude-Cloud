extends RefCounted
## The Effects tab with a search, and the found effect selected.
##
##   scripts/screenshot.sh build/screens/video-effects.png Video res://tetra_video/shots/effects_browser.gd


func drive(p_tree: SceneTree) -> void:
	var common: RefCounted = load("res://tetra_video/shots/video_common.gd").new()
	await common.setup(p_tree)
	var workspace: Object = common.workspace
	workspace.get_timeline().zoom_to_fit()
	workspace.select_clip(common.clips["blue"])
	workspace.seek(10)
	workspace.show_effects()
	var browser: Object = workspace.get_effects_browser()
	browser.set_filter_text("blur")
	await p_tree.process_frame
	browser.select_operation("kdenlive:boxblur")
	await common.settle(p_tree, 1.0)
