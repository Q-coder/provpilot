extends Node
## Probe (not a smoke test): the FGCP with the digital dimmer at 0.3 and the
## panel (backplate) dimmer at 1.0 - a glareshield close-up.
## Run NON-headless: godot --path provpilot res://tests/probe_md80_fgcp_shot.tscn -- <out_dir> [digital panel]
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var digital: float = float(args[1]) if args.size() > 1 else 0.3
	var panel: float = float(args[2]) if args.size() > 2 else 1.0
	var night: bool = args.size() > 3 and args[3] == "night"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	if night:
		root._settings.get_settings()["use_system_time"] = false
		root._settings.get_settings()["time_of_day_min"] = 23 * 60
		root._push_time_of_day_settings()
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var fgc = root._aircraft.get_fg_controls()
	fgc.assign_external("controls/lighting/fgcp-digital", digital)
	fgc.assign_external("controls/lighting/fgcp-panel", panel)
	var rig = root._camera_rig
	rig.set_mode(0)
	rig.set_cockpit_enabled(true)
	root.camera1.current = true
	rig.set_head_look(0.0, -4.0)
	for i in range(6):
		rig.mouse_zoom(1)
	await get_tree().create_timer(2.5).timeout
	get_viewport().get_texture().get_image().save_png(out.path_join("md80_fgcp_dim%s.png" % ("_night" if night else "")))
	print("saved")
	get_tree().quit(0)
