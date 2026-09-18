extends Node
## Smoke test: a pedal input must turn the aircraft the same way on the ground
## (nose-wheel steering, fcs/steer-cmd-norm) and in the air (rudder). JSBSim's
## two commands have opposite sign conventions; the bridge compensates
## (see copy_inputs_to_JSBSim). Both ground trials must follow the pedal.
## Run: godot --headless --path provpilot res://tests/smoke_ground_steering.tscn
const E := 2683732.0
const N := 1257695.0
var _root: Node = null
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", E, N, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var ac: Node3D = _root.get_node("AC")
	var jsb = _root.jsb_node
	# --- ground trial A: opposite command first (isolates steering from torque) ---
	var hA0: float = jsb.get_heading()
	jsb.set_input_throttle(0.25)
	jsb.set_input_rudder(-1.0)
	await get_tree().create_timer(6.0).timeout
	var hA1: float = jsb.get_heading()
	print("[rudder] GROUND A: rudder=-1 -> delta %.1f deg (steer-pos %.1f, yaw-rate %.3f, %.1f kts)" % [
		fposmod(hA1 - hA0 + 180.0, 360.0) - 180.0, jsb.get_property("fcs/steer-pos-deg[0]"), jsb.get_property("velocities/r-rad_sec"), jsb.get_airspeed_knots()])
	jsb.set_input_rudder(0.0)
	await get_tree().create_timer(2.0).timeout
	# --- ground trial B: right rudder command ---
	var h0: float = jsb.get_heading()
	jsb.set_input_rudder(1.0)
	await get_tree().create_timer(3.0).timeout
	print("[rudder] props: rudder-cmd=%.2f steer-cmd=%.2f yaw-rate=%.3f pitch=%.2f" % [
		jsb.get_property("fcs/rudder-cmd-norm"), jsb.get_property("fcs/steer-cmd-norm"),
		jsb.get_property("velocities/r-rad_sec"), jsb.get_pitch_deg()])
	for i in range(3):
		print("[rudder]   gear[%d]: steer=%.2f deg steer-pos=%.2f wow=%.0f compression=%.3f ft" % [i,
			jsb.get_property("gear/unit[%d]/steering-angle-deg" % i), jsb.get_property("fcs/steer-pos-deg[%d]" % i),
			jsb.get_property("gear/unit[%d]/WOW" % i), jsb.get_property("gear/unit[%d]/compression-ft" % i)])
	await get_tree().create_timer(3.0).timeout
	var h1: float = jsb.get_heading()
	var d_ground: float = fposmod(h1 - h0 + 180.0, 360.0) - 180.0
	print("[rudder] GROUND: rudder=+1, heading %.1f -> %.1f (delta %.1f deg), speed %.1f kts" % [h0, h1, d_ground, jsb.get_airspeed_knots()])
	jsb.set_input_rudder(0.0)
	jsb.set_input_throttle(0.0)
	# --- air: re-init at 1500 m AGL, 110 kts, right rudder ---
	var g: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_position)
	var xf: Transform3D = _root._ac_transform_at(E, N, g.z + 1500.0, 160.0, ac.scale)
	jsb.set_slew_mode(true)
	ac.global_transform = xf
	jsb.reinitialize_from_pose(xf, 110.0, g.z)
	jsb.set_slew_mode(false)
	await get_tree().create_timer(1.0).timeout
	var a0: float = jsb.get_heading()
	jsb.set_input_throttle(0.7)
	jsb.set_input_rudder(1.0)
	await get_tree().create_timer(3.0).timeout
	var a1: float = jsb.get_heading()
	var d_air: float = fposmod(a1 - a0 + 180.0, 360.0) - 180.0
	print("[rudder] AIR: rudder=+1, heading %.1f -> %.1f (delta %.1f deg), roll %.1f" % [a0, a1, d_air, jsb.get_roll_deg()])
	var dA: float = fposmod(hA1 - hA0 + 180.0, 360.0) - 180.0
	print("[rudder] signs: ground(-1) %s, ground(+1) %s, air(+1) %s" % ["right" if dA > 0 else "left", "right" if d_ground > 0 else "left", "right" if d_air > 0 else "left"])
	var ok := true
	if not ((d_ground > 5.0) == (d_air > 0.0)):
		printerr("FAIL: ground turn for rudder=+1 (%.1f deg) does not follow the air turn (%.1f deg)" % [d_ground, d_air]); ok = false
	if not ((dA < -5.0) == (d_air > 0.0)):
		printerr("FAIL: ground turn for rudder=-1 (%.1f deg) should be opposite to rudder=+1" % dA); ok = false
	if absf(d_air) < 3.0:
		printerr("FAIL: no yaw response in the air (%.1f deg)" % d_air); ok = false
	print("smoke_ground_steering: %s" % ("OK" if ok else "FAILED"))
	get_tree().quit(0 if ok else 1)
