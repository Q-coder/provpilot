extends Node
## Smoke: provpilot.cfg survives a second writer, and Settings > Cancel keeps
## the pad bindings.
##  - InstallRoot.save_config() merges this instance's changes into the file as
##    it is on disk (another instance's joystick bindings used to be put back).
##  - JoystickConfig.reload() restores the file's bindings (Cancel called an
##    unbound method after the reset: the pad was on its defaults from then on).
## Run from a scratch directory that holds a copy of provpilot.cfg (the
## install root is $PWD):
##   cd <scratch> && godot --headless --path <repo>/provpilot res://tests/smoke_config_merge.tscn
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _ready() -> void:
	var path: String = InstallRoot.get_config_path()
	print("  config: ", path)
	if path.begins_with("/Users/") and not path.contains("scratch") and not path.contains("tmp"):
		print("  refusing to run against a real provpilot.cfg")
		get_tree().quit(2)
		return
	# another instance writes the file behind this one's back
	var other := ConfigFile.new()
	other.load(path)
	other.set_value("button.trim_down", "button", 9)
	other.set_value("merge_test", "theirs", 2)
	other.save(path)
	# this instance changes two keys of its own and saves
	InstallRoot.set_value("merge_test", "ours", 3)
	InstallRoot.set_value("graphics", "render_distance", 5)
	InstallRoot.save_config()
	var back := ConfigFile.new()
	back.load(path)
	_check(int(back.get_value("button.trim_down", "button", -99)) == 9, "the other instance's pad binding survives our save (%s)" % str(back.get_value("button.trim_down", "button", -99)))
	_check(int(back.get_value("merge_test", "theirs", 0)) == 2, "... and so does its new key")
	_check(int(back.get_value("merge_test", "ours", 0)) == 3 and int(back.get_value("graphics", "render_distance", 0)) == 5, "our own changes are written")
	_check(int(InstallRoot.get_value("button.trim_down", "button", -99)) == 9, "the in-memory config has taken the file's value")
	# a key we erase stays erased, one they erased is not resurrected
	other.load(path)
	other.erase_section_key("merge_test", "theirs")
	other.save(path)
	InstallRoot.erase_section_key("merge_test", "ours")
	InstallRoot.save_config()
	back = ConfigFile.new()
	back.load(path)
	_check(not back.has_section_key("merge_test", "theirs") and not back.has_section_key("merge_test", "ours"), "erased keys stay erased on both sides")

	# Settings > Cancel: unsaved pad changes go, the file's bindings stay
	_check(JoystickConfig.has_method("reload"), "JoystickConfig.reload exists")
	var b: Dictionary = JoystickConfig.button_bindings.duplicate(true)
	b["flip_camera"] = {"button": 5}
	JoystickConfig.button_bindings = b
	JoystickConfig.reload()
	_check(int(JoystickConfig.button_bindings["flip_camera"]["button"]) == int(InstallRoot.get_value("button.flip_camera", "button", 1)), "reload discards the unsaved change (flip_camera %s)" % str(JoystickConfig.button_bindings["flip_camera"]["button"]))
	_check(int(JoystickConfig.button_bindings["trim_down"]["button"]) == 9, "reload reads the file, not the defaults (trim_down %s)" % str(JoystickConfig.button_bindings["trim_down"]["button"]))
	print("smoke_config_merge: %s" % ("OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails > 0 else 0)
