extends "res://tetra_test.gd"
## The timeline model: Kdenlive's editing rules, linked audio/video clips,
## and undo exactness under random operation sequences.

const V := TetraVideoProject.TRACK_VIDEO
const A := TetraVideoProject.TRACK_AUDIO


func ok(p: TetraVideoProject, what: String) -> void:
	check_eq(p.check_consistency(), "", "consistency after " + what)


func clip_span(p: TetraVideoProject, id: int) -> Array:
	var c := p.get_clip(id)
	return [c.track, c.position, c["in"], c.out]


func make_project() -> TetraVideoProject:
	var p := TetraVideoProject.new()
	p.add_track(V)  # V1
	p.add_track(V)  # V2
	p.add_track(A)  # A1 at the bottom
	p.add_track(A)  # A2 below A1
	return p


func run() -> void:
	test_profile()
	test_tracks_and_bin()
	test_clip_rules()
	test_linked_clips()
	test_range_operations()
	test_effects_markers_snapping()
	test_undo_groups_and_signals()
	test_random_sequences()


func test_profile() -> void:
	var p := TetraVideoProject.new()
	check_eq([p.get_width(), p.get_height(), p.get_fps_num(), p.get_fps_den(), p.get_sample_rate()], [1920, 1080, 25, 1, 48000], "default profile")
	check(p.set_profile_preset("atsc_720p_60"), "preset")
	check_eq([p.get_width(), p.get_height(), p.get_fps()], [1280, 720, 60.0], "720p60")
	check(p.set_profile_preset("atsc_1080p_2997"), "ntsc preset")
	check_near(p.get_fps(), 29.97003, 1e-4, "29.97")
	check(not p.set_profile_preset("nonsense"), "unknown preset refused")
	check(not p.set_profile(0, 10, 25, 1, 48000), "bad profile refused")
	check("uhd_2160p_30" in TetraVideoProject.get_profile_presets(), "preset list")
	p.undo()
	check_eq(p.get_fps(), 60.0, "undo profile")
	ok(p, "profile")


func test_tracks_and_bin() -> void:
	var p := make_project()
	check_eq(p.get_track_count(), 4, "track count")
	var names := []
	for i in 4:
		names.append(p.get_track_name(i))
	check_eq(names, ["A2", "A1", "V1", "V2"], "track order bottom to top")
	check_eq(p.get_paired_track(2), 1, "V1 pairs with A1")
	check_eq(p.get_paired_track(3), 0, "V2 pairs with A2")
	check_eq(p.get_paired_track(1), 2, "A1 pairs with V1")
	check_eq(p.get_paired_track(0), 3, "A2 pairs with V2")

	var red := p.add_bin_color("Red", Color.RED, 50)
	var media := p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Movie", "/tmp/movie.mp4", 200, true, true)
	var sound := p.add_bin_clip(TetraVideoProject.CLIP_AUDIO, "Song", "/tmp/song.wav", 300)
	check_eq([red, media, sound], [1, 2, 3], "bin ids")
	check_eq(p.get_bin_clip(red).source, "#ff0000ff", "colour payload")
	check_eq(p.get_bin_clip(sound).has_video, false, "audio clip has no video")
	check_eq(p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "x", "/x", 0), -1, "zero length refused")

	check_eq(p.insert_clip(red, 1, 0), -1, "colour on audio track refused")
	check_eq(p.insert_clip(sound, 2, 0), -1, "audio on video track refused")
	var c := p.insert_clip(red, 2, 10)
	check(c > 0, "insert colour")
	check(not p.remove_bin_clip(red), "used bin clip is not removed")
	check_eq(p.get_bin_clip_use_count(red), 1, "use count")

	# Moving tracks keeps clips on their track.
	check(p.move_track(2, 3), "move track up")
	check_eq(p.get_track_name(3), "V1", "moved track")
	check_eq(p.get_clip(c).track, 3, "clip follows its track")
	p.undo()
	check_eq(p.get_clip(c).track, 2, "undo track move")

	var extra := p.add_track(V, "Titles", 2)
	check_eq(extra, 2, "insert track index")
	check_eq(p.get_clip(c).track, 3, "clip index shifts with inserted track")
	check(p.set_track_locked(extra, true), "lock")
	check(not p.remove_track(extra), "locked track is not removed")
	p.set_track_locked(extra, false)
	check(p.remove_track(extra), "remove track")
	check_eq(p.get_clip(c).track, 2, "clip index shifts back")
	check(p.set_track_name(2, "Main") and p.set_track_hidden(2, true) and p.set_track_muted(1, true), "flags")
	check_eq(p.get_track(2), {"kind": V, "name": "Main", "hidden": true, "muted": false, "locked": false}, "track dictionary")

	check(p.remove_bin_clip(red, true), "cascade removal")
	check_eq(p.get_clip_ids().size(), 0, "cascade removed the clip")
	p.undo()
	check(p.has_clip(c) and p.has_bin_clip(red), "undo cascade")
	ok(p, "tracks and bin")


func test_clip_rules() -> void:
	var p := make_project()
	var red := p.add_bin_color("Red", Color.RED, 50)
	var media := p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Mute movie", "/tmp/m.mp4", 100, true, false)
	var a := p.insert_clip(media, 2, 0)            # 0..100
	var b := p.insert_clip(media, 2, 150, 10, 59)  # 150..200
	check_eq(clip_span(p, a), [2, 0, 0, 99], "default in/out")
	check_eq(clip_span(p, b), [2, 150, 10, 59], "explicit in/out")
	check_eq(p.insert_clip(media, 2, 99), -1, "overlap refused")
	check_eq(p.insert_clip(media, 2, 120, 0, 40), -1, "overlap with next refused")
	check_eq(p.insert_clip(media, 2, 100, 0, 100), -1, "out beyond source refused")
	check_eq(p.get_duration(), 200, "duration")
	check_eq(p.get_clip_at(2, 99), a, "clip at 99")
	check_eq(p.get_clip_at(2, 100), -1, "gap at 100")
	check_eq(p.get_clips_in_track(2), PackedInt32Array([a, b]), "clips in track")
	check(p.is_range_free(2, 100, 150), "free range")
	check(not p.is_range_free(2, 100, 151), "occupied range")
	check(p.is_range_free(2, 100, 160, [b]), "ignored clip")

	# Moves.
	check(not p.move_clip(b, 2, 50), "move onto another clip refused")
	check(p.move_clip(b, 2, 100), "move next to a clip")
	check(p.move_clip(b, 3, 20), "move to another track")
	check_eq(clip_span(p, b), [3, 20, 10, 59], "moved")
	check(not p.move_clip(b, 1, 20), "video clip on audio track refused")
	p.set_track_locked(3, true)
	check(not p.move_clip(b, 2, 150), "locked source track refuses")
	check(not p.move_clip(a, 3, 300), "locked target track refuses")
	check_eq(p.insert_clip(red, 3, 500), -1, "locked track refuses insert")
	check(not p.delete_clip(b), "locked track refuses delete")
	p.set_track_locked(3, false)
	check(p.move_clip(b, 2, 150), "move back")

	# Trims are clamped to the source and to the neighbours.
	check(p.trim_clip_start(b, 120), "trim start left")
	check_eq(clip_span(p, b), [2, 140, 0, 59], "start clamped to source frame 0")
	check(p.trim_clip_start(b, 170), "trim start right")
	check_eq(clip_span(p, b), [2, 170, 30, 59], "start trimmed")
	check(p.trim_clip_end(b, 1000), "trim end right")
	check_eq(clip_span(p, b), [2, 170, 30, 99], "end clamped to source length")
	check(p.trim_clip_end(b, 0), "trim end left")
	check_eq(clip_span(p, b), [2, 170, 30, 30], "one frame is always left")
	check(not p.trim_clip_end(a, 100), "no-op trim reports false")
	check(p.trim_clip_end(a, 60), "shorten a")
	check(p.move_clip(b, 2, 62), "b right after a")
	check(p.trim_clip_end(a, 90), "grow a towards b")
	check_eq(clip_span(p, a), [2, 0, 0, 61], "end clamped to the next clip")

	# Stills extend freely.
	var s := p.insert_clip(red, 3, 100)
	check(p.trim_clip_end(s, 1000), "extend a colour clip")
	check_eq(clip_span(p, s), [3, 100, 0, 899], "colour extended past its bin length")
	check(p.trim_clip_start(s, 40), "extend a colour clip to the left")
	check_eq(clip_span(p, s), [3, 40, 0, 959], "colour extended to the left")

	# Razor.
	var right := p.split_clip(s, 500)
	check(right > 0, "split")
	check_eq(clip_span(p, s), [3, 40, 0, 459], "left part")
	check_eq(clip_span(p, right), [3, 500, 460, 959], "right part")
	check_eq(p.split_clip(s, 40), -1, "split at the edge refused")
	check_eq(p.split_all_at(31).size(), 1, "split all cuts the clip under the frame")
	check_eq(p.get_clips_in_track(2).size(), 3, "three clips after razor")

	# Duplicate, delete, ripple delete.
	var d := p.duplicate_clip(right)
	check_eq(clip_span(p, d), [3, 1000, 460, 959], "duplicate goes right after the original")
	check_eq(p.duplicate_clip(s), -1, "duplicate refused when the space is taken")
	check(p.ripple_delete(right), "ripple delete")
	check_eq(clip_span(p, d), [3, 500, 460, 959], "later clip closed the gap")
	check(p.delete_clip(d), "delete")
	check(not p.has_clip(d), "deleted")
	ok(p, "clip rules")
	while p.can_undo():
		p.undo()
		ok(p, "undo of " + p.get_redo_name())
	check_eq(p.get_clip_ids().size(), 0, "everything undone")
	check_eq(p.get_track_count(), 0, "tracks undone")


func test_linked_clips() -> void:
	var p := make_project()
	var movie := p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Movie", "/tmp/movie.mp4", 200, true, true)
	var v := p.insert_clip(movie, 2, 10, 0, 99)
	var a := p.get_linked_clip(v)
	check(a > 0, "audio partner created")
	check_eq(clip_span(p, a), [1, 10, 0, 99], "partner on the paired audio track")
	check_eq(p.get_linked_clip(a), v, "link is mutual")
	check_eq(p.get_undo_name(), "Insert Clip", "one undo step for both")

	check(p.move_clip(v, 3, 30), "move video to V2")
	check_eq(clip_span(p, a), [0, 30, 0, 99], "audio follows to A2")
	check(p.move_clip(a, 1, 50), "move audio to A1")
	check_eq(clip_span(p, v), [2, 50, 0, 99], "video follows to V1")
	check(p.trim_clip_start(v, 60) and p.trim_clip_end(a, 120), "trim both ends")
	check_eq(clip_span(p, v), [2, 60, 10, 69], "video trimmed")
	check_eq(clip_span(p, a), [1, 60, 10, 69], "audio trimmed alike")

	var v2 := p.split_clip(v, 100)
	var a2 := p.get_linked_clip(v2)
	check(a2 > 0 and a2 != a, "right halves are linked to each other")
	check_eq(p.get_linked_clip(v), a, "left halves stay linked")
	check_eq(clip_span(p, a2), [1, 100, 50, 69], "audio split at the same frame")

	# A blocked partner blocks the whole operation.
	var song := p.add_bin_clip(TetraVideoProject.CLIP_AUDIO, "Song", "/tmp/song.wav", 300)
	var blocker := p.insert_clip(song, 1, 200, 0, 49)
	check(not p.move_clip(v2, 2, 210), "partner would overlap")
	check_eq(p.insert_clip(movie, 2, 220, 0, 9), -1, "insert refused when the audio side is taken")
	check(p.insert_clip(movie, 2, 220, 0, 9, false) > 0, "video-only insert")
	p.set_track_locked(1, true)
	check(not p.delete_clip(v2), "locked partner track refuses delete")
	check(not p.trim_clip_end(v2, 110), "locked partner track refuses trim")
	p.set_track_locked(1, false)

	check(p.unlink_clip(v2), "unlink")
	check_eq(p.get_linked_clip(a2), -1, "unlinked both sides")
	check(p.move_clip(v2, 2, 300), "unlinked video moves alone")
	check_eq(clip_span(p, a2)[1], 100, "audio stayed")
	check(p.link_clips(v2, a2), "link again")
	check(not p.link_clips(v2, blocker), "already linked")
	check(p.delete_clip(a2), "delete through the audio side")
	check(not p.has_clip(v2), "video died with it")
	check(p.ripple_delete(v), "ripple delete a linked pair")
	check_eq(p.get_clip(blocker).position, 160, "audio track closed up")
	ok(p, "linked clips")

	# Without a paired track only the video is inserted.
	var q := TetraVideoProject.new()
	q.add_track(V)
	var m := q.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Movie", "/tmp/movie.mp4", 200, true, true)
	var only := q.insert_clip(m, 0, 0)
	check(only > 0 and q.get_linked_clip(only) == -1, "no audio track, no partner")
	ok(q, "single track")


func test_range_operations() -> void:
	var p := make_project()
	var red := p.add_bin_color("Red", Color.RED, 100)
	var song := p.add_bin_clip(TetraVideoProject.CLIP_AUDIO, "Song", "/tmp/song.wav", 300)
	var c1 := p.insert_clip(red, 2, 0)        # V1 0..100
	var c2 := p.insert_clip(red, 2, 100)      # V1 100..200
	var c3 := p.insert_clip(red, 3, 50)       # V2 50..150
	var s1 := p.insert_clip(song, 1, 0)       # A1 0..300

	check(p.lift_range(80, 120), "lift")
	check_eq(clip_span(p, c1), [2, 0, 0, 79], "c1 cut at 80")
	check(not p.has_clip(c2), "the left part of c2 was inside the range")
	check_eq(clip_span(p, p.get_clip_at(2, 120)), [2, 120, 20, 99], "the rest of c2 starts at 120")
	check_eq(p.get_clip_at(3, 100), -1, "V2 lifted")
	check_eq(p.get_clips_in_track(3).size(), 2, "V2 clip cut in two")
	check_eq(p.get_clips_in_track(1).size(), 2, "audio cut in two")
	check_eq(p.get_duration(), 300, "lift keeps the duration")
	check(not p.lift_range(80, 120), "nothing left to lift")
	p.undo()
	check_eq(clip_span(p, c2), [2, 100, 0, 99], "undo lift")

	check(p.extract_range(80, 120), "extract")
	check_eq(clip_span(p, p.get_clip_at(2, 80)), [2, 80, 20, 99], "the rest of c2 closed up")
	check_eq(p.get_duration(), 260, "extract shortens the timeline")
	check_eq(p.get_undo_name(), "Extract Range", "extract is one step")
	p.undo()
	check_eq(p.get_duration(), 300, "undo extract")
	check_eq(p.get_clips_in_track(1), PackedInt32Array([s1]), "undo extract restores the audio clip")

	p.set_track_locked(1, true)
	check(p.extract_range(80, 120), "extract with a locked track")
	check_eq(clip_span(p, s1), [1, 0, 0, 299], "locked track untouched")
	check(not p.extract_range(0, 10, [1]), "explicit locked track refused")
	p.undo()
	p.set_track_locked(1, false)

	check(p.insert_space(100, 25), "insert space on all tracks")
	check_eq(p.get_clip(c2).position, 125, "c2 pushed")
	check_eq(p.get_clip(c3).position, 50, "clip before the position stays")
	check_eq(p.remove_space(110), 0, "position inside a clip on another track: refused")
	check_eq(p.remove_space(110, 2), 25, "remove the gap on one track")
	check_eq(p.get_clip(c2).position, 100, "gap closed")
	check(p.insert_space(0, 40, 3), "insert space on one track")
	check_eq(p.get_clip(c3).position, 90, "only that track moved")
	check_eq(p.get_clip(c1).position, 0, "other track unchanged")
	check_eq(p.remove_space(10, 3), 90, "remove the whole leading gap")
	check_eq(p.get_clip(c3).position, 0, "clip moved to the start")
	check_eq(p.remove_space(500, 3), 0, "no clip after the gap: nothing to do")
	ok(p, "range operations")


func test_effects_markers_snapping() -> void:
	var p := make_project()
	var red := p.add_bin_color("Red", Color.RED, 100)
	var c := p.insert_clip(red, 2, 20)
	check_eq(p.add_clip_effect(c, "kdenlive:invert", "invert"), 0, "first effect")
	check_eq(p.add_clip_effect(c, "", "greyscale"), 1, "second effect")
	check_eq(p.add_clip_effect(c, "mlt:brightness", "brightness", {"level": 0.5}, 0), 0, "insert at the front")
	var fx := p.get_clip_effects(c)
	check_eq([fx[0].service, fx[1].service, fx[2].service], ["brightness", "invert", "greyscale"], "effect order")
	check_eq(fx[2].operation, "mlt:greyscale", "default operation id")
	check(p.move_clip_effect(c, 0, 2), "move effect")
	check_eq(p.get_clip_effects(c)[2].values, {"level": 0.5}, "values kept")
	check(p.set_clip_effect_value(c, 2, "level", 0.25), "set one value")
	check(p.set_clip_effect_enabled(c, 0, false), "disable")
	check_eq(p.get_clip_effects(c)[0].enabled, false, "disabled")

	# Dictionaries handed out or in are copies.
	var values := {"level": 0.75}
	p.set_clip_effect_values(c, 2, values)
	values["level"] = 9.0
	p.get_clip_effects(c)[2].values["level"] = 8.0
	check_eq(p.get_clip_effects(c)[2].values, {"level": 0.75}, "stored values are isolated")
	p.undo()
	check_eq(p.get_clip_effects(c)[2].values, {"level": 0.25}, "undo values")
	check(p.remove_clip_effect(c, 1), "remove effect")
	check_eq(p.get_clip_effect_count(c), 2, "effect count")
	check(not p.remove_clip_effect(c, 5), "bad index refused")

	# Effects and fades survive a split on the right sides.
	check(p.set_clip_fades(c, 10, 500), "fades")
	check_eq([p.get_clip(c).fade_in, p.get_clip(c).fade_out], [10, 100], "fade clamped to the clip")
	p.set_clip_fades(c, 10, 20)
	var r := p.split_clip(c, 60)
	check_eq([p.get_clip(c).fade_in, p.get_clip(c).fade_out], [10, 0], "left keeps the fade in")
	check_eq([p.get_clip(r).fade_in, p.get_clip(r).fade_out], [0, 20], "right keeps the fade out")
	check_eq(p.get_clip_effect_count(r), 2, "effects copied to the right part")

	check(p.add_marker(75, "Chorus") and p.add_marker(5, "Intro"), "markers")
	check_eq(p.get_markers(), [{"frame": 5, "text": "Intro"}, {"frame": 75, "text": "Chorus"}], "markers sorted")
	check(p.move_marker(5, 8), "move marker")
	check(not p.move_marker(8, 75), "marker target taken")
	check(p.add_marker(75, "Refrain"), "retitle marker")
	check(p.remove_marker(8) and not p.remove_marker(8), "remove marker")
	check_eq(p.get_snap_points(), PackedInt32Array([0, 20, 60, 75, 120]), "snap points")
	check_eq(p.get_snap_points([r]), PackedInt32Array([0, 20, 60, 75]), "snap points ignoring a clip")
	check_eq(p.find_snap(63, 5), 60, "snap to the nearest")
	check_eq(p.find_snap(70, 5), 75, "snap to a marker")
	check_eq(p.find_snap(40, 5), -1, "nothing within tolerance")
	ok(p, "effects and markers")


func test_undo_groups_and_signals() -> void:
	var p := make_project()
	var counts := {"changed": 0, "structure": 0}
	p.changed.connect(func(): counts.changed += 1)
	p.structure_changed.connect(func(): counts.structure += 1)
	var red := p.add_bin_color("Red", Color.RED, 100)
	check_eq([counts.changed, counts.structure], [1, 1], "bin change is structural")
	var revision := p.get_revision()
	var c := p.insert_clip(red, 2, 0)
	check_eq([counts.changed, counts.structure], [2, 1], "clip change is not structural")
	check_eq(p.get_revision(), revision + 1, "revision increases")
	check_eq(p.insert_clip(red, 2, 0), -1, "refused")
	check_eq(p.get_revision(), revision + 1, "a refused operation changes nothing")
	check(p.get_last_error() != "", "a refusal explains itself")

	var before := p.get_state()
	var steps := p.get_undo_count()
	p.begin_undo_group("Drag Clip")
	for frame in [5, 9, 14, 30]:
		p.move_clip(c, 2, frame)
	p.end_undo_group()
	check_eq(p.get_undo_count(), steps + 1, "a group is one step")
	check_eq(p.get_undo_name(), "Drag Clip", "group name")
	check(p.undo(), "undo group")
	check_eq(p.get_state(), before, "group undone exactly")
	check(p.redo(), "redo group")
	check_eq(p.get_clip(c).position, 30, "group redone")
	p.begin_undo_group("Nothing")
	p.end_undo_group()
	check_eq(p.get_undo_count(), steps + 1, "an empty group leaves no step")
	check(p.undo() and p.undo(), "undo twice")
	check(p.insert_clip(red, 3, 0) > 0, "new action")
	check(not p.can_redo(), "a new action clears redo")
	check(p.get_revision() > revision, "revision never goes back")


func random_operation(p: TetraVideoProject, rng: RandomNumberGenerator) -> void:
	var ids := p.get_clip_ids()
	var id := ids[rng.randi() % ids.size()] if ids.size() > 0 else 0
	var tracks := p.get_track_count()
	var bins := p.get_bin_clip_ids()
	var frame := rng.randi_range(0, 400)
	match rng.randi() % 24:
		0, 1, 2, 3:
			p.insert_clip(bins[rng.randi() % bins.size()], rng.randi() % tracks, frame, rng.randi_range(-1, 20), rng.randi_range(-1, 90), rng.randi() % 4 != 0)
		4, 5:
			p.move_clip(id, rng.randi() % tracks, frame)
		6:
			p.trim_clip_start(id, frame)
		7:
			p.trim_clip_end(id, frame)
		8:
			p.split_clip(id, frame)
		9:
			p.split_all_at(frame)
		10:
			p.delete_clip(id)
		11:
			p.ripple_delete(id)
		12:
			p.lift_range(frame, frame + rng.randi_range(1, 80))
		13:
			p.extract_range(frame, frame + rng.randi_range(1, 80), [rng.randi() % tracks] if rng.randi() % 2 == 0 else [])
		14:
			p.insert_space(frame, rng.randi_range(1, 50), rng.randi_range(-1, tracks - 1))
		15:
			p.remove_space(frame, rng.randi_range(-1, tracks - 1))
		16:
			p.duplicate_clip(id, rng.randi_range(-1, tracks - 1), rng.randi_range(-1, 400))
		17:
			if rng.randi() % 2 == 0:
				p.unlink_clip(id)
			elif ids.size() > 1:
				p.link_clips(id, ids[rng.randi() % ids.size()])
		18:
			p.add_clip_effect(id, "", "brightness", {"level": rng.randf()})
		22:
			p.set_clip_fades(id, rng.randi_range(0, 30), rng.randi_range(0, 30))
		19:
			p.set_track_locked(rng.randi() % tracks, rng.randi() % 3 == 0)
		20:
			if tracks < 7:
				p.add_track(rng.randi() % 2, "", rng.randi_range(-1, tracks))
			else:
				p.remove_track(rng.randi() % tracks)
		21:
			p.move_track(rng.randi() % tracks, rng.randi() % tracks)
		23:
			p.add_marker(frame, "m")


func test_random_sequences() -> void:
	var rng := RandomNumberGenerator.new()
	var failures_before := _failures
	for round_index in 6:
		rng.seed = 1000 + round_index
		var p := make_project()
		p.set_max_undo_steps(1000)
		p.add_bin_color("Red", Color.RED, 60)
		p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Movie", "/tmp/movie.mp4", 120, true, true)
		p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Silent", "/tmp/silent.mp4", 90, true, false)
		p.add_bin_clip(TetraVideoProject.CLIP_AUDIO, "Song", "/tmp/song.wav", 150)
		p.add_bin_text("Title", "Hello", 40)
		p.clear_undo_history()
		var states := [p.get_state()]
		var names := []
		for step in 400:
			var revision := p.get_revision()
			var undo_count := p.get_undo_count()
			random_operation(p, rng)
			var problem := p.check_consistency()
			if problem != "":
				check(false, "round %d step %d: %s" % [round_index, step, problem])
				return
			if p.get_undo_count() > undo_count:
				check(p.get_undo_count() == undo_count + 1, "one operation, one step")
				states.append(p.get_state())
				names.append(p.get_undo_name())
			elif p.get_revision() != revision:
				check(false, "round %d step %d changed the document without an undo step" % [round_index, step])
		check(names.size() > 100, "round %d applied %d operations" % [round_index, names.size()])
		var final_xml := p.to_mlt_xml()

		# Walk the whole history back and forth: every state must come back exactly.
		for i in range(states.size() - 2, -1, -1):
			check(p.undo(), "undo")
			if p.get_state() != states[i] or p.check_consistency() != "":
				check(false, "round %d: undo of step %d (%s) is not exact" % [round_index, i, names[i]])
				return
		check(not p.can_undo(), "history exhausted")
		for i in range(1, states.size()):
			check(p.redo(), "redo")
			if p.get_state() != states[i]:
				check(false, "round %d: redo of step %d (%s) is not exact" % [round_index, i - 1, names[i - 1]])
				return
		check_eq(p.to_mlt_xml(), final_xml, "the XML after undo and redo is identical")
	check_eq(_failures, failures_before, "random sequences")
