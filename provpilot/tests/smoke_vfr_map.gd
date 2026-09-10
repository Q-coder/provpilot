extends Node
## Headless smoke test for the C++ VfrMap module (src/vfr_map.cpp).
## Hermetic: no JSBSim / Scenery3D / scenery data required. Generates a small
## synthetic tile pyramid in a temp dir, points the module at it, and drives
## update() with known positions/headings near LSZH. Exits 0 on success,
## 1 on any failure.

const TIMEOUT_FRAMES := 9000  # ~150 s at 60 fps (headless loading can be slow)

# LV95 around Zurich (LSZH is ~E 2683400 N 1282700).
const AC_E := 2683400.0
const AC_N := 1282700.0

var app: Node = null
var vfr_map: VfrMap = null
var frame := 0
var phase := "boot"
var phase_frame := 0
var fails: Array[String] = []
var tmp_dir := ""


func _ready() -> void:
	app = load("res://node_3d.tscn").instantiate()
	add_child(app)


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


## Build a synthetic tile pyramid around AC_E/AC_N: the L0 (1024 m) grid cells
## covering the default 20 km view plus margin, and the mip3 (8192 m) cells for
## the wider zoom steps. File names encode the SW corner of each cell.
func _make_tiles() -> void:
	DirAccess.make_dir_recursive_absolute("/tmp/vfr_map_smoke")
	tmp_dir = "/tmp/vfr_map_smoke"
	var rng := RandomNumberGenerator.new()
	rng.seed = 42

	# L0 cells around the aircraft (ei 2619..2630, ni 1251..1262).
	for ni in range(1251, 1263):
		for ei in range(2619, 2631):
			_write_l0_tile(ei * 1024, ni * 1024, rng)

	# mip3 cells covering the same area (8192 m cells).
	for ni3 in range(155, 157):
		for ei3 in range(326, 329):
			_write_mip3_tile(ei3 * 8192, ni3 * 8192, rng)


func _write_l0_tile(e: int, n: int, rng: RandomNumberGenerator) -> void:
	var img := Image.create(64, 64, false, Image.FORMAT_RGB8)
	for y in range(64):
		for x in range(64):
			# Diagonal ramp + noise so the texture is non-uniform.
			var v := (x + y) / 127.0 * 200.0 + rng.randf_range(-20.0, 20.0)
			img.set_pixel(x, y, Color(v / 255.0, v / 300.0, 0.4))
	var path := tmp_dir.path_join("map_%d_%d.jpg" % [e, n])
	img.save_jpg(path, 0.8)


func _write_mip3_tile(e: int, n: int, rng: RandomNumberGenerator) -> void:
	var img := Image.create(64, 64, false, Image.FORMAT_RGB8)
	for y in range(64):
		for x in range(64):
			var v := (x * 2 + y) / 190.0 * 255.0
			img.set_pixel(x, y, Color(v / 255.0, 0.3, v / 255.0))
	var path := tmp_dir.path_join("mip3/map_%d_%d.jpg" % [e, n])
	DirAccess.make_dir_recursive_absolute(tmp_dir.path_join("mip3"))
	img.save_jpg(path, 0.8)


func _enter_phase(name: String) -> void:
	phase = name
	phase_frame = frame


func _process(_delta: float) -> void:
	frame += 1
	if frame >= TIMEOUT_FRAMES:
		fail("timed out in phase '%s'" % phase)
		return
	if app == null:
		return

	match phase:
		"boot":
			vfr_map = app.get_node_or_null("Control/VfrMap")
			check(vfr_map != null, "VfrMap node exists in the HUD scene tree")
			if vfr_map == null:
				fail("no VfrMap node")
			else:
				_make_tiles()
				# One-time wiring exactly as node_3d.gd does it.
				vfr_map.setup(2682880.0, 1283072.0, "/tmp/vfr_map_smoke/no_cfg.cfg")
				vfr_map.set_tile_paths(PackedStringArray([tmp_dir]))
				_enter_phase("drive")

		"drive":
			# Drive the module like node_3d.gd does: Godot-space position for
			# AC_E/AC_N (swiss_e = center_e - x  =>  x = center_e - swiss_e).
			var godot_pos := Vector3(2682880.0 - AC_E, 500.0, AC_N - 1283072.0)
			vfr_map.update(godot_pos, 250.0, false)
			if frame - phase_frame >= 30:
				_enter_phase("check_tiles")

		"check_tiles":
			var cached := vfr_map.get_cached_tile_count()
			check(cached > 0, "map tiles were streamed into the LRU cache (got %d)" % cached)
			check(vfr_map.get_visible_airport_count() > 0,
				"in-view airports detected near LSZH (got %d)" % vfr_map.get_visible_airport_count())
			# Re-issue a known state synchronously: node_3d.gd's own _process
			# overwrites position/heading with JSBSim's every frame.
			var godot_pos := Vector3(2682880.0 - AC_E, 500.0, AC_N - 1283072.0)
			vfr_map.update(godot_pos, 250.0, false)
			check(absf(vfr_map.get_heading_rad() - deg_to_rad(250.0)) < 0.01,
				"heading was propagated to the module")
			var z_before := vfr_map.get_zoom_index()
			vfr_map.zoom_out()
			check(vfr_map.get_zoom_index() == min(4, z_before + 1), "zoom_out advances the zoom index")
			vfr_map.zoom_in()
			check(vfr_map.get_zoom_index() == z_before, "zoom_in restores the zoom index")
			_enter_phase("check_toggle")

		"check_toggle":
			var was_enabled := vfr_map.is_map_enabled()
			vfr_map.toggle_visible()
			check(vfr_map.is_map_enabled() == (not was_enabled), "toggle_visible flips the enabled flag")
			check(vfr_map.visible == vfr_map.is_map_enabled(), "frame visibility follows the enabled flag")
			vfr_map.toggle_visible()
			_enter_phase("done")

		"done":
			if fails.is_empty():
				print("SMOKE RESULT: PASS (all VFR map checks ok)")
			else:
				printerr("SMOKE RESULT: FAIL (%d failure%s)" % [fails.size(), "s" if fails.size() != 1 else ""])
			get_tree().quit(0 if fails.is_empty() else 1)


func fail(msg: String) -> void:
	fails.append(msg)
	printerr("SMOKE FAIL: ", msg)
	get_tree().quit(1)


