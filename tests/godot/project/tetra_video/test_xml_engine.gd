extends "res://tetra_test.gd"
## MLT XML export, the project file format, and frames pulled from the real
## engine for a timeline of colour clips.

const V := TetraVideoProject.TRACK_VIDEO
const A := TetraVideoProject.TRACK_AUDIO
const SCREENS := "res://../../../build/screens/"


func check_pixel(image: Image, expected: Color, what: String, tolerance := 0.02) -> void:
	if not check(image != null, what + ": no image"):
		return
	var c := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
	var corner := image.get_pixel(3, 3)
	check(absf(c.r - expected.r) <= tolerance and absf(c.g - expected.g) <= tolerance and absf(c.b - expected.b) <= tolerance,
			"%s: expected %s at the centre, got %s" % [what, expected, c])
	check(absf(corner.r - expected.r) <= tolerance and absf(corner.g - expected.g) <= tolerance and absf(corner.b - expected.b) <= tolerance,
			"%s: expected %s in the corner, got %s" % [what, expected, corner])


func save_png(image: Image, name: String) -> void:
	if image != null:
		image.save_png(ProjectSettings.globalize_path(SCREENS).path_join(name))


## V1: red 0..25, gap, green 50..75.  V2: blue 60..80 over the green.
func make_project() -> TetraVideoProject:
	var p := TetraVideoProject.new()
	p.set_profile(640, 360, 25, 1, 48000)
	p.add_track(V)
	p.add_track(V)
	p.add_track(A)
	var red := p.add_bin_color("Red", Color(1, 0, 0), 25)
	var green := p.add_bin_color("Green", Color(0, 1, 0), 25)
	var blue := p.add_bin_color("Blue", Color(0, 0, 1), 20)
	p.insert_clip(red, 1, 0)
	p.insert_clip(green, 1, 50)
	p.insert_clip(blue, 2, 60)
	return p


func run() -> void:
	test_xml_text()
	test_round_trip()
	test_foreign_and_malformed()
	if not check(TetraMltEngine.is_available(), "the MLT engine is built in"):
		return
	test_engine_services()
	test_frames()
	test_effects_and_fades()
	test_stills_and_titles()
	test_melt_accepts_the_file()


func test_xml_text() -> void:
	var p := make_project()
	var xml := p.to_mlt_xml()
	check_eq(p.to_mlt_xml(), xml, "the XML is deterministic")
	check(xml.begins_with("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<mlt LC_NUMERIC=\"C\""), "XML header")
	check(xml.contains("<profile description=\"640x360 25 fps\" width=\"640\" height=\"360\""), "profile element")
	check(xml.contains("frame_rate_num=\"25\" frame_rate_den=\"1\""), "frame rate")
	check(xml.contains("<producer id=\"black\" in=\"0\" out=\"79\">"), "black background as long as the project")
	check(xml.contains("<property name=\"resource\">0xff0000ff</property>"), "colour producer")
	check(xml.contains("<blank length=\"25\"/>"), "gap between red and green")
	check(xml.contains("<blank length=\"60\"/>"), "gap before blue")
	check(xml.contains("<track producer=\"track1\" hide=\"audio\"/>"), "video tracks never play sound")
	check(xml.contains("<track producer=\"track0\" hide=\"video\"/>"), "audio tracks never show pictures")
	check_eq(xml.count("<property name=\"mlt_service\">mix</property>"), 1, "one mix per audio track")
	check_eq(xml.count("<property name=\"always_active\">1</property>"), 3, "every track has an always-active transition")
	p.set_track_hidden(1, true)
	p.set_track_muted(0, true)
	xml = p.to_mlt_xml()
	check(xml.contains("<track producer=\"track1\" hide=\"both\"/>") and xml.contains("<track producer=\"track0\" hide=\"both\"/>"), "hidden and muted tracks")
	var plain := p.to_mlt_xml({"tetra_properties": false, "width": 320, "height": 180, "compositing": "composite"})
	check(not plain.contains("tetra:"), "export without tetra properties")
	check(plain.contains("width=\"320\" height=\"180\""), "profile override")
	check(plain.contains("<property name=\"mlt_service\">composite</property>"), "compositing override")


func test_round_trip() -> void:
	var p := make_project()
	p.add_track(A)
	var movie := p.add_bin_clip(TetraVideoProject.CLIP_VIDEO, "Movie <1> & \"co\"", "/tmp/does not exist.mp4", 200, true, true)
	var title := p.add_bin_text("Title", "Hello\nWorld <&>", 50, {"family": "Serif", "size": 64})
	var v := p.insert_clip(movie, 1, 100, 10, 59)
	p.insert_clip(title, 2, 100)
	p.split_clip(v, 120)
	p.add_clip_effect(v, "kdenlive:invert", "invert")
	p.add_clip_effect(v, "mlt:brightness", "brightness", {"level": 0.5, "tint": Color(1, 0.5, 0.25, 1), "name": "a \"quoted\" <text>", "on": true, "count": 3})
	p.set_clip_effect_enabled(v, 0, false)
	p.set_clip_fades(v, 5, 0)
	p.set_clip_fades(p.get_linked_clip(v), 5, 0)
	p.add_marker(12, "First & foremost")
	p.add_marker(99)
	p.set_track_locked(2, true)
	p.set_track_name(1, "Main <video>")
	p.delete_clip(p.get_clip_at(1, 55))
	check_eq(p.check_consistency(), "", "source project")

	var path := tmp_dir().path_join("round_trip.mlt")
	check_eq(p.save(path), OK, "save")
	var q := TetraVideoProject.new()
	check_eq(q.load(path), OK, "load: " + q.get_last_error())
	check_eq(q.check_consistency(), "", "loaded project")
	check_eq(q.get_state(), p.get_state(), "the loaded model equals the saved one")
	check_eq(q.to_mlt_xml(), p.to_mlt_xml(), "saving again gives the same file")
	check(not q.can_undo(), "a loaded project starts with an empty history")
	check_eq(q.load(tmp_dir().path_join("missing.mlt")), ERR_FILE_NOT_FOUND, "missing file")


func test_foreign_and_malformed() -> void:
	# A hand-written MLT document, the kind melt and Shotcut produce.
	var foreign := """<?xml version="1.0"?>
<mlt>
  <profile width="1280" height="720" frame_rate_num="30" frame_rate_den="1"/>
  <producer id="bg" in="0" out="99"><property name="mlt_service">color</property><property name="resource">black</property></producer>
  <producer id="clipA" in="0" out="299"><property name="resource">/media/a.mp4</property></producer>
  <producer id="colour" in="0" out="49"><property name="mlt_service">color</property><property name="resource">0x00ff00ff</property></producer>
  <producer id="music"><property name="resource">/media/song.mp3</property><property name="length">00:00:10.000</property></producer>
  <playlist id="background"><entry producer="bg" in="0" out="99"/></playlist>
  <playlist id="video">
    <entry producer="clipA" in="10" out="39"/>
    <blank length="20"/>
    <entry producer="colour" in="0" out="24"><filter><property name="mlt_service">greyscale</property></filter></entry>
    <entry producer="clipA" in="100" out="149"/>
  </playlist>
  <playlist id="audio"><blank length="00:00:01.000"/><entry producer="music" in="0" out="89"/></playlist>
  <tractor id="tractor0">
    <track producer="background"/>
    <track producer="video"/>
    <track producer="audio" hide="video"/>
    <transition><property name="a_track">0</property><property name="b_track">2</property><property name="mlt_service">mix</property></transition>
  </tractor>
</mlt>"""
	var p := TetraVideoProject.new()
	check_eq(p.load_from_xml(foreign), OK, "foreign MLT XML: " + p.get_last_error())
	check_eq(p.check_consistency(), "", "foreign project is consistent")
	check_eq([p.get_width(), p.get_height(), p.get_fps()], [1280, 720, 30.0], "foreign profile")
	check_eq(p.get_track_count(), 2, "the black background is not a track")
	check_eq([p.get_track_kind(0), p.get_track_kind(1)], [V, A], "track kinds")
	check_eq(p.get_bin_clip_ids().size(), 3, "one bin clip per distinct producer")
	var spans := []
	for id in p.get_clips_in_track(0):
		var c := p.get_clip(id)
		spans.append([c.position, c["in"], c.out])
	check_eq(spans, [[0, 10, 39], [50, 0, 24], [75, 100, 149]], "entries and blanks became clips")
	var colour := p.get_clip(p.get_clip_at(0, 60))
	check_eq(p.get_bin_clip(colour.bin_id).kind, TetraVideoProject.CLIP_COLOR, "colour producer")
	check_eq(p.get_bin_clip(colour.bin_id).source, "#00ff00ff", "colour value")
	check_eq(colour.effects.size(), 1, "entry filter became an effect")
	check_eq(colour.effects[0].operation, "mlt:greyscale", "effect operation id")
	var song := p.get_clip(p.get_clips_in_track(1)[0])
	check_eq([song.position, song.out], [30, 89], "clock values are converted with the frame rate")
	check_eq(p.get_bin_clip(song.bin_id).kind, TetraVideoProject.CLIP_AUDIO, "media on an audio track is audio")
	check_eq(p.get_bin_clip(song.bin_id).length, 300, "length from a clock value")

	var before := p.get_state()
	check_eq(p.load_from_xml("<mlt><playlist id=\"a\"><entry producer=\"x\"/></playlist>"), ERR_PARSE_ERROR, "unclosed element")
	check(p.get_last_error().contains("never closed"), "error names the problem: " + p.get_last_error())
	check_eq(p.load_from_xml("<mlt><playlist></tractor></mlt>"), ERR_PARSE_ERROR, "mismatched tag")
	check_eq(p.load_from_xml("this is not xml"), ERR_FILE_UNRECOGNIZED, "not XML")
	check_eq(p.load_from_xml("<html><body/></html>"), ERR_FILE_UNRECOGNIZED, "not MLT")
	check_eq(p.load_from_xml("<mlt><profile width=\"0\" height=\"0\"/></mlt>"), ERR_INVALID_DATA, "bad profile")
	check_eq(p.load_from_xml("<mlt><playlist id=\"a\"><entry producer=\"ghost\"/></playlist></mlt>"), ERR_INVALID_DATA, "missing producer")
	check(p.get_last_error().contains("ghost"), "error names the producer")
	check_eq(p.load_from_xml("<mlt></mlt>"), ERR_INVALID_DATA, "nothing to load")
	check_eq(p.load_from_xml(""), ERR_PARSE_ERROR, "empty document")
	check_eq(p.get_state(), before, "a failed load leaves the project untouched")


func test_engine_services() -> void:
	check(TetraMltEngine.get_version().begins_with("7."), "MLT 7: " + TetraMltEngine.get_version())
	check(TetraMltEngine.list_services("filter").size() > 100, "filters listed")
	check("color" in TetraMltEngine.list_services("producer"), "colour producer listed")
	check("mix" in TetraMltEngine.list_services("transition"), "mix transition listed")
	check("avformat" in TetraMltEngine.list_services("consumer"), "avformat consumer listed")
	check_eq(TetraMltEngine.list_services("nonsense").size(), 0, "unknown kind")
	check(TetraMltEngine.has_service("filter", "brightness") and not TetraMltEngine.has_service("filter", "no-such-filter"), "has_service")
	var op := TetraMltEngine.get_service_operation("filter", "brightness")
	check_eq([op.id, op.source, op.service, op.kind], ["mlt:brightness", "mlt", "brightness", "video_effect"], "operation header")
	check_eq(op.label, "Brightness", "operation label")
	var level := {}
	for param in op.params:
		if param.name == "level":
			level = param
	check_eq([level.get("type"), level.get("min"), level.get("max"), level.get("animated")], ["float", 0.0, 15.0, true], "level parameter from the metadata")
	check_eq(level.get("no_default"), true, "a parameter without a default in the metadata says so")
	check_eq(TetraMltEngine.get_service_operation("filter", "volume").kind, "audio_effect", "audio filter kind")
	check_eq(TetraMltEngine.get_service_operation("filter", "no-such-filter"), {}, "unknown service")


func test_frames() -> void:
	var p := make_project()
	var engine := TetraMltEngine.new()
	check(not engine.has_graph() and engine.render_frame(0) == null, "no graph yet")
	check_eq(engine.set_project(p), OK, "the engine accepts the XML")
	check_eq(engine.get_length(), p.get_duration(), "engine length equals the project duration")
	check_eq(engine.get_length(), 80, "80 frames")

	var frame := engine.render_frame(10)
	check_eq([frame.get_width(), frame.get_height(), frame.get_format()], [640, 360, Image.FORMAT_RGBA8], "full-size RGBA frame")
	check_pixel(frame, Color(1, 0, 0), "frame 10 is red")
	check_eq(frame.get_pixel(320, 180).a, 1.0, "opaque")
	check_pixel(engine.render_frame(24), Color(1, 0, 0), "frame 24 is still red")
	check_pixel(engine.render_frame(25), Color(0, 0, 0), "frame 25 is the gap: black")
	check_pixel(engine.render_frame(49), Color(0, 0, 0), "frame 49 is black")
	check_pixel(engine.render_frame(50), Color(0, 1, 0), "frame 50 is green")
	check_pixel(engine.render_frame(59), Color(0, 1, 0), "frame 59 is green")
	check_pixel(engine.render_frame(60), Color(0, 0, 1), "frame 60: the upper track covers the lower")
	check_pixel(engine.render_frame(77), Color(0, 0, 1), "frame 77: blue over black")
	check_pixel(engine.render_frame(10), Color(1, 0, 0), "seeking back")

	var small := engine.render_frame(55, Vector2i(320, 200))
	check_eq([small.get_width(), small.get_height()], [320, 180], "scaled to fit, aspect kept")
	check_pixel(small, Color(0, 1, 0), "scaled frame keeps its colour")
	save_png(small, "video-frame-green-320.png")

	# Edits reach the engine by setting the project again.
	p.set_track_hidden(2, true)
	engine.set_project(p)
	check_pixel(engine.render_frame(65), Color(0, 1, 0), "hidden upper track reveals the lower one")
	p.undo()
	p.move_clip(p.get_clip_at(2, 60), 2, 30)
	engine.set_project(p)
	check_pixel(engine.render_frame(35), Color(0, 0, 1), "moved clip")
	check_eq(engine.get_length(), 75, "length follows the edit")
	check_eq(engine.set_xml("<mlt><nonsense"), ERR_INVALID_DATA, "broken XML is refused")
	check_pixel(engine.render_frame(35), Color(0, 0, 1), "the previous graph stays after a refused one")


func test_effects_and_fades() -> void:
	var p := make_project()
	var red := p.get_clip_at(1, 0)
	var green := p.get_clip_at(1, 50)
	var blue := p.get_clip_at(2, 60)
	var engine := TetraMltEngine.new()
	p.add_clip_effect(red, "mlt:invert", "invert")
	engine.set_project(p)
	check_pixel(engine.render_frame(10), Color(0, 1, 1), "invert turns red into cyan", 0.03)
	save_png(engine.render_frame(10), "video-frame-inverted.png")
	p.set_clip_effect_enabled(red, 0, false)
	engine.set_project(p)
	check_pixel(engine.render_frame(10), Color(1, 0, 0), "a disabled effect does nothing")

	p.add_clip_effect(green, "mlt:greyscale", "greyscale")
	engine.set_project(p)
	var grey := engine.render_frame(55).get_pixel(320, 180)
	check(absf(grey.r - grey.g) < 0.02 and absf(grey.g - grey.b) < 0.02 and grey.g > 0.4 and grey.g < 0.95, "greyscale removes the colour: %s" % grey)

	p.add_clip_effect(red, "mlt:brightness", "brightness", {"level": 0.5})
	engine.set_project(p)
	check_pixel(engine.render_frame(10), Color(0.5, 0, 0), "brightness 0.5 halves red", 0.06)
	p.remove_clip_effect(red, 1)
	p.remove_clip_effect(green, 0)

	# Fades are clip-relative animations.
	p.set_clip_fades(red, 10, 5)
	engine.set_project(p)
	check_pixel(engine.render_frame(0), Color(0, 0, 0), "fade in starts black", 0.04)
	check_pixel(engine.render_frame(5), Color(0.5, 0, 0), "fade in is half way at frame 5", 0.05)
	check_pixel(engine.render_frame(10), Color(1, 0, 0), "fade in is over at frame 10", 0.04)
	check_pixel(engine.render_frame(18), Color(1, 0, 0), "full level in the middle", 0.04)
	check_pixel(engine.render_frame(24), Color(0, 0, 0), "fade out ends black", 0.04)
	save_png(engine.render_frame(5), "video-frame-fade.png")

	# On an upper track a fade is a dissolve onto what is below.
	p.set_clip_fades(blue, 10, 0)
	engine.set_project(p)
	check_pixel(engine.render_frame(60), Color(0, 1, 0), "upper fade starts transparent: green shows", 0.05)
	var mixed := engine.render_frame(65).get_pixel(320, 180)
	check(mixed.g > 0.3 and mixed.g < 0.7 and mixed.b > 0.3 and mixed.b < 0.7 and mixed.r < 0.05, "half way the two tracks are mixed: %s" % mixed)
	check_pixel(engine.render_frame(72), Color(0, 0, 1), "upper fade done: blue", 0.05)
	save_png(engine.render_frame(65), "video-frame-dissolve.png")


func test_stills_and_titles() -> void:
	var image := Image.create(160, 90, false, Image.FORMAT_RGBA8)
	image.fill(Color(1, 1, 0))
	image.fill_rect(Rect2i(0, 0, 80, 90), Color(1, 0, 1))
	var image_path := tmp_dir().path_join("still.png")
	image.save_png(image_path)

	var p := TetraVideoProject.new()
	p.set_profile(640, 360, 25, 1, 48000)
	p.add_track(V)
	p.add_track(V)
	var still := p.add_bin_clip(TetraVideoProject.CLIP_IMAGE, "Still", image_path, 50)
	var title := p.add_bin_text("Title", "Tetra Studio", 50)
	var s := p.insert_clip(still, 0, 0)
	p.trim_clip_end(s, 100)
	p.insert_clip(title, 1, 20)
	var engine := TetraMltEngine.new()
	check_eq(engine.set_project(p), OK, "project with a still and a title")
	check_eq(engine.get_length(), 100, "a still extended past its default length")
	var frame := engine.render_frame(90)
	check(frame != null, "still frame")
	if frame != null:
		var left := frame.get_pixel(100, 180)
		var right := frame.get_pixel(540, 180)
		check(left.r > 0.9 and left.g < 0.1 and left.b > 0.9, "left half of the still is magenta: %s" % left)
		check(right.r > 0.9 and right.g > 0.9 and right.b < 0.1, "right half of the still is yellow: %s" % right)
	var titled := engine.render_frame(30)
	save_png(titled, "video-frame-title.png")
	if check(titled != null and frame != null, "title frame"):
		var white := 0
		for y in range(0, 360, 2):
			for x in range(0, 640, 2):
				var c := titled.get_pixel(x, y)
				if c.r > 0.9 and c.g > 0.9 and c.b > 0.9:
					white += 1
		check(white > 200, "the title draws white text over the still (%d white samples)" % white)
		check(titled.get_pixel(5, 5).is_equal_approx(frame.get_pixel(5, 5)), "the title is transparent around the text")

	# Thumbnails of files and of bin clips.
	var thumb := engine.get_thumbnail(image_path, 0, Vector2i(160, 90))
	if check(thumb != null, "thumbnail of a file"):
		check_eq([thumb.get_width(), thumb.get_height()], [160, 90], "thumbnail size")
		check(thumb.get_pixel(20, 45).b > 0.9 and thumb.get_pixel(140, 45).b < 0.1, "thumbnail content")
	var colour_thumb := engine.get_thumbnail({"kind": TetraVideoProject.CLIP_COLOR, "source": "#0000ffff", "length": 10}, 0, Vector2i(64, 36))
	if check(colour_thumb != null, "thumbnail of a bin clip"):
		check(colour_thumb.get_pixel(32, 18).b > 0.95 and colour_thumb.get_pixel(32, 18).r < 0.05, "blue thumbnail")
	check(engine.get_thumbnail("/no/such/file.mp4", 0, Vector2i(64, 36)) == null, "thumbnail of a missing file")

	var info := TetraMltEngine.probe(image_path, p)
	check_eq([info.get("kind"), info.get("has_video"), info.get("has_audio"), info.get("length")], [TetraVideoProject.CLIP_IMAGE, true, false, 125], "probe of an image")
	check_eq([info.get("width"), info.get("height")], [160, 90], "probed size")
	check_eq(TetraMltEngine.probe("/no/such/file.mp4"), {}, "probe of a missing file")
	var imported := TetraMltEngine.import_media(p, image_path)
	check(imported > 0 and p.get_bin_clip(imported).name == "still.png", "import_media adds a bin clip")


func test_melt_accepts_the_file() -> void:
	var p := make_project()
	p.add_clip_effect(p.get_clip_at(1, 0), "mlt:invert", "invert")
	p.set_clip_fades(p.get_clip_at(1, 50), 5, 5)
	var path := tmp_dir().path_join("melt_check.mlt")
	p.save(path)
	var output := []
	var code := OS.execute("melt", [path, "-consumer", "xml"], output)
	if code == -1:
		print("melt is not installed; skipping the command-line check")
		return
	check_eq(code, 0, "melt accepts the project file")
	check(str(output[0]).contains("<tractor"), "melt parsed the tractor")
