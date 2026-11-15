extends Node
## Crash detection and the rebound of a hard touchdown: an aircraft is dropped
## or flown into the ground and the probe reports what the monitor says, how
## high it bounces and whether the flight was started again.
## Run: godot --headless --path provpilot res://tests/probe_crash.tscn -- <id> <case> [detection 0|1]
## cases: drop (30 m, level, no speed), dive (200 kt, 20 deg nose down from 150 m), g (pull at 300 kt), land (2 m/s touchdown)
const THR16 := Vector2(47.47606, 8.53597)
const THR34 := Vector2(47.4453, 8.5547)

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "a10"
	var kase: String = args[1] if args.size() > 1 else "drop"
	var detect: bool = (args[2] != "0") if args.size() > 2 else true
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var st: Dictionary = root._settings.get_settings()
	st["crash_detection"] = detect
	st["weather_layers"] = []
	st["live_weather"] = false
	st["wind_speed_kt"] = 0.0
	root._push_weather_settings()
	var agl_ft := 0.0
	var kt := 0.0
	match kase:
		"drop":
			agl_ft = 100.0
		"dive":
			agl_ft = 600.0
			kt = 200.0
		"g":
			agl_ft = 6000.0
			kt = 300.0
		"land":
			agl_ft = 30.0
			kt = 130.0
	st["aircraft_%s_start_agl_ft" % id] = agl_ft
	st["aircraft_%s_start_speed_kt" % id] = kt
	var a: Vector2 = Geo.wgs84_to_lv95(THR16.x, THR16.y)
	var b: Vector2 = Geo.wgs84_to_lv95(THR34.x, THR34.y)
	var dir: Vector2 = (b - a).normalized()
	var start: Vector2 = a + dir * 400.0
	root._on_welcome_flight_requested(id, start.x, start.y, fposmod(rad_to_deg(atan2(dir.x, dir.y)), 360.0))
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	print("probe_crash: %s %s, detection %s, limits %s" % [id, kase, str(detect), str(root._crash.get_limits())])
	var t := 0.0
	var touched := false
	var max_up_after := 0.0
	var max_agl_after := 0.0
	var reason := ""
	var restarted := false
	var min_agl := INF
	var e0 := -1.0
	var worst_gain := 0.0
	while t < 40.0:
		match kase:
			"drop":
				jsb.set_input_throttle(0.0)
			"dive":
				jsb.set_input_throttle(0.3)
				jsb.set_input_elevator(0.5)
				jsb.set_input_pitch(0.5)
			"g":
				jsb.set_input_throttle(1.0)
				var pull: float = -1.0 if t > 8.0 else 0.0
				jsb.set_input_elevator(pull)
				jsb.set_input_pitch(pull)
			"land":
				jsb.set_input_throttle(0.25)
		await get_tree().process_frame
		t += get_process_delta_time()
		var agl: float = jsb.get_property("position/h-agl-ft") * 0.3048
		var up: float = jsb.get_property("velocities/h-dot-fps") * 0.3048
		var vt: float = jsb.get_property("velocities/vt-fps") * 0.3048
		var wow := false
		for c in jsb.get_gear_contacts():
			if c["wow"]:
				wow = true
		min_agl = minf(min_agl, agl)
		if wow and not touched:
			touched = true
			e0 = agl + vt * vt / 19.62
			print("probe_crash:   first contact at t+%.1f: sink %.1f m/s (%.0f ft/min), %.0f kt, pitch %.1f, roll %.1f, energy height %.0f m" % [t, root._crash.get_last_impact_sink_mps(), root._crash.get_last_impact_sink_mps() * 196.85, jsb.get_property("velocities/vg-fps") * 0.5925, jsb.get_property("attitude/theta-deg"), jsb.get_property("attitude/phi-deg"), e0])
		elif touched:
			max_up_after = maxf(max_up_after, up)
			max_agl_after = maxf(max_agl_after, agl)
			worst_gain = maxf(worst_gain, agl + vt * vt / 19.62 - e0)
		if root._crash_reason != "" and reason == "":
			reason = root._crash_reason
			print("probe_crash:   CRASH at t+%.1f: %s (Nz %.1f, %.0f kt)" % [t, reason, jsb.get_property("accelerations/Nz"), jsb.get_property("velocities/vc-kts")])
		if reason != "" and not restarted:
			# the dialog is up and the flight frozen: check, then press Resume or Restart
			var h0: float = jsb.get_property("position/h-sl-ft")
			await get_tree().create_timer(1.0).timeout
			print("probe_crash:   dialog %s, FDM frozen %s" % [str(root._crash_dialog != null and root._crash_dialog.visible), str(absf(jsb.get_property("position/h-sl-ft") - h0) < 0.01)])
			if OS.get_environment("CRASH_RESUME") != "":
				root._on_crash_resume()
				await get_tree().create_timer(1.0).timeout
				print("probe_crash:   resumed: dialog %s, sim running %s" % [str(root._crash_dialog.visible), str(jsb.is_processing() or jsb.is_physics_processing())])
			else:
				root._on_crash_restart()
				print("probe_crash:   restart requested: dialog %s" % str(root._crash_dialog.visible))
			restarted = true
			break
	root._set_paused(true)
	var hp: float = jsb.get_property("position/h-sl-ft") if false else 0.0
	root._set_paused(false)
	root._end_logged_flight()
	print("probe_crash: logbook dir %s, entries %d" % [root._flight_log.get_directory(), FlightLog.read_entries(root._flight_log.get_directory()).size()])
	print("probe_crash: %s %s: reason '%s', restarted %s; after the first contact: highest climb rate %.1f m/s, highest point %.1f m, energy gained %.0f m" % [id, kase, reason, str(restarted), max_up_after, max_agl_after, worst_gain])
	get_tree().quit(0)
