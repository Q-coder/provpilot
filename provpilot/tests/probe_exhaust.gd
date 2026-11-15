extends Node
## Engine exhaust effects (src/exhaust_effects.h): screenshots of the heat
## shimmer on the ground and of the contrail in flight (forced on, whatever
## the air temperature), plus the numbers behind them.
## Run (real renderer): godot --path provpilot res://tests/probe_exhaust.tscn -- <out_dir> [id] [ground|air|both]
var _root: Node = null
var _cam: Camera3D = null
var _cam_offset := Vector3.ZERO   # right, up, forward in metres from the aircraft
var _cam_on := false

func _process(_dt: float) -> void:
	# runs after the FDM (process priority), so the camera sits still on the aircraft
	if not _cam_on or _cam == null:
		return
	var ac: Node3D = _root.get_node("AC")
	var b: Basis = ac.global_transform.basis.orthonormalized()
	var pos: Vector3 = ac.global_transform.origin - b.x * _cam_offset.x + b.y * _cam_offset.y + b.z * _cam_offset.z
	var g: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_transform.origin)
	var up: Vector3 = W3DCoords.geodetic_normal(g.x, g.y)
	var fwd: Vector3 = (ac.global_transform.origin + b.y * 2.0 - b.z * _look_aft - pos).normalized()
	var right: Vector3 = fwd.cross(up).normalized()
	_cam.global_transform = Transform3D(Basis(right, right.cross(fwd), -fwd), pos)
	if not _cam.current:
		_cam.make_current()   # the app's camera rig takes the view back every frame
var _look_aft := 0.0
var _aft := -13.0   # where the nozzles are along the fuselage (metres forward of the node)

func _shot(path: String, offset: Vector3, look_aft: float, fov: float = 50.0) -> void:
	_cam_offset = offset
	_look_aft = look_aft
	_cam.fov = fov
	_cam_on = true
	_cam.current = true
	await get_tree().create_timer(0.6).timeout
	await RenderingServer.frame_post_draw
	var cg: Vector3 = W3DCoords.ecef_to_geodetic(_cam.global_transform.origin)
	var active: Camera3D = get_viewport().get_camera_3d()
	print("probe_exhaust: shot %s: active camera %s (%s), %.1f m above the ground, %.1f m from the aircraft" % [path.get_file(), active.name, "mine" if active == _cam else "NOT mine",
		cg.z - _root._world.ground_height_geodetic(cg.x, cg.y), _cam.global_transform.origin.distance_to(_root.get_node("AC").global_transform.origin)])
	get_viewport().get_texture().get_image().save_png(path)

func _start(id: String, agl_ft: float, kt: float) -> void:
	_root._settings.get_settings()["aircraft_%s_start_agl_ft" % id] = agl_ft
	_root._settings.get_settings()["aircraft_%s_start_speed_kt" % id] = kt
	_root._settings.get_settings()["weather_layers"] = []
	_root._settings.get_settings()["live_weather"] = false
	_root._settings.get_settings()["wind_speed_kt"] = 0.0
	_root._push_weather_settings()
	var thr_a: Vector2 = Geo.wgs84_to_lv95(47.47606, 8.53597)
	var thr_b: Vector2 = Geo.wgs84_to_lv95(47.4453, 8.5547)
	var dir: Vector2 = (thr_b - thr_a).normalized()
	var start: Vector2 = thr_a + dir * 200.0
	_root._on_welcome_flight_requested(id, start.x, start.y, fposmod(rad_to_deg(atan2(dir.x, dir.y)), 360.0))
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	_root._cancel_intro_zoom()
	_root._welcome_screen.visible = false

func _ready() -> void:
	process_priority = 2000
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var id: String = args[1] if args.size() > 1 else "a10"
	var what: String = args[2] if args.size() > 2 else "both"
	_aft = float(args[3]) if args.size() > 3 else -13.0
	DirAccess.make_dir_recursive_absolute(out)
	get_window().size = Vector2i(1600, 1000)
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(3.0).timeout
	_cam = Camera3D.new()
	_cam.far = 80000.0
	_cam.near = 0.3
	# under the aircraft node: the terrain streamer and the spawn logic follow the active
	# camera, and one outside the aircraft makes them re-pin the aircraft
	_root.get_node("AC").add_child(_cam)
	var jsb = _root.jsb_node
	var ok := true
	if what != "air":
		await _start(id, 0.0, 0.0)
		var fx = _root._aircraft.get_exhaust_effects()
		var t := 0.0
		while t < 14.0:   # the scenery behind the plume streams in
			jsb.set_input_throttle(0.25)
			jsb.set_input_brake(1.0)
			await get_tree().process_frame
			t += get_process_delta_time()
		print("probe_exhaust: %s on the ground: %d nozzles, heat power %.2f, contrail factor %.2f, puffs %d" % [id, fx.get_nozzle_count(), fx.get_heat_power(0), fx.get_contrail_factor(), fx.get_puff_count()])
		ok = ok and fx.get_nozzle_count() > 0 and fx.get_heat_power(0) > 0.2 and fx.get_puff_count() == 0
		print("probe_exhaust: %s weight on wheels %s, %.0f kt" % [id, str(jsb.get_weight_on_wheels()), jsb.get_property("velocities/vc-kts")])
		var ag: Vector3 = W3DCoords.ecef_to_geodetic(_root.get_node("AC").global_transform.origin)
		print("probe_exhaust: node %.2f m above the ground, pitch %.1f, up.y-axis %.3f, origin %s" % [ag.z - _root._world.ground_height_geodetic(ag.x, ag.y), jsb.get_property("attitude/theta-deg"),
			_root.get_node("AC").global_transform.basis.orthonormalized().y.dot(W3DCoords.geodetic_normal(ag.x, ag.y)), str(_root.get_node("AC").global_transform.origin)])
		var side := Vector3(-20.0, 2.6, _aft - 6.0)
		await _shot(out.path_join("exhaust_%s_ground_side.png" % id), side, -_aft + 5.0, 30.0)
		fx.enabled = false
		await _shot(out.path_join("exhaust_%s_ground_side_off.png" % id), side, -_aft + 5.0, 30.0)
		fx.enabled = true
		await _shot(out.path_join("exhaust_%s_ground_aft.png" % id), Vector3(7.0, 3.2, _aft - 30.0), 0.0, 30.0)
		fx.enabled = false
		await _shot(out.path_join("exhaust_%s_ground_aft_off.png" % id), Vector3(7.0, 3.2, _aft - 30.0), 0.0, 30.0)
		fx.enabled = true
	if what != "ground":
		await _start(id, 6000.0, 280.0)
		var fx2 = _root._aircraft.get_exhaust_effects()
		print("probe_exhaust: %s at %.0f ft: outside air gives contrail factor %.2f" % [id, jsb.get_property("position/h-sl-ft"), fx2.get_contrail_factor()])
		fx2.force_contrail = true
		var t2 := 0.0
		var frames := 0
		while t2 < 40.0:
			jsb.set_input_throttle(0.9)
			await get_tree().process_frame
			t2 += get_process_delta_time()
			frames += 1
		print("probe_exhaust: %s in flight, contrail forced: %d puffs after %.0f s, %.1f fps, speed %.0f kt" % [id, fx2.get_puff_count(), t2, frames / t2, jsb.get_property("velocities/vc-kts")])
		ok = ok and fx2.get_puff_count() > 200
		await _shot(out.path_join("exhaust_%s_trail_side.png" % id), Vector3(120.0, 15.0, -60.0), 60.0, 50.0)
		await _shot(out.path_join("exhaust_%s_trail_chase.png" % id), Vector3(4.0, 6.0, -45.0), 0.0, 60.0)
		await _shot(out.path_join("exhaust_%s_trail_far.png" % id), Vector3(1500.0, -300.0, -2500.0), 2500.0, 50.0)
		await _shot(out.path_join("exhaust_%s_trail_front.png" % id), Vector3(-30.0, 8.0, 60.0), 0.0, 55.0)
	print(("  PASS: " if ok else "  FAIL: ") + "exhaust effects for %s" % id)
	get_tree().quit(0 if ok else 1)
