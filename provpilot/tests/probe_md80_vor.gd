extends Node
## MD-82 on the autopilot in VOR LOC: does it capture and hold a VOR radial?
## Airborne start some miles off a KPT (109.60) radial, inbound (TO) or
## outbound (FROM), VOR LOC armed; the course needle must come to the centre
## and stay there, and the heading must settle on the course.
## Run: godot --headless --path provpilot res://tests/probe_md80_vor.tscn -- <to|from> [course_deg] [offset_deg] [dist_nm] [seconds]
const KPT := Vector2(47.7458, 10.3500)

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var mode: String = args[0] if args.size() > 0 else "to"
	var course: float = float(args[1]) if args.size() > 1 else 64.0
	var offset: float = float(args[2]) if args.size() > 2 else 6.0
	var dist_nm: float = float(args[3]) if args.size() > 3 else 30.0
	var seconds: float = float(args[4]) if args.size() > 4 else 240.0
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	# where the aircraft starts: on a bearing from the station (true, close enough to magnetic here)
	var brg_from: float = (course + 180.0 + offset) if mode == "to" else (course + offset)
	var d_m := dist_nm * 1852.0
	var lat := KPT.x + d_m * cos(deg_to_rad(brg_from)) / 111320.0
	var lon := KPT.y + d_m * sin(deg_to_rad(brg_from)) / (111320.0 * cos(deg_to_rad(KPT.x)))
	var lv: Vector2 = Geo.wgs84_to_lv95(lat, lon)
	var st: Dictionary = root._settings.get_settings()
	st["aircraft_md80_start_agl_ft"] = 9000.0
	st["aircraft_md80_start_speed_kt"] = 250.0
	st["aircraft_md80_start_heading_deg"] = course
	st["weather_layers"] = []
	st["live_weather"] = false
	st["wind_speed_kt"] = 0.0
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", lv.x, lv.y, course)
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
	fgc.assign_external("instrumentation/nav/frequencies/selected-mhz", 109.60)
	fgc.assign_external("instrumentation/nav/radials/selected-deg", course)
	await get_tree().create_timer(3.0).timeout
	af.panel("apSwitch", 0.0)
	var cs: Dictionary = fgc.get_state()
	cs["it-autoflight/input/athr"] = 1.0
	cs["it-autoflight/input/kts"] = 250.0
	fgc.set_state(cs)
	await get_tree().create_timer(0.5).timeout
	af.panel("spdSel", 0.0)
	af.panel("hdgAdjust", course - float(af.get_value("it-autoflight/input/hdg")))   # heading bug on the course
	af.panel("hdgPull", 0.0)
	af.panel("vorLoc", 0.0)
	var P := func(k: String) -> float:
		var v = rig.get_fg_property(k)
		return float(v) if v != null else -99.0
	var t := 0.0
	var late_worst := 0.0
	var prev := Vector2.ZERO
	while t < seconds:
		await get_tree().create_timer(5.0).timeout
		t += 5.0
		var cdi: float = P.call("instrumentation/nav/heading-needle-deflection")
		if t > seconds - 60.0:
			late_worst = maxf(late_worst, absf(cdi))
		var ag: Vector3 = W3DCoords.ecef_to_geodetic(root.get_node("AC").global_transform.origin)
		var here := Vector2(ag.x, ag.y)
		var real_track := -1.0
		if prev != Vector2.ZERO:
			real_track = fposmod(rad_to_deg(atan2((here.y - prev.y) * cos(deg_to_rad(here.x)), here.x - prev.x)), 360.0)
		prev = here
		print("probe_vor:        position %.4f %.4f, track over the ground %.1f (scene), FDM track %.1f, FDM heading %.1f, FDM lat/lon %.4f %.4f, wind %.1f kt" % [ag.x, ag.y, real_track, P.call("orientation/track-deg"),
			fposmod(jsb.get_property("attitude/psi-deg"), 360.0), jsb.get_property("position/lat-geod-deg"), jsb.get_property("position/long-gc-deg"), jsb.get_property("atmosphere/wind-mag-fps") * 0.5925])
		print("probe_vor: t+%3.0f lat %s (%s/%s arm %s) | crs %.0f radial %.1f %s dist %.1f nm in-range %.0f | cdi %+.2f | hdg true %.1f mag %.1f target-auto %.1f err %+.1f | roll %+.1f target %+.1f | ias %.0f alt %.0f" % [
			t, str(af.get_value("it-autoflight/text/lat")), str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")), str(af.get_value("instrumentation/fma/arm-mode-a")),
			P.call("instrumentation/nav/radials/selected-deg"), P.call("instrumentation/nav/radials/actual-deg"),
			"TO" if P.call("instrumentation/nav/to-flag") > 0.5 else ("FROM" if P.call("instrumentation/nav/from-flag") > 0.5 else "--"),
			P.call("instrumentation/nav/nav-distance") / 1852.0, P.call("instrumentation/nav/in-range"), cdi,
			fposmod(jsb.get_property("attitude/psi-deg"), 360.0), P.call("orientation/heading-magnetic-deg"), P.call("instrumentation/nav/radials/target-auto-hdg-deg"),
			P.call("it-autoflight/internal/nav-heading-error-deg"), jsb.get_property("attitude/phi-deg"), P.call("it-autoflight/internal/target-roll-deg"),
			jsb.get_property("velocities/vc-kts"), jsb.get_property("position/h-sl-ft")])
	var ok := late_worst < 1.0
	print(("  PASS: " if ok else "  FAIL: ") + "VOR LOC %s course %.0f: needle within 1 deg over the last minute (worst %.2f)" % [mode, course, late_worst])
	get_tree().quit(0 if ok else 1)
