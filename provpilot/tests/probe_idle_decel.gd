extends Node
## Idle deceleration: airborne start, throttle closed, wings level on the
## autopilot-less FDM (pitch held by a simple altitude loop); prints speed,
## thrust, drag and engine state every 2 s.
## Run: godot --headless --path provpilot res://tests/probe_idle_decel.tscn -- <id> [kt] [throttle] [seconds]
const E := 2683732.0
const N := 1257695.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "a10"
	var kt0: float = float(args[1]) if args.size() > 1 else 280.0
	var thr: float = float(args[2]) if args.size() > 2 else 0.0
	var seconds: float = float(args[3]) if args.size() > 3 else 40.0
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var st: Dictionary = root._settings.get_settings()
	st["aircraft_%s_start_agl_ft" % id] = 5000.0
	st["aircraft_%s_start_speed_kt" % id] = kt0
	st["weather_layers"] = []
	st["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested(id, E, N, 250.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(5.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var alt0: float = jsb.get_property("position/h-sl-ft")
	jsb.set_gear_down(false)
	var t := 0.0
	var last := 0.0
	while t < seconds:
		# hands off (the airborne start is trimmed): only the throttle and the wings
		jsb.set_input_throttle(thr)
		var vs: float = jsb.get_property("velocities/h-dot-fps")
		var phi: float = jsb.get_property("attitude/phi-deg")
		var ail: float = clampf(-phi * 0.05 - jsb.get_property("velocities/p-rad_sec") * 0.4, -1.0, 1.0)
		jsb.set_input_roll(ail)
		jsb.set_input_aileron(ail)
		await get_tree().process_frame
		t += get_process_delta_time()
		if t - last >= 2.0:
			last = t
			var vt: float = jsb.get_property("velocities/vt-fps") * 0.3048
			print("probe_idle: energy height %.0f m" % (jsb.get_property("position/h-sl-ft") * 0.3048 + vt * vt / 19.62))
			print("probe_idle: t+%4.1f %s %.0f kt, alt %.0f ft, vs %.0f fpm | thrust %.0f + %.0f lbs, drag %.0f lbs, weight %.0f lbs | throttle cmd %.2f pos %.2f, n1 %.1f n2 %.1f | speedbrake %.2f, gear %.1f, flaps %.2f, alpha %.1f" % [t, id, jsb.get_property("velocities/vc-kts"), jsb.get_property("position/h-sl-ft"), vs * 60.0,
				jsb.get_property("propulsion/engine[0]/thrust-lbs"), jsb.get_property("propulsion/engine[1]/thrust-lbs"), jsb.get_property("forces/fwx-aero-lbs"), jsb.get_property("inertia/weight-lbs"),
				jsb.get_property("fcs/throttle-cmd-norm"), jsb.get_property("fcs/throttle-pos-norm"), jsb.get_property("propulsion/engine[0]/n1"), jsb.get_property("propulsion/engine[0]/n2"),
				jsb.get_property("fcs/speedbrake-pos-norm"), jsb.get_property("gear/gear-pos-norm"), jsb.get_property("fcs/flap-pos-norm"), jsb.get_property("aero/alpha-deg")])
	get_tree().quit(0)
