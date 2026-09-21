extends Node
## Cockpit zoom: Page Up / Page Down (and L1 / R1) change the cockpit camera's
## field of view through the C++ CameraRig, like the external cameras' zoom.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_cockpit_zoom.tscn
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _key(code: Key, pressed: bool) -> void:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.physical_keycode = code
	ev.pressed = pressed
	Input.parse_input_event(ev)

func _ready() -> void:
	var ac := Node3D.new()
	add_child(ac)
	var cam1 := Camera3D.new()
	var cam2 := Camera3D.new()
	ac.add_child(cam1)
	add_child(cam2)
	cam1.fov = 65.0
	var rig := CameraRig.new()
	rig.register_cameras(cam1, cam2, ac)
	rig.set_mode(0)
	rig.set_cockpit_enabled(true)
	rig.update(0.1, -1)
	_check(absf(cam1.fov - 65.0) < 1e-4, "FOV untouched without zoom input")
	_key(KEY_PAGEUP, true)
	await get_tree().process_frame
	rig.update(0.5, -1)
	_key(KEY_PAGEUP, false)
	await get_tree().process_frame
	_check(cam1.fov < 50.0 and cam1.fov > 40.0, "Page Up narrows the FOV by 20 deg in half a second (%.1f)" % cam1.fov)
	_key(KEY_PAGEDOWN, true)
	await get_tree().process_frame
	rig.update(2.0, -1)
	_key(KEY_PAGEDOWN, false)
	await get_tree().process_frame
	_check(absf(cam1.fov - 100.0) < 1e-3, "Page Down widens up to the 100 deg limit (%.1f)" % cam1.fov)
	_key(KEY_PAGEUP, true)
	await get_tree().process_frame
	rig.update(5.0, -1)
	_key(KEY_PAGEUP, false)
	await get_tree().process_frame
	_check(absf(cam1.fov - 15.0) < 1e-3, "zoom in stops at the 15 deg limit (%.1f)" % cam1.fov)
	# head look survives a cycle through the external views (like their positions do)
	_key(KEY_RIGHT, true)
	await get_tree().process_frame
	rig.update(0.5, -1)
	_key(KEY_RIGHT, false)
	await get_tree().process_frame
	var looked: Basis = cam1.transform.basis
	_check(absf(looked.z.x) > 0.1, "arrow right turns the head")
	rig.flip()   # orbit
	rig.flip()   # chase
	rig.flip()   # cockpit again
	rig.update(0.1, -1)
	_check(rig.get_mode() == 0 and cam1.transform.basis.is_equal_approx(looked) and absf(cam1.fov - 15.0) < 1e-3, "back in the cockpit the head direction and zoom are kept")
	# Space held + mouse = look around (no recentre on release after a long hold)
	_key(KEY_SPACE, true)
	await get_tree().process_frame
	rig.update(0.1, -1)
	_check(rig.is_mouse_look_active(), "Space held in the cockpit arms the mouse look")
	rig.mouse_look(Vector2(-200.0, 0.0))   # 200 px left = 30 deg
	rig.update(0.1, -1)
	var after_mouse: Basis = cam1.transform.basis
	_check(not after_mouse.is_equal_approx(looked) and after_mouse.z.x > looked.z.x, "mouse motion left turns the head back left while Space is held")
	rig.update(1.0, -1)
	_key(KEY_SPACE, false)
	await get_tree().process_frame
	rig.update(0.1, -1)
	_check(not rig.is_mouse_look_active() and cam1.transform.basis.is_equal_approx(after_mouse), "releasing Space after a long hold keeps the view")
	rig.mouse_zoom(1)
	_check(absf(cam1.fov - 15.0) < 1e-3, "wheel without Space does nothing")
	# Space + wheel zooms (and does not count as a tap)
	_key(KEY_SPACE, true)
	await get_tree().process_frame
	rig.update(0.05, -1)
	rig.mouse_zoom(-3)
	_check(absf(cam1.fov - 30.0) < 1e-3, "three wheel notches down widen the view by 15 deg (fov %.1f)" % cam1.fov)
	rig.mouse_zoom(1)
	_check(absf(cam1.fov - 25.0) < 1e-3, "one notch up narrows it by 5 deg (fov %.1f)" % cam1.fov)
	_key(KEY_SPACE, false)
	await get_tree().process_frame
	rig.update(0.05, -1)
	_check(absf(cam1.fov - 25.0) < 1e-3 and cam1.transform.basis.is_equal_approx(after_mouse), "releasing Space after wheel use is not a tap: view kept")
	# a short tap recentres head and zoom
	_key(KEY_SPACE, true)
	await get_tree().process_frame
	rig.update(0.05, -1)
	_key(KEY_SPACE, false)
	await get_tree().process_frame
	rig.update(0.05, -1)
	_check(cam1.transform.basis.is_equal_approx(Basis.IDENTITY) and absf(cam1.fov - 65.0) < 1e-3, "a Space tap recentres the head and resets the zoom (fov %.1f)" % cam1.fov)
	if _fails.is_empty():
		print("smoke_cockpit_zoom: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_cockpit_zoom: %d failure(s)" % _fails.size())
		get_tree().quit(1)
