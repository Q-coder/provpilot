extends Node
## Probe (not a smoke test): an ILS approach to LSZH 28 (IZW 109.75, path 3.3 deg)
## on the autopilot. Airborne start 7 nm out below the glideslope, AP + A/T on,
## ILS armed; prints the glideslope geometry and the DFGS's rate every 2 s.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_ils.tscn
const GS_LAT := 47.45765
const GS_LON := 8.56518
const GS_ELEV_M := 423.1

func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	# 7 nm out along the back bearing 096 of the 276 course
	var d_m := 7.0 * 1852.0
	var brg := deg_to_rad(96.0)
	var lat := GS_LAT + d_m * cos(brg) / 111320.0
	var lon := GS_LON + d_m * sin(brg) / (111320.0 * cos(deg_to_rad(GS_LAT)))
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
	jsb.set_flaps(0.75)
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
	var t := 0.0
	while t < 240.0:
		await get_tree().create_timer(2.0).timeout
		t += 2.0
		var alt: float = jsb.get_property("/instrumentation/altimeter/indicated-altitude-ft")
		var agl: float = jsb.get_property("/position/gear-agl-ft")
		var P := func(k: String) -> float:
			var v = rig.get_fg_property(k)
			return float(v) if v != null else -99.0
		print("t+%3.0f alt %5.0f agl %5.0f ias %3.0f dme %.1f nm | gs norm %+.2f inrange %.0f direct %.2f target %.2f roc %6.0f fpm | vs %5.0f fpm pitch-cmd %+.2f kts-bug %.0f | lat %s vert %s (%d) fma %s/%s %s/%s | loc %+.2f" % [
			t, alt, agl, jsb.get_property("velocities/vc-kts"), P.call("instrumentation/nav/dme-distance-nm"),
			P.call("instrumentation/nav/gs-needle-deflection-norm"), P.call("instrumentation/nav/gs-in-range"), P.call("instrumentation/nav/gs-direct-deg"), P.call("instrumentation/nav/gs-target-deg"), P.call("instrumentation/nav/gs-rate-of-climb-fpm"),
			jsb.get_property("velocities/h-dot-fps") * 60.0, jsb.get_property("/systems/dfgs/afs/pitch-cmd"), float(af.get_value("it-autoflight/input/kts")),
			str(af.get_value("it-autoflight/text/lat")), str(af.get_value("it-autoflight/text/vert")), af.get_vert_mode(),
			str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")), str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b")),
			P.call("instrumentation/nav/heading-needle-deflection-norm")])
		if agl < 120.0 or not af.is_engaged():
			print("stop: agl %.0f engaged %s" % [agl, str(af.is_engaged())])
			break
	get_tree().quit(0)
