extends Node
## Sky, haze and clouds through dusk and night: an MD-80 at 6000 ft over
## Zurich, the user's weather layers, one screenshot (cockpit off, chase view)
## per local time given. Prints the twilight factor, the fog light and the
## mean brightness of the upper (sky / clouds) and lower (ground / haze) half.
## Run: godot --path provpilot res://tests/probe_dusk.tscn -- <out_dir> [hours ...]
const E := 2683732.0
const N := 1257695.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir: String = args[0] if args.size() > 0 else ""
	var hours: Array = []
	for i in range(1, args.size()):
		hours.append(float(args[i]))
	if hours.is_empty():
		hours = [12.0, 16.6, 17.2, 22.0]
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 4500.0
	root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 220.0
	if OS.get_environment("DUSK_AIRCRAFT") != "":
		root._settings.get_settings()["aircraft_%s_start_speed_kt" % OS.get_environment("DUSK_AIRCRAFT")] = float(OS.get_environment("DUSK_KT")) if OS.get_environment("DUSK_KT") != "" else 220.0
	var se := E
	var sn := N
	var hdg := 250.0
	# DUSK_LAT / DUSK_LON / DUSK_HDG / DUSK_AGL_FT: anywhere else (the Oslofjord for the sea)
	if OS.get_environment("DUSK_LAT") != "":
		var lv: Vector2 = Geo.wgs84_to_lv95(float(OS.get_environment("DUSK_LAT")), float(OS.get_environment("DUSK_LON")))
		se = lv.x
		sn = lv.y
	if OS.get_environment("DUSK_HDG") != "":
		hdg = float(OS.get_environment("DUSK_HDG"))
	if OS.get_environment("DUSK_AGL_FT") != "":
		root._settings.get_settings()["aircraft_%s_start_agl_ft" % (OS.get_environment("DUSK_AIRCRAFT") if OS.get_environment("DUSK_AIRCRAFT") != "" else "md80")] = float(OS.get_environment("DUSK_AGL_FT"))
	root._on_welcome_flight_requested(OS.get_environment("DUSK_AIRCRAFT") if OS.get_environment("DUSK_AIRCRAFT") != "" else "md80", se, sn, hdg)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root.jsb_node.set_paused(true) if root.jsb_node.has_method("set_paused") else null
	for h in hours:
		root._sky.set_time(false, int(h * 60.0))
		await get_tree().create_timer(float(OS.get_environment("DUSK_WAIT")) if OS.get_environment("DUSK_WAIT") != "" else 2.0).timeout
		var img: Image = get_viewport().get_texture().get_image()
		var up := 0.0
		var lo := 0.0
		var n := 0
		for y in range(0, img.get_height(), 16):
			for x in range(0, img.get_width(), 16):
				var c: Color = img.get_pixel(x, y)
				if y < img.get_height() / 2:
					up += c.get_luminance()
				else:
					lo += c.get_luminance()
				n += 1
		var env: Environment = root._sky.environment
		print("probe_dusk: %05.2f h: twilight %.2f, fog light %.3f, cloud sky light %.3f, sun %.2f | mean brightness upper half %.3f, lower half %.3f" % [h, root._sky.twilight_now, env.fog_light_energy, root._sky.cloud_ambient_now, root._sky.sun_energy_now, up / (n * 0.5), lo / (n * 0.5)])
		print("probe_dusk: %05.2f h: aircraft %.2f m above sea level, water sink %.2f m, FDM agl %.2f m, sea floor %.1f m, ground query %.2f m" % [h, W3DCoords.ecef_to_geodetic(root.get_node("AC").global_position).z, root._world.water_sink_m, root.jsb_node.get_property("position/h-agl-ft") * 0.3048, root._world.terrain_elevation_below(root.get_node("AC").global_position), root._world.ground_height_below(root.get_node("AC").global_position)])
		print("probe_dusk: %05.2f h: city lights %s" % [h, str(root._city_lights.stats())])
		print("probe_dusk: %05.2f h: %.0f fps, %d draw calls" % [h, Engine.get_frames_per_second(), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)])
		if OS.get_environment("DUSK_NO_CITY") != "":
			root._city_lights.visible = false
			root._city_lights.set_process(false)
			var keep: bool = root._city_lights.enabled
			root._city_lights.enabled = false
			await get_tree().create_timer(3.0).timeout
			print("probe_dusk: %05.2f h without city lights: %.0f fps, %d draw calls" % [h, Engine.get_frames_per_second(), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)])
			root._city_lights.enabled = keep
		if out_dir != "":
			img.resize(img.get_width() / 2, img.get_height() / 2)
			img.save_png(out_dir.path_join("dusk_%04d.png" % int(h * 100.0)))
	get_tree().quit(0)
