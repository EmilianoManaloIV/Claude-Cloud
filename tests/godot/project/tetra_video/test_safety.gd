extends "res://tetra_test.gd"
## Guards against losing or misplacing work: failed saves, renders onto
## source files, scaled renders of pixel-valued effects and frame-rate changes.

const V := TetraVideoProject.TRACK_VIDEO
const A := TetraVideoProject.TRACK_AUDIO

var results := []


func run() -> void:
	test_failed_save_keeps_the_file()
	test_foreign_flag()
	test_frame_rate_change()
	test_track_uids()
	await test_render_guards()


## A save that cannot be written completely reports the error and leaves the previous file alone.
func test_failed_save_keeps_the_file() -> void:
	var p := TetraVideoProject.new()
	p.add_track(V)
	p.insert_clip(p.add_bin_color("Red", Color.RED, 50), 0, 0)
	var path := tmp_dir().path_join("tetra_video_save_guard.mlt")
	DirAccess.remove_absolute(path + ".tmp")
	check_eq(p.save(path), OK, "save")
	var good := FileAccess.get_file_as_string(path)
	check(good.length() > 100 and not FileAccess.file_exists(path + ".tmp"), "the file is written and no temporary file is left")
	p.add_marker(10, "more")
	if OS.get_name() != "Linux" or OS.execute("test", ["-c", "/dev/full"]) != 0:
		print("no /dev/full; skipping the failed-write check")
		return
	# The save goes through "<path>.tmp"; pointing that at /dev/full makes every write fail like a full disk.
	check_eq(OS.execute("ln", ["-s", "/dev/full", path + ".tmp"]), 0, "symlink to /dev/full")
	check(p.save(path) != OK, "a save that could not be written is an error")
	check(p.get_last_error() != "", "with a message: " + p.get_last_error())
	check_eq(FileAccess.get_file_as_string(path), good, "the previous file is intact")
	DirAccess.remove_absolute(path + ".tmp")
	check_eq(p.save(path), OK, "the next save works")
	check(FileAccess.get_file_as_string(path) != good, "and writes the new state")
	var q := TetraVideoProject.new()
	check_eq(q.load(path), OK, "which loads")
	check(q.get_state() == p.get_state(), "as the same project")


func test_foreign_flag() -> void:
	var p := TetraVideoProject.new()
	p.add_track(V)
	p.insert_clip(p.add_bin_color("Red", Color.RED, 50), 0, 0)
	check(not p.is_foreign(), "a new project is not foreign")
	var q := TetraVideoProject.new()
	check_eq(q.load_from_xml(p.to_mlt_xml()), OK, "load own XML")
	check(not q.is_foreign(), "a Tetra document is not foreign")
	check_eq(q.load_from_xml(p.to_mlt_xml({"tetra_properties": false})), OK, "load plain MLT XML")
	check(q.is_foreign(), "a document without Tetra's properties is foreign")
	var path := tmp_dir().path_join("tetra_video_foreign_flag.mlt")
	check_eq(q.save(path), OK, "save it")
	check(not q.is_foreign(), "once saved it is a Tetra project")


## Frames are counted at the project's frame rate, so a new rate converts every frame number.
func test_frame_rate_change() -> void:
	var p := TetraVideoProject.new()
	p.add_track(V)
	p.add_track(A, "", 0)
	var media := p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "ten seconds", "/nonexistent/ten.mp4", 250, true, true)
	var clip := p.insert_clip(media, 1, 50)  # with its audio partner
	var partner := p.get_linked_clip(clip)
	var part := p.insert_clip(media, 1, 300, 100, 149, false)
	p.set_clip_fades(part, 10, 20)
	var next := p.insert_clip(media, 1, 350, 0, 0, false)  # one frame, touching `part`
	p.add_marker(125, "five seconds")
	check(p.set_profile_preset("atsc_1080p_50"), "25 -> 50 fps")
	check_eq(p.check_consistency(), "", "consistent at 50 fps")
	check_eq(p.get_bin_clip(media).length, 500, "the media is 500 frames long at 50 fps")
	var c := p.get_clip(clip)
	check_eq([c.position, c["in"], c.out], [100, 0, 499], "the whole file is still on the timeline")
	var a := p.get_clip(partner)
	check_eq([a.position, a["in"], a.out], [100, 0, 499], "and so is its audio partner")
	c = p.get_clip(part)
	check_eq([c.position, c["in"], c.out, c.fade_in, c.fade_out], [600, 200, 299, 20, 40], "a trimmed clip keeps its seconds")
	check_eq(p.get_clip(next).position, 700, "the following clip still touches it")
	check_eq(p.get_markers()[0].frame, 250, "markers keep their time")
	check_eq(p.get_duration(), 702, "duration in the new frames")
	p.delete_clip(next)
	check(p.trim_clip_end(part, 2000), "the trimmed clip can be extended")
	check_eq(p.get_clip(part).out, 499, "up to the end of the media")
	p.undo()
	p.undo()
	check(p.set_profile_preset("atsc_1080p_25"), "50 -> 25 fps")
	check_eq(p.check_consistency(), "", "consistent back at 25 fps")
	c = p.get_clip(clip)
	check_eq([p.get_bin_clip(media).length, c.position, c["in"], c.out], [250, 50, 0, 249], "back to the 25 fps numbers")
	c = p.get_clip(part)
	check_eq([c.position, c["in"], c.out, c.fade_in, c.fade_out], [300, 100, 149, 10, 20], "also for the trimmed clip")
	check_eq([p.get_clip(next).position, p.get_clip(next).out], [350, 0], "a one-frame clip survives")
	# Halving again squeezes the one-frame clip; nothing may overlap or leave the media.
	check(p.set_profile(1920, 1080, 12, 1, 48000), "25 -> 12 fps")
	check_eq(p.check_consistency(), "", "consistent at 12 fps")
	for id in p.get_clip_ids():
		check(p.get_clip(id).out < p.get_bin_clip(media).length, "clip %d stays inside the media" % id)
	p.undo()
	check_eq([p.get_fps(), p.get_bin_clip(media).length, p.get_clip(part).position], [25.0, 250, 300], "undo restores rate and frames")
	# The picture size alone changes no frame number.
	check(p.set_profile(1280, 720, 25, 1, 48000), "size only")
	check_eq([p.get_bin_clip(media).length, p.get_clip(part).position], [250, 300], "frames untouched by a size change")


func test_track_uids() -> void:
	var p := TetraVideoProject.new()
	var v1 := p.add_track(V)
	var uid := p.get_track_uid(v1)
	check(uid > 0 and p.find_track_by_uid(uid) == v1, "a track has a uid")
	p.add_track(A)  # goes below
	check_eq(p.find_track_by_uid(uid), 1, "the uid follows the track")
	check(p.get_track_uid(0) != uid, "uids are unique")
	p.remove_track(1)
	check_eq(p.find_track_by_uid(uid), -1, "gone with the track")
	p.undo()
	check_eq(p.find_track_by_uid(uid), 1, "and back with undo")
	check_eq(p.load_from_xml(p.to_mlt_xml()), OK, "reload")
	check_eq(p.find_track_by_uid(uid), -1, "tracks of a loaded document are new tracks")
	check_eq([p.get_track_uid(-1), p.get_track_uid(9)], [0, 0], "no uid outside the list")


func wait_for(renderer: TetraVideoRenderer, seconds: float) -> void:
	var deadline := Time.get_ticks_msec() + int(seconds * 1000)
	while results.is_empty() and Time.get_ticks_msec() < deadline:
		await process_frame
	check(not results.is_empty(), "the render finished within %d s" % seconds)


## The picture of a rendered file at a frame.
func frame_of(path: String, frame: int, size: Vector2i) -> Image:
	var p := TetraVideoProject.new()
	var id := TetraMltEngine.import_media(p, path)
	if not check(id > 0, "the rendered file imports: " + path):
		return null
	p.set_profile(size.x, size.y, 25, 1, 48000)
	p.add_track(V)
	p.insert_clip(id, 0, 0)
	var engine := TetraMltEngine.new()
	engine.set_xml(p.to_mlt_xml())
	return engine.render_frame(frame)


func test_render_guards() -> void:
	if TetraVideoRenderer.find_melt() == "" or not TetraMltEngine.is_available():
		print("melt or MLT is missing; skipping the render guards")
		return
	var renderer := TetraVideoRenderer.new()
	renderer.finished.connect(func(ok, message): results.append([ok, message]))

	# A picture with a red and a blue half under effects whose values are project pixels: a scaled render
	# must show the same picture as the full one.
	var picture := Image.create(640, 360, false, Image.FORMAT_RGB8)
	picture.fill_rect(Rect2i(0, 0, 320, 360), Color.RED)
	picture.fill_rect(Rect2i(320, 0, 320, 360), Color.BLUE)
	var png := tmp_dir().path_join("tetra_video_halves.png")
	picture.save_png(png)
	for effect in [["kdenlive:pan_zoom", "transition.rect", "320 0 320 360"], ["kdenlive:crop", "left", 320]]:
		var operation := TetraVideoAssets.get_operation(effect[0], 640, 360)
		if operation.is_empty():
			print("%s is not available; skipping its scaled render check" % effect[0])
			continue
		var p := TetraVideoProject.new()
		p.set_profile(640, 360, 25, 1, 48000)
		p.add_track(V)
		var clip := p.insert_clip(TetraMltEngine.import_media(p, png), 0, 0, 0, 9)
		var values := TetraVideoAssets.get_default_values(operation)
		values[effect[1]] = effect[2]
		check(p.add_clip_effect(clip, effect[0], operation.service, values) >= 0, "add " + effect[0])
		var samples := []
		for divisor in [1, 2]:
			var path := tmp_dir().path_join("tetra_video_scaled_%d.mp4" % divisor)
			DirAccess.remove_absolute(path)
			results.clear()
			var options := {"quality": 15}
			if divisor > 1:
				options["width"] = 640 / divisor
				options["height"] = 360 / divisor
			check_eq(renderer.start(p, path, "tetra/mp4", options), OK, "render %s at 1/%d: %s" % [effect[0], divisor, renderer.get_last_error()])
			await wait_for(renderer, 120)
			if results.is_empty() or not check(results[0][0], "render at 1/%d succeeded: %s" % [divisor, results[0][1]]):
				continue
			var probe := []
			if OS.execute("ffprobe", ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,sample_aspect_ratio,field_order", "-of", "csv=p=0", path], probe) == 0:
				check_eq(probe[0].strip_edges(), "%d,%d,1:1,progressive" % [640 / divisor, 360 / divisor], "picture format of the 1/%d render" % divisor)
			var image := frame_of(path, 5, Vector2i(640, 360) / divisor)
			if check(image != null and image.get_size() == Vector2i(640, 360) / divisor, "frame of the 1/%d render" % divisor):
				var row := []
				for x in [1, 3, 5, 7]:
					row.append(image.get_pixel(image.get_width() * x / 8, image.get_height() / 2))
				samples.append(row)
		if samples.size() == 2:
			var same := true
			for i in 4:
				same = same and absf(samples[0][i].r - samples[1][i].r) < 0.2 and absf(samples[0][i].b - samples[1][i].b) < 0.2
			check(same, "%s: the half-size render shows the full-size picture: %s vs %s" % [effect[0], samples[0], samples[1]])
			check(samples[0][0] != samples[0][2] or samples[0][1] != samples[0][3], "the picture is not uniform")

	# Rendering onto one of the project's own source files is refused, and the file survives.
	var source := tmp_dir().path_join("tetra_video_scaled_1.mp4")
	if not FileAccess.file_exists(source):
		return
	var size := FileAccess.open(source, FileAccess.READ).get_length()
	var q := TetraVideoProject.new()
	q.add_track(V)
	q.insert_clip(TetraMltEngine.import_media(q, source), 0, 0)
	results.clear()
	var error := renderer.start(q, source.get_base_dir().path_join("./" + source.get_file()), "tetra/mp4", {"width": 320, "height": 180})
	check_eq(error, ERR_FILE_ALREADY_IN_USE, "a source clip is refused as the output: " + renderer.get_last_error())
	check(not renderer.is_running(), "no render was started")
	if renderer.is_running():
		renderer.cancel()
		await wait_for(renderer, 30)
	check(FileAccess.file_exists(source) and FileAccess.open(source, FileAccess.READ).get_length() == size, "the source file is intact")
