extends RefCounted
## The Video workspace in use: clips on several tracks, one selected with two
## effects, the monitor showing the frame under the playhead.
##
##   scripts/screenshot.sh build/screens/video-editing.png Video res://tetra_video/shots/editing.gd


func drive(p_tree: SceneTree) -> void:
	var common: RefCounted = load("res://tetra_video/shots/video_common.gd").new()
	await common.setup(p_tree)
	var workspace: Object = common.workspace
	var target: int = common.clips.get("video", common.clips["blue"])
	workspace.select_clip(target)
	print("screenshot effects added: %d %d" % [workspace.add_effect_to_selection("kdenlive:sepia"),
			workspace.add_effect_to_selection("kdenlive:vignette")])
	workspace.show_effect_stack()
	workspace.get_timeline().zoom_to_fit()
	workspace.seek(130)
	await common.settle(p_tree, 1.5)
