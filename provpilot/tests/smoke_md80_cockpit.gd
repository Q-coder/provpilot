extends Node
## MD-82 cockpit (docs/MD80_FLIGHTGEAR_PORT.md): the FlightGear model and its
## animations load, the FDM-mirrored instruments move, the cockpit's Nasal
## bindings act through the shim table (switch click sound pulse, battery
## switch into the FDM, NAV tuning into the receiver), the sound set loads.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_cockpit.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

## A bridged property as float (NAN when the model never references it).
func _fp(fga: FGAnimator, key: String) -> float:
	var v = fga.get_property(key)
	return float(v) if v != null and typeof(v) != TYPE_STRING else NAN

## The clickable part whose bindings mention [param needle] (JSON text search).
func _part_for(ctl, needle: String) -> Node3D:
	for it in ctl.get_items():
		if JSON.stringify(it["recs"]).find(needle) >= 0:
			return it["node"]
	return null

## Press and release [param button] on the centre of [param n]; true when a control took the press.
func _press(ctl, cam: Camera3D, n: Node3D, button: int) -> bool:
	var px: Vector2 = cam.unproject_position(n.global_transform * (n as MeshInstance3D).get_aabb().get_center())
	var took: bool = ctl.click(cam, px, button, true, false)
	ctl.click(cam, px, button, false, false)
	return took

## The clickable mesh called [param name].
func _mesh_named(ctl, name: String) -> Node3D:
	for it in ctl.get_items():
		if (it["node"] as Node).name == name:
			return it["node"]
	return null

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var fga: FGAnimator = root._fg_animator
	var jsb = root.jsb_node
	var ctl = root._fg_controls
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	_check(fga != null and fga.animation_count() > 900, "model animations loaded (%d)" % (fga.animation_count() if fga else 0))
	_check(ctl.count > 250, "cockpit controls clickable (%d parts)" % ctl.count)
	_check(ctl.get_shim_count() > 20, "Nasal shim table loaded (%d)" % ctl.get_shim_count())
	_check(fga.unknown_properties().size() == 0, "every model property bridged or mirrored (%d unbridged)" % fga.unknown_properties().size())
	for nm in ["asi1__Needle_kts", "altimeter1__AltNeedle", "hsi1__HSI_card", "eprL__Needle", "fgcp__FGCP", "overhead__PanelOverhead", "MD-80-cockpit__YokeL"]:
		_check(mroot.find_child(nm, true, false) != null, "part present: " + nm)
	var hidden: Node3D = mroot.find_child("PFD1__pfd1_screen", true, false)
	_check(hidden != null and not hidden.visible, "canvas screen hidden (PFD1)")
	# FDM-side instruments reach the animator
	var n2: float = _fp(fga, "engines/engine/n2-actual")
	if is_nan(n2):
		n2 = _fp(fga, "instrumentation/n2/indicated-n2")
	print("  [diag] animator sees N2 %s, DC-L %s, EPR %s" % [str(n2), str(_fp(fga, "systems/electrical/bus/dc-l")), str(_fp(fga, "engines/engine/epr-actual"))])
	_check(jsb.get_property("/engines/engine[0]/n2-actual") > 50.0, "engine N2 computed by the FDM systems (%.0f)" % jsb.get_property("/engines/engine[0]/n2-actual"))
	# the init list's "_pulse" holds autoconfig-running for 5 s like FlightGear's acconfig: the
	# AHRS latches can-align and aligns at once, so the ADI and HSI are live, not flagged
	_check(jsb.get_property("/systems/platform[0]/aligned") > 0.5, "AHRS platform aligned after the autoconfig pulse")
	_check(_fp(fga, "instrumentation/ai[0]/att-avail") > 0.5, "captain's ADI attitude available (no flag)")
	_check(_fp(fga, "instrumentation/hsi[0]/heading-avail") > 0.5, "captain's HSI heading available")
	_check(absf(_fp(fga, "engines/engine[2]/n2") - jsb.get_property("propulsion/engine[2]/n2")) < 0.5, "APU engine[2] published to the FlightGear engine interface")
	var dcl: float = _fp(fga, "systems/electrical/bus/dc-l")
	_check(is_nan(dcl) or dcl > 24.0, "DC bus value mirrored to the animator (%s V)" % str(dcl))
	_check(fga.animated_transform("altimeter1__AltNeedle") != Transform3D(), "altimeter needle animated")
	# control surfaces: the -set file aliases route the FDM's surface positions to the
	# model. The MD-80's ailerons and elevators are tab-driven and have no authority
	# without airflow (fcs authority table: 0 below 7 psf), so on the ground only the
	# control tabs and the hydraulic rudder move.
	var tab0: Transform3D = fga.animated_transform("AilTabControlL")
	var rud0: Transform3D = fga.animated_transform("Rudder")
	var nose0: Transform3D = fga.animated_transform("LGNStrutLow")
	jsb.set_input_aileron(1.0)
	jsb.set_input_rudder(1.0)
	await get_tree().create_timer(1.5).timeout
	var tab_deg: float = _fp(fga, "rotors/main/blade[2]/position-deg")
	var rud_deg: float = _fp(fga, "rotors/tail/blade/position-deg")
	print("  [diag] aileron tab alias %s deg (FDM %s), rudder alias %s deg (FDM %s), aileron surface %s deg at %s psf" % [str(tab_deg), str(jsb.get_property("/systems/fcs/aileron-l/control-tab-final-deg")), str(rud_deg), str(jsb.get_property("/systems/fcs/rudder/final-deg")), str(_fp(fga, "rotors/main/blade/flap-deg")), str(jsb.get_property("aero/qbar-psf"))])
	_check(not is_nan(tab_deg) and absf(tab_deg) > 5.0, "aileron control tab reaches the model through the alias (%.1f deg)" % tab_deg)
	_check(not is_nan(rud_deg) and absf(rud_deg) > 2.0, "rudder deflection reaches the model through the alias (%.1f deg)" % rud_deg)
	_check(not fga.animated_transform("AilTabControlL").basis.is_equal_approx(tab0.basis), "aileron tab mesh moves with the stick")
	_check(not fga.animated_transform("Rudder").basis.is_equal_approx(rud0.basis), "rudder mesh moves with the pedals")
	# nose wheel: the model rotates the strut by sim/multiplay/generic/float[5], aliased to
	# FlightGear's gear/gear[0]/steering-norm (steer-pos-deg over the FDE's 82 deg max_steer).
	# The tiller system fades the steering out below 1 kt, so at rest only the plumbing is
	# checked: both properties published, strut untouched.
	var steer_norm: float = _fp(fga, "gear/gear/steering-norm")
	var steer_alias: float = _fp(fga, "sim/multiplay/generic/float[5]")
	print("  [diag] steer-pos-deg %s, steering-norm %s, alias %s" % [str(jsb.get_property("fcs/steer-pos-deg[0]")), str(steer_norm), str(steer_alias)])
	_check(not is_nan(steer_norm) and not is_nan(steer_alias) and is_equal_approx(steer_norm, steer_alias) and (absf(jsb.get_property("fcs/steer-pos-deg[0]")) < 0.5 or absf(steer_alias) > 0.02), "nose steering fraction published and aliased to the model")
	var strut_moved: bool = not fga.animated_transform("LGNStrutLow").basis.is_equal_approx(nose0.basis)
	_check(strut_moved == (absf(jsb.get_property("fcs/steer-pos-deg[0]")) > 0.5), "nose gear strut follows the steering (moved %s)" % str(strut_moved))
	jsb.set_input_aileron(0.0)
	jsb.set_input_rudder(0.0)
	# tyre spin alias: the main gear tyres read sim/multiplay/generic/float[1] = gear rollspeed
	_check(fga.get_property("sim/multiplay/generic/float[1]") != null, "tyre spin alias published")
	# the minimums callouts compare against FDM-side values on both sides of the test:
	# they must be pulled (an unpulled side reads 0 and "minimums" repeats on the ground)
	_check(absf(_fp(fga, "systems/misc/minimums-minus-50") - 150.0) < 0.5, "minimums-minus-50 pulled from the FDM (%.0f)" % _fp(fga, "systems/misc/minimums-minus-50"))
	_check(absf(_fp(fga, "controls/switches/minimums") - 200.0) < 0.5, "decision height 200 ft")
	# analog instruments (EFIS option off): one altimeter per side, the EFIS one hidden
	var alt_vis := 0
	for n in root._ac.find_children("Sub_altimeter1*", "Node3D", true, false):
		print("  [diag] %s visible %s (in tree %s)" % [n.name, str(n.visible), str(n.is_visible_in_tree())])
		if n.is_visible_in_tree():
			alt_vis += 1
	_check(alt_vis == 1, "exactly one captain altimeter visible (%d)" % alt_vis)
	_check(_fp(fga, "systems/acconfig/options/efis") == 0.0, "EFIS option off (analog panel)")
	# centre panel: the EDP/SDP displays (a sub-model animated by its <model> name) hidden, round gauges shown
	var edp: Node3D = root._ac.find_child("Sub_EDP-SDP", true, false)
	var n1l: Node3D = root._ac.find_child("Sub_n1L", true, false)
	_check(edp != null and not edp.is_visible_in_tree(), "EDP/SDP displays hidden")
	_check(n1l != null and n1l.is_visible_in_tree(), "round N1 gauge shown")
	# callout gate: the predicted AGL two seconds ahead exists and is not below the current one on the ground
	_check(_fp(fga, "position/gear-agl-ft-2-sec") >= _fp(fga, "position/gear-agl-ft") - 0.5, "GPWS predictor: no descent on the ground (%.1f vs %.1f ft)" % [_fp(fga, "position/gear-agl-ft-2-sec"), _fp(fga, "position/gear-agl-ft")])
	# sounds
	var snd = root._aircraft.get_fg_sounds()
	_check(snd != null and snd.get_count() > 60, "FlightGear sound set loaded (%d)" % (snd.get_count() if snd else 0))
	# cockpit interaction through the shims
	var cam: Camera3D = root.camera1
	cam.current = true
	await get_tree().create_timer(0.5).timeout
	var bat: Node3D = _part_for(ctl, "controls/electrical/battery")
	_check(bat != null, "battery switch found (%s)" % (bat.name if bat else "-"))
	if bat:
		print("  [diag] battery: animator %s, FDM %s, in control state %s" % [str(fga.get_property("controls/electrical/battery")), str(jsb.get_property("/controls/electrical/battery")), str(ctl.get_state().has("controls/electrical/battery"))])
		for it in ctl.get_items():
			if it["node"] == bat:
				print("  [diag] battery recs: " + JSON.stringify(it["recs"]).left(600))
		var before: float = float(ctl.value_of("controls/electrical/battery"))
		var px: Vector2 = cam.unproject_position(bat.global_transform * (bat as MeshInstance3D).get_aabb().get_center())
		var took: bool = ctl.click(cam, px, MOUSE_BUTTON_LEFT, true, false)
		ctl.click(cam, px, MOUSE_BUTTON_LEFT, false, false)
		var pulse: float = float(ctl.value_of("sim/sound/switch1"))
		var after: float = float(ctl.value_of("controls/electrical/battery"))
		print("  [diag] battery %s -> %s, switch1 pulse %s" % [str(before), str(after), str(pulse)])
		_check(took and after != before, "battery switch toggles (%s -> %s)" % [str(before), str(after)])
		_check(pulse == 1.0, "switch click pulses sim/sound/switch1 through the shim")
		await get_tree().create_timer(0.6).timeout
		_check(float(ctl.value_of("sim/sound/switch1")) == 0.0, "pulse ends after 0.2 s")
		_check(absf(jsb.get_property("/controls/electrical/battery") - after) < 0.01, "switch state mirrored into the FDM")
		ctl.click(cam, px, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, px, MOUSE_BUTTON_LEFT, false, false)
	var navk: Node3D = _part_for(ctl, "instruments.NAV.adjustInt(0, 1)")
	_check(navk != null, "NAV 1 MHz knob found")
	if navk:
		var f0: float = float(ctl.value_of("instrumentation/nav/frequencies/selected-mhz"))
		var px2: Vector2 = cam.unproject_position(navk.global_transform * (navk as MeshInstance3D).get_aabb().get_center())
		ctl.click(cam, px2, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, px2, MOUSE_BUTTON_LEFT, false, false)
		var f1: float = float(ctl.value_of("instrumentation/nav/frequencies/selected-mhz"))
		print("  [diag] NAV1 %.2f -> %.2f" % [f0, f1])
		# the MHz and kHz knobs are concentric: whichever the ray hits first steps the frequency
		var d: float = absf(f1 - f0)
		_check((d > 0.9 and d < 1.1) or (d > 0.04 and d < 0.06), "NAV 1 knob steps the frequency (%.2f)" % d)
		await get_tree().create_timer(0.3).timeout
		var rn: RadioNav = root._aircraft.get_fg_bridge().get_radio_nav()
		_check(absf(rn.nav_frequency_mhz - f1) < 0.001, "receiver follows the MD-80 pedestal radio (%.2f)" % rn.nav_frequency_mhz)
		ctl.click(cam, px2, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, px2, MOUSE_BUTTON_RIGHT, false, false)
	# FlightGear's invisible sense surfaces (visible=false) are not drawn but still take the ray
	var gen_dn: Node3D = _mesh_named(ctl, "overhead__EngGenL_dn")
	var gen_lever: Node3D = _mesh_named(ctl, "overhead__EngGenL")
	_check(gen_dn != null and (gen_dn as MeshInstance3D).layers == 0, "L GEN lower sense surface left the camera's layers")
	_check(gen_lever == null or (gen_lever as MeshInstance3D).layers != 0, "L GEN lever itself still drawn")
	# house rule: left / wheel down = down, right / wheel up = up on a two-way switch
	var emer: Node3D = _part_for(ctl, "controls/lighting/emer-lt")
	_check(emer != null, "EMER LTS switch found")
	if emer:
		var st: Dictionary = ctl.get_state()
		st["controls/lighting/emer-lt"] = 0.5
		ctl.set_state(st)
		_press(ctl, cam, emer, MOUSE_BUTTON_RIGHT)
		_check(float(ctl.value_of("controls/lighting/emer-lt")) == 1.0, "right click moves the EMER LTS switch up (ARM -> ON)")
		_press(ctl, cam, emer, MOUSE_BUTTON_LEFT)
		_press(ctl, cam, emer, MOUSE_BUTTON_LEFT)
		_check(float(ctl.value_of("controls/lighting/emer-lt")) == 0.0, "two left clicks move it down to OFF")
		_press(ctl, cam, emer, MOUSE_BUTTON_WHEEL_UP)
		_check(float(ctl.value_of("controls/lighting/emer-lt")) == 0.5, "wheel up moves it up (OFF -> ARM)")
		_press(ctl, cam, emer, MOUSE_BUTTON_WHEEL_DOWN)
		_check(float(ctl.value_of("controls/lighting/emer-lt")) == 0.0, "wheel down moves it down")
	# a knob: right / wheel up clockwise (increase), left / wheel down counter-clockwise
	var epk: Node3D = _mesh_named(ctl, "overhead__EmerPwrKnob")
	_check(epk != null, "EMER PWR knob found")
	if epk:
		var st2: Dictionary = ctl.get_state()
		st2["controls/electrical/emer-pwr"] = 0.0
		ctl.set_state(st2)
		_press(ctl, cam, epk, MOUSE_BUTTON_RIGHT)
		_check(float(ctl.value_of("controls/electrical/emer-pwr")) == 1.0, "right click turns the EMER PWR knob clockwise (OFF -> ON)")
		_press(ctl, cam, epk, MOUSE_BUTTON_LEFT)
		_check(float(ctl.value_of("controls/electrical/emer-pwr")) == 0.0, "left click turns it back")
		_press(ctl, cam, epk, MOUSE_BUTTON_WHEEL_UP)
		_check(float(ctl.value_of("controls/electrical/emer-pwr")) == 1.0, "wheel up turns it clockwise")
		_press(ctl, cam, epk, MOUSE_BUTTON_WHEEL_DOWN)
		_check(float(ctl.value_of("controls/electrical/emer-pwr")) == 0.0, "wheel down turns it back")
	# a three-position switch stops at its ends: no jump from ON back to RETRACT (property-cycle no longer wraps)
	var ldg: Node3D = _mesh_named(ctl, "fgcp__SwLdgLtsL")
	_check(ldg != null, "left landing light switch found")
	if ldg:
		var st4: Dictionary = ctl.get_state()
		st4["controls/lighting/landing-light-l"] = 0.5
		ctl.set_state(st4)
		_press(ctl, cam, ldg, MOUSE_BUTTON_RIGHT)
		_press(ctl, cam, ldg, MOUSE_BUTTON_RIGHT)
		_check(float(ctl.value_of("controls/lighting/landing-light-l")) == 1.0, "landing light switch stays at its upper end on a further right click")
		_press(ctl, cam, ldg, MOUSE_BUTTON_WHEEL_UP)
		_check(float(ctl.value_of("controls/lighting/landing-light-l")) == 1.0, "and on a further wheel up")
		for i in range(3):
			_press(ctl, cam, ldg, MOUSE_BUTTON_WHEEL_DOWN)
		_check(float(ctl.value_of("controls/lighting/landing-light-l")) == 0.0, "three wheel downs: down to the lower end and no further")
		_press(ctl, cam, ldg, MOUSE_BUTTON_LEFT)
		_check(float(ctl.value_of("controls/lighting/landing-light-l")) == 0.0, "left click at the lower end stays")
	# the lower half of the APU master is a one-way pick over two of its three values: no wrap either
	var apu_dn: Node3D = _mesh_named(ctl, "overhead__ApuMaster_dn")
	if apu_dn:
		var st5: Dictionary = ctl.get_state()
		st5["controls/apu/master"] = 0.0
		ctl.set_state(st5)
		_press(ctl, cam, apu_dn, MOUSE_BUTTON_LEFT)
		_check(float(ctl.value_of("controls/apu/master")) == 0.0, "APU master lower half at OFF stays OFF")
	# APU master: the upper half held with the right button is START and springs back to RUN
	var apu_up: Node3D = _mesh_named(ctl, "overhead__ApuMaster_up")
	_check(apu_up != null and (apu_up as MeshInstance3D).layers == 0, "APU START sense surface found and not drawn")
	if apu_up:
		var st3: Dictionary = ctl.get_state()
		var master0: float = float(st3.get("controls/apu/master", 0.0))
		st3["controls/apu/master"] = 1.0
		ctl.set_state(st3)
		var px3: Vector2 = cam.unproject_position(apu_up.global_transform * (apu_up as MeshInstance3D).get_aabb().get_center())
		ctl.click(cam, px3, MOUSE_BUTTON_RIGHT, true, false)
		_check(float(ctl.value_of("controls/apu/master")) == 2.0, "right button held on the upper half: APU master START")
		ctl.click(cam, px3, MOUSE_BUTTON_RIGHT, false, false)
		_check(float(ctl.value_of("controls/apu/master")) == 1.0, "released: springs back to RUN")
		_press(ctl, cam, apu_up, MOUSE_BUTTON_LEFT)
		_check(float(ctl.value_of("controls/apu/master")) == 0.0, "left click on the upper half moves it down to OFF")
		st3 = ctl.get_state()
		st3["controls/apu/master"] = master0
		ctl.set_state(st3)
	# FlightGear <text> elements: the FGCP's NAV frequency and course windows are Label3Ds on the builder's empties
	var freq_anchor: Node = mroot.find_child("fgcp__Digits_Freq1", true, false)
	var crs_anchor: Node = mroot.find_child("fgcp__Digits_Crs1", true, false)
	var st_an: Dictionary = fga.stats()
	_check(int(st_an.get("texts", 0)) >= 60, "FlightGear text elements built as labels (%d)" % int(st_an.get("texts", 0)))
	var freq_lb: Label3D = (freq_anchor.get_node_or_null("Text") as Label3D) if freq_anchor else null
	var crs_lb: Label3D = (crs_anchor.get_node_or_null("Text") as Label3D) if crs_anchor else null
	_check(freq_lb != null and crs_lb != null, "NAV 1 frequency and course windows have labels")
	if freq_lb and crs_lb:
		await get_tree().create_timer(0.3).timeout
		var mhz: float = float(ctl.value_of("instrumentation/nav/frequencies/selected-mhz"))
		print("  [diag] FGCP NAV1 window '%s' for %.2f MHz, CRS window '%s'; fgcp power %s, fgcp-digital %s, visible %s / anchor %s" % [freq_lb.text, mhz, crs_lb.text, str(fga.get_property("systems/electrical/outputs/fgcp")), str(fga.get_property("systems/lighting/fgcp-digital")), str(freq_lb.is_visible_in_tree()), str((freq_anchor as Node3D).visible)])
		_check(freq_lb.text == "%6.2f" % mhz, "NAV 1 window shows the tuned frequency (%s)" % freq_lb.text)
		_check(freq_lb.is_visible_in_tree(), "NAV 1 window is lit (powered FGCP, digital lighting on)")
		# NAV 2 has not been touched: the window must show the init list's 109.55
		# from the first frame (the bridge's hard default of 0.00 showed until
		# a knob click seeded the state)
		var f2_anchor: Node = mroot.find_child("fgcp__Digits_Freq2", true, false)
		var f2_lb: Label3D = (f2_anchor.get_node_or_null("Text") as Label3D) if f2_anchor else null
		var mhz2: float = float(ctl.value_of("instrumentation/nav[1]/frequencies/selected-mhz"))
		print("  [diag] FGCP NAV2 window '%s', control state %.2f, receiver %.2f" % [f2_lb.text if f2_lb else "-", mhz2, root._aircraft.get_fg_bridge().get_radio_nav2().nav_frequency_mhz])
		_check(absf(root._aircraft.get_fg_bridge().get_radio_nav2().nav_frequency_mhz - 109.55) < 0.001, "NAV 2 receiver starts on the init list's 109.55 without a knob touch")
		_check(f2_lb != null and f2_lb.text == "109.55", "NAV 2 window shows 109.55 from the start (%s)" % (f2_lb.text if f2_lb else "-"))
		var crsk: Node3D = _mesh_named(ctl, "fgcp__Nav1CrsKnob")
		if crsk:
			var c0: String = crs_lb.text
			_press(ctl, cam, crsk, MOUSE_BUTTON_RIGHT)
			await get_tree().create_timer(0.2).timeout
			var crs: float = float(ctl.value_of("instrumentation/nav/radials/selected-deg"))
			_check(crs_lb.text == "%03d" % int(round(crs)) and crs_lb.text != c0, "CRS window follows the course knob (%s -> %s)" % [c0, crs_lb.text])
	# fuel quantity indicator: the per-tank readouts are text elements lit by the pedestal digital lighting
	var fl_anchor: Node = mroot.find_child("fuel__left-lbs", true, false)
	var fl_lb: Label3D = (fl_anchor.get_node_or_null("Text") as Label3D) if fl_anchor else null
	_check(fl_lb != null and fl_lb.is_visible_in_tree() and fl_lb.text.strip_edges() != "" and float(fl_lb.text) > 100.0, "fuel quantity indicator shows the left main tank (%s lb)" % (fl_lb.text if fl_lb else "-"))
	# FGCP ALT knob (2026-11-03, gery: "I cannot set the altitude on the fgcp alt"): the
	# wheel binding is a Nasal if / else on the knob's push state (thousands, hundreds
	# while pushed) with a Shift variant; both must reach the autoflight's altAdjust
	var af = root._aircraft.get_autoflight()
	var altk: Node3D = _part_for(ctl, "ApPanel.altAdjust")
	_check(altk != null, "FGCP ALT knob clickable")
	if altk and af:
		var a0: float = float(af.get_value("it-autoflight/input/alt"))
		_press(ctl, cam, altk, MOUSE_BUTTON_WHEEL_UP)
		await get_tree().create_timer(0.2).timeout
		var a1: float = float(af.get_value("it-autoflight/input/alt"))
		_check(absf(a1 - (a0 + 1000.0)) < 0.5, "ALT knob wheel up: +1000 ft (%.0f -> %.0f)" % [a0, a1])
		var px_a: Vector2 = cam.unproject_position(altk.global_transform * (altk as MeshInstance3D).get_aabb().get_center())
		ctl.click(cam, px_a, MOUSE_BUTTON_WHEEL_UP, true, true)
		ctl.click(cam, px_a, MOUSE_BUTTON_WHEEL_UP, false, true)
		await get_tree().create_timer(0.2).timeout
		var a2: float = float(af.get_value("it-autoflight/input/alt"))
		_check(absf(a2 - (a1 + 100.0)) < 0.5, "Shift + wheel up: +100 ft (%.0f -> %.0f)" % [a1, a2])
		_press(ctl, cam, altk, MOUSE_BUTTON_WHEEL_DOWN)
		await get_tree().create_timer(0.2).timeout
		_check(absf(float(af.get_value("it-autoflight/input/alt")) - (a2 - 1000.0)) < 0.5, "ALT knob wheel down: -1000 ft")
	# TOGA buttons on the thrust levers (2026-11-03, gery: "the take-off mode button on the
	# throttle levers do not work"): on the ground with flaps >= 5 the DFGS goes to T/O; the
	# handler reads the flap angle as fdm/jsbsim/fcs/flap-pos-deg, which the mirror pull
	# must map to the JSBSim property
	var toga: Node3D = _part_for(ctl, "ApPanel.toga")
	_check(toga != null, "TOGA button clickable")
	if toga and af:
		jsb.set_flaps(0.36)
		await get_tree().create_timer(6.0).timeout
		var took_t: bool = _press(ctl, cam, toga, MOUSE_BUTTON_LEFT)
		await get_tree().create_timer(0.5).timeout
		print("  [diag] TOGA: took %s, flaps %.1f deg, lat %d vert %d text %s, FMA %s/%s" % [str(took_t), jsb.get_property("fcs/flap-pos-deg"), af.get_lat_mode(), af.get_vert_mode(), str(af.get_value("it-autoflight/text/lat")), str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b"))])
		_check(took_t and af.get_lat_mode() == 5 and af.get_vert_mode() == 7 and str(af.get_value("it-autoflight/text/lat")) == "T/O", "TOGA on the ground with flaps 11: DFGS T/O mode (FMA TAK OFF)")
	if _fails.is_empty():
		print("smoke_md80_cockpit: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_md80_cockpit: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
