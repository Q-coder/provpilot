extends Node
## Cessna 172: FlightGear generic instruments in the panel (model/panel.xml,
## fg_panel_build.py), nose-wheel steering pivot, exterior light lenses.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_c172_panel.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

static func _centre_px(cam: Camera3D, n: Node3D) -> Vector2:
	return cam.unproject_position(n.global_transform * (n as MeshInstance3D).get_aabb().get_center())

## A point on the part's outer side (the master rocker halves hide each
## other's centre from the pilot's eye): the bounds centre shifted along the
## part's local X by [param side] × 40 % of its width.
static func _side_px(cam: Camera3D, n: Node3D, side: float) -> Vector2:
	var aabb: AABB = (n as MeshInstance3D).get_aabb()
	return cam.unproject_position(n.global_transform * (aabb.get_center() + Vector3(aabb.size.x * 0.4 * side, 0.0, 0.0)))

func _deg(fga: FGAnimator, node: String) -> float:
	return rad_to_deg(fga.animated_transform(node).basis.get_rotation_quaternion().get_angle())

func _near_mod360(a: float, expected: float, tol: float) -> bool:
	var d: float = fposmod(a - expected + 180.0, 360.0) - 180.0
	return absf(d) <= tol

static func _table(t: Array, x: float) -> float:
	if x <= t[0][0]:
		return t[0][1]
	for i in range(1, t.size()):
		if x <= t[i][0]:
			var f: float = (x - t[i - 1][0]) / (t[i][0] - t[i - 1][0])
			return lerpf(t[i - 1][1], t[i][1], f)
	return t[t.size() - 1][1]

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)   # headless default is tiny: adjacent rockers would share a pixel
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var fga: FGAnimator = root._fg_animator
	var jsb = root.jsb_node
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	_check(fga != null and fga.animation_count() >= 30, "panel animations loaded (%d)" % (fga.animation_count() if fga else 0))
	_check(root._fg_controls.count >= 4, "instrument knobs clickable (%d parts)" % root._fg_controls.count)
	for nm in ["asi__Needle", "ai__Face", "altimeter__needle", "tc__Airplane", "hi__Face", "vsi__Needle", "tach__Needle", "clock__SecondHand",
			"vor__LocalizerNeedle", "vor__Ring", "adf__Needle", "adf__Face", "kx165-1__NavUse1M", "kx165-1__NavKHz", "kr87__freq_001", "kr87__KR87_knob"]:
		_check(mroot.find_child(nm, true, false) != null, "instrument part present: " + nm)
	# radio navigation (docs/RADIO_NAVIGATION.md): NAV 1 defaults to KLO 114.85,
	# the Kloten VOR-DME on the field, so it is received on the ground
	_check(fga.get_property("instrumentation/nav/in-range") == 1.0 and str(fga.get_property("instrumentation/nav/nav-id")) == "KLO", "NAV 1 receives KLO at LSZH (in-range %s, id %s)" % [str(fga.get_property("instrumentation/nav/in-range")), str(fga.get_property("instrumentation/nav/nav-id"))])
	var dme_nm: float = float(fga.get_property("instrumentation/dme/indicated-distance-nm"))
	_check(dme_nm > 0.2 and dme_nm < 3.0, "DME reads the distance to KLO (%.2f nm)" % dme_nm)
	var magvar: float = float(fga.get_property("environment/magnetic-variation-deg"))
	var hdg_true: float = float(fga.get_property("orientation/heading-deg"))
	var hdg_mag: float = float(fga.get_property("instrumentation/magnetic-compass/indicated-heading-deg"))
	_check(magvar > 2.5 and magvar < 4.5 and _near_mod360(hdg_mag, hdg_true - magvar, 0.01), "compass reads magnetic: variation %.2f E, %.1f true -> %.1f mag" % [magvar, hdg_true, hdg_mag])
	var rn: RadioNav = root._aircraft.get_fg_bridge().get_radio_nav()
	_check(rn != null and rn.nav_in_range() and (rn.nav_to_flag() or rn.nav_from_flag()), "VOR flags show TO or FROM (radial %.0f)" % (rn.nav_radial_deg() if rn else -1.0))
	# ILS: tune 110.50 (IZH, runway 16 of the airport we stand on) through the control state
	var st: Dictionary = root._fg_controls.get_state()
	st["instrumentation/nav/frequencies/selected-mhz"] = 110.50
	root._fg_controls.set_state(st)
	await get_tree().create_timer(1.0).timeout
	_check(fga.get_property("instrumentation/nav/nav-loc") == 1.0 and str(fga.get_property("instrumentation/nav/nav-id")) == "IZH", "110.50 tunes the ILS IZH (nav-loc %s, id %s, %s)" % [str(fga.get_property("instrumentation/nav/nav-loc")), str(fga.get_property("instrumentation/nav/nav-id")), str(fga.get_property("instrumentation/nav/ils-runway"))])
	_check(fga.get_property("instrumentation/nav/has-gs") == 1.0, "ILS has a glide slope")
	st["instrumentation/nav/frequencies/selected-mhz"] = 114.85
	root._fg_controls.set_state(st)
	await get_tree().create_timer(1.0).timeout
	_check(absf(float(fga.get_property("instrumentation/nav/frequencies/selected-mhz")) - 114.85) < 0.001 and absf(float(fga.get_property("instrumentation/kr-87/outputs/selected-khz")) - 335.0) < 0.1, "KX 165 shows 114.85, KR 87 shows 335")
	# directional gyro face turns with the heading
	var hdg: float = float(fga.get_property("instrumentation/heading-indicator/indicated-heading-deg"))
	var face: float = _deg(fga, "hi__Face")
	_check(_near_mod360(face, hdg, 3.0) or _near_mod360(face, -hdg, 3.0), "heading indicator card at the heading (%.0f deg for %.0f)" % [face, hdg])
	# altimeter: 0.36 deg / ft, Kollsman at the QNH
	var alt: float = float(fga.get_property("instrumentation/altimeter/indicated-altitude-ft"))
	var alt_true: float = jsb.get_property("position/h-sl-ft")
	_check(absf(alt - alt_true) < 60.0, "altimeter reads the altitude (%.0f vs %.0f ft)" % [alt, alt_true])
	_check(_near_mod360(_deg(fga, "altimeter__needle"), fposmod(alt * 0.36, 360.0), 2.0) or _near_mod360(_deg(fga, "altimeter__needle"), fposmod(-alt * 0.36, 360.0), 2.0), "altimeter needle at 0.36 deg per ft")
	# tachometer from the piston engine
	var rpm: float = float(fga.get_property("engines/engine[0]/rpm"))
	var tach_table := [[0.0, -128.6], [0.1, -123.85], [500.0, -92.85], [1000.0, -57.1], [1500.0, -21.1], [2000.0, 15.4], [2500.0, 51.4], [3000.0, 87.4], [3500.0, 123.4]]
	var want: float = absf(_table(tach_table, rpm))
	print("  [diag] rpm %.0f, tach needle %.1f deg (table %.1f)" % [rpm, _deg(fga, "tach__Needle"), want])
	_check(rpm > 300.0, "engine rpm bridged (%.0f)" % rpm)
	_check(absf(_deg(fga, "tach__Needle") - want) < 3.0, "tachometer needle on its table value")
	# airspeed needle rests near zero on the ground
	var ias: float = float(fga.get_property("instrumentation/airspeed-indicator/indicated-speed-kt"))
	_check(ias < 30.0 and _deg(fga, "asi__Needle") < 40.0, "airspeed needle near zero on the ground (%.0f kt, %.1f deg)" % [ias, _deg(fga, "asi__Needle")])
	# clock second hand moves
	var s0: Transform3D = fga.animated_transform("clock__SecondHand")
	await get_tree().create_timer(1.5).timeout
	var s1: Transform3D = fga.animated_transform("clock__SecondHand")
	_check(not s0.basis.is_equal_approx(s1.basis), "clock second hand moves with the simulated time")
	# attitude indicator: the pitch card carries the roll too (nested animation)
	var ai_roll: Transform3D = fga.animated_transform("ai__Pitch")
	_check(ai_roll != Transform3D(), "attitude indicator pitch card animated")
	# nose wheel steers with the rudder: right pedal turns the wheel to the right (-X in the model)
	var steer: Node3D = mroot.find_child("NoseGear_Steer", true, false)
	_check(steer != null, "nose gear steering pivot present")
	if steer:
		var inv: Transform3D = (mroot as Node3D).global_transform.affine_inverse()
		jsb.set_input_rudder(1.0)
		await get_tree().create_timer(0.5).timeout
		var fwd: Vector3 = (inv * steer.global_transform).basis.z
		print("  [diag] steer basis.z with full right rudder: %s" % str(fwd))
		_check(absf(fwd.x) > 0.2, "nose wheel turns with rudder input (%.2f)" % fwd.x)
		_check(fwd.x < 0.0, "right rudder turns the nose wheel to the right")
		jsb.set_input_rudder(0.0)
	# tyres split out of the wheel objects, origin at the axle; they roll when taxiing
	var tyre_l: Node3D = mroot.find_child("MainWheel_L_Tyre", true, false)
	var tyre_n: Node3D = mroot.find_child("NoseWheel_Tyre", true, false)
	_check(tyre_l != null and tyre_n != null and mroot.find_child("MainWheel_R_Tyre", true, false) != null, "three tyre nodes present")
	if tyre_l and tyre_n:
		var inv3: Transform3D = (mroot as Node3D).global_transform.affine_inverse()
		var pl: Vector3 = inv3 * tyre_l.global_position
		var pn: Vector3 = inv3 * tyre_n.global_position
		print("  [diag] tyre origins in model space: left %s nose %s" % [str(pl), str(pn)])
		_check(pl.x > 1.3 and pl.x < 1.6 and pl.y > 0.1 and pl.y < 0.35, "left tyre origin on its axle")
		_check(absf(pn.x) < 0.02 and pn.z > 1.5 and pn.z < 1.8 and pn.y > 0.1 and pn.y < 0.3, "nose tyre origin on its axle, under the steering pivot")
		var b0: Basis = tyre_l.transform.basis
		var n0: Basis = tyre_n.transform.basis
		jsb.set_input_brake(0.0)
		root._parking_brake = false
		jsb.set_input_throttle(0.7)
		# rolling forward: the tyre's top moves forward (+Z in the model frame)
		var moved := false
		var top_z := 0.0
		for i in range(40):
			await get_tree().create_timer(0.1).timeout
			var b: Basis = (inv3 * tyre_l.global_transform).basis
			var spin_angle: float = (b0.inverse() * tyre_l.transform.basis).get_rotation_quaternion().get_angle()
			if not moved and spin_angle > 0.15:
				moved = true
				top_z = b.y.z
				print("  [diag] first spin %.2f rad after %.1f s at %.1f ft/s, top of the tyre tilted z %+.3f" % [spin_angle, (i + 1) * 0.1, jsb.get_ground_speed_fps(), top_z])
		var gs: float = jsb.get_ground_speed_fps()
		print("  [diag] ground speed %.1f ft/s after 4 s" % gs)
		_check(gs > 2.0 and moved and not tyre_n.transform.basis.is_equal_approx(n0), "tyres spin while rolling (%.1f ft/s)" % gs)
		_check(moved and top_z > 0.0, "tyres roll forward (top moves toward the nose)")
		jsb.set_input_throttle(0.0)
		jsb.set_input_brake(1.0)
	# exterior lights
	var L = root._aircraft_lights
	_check(L != null and L.fixture_count() >= 6, "nav, strobe, beacon and tail light fixtures (%d)" % (L.fixture_count() if L else 0))
	if L:
		# fixture sprites sit on the lenses: left wingtip +X, right wingtip -X, beacon on the fin (aft = -Z, high), tail light further aft
		var inv2: Transform3D = (mroot as Node3D).global_transform.affine_inverse()
		var at := {}
		for f in L.get_fixtures():
			# the sprite is a one-vertex point mesh: the vertex holds the lens position
			var v: Vector3 = ((f.sprite as MeshInstance3D).mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX] as PackedVector3Array)[0]
			at[f.kind] = inv2 * ((f.sprite as Node3D).global_transform * v)
		print("  [diag] fixtures in model space: ", at)
		_check(at.has("nav_left") and at["nav_left"].x > 5.0 and at.has("nav_right") and at["nav_right"].x < -5.0, "nav lights on the wingtips (left +X, right -X)")
		_check(at.has("beacon") and at["beacon"].z < -5.0 and at["beacon"].y > 2.5, "beacon on the fin top")
		_check(at.has("nav_tail") and at["nav_tail"].z < -6.5 and at["nav_tail"].y > 2.65 and at["nav_tail"].y < 2.76, "tail light at the top corner of the rudder's trailing edge (y %.2f z %.2f)" % [at["nav_tail"].y, at["nav_tail"].z])
		_check(at.has("landing") and at["landing"].x > 2.0 and at["landing"].z > 0.3, "landing light in the left wing's leading edge")
	# yokes: split from their shaft cones, turning with the aileron, hidden by a click on a cone
	var yoke_l: Node3D = mroot.find_child("Yoke_L", true, false)
	var cone_l: Node3D = mroot.find_child("Yoke_L_Cone", true, false)
	_check(yoke_l != null and cone_l != null and mroot.find_child("Yoke_R", true, false) != null, "yokes and shaft cones present")
	if yoke_l and cone_l:
		var yb: Transform3D = fga.animated_transform("Yoke_L")
		jsb.set_input_aileron(1.0)
		await get_tree().create_timer(0.5).timeout
		var ya: Transform3D = fga.animated_transform("Yoke_L")
		_check(not ya.basis.is_equal_approx(yb.basis), "yoke turns with the aileron")
		jsb.set_input_aileron(0.0)
		var ctl = root._fg_controls
		var cam: Camera3D = root.camera1
		cam.current = true
		await get_tree().create_timer(0.5).timeout
		var cpos: Vector2 = cam.unproject_position(cone_l.global_transform * (cone_l as MeshInstance3D).get_aabb().get_center())
		print("  [diag] yoke cone under the cursor: '%s'" % ctl.hover_name(cam, cpos))
		_check(ctl.click(cam, cpos, MOUSE_BUTTON_LEFT, true, false), "yoke shaft cone is clickable")
		ctl.click(cam, cpos, MOUSE_BUTTON_LEFT, false, false)
		await get_tree().create_timer(0.3).timeout
		_check(not yoke_l.visible and not (mroot.find_child("Yoke_R", true, false) as Node3D).visible, "clicking the cone hides both yokes")
		ctl.click(cam, cpos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, cpos, MOUSE_BUTTON_LEFT, false, false)
		await get_tree().create_timer(0.3).timeout
		_check(yoke_l.visible, "and a second click shows them again")
		# light switches: beacon and landing
		var sw_b: Node3D = mroot.find_child("Switch_BeaconLt", true, false)
		var sw_l: Node3D = mroot.find_child("Switch_LandingLt", true, false)
		_check(sw_b != null and sw_l != null and mroot.find_child("Switch_NavLt", true, false) != null and mroot.find_child("Switch_TaxiLt", true, false) != null and mroot.find_child("Switch_StrobeLt", true, false) != null, "five light rocker switches present")
		var sw_t: Node3D = mroot.find_child("Switch_TaxiLt", true, false)
		if sw_t:
			# the aircraft may still be rolling: measure in the model frame sampled the same frame
			var c0: Vector3 = (mroot as Node3D).global_transform.affine_inverse() * (sw_t.global_transform * (sw_t as MeshInstance3D).get_aabb().get_center())
			var tpos: Vector2 = _centre_px(cam, sw_t)
			ctl.click(cam, tpos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, tpos, MOUSE_BUTTON_LEFT, false, false)
			await get_tree().create_timer(0.3).timeout
			var c1: Vector3 = (mroot as Node3D).global_transform.affine_inverse() * (sw_t.global_transform * (sw_t as MeshInstance3D).get_aabb().get_center())
			print("  [diag] taxi rocker centre moved %.4f m when switched on" % (c1 - c0).length())
			_check((c1 - c0).length() < 0.01, "a switched-on rocker tilts about its own pivot and stays in place")
			_check(ctl.hover_name(cam, _centre_px(cam, sw_t)) == "Switch_TaxiLt", "and can be clicked again to switch it off")
			ctl.click(cam, _centre_px(cam, sw_t), MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, _centre_px(cam, sw_t), MOUSE_BUTTON_LEFT, false, false)
		# OBS: the VOR's course knob (invisible click zones from fgdata) turns the course ring
		var obs_zone: Node3D = mroot.find_child("vor__click-R", true, false)
		_check(obs_zone != null, "VOR OBS click zone present")
		if obs_zone:
			var obs0: float = float(ctl.value_of("instrumentation/nav/radials/selected-deg"))
			var opos: Vector2 = _centre_px(cam, obs_zone)
			print("  [diag] under the OBS zone: '%s'" % ctl.hover_name(cam, opos))
			var took: bool = ctl.click(cam, opos, MOUSE_BUTTON_WHEEL_UP, true, true)
			ctl.click(cam, opos, MOUSE_BUTTON_WHEEL_UP, false, true)
			var obs1: float = float(ctl.value_of("instrumentation/nav/radials/selected-deg"))
			_check(took and _near_mod360(obs1, obs0 + 5.0, 0.01), "Shift+wheel over the OBS turns the course 5 deg (%.0f -> %.0f)" % [obs0, obs1])
		# KR 87: the kHz knob tunes the standby frequency, FRQ swaps it into use
		var kr_knob: Node3D = mroot.find_child("kr87__KR87_knob1", true, false)
		var kr_frq: Node3D = mroot.find_child("kr87__KR87_frq", true, false)
		_check(kr_knob != null and kr_frq != null, "KR 87 knob and FRQ button present")
		if kr_knob and kr_frq:
			var stby0: float = float(ctl.value_of("instrumentation/kr-87/outputs/standby-khz"))
			var kpos: Vector2 = _centre_px(cam, kr_knob)
			ctl.click(cam, kpos, MOUSE_BUTTON_WHEEL_UP, true, false); ctl.click(cam, kpos, MOUSE_BUTTON_WHEEL_UP, false, false)
			var stby1: float = float(ctl.value_of("instrumentation/kr-87/outputs/standby-khz"))
			_check(absf(stby1 - stby0 - 1.0) < 0.01, "wheel over the ADF kHz knob steps the standby frequency (%.0f -> %.0f)" % [stby0, stby1])
			var use0: float = float(ctl.value_of("instrumentation/kr-87/outputs/selected-khz"))
			var fpos: Vector2 = _centre_px(cam, kr_frq)
			ctl.click(cam, fpos, MOUSE_BUTTON_LEFT, true, false)
			await get_tree().create_timer(0.3).timeout
			ctl.click(cam, fpos, MOUSE_BUTTON_LEFT, false, false)
			await get_tree().create_timer(0.3).timeout
			var use1: float = float(ctl.value_of("instrumentation/kr-87/outputs/selected-khz"))
			_check(absf(use1 - stby1) < 0.01 and absf(float(ctl.value_of("instrumentation/kr-87/outputs/standby-khz")) - use0) < 0.01, "FRQ swaps active and standby (%.0f <-> %.0f)" % [use0, use1])
		ctl.set_debug(true)
		_check(ctl.debug_box_count() == ctl.count, "Shift+Ctrl+M hotspot boxes, one per clickable part (%d)" % ctl.debug_box_count())
		ctl.set_debug(false)
		var sw_alt: Node3D = mroot.find_child("Switch_Alt", true, false)
		var sw_bat: Node3D = mroot.find_child("Switch_Bat", true, false)
		_check(sw_alt != null and sw_bat != null, "master switch split into ALT and BAT")
		if sw_alt and sw_bat:
			# the rockers tilt when toggled: aim at their current centre each time
			var hit_alt: bool = ctl.click(cam, _side_px(cam, sw_alt, 1.0), MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, _side_px(cam, sw_alt, 1.0), MOUSE_BUTTON_LEFT, false, false)
			var hit_bat: bool = ctl.click(cam, _side_px(cam, sw_bat, -1.0), MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, _side_px(cam, sw_bat, -1.0), MOUSE_BUTTON_LEFT, false, false)
			await get_tree().create_timer(0.3).timeout
			_check(hit_alt and hit_bat and root._fg_bridge.dc_volts <= 0.0, "ALT and BAT off: no electrical power (%.0f V)" % root._fg_bridge.dc_volts)
			ctl.click(cam, _side_px(cam, sw_alt, 1.0), MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, _side_px(cam, sw_alt, 1.0), MOUSE_BUTTON_LEFT, false, false)
			ctl.click(cam, _side_px(cam, sw_bat, -1.0), MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, _side_px(cam, sw_bat, -1.0), MOUSE_BUTTON_LEFT, false, false)
			await get_tree().create_timer(0.3).timeout
			_check(root._fg_bridge.dc_volts > 0.0, "and back on (%.0f V)" % root._fg_bridge.dc_volts)
		if sw_b and sw_l:
			var L2 = root._aircraft_lights
			var bpos: Vector2 = cam.unproject_position(sw_b.global_transform * (sw_b as MeshInstance3D).get_aabb().get_center())
			_check(ctl.click(cam, bpos, MOUSE_BUTTON_LEFT, true, false), "beacon switch clickable (hover '%s')" % ctl.hover_name(cam, bpos))
			ctl.click(cam, bpos, MOUSE_BUTTON_LEFT, false, false)
			_check(L2 != null and not L2.beacon_enabled, "beacon switch turns the beacon off")
			ctl.click(cam, bpos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, bpos, MOUSE_BUTTON_LEFT, false, false)
			_check(L2 != null and L2.beacon_enabled, "and on again")
			var lpos: Vector2 = cam.unproject_position(sw_l.global_transform * (sw_l as MeshInstance3D).get_aabb().get_center())
			var was_on: bool = L2.landing_lights_on()
			ctl.click(cam, lpos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, lpos, MOUSE_BUTTON_LEFT, false, false)
			_check(L2.landing_lights_on() != was_on, "landing light switch toggles the landing lights")
			ctl.click(cam, lpos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, lpos, MOUSE_BUTTON_LEFT, false, false)
		root._camera_rig.set_mode(1)
		root.camera_mode = 1
		root.camera2.current = true
	# spinner cone: its own part on the axis, spinning with the blades, visible at every rpm
	var spinner: Node3D = mroot.find_child("Spinner_Pivot", true, false)
	_check(spinner != null and spinner.visible, "spinner pivot present and visible")
	if spinner:
		var sb: Basis = spinner.transform.basis
		await get_tree().create_timer(0.5).timeout
		_check(not spinner.transform.basis.is_equal_approx(sb), "spinner spins with the propeller")
	if _fails.is_empty():
		print("smoke_c172_panel: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_c172_panel: %d failure(s)" % _fails.size())
		get_tree().quit(1)
