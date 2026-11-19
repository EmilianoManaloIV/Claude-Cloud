extends RefCounted
## Shared set-up of the Video workspace screenshot drivers: a small project
## with colour clips, a title and a generated test video on several tracks.

var workspace: Object
var project: TetraVideoProject
## Clip ids on the timeline by name.
var clips := {}
## Bin ids by name.
var bin := {}


static func tmp_dir() -> String:
	var dir := OS.get_cache_dir().path_join("tetra-tests")
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


## A 4 s test video with a sine tone, made by ffmpeg; "" when ffmpeg is missing.
static func make_test_video() -> String:
	var path := tmp_dir().path_join("tetra_video_shot.mp4")
	if FileAccess.file_exists(path):
		return path
	var output := []
	var code := OS.execute("ffmpeg", ["-y", "-loglevel", "error",
			"-f", "lavfi", "-i", "testsrc=duration=4:size=640x360:rate=25",
			"-f", "lavfi", "-i", "sine=frequency=440:duration=4",
			"-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", path], output, true)
	return path if code == 0 and FileAccess.file_exists(path) else ""


## Index of the n-th track (0 = lowest) of a kind, -1 when there is none.
func track_of(p_kind: int, p_nth: int) -> int:
	var found := []
	for i in project.get_track_count():
		if project.get_track_kind(i) == p_kind:
			found.append(i)
	# Tracks are named V1, V2... from the bottom; sort by name so the answer does not depend on index order.
	found.sort_custom(func(a: int, b: int) -> bool: return project.get_track_name(a) < project.get_track_name(b))
	return found[p_nth] if p_nth < found.size() else -1


func setup(p_tree: SceneTree) -> void:
	workspace = ClassDB.class_call_static("TetraVideoWorkspace", "get_singleton")
	workspace.new_project()
	project = workspace.get_project()

	bin["blue"] = workspace.add_color_clip("Blue", Color(0.15, 0.35, 0.8), 100)
	bin["orange"] = workspace.add_color_clip("Orange", Color(0.9, 0.5, 0.1), 75)
	bin["title"] = workspace.add_title_clip("Title", "Tetra Studio", 100)
	var video := make_test_video()
	if video != "":
		bin["video"] = workspace.import_file(video)

	var v1 := track_of(TetraVideoProject.TRACK_VIDEO, 0)
	var v2 := track_of(TetraVideoProject.TRACK_VIDEO, 1)
	clips["blue"] = project.insert_clip(bin["blue"], v1, 0)
	if bin.get("video", -1) >= 0:
		clips["video"] = project.insert_clip(bin["video"], v1, 100)
	clips["orange"] = project.insert_clip(bin["orange"], v1, 200)
	clips["title"] = project.insert_clip(bin["title"], v2, 25)
	project.add_marker(150, "Beat")
	await settle(p_tree)


## Waits until the engine shows the newest state of the project.
func settle(p_tree: SceneTree, p_seconds: float = 0.4) -> void:
	await p_tree.create_timer(p_seconds).timeout
	for i in 100:
		if not workspace.is_graph_pending():
			break
		await p_tree.create_timer(0.1).timeout
	await p_tree.create_timer(0.3).timeout
