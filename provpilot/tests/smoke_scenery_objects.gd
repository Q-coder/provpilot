extends Node
## Headless smoke: object packs of the scenery library (SceneryObjects).
## Puts the converted Schmerlat pack (scenery/custom/lspf-schmerlat, an area
## with an objects/ folder) at the top of the library, flies to LSPF and
## checks that its models are placed on the precise terrain with the right
## heading and scale, and that the pack's exclusion zones remove the
## swissBUILDINGS3D buildings of the lower-priority Switzerland area.
## The user's library is restored in memory afterwards (nothing is saved).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_scenery_objects.tscn
const E := 2681661.3
const N := 1282697.8
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)


func _ready() -> void:
	var source: String = ProjectSettings.globalize_path("res://").path_join("../scenery/custom/lspf-schmerlat").simplify_path()
	if not FileAccess.file_exists(source.path_join("objects/package.json")):
		print("smoke_scenery_objects: %s has no objects/package.json, skipping" % source)
		get_tree().quit(0)
		return
	# a scratch copy of the pack: package.json copied, the GLBs linked, so the
	# editor's save never touches the real pack
	var area: String = OS.get_user_data_dir().path_join("smoke_lspf_pack")
	DirAccess.make_dir_recursive_absolute(area.path_join("objects"))
	DirAccess.copy_absolute(source.path_join("objects/package.json"), area.path_join("objects/package.json"))
	# plus one object placed at an absolute elevation (elevation_m, no agl_m),
	# the way MSFS photogrammetry packs (Bern) place their blocks
	var pkg_json: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(area.path_join("objects/package.json")))
	for o in pkg_json["objects"]:
		if o["name"] == "schmerlat_tisch_kurz":
			var abs_o: Dictionary = o.duplicate(true)
			abs_o["name"] = "smoke_abs_elev"
			abs_o["position_e"] = float(o["position_e"]) + 30.0
			var abs_ll: Vector2 = Geo.lv95_to_wgs84(float(abs_o["position_e"]), float(abs_o["position_n"]))
			abs_o["lat"] = abs_ll.x
			abs_o["lon"] = abs_ll.y
			abs_o["agl_m"] = null
			abs_o["elevation_m"] = 460.0
			pkg_json["objects"].append(abs_o)
			break
	var pkg_file := FileAccess.open(area.path_join("objects/package.json"), FileAccess.WRITE)
	pkg_file.store_string(JSON.stringify(pkg_json, "\t"))
	pkg_file.close()
	for f in DirAccess.get_files_at(source.path_join("objects")):
		if f.ends_with(".glb") and not FileAccess.file_exists(area.path_join("objects").path_join(f)):
			OS.execute("/bin/ln", ["-sf", source.path_join("objects").path_join(f), area.path_join("objects").path_join(f)])
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var st = root._settings
	var saved: Array = st.get_scenery_areas().duplicate(true)
	var areas: Array = []
	for a in saved:
		# neither the scratch copy nor the user's own entry of the same pack
		if str(a.get("path", "")) != area and str(a.get("path", "")).simplify_path() != source:
			areas.append(a)
	# the Swiss detail area (swissBUILDINGS3D) right below the pack, so its
	# buildings around the airfield are the ones the zones remove
	const SWISS := "/Users/gery/provpilot/scenery/Switzerland"
	if DirAccess.dir_exists_absolute(SWISS.path_join("buildings")):
		var without: Array = []
		for a in areas:
			if str(a.get("path", "")) != SWISS:
				without.append(a)
		areas = without
		areas.push_front({"name": "Switzerland (smoke)", "path": SWISS, "enabled": true, "priority": 1})
	areas.push_front({"name": "lspf-schmerlat (smoke)", "path": area, "enabled": true, "priority": 0})
	st.set_scenery_areas(areas)
	root._apply_scenery_sources()
	var so = root._scenery_objects
	# count the scratch pack's objects only: the library may hold other packs (Kjeller) and the user's own Schmerlat entry
	var in_pack := 0
	for i in range(so.get_object_count()):
		if str(so.get_object_info(i).get("pack", "")) == "lspf-schmerlat (smoke)":
			in_pack += 1
	_check(in_pack == 36, "36 placed objects read from the pack (%d of %d)" % [in_pack, so.get_object_count()])
	_check(so.get_exclusion_zones().size() >= 25, "exclusion zones read from the pack (%d)" % so.get_exclusion_zones().size())
	_check(so.get_excluded_object_count() == 0, "no object of the pack is hidden by its own zones")
	var bm: Node = null
	for n in root.scenery3d_node.find_children("*", "S3DBuildingManager", true, false):
		bm = n
	_check(bm != null and bm.get_exclusion_zones().size() == so.get_exclusion_zones().size(), "building manager holds the zones")
	_check(bm != null and int(bm.get_exclusion_zones()[0]["min_dir"]) == 0, "the zones apply to every buildings dir below the pack (min_dir 0)")
	# fly to LSPF
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	waited = 0.0
	while (root._fly_loading or root._intro_playing) and waited < 20.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	if root._fly_loading or root._intro_playing:
		root._cancel_intro_zoom()
		await get_tree().create_timer(2.0).timeout
		root._intro_playing = false
		root._fly_loading = false
	waited = 0.0
	while so.get_loaded_count() < 30 and waited < 90.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	print("  [diag] %d of %d objects loaded after %.1f s" % [so.get_loaded_count(), so.get_object_count(), waited])
	for i in range(so.get_object_count()):
		var d: Dictionary = so.get_object_info(i)
		if not d["loaded"]:
			print("  [diag] not loaded: %s at E %.0f N %.0f (excluded %s)" % [d["name"], d["e"], d["n"], d["excluded"]])
	_check(so.get_loaded_count() >= 30, "the pack's models around the aircraft are loaded (%d)" % so.get_loaded_count())
	var info: Dictionary = {}
	for i in range(so.get_object_count()):
		var d: Dictionary = so.get_object_info(i)
		if d["name"] == "schmerlat_hauptgebaeude":
			info = d
	var node: Node3D = so.find_object_node("schmerlat_hauptgebaeude")
	_check(not info.is_empty() and node != null, "main building placed")
	if node != null:
		var g: Vector3 = W3DCoords.ecef_to_geodetic(node.global_position)
		var terr: float = root._world.scenery_elevation_geodetic(float(info["lat"]), float(info["lon"]))
		print("  [diag] main building at lat %.6f lon %.6f h %.2f (terrain %.2f + agl %.2f), heading %.1f" % [g.x, g.y, g.z, terr, float(info["agl_m"]), float(info["heading_deg"])])
		_check(absf(g.x - float(info["lat"])) < 2e-6 and absf(g.y - float(info["lon"])) < 3e-6, "placed at the pack's lat/lon")
		_check(not is_nan(terr) and absf(g.z - (terr + float(info["agl_m"]))) < 0.05, "sits on the precise terrain plus its height above ground")
		var ll: Vector2 = Geo.lv95_to_wgs84(float(info["e"]), float(info["n"]))
		var enu: Basis = W3DCoords.enu_to_ecef_transform(ll.x, ll.y, g.z, true).basis
		var f: Vector3 = enu.inverse() * node.global_transform.basis.z.normalized()
		var hdg := fposmod(rad_to_deg(atan2(-f.x, f.z)), 360.0)
		print("  [diag] model +Z points to %.1f deg true" % hdg)
		_check(absf(fposmod(hdg - float(info["heading_deg"]) + 180.0, 360.0) - 180.0) < 0.5, "faces its true heading with +Z (the orthophoto check of probe_scenery_objects)")
		_check(absf(node.global_transform.basis.get_scale().x - float(info["scale_mean"])) < 1e-3, "placement scale applied (%.2f)" % float(info["scale_mean"]))
		var meshes: int = node.find_children("*", "MeshInstance3D", true, false).size()
		_check(meshes == 75, "main building has its 75 meshes (%d)" % meshes)
	# an object placed at an absolute elevation: it sits at that height, reports
	# the matching height above ground, and re-applying its own pose (what a
	# click or a drag does) neither lifts it nor re-creates its node nor
	# restarts its fade-in (a selected Bern block lifted and vanished for a
	# second on every move)
	var abs_idx := -1
	for i in range(so.get_object_count()):
		if so.get_object_info(i)["name"] == "smoke_abs_elev":
			abs_idx = i
	var abs_node: Node3D = so.find_object_node("smoke_abs_elev")
	_check(abs_idx >= 0 and abs_node != null, "the absolute-elevation object is placed")
	if abs_node != null:
		var ag: Vector3 = W3DCoords.ecef_to_geodetic(abs_node.global_position)
		var ad: Dictionary = so.get_object_info(abs_idx)
		var aterr: float = root._world.scenery_elevation_geodetic(float(ad["lat"]), float(ad["lon"]))
		print("  [diag] absolute object at h %.2f, terrain %.2f, reported agl %.2f" % [ag.z, aterr, float(ad["agl_m"])])
		_check(absf(ag.z - 460.0) < 0.05, "it sits at its absolute elevation (%.2f)" % ag.z)
		_check(not is_nan(aterr) and absf(aterr + float(ad["agl_m"]) - 460.0) < 0.05, "it reports the matching height above ground (%.2f)" % float(ad["agl_m"]))
		var abs_mesh: MeshInstance3D = null
		for m in abs_node.find_children("*", "MeshInstance3D", true, false):
			abs_mesh = m
		var fade_before: float = abs_mesh.transparency if abs_mesh else -1.0
		so.set_object_pose(abs_idx, float(ad["e"]), float(ad["n"]), float(ad["agl_m"]), float(ad["heading_deg"]), float(ad["pitch_deg"]), float(ad["bank_deg"]), ad["scale"], false)
		await get_tree().process_frame
		var abs_node2: Node3D = so.find_object_node("smoke_abs_elev")
		var ag2: Vector3 = W3DCoords.ecef_to_geodetic(abs_node2.global_position) if abs_node2 else Vector3(NAN, NAN, NAN)
		_check(abs_node2 == abs_node, "re-applying its pose moves the node instead of re-creating it")
		_check(absf(ag2.z - 460.0) < 0.05, "re-applying its pose does not lift it (%.2f)" % ag2.z)
		_check(abs_mesh != null and fade_before == 0.0 and abs_mesh.transparency == 0.0, "a pose update does not restart the fade-in (transparency %.2f -> %.2f)" % [fade_before, abs_mesh.transparency if abs_mesh else -1.0])
	# a second instance of a repeated model is its own node
	var tables := 0
	for i in range(so.get_object_count()):
		var d: Dictionary = so.get_object_info(i)
		if d["name"] == "schmerlat_tisch_kurz" and d["loaded"]:
			tables += 1
	_check(tables == 10, "ten placed tables are ten instances (%d)" % tables)
	# buildings of the lower-priority Switzerland area vanish under the pack's models
	waited = 0.0
	while bm != null and bm.get_excluded_building_count() == 0 and waited < 40.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	var excluded: int = bm.get_excluded_building_count() if bm else -1
	print("  [diag] %d swissBUILDINGS3D buildings excluded after %.1f s (%d tiles active)" % [excluded, waited, bm.get_active_tile_count() if bm else 0])
	_check(excluded > 0, "buildings inside the exclusion zones are not instantiated")
	var vm: Node = root.vegetation_node
	if vm and vm.has_method("get_excluded_tree_count"):
		print("  [diag] %d trees excluded" % vm.get_excluded_tree_count())
	# the editor's Object tool: pick, move, turn, scale, drop, remove, undo, save
	var ed: SceneryEditor = root._scenery_editor
	ed.enter()
	await get_tree().process_frame
	_check(ed.is_active(), "editor entered for the object tool")
	ed.set_tool(SceneryEditor.TOOL_OBJECT)
	var idx := -1
	if node != null:
		# look at the building from 120 m above its origin and click its centre
		ed.camera_position = ed.world_of(float(info["e"]), float(info["n"]), root._world.scenery_elevation_geodetic(float(info["lat"]), float(info["lon"])) + 120.0)
		ed.yaw = 0.0
		ed.pitch = -PI * 0.5
		ed.apply_camera()
		await get_tree().process_frame
		idx = ed.select_object_at(ed.get_camera().unproject_position(node.global_position + node.global_transform.basis.y.normalized() * 3.0))
	_check(idx >= 0 and so.get_object_info(idx)["name"] == "schmerlat_hauptgebaeude", "clicking the main building selects it (%d)" % idx)
	# a small object next to the long fences: the click must hit its geometry, not a fence's bounding sphere
	var table: Node3D = so.find_object_node("schmerlat_tisch_kurz")
	if table != null:
		var tg: Vector3 = W3DCoords.ecef_to_geodetic(table.global_position)
		var ten: Vector2 = Geo.wgs84_to_lv95(tg.x, tg.y)
		ed.camera_position = ed.world_of(ten.x, ten.y, tg.z + 25.0)
		ed.yaw = 0.0
		ed.pitch = -PI * 0.5
		ed.apply_camera()
		await get_tree().process_frame
		var t_idx := ed.select_object_at(ed.get_camera().unproject_position(table.global_position + table.global_transform.basis.y.normalized() * 0.7))
		var t_name: String = so.get_object_info(t_idx)["name"] if t_idx >= 0 else "none"
		_check(t_name == "schmerlat_tisch_kurz", "clicking a table selects the table, not the fence around it (%s)" % t_name)
		var miss := ed.select_object_at(ed.get_camera().unproject_position(table.global_position) + Vector2(300, 300))
		_check(miss < 0 or so.get_object_info(miss)["name"] != "schmerlat_tisch_kurz", "a click on empty ground next to it does not select the table")
		ed.camera_position = ed.world_of(float(info["e"]), float(info["n"]), root._world.scenery_elevation_geodetic(float(info["lat"]), float(info["lon"])) + 120.0)
		ed.apply_camera()
		await get_tree().process_frame
		idx = ed.select_object_at(ed.get_camera().unproject_position(node.global_position + node.global_transform.basis.y.normalized() * 3.0))
	if idx >= 0:
		var before: Dictionary = so.get_object_info(idx)
		so.set_object_pose(idx, float(before["e"]) + 20.0, float(before["n"]) - 5.0, float(before["agl_m"]) + 1.0, float(before["heading_deg"]) + 10.0, 0.0, 0.0, Vector3(0.6, 0.6, 0.6))
		var after: Dictionary = so.get_object_info(idx)
		var n2: Node3D = after["node"]
		var g2: Vector3 = W3DCoords.ecef_to_geodetic(n2.global_position)
		var en2: Vector2 = Geo.wgs84_to_lv95(g2.x, g2.y)
		var terr2: float = root._world.scenery_elevation_geodetic(g2.x, g2.y)
		_check(en2.distance_to(Vector2(float(before["e"]) + 20.0, float(before["n"]) - 5.0)) < 0.05, "moved 20 m east, 5 m south (%.2f m off)" % en2.distance_to(Vector2(float(before["e"]) + 20.0, float(before["n"]) - 5.0)))
		_check(absf(g2.z - (terr2 + float(before["agl_m"]) + 1.0)) < 0.05, "raised by 1 m over the terrain there")
		_check(absf(n2.global_transform.basis.get_scale().x - 0.6) < 1e-3, "rescaled to 0.6")
		_check(so.has_unsaved_edits(), "the pack is marked unsaved")
		so.drop_to_ground(idx)
		var dropped: Dictionary = so.get_object_info(idx)
		_check(absf(float(dropped["agl_m"]) + float(dropped["lowest_m"])) < 1e-3, "Ctrl+G puts the model's lowest vertex on the terrain (agl %.2f)" % float(dropped["agl_m"]))
		so.remove_object(idx)
		_check(not so.get_object_info(idx)["loaded"] and so.get_object_info(idx)["removed"], "Delete unloads the object and marks it removed")
		_check(so.undo_object_edit() and so.get_object_info(idx)["loaded"], "undo brings it back")
		_check(so.undo_object_edit() and absf(float(so.get_object_info(idx)["agl_m"]) - (float(before["agl_m"]) + 1.0)) < 1e-6, "undo restores the height before G")
		_check(so.undo_object_edit() and absf(float(so.get_object_info(idx)["e"]) - float(before["e"])) < 1e-6 and (so.get_object_info(idx)["scale"] as Vector3).is_equal_approx(before["scale"]), "undo restores the original pose")
		# the model itself: a click leaves it where it is, a drag slides it in its
		# ground plane by the mouse delta, Shift makes the mouse count a tenth
		ed.set_gizmo_mode(SceneryEditor.GIZMO_MOVE)
		ed.update(0.016)
		var start: Dictionary = so.get_object_info(idx)
		var origin_px: Vector2 = ed.get_camera().unproject_position((start["node"] as Node3D).global_position)
		var click := InputEventMouseButton.new()
		click.button_index = MOUSE_BUTTON_LEFT
		click.pressed = true
		click.position = origin_px + Vector2(20, 0)
		ed.mouse = click.position
		ed.handle_mouse(click)
		click.pressed = false
		ed.handle_mouse(click)
		var after_click: Dictionary = so.get_object_info(idx)
		_check(absf(float(after_click["e"]) - float(start["e"])) < 1e-6 and absf(float(after_click["n"]) - float(start["n"])) < 1e-6, "a plain click on the model does not move it")
		click.pressed = true
		ed.handle_mouse(click)
		var slide := InputEventMouseMotion.new()
		slide.position = click.position + Vector2(50, 0)   # camera looks down, north up: +x on screen = east
		ed.handle_mouse(slide)
		click.pressed = false
		click.position = slide.position
		ed.handle_mouse(click)
		var slid: Dictionary = so.get_object_info(idx)
		var px_per_m: float = (ed.get_gizmo_handle(SceneryEditor.AXIS_EAST) - ed.get_camera().unproject_position((slid["node"] as Node3D).global_position)).length() / ed.get_gizmo_size_m()
		print("  [diag] model drag 50 px: %.2f m east, %.2f m north (%.2f px/m at the object)" % [float(slid["e"]) - float(start["e"]), float(slid["n"]) - float(start["n"]), px_per_m])
		_check(absf(float(slid["e"]) - float(start["e"]) - 50.0 / px_per_m) < 0.3 and absf(float(slid["n"]) - float(start["n"])) < 0.3, "dragging the model slides it under the mouse in its ground plane")
		click.pressed = true
		click.position = origin_px + Vector2(20, 0)
		ed.handle_mouse(click)
		var slow := InputEventMouseMotion.new()
		slow.position = click.position + Vector2(50, 0)
		slow.shift_pressed = true
		ed.handle_mouse(slow)
		click.pressed = false
		click.position = slow.position
		ed.handle_mouse(click)
		var slow_d: Dictionary = so.get_object_info(idx)
		_check(absf((float(slow_d["e"]) - float(slid["e"])) - 5.0 / px_per_m) < 0.3, "with Shift the same drag moves a tenth (%.2f m)" % (float(slow_d["e"]) - float(slid["e"])))
		so.undo_object_edit()
		so.undo_object_edit()
		ed.update(0.016)
		start = so.get_object_info(idx)
		origin_px = ed.get_camera().unproject_position((start["node"] as Node3D).global_position)
		var tip_px: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_EAST)
		var axis_dir: Vector2 = (tip_px - origin_px).normalized()
		var m_per_px: float = ed.get_gizmo_size_m() / (tip_px - origin_px).length()
		var press := InputEventMouseButton.new()
		press.button_index = MOUSE_BUTTON_LEFT
		press.pressed = true
		press.position = tip_px - axis_dir * 4.0
		ed.mouse = press.position
		ed.handle_mouse(press)
		var motion := InputEventMouseMotion.new()
		motion.position = press.position + axis_dir * 60.0
		ed.handle_mouse(motion)
		var rel := InputEventMouseButton.new()
		rel.button_index = MOUSE_BUTTON_LEFT
		rel.pressed = false
		rel.position = motion.position
		ed.handle_mouse(rel)
		var dragged: Dictionary = so.get_object_info(idx)
		print("  [diag] east arrow drag: %.2f m expected, %.2f m east %.2f m north" % [60.0 * m_per_px, float(dragged["e"]) - float(start["e"]), float(dragged["n"]) - float(start["n"])])
		_check(absf(float(dragged["e"]) - float(start["e"]) - 60.0 * m_per_px) < 0.5 and absf(float(dragged["n"]) - float(start["n"])) < 0.5, "dragging the east arrow moves the model east by the arrow's scale")
		ed.set_gizmo_mode(SceneryEditor.GIZMO_ROTATE)
		ed.update(0.016)
		var ring_px: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_ROT_UP)
		_check(ed.gizmo_hit_test(ring_px) == SceneryEditor.AXIS_ROT_UP, "the heading ring is picked at its rim")
		press.position = ring_px
		ed.handle_mouse(press)
		var o2: Vector2 = ed.get_camera().unproject_position((dragged["node"] as Node3D).global_position)
		var v: Vector2 = ring_px - o2
		motion.position = o2 + Vector2(-v.y, v.x)   # a quarter turn clockwise on screen (y down)
		ed.handle_mouse(motion)
		rel.position = motion.position
		ed.handle_mouse(rel)
		var turned: Dictionary = so.get_object_info(idx)
		var dh: float = fposmod(float(turned["heading_deg"]) - float(dragged["heading_deg"]) + 180.0, 360.0) - 180.0
		print("  [diag] ring drag: heading changed by %.1f deg, pitch %.2f bank %.2f (camera looks straight down)" % [dh, float(turned["pitch_deg"]), float(turned["bank_deg"])])
		# the pack may already carry a small tilt from the user's own edits: only the change counts
		var tilt_change: float = maxf(absf(float(turned["pitch_deg"]) - float(dragged["pitch_deg"])), absf(float(turned["bank_deg"]) - float(dragged["bank_deg"])))
		_check(absf(dh - 90.0) < 2.0 and tilt_change < 0.05, "a clockwise quarter turn on the heading ring adds 90 deg of heading and no tilt (tilt change %.3f)" % tilt_change)
		# the east ring tilts the model about the east axis: seen edge-on from above it is
		# not pickable, so look at the building from the east, 45 deg down
		ed.camera_position = ed.world_of(float(turned["e"]) + 120.0, float(turned["n"]), root._world.scenery_elevation_geodetic(float(turned["lat"]), float(turned["lon"])) + 120.0)
		ed.yaw = deg_to_rad(270.0)
		ed.pitch = -PI * 0.25
		ed.apply_camera()
		ed.update(0.016)
		var ering_px: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_ROT_EAST)
		_check(ed.gizmo_hit_test(ering_px) == SceneryEditor.AXIS_ROT_EAST, "the east ring is picked at its rim")
		press.position = ering_px
		ed.handle_mouse(press)
		var oe: Vector2 = ed.get_camera().unproject_position((turned["node"] as Node3D).global_position)
		var ve: Vector2 = ering_px - oe
		motion.position = oe + ve.rotated(deg_to_rad(20.0))
		ed.handle_mouse(motion)
		rel.position = motion.position
		ed.handle_mouse(rel)
		var tilted: Dictionary = so.get_object_info(idx)
		var tilt: float = maxf(absf(float(tilted["pitch_deg"])), absf(float(tilted["bank_deg"])))
		print("  [diag] east ring drag: pitch %.2f bank %.2f heading %.1f" % [float(tilted["pitch_deg"]), float(tilted["bank_deg"]), float(tilted["heading_deg"])])
		_check(tilt > 5.0 and tilt < 35.0, "the east ring tilts the model (%.1f deg)" % tilt)
		so.undo_object_edit()
		ed.camera_position = ed.world_of(float(turned["e"]), float(turned["n"]), root._world.scenery_elevation_geodetic(float(turned["lat"]), float(turned["lon"])) + 120.0)
		ed.yaw = 0.0
		ed.pitch = -PI * 0.5
		ed.apply_camera()
		ed.set_gizmo_mode(SceneryEditor.GIZMO_SCALE)
		ed.set_orientation(SceneryEditor.ORIENT_LOCAL)   # cubes along the model's own axes
		ed.update(0.016)
		var cube_px: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_SCALE_X)
		var o3: Vector2 = ed.get_camera().unproject_position((turned["node"] as Node3D).global_position)
		press.position = cube_px
		ed.handle_mouse(press)
		motion.position = o3 + (cube_px - o3) * 1.5
		ed.handle_mouse(motion)
		rel.position = motion.position
		ed.handle_mouse(rel)
		var scaled: Dictionary = so.get_object_info(idx)
		var sr: Vector3 = (scaled["scale"] as Vector3) / (turned["scale"] as Vector3)
		_check(absf(sr.x - 1.5) < 0.05 and absf(sr.y - 1.0) < 1e-3 and absf(sr.z - 1.0) < 1e-3, "dragging the X cube 1.5x further scales that axis 1.5x only (%s)" % str(sr))
		ed.update(0.016)
		var centre_px: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_UNIFORM)
		press.position = centre_px + Vector2(6, 0)
		ed.handle_mouse(press)
		motion.position = centre_px + Vector2(12, 0)
		ed.handle_mouse(motion)
		rel.position = motion.position
		ed.handle_mouse(rel)
		var uni: Dictionary = so.get_object_info(idx)
		var ur: Vector3 = (uni["scale"] as Vector3) / (scaled["scale"] as Vector3)
		_check(absf(ur.x - 2.0) < 0.05 and absf(ur.y - 2.0) < 0.05 and absf(ur.z - 2.0) < 0.05, "dragging the centre scales all axes (%s)" % str(ur))
		ed.set_orientation(SceneryEditor.ORIENT_GLOBAL)
		while so.undo_object_edit():
			pass
		ed.set_gizmo_mode(SceneryEditor.GIZMO_MOVE)
		# multi-selection: two tables, dragged together on the east arrow, undone with one Ctrl+Z
		ed.set_orientation(SceneryEditor.ORIENT_GLOBAL)
		ed.set_gizmo_mode(SceneryEditor.GIZMO_MOVE)
		var ta := -1
		var tb := -1
		for i in range(so.get_object_count()):
			var d: Dictionary = so.get_object_info(i)
			if d["name"] == "schmerlat_tisch_kurz" and d["loaded"]:
				if ta < 0:
					ta = i
				elif tb < 0:
					tb = i
		if ta >= 0 and tb >= 0:
			ed.select_object(ta, false)
			ed.select_object(tb, true)
			_check(ed.get_selection().size() == 2 and ed.get_selected_object() == tb, "Shift-click adds a second object, the last one is active (%s)" % str(ed.get_selection()))
			var pa: Dictionary = so.get_object_info(ta)
			var pb: Dictionary = so.get_object_info(tb)
			var mid := (Vector3(pa["ecef"]) + Vector3(pb["ecef"])) * 0.5
			var gm: Vector3 = W3DCoords.ecef_to_geodetic(mid)
			var men: Vector2 = Geo.wgs84_to_lv95(gm.x, gm.y)
			ed.camera_position = ed.world_of(men.x, men.y, gm.z + 40.0)
			ed.yaw = 0.0
			ed.pitch = -PI * 0.5
			ed.apply_camera()
			ed.update(0.016)
			var opx: Vector2 = ed.get_camera().unproject_position(mid)
			var tpx: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_EAST)
			_check(opx.distance_to(ed.get_camera().unproject_position(ed.get_camera().global_position + (mid - ed.get_camera().global_position))) < 3.0, "the gizmo sits on the median point of the selection")
			var adir: Vector2 = (tpx - opx).normalized()
			var mpp: float = ed.get_gizmo_size_m() / (tpx - opx).length()
			press.position = tpx - adir * 4.0
			ed.mouse = press.position
			ed.handle_mouse(press)
			motion.position = press.position + adir * 40.0
			ed.handle_mouse(motion)
			rel.position = motion.position
			ed.handle_mouse(rel)
			var qa: Dictionary = so.get_object_info(ta)
			var qb: Dictionary = so.get_object_info(tb)
			var da_e: float = float(qa["e"]) - float(pa["e"])
			var db_e: float = float(qb["e"]) - float(pb["e"])
			print("  [diag] group drag: table A %.2f m east, table B %.2f m east (expected %.2f)" % [da_e, db_e, 40.0 * mpp])
			_check(absf(da_e - 40.0 * mpp) < 0.3 and absf(db_e - 40.0 * mpp) < 0.3, "dragging the arrow moves both selected tables together")
			_check(so.undo_object_edit() and absf(float(so.get_object_info(ta)["e"]) - float(pa["e"])) < 1e-6 and absf(float(so.get_object_info(tb)["e"]) - float(pb["e"])) < 1e-6, "one undo restores both")
			# rotating the pair about the median point moves them around it
			ed.set_gizmo_mode(SceneryEditor.GIZMO_ROTATE)
			ed.update(0.016)
			var rpx: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_ROT_UP)
			press.position = rpx
			ed.handle_mouse(press)
			var vv: Vector2 = rpx - opx
			motion.position = opx + Vector2(-vv.y, vv.x)
			ed.handle_mouse(motion)
			rel.position = motion.position
			ed.handle_mouse(rel)
			var ra: Dictionary = so.get_object_info(ta)
			var rb: Dictionary = so.get_object_info(tb)
			print("  [diag] group turn detail: A %.2f m from mid -> %.2f m, B %.2f -> %.2f m; A agl %.2f -> %.2f; mid h %.2f, A h %.2f B h %.2f" % [
				Vector3(pa["ecef"]).distance_to(mid), Vector3(ra["ecef"]).distance_to(mid), Vector3(pb["ecef"]).distance_to(mid), Vector3(rb["ecef"]).distance_to(mid),
				float(pa["agl_m"]), float(ra["agl_m"]), W3DCoords.ecef_to_geodetic(mid).z, W3DCoords.ecef_to_geodetic(pa["ecef"]).z, W3DCoords.ecef_to_geodetic(pb["ecef"]).z])
			var sep_before: float = Vector2(pa["e"], pa["n"]).distance_to(Vector2(pb["e"], pb["n"]))
			var sep_after: float = Vector2(ra["e"], ra["n"]).distance_to(Vector2(so.get_object_info(tb)["e"], so.get_object_info(tb)["n"]))
			var moved_a: float = Vector2(ra["e"], ra["n"]).distance_to(Vector2(pa["e"], pa["n"]))
			print("  [diag] group turn: table A moved %.2f m, separation %.2f -> %.2f m, heading %.1f -> %.1f" % [moved_a, sep_before, sep_after, float(pa["heading_deg"]), float(ra["heading_deg"])])
			_check(moved_a > 0.5 and absf(sep_after - sep_before) < 0.05 and absf(fposmod(float(ra["heading_deg"]) - float(pa["heading_deg"]) + 180.0, 360.0) - 180.0 - 90.0) < 2.0, "a quarter turn about the median point swings both tables around it and turns each by 90 deg")
			so.undo_object_edit()
			ed.set_gizmo_mode(SceneryEditor.GIZMO_MOVE)
		# local orientation: the X arrow follows the active model's own axis
		ed.select_object(idx, false)
		ed.set_orientation(SceneryEditor.ORIENT_LOCAL)
		ed.camera_position = ed.world_of(float(before["e"]), float(before["n"]), root._world.scenery_elevation_geodetic(float(before["lat"]), float(before["lon"])) + 120.0)
		ed.yaw = 0.0
		ed.pitch = -PI * 0.5
		ed.apply_camera()
		ed.update(0.016)
		var s0: Dictionary = so.get_object_info(idx)
		var lo: Vector2 = ed.get_camera().unproject_position(Vector3(s0["ecef"]))
		var lx: Vector2 = ed.get_gizmo_handle(SceneryEditor.AXIS_EAST)
		var ldir: Vector2 = (lx - lo).normalized()
		press.position = lx - ldir * 4.0
		ed.handle_mouse(press)
		motion.position = press.position + ldir * 50.0
		ed.handle_mouse(motion)
		rel.position = motion.position
		ed.handle_mouse(rel)
		var s1: Dictionary = so.get_object_info(idx)
		var dv := Vector2(float(s1["e"]) - float(s0["e"]), float(s1["n"]) - float(s0["n"]))
		var h0 := deg_to_rad(float(s0["heading_deg"]))
		var local_x := Vector2(-cos(h0), sin(h0))   # a glTF model facing north (+Z) has +X pointing west; heading turns it clockwise
		print("  [diag] local X drag: moved %s, model X axis %s" % [str(dv.normalized()), str(local_x)])
		_check(dv.length() > 1.0 and dv.normalized().dot(local_x) > 0.995, "with Local orientation the red arrow moves the model along its own X axis")
		so.undo_object_edit()
		ed.set_orientation(SceneryEditor.ORIENT_GLOBAL)
		# the transform panel: typed values apply to the active object
		# input routing through the real viewport: the Transform panel swallows
		# wheel and clicks, a focused field gets the keys, Enter / Escape hand
		# them back (the welcome overlay would otherwise be the hovered control)
		if root._welcome_screen:
			root._welcome_screen.visible = false
		var tpanel: PanelContainer = null
		for pc in get_tree().root.find_children("*", "PanelContainer", true, false):
			for l in pc.find_children("*", "Label", true, false):
				if l.text == "Transform":
					tpanel = pc
		_check(tpanel != null and tpanel.is_visible_in_tree(), "Transform panel shown in the object tool")
		if tpanel:
			var tp: Vector2 = tpanel.get_global_rect().position + Vector2(40, 12)
			var mmp := InputEventMouseMotion.new(); mmp.position = tp; mmp.global_position = tp
			Input.parse_input_event(mmp); await get_tree().process_frame
			var cam0: Vector3 = ed.camera_position
			var whp := InputEventMouseButton.new(); whp.button_index = MOUSE_BUTTON_WHEEL_UP; whp.pressed = true; whp.position = tp; whp.global_position = tp
			Input.parse_input_event(whp); await get_tree().process_frame
			# the OS pairs every wheel press with a release; without it the
			# viewport keeps a "button held" mask and no later click grabs focus
			whp = InputEventMouseButton.new(); whp.button_index = MOUSE_BUTTON_WHEEL_UP; whp.pressed = false; whp.position = tp; whp.global_position = tp
			Input.parse_input_event(whp); await get_tree().process_frame
			_check(ed.camera_position.distance_to(cam0) < 0.01, "wheel over the panel does not zoom the camera")
			var sel_before: int = ed.get_selected_object()
			var mbp := InputEventMouseButton.new(); mbp.button_index = MOUSE_BUTTON_LEFT; mbp.pressed = true; mbp.position = tp; mbp.global_position = tp
			Input.parse_input_event(mbp); await get_tree().process_frame
			mbp = InputEventMouseButton.new(); mbp.button_index = MOUSE_BUTTON_LEFT; mbp.pressed = false; mbp.position = tp; mbp.global_position = tp
			Input.parse_input_event(mbp); await get_tree().process_frame
			_check(ed.get_selected_object() == sel_before, "a click on the panel does not reach the scene")
			var sp0: SpinBox = tpanel.find_children("*", "SpinBox", true, false)[0]
			var le0: LineEdit = sp0.get_line_edit()
			var lp0: Vector2 = le0.get_global_rect().get_center()
			mmp = InputEventMouseMotion.new(); mmp.position = lp0; mmp.global_position = lp0
			Input.parse_input_event(mmp); await get_tree().process_frame
			mbp = InputEventMouseButton.new(); mbp.button_index = MOUSE_BUTTON_LEFT; mbp.pressed = true; mbp.position = lp0; mbp.global_position = lp0
			Input.parse_input_event(mbp); await get_tree().process_frame
			mbp = InputEventMouseButton.new(); mbp.button_index = MOUSE_BUTTON_LEFT; mbp.pressed = false; mbp.position = lp0; mbp.global_position = lp0
			Input.parse_input_event(mbp); await get_tree().process_frame
			_check(le0.has_focus(), "a click in the E field focuses it")
			var tool_before: int = ed.get_tool()
			# caret to the end first: the click lands mid-number, and a digit
			# inserted there would send the object thousands of km east
			var kend := InputEventKey.new(); kend.keycode = KEY_END; kend.pressed = true
			Input.parse_input_event(kend); await get_tree().process_frame
			kend = InputEventKey.new(); kend.keycode = KEY_END; kend.pressed = false
			Input.parse_input_event(kend); await get_tree().process_frame
			var text_before: String = le0.text
			var k5 := InputEventKey.new(); k5.keycode = KEY_5; k5.unicode = 53; k5.pressed = true
			Input.parse_input_event(k5); await get_tree().process_frame
			k5 = InputEventKey.new(); k5.keycode = KEY_5; k5.unicode = 53; k5.pressed = false
			Input.parse_input_event(k5); await get_tree().process_frame
			_check(ed.get_tool() == tool_before and le0.text == text_before + "5", "typing 5 into the field edits the text, not the tool ('%s')" % le0.text)
			var kent := InputEventKey.new(); kent.keycode = KEY_ENTER; kent.pressed = true
			Input.parse_input_event(kent); await get_tree().process_frame
			kent = InputEventKey.new(); kent.keycode = KEY_ENTER; kent.pressed = false
			Input.parse_input_event(kent); await get_tree().process_frame
			await get_tree().process_frame
			_check(absf(float(so.get_object_info(idx)["e"]) - sp0.value) < 0.02 and get_viewport().gui_get_focus_owner() == null, "Enter applies the typed easting (%.2f) and hands the keys back" % sp0.value)
			le0.grab_focus(); await get_tree().process_frame
			var kesc := InputEventKey.new(); kesc.keycode = KEY_ESCAPE; kesc.pressed = true
			Input.parse_input_event(kesc); await get_tree().process_frame
			_check(get_viewport().gui_get_focus_owner() == null, "Escape leaves the field")
			var cen: Vector2 = Vector2(get_window().size) * 0.5
			mmp = InputEventMouseMotion.new(); mmp.position = cen; mmp.global_position = cen
			Input.parse_input_event(mmp); await get_tree().process_frame
		ed.set_transform_field(0, 2, 1.5)
		_check(absf(float(so.get_object_info(idx)["agl_m"]) - 1.5) < 1e-6, "typing a height in the panel applies it")
		ed.set_transform_field(1, 0, 90.0)
		_check(absf(float(so.get_object_info(idx)["heading_deg"]) - 90.0) < 1e-6, "typing a heading applies it")
		var bs: Vector3 = so.get_object_info(idx)["bounds_size"]
		ed.set_transform_field(3, 0, 20.0)
		_check(absf(float((so.get_object_info(idx)["scale"] as Vector3).x) - 20.0 / bs.x) < 1e-6, "typing a dimension sets the scale of that axis (%.3f)" % float((so.get_object_info(idx)["scale"] as Vector3).x))
		so.undo_object_edit()
		so.undo_object_edit()
		so.undo_object_edit()
		# the pivot: moving it keeps the model in place and is saved
		var node_pos_before: Vector3 = (so.get_object_info(idx)["node"] as Node3D).global_position
		so.set_object_pivot(idx, Vector3(10.0, 0.0, 0.0))
		var node_pos_after: Vector3 = (so.get_object_info(idx)["node"] as Node3D).global_position
		print("  [diag] pivot moved 10 m along X: model moved %.3f m, position moved %.2f m" % [node_pos_before.distance_to(node_pos_after), Vector2(so.get_object_info(idx)["e"], so.get_object_info(idx)["n"]).distance_to(Vector2(before["e"], before["n"]))])
		_check(node_pos_before.distance_to(node_pos_after) < 0.01 and Vector2(so.get_object_info(idx)["e"], so.get_object_info(idx)["n"]).distance_to(Vector2(before["e"], before["n"])) > 4.0, "changing the pivot moves the position, not the model")
		_check((so.get_object_info(idx)["pivot"] as Vector3).is_equal_approx(Vector3(10, 0, 0)), "the pivot is stored")
		so.undo_object_edit()
		_check((so.get_object_info(idx)["pivot"] as Vector3).is_equal_approx(Vector3()), "undo restores the pivot")
		# a lasting edit, saved into the (scratch) pack
		so.set_object_pose(idx, float(before["e"]) + 2.0, float(before["n"]), 0.5, float(before["heading_deg"]), 0.0, 0.0, before["scale"])
		_check(so.save_packs() == 1, "Ctrl+S writes the edited pack")
		var saved_pkg: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(area.path_join("objects/package.json")))
		var entry: Dictionary = {}
		for o in saved_pkg["objects"]:
			if o["name"] == "schmerlat_hauptgebaeude":
				entry = o
		_check(not entry.is_empty() and absf(float(entry["position_e"]) - (float(before["e"]) + 2.0)) < 0.02 and absf(float(entry["agl_m"]) - 0.5) < 1e-6, "package.json carries the new position and height")
		var zone: Dictionary = {}
		for z in saved_pkg["exclusions"]:
			if z.get("object", "") == "schmerlat_hauptgebaeude":
				zone = z
		# the zone is the footprint rectangle (polygon) once the model's bounds are known, a disc before:
		# it must cover the moved placement point and not a point 300 m away
		var at := Vector2(float(before["e"]) + 2.0, float(before["n"]))
		var away := at + Vector2(300.0, 0.0)
		var covers_at := false
		var covers_away := false
		if not zone.is_empty() and zone.has("polygon"):
			var poly := PackedVector2Array()
			for pt in zone["polygon"]:
				poly.push_back(Vector2(float(pt[0]), float(pt[1])))
			covers_at = Geometry2D.is_point_in_polygon(at, poly)
			covers_away = Geometry2D.is_point_in_polygon(away, poly)
		elif not zone.is_empty():
			var c := Vector2(float(zone["circle"][0]), float(zone["circle"][1]))
			covers_at = c.distance_to(at) <= float(zone["circle"][2])
			covers_away = c.distance_to(away) <= float(zone["circle"][2])
		_check(covers_at and not covers_away, "its exclusion zone follows the object (%s)" % ("polygon" if zone.has("polygon") else "circle"))
		_check(saved_pkg["objects"].size() == 36 and saved_pkg.has("unresolved_library_objects"), "the other objects and the pack's other fields are kept")
		var abs_entry: Dictionary = {}
		for o in saved_pkg["objects"]:
			if o["name"] == "smoke_abs_elev":
				abs_entry = o
		_check(not abs_entry.is_empty() and abs_entry["elevation_m"] == null and absf(float(abs_entry["agl_m"]) - (460.0 - root._world.scenery_elevation_geodetic(float(abs_entry["lat"]), float(abs_entry["lon"])))) < 0.05, "the absolute elevation is saved as a height above ground")
	ed.exit()
	await get_tree().process_frame
	# restore the library (in memory only; nothing was saved)
	st.set_scenery_areas(saved)
	root._apply_scenery_sources()
	await get_tree().process_frame
	var scratch_left := 0
	for i in range(so.get_object_count()):
		if str(so.get_object_info(i).get("pack", "")) == "lspf-schmerlat (smoke)":
			scratch_left += 1
	_check(scratch_left == 0, "removing the scratch area drops its objects (%d of the scratch pack left, %d in the library's own packs)" % [scratch_left, so.get_object_count()])
	if _fails.is_empty():
		print("smoke_scenery_objects: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_scenery_objects: %d failure(s)" % _fails.size())
		get_tree().quit(1)
