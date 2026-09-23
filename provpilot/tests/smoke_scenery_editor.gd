extends Node
## Scenery editor (F8, scenery_editor.gd): enter the mode at LSZH, place a
## road and a railway through the design API at cursor-like points, check
## the live meshes land on the terrain at the right place, save / reload
## the design file, and exit back to the aircraft camera.
## Run: PVP_DESIGN_FILE=/tmp/x.json godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_scenery_editor.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

## After a terrain-patch reload the precise terrain is gone for a moment:
## wait for the point, the design's whole box, and an idle streamer.
func _wait_terrain(ed: SceneryEditor, e: float, n: float) -> void:
	var waited := 0.0
	var root: Node = ed.host
	while waited < 60.0:
		var ok: bool = not is_nan(ed.elevation_lv95_precise(e, n))
		for dx in [-1200.0, 0.0, 1300.0]:
			for dy in [-900.0, 0.0, 700.0]:
				if is_nan(ed.elevation_lv95_precise(E + dx, N + dy)):
					ok = false
		if ok and not root._scenery_streamer_busy():
			break
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().process_frame


func _ready() -> void:
	var design_file: String = OS.get_environment("PVP_DESIGN_FILE")
	if design_file == "":
		design_file = OS.get_user_data_dir().path_join("smoke_scenery_design.json")
		OS.set_environment("PVP_DESIGN_FILE", design_file)
	if FileAccess.file_exists(design_file):
		DirAccess.remove_absolute(design_file)
	get_window().size = Vector2i(1400, 900)   # headless default is 64 x 64: screen-space picks need pixels
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
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
		# headless: the intro zoom never renders to completion; end the
		# loading phase the way the Space key does, then force it if needed
		root._cancel_intro_zoom()
		await get_tree().create_timer(2.0).timeout
		root._intro_playing = false
		root._fly_loading = false
		print("  [diag] loading phase forced to an end for the headless run")
	await get_tree().create_timer(2.0).timeout
	var ed: SceneryEditor = root._scenery_editor
	_check(ed != null and not ed.is_active(), "editor present and idle")
	_check(ed.design_path() == design_file, "design path honours PVP_DESIGN_FILE (%s)" % ed.design_path())
	# without the override: a fixed place under the install root, never a scenery area
	OS.set_environment("PVP_DESIGN_FILE", "")
	var default_path := ed.design_path()
	OS.set_environment("PVP_DESIGN_FILE", design_file)
	var ir: Node = get_tree().root.get_node_or_null("InstallRoot")
	var install_root: String = str(ir.get_install_root()) if ir else ""
	print("  [diag] default design path %s (install root %s)" % [default_path, install_root])
	_check(install_root != "" and default_path == install_root.path_join("scenery/Design/design/scenery_design.json"), "default design path is <install root>/scenery/Design/design/scenery_design.json")
	var flight_cam: Camera3D = get_viewport().get_camera_3d()
	ed.enter()
	await get_tree().process_frame
	_check(ed.is_active(), "F8 mode entered")
	_check(get_viewport().get_camera_3d() == ed._cam, "editor camera is the viewport camera")
	_check(root.jsb_node == null or root.jsb_node.is_slew_mode(), "flight model frozen (slew) while editing")
	# cursor: ray through the screen centre must hit terrain near the airport
	for i in range(3):
		ed.update(0.016)
		await get_tree().process_frame
	var hit: Dictionary = ed.cursor_hit_from_screen(get_viewport().get_visible_rect().size * 0.5)
	_check(not hit.is_empty(), "screen-centre ray hits the terrain")
	if not hit.is_empty():
		var d: float = Vector2(hit["e"], hit["n"]).distance_to(Vector2(E, N))
		var terr: float = ed.elevation_lv95(hit["e"], hit["n"])
		print("  [diag] cursor E %.0f N %.0f h %.1f (terrain %.1f), %.0f m from the spawn" % [hit["e"], hit["n"], hit["h"], terr, d])
		_check(d < 2000.0, "cursor lands within 2 km of the aircraft")
		_check(absf(hit["h"] - terr) < 1.0, "cursor height matches the terrain there")
	# frame axes the mesher and camera rely on: local +Z = north, -X = east
	var ll0: Vector2 = Geo.lv95_to_wgs84(E, N)
	var fr: Transform3D = W3DCoords.enu_to_ecef_transform(ll0.x, ll0.y, 430.0, true)
	var east_pt: Vector3 = fr.affine_inverse() * ed._world_of(E + 100.0, N, 430.0)
	var north_pt: Vector3 = fr.affine_inverse() * ed._world_of(E, N + 100.0, 430.0)
	print("  [diag] local of +100 m east %s, +100 m north %s" % [str(east_pt), str(north_pt)])
	# (grid east differs from true east by the LV95 convergence, ~0.8 deg here)
	_check(east_pt.x < -99.0 and absf(east_pt.z) < 3.0 and north_pt.z > 99.0 and absf(north_pt.x) < 3.0, "local ENU frame: +Z north, -X east")
	# camera heading: at heading 90 the camera looks east
	ed._yaw = deg_to_rad(90.0)
	ed._pitch = 0.0
	ed._apply_camera()
	var look: Vector3 = fr.affine_inverse().basis * (-ed._cam.global_transform.basis.z)
	print("  [diag] camera forward at heading 90 in local axes %s" % str(look))
	_check(look.x < -0.99, "camera at heading 90 looks east")
	ed._pitch = -0.7
	ed._apply_camera()
	# views: Space = straight down, the gizmo's axis views, orbit keeps the focus
	ed._yaw = deg_to_rad(45.0)
	ed._pitch = -0.4
	ed._apply_camera()
	var ksp := InputEventKey.new()
	ksp.keycode = KEY_SPACE
	ksp.pressed = true
	Input.parse_input_event(ksp)
	await get_tree().process_frame
	ksp.pressed = false
	Input.parse_input_event(ksp)
	await get_tree().process_frame
	_check(absf(ed._pitch + PI * 0.5) < 0.01 and absf(ed._yaw - deg_to_rad(45.0)) < 0.01, "Space looks straight down and keeps the heading")
	ed.view_top(true)
	_check(absf(ed._yaw) < 0.01, "Shift+Space also turns north up")
	ed.view_axis("x")
	_check(absf(ed._yaw - deg_to_rad(270.0)) < 0.01 and ed._pitch < 0.0 and ed._pitch > -1.0, "gizmo X = view from the east looking west, slightly down")
	ed.view_axis("-y")
	_check(absf(ed._yaw) < 0.01, "gizmo -Y = view from the south looking north")
	ed.view_axis("z")
	var focus_before: Dictionary = ed._focus()
	ed.orbit(120.0, 0.0)
	var focus_after: Dictionary = ed._focus()
	if not focus_before.is_empty() and not focus_after.is_empty():
		var moved: float = (focus_before["world"] as Vector3).distance_to(focus_after["world"])
		print("  [diag] orbit: focus moved %.1f m, distance %.1f -> %.1f" % [moved, float(focus_before["dist"]), float(focus_after["dist"])])
		_check(moved < 15.0 and absf(float(focus_before["dist"]) - float(focus_after["dist"])) < 5.0, "orbiting keeps the screen-centre point and its distance")
	else:
		_check(false, "focus point found before and after orbiting")
	ed._gizmo.queue_redraw()
	await get_tree().process_frame
	_check(ed._gizmo._hit_boxes.size() == 6 and ed._gizmo.pick(ed._gizmo._hit_boxes[0][1]) == ed._gizmo._hit_boxes[0][0], "gizmo draws six knobs and picks the one under the mouse")
	ed._pitch = -0.7
	ed._apply_camera()
	# design: a two-lane road with a bend and a railway crossing it
	var d0: SceneryDesign = ed.design
	d0.snapshot()
	var a := d0.add_node(E + 300.0, N - 200.0)
	var b := d0.add_node(E + 700.0, N - 200.0)
	var c := d0.add_node(E + 900.0, N + 100.0)
	var road := SceneryDesign.road_attrs("secondary", 2, false)
	_check(d0.add_edge(a, b, road) > 0 and d0.add_edge(b, c, road) > 0, "road edges added")
	_check(d0.add_edge(a, b, road) < 0, "duplicate edge refused")
	var r1 := d0.add_node(E + 500.0, N - 400.0)
	var r2 := d0.add_node(E + 500.0, N + 100.0)
	_check(d0.add_edge(r1, r2, SceneryDesign.rail_attrs("rail", 1435)) > 0, "rail edge added")
	# crossing: split the road where the rail crosses it, join both
	var near := d0.edge_near(E + 500.0, N - 200.0, 1.0)
	_check(not near.is_empty(), "edge_near finds the road under the crossing")
	var x := d0.split_edge(int(near["id"]), E + 500.0, N - 200.0)
	_check(x > 0 and d0.degree(x) == 2, "road split at the crossing (node degree 2)")
	var rn := d0.edge_near(E + 500.0, N - 200.0, 1.0)
	var rail_edge := -1
	for eid in d0.edges:
		if d0.edges[eid]["kind"] == "rail":
			rail_edge = eid
	var y := d0.split_edge(rail_edge, E + 500.0, N - 200.0)
	_check(y > 0, "rail split at the crossing")
	var chains: Array = d0.chains()
	var road_chains := 0
	var rail_chains := 0
	for ch in chains:
		if ch["attrs"]["kind"] == "road":
			road_chains += 1
		else:
			rail_chains += 1
	print("  [diag] chains: %d road, %d rail; nodes %d edges %d" % [road_chains, rail_chains, d0.node_count(), d0.edge_count()])
	_check(road_chains == 1 and rail_chains == 1, "chains join equal segments through degree-2 nodes (1 road, 1 rail)")
	# a ring of four one-way segments is one closed chain of five node entries
	var q: Array = [d0.add_node(E - 500.0, N - 500.0), d0.add_node(E - 400.0, N - 500.0), d0.add_node(E - 400.0, N - 400.0), d0.add_node(E - 500.0, N - 400.0)]
	var ring := SceneryDesign.road_attrs("secondary", 1, true)
	for i in range(4):
		d0.add_edge(q[i], q[(i + 1) % 4], ring)
	var ring_chain: Dictionary = {}
	for ch in d0.chains():
		if ch["attrs"]["oneway"]:
			ring_chain = ch
	_check(not ring_chain.is_empty() and ring_chain["edges"].size() == 4 and ring_chain["nodes"].size() == 5 and ring_chain["nodes"][0] == ring_chain["nodes"][4], "a ring meshes as one closed chain")
	# curves: a tangent arc continuing a straight, a roundabout, split of an arc
	var t_out := d0.node_tangent_out(c)          # node c ends the road chain (degree 1)
	_check(t_out.distance_to(Vector2(200.0, 300.0).normalized()) < 0.01, "tangent out of the last node follows the last segment")
	var q_end := Vector2(E + 1100.0, N + 300.0)
	var via := SceneryDesign.tangent_arc_via(d0.node_pos(c), t_out, q_end)
	_check(not via.is_empty() and float(via["radius"]) > 50.0, "tangent arc to a point off the tangent line has a radius (%.0f m)" % (float(via["radius"]) if not via.is_empty() else 0.0))
	var arc_pts := SceneryDesign.arc_points(d0.node_pos(c), via["via"], q_end)
	var first_dir: Vector2 = (arc_pts[1] - arc_pts[0]).normalized()
	_check(arc_pts.size() >= 5 and first_dir.distance_to(t_out) < 0.1 and arc_pts[-1] == q_end, "arc leaves along the tangent, is sampled and ends exactly at the target")
	var cn := d0.add_node(q_end.x, q_end.y)
	var arc_edge := d0.add_edge(c, cn, road)
	d0.set_edge_via(arc_edge, via["via"])
	_check(d0.is_arc(arc_edge) and d0.edge_points(arc_edge).size() >= 5, "arc edge stored with its via")
	var near_arc := d0.edge_near(arc_pts[arc_pts.size() / 2].x, arc_pts[arc_pts.size() / 2].y, 1.0)
	_check(not near_arc.is_empty() and int(near_arc["id"]) == arc_edge, "edge_near follows the arc, not its chord")
	var mid_node := d0.split_edge(arc_edge, arc_pts[arc_pts.size() / 2].x, arc_pts[arc_pts.size() / 2].y)
	var halves := d0.edges_of(mid_node)
	_check(halves.size() == 2 and d0.is_arc(halves[0]) and d0.is_arc(halves[1]), "splitting an arc gives two arcs")
	var ring_ids := d0.add_circle(Vector2(E - 800.0, N), 25.0, SceneryDesign.road_attrs("secondary", 1, true))
	var circle_chain: Dictionary = {}
	for ch in d0.chains():
		if ch["nodes"][0] in ring_ids:
			circle_chain = ch
	_check(ring_ids.size() == 8 and not circle_chain.is_empty() and circle_chain["edges"].size() == 8 and circle_chain["nodes"][0] == circle_chain["nodes"][8], "circle tool makes a closed ring of eight arcs")
	var ring_poly := SceneryDesignMesher.chain_polyline(d0, circle_chain)
	var max_dev := 0.0
	for pnt in ring_poly:
		max_dev = maxf(max_dev, absf((pnt as Vector2).distance_to(Vector2(E - 800.0, N)) - 25.0))
	_check(ring_poly.size() >= 33 and max_dev < 0.05, "ring polyline stays on the circle (max deviation %.3f m over %d points)" % [max_dev, ring_poly.size()])
	# junction: a third road at node b makes a T; its legs are cut back to the pavement
	var side := d0.add_node(E + 700.0, N - 450.0)
	d0.add_edge(b, side, SceneryDesign.road_attrs("tertiary", 2, false))
	await _wait_terrain(ed, E + 700.0, N - 450.0)
	ed.mesher.rebuild(d0)
	_check(ed.mesher.junction_count == 1, "one junction pavement at the T (%d)" % ed.mesher.junction_count)
	var jn: Node = ed.mesher.get_node_or_null("Junction_%d" % b)
	_check(jn != null, "junction mesh named after its node")
	var cuts: Dictionary = ed.mesher._cuts
	var leg_cut: float = float(cuts.get(d0.edges_of(side)[0], {}).get(b, 0.0))
	print("  [diag] T-junction leg cut %.2f m (primary half width %.2f)" % [leg_cut, SceneryDesignMesher.edge_width(d0, d0.edges_of(a)[0]) * 0.5])
	_check(leg_cut > 3.0, "side leg cut back beyond the main road's half width")
	# the side leg's first station lies at the cut, not at the node
	var side_mi: MeshInstance3D = null
	for mi in ed.mesher.get_children():
		if mi.has_meta("edges") and d0.edges_of(side)[0] in mi.get_meta("edges"):
			side_mi = mi
	_check(side_mi != null, "side leg mesh present")
	if side_mi == null and ed.mesher.get_child_count() > 0:
		side_mi = ed.mesher.get_children()[0]
	if side_mi == null:
		_check(false, "no mesh to measure the side leg on")
		side_mi = MeshInstance3D.new()
		side_mi.mesh = BoxMesh.new()
	var sv0: Vector3 = side_mi.global_transform * ((side_mi.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_VERTEX] as PackedVector3Array)[0]
	var sg0: Vector3 = W3DCoords.ecef_to_geodetic(sv0)
	var sen0: Vector2 = Geo.wgs84_to_lv95(sg0.x, sg0.y)
	var dist_from_node: float = sen0.distance_to(d0.node_pos(b))
	print("  [diag] side leg first vertex %.2f m from the junction node" % dist_from_node)
	_check(dist_from_node > 3.0, "side leg ribbon starts at the pavement edge")
	# lane taper: a one-lane link continues the secondary chain at node cn,
	# so the secondary (half 3.5 m) narrows to 1.95 m over its last stations
	var wide_edge: int = d0.edges_of(cn)[0]
	var nn := d0.add_node(E + 1300.0, N + 300.0)
	d0.add_edge(cn, nn, SceneryDesign.road_attrs("tertiary_link", 1, true))
	ed.mesher.rebuild(d0)
	var wide_mi: MeshInstance3D = null
	for mi in ed.mesher.get_children():
		if mi.has_meta("edges") and wide_edge in mi.get_meta("edges"):
			wide_mi = mi
	var uv2s: PackedVector2Array = (wide_mi.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_TEX_UV2]
	var w_first: float = uv2s[0].x
	var w_last: float = uv2s[uv2s.size() - 1].x
	print("  [diag] secondary chain half width first %.2f last %.2f" % [w_first, w_last])
	_check(absf(w_first - 3.5) < 0.05 and absf(w_last - 1.95) < 0.05, "secondary tapers to the one-lane link's half width at the lane change")
	# switch: a turnout on the rail south of the crossing, diverging east
	var track_edge := -1
	for eid in d0.edges_of(r1):
		track_edge = eid
	var refused := d0.add_switch(track_edge, E + 500.0, N - 330.0, Vector2(E + 520.0, N - 300.0))
	_check(refused.has("error"), "a turnout with a radius below %.0f m is refused (%s)" % [SceneryDesign.SWITCH_MIN_RADIUS_M, str(refused.get("error", ""))])
	var sw := d0.add_switch(track_edge, E + 500.0, N - 330.0, Vector2(E + 540.0, N - 60.0))
	_check(not sw.has("error") and d0.degree(int(sw["node"])) == 3, "turnout placed: switch node joins three tracks (%s)" % str(sw))
	if not sw.has("error"):
		var div_pts: Array = d0.edge_points(int(sw["edge"]))
		var leave: Vector2 = (div_pts[1] - div_pts[0]).normalized()
		_check(leave.distance_to(Vector2(0, 1)) < 0.05 and float(sw["radius"]) >= SceneryDesign.SWITCH_MIN_RADIUS_M, "diverging track leaves along the through track (radius %.0f m)" % float(sw["radius"]))
		_check(d0.edges[int(sw["edge"])]["kind"] == "rail" and int(d0.edges[int(sw["edge"])]["gauge"]) == 1435, "diverging track copies the track's attributes")
	ed.mesher.rebuild(d0)
	var names: Array = []
	for mi in ed.mesher.get_children():
		names.append(mi.name)
	print("  [diag] meshes: %s" % ", ".join(names))
	_check(ed.mesher.chain_count >= 5, "mesher built the chains that have terrain (%d)" % ed.mesher.chain_count)
	# the road mesh must sit on the terrain at its first node
	var ok_pos := false
	var ok_uv := false
	for mi in ed.mesher.get_children():
		var mesh: ArrayMesh = (mi as MeshInstance3D).mesh
		var arrays: Array = mesh.surface_get_arrays(0)
		var v0: Vector3 = (mi as MeshInstance3D).global_transform * (arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array)[0]
		var g: Vector3 = W3DCoords.ecef_to_geodetic(v0)
		var en: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
		var terr: float = ed.elevation_lv95(en.x, en.y)
		var uv2: Vector2 = (arrays[Mesh.ARRAY_TEX_UV2] as PackedVector2Array)[0]
		print("  [diag] %s first vertex E %.1f N %.1f h %.2f terrain %.2f uv2 %s" % [mi.name, en.x, en.y, g.z, terr, str(uv2)])
		if not is_nan(terr) and g.z > terr and g.z < terr + 1.0:
			ok_pos = true
		if uv2.x > 1.0:
			ok_uv = true
	_check(ok_pos, "a mesh vertex sits just above the terrain")
	_check(ok_uv, "UV2 carries the half width")
	# hidden ways: a replaced OSM way vanishes from its road tile without a bake
	var rms_h: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	var real_way: Dictionary = {}
	var real_tile := ""
	var real_dir := ""
	if rms_h.size() > 0:
		for dir in rms_h[0].get_road_paths():
			if "switzerland-base" in str(dir):
				real_dir = str(dir)
		var te_h: int = int(floor(E / 1024.0)) * 1024
		var tn_h: int = int(floor(N / 1024.0)) * 1024
		real_tile = "%d_%d" % [te_h, tn_h]
		var sc := real_dir.path_join("roads_%s.ways.json" % real_tile)
		if FileAccess.file_exists(sc):
			var recs = JSON.parse_string(FileAccess.get_file_as_string(sc))
			if recs is Array and recs.size() > 0:
				real_way = recs[0]
	_check(not real_way.is_empty(), "a real tile sidecar next to the airport lists its ways (%s)" % real_tile)
	if not real_way.is_empty():
		var tile_node: MeshInstance3D = null
		for ch_node in rms_h[0].get_children():
			if ch_node is MeshInstance3D and str(ch_node.name).ends_with("_" + real_tile):
				tile_node = ch_node
		var before_idx: int = (tile_node.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_INDEX].size() if tile_node else -1
		var wid_h := int(real_way["id"])
		d0.replace_ids.append(wid_h)
		ed.apply_hidden_ways()
		_check(wid_h in rms_h[0].get_hidden_ways(), "road manager holds the replaced way id")
		var waited_h := 0.0
		tile_node = null
		while tile_node == null and waited_h < 30.0:
			await get_tree().create_timer(0.5).timeout
			waited_h += 0.5
			for ch_node in rms_h[0].get_children():
				if ch_node is MeshInstance3D and str(ch_node.name).ends_with("_" + real_tile) and ch_node.mesh != null:
					tile_node = ch_node
		var after_idx: int = (tile_node.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_INDEX].size() if tile_node else -1
		print("  [diag] tile %s: %d indices before, %d after hiding way %d (%.1f s)" % [real_tile, before_idx, after_idx, wid_h, waited_h])
		_check(before_idx > 0 and after_idx > 0 and after_idx < before_idx, "the replaced way's triangles are gone from the reloaded tile")
		d0.replace_ids.erase(wid_h)
		ed.apply_hidden_ways()
	# terrain patches at tile load: a plateau and a road corridor change the precise terrain without a bake
	var tm = root._get_tile_manager()
	_check(tm != null and tm.has_method("set_terrain_patches"), "tile manager takes terrain patches")
	var pe := E - 700.0
	var pn := N - 700.0
	var before_h: float = ed.elevation_lv95_precise(pe, pn)
	d0.flatten.append({"poly": [Vector2(pe - 60.0, pn - 60.0), Vector2(pe + 60.0, pn - 60.0), Vector2(pe + 60.0, pn + 60.0), Vector2(pe - 60.0, pn + 60.0)], "h": 450.0})
	var s0 := d0.add_node(E - 1200.0, N + 400.0)
	var s1 := d0.add_node(E - 800.0, N + 700.0)
	d0.add_edge(s0, s1, SceneryDesign.road_attrs("secondary", 2, false))
	ed.apply_terrain_patches(true)
	_check(tm.get_terrain_patch_count() >= 2, "plateau and corridor handed over (%d patches)" % tm.get_terrain_patch_count())
	var waited_p := 0.0
	while (is_nan(ed.elevation_lv95_precise(pe, pn)) or absf(ed.elevation_lv95_precise(pe, pn) - 450.0) > 0.05) and waited_p < 40.0:
		await get_tree().create_timer(0.5).timeout
		waited_p += 0.5
	var after_h: float = ed.elevation_lv95_precise(pe, pn)
	print("  [diag] plateau: terrain %.2f -> %.2f after %.1f s" % [before_h, after_h, waited_p])
	_check(absf(after_h - 450.0) < 0.05, "the precise terrain inside the plateau is at its height after the reload")
	var patches := ed.terrain_patches()
	var corr: Array = patches["corridors"]
	_check(corr.size() >= 1, "the road became a corridor of stations")
	if corr.size() >= 1:
		var st: PackedVector3Array = corr[0]["stations"]
		var mid_st: Vector3 = st[st.size() / 2]
		var g_mid: float = ed.elevation_lv95_precise(mid_st.x, mid_st.y)
		print("  [diag] corridor: %d stations, mid station h %.2f, terrain there %.2f" % [st.size(), mid_st.z, g_mid])
		_check(absf(g_mid - mid_st.z) < 0.5, "the ground under the road follows the corridor profile (within the 31 m posting)")
	# airport protection: a corridor across a runway strip leaves the strip's terrain alone
	var prot := ed.airport_protection()
	print("  [diag] protection: %d strips / taxiways, %d aprons" % [(prot["corridors"] as Array).size(), (prot["polys"] as Array).size()])
	_check((prot["corridors"] as Array).size() > 0, "runway strips of the known airports become protection corridors")
	var rw_pt := Vector2(E, N)   # the spawn: on the LSZH runway
	var rw_before: float = ed.elevation_lv95_precise(rw_pt.x, rw_pt.y)
	var x0 := d0.add_node(E - 300.0, N)
	var x1 := d0.add_node(E + 300.0, N)
	d0.add_edge(x0, x1, SceneryDesign.road_attrs("secondary", 2, false))
	d0.set_node_offset(x0, -3.0)
	d0.set_node_offset(x1, -3.0)
	ed.apply_terrain_patches(true)
	await _wait_terrain(ed, rw_pt.x, rw_pt.y)
	await get_tree().create_timer(1.0).timeout
	var rw_after: float = ed.elevation_lv95_precise(rw_pt.x, rw_pt.y)
	print("  [diag] runway terrain under a 3 m cut corridor: %.2f -> %.2f" % [rw_before, rw_after])
	_check(absf(rw_after - rw_before) < 0.05, "the runway plateau is not touched by a design corridor")
	d0.remove_edge(d0.edges_of(x0)[0])
	d0.flatten.pop_back()
	d0.remove_edge(d0.edges_of(s0)[0])
	ed.apply_terrain_patches(true)
	await _wait_terrain(ed, E + 400.0, N - 250.0)
	# bake from the editor: the command names the region, the box around the camera and the design
	var cmd: Array = ed.bake_command()
	print("  [diag] bake command: %s" % " ".join(cmd))
	_check(cmd.size() >= 9 and str(cmd[0]).ends_with("build_world_roads.py") and "--region" in cmd and "switzerland-base" in cmd and "--design" in cmd and design_file in cmd, "bake command covers the region, a box around the camera and this design")
	# reload: the road tiles are dropped and stream in again
	var rms0: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	if rms0.size() > 0 and rms0[0].has_method("reload_tiles"):
		var before_tiles: int = rms0[0].get_active_tile_count()
		rms0[0].reload_tiles()
		await get_tree().process_frame
		var right_after: int = rms0[0].get_active_tile_count()
		var waited_r := 0.0
		while rms0[0].get_active_tile_count() < before_tiles and waited_r < 30.0:
			await get_tree().create_timer(0.5).timeout
			waited_r += 0.5
		print("  [diag] reload_tiles: %d tiles -> %d right after -> %d after %.1f s" % [before_tiles, right_after, rms0[0].get_active_tile_count(), waited_r])
		_check(right_after == 0 and rms0[0].get_active_tile_count() >= before_tiles, "reload_tiles drops every road tile and streams them in again")
	else:
		_check(false, "road manager has reload_tiles")
	# save through the keyboard: Ctrl+S must write the file and not move the camera
	var before: Vector3 = ed._pos
	var ks := InputEventKey.new()
	ks.keycode = KEY_S
	ks.ctrl_pressed = true
	ks.pressed = true
	Input.parse_input_event(ks)
	await get_tree().process_frame
	await get_tree().process_frame
	_check(FileAccess.file_exists(design_file), "Ctrl+S key event wrote %s" % design_file)
	_check(not ed.design.dirty, "design no longer marked unsaved after Ctrl+S")
	for i in range(5):
		ed.update(0.016)
	_check(ed._pos.distance_to(before) < 0.01, "Ctrl+S held does not move the camera (moved %.2f m)" % ed._pos.distance_to(before))
	ks.pressed = false
	Input.parse_input_event(ks)
	await get_tree().process_frame
	_check(ed.save(), "design saved to %s" % design_file)
	# node height offsets: a deck 6 m over node b lifts the ribbon there
	d0.set_node_offset(b, 6.0)
	_check(absf(d0.node_offset(b) - 6.0) < 0.001 and d0.node_offset(a) == 0.0, "node height offset stored, others stay zero")
	ed.mesher.rebuild(d0)
	var best_h := NAN
	var best_d := 1e9
	for mi in ed.mesher.get_children():
		if not mi.has_meta("edges"):
			continue
		var arr: PackedVector3Array = (mi.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		for v in arr:
			var gw: Vector3 = W3DCoords.ecef_to_geodetic((mi as MeshInstance3D).global_transform * v)
			var enw: Vector2 = Geo.wgs84_to_lv95(gw.x, gw.y)
			var dd: float = enw.distance_to(d0.node_pos(b))
			if dd < best_d:
				best_d = dd
				best_h = gw.z
	var terr_b: float = ed.elevation_lv95(d0.node_pos(b).x, d0.node_pos(b).y)
	print("  [diag] nearest vertex to the deck node: %.1f m away, height %.2f, terrain %.2f" % [best_d, best_h, terr_b])
	_check(best_d < 8.0 and best_h > terr_b + 5.0 and best_h < terr_b + 7.5, "ribbon rises to the node's height offset (deck)")
	d0.set_node_offset(b, 0.0)
	_check(not d0.nodes[b].has("h"), "a zero offset is dropped from the node")
	# flatten plateaus round-trip
	d0.flatten.append({"poly": [Vector2(E - 300.0, N - 300.0), Vector2(E - 200.0, N - 300.0), Vector2(E - 250.0, N - 200.0)], "h": 431.5})
	var tmp2 := SceneryDesign.new()
	d0.set_node_offset(c, -2.0)
	tmp2.from_dict(d0.to_dict())
	_check(tmp2.flatten.size() == 1 and absf(float(tmp2.flatten[0]["h"]) - 431.5) < 0.001 and absf(tmp2.node_offset(c) + 2.0) < 0.001, "plateaus and node offsets survive the JSON round trip")
	_check(d0.flatten_near(E - 250.0, N - 200.0, 1.0) == 0, "flatten polygon corner is found")
	d0.set_node_offset(c, 0.0)
	# screen-space picking: an elevated node and an edge are found by pixels, not by the terrain hit
	_check(ed.mesher.elevation.get_method() == "elevation_lv95_precise", "live meshes use the precise terrain only")
	# look straight down on the road from 300 m so its nodes and edges are all on screen
	ed._pos = ed._world_of(E + 450.0, N - 250.0, ed.elevation_lv95(E + 450.0, N - 250.0) + 300.0)
	ed._yaw = 0.0
	ed._pitch = -PI * 0.5
	ed._apply_camera()
	d0.set_node_offset(b, 6.0)
	ed._rebuild_live()
	await _wait_terrain(ed, E + 400.0, N - 250.0)
	ed.mesher.rebuild(d0)
	var sp_b: Vector2 = ed._cam.unproject_position(ed.node_world(b))
	_check(ed.node_at_screen(sp_b + Vector2(6, -5)) == b, "an elevated node is picked within a few pixels on screen")
	_check(ed.node_at_screen(sp_b + Vector2(60, 0)) != b, "and not from far away on screen")
	var ab_edge: int = -1
	for eid in d0.edges_of(a):
		ab_edge = eid
	# a quarter along the edge from a (the middle is where the railway crosses: either edge would be right there)
	var mid := d0.node_pos(a).lerp(d0.node_pos(d0.edges[ab_edge]["b"] if d0.edges[ab_edge]["a"] == a else d0.edges[ab_edge]["a"]), 0.25)
	var mid_h := ed.elevation_lv95(mid.x, mid.y)
	var sp_mid: Vector2 = ed._cam.unproject_position(ed._world_of(mid.x, mid.y, mid_h + 0.3))
	var eh := ed.edge_at_screen(sp_mid + Vector2(4, 4))
	print("  [diag] edge pick: edge %d probe %s screen %s -> %s" % [ab_edge, str(mid), str(sp_mid), str(eh)])
	_check(not eh.is_empty() and int(eh["id"]) == ab_edge and Vector2(eh["e"], eh["n"]).distance_to(mid) < 20.0, "an edge is picked on screen with the point under the mouse")
	_check(ed.node_markers_count() == d0.node_count(), "one marker per design node")
	d0.set_node_offset(b, 0.0)
	# Select on a design edge: [ ] lifts both its ends
	ed._set_tool(SceneryEditor.Tool.SELECT)
	var sel_edge: int = ab_edge
	var sel_pt := d0.node_pos(a).lerp(d0.node_pos(d0.edges[sel_edge]["b"] if d0.edges[sel_edge]["a"] == a else d0.edges[sel_edge]["a"]), 0.25)
	var sel_scr: Vector2 = ed._cam.unproject_position(ed._world_of(sel_pt.x, sel_pt.y, ed.elevation_lv95(sel_pt.x, sel_pt.y) + 0.3))
	var mbe := InputEventMouseButton.new()
	mbe.button_index = MOUSE_BUTTON_LEFT
	mbe.pressed = true
	mbe.position = sel_scr
	ed._mouse = sel_scr
	ed._unhandled_input(mbe)
	mbe.pressed = false
	ed._unhandled_input(mbe)
	_check(ed._selected_edge == sel_edge, "Select picks a design edge on screen (%d)" % ed._selected_edge)
	ed.nudge_height(2.0)
	var ea: int = d0.edges[sel_edge]["a"]
	var eb: int = d0.edges[sel_edge]["b"]
	_check(absf(d0.node_offset(ea) - 2.0) < 0.01 and absf(d0.node_offset(eb) - 2.0) < 0.01, "[ ] on a selected edge raises both ends")
	d0.set_node_offset(ea, 0.0)
	d0.set_node_offset(eb, 0.0)
	ed._selected_edge = -1
	ed._set_tool(SceneryEditor.Tool.ROAD)
	ed.apply_terrain_patches(true)                 # the offsets changed the patches: tiles reload
	await _wait_terrain(ed, E + 400.0, N - 250.0)
	# bridge structures under a deck, areas (patch + building), clear air
	_check(root._sky.clear_air, "no fog while the editor is on")
	d0.set_node_offset(b, 6.0)
	d0.set_node_offset(c, 6.0)
	await _wait_terrain(ed, E + 400.0, N - 250.0)
	ed.mesher.rebuild(d0)
	print("  [diag] piers under the deck: %d" % ed.mesher.pier_count)
	_check(ed.mesher.pier_count >= 8, "piers every 25 m under the elevated run (%d)" % ed.mesher.pier_count)
	d0.set_node_offset(b, 0.0)
	d0.set_node_offset(c, 0.0)
	d0.areas.append({"poly": [Vector2(E - 400.0, N - 100.0), Vector2(E - 300.0, N - 100.0), Vector2(E - 300.0, N), Vector2(E - 350.0, N + 40.0), Vector2(E - 400.0, N)], "type": "forest", "h": 0.0})
	d0.areas.append({"poly": [Vector2(E - 500.0, N + 100.0), Vector2(E - 470.0, N + 100.0), Vector2(E - 470.0, N + 120.0), Vector2(E - 500.0, N + 120.0)], "type": "building", "h": 9.0})
	ed.mesher.rebuild(d0)
	_check(ed.mesher.area_count == 2, "two area meshes built (%d)" % ed.mesher.area_count)
	var bld: MeshInstance3D = ed.mesher.get_node_or_null("Area_1")
	if bld:
		var av: PackedVector3Array = (bld.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var top := -1e9
		var bot := 1e9
		for v in av:
			top = maxf(top, v.y)
			bot = minf(bot, v.y)
		print("  [diag] building mesh: %d vertices, roof at %.1f, walls down to %.1f (local)" % [av.size(), top, bot])
		_check(av.size() == 4 + 16 and absf(top - 9.0) < 0.01 and bot < 0.0, "building: roof at the height, four walls to the ground")
	else:
		_check(false, "building mesh present")
	var tmp3 := SceneryDesign.new()
	tmp3.from_dict(d0.to_dict())
	_check(tmp3.areas.size() == 2 and tmp3.areas[1]["type"] == "building" and absf(float(tmp3.areas[1]["h"]) - 9.0) < 0.001, "areas survive the JSON round trip")
	_check(d0.area_near(E - 300.0, N, 1.0) == 0, "area corner is found")
	# replace polygons round-trip and the baked-design detection
	d0.replace.append([Vector2(E - 100.0, N - 100.0), Vector2(E + 100.0, N - 100.0), Vector2(E, N + 100.0)])
	d0.dirty = true
	_check(d0.replace_near(E + 100.0, N - 100.0, 1.0) == 0, "replace polygon corner is found")
	var tmp := SceneryDesign.new()
	tmp.from_dict(d0.to_dict())
	_check(tmp.replace.size() == 1 and (tmp.replace[0] as Array).size() == 3, "replace polygons survive the JSON round trip")
	_check(not ed.design_is_baked(), "an unsaved design is never baked")
	# a roads directory whose manifest lists this design's signature marks it baked
	ed.save()
	var sha := SceneryDesign.file_sha256(design_file)
	var fake_dir := design_file.get_base_dir().path_join("fake_roads")
	DirAccess.make_dir_recursive_absolute(fake_dir)
	var mf := FileAccess.open(fake_dir.path_join("manifest.json"), FileAccess.WRITE)
	mf.store_string(JSON.stringify({"schema_version": 1, "tiles": {}, "designs": [{"path": design_file, "sha256": sha}]}))
	mf.close()
	var rms: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	var saved_paths: PackedStringArray = rms[0].get_road_paths() if rms.size() > 0 else PackedStringArray()
	if rms.size() > 0:
		var withfake := PackedStringArray(saved_paths)
		withfake.append(fake_dir)
		rms[0].set_road_paths(withfake)
	_check(sha != "" and ed.design_is_baked(), "a manifest listing the design's sha256 marks it baked")
	ed._rebuild_live()
	_check(ed.mesher.get_child_count() == 0, "live meshes are off while the tiles carry the design")
	d0.add_node(E - 900.0, N - 900.0)
	_check(not ed.design_is_baked(), "an edit (dirty) turns the live meshes back on")
	ed._rebuild_live()
	await _wait_terrain(ed, E + 400.0, N - 250.0)
	ed._rebuild_live()
	_check(ed.mesher.get_child_count() > 0, "live meshes rebuilt after the edit")
	# Pick: a sidecar in that roads directory describes a baked OSM way near the aircraft
	var te: int = int(floor((E + 2000.0) / 1024.0)) * 1024
	var tn: int = int(floor(N / 1024.0)) * 1024
	var sf := FileAccess.open(fake_dir.path_join("roads_%d_%d.ways.json" % [te, tn]), FileAccess.WRITE)
	sf.store_string(JSON.stringify([{"id": 4242, "kind": "road", "cls": "primary", "lanes": 2, "oneway": false, "gauge": 0, "tracks": 0, "bridge": true, "tunnel": false,
		"pts": [[E + 1900.0, N - 50.0], [E + 2000.0, N - 20.0], [E + 2100.0, N + 10.0], [E + 2200.0, N + 50.0]]}]))
	sf.close()
	ed._ways_tiles.clear()
	var found := ed.way_near(E + 2050.0, N - 6.0)
	_check(not found.is_empty() and int(found["id"]) == 4242, "way_near finds the baked way from the tile sidecar (%s)" % SceneryEditor.way_label(found))
	var before_edges := d0.edge_count()
	var picked := d0.import_way(found)
	_check(picked.size() >= 2 and d0.edge_count() == before_edges + picked.size() - 1 and 4242 in d0.replace_ids, "picking imports the way as nodes / edges and lists its id for the bake")
	_check(d0.edges[d0.edges_of(picked[0])[0]]["cls"] == "primary", "imported edges carry the way's class")
	ed.adopt_attrs(found)
	_check(SceneryDesign.ROAD_CLASSES[ed._cls_option.selected] == "primary" and int(ed._lanes_spin.value) == 2 and not ed._oneway_check.button_pressed, "the panel adopts a picked way's class, lanes and direction")
	# an explicit width survives the pick and sets the ribbon width
	var wide := {"id": 4444, "kind": "road", "cls": "secondary", "lanes": 2, "oneway": false, "gauge": 0, "tracks": 0, "width": 9.0, "bridge": false, "tunnel": false,
		"pts": [[E + 1900.0, N + 600.0], [E + 2200.0, N + 600.0]]}
	var wtag_ids := d0.import_way(wide)
	var wtag_edge: int = d0.edges_of(wtag_ids[0])[0]
	_check(absf(float(d0.edges[wtag_edge]["width"]) - 9.0) < 0.01 and absf(SceneryDesignMesher.edge_width(d0, wtag_edge) - 9.0) < 0.01, "a way's width tag becomes the edge width (9 m instead of 7 m from the lanes)")
	var tmp5 := SceneryDesign.new()
	tmp5.from_dict(d0.to_dict())
	_check(absf(float(tmp5.edges[wtag_edge]["width"]) - 9.0) < 0.01, "edge widths survive the JSON round trip")
	_check(ed.way_near(E + 2050.0, N - 6.0).is_empty(), "a picked way is not offered again")
	# snapping to the end of a baked road (a way not yet picked)
	var sf3 := FileAccess.open(fake_dir.path_join("roads_%d_%d.ways.json" % [te, tn]), FileAccess.WRITE)
	sf3.store_string(JSON.stringify([{"id": 4545, "kind": "road", "cls": "tertiary", "lanes": 2, "oneway": false, "gauge": 0, "tracks": 0, "bridge": false, "tunnel": false,
		"pts": [[E + 1900.0, N + 400.0], [E + 2000.0, N + 400.0], [E + 2100.0, N + 400.0]]}]))
	sf3.close()
	ed._ways_tiles.clear()
	var snap := ed.baked_endpoint_near(E + 2104.0, N + 396.0, 12.0)
	_check(snap.distance_to(Vector2(E + 2100.0, N + 400.0)) < 0.01, "a point placed near a baked road's end snaps onto that end")
	_check(ed.baked_endpoint_near(E + 2000.0, N + 400.0, 12.0) == Vector2.INF, "but not onto its middle")
	# typed spin box text counts before Enter
	ed._lanes_spin.get_line_edit().text = "3"
	var typed := ed._current_attrs()
	_check(int(typed["lanes"]) == 3, "a lane count typed into the panel is used without Enter (%d)" % int(typed["lanes"]))
	# a line drawn towards the END of a one-way baked road flows out of it: reversed, with the road's settings
	var sf4 := FileAccess.open(fake_dir.path_join("roads_%d_%d.ways.json" % [te, tn]), FileAccess.WRITE)
	sf4.store_string(JSON.stringify([{"id": 4646, "kind": "road", "cls": "motorway", "lanes": 3, "oneway": true, "gauge": 0, "tracks": 0, "bridge": false, "tunnel": false, "width": 0,
		"pts": [[E + 1700.0, N + 800.0], [E + 1800.0, N + 800.0], [E + 1900.0, N + 800.0]]}]))
	sf4.close()
	ed._ways_tiles.clear()
	ed._ways.clear()
	ed._set_tool(SceneryEditor.Tool.ROAD)
	ed._cls_option.selected = SceneryDesign.ROAD_CLASSES.find("secondary")
	ed._oneway_check.button_pressed = false
	ed._cursor_valid = true
	ed._mouse = Vector2(-1000, -1000)
	ed._cursor_e = E + 2200.0
	ed._cursor_n = N + 800.0
	ed.place_point()
	ed._cursor_e = E + 1900.0     # exactly the way's end
	ed._cursor_n = N + 800.0
	ed.place_point()
	var line_nodes: Array = ed._line_nodes.duplicate()
	ed.finish_line()
	var jedge: int = -1
	for eid in d0.edges_of(line_nodes[0]):
		var ed_: Dictionary = d0.edges[eid]
		if ed_["a"] == line_nodes[1] or ed_["b"] == line_nodes[1]:
			jedge = eid
	_check(jedge >= 0 and d0.edges[jedge]["cls"] == "motorway" and int(d0.edges[jedge]["lanes"]) == 3 and bool(d0.edges[jedge]["oneway"]), "a line joining a baked road takes its class, lanes and direction")
	_check(jedge >= 0 and d0.edges[jedge]["a"] == line_nodes[1] and d0.edges[jedge]["b"] == line_nodes[0], "drawn towards the end of a one-way road, the line is reversed to flow out of it")
	# the Ctrl+S test above released S with Ctrl still down; the Ctrl release itself clears the flag
	var kctrl := InputEventKey.new()
	kctrl.keycode = KEY_CTRL
	kctrl.pressed = false
	Input.parse_input_event(kctrl)
	await get_tree().process_frame
	_check(not ed._modifier_down, "the modifier flag follows the last key event (Ctrl released)")
	ed._modifier_down = true
	ed._enter_common(true)
	_check(not ed._modifier_down, "entering the editor clears a stuck modifier")
	# direction: R reverses the joined one-way line, the panel heading follows, arrows are drawn
	var before_a: int = d0.edges[jedge]["a"]
	var flipped := d0.reverse_chain(jedge)
	_check(flipped >= 1 and d0.edges[jedge]["b"] == before_a, "R reverses every edge of the selected road's chain (%d)" % flipped)
	var hdg := SceneryEditor.heading_at([Vector2(0, 0), Vector2(100, 0)], Vector2(50, 1))
	_check(absf(hdg - 90.0) < 0.01, "flow heading east = 90 degrees")
	ed._update_direction_arrows()
	_check(ed._dir_overlay_mesh.get_surface_count() >= 1, "direction arrows drawn for one-way roads")
	var tmp4 := SceneryDesign.new()
	tmp4.from_dict(d0.to_dict())
	_check(4242 in tmp4.replace_ids, "replaced way ids survive the JSON round trip")
	# Delete on a baked road marks it for removal at the next bake
	var sf2 := FileAccess.open(fake_dir.path_join("roads_%d_%d.ways.json" % [te, tn]), FileAccess.WRITE)
	sf2.store_string(JSON.stringify([{"id": 4343, "kind": "road", "cls": "tertiary", "lanes": 2, "oneway": false, "gauge": 0, "tracks": 0, "bridge": false, "tunnel": false,
		"pts": [[E + 1900.0, N + 300.0], [E + 2200.0, N + 300.0]]}]))
	sf2.close()
	ed._ways_tiles.clear()
	ed._cursor_valid = true
	ed._cursor_e = E + 2050.0
	ed._cursor_n = N + 301.0
	ed._mouse = Vector2(-1000, -1000)
	_check(ed.delete_at_cursor() and 4343 in d0.replace_ids, "Delete on a baked road lists it for removal at the next bake")
	if rms.size() > 0:
		rms[0].set_road_paths(saved_paths)
	ed.save()   # the reload check below compares against the file
	var d1 := SceneryDesign.new()
	_check(d1.load(design_file) and d1.edge_count() == d0.edge_count() and d1.node_count() == d0.node_count(), "reloaded design has the same nodes / edges")
	while d0.can_undo():
		d0.undo()
	_check(d0.edge_count() == 0, "undo all the way restores the empty design")
	ed.exit()
	await get_tree().process_frame
	_check(not ed.is_active() and get_viewport().get_camera_3d() == flight_cam, "exit restores the aircraft camera")
	_check(not root._sky.clear_air, "fog back after leaving the editor")
	if _fails.is_empty():
		print("smoke_scenery_editor: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_scenery_editor: %d failure(s)" % _fails.size())
		get_tree().quit(1)
