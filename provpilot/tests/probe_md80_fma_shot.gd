extends Node
## Probe (not a smoke test): close-up of the MD-80's left FMA (the screen-text
## labels of docs/MD80_FLIGHTGEAR_PORT.md step 18), airborne start so the
## autoflight has modes to show.
## Run NON-headless: godot --path provpilot res://tests/probe_md80_fma_shot.tscn -- <out_dir> [yaw pitch zoom]
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var yaw: float = float(args[1]) if args.size() > 1 else 4.0
	var pitch: float = float(args[2]) if args.size() > 2 else 5.0
	var zoom: int = int(args[3]) if args.size() > 3 else 10
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 6000.0
	root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 250.0
	root._settings.get_settings()["aircraft_md80_start_heading_deg"] = 155.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var rig = root._camera_rig
	rig.set_mode(0)
	rig.set_cockpit_enabled(true)
	root.camera1.current = true
	# engage AP 1 and arm 6 000 ft so the ARM window shows a flight level
	var af = root._aircraft.get_autoflight()
	if af != null:
		af.panel("apSwitch", 0.0)
		for i in range(4):
			af.panel("altAdjust", -10.0)
		await get_tree().create_timer(2.0).timeout
		print("  [diag] FMA thr %s/%s arm %s/%s roll %s/%s pitch %s/%s" % [str(af.get_value("instrumentation/fma/thr-mode-a")), str(af.get_value("instrumentation/fma/thr-mode-b")), str(af.get_value("instrumentation/fma/arm-mode-a")), str(af.get_value("instrumentation/fma/arm-mode-b")), str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")), str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b"))])
	rig.set_head_look(yaw, pitch)
	for i in range(zoom):
		rig.mouse_zoom(1)
	await get_tree().create_timer(2.0).timeout
	get_viewport().get_texture().get_image().save_png(out.path_join("md80_fma.png"))
	print("saved %s" % out.path_join("md80_fma.png"))
	get_tree().quit(0)
