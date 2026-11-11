extends "res://tetra_test.gd"
## TetraEngineRegistry: the list of engines the modules announce.


func run() -> void:
	var before := TetraEngineRegistry.get_engines().size()
	TetraEngineRegistry.add_engine("TestEngine", "1.2.3", true, "found at /usr/lib/test")
	check_eq(TetraEngineRegistry.get_engines().size(), before + 1, "an engine was added")
	check_eq(TetraEngineRegistry.get_engine("TestEngine"),
			{"name": "TestEngine", "version": "1.2.3", "available": true, "details": "found at /usr/lib/test"}, "the entry")
	check_eq(TetraEngineRegistry.get_engines()[before], TetraEngineRegistry.get_engine("TestEngine"), "new engines go to the end of the list")
	check(TetraEngineRegistry.is_engine_available("testengine"), "names are compared without case")

	# Announcing an engine again replaces its entry in place.
	TetraEngineRegistry.add_engine("TESTENGINE", "", false, "library not found")
	check_eq(TetraEngineRegistry.get_engines().size(), before + 1, "no second entry")
	check_eq(TetraEngineRegistry.get_engine("TestEngine"),
			{"name": "TESTENGINE", "version": "", "available": false, "details": "library not found"}, "the replaced entry")
	check(not TetraEngineRegistry.is_engine_available("TestEngine"), "an unavailable engine")

	TetraEngineRegistry.add_engine("OtherEngine", "2.0", true)
	check_eq(TetraEngineRegistry.get_engine("OtherEngine")["details"], "", "details are optional")
	check_eq(TetraEngineRegistry.get_engine("missing"), {}, "an unknown engine")
	check(not TetraEngineRegistry.is_engine_available("missing"), "an unknown engine is not available")
	for engine: Dictionary in TetraEngineRegistry.get_engines():
		check_eq(engine.keys(), ["name", "version", "available", "details"], "keys of %s" % engine["name"])
