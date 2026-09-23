extends Node
## Welcome "Edit Mode": straight into the scenery editor over the selected
## airport with no aircraft and no intro zoom; leaving it returns to the
## welcome screen, and a Fly! afterwards still starts a normal flight.
## Run: PVP_DESIGN_FILE=/tmp/x.json godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_edit_mode.tscn
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
	if OS.get_environment("PVP_DESIGN_FILE") == "":
		OS.set_environment("PVP_DESIGN_FILE", OS.get_user_data_dir().path_join("smoke_edit_mode.json"))
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var ed: SceneryEditor = root._scenery_editor
	root._on_welcome_edit_requested(E, N, 155.0)
	await get_tree().process_frame
	_check(ed.is_active(), "editor active right after the Edit Mode request")
	_check(not root.terrain_initialized and not root._intro_playing and not root._fly_loading, "no flight started: no terrain init, no intro zoom, no fly loading")
	_check(get_viewport().get_camera_3d() == ed._cam, "editor camera drives the viewport (and the tile streamer)")
	var ac: Node3D = root.get_node("AC")
	_check(not ac.visible, "aircraft hidden")
	_check(root.jsb_node == null or root.jsb_node.is_slew_mode(), "flight model kept in slew")
	var g: Vector3 = W3DCoords.ecef_to_geodetic(ed._cam.global_position)
	var en: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
	print("  [diag] editor camera at E %.0f N %.0f h %.0f" % [en.x, en.y, g.z])
	_check(en.distance_to(Vector2(E, N)) < 50.0, "camera over the selected airport")
	# scenery streams in around the editor camera: precise terrain appears
	var waited := 0.0
	while is_nan(root._scenery_elevation_geodetic(g.x, g.y)) and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	var h: float = root._scenery_elevation_geodetic(g.x, g.y)
	print("  [diag] precise terrain under the camera after %.1f s: %.1f m" % [waited, h])
	_check(not is_nan(h) and h > 300.0 and h < 600.0, "Swiss terrain streamed in under the editor camera")
	for i in range(3):
		ed.update(0.016)
		await get_tree().process_frame
	var hit: Dictionary = ed.cursor_hit_from_screen(get_viewport().get_visible_rect().size * 0.5)
	_check(not hit.is_empty(), "cursor hits the terrain")
	# leave: back to the welcome screen
	ed.exit()
	await get_tree().process_frame
	_check(not ed.is_active() and root._welcome_screen.visible, "leaving Edit Mode shows the welcome screen again")
	# a normal flight still works afterwards
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	waited = 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	_check(root.terrain_initialized, "Fly! after an edit session initialises the flight")
	_check(ac.visible, "aircraft visible again for the flight")
	if _fails.is_empty():
		print("smoke_edit_mode: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_edit_mode: %d failure(s)" % _fails.size())
		get_tree().quit(1)
