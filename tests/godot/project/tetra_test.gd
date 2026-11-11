extends SceneTree
## Base class of the headless tests run by scripts/test.sh.
##
## A test file extends this script and implements run(), which may await.
## The process exits with status 1 if any check failed.

var _checks := 0
var _failures := 0


func _initialize() -> void:
	_main()


func _main() -> void:
	await process_frame
	await run()
	print("%s: %d checks, %d failed" % [get_script().resource_path, _checks, _failures])
	quit(1 if _failures > 0 else 0)


func run() -> void:
	pass


func check(p_condition: bool, p_what: String = "") -> bool:
	_checks += 1
	if not p_condition:
		_failures += 1
		printerr("FAILED: " + p_what)
	return p_condition


func check_eq(p_actual: Variant, p_expected: Variant, p_what: String = "") -> bool:
	return check(p_actual == p_expected, "%s: expected %s, got %s" % [p_what, var_to_str(p_expected), var_to_str(p_actual)])


func check_near(p_actual: float, p_expected: float, p_epsilon: float = 1e-4, p_what: String = "") -> bool:
	return check(absf(p_actual - p_expected) <= p_epsilon, "%s: expected %f, got %f" % [p_what, p_expected, p_actual])


## A writable scratch directory for files a test creates.
func tmp_dir() -> String:
	var dir := OS.get_cache_dir().path_join("tetra-tests")
	DirAccess.make_dir_recursive_absolute(dir)
	return dir
