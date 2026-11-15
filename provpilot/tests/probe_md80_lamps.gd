extends Node
## MD-80 landing lamps: the wing lamps swing out over 7 s; their sprite and
## beam must ride on the lamp (beam down when stowed, forward when out), and
## both nose gear lamps must burn. Night, real renderer, two screenshots.
## Run: godot --path provpilot res://tests/probe_md80_lamps.tscn -- <out_dir> [id]
const E := 2682767.0
const N := 1258797.0

func _report(tag: String, root: Node, ac: Node3D) -> Array:
	var L = root._aircraft_lights
	var jsb = root.jsb_node
	var inv: Transform3D = ac.global_transform.affine_inverse()
	var out: Array = []
	var k := 0
	for f in L.get_fixtures():
		if f.kind != "landing":
			continue
		var spot: SpotLight3D = f.spot
		var p: Vector3 = inv * (f.sprite.global_transform * f.sprite.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0])
		var d := Vector3.ZERO
		if spot != null:
			d = (inv.basis * (-spot.global_transform.basis.z)).normalized()
		print("probe_lamps: %s landing %d: level %.2f, lens at left %+.2f up %+.2f nose %+.2f m, beam nose %+.2f up %+.2f, lit %s" % [tag, k, L.get_fixture_level("landing", k), p.x * 0.1, p.y * 0.1, p.z * 0.1, d.z, d.y, str(spot != null and spot.visible)])
		out.append({"level": L.get_fixture_level("landing", k), "dir": d, "pos": p * 0.1, "lit": spot != null and spot.visible})
		k += 1
	print("probe_lamps: %s extension L %.2f R %.2f" % [tag, jsb.get_property("/systems/exterior-lights/left-landing-light-ext"), jsb.get_property("/systems/exterior-lights/right-landing-light-ext")])
	return out

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir: String = args[0] if args.size() > 0 else ""
	var id: String = args[1] if args.size() > 1 else "md80"
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
	root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
	root._on_welcome_flight_requested(id, E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._sky.set_time(false, 22 * 60)
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root._camera_rig.set_orbit_distance_min(30.0)
	root._camera_rig.set_orbit_distance_max(30.0)
	root._camera_rig.set_orbit_pitch_min(4.0)
	root._camera_rig.set_orbit_pitch_max(4.0)
	root._camera_rig.set_orbit_yaw(float(args[2]) if args.size() > 2 else 200.0)
	var ac: Node3D = root.get_node("AC")
	var fgc = root._aircraft.get_fg_controls()
	var cs: Dictionary = fgc.get_state()
	var sw := ["controls/lighting/landing-light-l", "controls/lighting/landing-light-r", "controls/lighting/landing-light-n"]
	for k in sw:
		cs[k] = 0.0
	fgc.set_state(cs)
	for i in range(9):
		await get_tree().create_timer(1.0).timeout
		print("probe_lamps: stowing t+%d: switch state %s, FDM switch %.2f, ext-cmd %.2f, rate %.3f, ext %.2f, ac-l %.0f V" % [i + 1, str(fgc.value_of(sw[0])), root.jsb_node.get_property("/controls/lighting/landing-light-l"),
			root.jsb_node.get_property("/systems/exterior-lights/left-landing-light-ext-cmd"), root.jsb_node.get_property("/systems/exterior-lights/left-landing-light-ext-rate"), root.jsb_node.get_property("/systems/exterior-lights/left-landing-light-ext"), root.jsb_node.get_property("/systems/electrical/bus/ac-l")])
	_report("stowed", root, ac)
	cs = fgc.get_state()
	for k in sw:
		cs[k] = 1.0
	fgc.set_state(cs)
	for i in range(3):
		await get_tree().create_timer(0.5).timeout
		print("probe_lamps: extending t+%.1f: ext %.2f, lamp level %.2f, float7 %s" % [(i + 1) * 0.5, root.jsb_node.get_property("/systems/exterior-lights/left-landing-light-ext"), root._aircraft_lights.get_fixture_level("landing", 0), str(root._aircraft.get_fg_property("sim/multiplay/generic/float[7]"))])
	var mid := _report("half out", root, ac)
	if out_dir != "":
		get_viewport().get_texture().get_image().save_png(out_dir.path_join("lamps_%s_mid.png" % id))
	await get_tree().create_timer(4.0).timeout
	var full := _report("out", root, ac)
	if out_dir != "":
		get_viewport().get_texture().get_image().save_png(out_dir.path_join("lamps_%s_out.png" % id))
	var ok: bool = full.size() == 4
	if ok:
		for i in range(4):
			ok = ok and full[i]["lit"] and full[i]["dir"].z > 0.95
		ok = ok and mid[0]["lit"] and mid[0]["dir"].y < -0.3 and mid[0]["dir"].z > 0.3 and mid[2]["lit"] and mid[3]["lit"]
		ok = ok and absf(full[2]["pos"].x + full[3]["pos"].x) < 0.02 and absf(full[2]["pos"].x) > 0.1
	print(("  PASS: " if ok else "  FAIL: ") + "%s: four landing lamps, the wing beams swing with the lamps, both nose lamps burn" % id)
	get_tree().quit(0 if ok else 1)
