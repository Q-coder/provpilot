extends Node
## Canopy glass check: an airborne start in the given aircraft, a cockpit
## screenshot looking out through the canopy (docs/AIRCRAFT_CONFIG.md [model]
## glass_*). Run (real renderer):
##   godot/bin/godot.macos.editor.double.arm64 --path provpilot res://tests/probe_canopy_shot.tscn -- <out_dir> <aircraft_id>
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var id: String = args[1] if args.size() > 1 else "pc7"
	DirAccess.make_dir_recursive_absolute(out)
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_%s_start_agl_ft" % id] = 2000.0
	root._settings.get_settings()["aircraft_%s_start_speed_kt" % id] = 90.0
	root._settings.get_settings()["aircraft_%s_start_heading_deg" % id] = 200.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested(id, 2682767.0, 1258797.0, 200.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root.camera1.current = true   # the cockpit camera: the canopy seen from inside
	await get_tree().create_timer(6.0).timeout
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out.path_join("canopy_%s.png" % id))
	print("[canopy] saved %s" % out.path_join("canopy_%s.png" % id))
	get_tree().quit(0)
