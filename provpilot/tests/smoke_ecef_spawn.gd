extends Node
## Headless smoke for the ECEF world (design §9.5 step C2): start a flight at
## LSZH through the normal welcome path and verify the aircraft is placed on
## the ellipsoid with the right geodetic position and heading, JSBSim runs at
## the true location, the ground handshake holds it on the runway, and the
## tile manager streams around the ECEF camera.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_ecef_spawn.tscn

const E := 2682767.0   # LSZH runway 16, 120 m past the pavement end (flat plateau; the
const N := 1258797.0   # old airport reference sat on sloped grass west of the runway)
const HDG := 155.0     # true heading, along runway 16
var _root: Node = null
var _fails: Array[String] = []

func _fail(m: String) -> void:
	_fails.append(m)
	printerr("FAIL: " + m)

func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	print("[ecef-spawn] requesting flight at E=%.0f N=%.0f hdg=%.0f" % [E, N, HDG])
	_root._on_welcome_flight_requested("c172p", E, N, HDG)
	# Wait for the deferred terrain init (tiles must stream first).
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	if not _root.terrain_initialized:
		_fail("terrain init did not complete in 60 s"); _finish(); return
	print("[ecef-spawn] terrain initialized after %.1f s" % waited)
	var ac: Node3D = _root.get_node("AC")
	var jsb = _root.jsb_node
	var target_ll: Vector2 = Geo.lv95_to_wgs84(E, N)
	# The rebase cooldown pins the aircraft and JSBSim only starts writing the
	# node after it expires; check once settled, then again a few seconds later.
	await get_tree().create_timer(6.0).timeout
	_check_pose(ac, jsb, target_ll, "after 6 s")
	var p0: Vector3 = ac.global_position
	await get_tree().create_timer(5.0).timeout
	_check_pose(ac, jsb, target_ll, "after 11 s")
	# The engine idles with brakes released, so a slow forward creep is normal;
	# anything sideways/vertical or fast means the ground handshake is broken.
	var dvec: Vector3 = ac.global_position - p0
	var drift: float = dvec.length()
	var along_nose: float = dvec.dot(ac.global_transform.basis.z.normalized())
	print("[ecef-spawn] drift over 5 s on the ground: %.2f m (%.2f m along the nose)" % [drift, along_nose])
	# (~3 m/s at idle on the C172 since JSBSim runs at real time, 2026-09-20)
	if drift > 25.0 or (drift > 1.0 and along_nose < 0.8 * drift):
		_fail("aircraft moved %.1f m on the ground, only %.1f m of it forward" % [drift, along_nose])
	# Streaming around the ECEF camera.
	var tm = _root._world.get_tile_manager()
	if tm == null:
		_fail("no tile manager")
	else:
		print("[ecef-spawn] tiles active=%d cam tile=(%d,%d)" % [tm.get_active_tile_count(), tm.get_cam_tile_ei(), tm.get_cam_tile_ni()])
		if absi(tm.get_cam_tile_ei() - int(E / 1024.0)) > 1 or absi(tm.get_cam_tile_ni() - int(N / 1024.0)) > 1:
			_fail("tile manager camera tile is not at LSZH")
		if tm.get_active_tile_count() < 50:
			_fail("too few tiles streamed: %d" % tm.get_active_tile_count())
	# Globe mask frame + terrain query through the scene helpers.
	if _root.globe_node and _root.globe_node.get_coverage_mask_frame() != 1:
		_fail("globe mask frame is %d, expected 1" % _root.globe_node.get_coverage_mask_frame())
	var h_here: float = _root._world.terrain_elevation_below(ac.global_position)
	print("[ecef-spawn] terrain below AC: %.1f m" % h_here)
	if is_nan(h_here) or h_here < 400.0 or h_here > 470.0:
		_fail("terrain elevation below AC implausible: %s" % str(h_here))
	_finish()

func _check_pose(ac: Node3D, jsb, target_ll: Vector2, tag: String) -> void:
	var g: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_position)
	var d_m: float = Vector2((g.x - target_ll.x) * 111000.0, (g.y - target_ll.y) * 111000.0 * cos(deg_to_rad(g.x))).length()
	var up_exp: Vector3 = W3DCoords.geodetic_normal(g.x, g.y)
	var up_dot: float = ac.global_transform.basis.y.normalized().dot(up_exp)
	var hdg: float = _root._world.ac_true_heading(ac)
	var jsb_lat: float = jsb.get_latitude_deg()
	var jsb_lon: float = jsb.get_longitude_deg()
	var jsb_hdg: float = jsb.get_heading()
	var jsb_alt: float = jsb.get_altitude_ft() * 0.3048
	# Attitude mapping check: pitch of the nose above the local horizon and
	# roll of the wing line must match JSBSim's theta / phi (sign included).
	var b: Basis = ac.global_transform.basis
	var nose: Vector3 = b.z.normalized()
	var right: Vector3 = (-b.x).normalized()
	var node_pitch: float = rad_to_deg(asin(clampf(nose.dot(up_exp), -1.0, 1.0)))
	var node_roll: float = rad_to_deg(asin(clampf(-right.dot(up_exp), -1.0, 1.0)))  # right wing down = +roll
	var jsb_pitch: float = jsb.get_pitch_deg()
	var jsb_roll: float = jsb.get_roll_deg()
	print("[ecef-spawn] %s: |p|=%.0f h=%.1f m d=%.1f m up·n=%.5f hdg=%.1f pitch=%.2f roll=%.2f | jsb lat=%.5f lon=%.5f hdg=%.1f alt=%.1f pitch=%.2f roll=%.2f" % [
		tag, ac.global_position.length(), g.z, d_m, up_dot, hdg, node_pitch, node_roll, jsb_lat, jsb_lon, jsb_hdg, jsb_alt, jsb_pitch, jsb_roll])
	if absf(node_pitch - jsb_pitch) > 1.0 or absf(node_roll - jsb_roll) > 1.0:
		_fail("%s: attitude mapping: node pitch/roll %.2f/%.2f vs JSBSim %.2f/%.2f" % [tag, node_pitch, node_roll, jsb_pitch, jsb_roll])
	if d_m > 50.0:
		_fail("%s: aircraft %.1f m from the target" % [tag, d_m])
	if g.z < 380.0 or g.z > 480.0:
		_fail("%s: aircraft height %.1f m implausible for LSZH" % [tag, g.z])
	if up_dot < 0.99:
		_fail("%s: aircraft up axis far from the local normal (dot %.4f)" % [tag, up_dot])
	if absf(fposmod(hdg - HDG + 180.0, 360.0) - 180.0) > 5.0:
		_fail("%s: node heading %.1f != %.1f" % [tag, hdg, HDG])
	if absf(jsb_lat - target_ll.x) > 0.001 or absf(jsb_lon - target_ll.y) > 0.001:
		_fail("%s: JSBSim lat/lon %.4f/%.4f != target %.4f/%.4f" % [tag, jsb_lat, jsb_lon, target_ll.x, target_ll.y])
	if absf(fposmod(jsb_hdg - HDG + 180.0, 360.0) - 180.0) > 5.0:
		_fail("%s: JSBSim heading %.1f != %.1f" % [tag, jsb_hdg, HDG])
	if absf(jsb_alt - g.z) > 5.0:
		_fail("%s: JSBSim altitude %.1f vs node height %.1f" % [tag, jsb_alt, g.z])

func _finish() -> void:
	if _fails.is_empty():
		print("smoke_ecef_spawn: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_ecef_spawn: %d failure(s)" % _fails.size())
		get_tree().quit(1)
