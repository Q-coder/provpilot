extends Node
## Strut compression: do the drawn wheels stay on the ground while the FDM
## compresses its gear? The aircraft is dropped from a small height (a hard
## landing) and, for every frame, each FDM gear unit's compression is
## compared with the lowest tyre vertex of the wheel drawn at that unit
## (height above the ground; negative = in the ground, positive = floating).
## Run: godot --headless --path provpilot res://tests/probe_gear_compress.tscn -- <id> [drop_ft] [tolerance_m] [dump]
const THR16 := Vector2(47.47606, 8.53597)
const THR34 := Vector2(47.4453, 8.5547)
var _root: Node = null
var _verts := {}

func _wheel_meshes(ac: Node3D) -> Array:
	var out: Array = []
	var stack: Array = [ac]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and n.is_visible_in_tree():
			var nm := n.name.to_lower()
			if "shadow" in nm or "chock" in nm or "well" in nm or "door" in nm:
				continue
			if "tyre" in nm or "tire" in nm or "wheel" in nm or "whell" in nm:
				var mi := n as MeshInstance3D
				var size: Vector3 = mi.get_aabb().size * mi.global_transform.basis.get_scale()
				if size.length() < 3.0:
					out.append(mi)
	return out

func _dump(n: Node, depth: int, on: bool) -> void:
	var nm := n.name.to_lower()
	var hit := on
	for k in ["gear", "wheel", "whell", "strut", "leg", "mechanic", "tyre", "tire", "lg", "uc-", "fork", "oleo"]:
		if k in nm:
			hit = true
	if hit and n is Node3D:
		var s := ""
		if n is MeshInstance3D and (n as MeshInstance3D).mesh != null:
			var mi := n as MeshInstance3D
			var a: AABB = mi.get_aabb()
			var ac: Node3D = _root.get_node("AC")
			var c: Vector3 = ac.global_transform.affine_inverse() * (mi.global_transform * a.get_center())
			var sz: Vector3 = a.size * mi.global_transform.basis.get_scale()
			s = " mesh centre(ac) %s size %s" % [str(c.snapped(Vector3(0.01, 0.01, 0.01))), str(sz.snapped(Vector3(0.01, 0.01, 0.01)))]
		print("dump: %s%s [%s]%s" % ["  ".repeat(depth), n.name, n.get_class(), s])
	for c in n.get_children():
		_dump(c, depth + 1, hit and depth < 40)

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "a10"
	var drop_ft: float = float(args[1]) if args.size() > 1 else 3.0
	var tol: float = float(args[2]) if args.size() > 2 else 0.04
	var dump: bool = args.size() > 3
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._settings.get_settings()["weather_layers"] = []
	_root._settings.get_settings()["live_weather"] = false
	_root._settings.get_settings()["wind_speed_kt"] = 0.0
	_root._push_weather_settings()
	_root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
	_root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
	var thr_a: Vector2 = Geo.wgs84_to_lv95(THR16.x, THR16.y)
	var thr_b: Vector2 = Geo.wgs84_to_lv95(THR34.x, THR34.y)
	var dir: Vector2 = (thr_b - thr_a).normalized()
	var start: Vector2 = thr_a + dir * 200.0
	var hdg: float = fposmod(rad_to_deg(atan2(dir.x, dir.y)), 360.0)
	_root._on_welcome_flight_requested(id, start.x, start.y, hdg)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	if _root.has_method("_cancel_intro_zoom"):
		_root._cancel_intro_zoom()
	_root._welcome_screen.visible = false   # the flight loop (control surfaces, wheels) is paused under it
	var jsb = _root.jsb_node
	var ac: Node3D = _root.get_node("AC")
	if dump:
		_dump(ac, 0, false)
	var ag00: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_transform.origin)
	print("dbg parked node agl %.3f theta %.2f" % [ag00.z - _root._world.ground_height_geodetic(ag00.x, ag00.y), jsb.get_property("attitude/theta-deg")])
	for c in jsb.get_gear_contacts():
		if c["bogey"]:
			print("dbg parked FDM %s comp %.3f wow %s" % [c["name"], c["compression_m"], str(c["wow"])])
	# drop: lift the parked aircraft and let it fall
	var up: Vector3 = ac.global_transform.origin.normalized()
	var lifted: Transform3D = ac.global_transform
	lifted.origin += up * drop_ft * 0.3048
	var ag0: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_transform.origin)
	jsb.reinitialize_from_pose(lifted, 0.0, _root._world.ground_height_geodetic(ag0.x, ag0.y))
	var wheels := _wheel_meshes(ac)
	var stats := {}   # contact name -> {max_c, worst_lo, worst_hi, at_max, last}
	var t := 0.0
	var settle_c := {}
	while t < 12.0:
		await get_tree().process_frame
		t += get_process_delta_time()
		jsb.set_input_brake(1.0)
		var xf: Transform3D = ac.global_transform
		var b: Basis = xf.basis.orthonormalized()
		var ag: Vector3 = W3DCoords.ecef_to_geodetic(xf.origin)
		var ground: float = _root._world.ground_height_geodetic(ag.x, ag.y)
		var gup: Vector3 = W3DCoords.geodetic_normal(ag.x, ag.y)
		# lowest tyre vertex per wheel mesh, and where it is along / across the aircraft
		var lows: Array = []
		for mi in wheels:
			if not is_instance_valid(mi) or not mi.is_visible_in_tree():
				continue
			if not _verts.has(mi):
				var all := PackedVector3Array()
				for si in range(mi.mesh.get_surface_count()):
					all.append_array(mi.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX])
				_verts[mi] = all
			var gx: Transform3D = mi.global_transform
			var lo := INF
			for v in _verts[mi]:
				lo = minf(lo, ag.z + (gx * v - xf.origin).dot(gup) - ground)
			var c: Vector3 = gx * mi.get_aabb().get_center() - xf.origin
			lows.append([c.dot(b.z) + jsb.get_origin_forward_m(), -c.dot(b.x), lo, mi.name])
		if OS.get_environment("COMPRESS_DEBUG") != "" and Engine.get_process_frames() % 6 == 0 and t < 3.0:
			var fga = _root._rig.get_fg_animator() if "_rig" in _root else null
			for l in lows:
				print("dbg t %.2f %s fwd %.3f right %.3f low %.3f norm1 %s" % [t, l[3], l[0], l[1], l[2], str(fga.get_property("gear/gear[1]/compression-norm")) if fga else "-"])
			print("dbg t %.2f shifts %s" % [t, str(_root._aircraft.get_strut_shifts())])
			print("dbg t %.2f node agl %.3f theta %.2f" % [t, ag.z - ground, jsb.get_property("attitude/theta-deg")])
			for c in jsb.get_gear_contacts():
				if c["bogey"]:
					print("dbg t %.2f FDM %s comp %.3f wow %s" % [t, c["name"], c["compression_m"], str(c["wow"])])
		for c in jsb.get_gear_contacts():
			if not c["bogey"]:
				continue
			var best := INF
			var who := ""
			for l in lows:
				if absf(l[0] - float(c["forward_m"])) < 1.5 and absf(l[1] - float(c["right_m"])) < 1.5:
					if l[2] < best:
						best = l[2]
						who = l[3]
			var nm: String = "%s@%+.1f" % [c["name"], c["right_m"]]
			if not stats.has(nm):
				stats[nm] = {"max_c": 0.0, "at_max": 0.0, "lo": INF, "hi": -INF, "last": 0.0, "last_c": 0.0, "mesh": who}
			var s: Dictionary = stats[nm]
			var comp: float = c["compression_m"]
			s["last"] = best
			s["last_c"] = comp
			if c["wow"] and best < INF and t > 0.15:
				if comp > s["max_c"]:
					s["max_c"] = comp
					s["at_max"] = best
				s["lo"] = minf(s["lo"], best)
				s["hi"] = maxf(s["hi"], best)
	var ok := true
	for nm in stats:
		var s: Dictionary = stats[nm]
		print("probe_compress: %s %-18s wheel %-22s at rest: compression %.3f m, tyre %+.3f m | hardest: compression %.3f m, tyre %+.3f m | on the ground the tyre ranged %+.3f .. %+.3f m" % [
			id, nm, s["mesh"], s["last_c"], s["last"], s["max_c"], s["at_max"], s["lo"], s["hi"]])
		if s["lo"] < -tol or s["hi"] > tol:   # a unit without a drawn wheel (skids) never gets a reading
			ok = false
	print(("  PASS: " if ok else "  FAIL: ") + "%s wheels stay on the ground through a %.0f ft drop (tolerance %.2f m)" % [id, drop_ft, tol])
	get_tree().quit(0 if ok else 1)
