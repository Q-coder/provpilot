extends Node
## Headless smoke: the aircraft must be usable after the scenery editor.
## Two paths at LSPF: (a) welcome Edit Mode -> exit -> Fly!, (b) a flight ->
## F8 in, camera moved 2 km away, F8 out. After each the flight model must
## be out of slew, on its wheels, level and still, at the terrain height.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_editor_flight_exit.tscn
const E := 2681661.3
const N := 1282697.8
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)


func _aircraft_state(root: Node, label: String) -> void:
	var jsb = root.jsb_node
	var ac: Node3D = root.get_node("AC")
	var g: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_position)
	var terr: float = root._world.terrain_elevation_geodetic(g.x, g.y)
	var agl: float = g.z - terr if not is_nan(terr) else NAN
	print("  [diag] %s: slew %s wow %s pitch %.1f roll %.1f ias %.1f kt agl %.2f m alt %.0f ft" % [label, jsb.is_slew_mode(), jsb.get_weight_on_wheels(), jsb.get_pitch_deg(), jsb.get_roll_deg(), jsb.get_airspeed_knots(), agl, jsb.get_altitude_ft()])
	_check(not jsb.is_slew_mode(), "%s: flight model running (not slewed)" % label)
	_check(absf(jsb.get_pitch_deg()) < 8.0 and absf(jsb.get_roll_deg()) < 8.0, "%s: aircraft level (pitch %.1f roll %.1f)" % [label, jsb.get_pitch_deg(), jsb.get_roll_deg()])
	_check(jsb.get_airspeed_knots() < 5.0, "%s: aircraft still (%.1f kt)" % [label, jsb.get_airspeed_knots()])
	_check(not is_nan(agl) and agl > -1.0 and agl < 3.0, "%s: on the terrain (%.2f m)" % [label, agl])
	_check(jsb.get_weight_on_wheels(), "%s: weight on wheels" % label)


func _wait_flight(root: Node) -> void:
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	waited = 0.0
	while (root._fly_loading or root._intro_playing) and waited < 20.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	if root._fly_loading or root._intro_playing:
		root._cancel_intro_zoom()
		await get_tree().create_timer(2.0).timeout
		root._intro_playing = false
		root._fly_loading = false
	await get_tree().create_timer(4.0).timeout


func _ready() -> void:
	OS.set_environment("PVP_DESIGN_FILE", OS.get_user_data_dir().path_join("smoke_flight_exit.json"))
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var ed: SceneryEditor = root._scenery_editor
	# (a) Edit Mode from the welcome screen, then a flight
	root._on_welcome_edit_requested(E, N, 155.0)
	await get_tree().create_timer(6.0).timeout
	_check(ed.is_active(), "edit session entered")
	ed.exit()
	await get_tree().process_frame
	root._welcome_screen.hide_welcome()
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	await _wait_flight(root)
	_aircraft_state(root, "after Edit Mode + Fly!")
	# (b) F8 from the flight, wander 2 km, F8 back
	var before: Vector3 = root.get_node("AC").global_position
	ed.enter()
	await get_tree().process_frame
	_check(ed.is_active(), "F8 enters the editor from the flight")
	ed.camera_position = ed.world_of(E + 2000.0, N + 1500.0, 900.0)
	ed.apply_camera()
	for i in range(120):
		ed.update(0.05)
		await get_tree().process_frame
	ed.exit()
	await get_tree().create_timer(4.0).timeout
	var moved: float = root.get_node("AC").global_position.distance_to(before)
	print("  [diag] aircraft moved %.2f m across the F8 session" % moved)
	_check(moved < 1.0, "aircraft stays put across an F8 session (%.2f m)" % moved)
	_aircraft_state(root, "after F8 in/out")
	if _fails.is_empty():
		print("smoke_editor_flight_exit: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_editor_flight_exit: %d failure(s)" % _fails.size())
		get_tree().quit(1)
