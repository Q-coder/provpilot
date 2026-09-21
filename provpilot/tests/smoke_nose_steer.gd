extends Node
## Smoke: the A-10's nose-wheel steering range toggle (aircraft.cfg [gear]
## steer_limited_deg / steer_full_deg, Ctrl+W → JSBGodot.set_nose_steer_full).
## Full pedal must give the limited angle by default and the full angle when
## engaged, read back from JSBSim's fcs/steer-pos-deg[0].
## Run: godot --headless --path provpilot res://tests/smoke_nose_steer.tscn
const E := 2683732.0
const N := 1257695.0
var _root: Node = null
var _fails := 0
func _check(cond: bool, msg: String) -> void:
	if cond: print("  ok   " + msg)
	else: _fails += 1; printerr("  FAIL " + msg)
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("a10", E, N, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(10.0).timeout
	var jsb = _root.jsb_node
	print("[nose] sim-time %.1f s, slew %s, wow %.0f, agl %.2f m" % [jsb.get_property("simulation/sim-time-sec"), str(jsb.is_slew_mode()), jsb.get_property("gear/wow"), jsb.get_property("position/h-agl-ft") * 0.3048])
	_check(not jsb.get_nose_steer_full(), "starts in the limited range")
	print("[nose] cfg: limited %.1f full %.1f (aircraft %s)" % [_root._aircraft_cfg.get_float("gear", "steer_limited_deg", -1.0), _root._aircraft_cfg.get_float("gear", "steer_full_deg", -1.0), _root._aircraft_id])
	jsb.set_input_rudder(1.0)
	await get_tree().create_timer(1.0).timeout
	print("[nose] steer-cmd-norm %.2f, unit[0] angle %.2f, steer-pos[0] %.2f, sim-time %.1f" % [jsb.get_property("fcs/steer-cmd-norm"), jsb.get_property("fcs/steer-pos-deg[0]"), jsb.get_property("fcs/steer-pos-deg[0]"), jsb.get_property("simulation/sim-time-sec")])
	var a_lim: float = absf(jsb.get_property("fcs/steer-pos-deg[0]"))
	jsb.set_nose_steer_full(true)
	await get_tree().create_timer(1.0).timeout
	var a_full: float = absf(jsb.get_property("fcs/steer-pos-deg[0]"))
	print("[nose] FULL: get_nose_steer_full %s, steer-cmd-norm %.2f, steer-pos[0] %.2f, unit[0] %.2f" % [str(jsb.get_nose_steer_full()), jsb.get_property("fcs/steer-cmd-norm"), jsb.get_property("fcs/steer-pos-deg[0]"), jsb.get_property("gear/unit[0]/steering-angle-deg")])
	jsb.set_nose_steer_full(false)
	await get_tree().create_timer(1.0).timeout
	var a_back: float = absf(jsb.get_property("fcs/steer-pos-deg[0]"))
	print("[nose] full pedal: limited %.1f deg, full %.1f deg, back %.1f deg" % [a_lim, a_full, a_back])
	_check(absf(a_lim - 5.0) < 0.5, "limited range is the FDM max_steer (5 deg)")
	_check(absf(a_full - 60.0) < 1.0, "full range is steer_full_deg (60 deg)")
	_check(absf(a_back - 5.0) < 0.5, "toggling back restores the limited range")
	if _fails == 0:
		print("smoke_nose_steer: all checks passed")
		get_tree().quit(0)
	else:
		printerr("smoke_nose_steer: %d failure(s)" % _fails)
		get_tree().quit(1)
