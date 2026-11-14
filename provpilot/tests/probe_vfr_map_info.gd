extends Node
## Probe: the VFR map's wide views (160 / 250 km), the km / NM labels and the
## details panel (a click on an airport or navaid symbol). At Copenhagen.
## Run NON-headless: godot --path provpilot res://tests/probe_vfr_map_info.tscn -- <out_dir>
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _shot(map, out_dir: String, name: String) -> void:
	await get_tree().create_timer(6.0).timeout
	var img: Image = get_viewport().get_texture().get_image()
	var r: Rect2 = map.get_global_rect()
	img.get_region(Rect2i(int(r.position.x), int(r.position.y), int(r.size.x), int(r.size.y))).save_png(out_dir.path_join(name))

func _click(pos: Vector2) -> void:
	for down in [true, false]:
		var ev := InputEventMouseButton.new()
		ev.button_index = MOUSE_BUTTON_LEFT
		ev.pressed = down
		ev.position = pos
		ev.global_position = pos
		get_viewport().push_input(ev)
		await get_tree().process_frame

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir: String = args[0] if args.size() > 0 else "/tmp"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var lv: Vector2 = Geo.wgs84_to_lv95(55.625446, 12.667644)
	root._on_welcome_flight_requested("c172p", lv.x, lv.y, 221.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var map = root._vfr_map
	var was_enabled: bool = map.is_map_enabled()
	if not was_enabled:
		map.toggle_visible()
	var zoom0: int = map.get_zoom_index()
	for i in range(8):
		map.zoom_out()
	_check(map.get_zoom_index() == 6 and absf(map.get_view_width_m() - 250000.0) < 1.0, "the widest view is 250 km (index %d)" % map.get_zoom_index())
	await _shot(map, out_dir, "vfr_map_250km.png")
	map.zoom_in()
	await _shot(map, out_dir, "vfr_map_160km.png")
	while map.get_zoom_index() > 3:
		map.zoom_in()
	await get_tree().create_timer(1.0).timeout
	var w0: float = map.get_global_rect().size.x
	# a real click on the EKCH symbol
	var ap_px: Vector2 = map.get_projected_px(Geo.wgs84_to_lv95(55.6179, 12.656).x, Geo.wgs84_to_lv95(55.6179, 12.656).y)
	await _click(map.get_global_rect().position + ap_px)
	await get_tree().create_timer(0.5).timeout
	var txt: String = map.get_info_text()
	print("  [diag] info text:\n" + txt)
	_check(txt.contains("EKCH") and txt.contains("Copenhagen"), "a click on the airport symbol opens its details")
	_check(txt.contains("22L  109.50") and txt.contains("22R  110.90"), "the panel lists the ILS frequencies of both 22 runways")
	_check(txt.contains("NAVAIDS"), "... and the navaids around the field")
	_check(map.get_global_rect().size.x > w0 + 200.0, "the frame widens for the panel (%.0f -> %.0f)" % [w0, map.get_global_rect().size.x])
	await _shot(map, out_dir, "vfr_map_info_airport.png")
	# a click on bare map closes it
	await _click(map.get_global_rect().position + Vector2(30, 60))
	await get_tree().create_timer(0.5).timeout
	_check(map.get_info_text() == "" and absf(map.get_global_rect().size.x - w0) < 1.0, "a click on the bare map closes the panel")
	# back to the pilot's own zoom
	while map.get_zoom_index() < zoom0:
		map.zoom_out()
	while map.get_zoom_index() > zoom0:
		map.zoom_in()
	if not was_enabled:
		map.toggle_visible()
	print("probe_vfr_map_info: %s" % ("OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails > 0 else 0)
