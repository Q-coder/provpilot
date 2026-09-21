extends Node
## Real-renderer probe: parks the A-10 at LSZH at night with the landing
## lights on and screenshots it from an external camera. Env: PROBE_PNG
## (output), PROBE_HOUR (local hour, default 22), PROBE_DIST, PROBE_PITCH.
##   PROBE_PNG=/tmp/a10.png godot/bin/godot.macos.editor.double.arm64 --path provpilot --resolution 1400x900 res://tests/probe_aircraft_lights.tscn
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var aircraft := OS.get_environment("PROBE_AIRCRAFT")
	root._on_welcome_flight_requested(aircraft if aircraft != "" else "a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	var hour := float(OS.get_environment("PROBE_HOUR")) if OS.get_environment("PROBE_HOUR") != "" else 22.0
	root._sky.set_time(false, int(hour * 60.0))
	# PROBE_ALT_FT / PROBE_SETTING: force the altimeter's values (drum checks)
	if root._fg_controls and OS.get_environment("PROBE_ALT_FT") != "":
		root._fg_controls.state["instrumentation/altimeter/indicated-altitude-ft"] = float(OS.get_environment("PROBE_ALT_FT"))
	if root._fg_controls and OS.get_environment("PROBE_SETTING") != "":
		root._fg_controls.state["instrumentation/altimeter/setting-inhg"] = float(OS.get_environment("PROBE_SETTING"))
	await get_tree().create_timer(4.0).timeout
	var L: AircraftLights = root._aircraft_lights
	if L:
		L.set_landing_lights(true)
		L.debug_all_on = true
	# the host builds airports a few per second in catalog order; force LSZH now
	if root._runways and root._runways_merged.has("LSZH"):
		root._runways._build_airport("LSZH", root._runways_merged["LSZH"], Callable(root, "_terrain_elevation_geodetic"))
	# PROBE_GENS_OFF=1: generators off -> generator cautions and master caution lit
	if OS.get_environment("PROBE_GENS_OFF") == "1":
		root._fg_controls.state["controls/electric/engine/generator"] = 0.0
		root._fg_controls.state["controls/electric/engine[1]/generator"] = 0.0
	# the welcome overlay stays up when the flight is requested from a script,
	# and the intro zoom would keep its own camera
	root._cancel_intro_zoom()
	if root._welcome_screen:
		if root._welcome_screen.has_method("hide_welcome"):
			root._welcome_screen.hide_welcome()
		root._welcome_screen.visible = false
	# The C++ camera rig drives the external (orbit) camera every frame, so
	# pin its distance and pitch through the rig's clamps instead of adding a
	# camera of our own. PROBE_DIST (AC units = 10 m each, default 1.6), PROBE_PITCH (deg, 10).
	var dist := float(OS.get_environment("PROBE_DIST")) if OS.get_environment("PROBE_DIST") != "" else 1.6
	var pitch := float(OS.get_environment("PROBE_PITCH")) if OS.get_environment("PROBE_PITCH") != "" else 10.0
	root._camera_rig.set_orbit_distance_min(dist)
	root._camera_rig.set_orbit_distance_max(dist)
	root._camera_rig.set_orbit_pitch_min(pitch)
	root._camera_rig.set_orbit_pitch_max(pitch)
	# PROBE_COCKPIT=1: the pilot's view instead of the orbit camera
	if OS.get_environment("PROBE_COCKPIT") == "1":
		root._camera_rig.set_mode(0)
		root.camera_mode = 0
		root._camera_rig.set_cockpit_enabled(true)
	elif OS.get_environment("PROBE_COCKPIT") == "3":
		# close-up of a cockpit part: PROBE_LOOK_AT=<node name>, 0.45 m in front of it seen from the eye point
		root._camera_rig.set_mode(0)
		root.camera_mode = 0
		root._camera_rig.set_cockpit_enabled(false)
		root.camera1.current = true
		var part: MeshInstance3D = root.get_node(NodePath(root._model_root_path)).find_child(OS.get_environment("PROBE_LOOK_AT"), true, false)
		if part:
			var target: Vector3 = part.global_transform * part.get_aabb().get_center()
			var eye: Vector3 = root.camera1.global_position
			var dir: Vector3 = (target - eye).normalized()
			var pos: Vector3 = target - dir * 0.45
			var up: Vector3 = root.get_node("AC").global_transform.basis.y.normalized()
			var right: Vector3 = dir.cross(up).normalized()
			var cup: Vector3 = right.cross(dir).normalized()
			root.camera1.global_transform = Transform3D(Basis(right, cup, -dir), pos)
			root.camera1.fov = 40.0
	elif OS.get_environment("PROBE_COCKPIT") == "2":
		# pilot looking down at the instrument panel (rig's head look disabled)
		root._camera_rig.set_mode(0)
		root.camera_mode = 0
		root._camera_rig.set_cockpit_enabled(false)
		root.camera1.current = true
		root.camera1.transform = root.camera1.transform.rotated_local(Vector3(1, 0, 0), deg_to_rad(-35.0))
		root.camera1.fov = 75.0
	var ac: Node3D = root.get_node("AC")
	for i in range(10):
		await get_tree().process_frame
	# where each fixture lands on screen (to match dots in the screenshot)
	var vcam: Camera3D = get_viewport().get_camera_3d()
	if L and vcam:
		for f in L._fixtures:
			var gp: Vector3 = f.sprite.global_transform * f.sprite.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0]
			var sp: Vector2 = vcam.unproject_position(gp)
			var ac_p: Vector3 = ac.global_transform.affine_inverse() * gp
			print("probe_aircraft_lights: %-9s screen (%.0f, %.0f) AC (%.2f, %.2f, %.2f) behind=%s" % [f.kind, sp.x, sp.y, ac_p.x, ac_p.y, ac_p.z, vcam.is_position_behind(gp)])
	# frame rate in this view: average over 120 frames after settling
	var t0 := Time.get_ticks_usec()
	for i in range(120):
		await get_tree().process_frame
	var ms: float = (Time.get_ticks_usec() - t0) / 120.0 / 1000.0
	print("probe_aircraft_lights: %.1f ms per frame (%.0f fps), %d draw calls, %d primitives" % [ms, 1000.0 / maxf(ms, 0.001), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)])
	var png := OS.get_environment("PROBE_PNG")
	if png != "":
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(png)
		print("probe_aircraft_lights: screenshot ", png)
	if L:
		print("probe_aircraft_lights: fixtures %d, landing %s, nav %s, strobe lit now %s" % [L.fixture_count(), L.is_lit("landing"), L.is_lit("nav_left"), L.is_lit("strobe")])
	get_tree().quit(0)
