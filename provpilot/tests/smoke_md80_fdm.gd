extends Node
## MD-82 flight model (docs/MD80_FLIGHTGEAR_PORT.md): the JSBSim MD-80 with
## its systems loads, starts with engines running from the init properties,
## the FDM-side electrical / hydraulic / pneumatic systems come alive, and
## the thrust follows the throttle.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_fdm.tscn
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
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	_check(jsb != null and jsb.is_initialized(), "JSBSim MD-82 initialised")
	await get_tree().create_timer(6.0).timeout
	var n1 := [jsb.get_property("propulsion/engine[0]/n1"), jsb.get_property("propulsion/engine[1]/n1")]
	var n2 := [jsb.get_property("propulsion/engine[0]/n2"), jsb.get_property("propulsion/engine[1]/n2")]
	print("  [diag] N1 %s N2 %s APU n2 %.1f" % [str(n1), str(n2), jsb.get_property("propulsion/engine[2]/n2")])
	_check(n2[0] > 50.0 and n2[1] > 50.0, "both JT8D engines running (N2 %.0f / %.0f)" % [n2[0], n2[1]])
	for p in ["/systems/ignition/cutoff-1", "/systems/electrical/bus/dc-l", "/systems/electrical/bus/ac-l", "/systems/hydraulics/sys-l-psi",
			"/systems/hydraulics/sys-r-psi", "/systems/pneumatics/bleed-l-psi", "/engines/engine[0]/n1-actual", "/engines/engine[0]/epr-actual",
			"/instrumentation/ai[0]/pitch-deg", "/systems/fcs/flaps/input", "/systems/gear/all-norm", "/controls/gear/lever", "/systems/iru[0]/main-avail", "/systems/electrical/bus/dc-bat", "/systems/electrical/sources/idg-l/output-volt", "/systems/electrical/sources/bat-1/volt", "/systems/electrical/bus/emer-ac"]:
		print("  [diag] %-40s %s (has %s)" % [p, str(jsb.get_property(p)), str(jsb.has_property(p))])
	_check(jsb.has_property("/systems/electrical/bus/dc-l") and jsb.get_property("/systems/electrical/bus/dc-l") > 24.0, "left DC bus powered (%.1f V)" % jsb.get_property("/systems/electrical/bus/dc-l"))
	_check(jsb.get_property("/systems/electrical/bus/ac-l") > 100.0, "left AC bus powered by the generator (%.0f V)" % jsb.get_property("/systems/electrical/bus/ac-l"))
	_check(jsb.get_property("/systems/hydraulics/sys-l-psi") > 2500.0 and jsb.get_property("/systems/hydraulics/sys-r-psi") > 2500.0, "both hydraulic systems pressurised (%.0f / %.0f psi)" % [jsb.get_property("/systems/hydraulics/sys-l-psi"), jsb.get_property("/systems/hydraulics/sys-r-psi")])
	_check(jsb.get_property("/systems/pneumatics/bleed-l-psi") > 15.0, "left bleed air (%.0f psi)" % jsb.get_property("/systems/pneumatics/bleed-l-psi"))
	_check(jsb.get_property("/systems/gear/all-norm") > 0.95, "gear down (%.2f)" % jsb.get_property("/systems/gear/all-norm"))
	# the bridge mirror: FlightGear-side inputs land in the FDM, FDM outputs reach the animator dictionary
	var br: FGBridge = root._aircraft.get_fg_bridge()
	_check(br.get_fdm_mirror() and br.get_mirror_pushed() > 20, "property mirror pushes inputs into the FDM (%d)" % br.get_mirror_pushed())
	print("  [diag] mirror pulled %d" % br.get_mirror_pulled())
	_check(absf(jsb.get_property("/instrumentation/airspeed-indicator/indicated-speed-kt") - jsb.get_property("velocities/vc-kts")) < 2.0, "indicated airspeed mirrored into the FDM")
	_check(absf(jsb.get_property("/orientation/pitch-deg") - jsb.get_pitch_deg()) < 0.5, "orientation mirrored into the FDM")
	# throttle up: thrust rises
	var t0: float = jsb.get_property("propulsion/engine[0]/thrust-lbs")
	jsb.set_input_throttle(0.9)
	await get_tree().create_timer(8.0).timeout
	var t1: float = jsb.get_property("propulsion/engine[0]/thrust-lbs")
	print("  [diag] thrust idle %.0f lbf -> 90%% throttle %.0f lbf, N1 %.1f, EPR %.2f" % [t0, t1, jsb.get_property("propulsion/engine[0]/n1"), jsb.get_property("/engines/engine[0]/epr-actual")])
	_check(t1 > t0 + 5000.0, "thrust follows the throttle")
	jsb.set_input_throttle(0.0)
	# wheel brakes: the MD-80's flight controls compute fcs/*-brake-cmd-norm from the FlightGear pedal
	# properties (with parking brake and autobrake); PVP's pedal must arrive there
	root.jsb_node.set_input_brake(0.0)
	await get_tree().create_timer(1.5).timeout
	var b_off: float = jsb.get_property("fcs/left-brake-cmd-norm")
	print("  [diag] before pedal: parking %s, left-input %s, actuator %s, rate %s, hyd brakes %s, cmd %.2f" % [str(jsb.get_property("/controls/gear/brake-parking")), str(jsb.get_property("/systems/fcs/brakes/left-input")), str(jsb.get_property("/systems/fcs/brakes/left-actuator")), str(jsb.get_property("/systems/fcs/brakes/rate")), str(jsb.get_property("/systems/hydraulics/brakes/sys-l-psi")), b_off])
	root.jsb_node.set_input_brake(1.0)
	await get_tree().create_timer(1.5).timeout
	var b_on: float = jsb.get_property("fcs/left-brake-cmd-norm")
	print("  [diag] brakes: pedal property %s, command %.2f -> %.2f (parking %s)" % [str(jsb.get_property("/controls/gear/brake-left")), b_off, b_on, str(jsb.get_property("/controls/gear/brake-parking"))])
	for p in ["/systems/fcs/brakes/left-input", "/systems/fcs/brakes/left-actuator", "/systems/fcs/brakes/rate", "/systems/hydraulics/brakes/left-actuator-feedback", "/systems/acconfig/autoconfig-running", "/services/chocks/enable", "fcs/parking-brake-cmd-norm", "gear/unit[1]/WOW"]:
		print("  [diag] %-48s %s" % [p, str(jsb.get_property(p))])
	_check(b_on > 0.9 and absf(jsb.get_property("/controls/gear/brake-left") - 1.0) < 0.01, "pedal brakes reach the MD-80's brake command (%.2f)" % b_on)
	root.jsb_node.set_input_brake(0.0)
	await get_tree().create_timer(1.5).timeout
	print("  [diag] parking only: command %.2f, left-input %s, actuator %s, parking %s" % [jsb.get_property("fcs/left-brake-cmd-norm"), str(jsb.get_property("/systems/fcs/brakes/left-input")), str(jsb.get_property("/systems/fcs/brakes/left-actuator")), str(jsb.get_property("/controls/gear/brake-parking"))])
	# flap lever detents: each cfg position through the MD-80's lever table
	var cc = root._aircraft.get_cockpit_controls() if root._aircraft.has_method("get_cockpit_controls") else null
	for pos in [0.0, 0.14, 0.25, 0.36, 0.47, 0.58, 0.69, 0.8, 0.91]:
		root.jsb_node.set_flaps(pos)
		await get_tree().create_timer(2.0).timeout
		print("  [diag] flaps lever %.2f: FG flaps %s fixed %s input %s cmd %s deg, pos %.1f deg, daf %s" % [pos, str(jsb.get_property("/controls/flight/flaps")), str(jsb.get_property("/systems/fcs/flaps/fixed")), str(jsb.get_property("/systems/fcs/flaps/input")), str(jsb.get_property("/systems/fcs/flaps/cmd")), jsb.get_property("fcs/flap-pos-deg"), str(jsb.get_property("/controls/flight/dial-a-flap"))])
	root.jsb_node.set_flaps(0.36)
	await get_tree().create_timer(2.0).timeout
	_check(absf(jsb.get_property("/systems/fcs/flaps/cmd") - 11.0) < 0.1, "lever detent 0.36 commands flaps 11 (%.0f)" % jsb.get_property("/systems/fcs/flaps/cmd"))
	root.jsb_node.set_flaps(0.58)
	await get_tree().create_timer(2.0).timeout
	_check(absf(jsb.get_property("/systems/fcs/flaps/cmd") - 15.0) < 0.1, "lever detent 0.58 commands flaps 15 (%.0f)" % jsb.get_property("/systems/fcs/flaps/cmd"))
	# dial-a-flap: the thumbwheel value and the 0.25 detent
	var st_d: Dictionary = root._aircraft.get_fg_controls().get_state()
	st_d["controls/flight/dial-a-flap"] = 5.0
	root._aircraft.get_fg_controls().set_state(st_d)
	root.jsb_node.set_flaps(0.25)
	await get_tree().create_timer(2.5).timeout
	_check(absf(jsb.get_property("/systems/fcs/flaps/cmd") - 5.0) < 0.1, "dial-a-flap 5 at the 0.25 detent commands flaps 5 (%.0f)" % jsb.get_property("/systems/fcs/flaps/cmd"))
	root.jsb_node.set_flaps(0.0)
	# the keys / joystick buttons step the cfg detents (JSBGodot::increase_flaps): UP/RET, UP/MID, DIAL, 11, 15, 28, 40
	jsb.set_flaps(0.0)
	var seq: Array = []
	var cmds: Array = []
	for i in range(6):
		jsb.increase_flaps()
		await get_tree().create_timer(1.0).timeout
		seq.append(snappedf(jsb.get_flap_cmd(), 0.01))
		cmds.append(roundi(jsb.get_property("/systems/fcs/flaps/cmd")))
	print("  [diag] flap steps: lever %s -> cmd %s" % [str(seq), str(cmds)])
	_check(seq == [0.14, 0.25, 0.36, 0.58, 0.8, 0.91], "flap steps walk the MD-80 detents (%s)" % str(seq))
	_check(cmds == [0, 5, 11, 15, 28, 40], "detents command slats, dial 5 (dialled above), 11, 15, 28, 40 (%s)" % str(cmds))
	for i in range(6):
		jsb.decrease_flaps()
	_check(absf(jsb.get_flap_cmd()) < 0.01, "six steps back reach UP/RET")
	await get_tree().create_timer(1.0).timeout
	# thumbwheel stowed (-1): the DIAL detent would be UP/MID again, so every
	# step path skips it (gery 2026-11-02: "to get to 11 I have to click twice")
	st_d = root._aircraft.get_fg_controls().get_state()
	st_d["controls/flight/dial-a-flap"] = -1.0
	root._aircraft.get_fg_controls().set_state(st_d)
	await get_tree().create_timer(1.0).timeout
	_check(not jsb.is_flap_detent_available(2) and jsb.is_flap_detent_available(1), "DIAL detent unavailable while the wheel is stowed (daf %s)" % str(jsb.get_property("/controls/flight/dial-a-flap")))
	jsb.set_flaps(0.0)
	var seq2: Array = []
	for i in range(3):
		jsb.increase_flaps()
		seq2.append(snappedf(jsb.get_flap_cmd(), 0.01))
	_check(seq2 == [0.14, 0.36, 0.58], "stowed wheel: UP/RET -> UP/MID -> 11 -> 15 (%s)" % str(seq2))
	jsb.decrease_flaps()
	jsb.decrease_flaps()
	_check(absf(jsb.get_flap_cmd() - 0.14) < 0.01, "two steps up from 15 reach UP/MID (%.2f)" % jsb.get_flap_cmd())
	jsb.decrease_flaps()
	_check(absf(jsb.get_flap_cmd()) < 0.01, "one more retracts the slats")
	if cc != null:
		cc.set_flap_position(1, jsb)
		cc.step_flaps(1, jsb)
		_check(cc.get_flap_position() == 3, "the cockpit lever click from UP/MID lands on 11 (notch %d)" % cc.get_flap_position())
		cc.step_flaps(-1, jsb)
		_check(cc.get_flap_position() == 1, "and back on UP/MID (notch %d)" % cc.get_flap_position())
		cc.set_flap_position(0, jsb)
	jsb.set_flaps(0.0)
	# NAV 2: its own receiver (instrumentation/nav[1]) for the first officer's HSI
	var st_n: Dictionary = root._aircraft.get_fg_controls().get_state()
	st_n["instrumentation/nav[1]/frequencies/selected-mhz"] = 114.85   # KLO, in range at LSZH
	st_n["instrumentation/nav/frequencies/selected-mhz"] = 109.55       # NAV 1 off station
	root._aircraft.get_fg_controls().set_state(st_n)
	await get_tree().create_timer(3.0).timeout
	var rn2: RadioNav = br.get_radio_nav2()
	print("  [diag] NAV1 %.2f in-range %s, NAV2 %.2f in-range %s ident %s, FDM nav[1]/in-range %s" % [br.get_radio_nav().nav_frequency_mhz, str(br.get_radio_nav().nav_in_range()), rn2.nav_frequency_mhz, str(rn2.nav_in_range()), rn2.nav_ident(), str(jsb.get_property("/instrumentation/nav[1]/in-range"))])
	_check(rn2.receiver_index == 1 and rn2.nav_in_range() and rn2.nav_ident() == "KLO", "NAV 2 receives KLO on 114.85")
	_check(not br.get_radio_nav().nav_in_range(), "NAV 1 is independent of NAV 2")
	_check(jsb.get_property("/instrumentation/nav[1]/in-range") > 0.5, "NAV 2 in-range reaches the FDM (the FO's HSI)")
	st_n["instrumentation/nav/frequencies/selected-mhz"] = 114.85
	root._aircraft.get_fg_controls().set_state(st_n)
	# landing lights: the cockpit switches light the FDM's exterior-light levels, which drive the fixtures
	var al = root._aircraft.get_aircraft_lights()
	_check(al != null and al.fixture_count_of("landing") == 3 and al.fixture_count_of("flood") == 4, "MD-80 has 3 landing light fixtures and 4 cockpit floods")
	var st_l: Dictionary = root._aircraft.get_fg_controls().get_state()
	st_l["controls/lighting/landing-light-n"] = 1.0
	st_l["controls/lighting/capt-flood"] = 1.0
	root._aircraft.get_fg_controls().set_state(st_l)
	await get_tree().create_timer(3.0).timeout
	print("  [diag] lights: nose level %.2f (FDM %s), left %.2f, capt flood %.2f" % [al.get_fixture_level("landing", 1), str(jsb.get_property("/systems/exterior-lights/nose-landing-light")), al.get_fixture_level("landing", 0), al.get_fixture_level("flood", 0)])
	_check(al.get_fixture_level("landing", 1) > 0.9, "nose landing light lit from the switch (%.2f)" % al.get_fixture_level("landing", 1))
	_check(al.get_fixture_level("flood", 0) > 0.9, "captain's flood follows the knob (%.2f)" % al.get_fixture_level("flood", 0))
	# nose light DIM: the systems give 0.6 at the switch's middle position; the spot's energy follows the level
	st_l = root._aircraft.get_fg_controls().get_state()
	st_l["controls/lighting/landing-light-n"] = 0.5
	root._aircraft.get_fg_controls().set_state(st_l)
	await get_tree().create_timer(3.0).timeout
	_check(absf(al.get_fixture_level("landing", 1) - 0.6) < 0.05, "nose landing light at DIM reads the systems' 0.6 (%.2f)" % al.get_fixture_level("landing", 1))
	# flood knobs: rotate animations without a binding in FlightGear, given a property-adjust action by the cfg
	var bound: PackedStringArray = root._aircraft.get_fg_controls().binding_properties()
	_check(bound.has("controls/lighting/capt-flood") and bound.has("controls/lighting/fo-flood") and bound.has("controls/lighting/pedestal-flood") and bound.has("controls/lighting/overhead-flood"), "the four flood knobs are clickable (knob_actions)")
	# property rules: sound-control.xml computes the sound set's volumes and pitches from the engines
	var rules: FGPropertyRules = root._aircraft.get_fg_property_rules()
	_check(rules != null and rules.get_component_count() >= 50 and rules.unsupported().is_empty(), "sound-control property rules loaded (%d components, unsupported %s)" % [rules.get_component_count() if rules else 0, str(rules.unsupported()) if rules else "-"])
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	await get_tree().create_timer(2.0).timeout
	print("  [diag] rules: internal-custom %.0f fake-n2 %.1f MDA_WHN_1 %.2f WHN_1 pitch %.2f packs %.2f wind-v %.2f roll %.2f" % [rules.get_value("/sim/sound/internal-custom"), rules.get_value("/sim/sound/fake-n2[0]"), rules.get_value("/sim/sound/engine/MDA_WHN_1"), rules.get_value("/sim/sound/enginep/MDA_WHN_1"), rules.get_value("/sim/sound/packs"), rules.get_value("/sim/sound/wind-v"), rules.get_value("/sim/sound/other/roll")])
	_check(rules.get_value("/sim/sound/internal-custom") == 1.0, "cockpit view selects the internal sounds (internal-custom)")
	_check(rules.get_value("/sim/sound/fake-n2[0]") > 50.0, "fake N2 follows the running engine (%.1f)" % rules.get_value("/sim/sound/fake-n2[0]"))
	var snd: FGSounds = root._fg_sounds
	var playing: Array = snd.playing()
	print("  [diag] sounds playing: %s" % str(playing))
	_check("MDA WHN 1" in playing and snd.volume_db_of("MDA WHN 1") > -40.0, "engine whine plays at the rules' volume (%.1f dB)" % snd.volume_db_of("MDA WHN 1"))
	_check("MDB WHN 1" in playing, "second engine's whine plays")
	if _fails.is_empty():
		print("smoke_md80_fdm: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_md80_fdm: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
