extends "res://tetra_test.gd"
## Rendering through melt: presets, progress, results checked with ffprobe.

const V := TetraVideoProject.TRACK_VIDEO

var progress_log := []
var results := []


func probe_duration(path: String) -> Dictionary:
	var output := []
	var code := OS.execute("ffprobe", ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,codec_name:format=duration", "-of", "default=noprint_wrappers=1", path], output)
	var info := {"ok": code == 0}
	if code == 0:
		for line in str(output[0]).split("\n"):
			var pair := line.split("=")
			if pair.size() == 2:
				info[pair[0]] = pair[1]
	return info


func wait_for(renderer: TetraVideoRenderer, seconds: float) -> void:
	var deadline := Time.get_ticks_msec() + int(seconds * 1000)
	while results.is_empty() and Time.get_ticks_msec() < deadline:
		await process_frame
	check(not results.is_empty(), "the render finished within %d s" % seconds)
	check(not renderer.is_running(), "not running after finished")


func run() -> void:
	var presets := TetraVideoRenderer.get_presets()
	var parsed := presets.filter(func(p): return not p.builtin)
	print("render presets: %d built in, %d from Kdenlive" % [presets.size() - parsed.size(), parsed.size()])
	check(parsed.size() > 20, "more than 20 presets parsed from Kdenlive's profiles.xml: %d" % parsed.size())
	check_eq(presets.size() - parsed.size(), 3, "three built-in presets")
	var ids := {}
	for preset in presets:
		check(preset.id != "" and preset.name != "" and preset.group != "" and preset.extension != "" and preset.has("args"), "preset is complete: " + str(preset.id))
		check(not ids.has(preset.id), "preset id is unique: " + preset.id)
		ids[preset.id] = true
	var h264 := TetraVideoRenderer.get_preset("generic/MP4-H264/AAC")
	check_eq([h264.get("extension"), h264.get("group"), h264.get("default_quality"), h264.get("default_audio_bitrate")],
			["mp4", "Generic (HD for web, mobile devices...)", 23, 160], "Kdenlive's MP4 preset")
	var args := TetraVideoRenderer.expand_preset_arguments(h264, {"quality": 30, "audio_bitrate": 96, "threads": 2})
	check("crf=30" in args and "ab=96k" in args and "vcodec=libx264" in args and "acodec=aac" in args, "placeholders filled: %s" % str(args))
	check("preset=veryfast" in args, "default speed appended")
	check("threads=2" in args and "real_time=-2" in args, "thread options")
	check_eq(TetraVideoRenderer.get_preset("tetra/ogv").extension, "ogv", "built-in Theora preset")
	check_eq(TetraVideoRenderer.get_preset("nope"), {}, "unknown preset")

	# Two tracks of colour clips, one inverted: red|green below, a blue title bar clip above for the second half.
	var p := TetraVideoProject.new()
	p.set_profile(640, 360, 25, 1, 48000)
	p.add_track(V)
	p.add_track(V)
	var red := p.add_bin_color("Red", Color(1, 0, 0), 25)
	var blue := p.add_bin_color("Blue", Color(0, 0, 1), 25)
	var lower := p.insert_clip(red, 0, 0)
	p.insert_clip(blue, 1, 13, 0, 11)
	p.add_clip_effect(lower, "kdenlive:invert", "invert")
	check_eq(p.get_duration(), 25, "one second")

	var renderer := TetraVideoRenderer.new()
	renderer.progress.connect(func(frame, percent): progress_log.append([frame, percent]))
	renderer.finished.connect(func(ok, message): results.append([ok, message]))
	check_eq(renderer.start(p, tmp_dir().path_join("x.mp4"), "no-such-preset"), ERR_DOES_NOT_EXIST, "unknown preset refused")
	check_eq(renderer.start(TetraVideoProject.new(), tmp_dir().path_join("x.mp4"), "tetra/mp4"), ERR_INVALID_DATA, "empty timeline refused")
	if TetraVideoRenderer.find_melt() == "":
		print("melt is not installed; skipping the render tests")
		return
	var has_ffprobe := OS.execute("ffprobe", ["-version"], []) == 0

	for job in [["render.mp4", "generic/MP4-H264/AAC", "h264"], ["render.ogv", "tetra/ogv", "theora"]]:
		var path := tmp_dir().path_join(job[0])
		DirAccess.remove_absolute(path)
		progress_log.clear()
		results.clear()
		check_eq(renderer.start(p, path, job[1], {"width": 320, "height": 180}), OK, "start %s: %s" % [job[0], renderer.get_last_error()])
		check(renderer.is_running(), "running")
		check_eq(renderer.start(p, path, job[1]), ERR_BUSY, "a second start while running is refused")
		await wait_for(renderer, 60)
		if results.is_empty():
			printerr(renderer.get_command_line(), "\n", renderer.get_log())
			continue
		check(results[0][0], "%s rendered: %s\n%s" % [job[0], results[0][1], renderer.get_log()])
		check_eq(results[0][1], path, "finished reports the file")
		check(FileAccess.file_exists(path) and FileAccess.get_file_as_bytes(path).size() > 1000, job[0] + " exists")
		await process_frame
		if check(progress_log.size() >= 1, "progress was reported: %d times" % progress_log.size()):
			check_eq(progress_log[-1], [24, 100.0], "progress ends on the last frame at 100 %")
			check(progress_log[0][1] <= progress_log[-1][1], "progress grows")
		check_eq(renderer.get_percent(), 100.0, "100 % when done")
		if has_ffprobe:
			var info := probe_duration(path)
			check(info.ok, "ffprobe reads " + job[0])
			check_eq([info.get("codec_name"), info.get("width"), info.get("height")], [job[2], "320", "180"], "codec and size of " + job[0])
			check_near(float(info.get("duration", "0")), 1.0, 0.15, "duration of " + job[0])

	# The rendered pictures are the timeline's: cyan first, blue over it later.
	if TetraMltEngine.is_available() and FileAccess.file_exists(tmp_dir().path_join("render.mp4")):
		var engine := TetraMltEngine.new()
		var first := engine.get_thumbnail(tmp_dir().path_join("render.mp4"), 4, Vector2i(320, 180))
		var later := engine.get_thumbnail(tmp_dir().path_join("render.mp4"), 20, Vector2i(320, 180))
		if check(first != null and later != null, "frames of the rendered file"):
			var a := first.get_pixel(160, 90)
			var b := later.get_pixel(160, 90)
			check(a.r < 0.15 and a.g > 0.8 and a.b > 0.8, "rendered frame 4 is cyan (inverted red): %s" % a)
			check(b.r < 0.15 and b.g < 0.15 and b.b > 0.8, "rendered frame 20 is blue: %s" % b)
			first.save_png(ProjectSettings.globalize_path("res://../../../build/screens/video-render-frame4.png"))
			later.save_png(ProjectSettings.globalize_path("res://../../../build/screens/video-render-frame20.png"))

	# The rendered file as a source: video and sound come in as a linked pair and play through avformat.
	if TetraMltEngine.is_available() and FileAccess.file_exists(tmp_dir().path_join("render.mp4")):
		var q := TetraVideoProject.new()
		q.set_profile(320, 180, 25, 1, 48000)
		q.add_track(V)
		q.add_track(TetraVideoProject.TRACK_AUDIO)
		var info := TetraMltEngine.probe(tmp_dir().path_join("render.mp4"), q)
		check_eq([info.get("kind"), info.get("has_video"), info.get("has_audio"), info.get("length")], [TetraVideoProject.CLIP_VIDEO, true, true, 25], "probe of the rendered MP4")
		check_eq([info.get("width"), info.get("height"), info.get("fps")], [320, 180, 25.0], "probed picture format")
		var movie := TetraMltEngine.import_media(q, tmp_dir().path_join("render.mp4"))
		var video := q.insert_clip(movie, 1, 10)
		check(video > 0 and q.get_linked_clip(video) > 0, "the movie came in as a linked video and audio clip")
		check_eq(q.get_clip(q.get_linked_clip(video)).track, 0, "its sound is on the audio track")
		var movie_engine := TetraMltEngine.new()
		check_eq(movie_engine.set_project(q), OK, "a project with a media file builds")
		check_eq(movie_engine.get_length(), 35, "length with the movie at frame 10")
		var px_black := movie_engine.render_frame(5).get_pixel(160, 90)
		var px_cyan := movie_engine.render_frame(14).get_pixel(160, 90)
		var px_blue := movie_engine.render_frame(30).get_pixel(160, 90)
		check(px_black.r < 0.05 and px_black.g < 0.05 and px_black.b < 0.05, "before the movie: black")
		check(px_cyan.r < 0.15 and px_cyan.g > 0.8 and px_cyan.b > 0.8, "movie frame 4 on the timeline is cyan: %s" % px_cyan)
		check(px_blue.r < 0.15 and px_blue.g < 0.15 and px_blue.b > 0.8, "movie frame 20 on the timeline is blue: %s" % px_blue)

	# Part of the timeline only.
	var part := tmp_dir().path_join("render_part.mp4")
	results.clear()
	check_eq(renderer.start(p, part, "tetra/mp4", {"in": 5, "out": 14, "width": 320, "height": 180, "quality": 30}), OK, "partial render")
	await wait_for(renderer, 60)
	if not results.is_empty() and has_ffprobe:
		check_near(float(probe_duration(part).get("duration", "0")), 0.4, 0.1, "ten frames rendered")

	# Cancelling a long render stops melt and removes the partial file.
	var long_project := TetraVideoProject.new()
	long_project.add_track(V)
	long_project.insert_clip(long_project.add_bin_color("Red", Color(1, 0, 0), 20000), 0, 0)
	var long_path := tmp_dir().path_join("render_long.mp4")
	results.clear()
	check_eq(renderer.start(long_project, long_path, "tetra/mp4"), OK, "long render started")
	progress_log.clear()
	await create_timer(1.5).timeout
	check(renderer.is_running(), "still rendering")
	check(progress_log.size() >= 3, "a long render reports progress as it goes: %d times" % progress_log.size())
	if progress_log.size() >= 3:
		check(progress_log[-1][0] > progress_log[0][0] and progress_log[-1][1] > progress_log[0][1], "frame and percentage grow: %s .. %s" % [progress_log[0], progress_log[-1]])
		check(progress_log[-1][1] < 100.0, "not finished yet")
	renderer.cancel()
	await wait_for(renderer, 10)
	if not results.is_empty():
		check_eq(results[0][0], false, "a cancelled render reports failure")
		check(str(results[0][1]).contains("cancelled"), "and says why: " + str(results[0][1]))
	check(not FileAccess.file_exists(long_path), "the partial file is removed")
