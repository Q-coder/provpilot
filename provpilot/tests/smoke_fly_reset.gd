extends Node
## Smoke: a second "Fly!" (teleport path) must start a fresh session — no
## throttle, flaps, trim or speed carried over from the previous flight.
## Run: godot --headless --path provpilot res://tests/smoke_fly_reset.tscn
var _root: Node = null
var _fails: Array[String] = []
func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var jsb = _root.jsb_node
	# Fly for a bit with power, flaps and trim set.
	jsb.set_input_throttle(0.9)
	jsb.set_flaps(1.0)
	jsb.set_input_pitch(-0.3)
	await get_tree().create_timer(8.0).timeout
	print("[reset] before: speed %.1f kts throttle %.2f flaps %.2f" % [jsb.get_airspeed_knots(), jsb.get_input_throttle(), jsb.get_flaps()])
	if jsb.get_airspeed_knots() < 5.0:
		_fail("aircraft did not accelerate before the second Fly! (%.1f kts)" % jsb.get_airspeed_knots())
	# Second Fly! at another airport (teleport path).
	_root._on_welcome_flight_requested("c172p", 2682163.0, 1282948.0, 250.0)
	await get_tree().create_timer(8.0).timeout
	print("[reset] after: speed %.1f kts throttle %.2f flaps %.2f elevator %.2f" % [jsb.get_airspeed_knots(), jsb.get_input_throttle(), jsb.get_flaps(), jsb.get_input_elevator()])
	if jsb.get_input_throttle() > 0.01: _fail("throttle carried over (%.2f)" % jsb.get_input_throttle())
	if jsb.get_flap_cmd() > 0.01 or jsb.get_flaps() > 0.2: _fail("flaps carried over (cmd %.2f pos %.2f)" % [jsb.get_flap_cmd(), jsb.get_flaps()])
	if absf(jsb.get_input_elevator()) > 0.01: _fail("elevator carried over (%.2f)" % jsb.get_input_elevator())
	if jsb.get_airspeed_knots() > 15.0: _fail("speed carried over (%.1f kts)" % jsb.get_airspeed_knots())
	if _fails.is_empty():
		print("smoke_fly_reset: OK"); get_tree().quit(0)
	else:
		printerr("smoke_fly_reset: %d failure(s)" % _fails.size()); get_tree().quit(1)
