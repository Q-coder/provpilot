extends Node
## Smoke: a second Fly! to a region whose tiles are not loaded yet must keep
## JSBSim pinned until the PRECISE scenery tile is in, then release the
## aircraft on it — no drop from the coarse globe height, no bounce, no NaN.
## Run: godot --headless --path provpilot res://tests/smoke_teleport_far.tscn
var _root: Node = null
var _fails: Array[String] = []
func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)  # LSZH
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var jsb = _root.jsb_node
	# Geneva (LSGG): ~230 km away, nothing streamed there yet.
	var lv: Vector2 = Geo.wgs84_to_lv95(46.2381, 6.1090)
	var t0 := Time.get_ticks_msec()
	_root._on_welcome_flight_requested("c172p", lv.x, lv.y, 45.0)
	# Wait for the release (slew off) or the fallback timeout.
	waited = 0.0
	while jsb.is_slew_mode() and waited < 30.0:
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
		if int(waited * 4) % 8 == 0:
			var cam := get_viewport().get_camera_3d()
			var cg: Vector3 = _root._geodetic_of(cam.global_position) if cam else Vector3.ZERO
			var ag: Vector3 = _root._geodetic_of(_root.get_node("AC").global_position)
			print("[far] t=%.1f cam=%s at (%.3f, %.3f, %.0f m) AC at (%.3f, %.3f, %.0f m) fly_loading=%s intro=%s busy=%s" % [waited, cam.name if cam else "none", cg.x, cg.y, cg.z, ag.x, ag.y, ag.z, _root._fly_loading, _root._intro_playing, _root._scenery_streamer_busy()])
	var released_after := (Time.get_ticks_msec() - t0) / 1000.0
	var g: Vector3 = _root._geodetic_of(_root.get_node("AC").global_position)
	var precise: float = _root._scenery_elevation_geodetic(g.x, g.y)
	var globe: float = _root._globe_elevation_geodetic(g.x, g.y)
	print("[far] released after %.1f s: AC h=%.1f m, precise terrain=%.1f m, globe=%.1f m" % [released_after, g.z, precise, globe])
	if jsb.is_slew_mode():
		_fail("JSBSim never released")
	if is_nan(precise):
		_fail("precise tile still not loaded at release")
	elif absf(g.z - precise) > 5.0:
		_fail("aircraft released %.1f m from the precise ground (globe was %.1f m)" % [g.z - precise, globe - precise])
	# Settle: still on the ground, finite, no bounce.
	var max_agl := -1e9
	var max_vs := 0.0
	for i in range(40):
		await get_tree().create_timer(0.25).timeout
		var p: Vector3 = _root.get_node("AC").global_position
		if not p.is_finite():
			_fail("aircraft position went non-finite"); break
		var gg: Vector3 = _root._geodetic_of(p)
		var h: float = _root._scenery_elevation_geodetic(gg.x, gg.y)
		if not is_nan(h):
			max_agl = maxf(max_agl, gg.z - h)
		max_vs = maxf(max_vs, absf(jsb.get_vertical_speed_fpm()))
	print("[far] settle: max AGL %.1f m, max |vs| %.0f fpm, speed %.1f kts" % [max_agl, max_vs, jsb.get_airspeed_knots()])
	if max_agl > 6.0: _fail("aircraft bounced / floated to %.1f m AGL" % max_agl)
	if max_vs > 400.0: _fail("vertical speed spike %.0f fpm" % max_vs)
	if jsb.get_airspeed_knots() > 12.0: _fail("aircraft moving at %.1f kts after teleport (idle creep is ~5 kts)" % jsb.get_airspeed_knots())
	if _fails.is_empty():
		print("smoke_teleport_far: OK"); get_tree().quit(0)
	else:
		printerr("smoke_teleport_far: %d failure(s)" % _fails.size()); get_tree().quit(1)
