extends "res://tetra_test.gd"
## Real-time playback on the engine thread: position follows the clock,
## frames reach the main thread, sound is pulled at normal speed.

const V := TetraVideoProject.TRACK_VIDEO
const A := TetraVideoProject.TRACK_AUDIO

var frames_seen := []
var finished := 0


## Three seconds of a 440 Hz tone, 16-bit mono at 48 kHz.
func write_tone(path: String) -> void:
	var samples := 48000 * 3
	var data := StreamPeerBuffer.new()
	data.put_data("RIFF".to_ascii_buffer())
	data.put_u32(36 + samples * 2)
	data.put_data("WAVEfmt ".to_ascii_buffer())
	data.put_u32(16)
	data.put_u16(1)
	data.put_u16(1)
	data.put_u32(48000)
	data.put_u32(96000)
	data.put_u16(2)
	data.put_u16(16)
	data.put_data("data".to_ascii_buffer())
	data.put_u32(samples * 2)
	for i in samples:
		data.put_16(int(sin(i * TAU * 440.0 / 48000.0) * 16000.0))
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(data.data_array)
	file.close()


func wait(seconds: float) -> void:
	await create_timer(seconds).timeout


func run() -> void:
	if not check(TetraMltEngine.is_available(), "the MLT engine is built in"):
		return
	var tone := tmp_dir().path_join("tone.wav")
	write_tone(tone)

	var p := TetraVideoProject.new()
	p.set_profile(640, 360, 25, 1, 48000)
	p.add_track(V)
	p.add_track(A)
	var info := TetraMltEngine.probe(tone, p)
	check_eq([info.get("kind"), info.get("has_video"), info.get("has_audio")], [TetraVideoProject.CLIP_AUDIO, false, true], "probe of a sound file")
	check_eq([info.get("length"), info.get("sample_rate"), info.get("channels")], [75, 48000, 1], "probed length and format")
	var sound := TetraMltEngine.import_media(p, tone)
	check(sound > 0, "import the tone")
	var red := p.add_bin_color("Red", Color(1, 0, 0), 25)
	var green := p.add_bin_color("Green", Color(0, 1, 0), 25)
	var blue := p.add_bin_color("Blue", Color(0, 0, 1), 25)
	p.insert_clip(red, 1, 0)
	p.insert_clip(green, 1, 25)
	p.insert_clip(blue, 1, 50)
	check(p.insert_clip(sound, 0, 0) > 0, "tone on the audio track")

	var peaks := TetraMltEngine.get_audio_peaks(tone, 50)
	check(peaks.size() >= 148 and peaks.size() <= 152, "50 peaks per second for 3 s: %d" % peaks.size())
	if peaks.size() > 100:
		check_near(peaks[70], 16000.0 / 32768.0, 0.03, "peak height is the tone's amplitude")

	var engine := TetraMltEngine.new()
	engine.frame_ready.connect(func(frame): frames_seen.append(frame))
	engine.playback_finished.connect(func(): finished += 1)
	engine.set_preview_size(Vector2i(320, 180))
	check_eq(engine.set_project(p), OK, "graph built")
	check_eq(engine.get_length(), 75, "three seconds")
	check(not engine.is_playing() and engine.get_position() == 0, "starts paused at 0")

	# A seek while paused renders that frame in the background.
	engine.seek(30)
	await wait(0.3)
	check_eq(engine.get_position(), 30, "position after seek")
	check_eq(engine.get_current_frame(), 30, "the sought frame was rendered")
	var still := engine.get_current_frame_image()
	if check(still != null, "frame image after seek"):
		check_eq([still.get_width(), still.get_height()], [320, 180], "preview size")
		check(still.get_pixel(160, 90).g > 0.95 and still.get_pixel(160, 90).r < 0.05, "frame 30 is green")
	check(frames_seen.has(30), "frame_ready was emitted for the seek")

	# Normal speed: real time, with sound.
	engine.seek(0)
	frames_seen.clear()
	var started := Time.get_ticks_msec()
	engine.play()
	check(engine.is_playing(), "playing")
	await wait(1.0)
	var position := engine.get_position()
	var elapsed := (Time.get_ticks_msec() - started) / 1000.0
	check(absf(position - elapsed * 25.0) <= 6.0, "after %.2f s the position is %d" % [elapsed, position])
	check(frames_seen.size() >= 10, "frames were published while playing: %d" % frames_seen.size())
	var ordered := true
	for i in range(1, frames_seen.size()):
		ordered = ordered and frames_seen[i] >= frames_seen[i - 1]
	check(ordered, "frames arrive in order")
	var pushed := engine.get_audio_frames_pushed()
	var mix_rate := AudioServer.get_mix_rate()
	check(pushed > mix_rate * 0.7 and pushed < mix_rate * 1.6, "about one second of sound was pulled: %d frames at %d Hz" % [pushed, mix_rate])
	engine.pause()
	check(not engine.is_playing(), "paused")
	await wait(0.15)
	var paused_at := engine.get_position()
	await wait(0.3)
	check_eq(engine.get_position(), paused_at, "position holds while paused")

	# Reverse at double speed, silently (J).
	engine.seek(60)
	engine.set_speed(-2.0)
	pushed = engine.get_audio_frames_pushed()
	engine.play()
	await wait(0.6)
	position = engine.get_position()
	check(position < 45 and position > 15, "reverse 2x from 60 for 0.6 s reaches about 30: %d" % position)
	check_eq(engine.get_audio_frames_pushed(), pushed, "no sound in reverse")
	await wait(1.0)
	check(not engine.is_playing() and engine.get_position() == 0, "reverse playback stops at the first frame")
	check_eq(finished, 1, "playback_finished at the start")

	# Forward to the end, then looping.
	engine.set_speed(1.0)
	engine.seek(60)
	engine.play()
	await wait(1.2)
	check(not engine.is_playing(), "stopped at the end")
	check_eq(engine.get_position(), 74, "position is the last frame")
	check_eq(finished, 2, "playback_finished at the end")
	var last := engine.get_current_frame_image()
	check(last != null and last.get_pixel(160, 90).b > 0.95, "the last frame is blue")

	engine.set_loop(true)
	engine.seek(65)
	engine.play()
	await wait(1.0)
	check(engine.is_playing(), "looping keeps playing")
	check(engine.get_position() < 30, "wrapped around to the start: %d" % engine.get_position())
	check_eq(finished, 2, "no finish while looping")

	# Edits while playing swap the graph under the running thread.
	p.delete_clip(p.get_clip_at(1, 60))
	check_eq(engine.set_project(p), OK, "graph replaced during playback")
	await wait(0.2)
	check(engine.is_playing(), "still playing after the swap")

	# Dropping the engine while it plays must not hang or crash.
	var before := Time.get_ticks_msec()
	engine = null
	check(Time.get_ticks_msec() - before < 1000, "the playback thread stops promptly")
	await wait(0.1)
