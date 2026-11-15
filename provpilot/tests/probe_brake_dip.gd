extends Node
## Braking dips the nose: which drawn parts of the nose gear go under the
## ground when the strut compresses? Accelerates to a taxi speed, brakes hard,
## and prints every matching mesh's lowest vertex above the ground at rest and
## at its lowest, with the nose strut's compression.
## Run: godot --headless --path provpilot res://tests/probe_brake_dip.tscn -- <id> [regex] [speed_kt]
const THR16 := Vector2(47.47606, 8.53597)
const THR34 := Vector2(47.4453, 8.5547)
var _root: Node = null
var _verts := {}

func _lowest(ac: Node3D, meshes: Array) -> Dictionary:
	var pos: Vector3 = ac.global_transform.origin
	var ag: Vector3 = W3DCoords.ecef_to_geodetic(pos)
	var ground: float = _root._world.ground_height_geodetic(ag.x, ag.y)
	var up: Vector3 = W3DCoords.geodetic_normal(ag.x, ag.y)
	var out := {}
	for mi in meshes:
		if not mi.is_visible_in_tree():
			continue
		var lo := INF
		var gx: Transform3D = mi.global_transform
		for v in _verts[mi]:
			lo = minf(lo, ag.z + (gx * v - pos).dot(up) - ground)
		out[mi.name] = lo
	return out

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "md80"
	var rx := RegEx.create_from_string(args[1] if args.size() > 1 else "^LGN")
	var speed: float = float(args[2]) if args.size() > 2 else 40.0
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
	_root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
	var a: Vector2 = Geo.wgs84_to_lv95(THR16.x, THR16.y)
	var b: Vector2 = Geo.wgs84_to_lv95(THR34.x, THR34.y)
	var dir: Vector2 = (b - a).normalized()
	var start: Vector2 = a + dir * 200.0
	_root._on_welcome_flight_requested(id, start.x, start.y, fposmod(rad_to_deg(atan2(dir.x, dir.y)), 360.0))
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	_root._cancel_intro_zoom()
	_root._welcome_screen.visible = false
	var jsb = _root.jsb_node
	var ac: Node3D = _root.get_node("AC")
	var meshes: Array = []
	for n in ac.find_children("*", "MeshInstance3D", true, false):
		var mi := n as MeshInstance3D
		if mi.mesh != null and rx.search(mi.name) != null:
			var all := PackedVector3Array()
			for si in range(mi.mesh.get_surface_count()):
				all.append_array(mi.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX])
			_verts[mi] = all
			meshes.append(mi)
	var rest := _lowest(ac, meshes)
	var comp0 := 0.0
	for c in jsb.get_gear_contacts():
		if c["bogey"]:
			comp0 = float(c["compression_m"])
			break
	var worst := {}
	var worst_comp := {}
	var braking := false
	var max_comp := 0.0
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < 90000:
		var kt: float = jsb.get_property("velocities/vg-fps") * 0.5925
		if not braking and kt >= speed:
			braking = true
		jsb.set_input_throttle(0.0 if braking else 0.7)
		jsb.set_input_brake(1.0 if braking else 0.0)
		await get_tree().process_frame
		var comp := 0.0
		for c in jsb.get_gear_contacts():
			if c["bogey"]:
				comp = float(c["compression_m"])
				break
		max_comp = maxf(max_comp, comp)
		var now := _lowest(ac, meshes)
		for k in now:
			if "Tires" in str(k) and now[k] < -0.04:
				print("dipframe: frame %d t %.2f s, %.1f kt, comp %.3f, %s %+.3f, shifts %s, braking %s" % [Engine.get_process_frames(), (Time.get_ticks_msec() - t0) / 1000.0, kt, comp, k, now[k], str(_root._aircraft.get_strut_shifts()), str(braking)])
		if OS.get_environment("DIP_TRACE") != "" and Engine.get_process_frames() % 6 == 0:
			var line := "trace: %.1f kt comp %.3f norm %s shifts %s |" % [kt, comp, str(_root._aircraft.get_fg_property("gear/gear/compression-norm")), str(_root._aircraft.get_strut_shifts())]
			for k in now:
				if OS.get_environment("DIP_TRACE") in str(k):
					line += " %s %+.3f" % [k, now[k]]
			print(line)
		for k in now:
			if not worst.has(k) or now[k] < worst[k]:
				worst[k] = now[k]
				worst_comp[k] = comp
		if braking and kt < 0.5:
			break
	print("probe_brake_dip: %s nose strut compression at rest %.3f m, most %.3f m; strut shifts now %s" % [id, comp0, max_comp, str(_root._aircraft.get_strut_shifts())])
	var bad := 0
	for k in worst:
		var flag := ""
		if worst[k] < -0.03 and worst[k] < float(rest.get(k, 0.0)) - 0.03:
			flag = "  <-- sinks"
			bad += 1
		print("probe_brake_dip:   %-28s rest %+.3f m, lowest %+.3f m (at compression %.3f m)%s" % [k, float(rest.get(k, NAN)), worst[k], worst_comp[k], flag])
	print(("  PASS: " if bad == 0 else "  FAIL: ") + "%s: no matching part sinks under the ground while braking (%d do)" % [id, bad])
	get_tree().quit(0 if bad == 0 else 1)
