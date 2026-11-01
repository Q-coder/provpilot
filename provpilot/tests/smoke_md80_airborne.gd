extends Node
## MD-82 airborne start (docs/MD80_FLIGHTGEAR_PORT.md step 8): the trim lands
## on the stabilizer with the column centred, the IVSI follows the climb rate
## and the CG bar follows the fuel slider at once.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_airborne.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	# Settings > Aircraft > Start: 8000 ft above the ground at 250 kt (the flight request reads these keys)
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 8000.0
	root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 250.0
	root._settings.get_settings()["aircraft_md80_start_heading_deg"] = 155.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	_check(jsb != null and jsb.is_initialized() and root._spawn.is_airborne_start(), "JSBSim MD-82 initialised airborne")
	await get_tree().create_timer(8.0).timeout
	var trim: float = jsb.get_pitch_trim()
	var ele: float = jsb.get_property("fcs/elevator-cmd-norm")
	var stab: float = jsb.get_property("/systems/fcs/stabilizer/final-deg")
	var fg_trim: float = jsb.get_property("/controls/flight/elevator-trim")
	var hdot: float = jsb.get_property("velocities/h-dot-fps")
	var vsi: float = jsb.get_property("/instrumentation/vertical-speed-indicator[0]/vs-fpm")
	var af_vs: float = jsb.get_property("/it-autoflight/internal/vert-speed-fpm")
	print("  [diag] t+8 s: pitch trim %.3f, elevator cmd %.3f, stabilizer %.2f deg, FG elevator-trim %.3f, alpha %.1f, theta %.1f, vc %.0f kt, h-dot %.1f fps" % [trim, ele, stab, fg_trim, jsb.get_property("aero/alpha-deg"), jsb.get_property("attitude/theta-deg"), jsb.get_property("velocities/vc-kts"), hdot])
	print("  [diag] IVSI %.0f fpm, autoflight vs %.0f fpm" % [vsi, af_vs])
	print("  [diag] engines: n2 %.1f thrust %.0f lb throttle pos %.2f cutoff %s pipe %.1f lb feed-l %s state %s rated factor %.2f ac-l %.0f V" % [jsb.get_property("propulsion/engine[0]/n2"), jsb.get_property("propulsion/engine[0]/thrust-lbs"), jsb.get_property("fcs/throttle-pos-norm[0]"), str(jsb.get_property("propulsion/engine[0]/cutoff")), jsb.get_property("propulsion/tank[5]/contents-lbs"), str(jsb.get_property("/systems/fuel/feed-l")), str(jsb.get_property("/engines/engine[0]/state")), jsb.get_property("/systems/engines/limit/rated-thrust-factor"), jsb.get_property("/systems/electrical/bus/ac-l")])
	print("  [diag] limiter: autoconfig %s mode %s target %.0f actual %.0f factor %.2f lever %s eng2 thrust %.0f" % [str(jsb.get_property("/systems/acconfig/autoconfig-running")), str(jsb.get_property("/systems/engines/limit/active-mode-int")), jsb.get_property("/systems/engines/limit/rated-thrust-target"), jsb.get_property("/systems/engines/limit/rated-thrust-actual"), jsb.get_property("/systems/engines/limit/rated-thrust-factor"), str(jsb.get_property("/controls/engines/engine[0]/throttle")), jsb.get_property("propulsion/engine[1]/thrust-lbs")])
	_check(absf(ele) < 0.05, "column centred after the airborne trim (elevator cmd %.3f)" % ele)
	_check(trim < -0.02 and absf(stab - 12.5 * trim) < 0.6, "trim carried by the stabilizer (%.3f -> %.2f deg)" % [trim, stab])
	_check(absf(fg_trim - trim) < 0.01, "FlightGear elevator-trim mirrors the pitch trim command (%.3f)" % fg_trim)
	_check(absf(hdot) < 10.0, "near level after release (h-dot %.1f fps)" % hdot)
	_check(absf(af_vs - hdot * 60.0) < 150.0, "autoflight vertical speed linked from h-dot (%.0f vs %.0f fpm)" % [af_vs, hdot * 60.0])
	_check(absf(vsi - hdot * 60.0) < 400.0, "IVSI shows the vertical speed (%.0f fpm)" % vsi)
	# a climb: pull a little and the IVSI must go up
	jsb.set_input_elevator(-0.3)
	await get_tree().create_timer(4.0).timeout
	var hdot2: float = jsb.get_property("velocities/h-dot-fps")
	var vsi2: float = jsb.get_property("/instrumentation/vertical-speed-indicator[0]/vs-fpm")
	print("  [diag] pulling: h-dot %.1f fps, IVSI %.0f fpm" % [hdot2, vsi2])
	_check(hdot2 > 5.0 and vsi2 > 200.0, "IVSI follows a climb (%.0f fpm at %.1f fps)" % [vsi2, hdot2])
	jsb.set_input_elevator(0.0)
	# CG follows the fuel at once (the settings screen pauses the FDM)
	var cg0: Dictionary = jsb.get_cg_info()
	AircraftSettingsPanel.apply_fuel(jsb, 5.0)
	var cg1: Dictionary = jsb.get_cg_info()
	print("  [diag] CG %.2f in / %.2f %% MAC at %.0f lb -> %.2f in / %.2f %% MAC at %.0f lb with 5 %% fuel" % [cg0.get("cg_x_in", 0.0), cg0.get("pct_mac", 0.0), cg0.get("weight_lbs", 0.0), cg1.get("cg_x_in", 0.0), cg1.get("pct_mac", 0.0), cg1.get("weight_lbs", 0.0)])
	_check(cg1.get("weight_lbs", 0.0) < cg0.get("weight_lbs", 0.0) - 1000.0 and absf(cg1.get("cg_x_in", 0.0) - cg0.get("cg_x_in", 0.0)) > 0.01, "weight and CG change with the fuel slider without an FDM step")
	await get_tree().create_timer(2.0).timeout
	var cg2: Dictionary = jsb.get_cg_info()
	print("  [diag] CG two seconds later: %.2f in / %.2f %% MAC at %.0f lb" % [cg2.get("cg_x_in", 0.0), cg2.get("pct_mac", 0.0), cg2.get("weight_lbs", 0.0)])
	# gear: the FDM has no gear kinematic; PVP moves gear-pos-norm from the lever and the model follows
	var fga = root._fg_animator
	jsb.set_gear_down(false)
	await get_tree().create_timer(3.0).timeout
	var gp_mid: float = jsb.get_property("gear/gear-pos-norm")
	await get_tree().create_timer(9.0).timeout
	var gp_up: float = jsb.get_property("gear/gear-pos-norm")
	var anim_gear: float = float(fga.get_property("gear/gear[0]/position-norm"))
	print("  [diag] gear up: pos %.2f after 3 s, %.2f after 12 s, model property %.2f" % [gp_mid, gp_up, anim_gear])
	_check(gp_mid > 0.5 and gp_mid < 0.9 and gp_up < 0.01, "gear retracts over retract_seconds (%.2f -> %.2f)" % [gp_mid, gp_up])
	_check(anim_gear < 0.01, "exterior gear animation property follows (%.2f)" % anim_gear)
	jsb.set_gear_down(true)
	if _fails.is_empty():
		print("smoke_md80_airborne: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_md80_airborne: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
