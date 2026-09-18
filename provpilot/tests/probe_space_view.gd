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
	var cam: Camera3D = _root._intro_camera
	var ac: Node3D = _root.get_node("AC")
	for a in alts_s.split(","):
		var alt := float(a)
		var up: Vector3 = ac.global_position.normalized()
		var g: Vector3 = _root._geodetic_of(ac.global_position)
		var n: Vector3 = W3DCoords.geodetic_normal(g.x, g.y)
		var pos: Vector3 = ac.global_position + n * alt
		var east := Vector3(0, 0, 1).cross(n).normalized()
		var north := n.cross(east).normalized()
		cam.global_transform = Transform3D(Basis.looking_at(-n, north), pos)
		cam.far = 5.0e7
		cam.current = true
		await get_tree().create_timer(1.0).timeout
		var env: Environment = _root.get_node("Environment/WorldEnvironment").environment
		print("[space] alt " + str(int(alt / 1000.0)) + " km: sun vis=" + str(sun.visible) + " energy=" + str(sun.light_energy) + " color=" + str(sun.light_color) + " shadow=" + str(sun.shadow_enabled) + " ambient=" + str(env.ambient_light_energy) + " fog=" + str(env.fog_enabled) + " density=" + str(env.fog_density) + " ssao=" + str(env.ssao_enabled) + " ssil=" + str(env.ssil_enabled) + " far=" + str(cam.far) + " near=" + str(cam.near) + " sun-z=" + str(sun.global_transform.basis.z))
		var img := get_viewport().get_texture().get_image()
		img.save_png(out.path_join("space_%dkm.png" % int(alt / 1000.0)))
	get_tree().quit(0)
