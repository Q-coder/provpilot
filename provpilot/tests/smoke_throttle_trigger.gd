extends Node
## Gamepad trigger throttle: a trigger resting a little off centre (shared
## trigger axis, Logitech F310 in D mode) must not move the throttle; a real
## pull does; letting go to a drifted rest stops it.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_throttle_trigger.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _trig(axis: JoyAxis, v: float) -> void:
	var ev := InputEventJoypadMotion.new()
	ev.device = 0
	ev.axis = axis
	ev.axis_value = v
	Input.parse_input_event(ev)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	var jsb = root.jsb_node
	jsb.set_input_throttle(0.6)
	await get_tree().process_frame
	# drifted rest: 0.15 on the left trigger for a second -> no change
	_trig(JOY_AXIS_TRIGGER_LEFT, 0.15)
	await get_tree().create_timer(1.0).timeout
	_check(absf(jsb.get_input_throttle() - 0.6) < 1e-4, "left trigger at 0.15 (drift) leaves the throttle alone (%.3f)" % jsb.get_input_throttle())
	# a real pull decreases
	_trig(JOY_AXIS_TRIGGER_LEFT, 1.0)
	await get_tree().create_timer(0.5).timeout
	var after_pull: float = jsb.get_input_throttle()
	_check(after_pull < 0.5 and after_pull > 0.3, "full left trigger for 0.5 s decreases by about 0.15 (%.3f)" % after_pull)
	# release to a drifted rest: stops
	_trig(JOY_AXIS_TRIGGER_LEFT, 0.2)
	await get_tree().create_timer(1.0).timeout
	_check(absf(jsb.get_input_throttle() - after_pull) < 1e-4, "release to 0.2 stops the decrease (%.3f)" % jsb.get_input_throttle())
	# right trigger symmetric
	_trig(JOY_AXIS_TRIGGER_LEFT, 0.0)
	_trig(JOY_AXIS_TRIGGER_RIGHT, 1.0)
	await get_tree().create_timer(0.5).timeout
	_trig(JOY_AXIS_TRIGGER_RIGHT, 0.0)
	_check(jsb.get_input_throttle() > after_pull + 0.1, "right trigger increases again (%.3f)" % jsb.get_input_throttle())
	if _fails.is_empty():
		print("smoke_throttle_trigger: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_throttle_trigger: %d failure(s)" % _fails.size())
		get_tree().quit(1)
