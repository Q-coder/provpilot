extends Node
## Probe: after a flight is set up, park the intro camera at several altitudes
## straight above the aircraft looking down and save screenshots + light state.
## Run (windowed): godot --path provpilot res://tests/probe_space_view.tscn
## Env: PROBE_OUT dir, PROBE_ALTS "1500000,600000,300000,150000,60000",
##      PROBE_NOSHADOW=1 (sun shadows off), PROBE_NOGLOBE=1 (hide the globe).
var _root: Node = null
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(10.0).timeout
	_root._cancel_intro_zoom()
	if _root._welcome_screen:
		_root._welcome_screen.visible = false
	if _root._loading_screen:
		_root._loading_screen.visible = false
	await get_tree().process_frame
	var out := OS.get_environment("PROBE_OUT")
	if out == "":
		out = "user://"
	var alts_s := OS.get_environment("PROBE_ALTS")
	if alts_s == "":
		alts_s = "1500000,600000,300000,150000,60000"
	var sun: DirectionalLight3D = _root.get_node("Environment/Sun")
	if OS.get_environment("PROBE_NOSHADOW") == "1":
		sun.shadow_enabled = false
	if OS.get_environment("PROBE_NOGLOBE") == "1" and is_instance_valid(_root.globe_node):
		_root.globe_node.visible = false
	var env0: Environment = _root.get_node("Environment/WorldEnvironment").environment
	if OS.get_environment("PROBE_NOSSAO") == "1":
		env0.ssao_enabled = false
		env0.ssil_enabled = false
		env0.sdfgi_enabled = false
		env0.volumetric_fog_enabled = false
		env0.glow_enabled = false
	if OS.get_environment("PROBE_SUN_X") != "":
		_root._sky._base_sun_energy = float(OS.get_environment("PROBE_SUN_X"))
	if OS.get_environment("PROBE_NOFOG") == "1":
		_root._sky._base_fog_density = 0.0
	match OS.get_environment("PROBE_DEBUG_DRAW"):
		"lighting": get_viewport().debug_draw = Viewport.DEBUG_DRAW_LIGHTING
		"normals": get_viewport().debug_draw = Viewport.DEBUG_DRAW_NORMAL_BUFFER
		"unshaded": get_viewport().debug_draw = Viewport.DEBUG_DRAW_UNSHADED
	# PROBE_HIDE="roads,water,buildings,vegetation": hide scenery layers by
	# node-name substring (also prints the Scenery3D children once).
	var names: PackedStringArray = []
	for c in _root.scenery3d_node.get_children():
		names.append(c.name + ":" + c.get_class())
	print("[space] Scenery3D children: " + ", ".join(names))
	if OS.get_environment("PROBE_HIDE") != "":
		for key in OS.get_environment("PROBE_HIDE").split(","):
			for c in _root.scenery3d_node.get_children():
				if c.name.to_lower().contains(key.to_lower()) and c is Node3D:
					c.visible = false
					print("[space] hidden " + c.name)
			if key == "vegetation" and is_instance_valid(_root.vegetation_node):
				_root.vegetation_node.visible = false
	var cam: Camera3D = _root._intro_camera
	var ac: Node3D = _root.get_node("AC")
	var base_pos: Vector3 = ac.global_position
	if OS.get_environment("PROBE_LV95") != "":
		var en := OS.get_environment("PROBE_LV95").split(",")
		base_pos = _root._ecef_of_lv95(float(en[0]), float(en[1]), 450.0)
	var wait_s := float(OS.get_environment("PROBE_WAIT")) if OS.get_environment("PROBE_WAIT") != "" else 1.0
	for a in alts_s.split(","):
		var alt := float(a)
		var g: Vector3 = _root._geodetic_of(base_pos)
		var n: Vector3 = W3DCoords.geodetic_normal(g.x, g.y)
		var pos: Vector3 = base_pos + n * alt
		var east := Vector3(0, 0, 1).cross(n).normalized()
		var north := n.cross(east).normalized()
		cam.global_transform = Transform3D(Basis.looking_at(-n, north), pos)
		cam.far = 5.0e7
		cam.current = true
		await get_tree().create_timer(wait_s).timeout
		var env: Environment = _root.get_node("Environment/WorldEnvironment").environment
		print("[space] alt " + str(int(alt / 1000.0)) + " km: sun vis=" + str(sun.visible) + " energy=" + str(sun.light_energy) + " color=" + str(sun.light_color) + " shadow=" + str(sun.shadow_enabled) + " ambient=" + str(env.ambient_light_energy) + " fog=" + str(env.fog_enabled) + " density=" + str(env.fog_density) + " ssao=" + str(env.ssao_enabled) + " ssil=" + str(env.ssil_enabled) + " far=" + str(cam.far) + " near=" + str(cam.near) + " sun-z=" + str(sun.global_transform.basis.z))
		var img := get_viewport().get_texture().get_image()
		img.save_png(out.path_join("space_%dkm.png" % int(alt / 1000.0)))
	get_tree().quit(0)
