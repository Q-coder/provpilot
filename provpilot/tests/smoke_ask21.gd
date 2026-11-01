extends Node
## ASK 21 glider (docs/ASK21_PORT.md): the FlightGear JSBSim glider loads, the
## model and its instruments animate, and the airborne start from the Aircraft
## tab defaults ([start] agl_ft / speed_kt) puts it in a trimmed glide over the
## airport: still flying after a while, speed held, a glider's sink rate.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_ask21.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _fdm_diag(jsb, label: String) -> void:
	var keys := ["fcs/speedbrake-pos-norm", "fcs/speedbrake-cmd-norm", "fcs/flap-pos-deg", "fcs/elevator-pos-norm", "fcs/elevator-cmd-norm", "fcs/pitch-trim-cmd-norm",
		"aero/qbar-psf", "aero/coefficient/CLwbh", "aero/coefficient/CDo", "aero/coefficient/CDi", "aero/coefficient/CDsb", "forces/fbx-aero-lbs", "forces/fbz-aero-lbs", "forces/fbx-external-lbs", "forces/fbz-external-lbs",
		"moments/m-aero-lbsft", "inertia/weight-lbs", "velocities/vt-fps", "velocities/vc-kts", "velocities/ve-kts", "flight-path/gamma-deg", "gear/wow", "external_reactions/turn/magnitude", "external_reactions/pull/magnitude", "fcs/winch-cmd-norm",
		"aero/beta-deg", "attitude/phi-deg", "velocities/p-rad_sec", "velocities/r-rad_sec", "fcs/aileron-cmd-norm", "fcs/rudder-cmd-norm", "fcs/roll-trim-cmd-norm", "fcs/yaw-trim-cmd-norm", "atmosphere/wind-north-fps", "atmosphere/wind-east-fps", "atmosphere/turb-type", "moments/l-aero-lbsft", "moments/n-aero-lbsft"]
	var parts: PackedStringArray = []
	for k in keys:
		parts.append("%s=%s" % [k.get_file() if k.begins_with("aero/coefficient") else k, str(jsb.get_property(k)).left(8)])
	print("  [fdm %s] %s" % [label, " ".join(parts)])

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	# the cfg defaults (3000 ft, 55 kt): no stored start settings interfere
	if root._settings:
		for k in ["aircraft_ask21_start_agl_ft", "aircraft_ask21_start_speed_kt", "aircraft_ask21_start_heading_deg"]:
			root._settings.get_settings().erase(k)
		# calm air from the first frame: the user's static layer (wind, turbulence)
		# applies below its base too, and a hands-off glider in a 5 kt wind step
		# plus light turbulence banks 30 deg within 15 s (docs/ASK21_PORT.md)
		root._settings.get_settings()["weather_layers"] = []
		root._settings.get_settings()["live_weather"] = false
		# the JSBGodot node exists from scene start and got the cfg weather in
		# _ready; a flight start does not push it again, so push the calm set now
		root._push_weather_settings()
	root._on_welcome_flight_requested("ask21", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(2.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	_check(jsb != null and jsb.is_initialized(), "JSBSim ASK 21 initialised")
	_check(root._spawn.is_airborne_start(), "airborne start requested by the cfg defaults")
	var agl0: float = jsb.get_property("position/h-agl-ft")
	var vc0: float = jsb.get_property("velocities/vc-kts")
	print("  [diag] t0: agl %.0f ft, vc %.1f kt, alpha %.1f deg, elevator %.2f, pitch %.1f" % [agl0, vc0, jsb.get_property("aero/alpha-deg"), jsb.get_property("fcs/elevator-pos-norm"), jsb.get_pitch_deg()])
	_fdm_diag(jsb, "t0")
	_check(agl0 > 2500.0 and agl0 < 3300.0, "starts about 3000 ft above the ground (%.0f ft)" % agl0)
	_check(absf(vc0 - 55.0) < 5.0, "starts at the cfg airspeed (%.1f kt)" % vc0)
	# model
	var fga: FGAnimator = root._aircraft.get_fg_animator()
	_check(fga != null and fga.animation_count() > 200, "model animations loaded (%d)" % (fga.animation_count() if fga else 0))
	var unknown: PackedStringArray = fga.unknown_properties() if fga else PackedStringArray()
	print("  [diag] unbridged model properties (%d): %s" % [unknown.size(), str(unknown)])
	var snd = root._aircraft.get_fg_sounds()
	_check(snd != null and snd.get_count() >= 4, "wind sound set loaded (%d)" % (snd.get_count() if snd else 0))
	# glide
	await get_tree().create_timer(15.0).timeout
	var agl1: float = jsb.get_property("position/h-agl-ft")
	var vc1: float = jsb.get_property("velocities/vc-kts")
	var sink_mps: float = -jsb.get_property("velocities/h-dot-fps") * 0.3048
	print("  [diag] t+15 s: agl %.0f ft, vc %.1f kt, sink %.2f m/s, pitch %.1f, roll %.1f, alpha %.1f" % [agl1, vc1, sink_mps, jsb.get_pitch_deg(), jsb.get_roll_deg(), jsb.get_property("aero/alpha-deg")])
	_fdm_diag(jsb, "t+15")
	_check(agl1 > 2000.0, "still well above the ground after 15 s (%.0f ft)" % agl1)
	_check(vc1 > 40.0 and vc1 < 75.0, "airspeed held near the trim speed (%.1f kt)" % vc1)
	_check(sink_mps > 0.2 and sink_mps < 3.0, "a glider's sink rate (%.2f m/s)" % sink_mps)
	_check(absf(jsb.get_roll_deg()) < 15.0, "wings about level (%.1f deg)" % jsb.get_roll_deg())
	# instruments fed by the bridge
	var br: FGBridge = root._aircraft.get_fg_bridge()
	print("  [diag] variometer %s m/s, ASI %s kt, altimeter %s ft" % [str(fga.get_property("instrumentation/variometer/te-reading-mps")), str(fga.get_property("instrumentation/airspeed-indicator/indicated-speed-kt")), str(fga.get_property("instrumentation/altimeter/indicated-altitude-ft"))])
	var te = fga.get_property("instrumentation/variometer/te-reading-mps")
	_check(te != null and absf(float(te) + sink_mps) < 1.5, "total-energy variometer shows the sink")
	# instruments: one ASI / altimeter variant visible (km/h, metres), the others hidden
	var asi_kph: Node3D = root._ac.find_child("Sub_80mm_asi_6fms4", true, false)
	var asi_kt: Node3D = root._ac.find_child("Sub_80mm_asi_6fms4_001", true, false)
	var alt_ft: Node3D = root._ac.find_child("Sub_winter-4320", true, false)
	_check(asi_kph != null and asi_kph.is_visible_in_tree(), "km/h airspeed indicator shown")
	_check(asi_kt != null and not asi_kt.is_visible_in_tree(), "knots airspeed indicator hidden")
	_check(alt_ft != null and not alt_ft.is_visible_in_tree(), "feet altimeter hidden")
	var vsi_kt: Node3D = root._ac.find_child("Sub_winter-vario-5stv5-knots", true, false)
	_check(vsi_kt != null and not vsi_kt.is_visible_in_tree(), "knots variometer hidden (one needle on the m/s dial)")
	# duplicated instruments animate their own parts only: the needles and the compass
	# cards stay on their instruments (a shared name once composed four rotations)
	for nm in ["57mm_compass__scala", "57mm_compass__scala_003", "winter-4110__needle-m", "winter-4110__needle-m_003"]:
		var tf: Transform3D = fga.animated_transform(nm)
		_check(tf.origin.length() < 0.1, "%s stays on its instrument (offset %.2f m)" % [nm, tf.origin.length()])
	# the altimeter needle turns once per 1000 m: the long needle's angle matches the HUD altitude
	var alt_m: float = jsb.get_property("position/h-sl-ft") * 0.3048
	var needle_deg: float = rad_to_deg(fga.animated_transform("winter-4110__needle-m").basis.get_rotation_quaternion().get_angle())
	var expect_deg: float = fmod(alt_m, 1000.0) * 0.36
	# Quaternion.get_angle() is 0..2pi and depends on the sign of w: fold both to 0..180
	expect_deg = minf(expect_deg, 360.0 - expect_deg)
	needle_deg = minf(needle_deg, 360.0 - needle_deg)
	print("  [diag] altimeter needle %.1f deg, expected %.1f deg for %.0f m" % [needle_deg, expect_deg, alt_m])
	_check(absf(needle_deg - expect_deg) < 8.0, "altimeter needle reads the altitude in metres")
	# control surfaces follow the stick with the YASim scaling (full elevator about 24 deg on the model)
	var el0: Transform3D = fga.animated_transform("elevator")
	jsb.set_input_elevator(-1.0)
	jsb.set_input_rudder(1.0)
	await get_tree().create_timer(1.5).timeout
	var el_deg: float = rad_to_deg((el0.basis.inverse() * fga.animated_transform("elevator").basis).get_rotation_quaternion().get_angle())
	var rud_deg: float = rad_to_deg(fga.animated_transform("rudder").basis.get_rotation_quaternion().get_angle())
	print("  [diag] elevator model %.1f deg (FDM %.1f deg), rudder model %.1f deg (FDM %.1f deg)" % [el_deg, jsb.get_property("fcs/elevator-pos-deg"), rud_deg, jsb.get_property("fcs/rudder-pos-deg")])
	_check(el_deg > 10.0, "elevator moves on the model")
	_check(rud_deg > 10.0, "rudder moves on the model")
	jsb.set_input_elevator(0.0)
	jsb.set_input_rudder(0.0)
	_check(fga.get_property("instrumentation/yawstring") != null, "yaw string property published")
	# metric panel: the HUD readouts follow (aircraft.cfg [instruments] units)
	_check(root._hud.is_metric(), "HUD readouts metric for the glider")
	# controls: the throttle inputs work the airbrake lever ([controls] throttle_drives)
	_check(jsb.get_throttle_drives_airbrakes() and jsb.get_input_throttle() > 0.99, "throttle inputs drive the airbrakes, lever forward = brakes in")
	_check(jsb.get_property("fcs/speedbrake-pos-norm") < 0.01, "airbrakes in while gliding")
	jsb.set_input_throttle(0.0)
	await get_tree().create_timer(4.0).timeout
	var sb = fga.get_property("surface-positions/speedbrake-pos-norm")
	print("  [diag] speedbrake pos %s, FDM %s" % [str(sb), str(jsb.get_property("fcs/speedbrake-pos-norm"))])
	_check(sb != null and float(sb) > 0.5, "throttle idle extends the airbrakes on the model")
	# the blue lever is FlightGear's throttle lever (1 = brakes in, 0 = out): it follows
	# the throttle input (the control state must not pin it) ...
	var lever = fga.get_property("controls/engines/engine/throttle")
	_check(lever != null and float(lever) < 0.1, "airbrake lever property follows the throttle input (%s)" % str(lever))
	_check(not fga.animated_transform("interior__airbrake-front").origin.is_zero_approx(), "airbrake lever mesh moved")
	# ... and the mouse binding on the lever drives the FDM back (property-adjust 0..1)
	root._aircraft._on_fg_control("controls/engines/engine/throttle", 1.0)
	await get_tree().create_timer(3.0).timeout
	_check(jsb.get_input_throttle() > 0.99 and jsb.get_property("fcs/speedbrake-pos-norm") < 0.05, "lever forward closes the airbrakes through the throttle input")
	# the canopy glass (Nasal-only bindings we cannot run) must not be clickable: it would
	# shadow every real control behind it (the lever) and paint the whole cockpit green
	var ctl: FGControls = root._aircraft.get_fg_controls()
	var canopy_items := 0
	var lever_items := 0
	for it in ctl.get_items():
		var nm: String = it["node"].name if it["node"] else ""
		if nm.begins_with("interior__canopy") or nm.begins_with("canopy_"):
			canopy_items += 1
		if nm.begins_with("interior__airbrake"):
			lever_items += 1
	_check(canopy_items == 0, "canopy glass is not a click target (%d)" % canopy_items)
	_check(lever_items == 2, "both airbrake levers are click targets (%d)" % lever_items)
	# instrument shadows / glass are alpha-blended (FlightGear model-transparent effect)
	var shadow: MeshInstance3D = root._ac.find_child("winter-4110__shadow", true, false)
	var shadow_ok := false
	if shadow != null and shadow.get_surface_override_material(0) != null:
		var bm := shadow.get_surface_override_material(0) as BaseMaterial3D
		shadow_ok = bm != null and bm.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED
	_check(shadow_ok, "altimeter shadow plane alpha-blended")
	if _fails.is_empty():
		print("smoke_ask21: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_ask21: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
