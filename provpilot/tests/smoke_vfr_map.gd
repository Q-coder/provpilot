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
var zoom_before_key := 0


func _ready() -> void:
	# Parsed key events must be delivered per-event (not batched), so the
	# press/release pairs below reach VfrMap::_unhandled_input cleanly.
	Input.set_use_accumulated_input(false)
	app = load("res://node_3d.tscn").instantiate()
	add_child(app)
	# node_3d.gd feeds JSBSim's position into VfrMap every frame (0,0 at the
	# headless spawn point). Re-feed the known LSZH position after all
	# _process callbacks so tile streaming targets the synthetic pyramid.
	get_tree().process_frame.connect(_feed_known_position)


## Runs after every node's _process (incl. node_3d.gd's JSBSim feed), so the
## VfrMap state — and thus its 10 Hz redraws — always sees the LSZH position.
func _feed_known_position() -> void:
	if vfr_map != null and phase != "done":
		vfr_map.update(Vector3(2682880.0 - AC_E, 500.0, AC_N - 1283072.0), 250.0, false)


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


# This custom engine build does not expose the static Input.key_press/release
# helpers, so drive the Input singleton through parsed key events instead.
func press_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = true
	Input.parse_input_event(ev)


func release_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = false
	Input.parse_input_event(ev)


## Synthesize a mouse-wheel press+release at a viewport position. The GUI
## router hit-tests the event against the scene tree, so `at` must be inside
## the map frame (use vfr_map.get_global_rect().get_center()).
func wheel_event(btn: MouseButton, at: Vector2) -> void:
	for pressed in [true, false]:
		var ev := InputEventMouseButton.new()
		ev.button_index = btn
		ev.position = at
		ev.global_position = at
		ev.pressed = pressed
		Input.parse_input_event(ev)


## Build a synthetic tile pyramid around AC_E/AC_N: the L0 (1024 m) grid cells
## covering the default 20 km view plus margin, and the mip3 (8192 m) cells for
## the wider zoom steps. File names encode the SW corner of each cell.
func _make_tiles() -> void:
	DirAccess.make_dir_recursive_absolute("/tmp/vfr_map_smoke")
	tmp_dir = "/tmp/vfr_map_smoke"
	# Known config state. node_3d.gd's own _ready() already ran setup()
	# against the repo's provpilot.cfg, whose [map] zoom_index would leak
	# into this run if the file below were missing (load_config keeps the
	# previous values when it cannot load). Writing a real file makes our
	# setup() call deterministic.
	var f := FileAccess.open(tmp_dir.path_join("no_cfg.cfg"), FileAccess.WRITE)
	f.store_string("[map]\nmap_enabled=true\nmap_frame_size=1\nzoom_index=2\n")
	f.close()
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
			_enter_phase("check_motion")

		"check_motion":
			# The map world must track the aircraft. get_projected_px() exposes
			# the heading-up projection without pixel capture: (a) the aircraft
			# itself must land on the map-area center, and (b) moving the
			# aircraft 1 km east must shift a fixed world point by exactly 1 km
			# in screen space (rotation-invariant). Default frame is 320 px;
			# map_area width = 320 - 2*8 margin = 304 px.
			var center := Vector2(2682880.0, 1283072.0)  # setup() LV95 origin
			vfr_map.update(Vector3(center.x - AC_E, 500.0, AC_N - center.y), 250.0, false)
			var ac_px := vfr_map.get_projected_px(AC_E, AC_N)
			check(absf(ac_px.x - 160.0) < 0.5 and absf(ac_px.y - 171.0) < 0.5,
				"aircraft projects to the map-area center (got %.2f, %.2f)" % [ac_px.x, ac_px.y])
			var world_pt := Vector2(AC_E + 500.0, AC_N)
			var p0 := vfr_map.get_projected_px(world_pt.x, world_pt.y)
			vfr_map.update(Vector3(center.x - (AC_E + 1000.0), 500.0, AC_N - center.y), 250.0, false)
			var p1 := vfr_map.get_projected_px(world_pt.x, world_pt.y)
			var expected := 1000.0 * (304.0 / vfr_map.get_view_width_m())
			var got := (p1 - p0).length()
			check(absf(got - expected) < 0.5,
				"map world moves with the aircraft (shift %.2f px, expected %.2f px)" % [got, expected])
			_enter_phase("check_toggle")

		"check_toggle":
			var was_enabled := vfr_map.is_map_enabled()
			vfr_map.toggle_visible()
			check(vfr_map.is_map_enabled() == (not was_enabled), "toggle_visible flips the enabled flag")
			check(vfr_map.visible == vfr_map.is_map_enabled(), "frame visibility follows the enabled flag")
			vfr_map.toggle_visible()
			_enter_phase("check_keys")

		"check_keys":
			# End-to-end key path through Godot's input pipeline — the fix for
			# "N hides the map but never re-shows it". Hide the welcome screen
			# first so node_3d.gd stops feeding ui_up=true into update() (which
			# would gate _unhandled_input).
			if app._welcome_screen != null and app._welcome_screen.visible:
				app._welcome_screen.hide_welcome()
			vfr_map.toggle_visible()  # hide the map again
			check(not vfr_map.is_map_enabled(), "map hidden before N key test")
			_enter_phase("check_keys_settle")

		"check_keys_settle":
			# Let one full frame pass so node_3d.gd's own update() feeds
			# ui_up=false (welcome screen now hidden) into the module — a key
			# pressed mid-frame would otherwise see the previous frame's state.
			if frame - phase_frame >= 2:
				press_key(KEY_N)
				release_key(KEY_N)
				_enter_phase("check_keys_n")

		"check_keys_n":
			if frame - phase_frame >= 5:
				check(vfr_map.is_map_enabled() and vfr_map.visible,
					"N key re-shows the hidden map (unhandled input)")
				zoom_before_key = vfr_map.get_zoom_index()
				press_key(KEY_EQUAL)  # zoom in
				release_key(KEY_EQUAL)
				_enter_phase("check_keys_zin")

		"check_keys_zin":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == max(0, zoom_before_key - 1),
					"'=' key zooms in (got %d)" % vfr_map.get_zoom_index())
				press_key(KEY_PLUS)  # '+' — Shift+=' on US, Shift+1 on Swiss
				release_key(KEY_PLUS)
				_enter_phase("check_keys_zin_plus")

		"check_keys_zin_plus":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == max(0, zoom_before_key - 2),
					"'+' key zooms in further (got %d)" % vfr_map.get_zoom_index())
				press_key(KEY_MINUS)  # zoom out one step
				release_key(KEY_MINUS)
				_enter_phase("check_keys_zout")

		"check_keys_zout":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == max(0, zoom_before_key - 1),
					"'-' key zooms out one step (got %d)" % vfr_map.get_zoom_index())
				press_key(KEY_MINUS)  # back to the starting zoom level
				release_key(KEY_MINUS)
				_enter_phase("check_keys_zout2")

		"check_keys_zout2":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == zoom_before_key,
					"'-' key zooms back to the starting level (got %d)" % vfr_map.get_zoom_index())
				wheel_event(MOUSE_BUTTON_WHEEL_UP, vfr_map.get_global_rect().get_center())
				_enter_phase("check_wheel_up")

		"check_wheel_up":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == max(0, zoom_before_key - 1),
					"mouse wheel up zooms in (got %d)" % vfr_map.get_zoom_index())
				wheel_event(MOUSE_BUTTON_WHEEL_DOWN, vfr_map.get_global_rect().get_center())
				_enter_phase("check_wheel_down")

		"check_wheel_down":
			if frame - phase_frame >= 5:
				check(vfr_map.get_zoom_index() == zoom_before_key,
					"mouse wheel down zooms back out (got %d)" % vfr_map.get_zoom_index())
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


