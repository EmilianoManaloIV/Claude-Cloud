extends "res://tetra_test.gd"
## How TetraBlenderBridge finds Blender, and what happens when it cannot.


func run() -> void:
	var python := OS.get_environment("TETRA_BLENDER_PYTHON")
	var search_path := OS.get_environment("PATH")

	# Nothing configured and no blender on PATH.
	OS.unset_environment("TETRA_BLENDER_PYTHON")
	OS.set_environment("PATH", tmp_dir().path_join("no-such-bin"))
	var launcher := TetraBlenderBridge.find_launcher()
	check_eq(launcher["kind"], "", "no launcher")
	check(str(launcher["description"]).contains("tetra/blender/python_path"), "the description says what to configure")
	check(not TetraBlenderBridge.is_available(), "not available")
	var bridge := TetraBlenderBridge.new()
	check_eq(bridge.get_launch_description(), launcher["description"], "launch description without a launcher")
	check_eq(bridge.start(), ERR_FILE_NOT_FOUND, "start() fails")
	var answer := bridge.request({"cmd": "hello"})
	check(answer.get("ok") == false and answer.get("error") == launcher["description"], "requests answer with the reason: %s" % answer)
	check(bridge.apply_modifier_stack(BoxMesh.new(), []) == null and bridge.get_last_error() == launcher["description"], "mesh calls return null")
	check(bridge.list_operators().is_empty(), "lists are empty")
	var results := []
	bridge.render_scene_async({}, {}, func(result: Dictionary) -> void: results.append(result))
	while results.is_empty():
		await process_frame
	check(results[0].get("ok") == false and results[0].get("error") == launcher["description"], "asynchronous calls report it too")
	check(not bridge.is_running(), "nothing is running")

	if python == "" or not FileAccess.file_exists(python):
		print("SKIPPED the launch checks of tetra_model/test_launcher.gd: TETRA_BLENDER_PYTHON does not name a Python")
		OS.set_environment("PATH", search_path)
		return

	ProjectSettings.set_setting("tetra/blender/python_path", python)
	launcher = TetraBlenderBridge.find_launcher()
	check(launcher["kind"] == "python" and launcher["path"] == python and str(launcher["description"]).contains("project setting"), "project setting: %s" % launcher)
	ProjectSettings.set_setting("tetra/blender/python_path", "")

	OS.set_environment("TETRA_BLENDER_PYTHON", python)
	launcher = TetraBlenderBridge.find_launcher()
	check(launcher["kind"] == "python" and str(launcher["description"]).contains("TETRA_BLENDER_PYTHON"), "environment variable: %s" % launcher)
	OS.set_environment("TETRA_BLENDER_PYTHON", tmp_dir().path_join("no-such-python"))
	check_eq(TetraBlenderBridge.find_launcher()["kind"], "", "a path that does not exist is not a launcher")
	OS.unset_environment("TETRA_BLENDER_PYTHON")

	# A Blender executable on PATH. The stand-in hands Blender's command line
	# to the bpy Python, so the arguments the bridge builds are exercised.
	var bin := tmp_dir().path_join("stand-in-bin")
	DirAccess.make_dir_recursive_absolute(bin)
	var stand_in := FileAccess.open(bin.path_join("blender"), FileAccess.WRITE)
	stand_in.store_string("#!/bin/sh\nfor argument in \"$@\"; do\n\tif [ \"$previous\" = \"--python\" ]; then script=\"$argument\"; fi\n\tprevious=\"$argument\"\ndone\nexec \"%s\" \"$script\" \"$@\"\n" % python)
	stand_in.close()
	FileAccess.set_unix_permissions(bin.path_join("blender"), FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER | FileAccess.UNIX_EXECUTE_OWNER)
	OS.set_environment("PATH", bin + ":" + search_path)
	launcher = TetraBlenderBridge.find_launcher()
	check(launcher["kind"] == "blender" and launcher["path"] == bin.path_join("blender") and str(launcher["description"]).contains("found on PATH"), "blender on PATH: %s" % launcher)
	answer = bridge.request({"cmd": "hello"})
	check(answer.get("ok") == true, "the bridge recovers once a Blender is there: %s" % answer.get("error", ""))
	check_eq(answer.get("temp_dir"), bridge.get_exchange_dir(), "arguments after -- reach the server")
	check(bridge.get_launch_description().contains("Blender executable"), "launch description: " + bridge.get_launch_description())
	var cube := bridge.create_primitive("blender:mesh.primitive_cube_add", {})
	check(cube != null and cube.get_aabb().is_equal_approx(AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))), "and works")
	bridge = null

	OS.set_environment("PATH", search_path)
	OS.set_environment("TETRA_BLENDER_PYTHON", python)
