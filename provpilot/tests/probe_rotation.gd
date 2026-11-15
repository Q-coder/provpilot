extends Node
## Take-off rotation: does the visual model stay on top of the runway while
## the FDM pitches the aircraft about its main wheels? Full throttle, stick
## held back from a given speed, and for every frame with a main wheel on the
## ground the lowest point of the tyres and of the whole airframe above the
## ground is measured (negative = below the surface).
## Run: godot --headless --path provpilot res://tests/probe_rotation.tscn -- <id> <rotate_kt> [max_sink_m] [shot_dir]
# Zurich runway 16: 3.7 km of level pavement ahead (thresholds from the AIP)
const THR16 := Vector2(47.47606, 8.53597)
const THR34 := Vector2(47.4453, 8.5547)
var _root: Node = null
var _verts := {}
var _ac_pos := Vector3.ZERO
var _ac_h := 0.0
var _ac_ground := 0.0
var _ac_up := Vector3.UP
var _low_h := INF          # lowest airframe vertex of the current frame
var _low_world := Vector3.ZERO

func _low(ac: Node3D) -> Array:
	_ac_pos = ac.global_transform.origin
	var ag: Vector3 = W3DCoords.ecef_to_geodetic(_ac_pos)
	_ac_h = ag.z
	_ac_ground = _root._world.ground_height_geodetic(ag.x, ag.y)
	_ac_up = W3DCoords.geodetic_normal(ag.x, ag.y)
	var min_all := INF
	var min_wheel := INF
	var all_name := ""
	var stack: Array = [ac]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and n.is_visible_in_tree():
			var mi := n as MeshInstance3D
			var aabb: AABB = mi.get_aabb()
			var lo := INF
			for i in range(8):
				var w: Vector3 = mi.global_transform * aabb.get_endpoint(i)
				var g: Vector3 = W3DCoords.ecef_to_geodetic(w)
				var ground: float = _root._world.ground_height_geodetic(g.x, g.y)
				if not is_nan(ground):
					lo = minf(lo, g.z - ground)
			var nm := n.name.to_lower()
			if "shadow" in nm:
				continue
			if lo < -0.03:
				# a box corner of a turned mesh is not a point of the mesh: ask the vertices
				lo = INF
				if not _verts.has(mi):
					var all := PackedVector3Array()
					for si in range(mi.mesh.get_surface_count()):
						all.append_array(mi.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX])
					_verts[mi] = all
				# heights from the aircraft node: its altitude, its ground, the geodetic up there
				# (a mesh origin can sit anywhere, and the airfield under one aircraft is level)
				var gx: Transform3D = mi.global_transform
				for v in _verts[mi]:
					var hv: float = _ac_h + (gx * v - _ac_pos).dot(_ac_up) - _ac_ground
					if hv < lo:
						lo = hv
						if lo < _low_h and not ("tyre" in nm or "tire" in nm or "wheel" in nm):
							_low_h = lo
							_low_world = gx * v
			if lo < min_all:
				min_all = lo
				all_name = n.name
			if "tyre" in nm or "tire" in nm:
				min_wheel = minf(min_wheel, lo)
	return [min_all, min_wheel, all_name]

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "a10"
	var vr: float = float(args[1]) if args.size() > 1 else 110.0
	var max_sink: float = float(args[2]) if args.size() > 2 else 0.25
	var shot_dir: String = args[3] if args.size() > 3 else ""
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
	# calm air: a crosswind rolls the aircraft off one wheel and the test is about pitch
	_root._settings.get_settings()["weather_layers"] = []
	_root._settings.get_settings()["live_weather"] = false
	_root._settings.get_settings()["wind_speed_kt"] = 0.0
	_root._push_weather_settings()
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
	var jsb = _root.jsb_node
	var ac: Node3D = _root.get_node("AC")
	jsb.set_input_brake(0.0)  # the parking brake set at flight start
	var parked := _low(ac)
	print("probe_rotation: %s parked: tyres %+.3f m, lowest mesh %+.3f m (%s)" % [id, parked[1], parked[0], parked[2]])
	var worst_all := INF
	var worst_wheel := INF
	var worst_name := ""
	var worst_pitch := 0.0
	var max_pitch := 0.0
	var shot_done := false
	var worst_roll := 0.0
	var first_touch_pitch := -1.0
	var worst_vertex_h := INF
	var worst_body := Vector3.ZERO
	var psi0: float = jsb.get_property("attitude/psi-deg")
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < 120000:
		jsb.set_input_throttle(1.0)
		jsb.set_input_brake(0.0)
		var kt: float = jsb.get_property("velocities/vc-kts")
		# stick back (not slammed), wings held level with aileron and rudder
		var pull: float = -0.7 if kt >= vr else 0.0
		jsb.set_input_elevator(pull)
		jsb.set_input_pitch(pull)
		var phi: float = jsb.get_property("attitude/phi-deg")
		var p: float = jsb.get_property("velocities/p-rad_sec")
		var ail: float = clampf(-phi * 0.08 - p * 0.5, -1.0, 1.0)
		jsb.set_input_roll(ail)
		jsb.set_input_aileron(ail)
		var herr: float = wrapf(jsb.get_property("attitude/psi-deg") - psi0, -180.0, 180.0)
		jsb.set_input_rudder(clampf(-herr * 0.08 - jsb.get_property("velocities/r-rad_sec") * 1.5, -0.6, 0.6))
		await get_tree().process_frame
		var pitch: float = jsb.get_property("attitude/theta-deg")
		var main_wow := false
		for c in jsb.get_gear_contacts():
			if c["bogey"] and c["wow"] and float(c["forward_m"]) < 0.5:
				main_wow = true
		var roll: float = jsb.get_property("attitude/phi-deg")
		if kt >= vr and main_wow and absf(roll) < 2.0:
			_low_h = INF
			var r := _low(ac)
			if _low_h < 0.0 and first_touch_pitch < 0.0:
				first_touch_pitch = pitch
			if _low_h < worst_vertex_h:
				worst_vertex_h = _low_h
				var rel: Vector3 = _low_world - ac.global_transform.origin
				var b: Basis = ac.global_transform.basis.orthonormalized()
				worst_body = Vector3(rel.dot(b.z) + jsb.get_origin_forward_m(), -rel.dot(b.x), -rel.dot(b.y))
			max_pitch = maxf(max_pitch, pitch)
			if r[0] < worst_all:
				worst_all = r[0]
				worst_name = r[2]
				worst_pitch = pitch
			worst_wheel = minf(worst_wheel, r[1])
			if shot_dir != "" and not shot_done and pitch > 8.0:
				shot_done = true
				get_viewport().get_texture().get_image().save_png(shot_dir.path_join("rotation_%s.png" % id))
		if jsb.get_property("position/h-agl-ft") > 60.0:
			break
	print("probe_rotation: %s rotating with the mains on the ground (to %.1f deg): tyres lowest %+.3f m, airframe lowest %+.3f m (%s at %.1f deg); roll up to %.1f deg (frames beyond 2 deg left out)" % [
		id, max_pitch, worst_wheel, worst_all, worst_name, worst_pitch, worst_roll])
	if worst_vertex_h < 0.0:
		print("probe_rotation: %s airframe first under the surface at %.1f deg nose up, %.2f m aft of the CG (tail contact: see probe_gear_align)" % [id, first_touch_pitch, -worst_body.x])
	var ok: bool = worst_all > -max_sink and worst_wheel > -max_sink and max_pitch > 3.0
	print(("  PASS: " if ok else "  FAIL: ") + "%s stays on top of the runway while rotating (limit %.2f m)" % [id, max_sink])
	get_tree().quit(0 if ok else 1)
