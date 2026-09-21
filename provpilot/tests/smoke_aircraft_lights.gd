extends Node
## Aircraft exterior lights (aircraft.cfg [lights]): the A-10's fixtures are
## found in the model, the L switch swaps the nose-gear lit/unlit meshes and
## the spot lights, strobes flash only when airborne and stop with the gear
## logic. Headless (no GPU).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_aircraft_lights.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	var L: AircraftLights = root._aircraft_lights
	_check(L != null, "AircraftLights node exists")
	if L == null:
		_finish(); return
	_check(L.fixture_count("nav_left") == 1 and L.fixture_count("nav_right") == 1, "wingtip nav lights found")
	_check(L.fixture_count("strobe") == 3, "three strobes found (got %d)" % L.fixture_count("strobe"))
	_check(L.fixture_count("beacon") == 1, "beacon found")
	_check(L.fixture_count("landing") == 1 and L.fixture_count("taxi") == 1, "nose-gear landing and taxi lights found")
	var on_mesh: Node3D = root.get_node(NodePath(root._model_root_path)).find_child("UC-NoseStrutTop-light-on", true, false)
	var off_mesh: Node3D = root.get_node(NodePath(root._model_root_path)).find_child("UC-NoseStrutTop-light-off", true, false)
	_check(on_mesh != null and off_mesh != null, "nose strut lit/unlit mesh pair present")
	# the lens rides on the nose gear through the FlightGear gear animation
	# (JSBSim gear/unit[0]/pos-norm = 1 parked): it hangs below the fuselage datum
	if on_mesh:
		var lens_p: Vector3 = on_mesh.global_transform * (on_mesh as MeshInstance3D).get_aabb().get_center()
		var body_p: Vector3 = root.get_node("AC").global_position
		var frame: Basis = root.get_node("AC").global_transform.basis
		# the fuselage datum sits [model] offset (loader units x 0.2 m = 2 AC units each) above the AC origin
		var offs: Array = root._aircraft_cfg.get_float_array("model", "offset", [0.0, 0.0, 0.0])
		var below: float = (lens_p - body_p).dot(-frame.y.normalized()) + float(offs[1]) * 2.0
		_check(below > 0.3, "landing lens hangs below the fuselage datum with the gear down (%.2f AC units)" % below)
		var fga: FGAnimator = root._fg_animator
		_check(fga != null and fga.animation_count() > 300, "FlightGear animator running on the A-10 (%d animations)" % (fga.animation_count() if fga else 0))
		if fga:
			print("  [diag] unbridged properties: %d of %d" % [fga.unknown_properties().size(), fga.property_names().size()])
			print("  [diag] gear pos-norm from JSBSim: ", fga.get_property("gear/gear[0]/position-norm"), " rudder ", fga.get_property("surface-positions/rudder-pos-norm"), " comp ", fga.get_property("gear/gear[0]/compression-norm"))
			var mroot0: Node = root.get_node(NodePath(root._model_root_path))
			var ac_inv0: Transform3D = root.get_node("AC").global_transform.affine_inverse()
			for nm in ["UC-NoseMain", "UC-NoseLow", "UC-Nose", "UC-LeftTop", "UC-NoseStrutTop-light-on"]:
				var tt: Transform3D = fga.animated_transform(nm)
				var nn: Node3D = mroot0.find_child(nm, true, false)
				var gp: Vector3 = ac_inv0 * nn.global_position if nn else Vector3.INF
				print("  [diag] %-26s anim angle %.1f origin %s | AC pos %s parent %s" % [nm, rad_to_deg(tt.basis.get_rotation_quaternion().get_angle()), str(tt.origin), str(gp), nn.get_parent().name if nn else "-"])
	# every mesh must lie within the airframe (AC units 0.1 m: 17.5 m span, 16 m length,
	# 2.1 m offset): parts posed in a wrong frame land on the runway or in the air
	var stray: Array = []
	var ac_inv1: Transform3D = root.get_node("AC").global_transform.affine_inverse()
	for n in root.get_node(NodePath(root._model_root_path)).find_children("*", "MeshInstance3D", true, false):
		if not (n as MeshInstance3D).mesh or not n.is_visible_in_tree():
			continue
		var c: Vector3 = ac_inv1 * (n.global_transform * (n as MeshInstance3D).get_aabb().get_center())
		if absf(c.x) > 95.0 or c.y < -5.0 or c.y > 60.0 or c.z > 5.0 or c.z < -170.0:
			stray.append([n.name, c])
	_check(stray.is_empty(), "all meshes inside the airframe box (%d stray: %s)" % [stray.size(), str(stray.slice(0, 5))])
	# diagnostics: extents of the nose gear parts in the AC frame (x right?, y up, z nose)
	var ac_inv: Transform3D = root.get_node("AC").global_transform.affine_inverse()
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	for nm in ["UC-NoseStrutTop", "UC-NoseStrutSteerRing", "UC-NoseStrutTop-light-on", "UC-NoseStrutSteerRing-light-on", "UC-NoseStrutBottom", "L-NavLight", "L-StrobeLight-off", "TopLightLens"]:
		var n: Node = mroot.find_child(nm, true, false)
		if n is MeshInstance3D and (n as MeshInstance3D).mesh:
			var bb: AABB = (n as MeshInstance3D).get_aabb()
			var lo := Vector3(INF, INF, INF)
			var hi := -lo
			for i in range(8):
				var w: Vector3 = ac_inv * (n.global_transform * bb.get_endpoint(i))
				lo = Vector3(minf(lo.x, w.x), minf(lo.y, w.y), minf(lo.z, w.z))
				hi = Vector3(maxf(hi.x, w.x), maxf(hi.y, w.y), maxf(hi.z, w.z))
			print("  [diag] %-32s AC x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f" % [nm, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z])
	var wheel_lo := INF
	for n in mroot.find_children("*", "MeshInstance3D", true, false):
		var lnm: String = n.name.to_lower()
		if ("nose" in lnm) and ("tyre" in lnm or "wheel" in lnm or "tire" in lnm) and (n as MeshInstance3D).mesh:
			var bb: AABB = (n as MeshInstance3D).get_aabb()
			for i in range(8):
				wheel_lo = minf(wheel_lo, (ac_inv * (n.global_transform * bb.get_endpoint(i))).y)
			print("  [diag] nose wheel mesh: ", n.name)
	print("  [diag] nose wheel lowest AC y %.2f" % wheel_lo)
	# sprites sit OUT_OFFSET_M outside the lens: the left nav sprite is outboard (+x) of its lens
	var nav_l: Node = mroot.find_child("L-NavLight", true, false)
	if nav_l is MeshInstance3D:
		var lens_ac: Vector3 = ac_inv * (nav_l.global_transform * (nav_l as MeshInstance3D).get_aabb().get_center())
		var spr: MeshInstance3D = nav_l.get_node("LightSprite_nav_left")
		var spr_ac: Vector3 = ac_inv * (spr.global_transform * spr.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0])
		var scale_ac: float = (ac_inv.basis * root.get_node("AC").global_transform.basis.x).length()
		var lens_w: Vector3 = nav_l.global_transform * (nav_l as MeshInstance3D).get_aabb().get_center()
		var spr_w: Vector3 = spr.global_transform * spr.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0]
		print("  [diag] nav sprite offset in world: %.3f m; AC basis scale %s; lens basis scale %s; AC unit = %.3f m" % [
			(spr_w - lens_w).length(), root.get_node("AC").global_transform.basis.get_scale(), nav_l.global_transform.basis.get_scale(),
			(root.get_node("AC").global_transform.basis * Vector3(1, 0, 0)).length()])
		var nav_r: MeshInstance3D = mroot.find_child("R-NavLight", true, false)
		var lens_r_w: Vector3 = nav_r.global_transform * nav_r.get_aabb().get_center()
		print("  [diag] wingtip lens distance in world: %.2f m (A-10 span 17.5 m); in AC units %.2f" % [(lens_w - lens_r_w).length(), (lens_ac - (ac_inv * lens_r_w)).length()])
		_check(spr_ac.x > lens_ac.x + 0.5 * scale_ac and spr_ac.x < lens_ac.x + 1.5 * scale_ac and absf(spr_ac.y - lens_ac.y) < 0.05 * scale_ac, "left nav sprite sits outboard of its lens (dx %.3f AC units)" % (spr_ac.x - lens_ac.x))
	# parked: nav on, strobes off, landing off
	var fwd := Vector3(0, 0, -1)
	var down := Vector3(0, -1, 0)
	L.update(0.0, false, true, 1.0, fwd, down)
	_check(L.is_lit("nav_left") and L.is_lit("nav_right"), "nav lights on while parked")
	_check(not L.is_lit("landing"), "landing lights off by default")
	if on_mesh and off_mesh:
		_check(off_mesh.visible, "unlit nose-strut lens shown while off (FlightGear select)")
	var strobe_hits := 0
	for i in range(120):
		L.update(0.02, false, true, 1.0, fwd, down)
		if L.is_lit("strobe"):
			strobe_hits += 1
	_check(strobe_hits == 0, "strobes stay off on the ground")
	strobe_hits = 0
	var beacon_hits := 0
	for i in range(120):
		L.update(0.02, true, true, 1.0, fwd, down)
		if L.is_lit("strobe"):
			strobe_hits += 1
		if L.is_lit("beacon"):
			beacon_hits += 1
	_check(strobe_hits > 0 and strobe_hits < 40, "strobes flash when airborne (%d of 120 ticks lit)" % strobe_hits)
	_check(beacon_hits > 0 and beacon_hits < 60, "beacon flashes (%d of 120 ticks lit)" % beacon_hits)
	# L switch (the FlightGear select on the lens pair runs in the host's frame loop)
	L.set_landing_lights(true)
	L.update(0.0, false, true, 1.0, fwd, down)
	await get_tree().process_frame
	await get_tree().process_frame
	_check(L.is_lit("landing") and L.is_lit("taxi"), "L switches landing and taxi lights on")
	if on_mesh and off_mesh:
		_check(not off_mesh.visible, "unlit nose-strut lens hidden when on (FlightGear select)")
	var spot: SpotLight3D = L.get_node_or_null("Spot_landing")
	_check(spot != null and spot.visible, "landing spot light present and visible")
	if spot:
		var aim: Vector3 = -spot.global_transform.basis.z
		var fwd_ac: Vector3 = root.get_node("AC").global_transform.basis.z.normalized()   # the host re-aims every frame along the AC nose (+Z)
		_check(aim.dot(fwd_ac) > 0.9, "landing spot aims along the nose (dot %.3f)" % aim.dot(fwd_ac))
		var d: float = (spot.global_position - on_mesh.global_position).length() if on_mesh else INF
		_check(d < 2.0, "landing spot sits at the nose strut light (%.2f m)" % d)
	L.update(0.0, true, false, 1.0, fwd, down)
	_check(not L.is_lit("landing"), "landing lights out with the gear retracted")
	_finish()

func _finish() -> void:
	if _fails.is_empty():
		print("smoke_aircraft_lights: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_aircraft_lights: %d failure(s)" % _fails.size())
		get_tree().quit(1)
