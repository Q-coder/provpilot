extends Node
## Probe: how fast the VFR map background fills (loader threads + disk cache)
## and the title-bar Terrain checkbox. At Frankfurt.
## Run NON-headless from a scratch PWD holding a copy of provpilot.cfg (the
## probe changes [map] keys): godot --path <repo>/provpilot res://tests/probe_vfr_map_fill.tscn -- <out_dir> [max_fill_s]
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _shot(map, out_dir: String, name: String) -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var img: Image = get_viewport().get_texture().get_image()
	var r: Rect2 = map.get_global_rect()
	img.get_region(Rect2i(int(r.position.x), int(r.position.y), int(r.size.x), int(r.size.y))).save_png(out_dir.path_join(name))

## Seconds until no cell is waiting any more (and the redraw that shows them ran).
func _fill_time(map) -> float:
	var t0 := Time.get_ticks_msec()
	await get_tree().create_timer(0.25).timeout
	while map.get_pending_tile_count() > 0 and Time.get_ticks_msec() - t0 < 180000:
		await get_tree().process_frame
	var t := float(Time.get_ticks_msec() - t0) / 1000.0
	await get_tree().create_timer(0.4).timeout
	return t

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
	var max_fill: float = float(args[1]) if args.size() > 1 else 30.0
	var tag: String = args[2] if args.size() > 2 else "run"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var lv: Vector2 = Geo.wgs84_to_lv95(50.0333, 8.5706)
	root._on_welcome_flight_requested("c172p", lv.x, lv.y, 250.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var map = root._vfr_map
	if not map.is_map_enabled():
		map.toggle_visible()
	if not map.is_background_enabled():
		map.set_background_enabled(true)
	while map.get_zoom_index() > 0:
		map.zoom_in()
	await _fill_time(map)
	for step in [[6, "250km"], [5, "160km"], [3, "40km"], [2, "20km"]]:
		while map.get_zoom_index() < step[0]:
			map.zoom_out()
		while map.get_zoom_index() > step[0]:
			map.zoom_in()
		var n0: int = map.get_cached_tile_count()
		var t: float = await _fill_time(map)
		print("FILL %s %s: %.2f s, %d new cells" % [tag, step[1], t, map.get_cached_tile_count() - n0])
		_check(t <= max_fill, "the %s view fills within %.0f s (%.2f s)" % [step[1], max_fill, t])
		await _shot(map, out_dir, "fill_%s_%s.png" % [tag, step[1]])
	# the Terrain checkbox in the title bar
	var box: Vector2 = map.get_global_rect().position + Vector2(86.0, 13.0)
	await _click(box)
	_check(not map.is_background_enabled(), "a click on the title-bar checkbox turns the terrain background off")
	await get_tree().create_timer(0.5).timeout
	await _shot(map, out_dir, "fill_%s_background_off.png" % tag)
	_check(map.get_visible_airport_count() > 0, "airports stay on the map without the background (%d)" % map.get_visible_airport_count())
	var cfg := ConfigFile.new()
	cfg.load(InstallRoot.get_config_path())
	_check(cfg.get_value("map", "map_background", true) == false, "the choice is saved in [map] map_background")
	await _click(box)
	_check(map.is_background_enabled(), "a second click turns it back on")
	print("PROBE RESULT: %s" % ("PASS" if _fails == 0 else "FAIL (%d)" % _fails))
	get_tree().quit(0 if _fails == 0 else 1)
