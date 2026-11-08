extends Node
## Forest frame-rate probe (docs/WORLD_SCENERY_DESIGN.md §14.2 step 5): the
## app over the Tösstal forests (Zürich Oberland) with the swissTLM3D tree
## tiles of the online scenery, airborne at 1 000 ft. Prints frame time,
## primitives and draw calls averaged over a few seconds, saves screenshots
## from the cockpit, from the chase camera and straight down.
## Run (real renderer): godot/bin/godot.macos.editor.double.arm64 --path provpilot res://tests/probe_forest_fps.tscn -- <out_dir> [veg_dir] [density 0-4] [E N heading agl_ft]
## Default: the Tösstal forests (Zürich Oberland); e.g. 2674500 1186500 160 2500 for Engelberg and the tree line.
const E_DEFAULT := 2707800.0
const N_DEFAULT := 1246400.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var veg_dir: String = args[1] if args.size() > 1 else "/Users/gery/provpilot/online_scenery/Switzerland/vegetation"
	var density: int = int(args[2]) if args.size() > 2 else 3
	var E: float = float(args[3]) if args.size() > 3 else E_DEFAULT
	var N: float = float(args[4]) if args.size() > 4 else N_DEFAULT
	var heading: float = float(args[5]) if args.size() > 5 else 40.0
	var agl_ft: float = float(args[6]) if args.size() > 6 else 1000.0
	DirAccess.make_dir_recursive_absolute(out)
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_c172p_start_agl_ft"] = agl_ft
	root._settings.get_settings()["aircraft_c172p_start_speed_kt"] = 100.0
	root._settings.get_settings()["aircraft_c172p_start_heading_deg"] = heading
	root._settings.get_settings()["tree_density"] = density
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("c172p", E, N, heading)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var veg = root.vegetation_node
	var paths: PackedStringArray = veg.vegetation_paths   # keep the library's dirs (corridor files of the base packs)
	if not paths.has(veg_dir):
		paths.append(veg_dir)
	veg.vegetation_paths = paths
	root._settings.apply_tree_density(veg)
	print("[forest] card_mode %s, species %d, density idx %d -> fraction %.2f, near %.0f m, far %.0f m" % [str(veg.card_mode), veg.get_species_count(), density, veg.density_fraction, veg.near_range_m, veg.far_range_m])
	await get_tree().create_timer(12.0).timeout   # tiles stream in
	var tiles_before: int = veg.get_active_tile_count()
	var frames := 0
	var t := 0.0
	var prims := 0.0
	var calls := 0.0
	while t < 6.0:
		await get_tree().process_frame
		var dt: float = get_process_delta_time()
		t += dt
		frames += 1
		prims += Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)
		calls += Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)
	var fps := frames / t
	print("[forest] %d vegetation tiles active (%d before the measurement), %d still waiting for terrain heights" % [veg.get_active_tile_count(), tiles_before, veg.pending_tile_count()])
	print("[forest] %.1f fps over %.1f s, %.1f M primitives / frame, %.0f draw calls / frame, %.0f MB video memory" % [fps, t, prims / frames / 1e6, calls / frames, Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0])
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out.path_join("forest_cockpit.png"))
	# chase camera
	root._camera_rig.flip()
	await get_tree().create_timer(0.5).timeout
	root._camera_rig.flip()
	await get_tree().create_timer(1.0).timeout
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out.path_join("forest_chase.png"))
	# straight down from 600 m above the aircraft
	var cam := Camera3D.new()
	root.add_child(cam)
	var ac: Node3D = root._ac
	var up: Vector3 = ac.global_position.normalized()
	cam.global_position = ac.global_position + up * 600.0
	cam.look_at(ac.global_position, ac.global_transform.basis.z)
	cam.far = 50000.0
	cam.current = true
	await get_tree().create_timer(1.0).timeout
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(out.path_join("forest_top.png"))
	print("[forest] screenshots in %s" % out)
	get_tree().quit(0)
