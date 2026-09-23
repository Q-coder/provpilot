extends Node
## Visual probe of the scenery editor: start a flight, load the design in
## PVP_DESIGN_FILE, enter the editor, put its camera PROBE_AGL metres over
## E/N (PROBE_E / PROBE_N, default the LSZH spawn) looking along PROBE_YAW
## with PROBE_PITCH degrees down, and save PROBE_PNG.
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var ed: SceneryEditor = root._scenery_editor
	var waited := 0.0
	# PROBE_ROADS_DIR: the road manager draws only this baked roads/ directory (a test bake)
	if OS.get_environment("PROBE_ROADS_DIR") != "":
		for rm in get_tree().root.find_children("*", "S3DRoadManager", true, false):
			rm.set_road_paths(PackedStringArray([OS.get_environment("PROBE_ROADS_DIR")]))
			print("probe_scenery_editor: road paths %s" % str(rm.get_road_paths()))
	if OS.get_environment("PROBE_EDIT") == "1":
		# the welcome screen's Edit Mode: no aircraft, no intro zoom
		if root._welcome_screen:
			root._welcome_screen.hide_welcome()
		root._on_welcome_edit_requested(E, N, 155.0)
		root._sky.set_time(false, 14 * 60)
		await get_tree().create_timer(12.0).timeout
	else:
		root._on_welcome_flight_requested("a10", E, N, 155.0)
		while not root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		root._sky.set_time(false, 14 * 60)
		if root._welcome_screen:
			if root._welcome_screen.has_method("hide_welcome"):
				root._welcome_screen.hide_welcome()
			root._welcome_screen.visible = false
		waited = 0.0
		while (root._fly_loading or root._intro_playing) and waited < 90.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(2.0).timeout
		ed.enter()
	if not ed.is_active():
		printerr("probe_scenery_editor: editor refused to start (loading %s, intro %s)" % [root._fly_loading, root._intro_playing])
		get_tree().quit(1)
		return
	var e := float(OS.get_environment("PROBE_E")) if OS.get_environment("PROBE_E") != "" else E
	var n := float(OS.get_environment("PROBE_N")) if OS.get_environment("PROBE_N") != "" else N
	var agl := float(OS.get_environment("PROBE_AGL")) if OS.get_environment("PROBE_AGL") != "" else 250.0
	var yaw := float(OS.get_environment("PROBE_YAW")) if OS.get_environment("PROBE_YAW") != "" else 0.0
	var pitch := float(OS.get_environment("PROBE_PITCH")) if OS.get_environment("PROBE_PITCH") != "" else 45.0
	var h := ed.elevation_lv95(e, n)
	if is_nan(h):
		h = 430.0
	ed._pos = ed._world_of(e, n, h + agl)
	ed._yaw = deg_to_rad(yaw)   # true heading
	ed._pitch = -deg_to_rad(pitch)
	ed._apply_camera()
	for i in range(10):
		ed.update(0.016)
		await get_tree().process_frame
	if OS.get_environment("PROBE_SPACE") == "1":   # press Space: straight down
		var ksp := InputEventKey.new()
		ksp.keycode = KEY_SPACE
		ksp.pressed = true
		Input.parse_input_event(ksp)
		await get_tree().process_frame
		ksp.pressed = false
		Input.parse_input_event(ksp)
	await get_tree().create_timer(4.0).timeout
	if OS.get_environment("PROBE_PICK") == "1":   # pick the baked way nearest to E/N and lift its middle node 6 m
		var rec: Dictionary = ed.way_near(e, n)
		print("probe_scenery_editor: under E %.0f N %.0f: %s" % [e, n, SceneryEditor.way_label(rec)])
		if not rec.is_empty():
			var ids: Array = ed.design.import_way(rec)
			ed.design.set_node_offset(ids[ids.size() / 2], 6.0)
			print("probe_scenery_editor: picked %d nodes, middle node lifted" % ids.size())
	ed._rebuild_live()
	for i in range(30):
		ed.update(0.016)
		await get_tree().process_frame
	if OS.get_environment("PROBE_BAKE") == "1":   # the editor's Bake area: run it and wait for the reload
		var ok := ed.bake_area()
		print("probe_scenery_editor: bake started = %s, note: %s" % [ok, ed._tool_note])
		var waited_b := 0.0
		while ed.bake_running() and waited_b < 300.0:
			ed.update(0.1)
			await get_tree().create_timer(1.0).timeout
			waited_b += 1.0
		ed.update(0.016)
		await get_tree().create_timer(6.0).timeout
		for i in range(30):
			ed.update(0.016)
			await get_tree().process_frame
		print("probe_scenery_editor: after %.0f s: %s | baked = %s" % [waited_b, ed._tool_note, ed.design_is_baked()])
	for ch in ed.design.chains():
		var names: Array = []
		for nid in ch["nodes"]:
			var np: Vector2 = ed.design.node_pos(nid)
			names.append("%d(%+.0f,%+.0f)" % [nid, np.x - E, np.y - N])
		print("  [chain] %s %s: %s" % [ch["attrs"]["kind"], ch["attrs"]["cls"], " ".join(names)])
	for mi in ed.mesher.get_children():
		var aabb: AABB = (mi as MeshInstance3D).get_aabb()
		print("  [mesh] %s verts %d size %.0f x %.0f x %.0f" % [mi.name, ((mi as MeshInstance3D).mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size(), aabb.size.x, aabb.size.y, aabb.size.z])
	print("probe_scenery_editor: design baked = %s" % ed.design_is_baked())
	print("probe_scenery_editor: %d nodes, %d edges, %d meshes, %d stations without terrain" % [ed.design.node_count(), ed.design.edge_count(), ed.mesher.chain_count, ed.mesher.unknown_height_count])
	var png := OS.get_environment("PROBE_PNG")
	if png != "":
		get_viewport().get_texture().get_image().save_png(png)
		print("probe_scenery_editor: screenshot ", png)
	get_tree().quit(0)
