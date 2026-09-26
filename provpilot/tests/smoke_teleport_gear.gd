extends Node
## Smoke: Fly! after a gear-up flight in a retractable airframe (A-10, 5 s
## gear kinematic). JSBSim computes no ground contact while the gear is in
## transit, so the session reset must snap gear-pos-norm to 1 — otherwise the
## aircraft falls through the runway and bounces (Dübendorf 2026-09-18).
## Run: godot --headless --path provpilot res://tests/smoke_teleport_gear.tscn
var _root: Node = null
var _fails: Array[String] = []
func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("a10", 2683732.0, 1257695.0, 160.0)  # LSZH
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var jsb = _root.jsb_node
	if jsb.get_property("gear/gear-pos-norm") < 0.99:
		_fail("A-10 should start with the gear down (pos %.2f)" % jsb.get_property("gear/gear-pos-norm"))
	# Gear up (as in flight), give the kinematic a moment so it is in transit.
	_root._cockpit_controls.set_gear_down(false)
	_root._control_surfaces.set_gear_down(false)
	jsb.set_gear_down(false)
	await get_tree().create_timer(1.5).timeout
	print("[gear] before Fly!: gear-pos-norm %.2f" % jsb.get_property("gear/gear-pos-norm"))
	# Fly! to Dübendorf.
	var lv: Vector2 = Geo.wgs84_to_lv95(47.399, 8.648)
	_root._on_welcome_flight_requested("a10", lv.x, lv.y, 110.0)
	waited = 0.0
	while jsb.is_slew_mode() and waited < 40.0:
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
	var pos_at_release: float = jsb.get_property("gear/gear-pos-norm")
	print("[gear] released after %.1f s: gear-pos-norm %.2f cmd %.2f visual %.2f" % [waited, pos_at_release, jsb.get_property("gear/gear-cmd-norm"), _root._control_surfaces.get_gear_visual_pos()])
	if jsb.is_slew_mode(): _fail("JSBSim never released")
	if pos_at_release < 0.99: _fail("gear position not snapped down at release (%.2f)" % pos_at_release)
	if _root._control_surfaces.get_gear_visual_pos() < 0.99: _fail("gear visual not down at release")
	var max_agl := -1e9
	var min_agl := 1e9
	var max_vs := 0.0
	for i in range(40):
		await get_tree().create_timer(0.25).timeout
		var p: Vector3 = _root.get_node("AC").global_position
		if not p.is_finite():
			_fail("aircraft position went non-finite"); break
		var gg: Vector3 = _root._world.geodetic_of(p)
		var h: float = _root._world.scenery_elevation_geodetic(gg.x, gg.y)
		if not is_nan(h):
			max_agl = maxf(max_agl, gg.z - h)
			min_agl = minf(min_agl, gg.z - h)
		max_vs = maxf(max_vs, absf(jsb.get_vertical_speed_fpm()))
	print("[gear] settle: AGL %.1f..%.1f m, max |vs| %.0f fpm, speed %.1f kts, gear-pos %.2f" % [min_agl, max_agl, max_vs, jsb.get_airspeed_knots(), jsb.get_property("gear/gear-pos-norm")])
	if max_agl > 6.0: _fail("aircraft bounced to %.1f m AGL" % max_agl)
	if min_agl < -3.0: _fail("aircraft sank to %.1f m AGL" % min_agl)
	if max_vs > 600.0: _fail("vertical speed spike %.0f fpm" % max_vs)
	if _fails.is_empty():
		print("smoke_teleport_gear: OK"); get_tree().quit(0)
	else:
		printerr("smoke_teleport_gear: %d failure(s)" % _fails.size()); get_tree().quit(1)
