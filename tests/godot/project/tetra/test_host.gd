extends "res://tetra_test.gd"
## The host is the Godot editor built from upstream/godot with the Tetra modules.


func run() -> void:
	var version := Engine.get_version_info()
	check_eq(version["major"], 4, "engine major version")
	check(version["minor"] >= 7, "engine minor version is at least 7")
	check(OS.has_feature("editor"), "this is an editor build")
