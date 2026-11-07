extends Node
## MD-82 autoflight (docs/MD80_FLIGHTGEAR_PORT.md step 18): the DFGS control
## laws (property rules) and the ported mode logic (ItAutoflight). Airborne
## start over LSZH, engage the autopilot, fly a heading, climb to an armed
## altitude in V/S, hold the speed with the autothrottle, disconnect.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_autoflight.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _hdg_err(a: float, b: float) -> float:
	return absf(fposmod(a - b + 180.0, 360.0) - 180.0)

func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 6000.0
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
	var rig = root._aircraft
	_check(jsb != null and jsb.is_initialized() and root._spawn.is_airborne_start(), "JSBSim MD-82 initialised airborne")
	if rig.get_autoflight() != null:
		rig.get_autoflight().set_option("debug", 1.0)
	await get_tree().create_timer(6.0).timeout
	# the control laws: every DFGS rule component runs
	var rules = rig.get_fg_property_rules()
	print("  [diag] property rules: %d components, not run: %s" % [rules.get_component_count(), str(rules.unsupported())])
	_check(rules.get_component_count() > 180, "DFGS rule files loaded (%d components)" % rules.get_component_count())
	_check(rules.unsupported().is_empty(), "every rule component kind runs (pid / pi / predict)")
	var af = rig.get_autoflight()
	_check(af != null, "autoflight module enabled by the cfg")
	if af == null:
		_finish()
		return
	var fga = root._fg_animator
	var fgc0 = rig.get_fg_controls()
	var st: Dictionary = fga.stats()
	var types: Dictionary = st.get("types", {})
	_check(int(types.get("screen-text", 0)) == 16, "FMA windows laid out as screen texts (%d labels)" % int(types.get("screen-text", 0)))
	# The labels are lit (DC radio buses), face the crew (Label3D draws its +z
	# side only; the screen quads' winding points into the glareshield) and
	# an empty mode string is an empty window, not "0".
	var cam: Camera3D = root.camera1
	var lit := 0
	var facing := 0
	var zero_windows := 0
	var labels := root.find_children("ScreenText_*", "Label3D", true, false)
	for lb in labels:
		if lb.is_visible_in_tree():
			lit += 1
		if lb.global_transform.basis.z.dot((cam.global_position - lb.global_position).normalized()) > 0.3:
			facing += 1
		if lb.text == "0":
			zero_windows += 1
	_check(labels.size() == 16 and lit == 16, "every FMA label is lit (%d of %d)" % [lit, labels.size()])
	_check(facing == 16, "every FMA label faces the cockpit camera (%d of %d)" % [facing, labels.size()])
	_check(zero_windows == 0, "empty FMA windows show nothing, not 0 (%d show 0)" % zero_windows)
	print("  [diag] start: lat text %s vert text %s, FMA roll %s/%s pitch %s/%s thr %s/%s arm %s/%s, TRI mode %s (%s), fgcp %.0f V, ap1-avail %s athr-avail %s" % [
		str(af.get_value("it-autoflight/text/lat")), str(af.get_value("it-autoflight/text/vert")),
		str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")),
		str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b")),
		str(af.get_value("instrumentation/fma/thr-mode-a")), str(af.get_value("instrumentation/fma/thr-mode-b")),
		str(af.get_value("instrumentation/fma/arm-mode-a")), str(af.get_value("instrumentation/fma/arm-mode-b")),
		str(jsb.get_property("/systems/engines/limit/active-mode-int")), str(af.get_value("systems/engines/limit/active-mode")),
		jsb.get_property("/systems/electrical/outputs/fgcp"), str(fga.get_property("it-autoflight/input/ap1-avail")), str(fga.get_property("it-autoflight/input/athr-avail"))])
	print("  [diag] FD outputs %s/%s, cockpit fd1 %s, alt %.0f, vs %.0f" % [str(af.get_value("it-autoflight/output/fd1")), str(af.get_value("it-autoflight/output/fd2")), str(fgc0.value_of("it-autoflight/input/fd1")), jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft"), jsb.get_property("/it-autoflight/internal/vert-speed-fpm")])
	_check(str(af.get_value("it-autoflight/text/lat")) == "HDG" and str(af.get_value("it-autoflight/text/vert")) == "V/S", "basic modes HDG / V-S after init")
	_check(str(af.get_value("instrumentation/fma/roll-mode-a")) == "HDG" and str(af.get_value("instrumentation/fma/roll-mode-b")) == "HLD", "FMA roll window HDG HLD")
	_check(str(af.get_value("it-autoflight/input/alt-thousands")) == "10" and str(af.get_value("it-autoflight/input/alt-hundreds")) == "000", "FGCP altitude window strings (10 000)")
	# engage the autopilot (the FGCP AP switch)
	af.panel("apSwitch", 0.0)
	await get_tree().create_timer(1.0).timeout
	print("  [diag] AP: engaged %s, afs/enabled %s, roll-cmd %.3f pitch-cmd %.3f, FD on %s" % [str(af.is_engaged()), str(jsb.get_property("/systems/dfgs/afs/enabled")), jsb.get_property("/systems/dfgs/afs/roll-cmd"), jsb.get_property("/systems/dfgs/afs/pitch-cmd"), str(jsb.get_property("/it-autoflight/output/apfd-on"))])
	_check(af.is_engaged(), "AP 1 engages from the FGCP switch")
	_check(jsb.get_property("/systems/dfgs/afs/enabled") > 0.5, "the FDM's flight controls take the AFS commands")
	# heading select: 40 degrees right of the present heading
	var hdg0: float = fposmod(jsb.get_property("attitude/psi-deg"), 360.0)
	var target_hdg: float = fposmod(roundf(hdg0) + 40.0, 360.0)
	var guard := 0
	while _hdg_err(float(af.get_value("it-autoflight/input/hdg")), target_hdg) > 5.5 and guard < 60:
		guard += 1
		af.panel("hdgAdjust", 10.0 if _hdg_err(float(af.get_value("it-autoflight/input/hdg")) + 10.0, target_hdg) < _hdg_err(float(af.get_value("it-autoflight/input/hdg")), target_hdg) else -10.0)
	while _hdg_err(float(af.get_value("it-autoflight/input/hdg")), target_hdg) > 0.5 and guard < 120:
		guard += 1
		af.panel("hdgAdjust", 1.0 if _hdg_err(float(af.get_value("it-autoflight/input/hdg")) + 1.0, target_hdg) < _hdg_err(float(af.get_value("it-autoflight/input/hdg")), target_hdg) else -1.0)
	af.panel("hdgPull", 0.0)
	await get_tree().create_timer(0.5).timeout
	_check(af.get_lat_mode() == 0 and str(af.get_value("instrumentation/fma/roll-mode-b")) == "SEL", "HDG SEL from the pulled heading knob (bug %s)" % str(af.get_value("it-autoflight/input/hdg")))
	# climb 1500 ft in V/S with the altitude armed
	var alt0: float = jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft")
	var target_alt: float = roundf((alt0 + 1500.0) / 100.0) * 100.0
	guard = 0
	while float(af.get_value("it-autoflight/input/alt")) < target_alt - 50.0 and guard < 200:
		guard += 1
		af.panel("altAdjust", 1.0)
	while float(af.get_value("it-autoflight/input/alt")) > target_alt + 50.0 and guard < 400:
		guard += 1
		af.panel("altAdjust", -1.0)
	for i in range(12):
		af.panel("vsAdjust", 1.0)
	await get_tree().create_timer(2.5).timeout
	print("  [diag] climb: alt bug %s armed %s, V/S bug %s, vert mode %d text %s, FMA arm %s/%s pitch %s/%s, IVSI %.0f" % [str(af.get_value("it-autoflight/input/alt")), str(af.get_value("it-autoflight/input/alt-armed")), str(af.get_value("it-autoflight/input/vs")), af.get_vert_mode(), str(af.get_value("it-autoflight/text/vert")), str(af.get_value("instrumentation/fma/arm-mode-a")), str(af.get_value("instrumentation/fma/arm-mode-b")), str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b")), jsb.get_property("/it-autoflight/internal/vert-speed-fpm")])
	_check(float(af.get_value("it-autoflight/input/alt-armed")) > 0.5 and str(af.get_value("instrumentation/fma/arm-mode-b")) == "ALT", "altitude arms itself a second after the knob (FMA ARM ALT)")
	_check(af.get_vert_mode() == 1 and absf(float(af.get_value("it-autoflight/input/vs")) - 1200.0) < 1.0, "V/S 1200 fpm from the pitch wheel")
	# autothrottle: the A/T switch (a cockpit toggle) and SPD SEL at 250 kt
	var fgc = rig.get_fg_controls()
	var cs: Dictionary = fgc.get_state()
	cs["it-autoflight/input/athr"] = 1.0
	cs["systems/engines/limit/active-mode-int"] = 3.0   # TRI knob to CL after the take-off (T/O rating keeps the A/T clamped)
	fgc.set_state(cs)
	await get_tree().create_timer(0.5).timeout
	af.panel("spdSel", 0.0)
	await get_tree().create_timer(0.5).timeout
	print("  [diag] A/T: output %s thr-mode %d, FMA thr %s/%s, ats-cmd-1 %.3f throttle-pos %.3f lever %.3f" % [str(af.get_value("it-autoflight/output/athr")), af.get_thr_mode(), str(af.get_value("instrumentation/fma/thr-mode-a")), str(af.get_value("instrumentation/fma/thr-mode-b")), jsb.get_property("/systems/engines/ats-cmd-1"), jsb.get_property("fcs/throttle-pos-norm[0]"), jsb.get_input_throttle()])
	_check(float(af.get_value("it-autoflight/output/athr")) > 0.5, "autothrottle engages from the A/T switch")
	_check(af.get_thr_mode() == 0 and str(af.get_value("instrumentation/fma/thr-mode-a")) == "SPD", "SPD SEL: thrust mode, FMA THR SPD")
	# let it fly
	var t := 0.0
	var captured := false
	while t < 110.0:
		await get_tree().create_timer(2.0).timeout
		t += 2.0
		if not captured and str(af.get_value("it-autoflight/text/vert")) == "ALT HLD":
			captured = true
			print("  [diag] ALT HLD after %.0f s at %.0f ft (target %.0f)" % [t, jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft"), target_alt])
		if int(t) % 20 == 0:
			print("  [diag] t+%.0f s: hdg %.1f (bug %s) roll %.1f, alt %.0f vs %.0f, ias %.0f kt, vert %s, thr %s, lever %.2f, trim %.3f, ap %s" % [t, fposmod(jsb.get_property("attitude/psi-deg"), 360.0), str(af.get_value("it-autoflight/input/hdg")), jsb.get_property("attitude/phi-deg"), jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft"), jsb.get_property("velocities/h-dot-fps") * 60.0, jsb.get_property("velocities/vc-kts"), str(af.get_value("it-autoflight/text/vert")), str(af.get_value("instrumentation/fma/thr-mode-a")), jsb.get_input_throttle(), jsb.get_pitch_trim(), str(af.is_engaged())])
	var hdg_now: float = fposmod(float(fga.get_property("orientation/heading-magnetic-deg")), 360.0)   # the bug is magnetic
	var alt_now: float = jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft")
	var ias_now: float = jsb.get_property("velocities/vc-kts")
	_check(af.is_engaged(), "AP still engaged after the climb")
	_check(_hdg_err(hdg_now, target_hdg) < 3.0, "magnetic heading held on the bug (%.1f for %.0f)" % [hdg_now, target_hdg])
	_check(captured and absf(alt_now - target_alt) < 200.0, "altitude captured and held (%.0f for %.0f)" % [alt_now, target_alt])
	_check(absf(ias_now - 250.0) < 15.0, "autothrottle holds 250 kt (%.0f)" % ias_now)
	# disconnect: the yoke button
	af.panel("apDisc", 0.0)
	await get_tree().create_timer(0.5).timeout
	_check(not af.is_engaged() and jsb.get_property("/systems/dfgs/afs/enabled") < 0.5, "AP disconnects from the yoke button")
	_check(float(af.get_value("it-autoflight/sound/apoff")) > 0.5, "AP OFF warning sounds")
	# the thrust levers (2026-11-03, gery: "the right engine keeps going into idle in
	# autothrottle mode; once clicked on the left throttle handle the right one does not
	# move any more"): either lever dragged moves the one throttle input, both engines
	# follow, and no lever pins its engine through the control state afterwards
	af.panel("atDisc", 0.0)
	await get_tree().create_timer(0.5).timeout
	fgc.assign_external("controls/engines/engine[1]/throttle", 0.3)
	await get_tree().create_timer(1.5).timeout
	var tp1: float = jsb.get_property("/systems/engines/control-1/throttle-pos")
	var tp2: float = jsb.get_property("/systems/engines/control-2/throttle-pos")
	print("  [diag] right lever to 0.30: throttle-pos %.3f / %.3f, input %.3f" % [tp1, tp2, jsb.get_input_throttle()])
	_check(absf(tp1 - 0.3) < 0.03 and absf(tp2 - 0.3) < 0.03, "right lever dragged: both engines follow (%.2f / %.2f)" % [tp1, tp2])
	fgc.assign_external("controls/engines/engine/throttle", 0.8)
	await get_tree().create_timer(1.5).timeout
	tp1 = jsb.get_property("/systems/engines/control-1/throttle-pos")
	tp2 = jsb.get_property("/systems/engines/control-2/throttle-pos")
	_check(absf(tp1 - 0.8) < 0.03 and absf(tp2 - 0.8) < 0.03, "left lever dragged after the right: both engines follow (%.2f / %.2f)" % [tp1, tp2])
	var pinned: Array = []
	for k in fgc.get_state().keys():
		if String(k).begins_with("controls/engines/engine") and String(k).ends_with("/throttle"):
			pinned.append(k)
	_check(pinned.is_empty(), "no thrust lever left in the control state (%s)" % str(pinned))
	_finish()

func _finish() -> void:
	if _fails.is_empty():
		print("smoke_md80_autoflight: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_md80_autoflight: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
