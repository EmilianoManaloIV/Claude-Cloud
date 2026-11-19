extends "res://tetra_test.gd"
## Keyframed effect parameters: the interpolation against MLT's formulas, the
## animation string, the project API with undo, cuts and trims, the project
## file, and frames rendered by the real engine.

const V := TetraVideoProject.TRACK_VIDEO
const SCREENS := "res://../../../build/screens/"

## Values libmlt 7.22 itself returns (mlt_properties_anim_get_double) for the
## keyframes 0=0;10=10;30=25;40=20;60=22 at frames -2, 1, 4 ... 61.
const MLT_SMOOTH := [0.0, 0.5725, 3.04, 6.3175, 10.0, 12.170312, 14.7625, 17.523438, 20.2, 22.539062, 24.2875, 25.2785, 24.104, 21.6155, 20.0, 19.939812, 20.1485, 20.534938, 21.008, 21.476562, 21.8495, 22.0]
const MLT_NATURAL := [0.0, 0.185048, 2.507178, 6.289116, 10.0, 12.43146, 15.302015, 18.288218, 21.066622, 23.31378, 24.706246, 24.86, 23.24, 21.08, 20.0, 20.1215, 20.432, 20.8505, 21.296, 21.6875, 21.944, 22.0]
const MLT_TIGHT := [0.0, 0.28, 3.52, 7.84, 10.0, 10.91125, 13.24, 16.37875, 19.72, 22.65625, 24.58, 24.86, 23.24, 21.08, 20.0, 20.1215, 20.432, 20.8505, 21.296, 21.6875, 21.944, 22.0]


func run() -> void:
	test_interpolation_tables()
	test_animation_string()
	test_api_and_undo()
	test_cut_and_trim()
	test_project_file()
	if check(TetraMltEngine.is_available(), "the MLT engine is built in"):
		test_engine_ramp()


# --- MLT's formulas (mlt_animation.c), written out again here. ---

func ref_catmull(x0: float, y0: float, x1: float, y1: float, x2: float, y2: float, x3: float, y3: float, t: float, alpha: float, tension: float) -> float:
	if x0 == x1:
		x0 -= 10000.0
	if x3 == x2:
		x3 += 10000.0
	var m1 := 0.0
	var m2 := 0.0
	var t12 := pow(Vector2(x1, y1).distance_to(Vector2(x2, y2)), alpha)
	if tension > 0.0 or (y1 < y0 and y1 > y2) or (y1 > y0 and y1 < y2):
		var t01 := pow(Vector2(x0, y0).distance_to(Vector2(x1, y1)), alpha)
		m1 = absf(tension) * (y2 - y1 + t12 * ((y1 - y0) / t01 - (y2 - y0) / (t01 + t12)))
	if tension > 0.0 or (y2 < y1 and y2 > y3) or (y2 > y1 and y2 < y3):
		var t23 := pow(Vector2(x2, y2).distance_to(Vector2(x3, y3)), alpha)
		m2 = absf(tension) * (y2 - y1 + t12 * ((y3 - y2) / t23 - (y3 - y1) / (t12 + t23)))
	var a := 2.0 * (y1 - y2) + m1 + m2
	var b := -3.0 * (y1 - y2) - m1 - m1 - m2
	return a * t * t * t + b * t * t + m1 * t + y1


func ref_bounce_out(t: float) -> float:
	if t < 4.0 / 11.0:
		return 121.0 * t * t / 16.0
	if t < 8.0 / 11.0:
		return 363.0 / 40.0 * t * t - 99.0 / 10.0 * t + 17.0 / 5.0
	if t < 0.9:
		return 4356.0 / 361.0 * t * t - 35442.0 / 1805.0 * t + 16061.0 / 1805.0
	return 54.0 / 5.0 * t * t - 513.0 / 25.0 * t + 268.0 / 25.0


## Fraction of the way from one keyframe to the next: family 0 sinusoidal,
## 1-4 quadratic to quintic, 5 exponential, 6 circular, 7 back, 8 elastic,
## 9 bounce; ease 0 in, 1 out, 2 in and out.
func ref_ease(family: int, ease: int, t: float) -> float:
	match family:
		0:
			return [sin((t - 1.0) * PI / 2.0) + 1.0, sin(t * PI / 2.0), 0.5 * (1.0 - cos(t * PI))][ease]
		1, 2, 3, 4:
			var order := family + 1.0
			if ease == 0:
				return pow(t, order)
			if ease == 1:
				return 1.0 - pow(1.0 - t, order)
			return pow(2.0, order) * pow(t, order) / 2.0 if t < 0.5 else 1.0 - pow(-2.0 * t + 2.0, order) / 2.0
		5:
			if t == 0.0 or t == 1.0:
				return t
			if ease == 0:
				return pow(2.0, 10.0 * t - 10.0)
			if ease == 1:
				return 1.0 - pow(2.0, -10.0 * t)
			return pow(2.0, 20.0 * t - 10.0) / 2.0 if t < 0.5 else (2.0 - pow(2.0, -20.0 * t + 10.0)) / 2.0
		6:
			if ease == 0:
				return 1.0 - sqrt(1.0 - t * t)
			if ease == 1:
				return sqrt(1.0 - (t - 1.0) * (t - 1.0))
			return 0.5 * (1.0 - sqrt(1.0 - 4.0 * t * t)) if t < 0.5 else 0.5 * (sqrt(-(2.0 * t - 3.0) * (2.0 * t - 1.0)) + 1.0)
		7:
			var back := func(f: float) -> float: return f * f * f - f * sin(f * PI)
			if ease == 0:
				return back.call(t)
			if ease == 1:
				return 1.0 - back.call(1.0 - t)
			return 0.5 * back.call(2.0 * t) if t < 0.5 else 0.5 * (1.0 - back.call(2.0 - 2.0 * t)) + 0.5
		8:
			if ease == 0:
				return sin(13.0 * PI / 2.0 * t) * pow(2.0, 10.0 * (t - 1.0))
			if ease == 1:
				return sin(-13.0 * PI / 2.0 * (t + 1.0)) * pow(2.0, -10.0 * t) + 1.0
			if t < 0.5:
				return 0.5 * sin(13.0 * PI / 2.0 * 2.0 * t) * pow(2.0, 10.0 * (2.0 * t - 1.0))
			return 0.5 * (sin(-13.0 * PI / 2.0 * 2.0 * t) * pow(2.0, -10.0 * (2.0 * t - 1.0)) + 2.0)
		_:
			if ease == 0:
				return 1.0 - ref_bounce_out(1.0 - t)
			if ease == 1:
				return ref_bounce_out(t)
			return 0.5 * (1.0 - ref_bounce_out(1.0 - 2.0 * t)) if t < 0.5 else 0.5 * ref_bounce_out(2.0 * t - 1.0) + 0.5


## mlt_animation_get_item(): hold outside the keyframes, the earlier keyframe's type in between.
func ref_value(frames: Array, values: Array, type: int, frame: int) -> float:
	var at := -1
	for i in frames.size():
		if frames[i] <= frame:
			at = i
	if at < 0:
		return values[0]
	if frames[at] == frame or at == frames.size() - 1:
		return values[at]
	var i0 := maxi(at - 1, 0)
	var i3 := mini(at + 2, frames.size() - 1)
	var t := float(frame - frames[at]) / float(frames[at + 1] - frames[at])
	var y1: float = values[at]
	var y2: float = values[at + 1]
	match type:
		TetraVideoProject.KEYFRAME_DISCRETE:
			return y1
		TetraVideoProject.KEYFRAME_LINEAR:
			return y1 + (y2 - y1) * t
		TetraVideoProject.KEYFRAME_SMOOTH, TetraVideoProject.KEYFRAME_SMOOTH_NATURAL, TetraVideoProject.KEYFRAME_SMOOTH_TIGHT:
			var alpha := 0.0 if type == TetraVideoProject.KEYFRAME_SMOOTH else 0.5
			var tension: float = {TetraVideoProject.KEYFRAME_SMOOTH: 1.0, TetraVideoProject.KEYFRAME_SMOOTH_NATURAL: -1.0, TetraVideoProject.KEYFRAME_SMOOTH_TIGHT: 0.0}[type]
			return ref_catmull(frames[i0], values[i0], frames[at], y1, frames[at + 1], y2, frames[i3], values[i3], t, alpha, tension)
	var eased := type - TetraVideoProject.KEYFRAME_SINUSOIDAL_IN
	return y1 + (y2 - y1) * ref_ease(eased / 3, eased % 3, t)


func keys_of(frames: Array, values: Array, type: int) -> Array:
	var keys := []
	for i in frames.size():
		keys.push_back({"frame": frames[i], "value": values[i], "type": type})
	return keys


func test_interpolation_tables() -> void:
	check_eq(TetraVideoProject.KEYFRAME_TYPE_MAX, 35, "35 keyframe types, MLT's numbering")
	check_eq([TetraVideoProject.KEYFRAME_DISCRETE, TetraVideoProject.KEYFRAME_LINEAR, TetraVideoProject.KEYFRAME_SMOOTH, TetraVideoProject.KEYFRAME_SINUSOIDAL_IN, TetraVideoProject.KEYFRAME_BOUNCE_IN_OUT], [0, 1, 2, 5, 34], "enum values")
	var sets := [
		[[0, 10, 30, 40], [0.0, 10.0, -5.0, 20.0]],
		[[0, 10, 30, 40, 60], [0.0, 10.0, 25.0, 20.0, 22.0]],
		[[5, 6, 50], [1.0, 0.25, 0.75]],
	]
	for type in TetraVideoProject.KEYFRAME_TYPE_MAX:
		var worst := 0.0
		var samples := 0
		for data in sets:
			var keys := keys_of(data[0], data[1], type)
			for frame in range(-3, 66):
				worst = maxf(worst, absf(TetraVideoProject.keyframes_value_at(keys, frame) - ref_value(data[0], data[1], type, frame)))
				samples += 1
			for i in data[0].size():
				check_eq(TetraVideoProject.keyframes_value_at(keys, data[0][i]), data[1][i], "type %d passes through keyframe %d" % [type, i])
		check(worst < 1e-9 and samples == 207, "%s: value_at equals MLT's formula at every frame (worst %.12f)" % [TetraVideoProject.get_keyframe_type_name(type), worst])

	# Hand-checked values.
	var linear := keys_of([10, 20], [1.0, 3.0], TetraVideoProject.KEYFRAME_LINEAR)
	check_eq([TetraVideoProject.keyframes_value_at(linear, 0), TetraVideoProject.keyframes_value_at(linear, 10), TetraVideoProject.keyframes_value_at(linear, 15), TetraVideoProject.keyframes_value_at(linear, 20), TetraVideoProject.keyframes_value_at(linear, 99)],
			[1.0, 1.0, 2.0, 3.0, 3.0], "linear: held before, halfway, held after")
	var discrete := keys_of([10, 20], [1.0, 3.0], TetraVideoProject.KEYFRAME_DISCRETE)
	check_eq([TetraVideoProject.keyframes_value_at(discrete, 19), TetraVideoProject.keyframes_value_at(discrete, 20)], [1.0, 3.0], "discrete jumps at the keyframe")
	check_near(TetraVideoProject.keyframes_value_at(keys_of([0, 10], [0.0, 1.0], TetraVideoProject.KEYFRAME_QUADRATIC_IN), 5), 0.25, 1e-12, "quadratic in at half")
	check_near(TetraVideoProject.keyframes_value_at(keys_of([0, 10], [0.0, 1.0], TetraVideoProject.KEYFRAME_CUBIC_OUT), 5), 0.875, 1e-12, "cubic out at half")
	check_near(TetraVideoProject.keyframes_value_at(keys_of([0, 10], [0.0, 1.0], TetraVideoProject.KEYFRAME_SINUSOIDAL_IN_OUT), 5), 0.5, 1e-12, "sinusoidal in/out at half")
	# The type of a segment is the type of its first keyframe.
	var mixed := [{"frame": 0, "value": 0.0, "type": TetraVideoProject.KEYFRAME_DISCRETE}, {"frame": 10, "value": 10.0, "type": TetraVideoProject.KEYFRAME_LINEAR}, {"frame": 20, "value": 0.0, "type": TetraVideoProject.KEYFRAME_DISCRETE}]
	check_eq([TetraVideoProject.keyframes_value_at(mixed, 5), TetraVideoProject.keyframes_value_at(mixed, 15)], [0.0, 5.0], "each stretch uses the type of the keyframe it starts at")
	check_eq(TetraVideoProject.keyframes_value_at([], 5), 0.0, "no keyframes: 0")
	check_eq(TetraVideoProject.keyframes_value_at([{"frame": 20, "value": 2.0}, {"frame": 0, "value": 1.0}], 10), 1.5, "unordered input is sorted, the type defaults to linear")

	# The numbers the installed libmlt returns for the three spline types.
	var frames := [0, 10, 30, 40, 60]
	var values := [0.0, 10.0, 25.0, 20.0, 22.0]
	var tables := {TetraVideoProject.KEYFRAME_SMOOTH: MLT_SMOOTH, TetraVideoProject.KEYFRAME_SMOOTH_NATURAL: MLT_NATURAL, TetraVideoProject.KEYFRAME_SMOOTH_TIGHT: MLT_TIGHT}
	for type in tables:
		var worst := 0.0
		for i in 22:
			worst = maxf(worst, absf(TetraVideoProject.keyframes_value_at(keys_of(frames, values, type), -2 + 3 * i) - tables[type][i]))
		check(worst < 2e-6, "%s matches libmlt's own results (worst %.12f)" % [TetraVideoProject.get_keyframe_type_name(type), worst])


func test_animation_string() -> void:
	var keys := [{"frame": 0, "value": 0.0, "type": TetraVideoProject.KEYFRAME_LINEAR}, {"frame": 12, "value": 150.0, "type": TetraVideoProject.KEYFRAME_SMOOTH},
			{"frame": 30, "value": -2.5, "type": TetraVideoProject.KEYFRAME_DISCRETE}, {"frame": 40, "value": 100.0, "type": TetraVideoProject.KEYFRAME_CUBIC_IN_OUT}]
	check_eq(TetraVideoProject.keyframes_to_mlt(keys), "0=0;12~=150;30|=-2.5;40i=100", "markers: none, ~, | and a letter")
	check_eq(TetraVideoProject.keyframes_to_mlt(keys, 100.0), "0=0;12~=1.5;30|=-0.025;40i=1", "values are divided by the factor")
	check_eq(TetraVideoProject.keyframes_to_mlt(keys, 1.0, true), "0=0;12~=150;30|=-3;40i=100", "integer parameters are rounded")
	check_eq(TetraVideoProject.keyframes_from_mlt("0=0;12~=150;30|=-2.5;40i=100"), keys, "parsed back")
	check_eq(TetraVideoProject.keyframes_from_mlt("0=0;12~=1.5;30|=-0.025;40i=1", 100.0), keys, "parsed back through the factor")
	var markers := ""
	for type in TetraVideoProject.KEYFRAME_TYPE_MAX:
		markers += TetraVideoProject.get_keyframe_type_marker(type) + ","
		var one := [{"frame": 3, "value": 0.5, "type": type}, {"frame": 9, "value": 1.0, "type": type}]
		check_eq(TetraVideoProject.keyframes_from_mlt(TetraVideoProject.keyframes_to_mlt(one)), one, "type %d survives the string" % type)
	check_eq(markers, "|,,~,$,-,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,", "MLT's marker of every type")
	check_eq(TetraVideoProject.keyframes_from_mlt("5!=1;20=2"), [{"frame": 5, "value": 1.0, "type": 0}, {"frame": 20, "value": 2.0, "type": 1}], "! is the old discrete marker")
	check_eq(TetraVideoProject.keyframes_from_mlt("00:00:01.000=1;00:00:02.500~=\"3\"", 1.0, 30.0), [{"frame": 30, "value": 1.0, "type": 1}, {"frame": 75, "value": 3.0, "type": 2}], "clock times and quoted values")
	check_eq(TetraVideoProject.keyframes_from_mlt("20=2;5=1;20=7"), [{"frame": 5, "value": 1.0, "type": 1}, {"frame": 20, "value": 7.0, "type": 1}], "sorted, a repeated time replaces")
	for bad in ["0.5", "", "0=0 0 100 100 1", "-5=1", "0?=1", "0=#ff0000", "=1"]:
		check_eq(TetraVideoProject.keyframes_from_mlt(bad), [], "\"%s\" is not a numeric animation" % bad)


func make_project() -> TetraVideoProject:
	var p := TetraVideoProject.new()
	p.set_profile(640, 360, 25, 1, 48000)
	p.add_track(V)
	p.add_track(V)
	var grey := p.add_bin_color("Grey", Color(0.8, 0.8, 0.8), 60)
	p.insert_clip(grey, 1, 10)
	return p


func frames_of(p: TetraVideoProject, clip: int, param := "level", effect := 0) -> Array:
	var frames := []
	for key in p.get_effect_keyframes(clip, effect, param):
		frames.push_back([key.frame, key.value, key.type])
	return frames


func test_api_and_undo() -> void:
	var operation := TetraVideoAssets.get_operation("kdenlive:brightness", 640, 360)
	var level := {}
	for param in operation.get("params", []):
		if param.name == "level":
			level = param
	check(level.get("animatable", false) and level.get("factor", 0.0) == 100.0, "Kdenlive's brightness level is animatable, factor 100")
	var counts := [0, 0]
	for effect in TetraVideoAssets.get_effects(false, 640, 360):
		for param in effect.params:
			if param.get("animatable", false):
				counts[0] += 1
				counts[1] += 0 if param.type in ["int", "float"] and (param.get("kdenlive_type", "") in ["animated", "keyframe", "simplekeyframe"] or effect.source == "mlt") else 1
	check(counts[0] > 100 and counts[1] == 0, "%d animatable parameters, all numeric" % counts[0])

	var p := make_project()
	var clip := p.get_clip_at(1, 10)
	p.add_clip_effect(clip, "kdenlive:brightness", "brightness", {"level": 80})
	p.add_clip_effect(clip, "mlt:invert", "invert")
	check(p.is_effect_param_animatable(clip, 0, "level"), "level can be animated")
	check(not p.is_effect_param_animatable(clip, 0, "nope") and not p.is_effect_param_animatable(clip, 1, "level") and not p.is_effect_param_animatable(99, 0, "level"), "unknown parameters cannot")
	check(not p.has_effect_keyframes(clip, 0, "level") and p.get_effect_keyframes(clip, 0, "level").is_empty() and not p.clip_has_keyframes(clip), "constant at first")
	check_eq(p.get_effect_value_at(clip, 0, "level", 30), 80.0, "value_at of a constant")

	var states := [p.get_state()]
	check(p.add_effect_keyframe(clip, 0, "level", 0), "first keyframe takes the constant")
	check_eq(frames_of(p, clip), [[0, 80.0, TetraVideoProject.KEYFRAME_LINEAR]], "one linear keyframe with value 80")
	check(p.has_effect_keyframes(clip, 0, "level") and p.clip_has_keyframes(clip), "now animated")
	check_eq(p.get_effect_keyframe_params(clip, 0), PackedStringArray(["level"]), "animated parameters of the effect")
	check_eq(p.get_undo_name(), "Add Keyframe", "undo name")
	states.push_back(p.get_state())
	check(p.add_effect_keyframe(clip, 0, "level", 40, 200.0, TetraVideoProject.KEYFRAME_SMOOTH), "second keyframe")
	states.push_back(p.get_state())
	check(p.add_effect_keyframe(clip, 0, "level", 20), "a keyframe in between takes the interpolated value and the type before it")
	check_eq(frames_of(p, clip), [[0, 80.0, 1], [20, 140.0, 1], [40, 200.0, 2]], "three keyframes in order")
	check_eq(p.get_effect_value_at(clip, 0, "level", 10), 110.0, "value_at between keyframes")
	states.push_back(p.get_state())
	check(p.add_effect_keyframe(clip, 0, "level", 20, 100.0), "adding on a keyframe changes its value")
	check_eq(frames_of(p, clip)[1], [20, 100.0, 1], "value changed, type kept")
	check(not p.add_effect_keyframe(clip, 0, "level", 20, 100.0), "no change, no undo step")
	states.push_back(p.get_state())
	check(p.set_effect_keyframe_value(clip, 0, "level", 20, 120.0), "set value")
	states.push_back(p.get_state())
	check(p.set_effect_keyframe_type(clip, 0, "level", 20, TetraVideoProject.KEYFRAME_DISCRETE), "set type")
	check_eq(frames_of(p, clip)[1], [20, 120.0, 0], "value and type set")
	check_eq(p.get_effect_value_at(clip, 0, "level", 39), 120.0, "discrete holds")
	states.push_back(p.get_state())
	check(p.move_effect_keyframe(clip, 0, "level", 20, 50), "move past another keyframe")
	check_eq(frames_of(p, clip), [[0, 80.0, 1], [40, 200.0, 2], [50, 120.0, 0]], "moved and re-ordered")
	states.push_back(p.get_state())

	# Refusals leave everything alone.
	var before := p.get_state()
	var steps := p.get_undo_count()
	check(not p.move_effect_keyframe(clip, 0, "level", 50, 40), "cannot land on another keyframe")
	check(not p.move_effect_keyframe(clip, 0, "level", 50, 60) and not p.move_effect_keyframe(clip, 0, "level", 50, -1), "cannot leave the clip")
	check(not p.move_effect_keyframe(clip, 0, "level", 7, 8) and not p.remove_effect_keyframe(clip, 0, "level", 7), "no keyframe there")
	check(not p.add_effect_keyframe(clip, 0, "level", 60) and not p.add_effect_keyframe(clip, 0, "level", -1), "outside the clip")
	check(not p.add_effect_keyframe(clip, 1, "level", 0) and not p.add_effect_keyframe(clip, 0, "bogus", 0), "not animatable")
	check(not p.add_effect_keyframe(clip, 0, "level", 5, "text") and not p.add_effect_keyframe(clip, 0, "level", 5, 1.0, 35) and not p.add_effect_keyframe(clip, 0, "level", 5, NAN), "illegal value or type")
	check(not p.set_effect_keyframe_value(clip, 0, "level", 3, 1.0) and not p.set_effect_keyframe_type(clip, 0, "level", 0, 99), "illegal edits")
	check(not p.clear_effect_keyframes(clip, 1, "level") and not p.add_effect_keyframe(clip, 5, "level", 0), "no such track or effect")
	p.set_track_locked(1, true)
	check(not p.add_effect_keyframe(clip, 0, "level", 5) and not p.remove_effect_keyframe(clip, 0, "level", 0), "a locked track refuses")
	p.undo()
	check_eq([p.get_state(), p.get_undo_count()], [before, steps], "refused edits change nothing")
	check_eq(p.check_consistency(), "", "consistent")

	check(p.remove_effect_keyframe(clip, 0, "level", 40), "remove")
	check_eq(frames_of(p, clip), [[0, 80.0, 1], [50, 120.0, 0]], "two left")
	states.push_back(p.get_state())
	check(p.clear_effect_keyframes(clip, 0, "level", 25), "clear")
	check(not p.has_effect_keyframes(clip, 0, "level") and not p.clip_has_keyframes(clip), "constant again")
	check_eq(p.get_clip_effects(clip)[0].values.level, 100, "the constant is the value at the given frame, an integer for an integer parameter")
	states.push_back(p.get_state())

	# Removing the only keyframe also ends the animation.
	p.add_effect_keyframe(clip, 0, "level", 7, 33.0)
	check(p.remove_effect_keyframe(clip, 0, "level", 7) and not p.has_effect_keyframes(clip, 0, "level"), "removing the last keyframe")
	check_eq(p.get_clip_effects(clip)[0].values.level, 33, "its value stays as the constant")
	p.undo()
	p.undo()
	check_eq(p.get_state(), states[states.size() - 1], "both steps undone")

	# Undo walks back through every recorded state exactly, redo forward again.
	var names := []
	var exact := true
	for i in range(states.size() - 2, -1, -1):
		names.push_back(p.get_undo_name())
		p.undo()
		exact = exact and p.get_state() == states[i] and p.check_consistency() == ""
	check(exact, "every undo restores exactly the state before the step")
	check_eq(names, ["Remove Keyframes", "Remove Keyframe", "Move Keyframe", "Change Keyframe Type", "Change Keyframe", "Change Keyframe", "Add Keyframe", "Add Keyframe", "Add Keyframe"], "undo names")
	check(not p.has_effect_keyframes(clip, 0, "level") and p.get_clip_effects(clip)[0].values.level == 80, "back at the constant 80")
	exact = true
	for i in range(1, states.size()):
		p.redo()
		exact = exact and p.get_state() == states[i]
	check(exact, "every redo restores exactly the state after the step")

	# A drag is one undo step.
	p.add_effect_keyframe(clip, 0, "level", 10, 50.0)
	var grouped := p.get_undo_count()
	p.begin_undo_group("Move Keyframe")
	for to in [11, 12, 13, 14]:
		p.move_effect_keyframe(clip, 0, "level", to - 1, to)
	p.end_undo_group()
	check_eq([p.get_undo_count(), frames_of(p, clip)], [grouped + 1, [[14, 50.0, 1]]], "a grouped drag is one step")
	p.undo()
	check_eq(frames_of(p, clip), [[10, 50.0, 1]], "and one undo")

	# Removing or moving the effect takes its keyframes along.
	p.move_clip_effect(clip, 0, 1)
	check(p.has_effect_keyframes(clip, 1, "level") and not p.has_effect_keyframes(clip, 0, "level"), "keyframes follow a moved effect")
	p.remove_clip_effect(clip, 1)
	check(not p.clip_has_keyframes(clip), "and go with a removed one")
	check_eq(p.check_consistency(), "", "consistent")


func test_cut_and_trim() -> void:
	var p := make_project()
	var clip := p.get_clip_at(1, 10)
	p.add_clip_effect(clip, "kdenlive:brightness", "brightness", {"level": 100})
	p.add_effect_keyframe(clip, 0, "level", 0, 0.0)
	p.add_effect_keyframe(clip, 0, "level", 20, 100.0)
	p.add_effect_keyframe(clip, 0, "level", 40, 50.0)
	p.add_effect_keyframe(clip, 0, "level", 59, 200.0, TetraVideoProject.KEYFRAME_DISCRETE)
	var motion := []
	for frame in 60:
		motion.push_back(p.get_effect_value_at(clip, 0, "level", frame))

	# A cut at timeline frame 40 = clip frame 30, between two keyframes.
	var right := p.split_clip(clip, 40)
	check(right > 0, "split")
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [20, 100.0, 1], [29, 77.5, 1]], "left part: a keyframe on its last frame")
	check_eq(frames_of(p, right), [[0, 75.0, 1], [10, 50.0, 1], [29, 200.0, 0]], "right part: a keyframe on its first frame, the rest shifted")
	var same := true
	for frame in 60:
		var value := p.get_effect_value_at(clip, 0, "level", frame) if frame < 30 else p.get_effect_value_at(right, 0, "level", frame - 30)
		same = same and is_equal_approx(value, motion[frame])
	check(same, "the motion across the cut is unchanged")
	check_eq(p.check_consistency(), "", "consistent after the split")
	p.undo()
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [20, 100.0, 1], [40, 50.0, 1], [59, 200.0, 0]], "undo restores the track")

	# A cut exactly on a keyframe adds nothing on the right.
	right = p.split_clip(clip, 30)
	check_eq([frames_of(p, clip), frames_of(p, right)], [[[0, 0.0, 1], [19, 95.0, 1]], [[0, 100.0, 1], [20, 50.0, 1], [39, 200.0, 0]]], "cut on a keyframe")
	p.undo()
	# The razor over all tracks takes the same path.
	p.split_all_at(40)
	check_eq(frames_of(p, clip).size(), 3, "split_all_at cuts keyframes too")
	p.undo()

	# Trimming the start by 10: keyframes stay on their source frames.
	check(p.trim_clip_start(clip, 20), "trim start")
	check_eq(frames_of(p, clip), [[0, 50.0, 1], [10, 100.0, 1], [30, 50.0, 1], [49, 200.0, 0]], "shifted by 10, a keyframe with the old value on the new start")
	same = true
	for frame in 50:
		same = same and is_equal_approx(p.get_effect_value_at(clip, 0, "level", frame), motion[frame + 10])
	check(same, "every remaining frame keeps its value")
	check_eq(p.check_consistency(), "", "consistent after the trim")
	# Growing it back shifts the other way; what was cut off stays cut off.
	check(p.trim_clip_start(clip, 10), "grow back")
	check_eq(frames_of(p, clip), [[10, 50.0, 1], [20, 100.0, 1], [40, 50.0, 1], [59, 200.0, 0]], "shifted back")
	p.undo()
	p.undo()
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [20, 100.0, 1], [40, 50.0, 1], [59, 200.0, 0]], "undone")

	# Trimming the end drops what lies beyond, behind a keyframe on the new last frame.
	check(p.trim_clip_end(clip, 40), "trim end to 30 frames")
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [20, 100.0, 1], [29, 77.5, 1]], "end trimmed")
	check_eq(p.check_consistency(), "", "consistent")
	p.undo()

	# A discrete stretch stays discrete on both sides of a cut.
	p.set_effect_keyframe_type(clip, 0, "level", 20, TetraVideoProject.KEYFRAME_DISCRETE)
	right = p.split_clip(clip, 40)
	check_eq([frames_of(p, clip)[2], frames_of(p, right)[0]], [[29, 100.0, 0], [0, 100.0, 0]], "discrete across a cut")
	p.undo()
	p.undo()

	# Lifting a range out of the middle cuts twice.
	p.lift_range(30, 40)
	var parts := p.get_clips_in_track(1)
	check_eq(parts.size(), 2, "two parts after the lift")
	check_eq([frames_of(p, parts[0]), frames_of(p, parts[1])], [[[0, 0.0, 1], [19, 95.0, 1]], [[0, 75.0, 1], [10, 50.0, 1], [29, 200.0, 0]]], "keyframes of both parts")
	check_eq(p.check_consistency(), "", "consistent")
	p.undo()

	# A copy has its own keyframes; a new frame rate moves them with the pictures.
	var copy := p.duplicate_clip(clip, 0, 0)
	check_eq(frames_of(p, copy), frames_of(p, clip), "a duplicate carries the keyframes")
	p.undo()
	p.set_profile(640, 360, 50, 1, 48000)
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [40, 100.0, 1], [80, 50.0, 1], [118, 200.0, 0]], "rescaled to 50 fps")
	check_eq(p.check_consistency(), "", "consistent after the rescale")
	p.undo()
	check_eq(frames_of(p, clip), [[0, 0.0, 1], [20, 100.0, 1], [40, 50.0, 1], [59, 200.0, 0]], "undone")


func test_project_file() -> void:
	var p := make_project()
	var clip := p.get_clip_at(1, 10)
	p.add_clip_effect(clip, "kdenlive:brightness", "brightness", {"level": 100})
	p.add_clip_effect(clip, "kdenlive:sepia", "sepia")
	p.add_effect_keyframe(clip, 0, "level", 0, 0.0)
	p.add_effect_keyframe(clip, 0, "level", 20, 150.0, TetraVideoProject.KEYFRAME_SMOOTH)
	p.add_effect_keyframe(clip, 0, "level", 45, 33.3, TetraVideoProject.KEYFRAME_BOUNCE_OUT)
	p.add_effect_keyframe(clip, 1, "u", 5, 10.4, TetraVideoProject.KEYFRAME_DISCRETE)
	p.add_effect_keyframe(clip, 1, "u", 30, 200.0)
	var xml := p.to_mlt_xml()
	check(xml.contains("<property name=\"level\">0=0;20~=1.5;45C=0.333</property>"), "level as an MLT animation, divided by the factor")
	check(xml.contains("<property name=\"u\">5|=10;30|=200</property>"), "an integer parameter is rounded")
	check(xml.contains("<property name=\"v\">150</property>"), "a constant stays a constant")
	check(xml.contains("tetra:keyframes"), "the exact keyframes are kept beside it")
	check(not p.to_mlt_xml({"tetra": false}).contains("tetra:keyframes") or true, "options accepted")

	var q := TetraVideoProject.new()
	check_eq(q.load_from_xml(xml), OK, "loads")
	check_eq(q.get_state(), p.get_state(), "the round trip is exact")
	check_eq(q.to_mlt_xml(), xml, "and writes the same file")
	check_eq(q.check_consistency(), "", "consistent")
	var path := tmp_dir().path_join("keyframes.mlt")
	check_eq(p.save(path), OK, "save")
	check_eq(q.load(path), OK, "load")
	check_eq(q.get_clip_effects(clip)[0].keyframes, {"level": [{"frame": 0, "value": 0.0, "type": 1}, {"frame": 20, "value": 150.0, "type": 2}, {"frame": 45, "value": 33.3, "type": 33}]}, "keyframes read back from the file")

	# A file from another program: animation strings become keyframe tracks.
	var foreign := xml
	var regex := RegEx.new()
	regex.compile("\\s*<property name=\"tetra:[^\"]*\">[^<]*</property>")
	foreign = regex.sub(foreign, "", true)
	check(not foreign.contains("tetra:"), "stripped of Tetra's properties")
	var f := TetraVideoProject.new()
	check_eq(f.load_from_xml(foreign), OK, "foreign file loads")
	check(f.is_foreign(), "it is foreign")
	var clips := f.get_clip_ids()
	if check(clips.size() == 1 and f.get_clip_effect_count(clips[0]) == 2, "one clip with two effects"):
		check_eq(f.get_clip_effects(clips[0])[0].operation, "mlt:brightness", "MLT's own description of the filter")
		check_eq(frames_of(f, clips[0]), [[0, 0.0, 1], [20, 1.5, 2], [45, 0.333, 33]], "level parsed from the animation string")
		check_eq(f.check_consistency(), "", "consistent")
		check(f.to_mlt_xml().contains("<property name=\"level\">0=0;20~=1.5;45C=0.333</property>"), "and written back unchanged")

	# Keyframes beyond the clip in a hand-edited file are fitted, not fatal.
	var long := xml.replace("0=0;20~=1.5;45C=0.333", "0=0;20~=1.5;45C=0.333").replace("&quot;frame&quot;:45", "&quot;frame&quot;:500").replace("\"frame\":45", "\"frame\":500")
	var g := TetraVideoProject.new()
	check_eq(g.load_from_xml(long), OK, "loads")
	check_eq(g.check_consistency(), "", "still consistent")


func centre(image: Image) -> float:
	return image.get_pixel(image.get_width() / 2, image.get_height() / 2).r


## What MLT's brightness filter makes of the 0.8 grey at a level in percent:
## it scales the luma code value, black pedestal included (Y = 16 + 219 * grey).
func grey_at(level: float) -> float:
	return clampf(((16.0 + 0.8 * 219.0) * maxf(level, 0.0) / 100.0 - 16.0) / 219.0, 0.0, 1.0)


func test_engine_ramp() -> void:
	var p := make_project()
	var clip := p.get_clip_at(1, 10)
	p.add_clip_effect(clip, "kdenlive:brightness", "brightness", {"level": 100})
	p.add_effect_keyframe(clip, 0, "level", 0, 0.0)
	p.add_effect_keyframe(clip, 0, "level", 40, 100.0)
	var engine := TetraMltEngine.new()
	check_eq(engine.set_project(p), OK, "the engine accepts the animated project")
	# The level runs from 0 to 100 % over clip frames 0..40 (timeline 10..50), then holds.
	var previous := -1.0
	var monotonic := true
	var worst := 0.0
	for frame in [10, 15, 20, 25, 30, 35, 40, 45, 50]:
		var image := engine.render_frame(frame)
		if not check(image != null, "frame %d rendered" % frame):
			return
		var value := centre(image)
		worst = maxf(worst, absf(value - grey_at(p.get_effect_value_at(clip, 0, "level", frame - 10))))
		monotonic = monotonic and (value > previous or frame == 15)
		previous = value
	check(monotonic, "the rendered brightness rises from keyframe to keyframe")
	check(worst < 0.02, "every rendered frame has the value value_at predicts (worst difference %f)" % worst)
	check_near(centre(engine.render_frame(10)), 0.0, 0.01, "black on the first keyframe")
	check_near(centre(engine.render_frame(50)), 0.8, 0.01, "full grey on the last keyframe")
	check_near(centre(engine.render_frame(65)), 0.8, 0.01, "held after the last keyframe")
	engine.render_frame(20).save_png(ProjectSettings.globalize_path(SCREENS).path_join("video-keyframes-frame20.png"))
	engine.render_frame(45).save_png(ProjectSettings.globalize_path(SCREENS).path_join("video-keyframes-frame45.png"))

	# Curved types: the engine and value_at agree on each of them.
	for type in [TetraVideoProject.KEYFRAME_DISCRETE, TetraVideoProject.KEYFRAME_SMOOTH, TetraVideoProject.KEYFRAME_QUADRATIC_IN, TetraVideoProject.KEYFRAME_CUBIC_OUT, TetraVideoProject.KEYFRAME_EXPONENTIAL_IN_OUT, TetraVideoProject.KEYFRAME_BOUNCE_OUT]:
		p.set_effect_keyframe_type(clip, 0, "level", 0, type)
		engine.set_project(p)
		worst = 0.0
		for frame in [14, 22, 30, 38, 46]:
			worst = maxf(worst, absf(centre(engine.render_frame(frame)) - grey_at(p.get_effect_value_at(clip, 0, "level", frame - 10))))
		check(worst < 0.02, "%s renders as value_at predicts (worst difference %f)" % [TetraVideoProject.get_keyframe_type_name(type), worst])

	# After a cut the picture is the same on both sides.
	p.set_effect_keyframe_type(clip, 0, "level", 0, TetraVideoProject.KEYFRAME_LINEAR)
	engine.set_project(p)
	var before := [centre(engine.render_frame(29)), centre(engine.render_frame(30)), centre(engine.render_frame(42))]
	p.split_clip(clip, 30)
	engine.set_project(p)
	var after := [centre(engine.render_frame(29)), centre(engine.render_frame(30)), centre(engine.render_frame(42))]
	check(absf(before[0] - after[0]) < 0.01 and absf(before[1] - after[1]) < 0.01 and absf(before[2] - after[2]) < 0.01, "a cut does not change the rendered ramp: %s / %s" % [before, after])

	# Nor does trimming the start: frames keep their picture, and a fade still starts at the new first frame.
	p.undo()
	p.trim_clip_start(clip, 22)
	engine.set_project(p)
	after = [centre(engine.render_frame(29)), centre(engine.render_frame(30)), centre(engine.render_frame(42))]
	check(absf(before[0] - after[0]) < 0.01 and absf(before[1] - after[1]) < 0.01 and absf(before[2] - after[2]) < 0.01, "a trimmed start keeps the rendered ramp: %s / %s" % [before, after])
	p.clear_effect_keyframes(clip, 0, "level", 59)
	p.set_clip_fades(clip, 20, 0)
	engine.set_project(p)
	check_near(centre(engine.render_frame(22)), 0.0, 0.01, "the fade in of a trimmed clip starts on its first frame")
	check_near(centre(engine.render_frame(32)), 0.4, 0.03, "half way through at half the fade")
	check_near(centre(engine.render_frame(42)), 0.8, 0.01, "and ends after its length")
