extends Node
## MD-82 ILS approach to LSZH 28 (IZW 109.75, path 3.3 deg) on the autopilot
## (docs/MD80_FLIGHTGEAR_PORT.md step 22): airborne start 7 nm out under the
## glideslope at 160 kt, gear and flaps, AP + A/T, ILS armed. The DFGS flies
## FlightGear's gs-rate-of-climb (the path angle steepened by ten times the
## angular error), so the needle must stay centred down to 150 ft; then
## AUTO LAND flares, touches down, rolls out, and the ATS lets go at idle.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_ils.tscn
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

const GS_LAT := 47.45765
const GS_LON := 8.56518
const GS_ELEV_M := 423.1
const LOC_LAT := 47.459244
const LOC_LON := 8.533387

func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	# 8.5 nm from the localizer antenna on the extended centreline (back
	# bearing 096 of the 276 true course): about 7 nm from the GS antenna
	var d_m := 8.5 * 1852.0
	var brg := deg_to_rad(96.0)
	var lat := LOC_LAT + d_m * cos(brg) / 111320.0
	var lon := LOC_LON + d_m * sin(brg) / (111320.0 * cos(deg_to_rad(LOC_LAT)))
	var lv: Vector2 = Geo.wgs84_to_lv95(lat, lon)
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 1450.0   # terrain ~2000 ft: ~3450 ft MSL, 400 ft under the 3.3 deg beam at 7 nm
	root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 180.0
	root._settings.get_settings()["aircraft_md80_start_heading_deg"] = 276.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", lv.x, lv.y, 276.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var rig = root._aircraft
	var af = rig.get_autoflight()
	var fgc = rig.get_fg_controls()
	var rules = rig.get_fg_property_rules()
	fgc.assign_external("instrumentation/nav/frequencies/selected-mhz", 109.75)
	fgc.assign_external("instrumentation/nav/radials/selected-deg", 273.0)
	jsb.set_flaps(1.0)   # flaps 40: the retard needs 28 or more
	jsb.set_gear_down(true)
	await get_tree().create_timer(3.0).timeout
	af.panel("apSwitch", 0.0)
	var cs: Dictionary = fgc.get_state()
	cs["it-autoflight/input/athr"] = 1.0
	cs["systems/engines/limit/active-mode-int"] = 3.0
	cs["it-autoflight/input/kts"] = 170.0
	fgc.set_state(cs)
	await get_tree().create_timer(0.5).timeout
	af.panel("spdSel", 0.0)
	for i in range(9):
		af.panel("spdAdjust", -10.0)   # SPD SEL syncs the bug to the IAS: wind it down to 160
	af.panel("ils", 0.0)
	af.panel("autoLand", 0.0)   # AUTO LAND: flare, rollout
	var t := 0.0
	var captured_at := -1.0
	var tracked := 0
	var off_beam := 0
	var worst := 0.0
	var loc_worst := 0.0
	var P := func(k: String) -> float:
		var v = rig.get_fg_property(k)
		return float(v) if v != null else -99.0
	var touchdown_at := -1.0
	var ats_off_at := -1.0
	var saw_flare := false
	var saw_rollout := false
	var ap_on_rollout := false
	var min_gs := 1e9
	while t < 330.0:
		await get_tree().create_timer(2.0).timeout
		t += 2.0
		var alt: float = jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft")
		var agl: float = jsb.get_property("/position/gear-agl-ft")
		var gsn: float = P.call("instrumentation/nav/gs-needle-deflection-norm")
		var locn: float = P.call("instrumentation/nav/heading-needle-deflection-norm")
		if int(t) % 20 == 0:
			print("  [diag] t+%3.0f heading %.1f track %.1f drift %.2f wind %.1f kt from %.0f | target-auto-hdg %.1f nav-hdg-err %.2f | cdi %+.2f" % [t, fposmod(jsb.get_property("attitude/psi-deg"), 360.0), P.call("orientation/track-deg"), P.call("it-autoflight/internal/drift-angle-deg"), jsb.get_property("atmosphere/wind-mag-fps") * 0.5925, fposmod(rad_to_deg(jsb.get_property("atmosphere/psiw-rad")), 360.0), P.call("instrumentation/nav/radials/target-auto-hdg-deg"), P.call("it-autoflight/internal/nav-heading-error-deg"), P.call("instrumentation/nav/heading-needle-deflection")])
		if int(t) % 10 == 0:
			print("  [diag] t+%3.0f alt %5.0f agl %5.0f ias %3.0f | gs norm %+.2f direct %.2f target %.2f roc %6.0f fpm | vs %5.0f fpm | %s %s fma %s/%s %s/%s | loc %+.2f" % [
				t, alt, agl, jsb.get_property("velocities/vc-kts"), gsn, P.call("instrumentation/nav/gs-direct-deg"), P.call("instrumentation/nav/gs-target-deg"), P.call("instrumentation/nav/gs-rate-of-climb-fpm"),
				jsb.get_property("velocities/h-dot-fps") * 60.0, str(af.get_value("it-autoflight/text/lat")), str(af.get_value("it-autoflight/text/vert")),
				str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")), str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b")), locn])
		if captured_at < 0.0 and af.get_vert_mode() == 2 and str(af.get_value("it-autoflight/text/vert")) == "G/S":
			captured_at = t
		if captured_at >= 0.0 and t >= captured_at + 40.0 and agl > 150.0:
			tracked += 1
			worst = maxf(worst, absf(gsn))
			loc_worst = maxf(loc_worst, absf(locn))
			if absf(gsn) > 0.15:
				off_beam += 1
		var vt := str(af.get_value("it-autoflight/text/vert"))
		if vt == "FLARE":
			saw_flare = true
		if vt == "ROLLOUT":
			saw_rollout = true
			if af.is_engaged():
				ap_on_rollout = true
		if touchdown_at < 0.0 and P.call("gear/gear[1]/wow") > 0.5 and P.call("gear/gear[2]/wow") > 0.5:
			touchdown_at = t
			print("  [diag] touchdown at t+%.0f, ias %.0f, vert %s, athr %s" % [t, jsb.get_property("velocities/vc-kts"), vt, str(af.get_value("it-autoflight/output/athr"))])
		if touchdown_at >= 0.0 and ats_off_at < 0.0 and not bool(af.get_value("it-autoflight/output/athr")):
			ats_off_at = t
			print("  [diag] ATS off at t+%.0f (%.0f s after touchdown): levers %.3f / %.3f, idle-norm %.3f, input athr %s, thr FMA %s/%s" % [t, t - touchdown_at, jsb.get_property("/systems/engines/control-1/throttle-pos"), jsb.get_property("/systems/engines/control-2/throttle-pos"), jsb.get_property("/systems/engines/limit/idle-norm"), str(fgc.value_of("it-autoflight/input/athr")), str(af.get_value("instrumentation/fma/thr-mode-a")), str(af.get_value("instrumentation/fma/thr-mode-b"))])
		if touchdown_at >= 0.0:
			min_gs = minf(min_gs, jsb.get_property("velocities/groundspeed-kt"))
			if t >= touchdown_at + 40.0:
				break
		if agl >= 150.0 and not af.is_engaged():
			break
	_check(captured_at >= 0.0 and captured_at <= 30.0, "G/S captured from below within 30 s (at %.0f s)" % captured_at)
	_check(saw_flare and saw_rollout and touchdown_at >= 0.0, "AUTO LAND: FLARE, touchdown (t+%.0f) and ROLLOUT" % touchdown_at)
	_check(ap_on_rollout and af.is_engaged(), "the autopilots stay engaged through the rollout (FlightGear: until the pilot disconnects)")
	_check(ats_off_at >= 0.0 and ats_off_at - touchdown_at <= 20.0 and float(fgc.value_of("it-autoflight/input/athr")) < 0.5, "the ATS lets go once it has retarded the levers to idle on the ground, the AUTO THROT switch drops (%.0f s after touchdown)" % (ats_off_at - touchdown_at))
	_check(tracked >= 20 and off_beam == 0, "glideslope needle within 0.15 norm from capture + 40 s down to 150 ft (%d samples, worst %.2f, %d off)" % [tracked, worst, off_beam])
	_check(tracked >= 20 and loc_worst < 0.1, "localizer needle centred on final, inside the flare's 0.1 gate (worst %.2f)" % loc_worst)
	_check(absf(P.call("it-autoflight/internal/drift-angle-deg")) < 5.0, "the DFGS drift angle is a real track - heading difference (%.2f deg)" % P.call("it-autoflight/internal/drift-angle-deg"))
	_check(absf(P.call("instrumentation/nav/gs-target-deg") - 3.3) < 0.01, "the receiver publishes the site's 3.3 deg path (gs-target-deg %.2f)" % P.call("instrumentation/nav/gs-target-deg"))
	if _fails.is_empty():
		print("smoke_md80_ils: OK")
	else:
		printerr("smoke_md80_ils: %d FAIL" % _fails.size())
		for f in _fails:
			printerr("  - " + f)
	get_tree().quit(0 if _fails.is_empty() else 1)
