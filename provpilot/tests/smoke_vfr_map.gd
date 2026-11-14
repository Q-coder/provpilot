extends Node
## Headless smoke test for the C++ VfrMap module (src/vfr_map.cpp) and its
## node_3d.gd feed path. Hermetic: no Scenery3D / scenery data required —
## generates a small synthetic tile pyramid in a temp dir, points the module
## at it, and drives update() with known positions/headings near LSZH. The
## slew phase initializes JSBSim (the model ships in the repo) to mirror
## production, where physics is always running. Exits 0 on success, 1 on any
## failure.

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
var slew_p_before := Vector2.ZERO
var slew_p_after := Vector2.ZERO
# Position fed while the terrain ground map is tested (zero = AC_E / AC_N).
var feed_override := Vector2.ZERO
var terrain_cached_before := 0


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
## Skipped during the slew phases, where node_3d.gd must be the one driving
## the map (from $AC while slewing) and we verify exactly that.
func _feed_known_position() -> void:
	if vfr_map != null and phase != "done" and not phase.begins_with("slew"):
		if feed_override != Vector2.ZERO:
			vfr_map.update_lv95(feed_override.x, feed_override.y, 250.0, false)
		else:
			vfr_map.update_lv95(AC_E, AC_N, 250.0, false)


## A base-tier terrain tile: 34 x 34 float32 LE, pixel (0,0) at the SE
## corner (rows S->N, cols E->W), a slope rising to the north-west.
func _write_terrain_tile(dir: String, e: int, n: int) -> void:
	var b := PackedByteArray()
	b.resize(34 * 34 * 4)
	for row in range(34):
		for col in range(34):
			b.encode_float((row * 34 + col) * 4, 400.0 + row * 6.0 + col * 3.0)
	var f := FileAccess.open(dir.path_join("tile_%d_%d.raw" % [e, n]), FileAccess.WRITE)
	f.store_buffer(b)
	f.close()


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


# This custom engine build does not expose the static Input.key_press/release
# helpers, so drive the Input singleton through parsed key events instead.
func press_key(k: Key, shift: bool = false) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.shift_pressed = shift
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
				vfr_map.setup("/tmp/vfr_map_smoke/no_cfg.cfg")
				vfr_map.set_tile_paths(PackedStringArray([tmp_dir]))
				_enter_phase("drive")

		"drive":
			# Drive the module like node_3d.gd does: Godot-space position for
			# AC_E/AC_N (swiss_e = center_e - x  =>  x = center_e - swiss_e).
			vfr_map.update_lv95(AC_E, AC_N, 250.0, false)
			if frame - phase_frame >= 30:
				_enter_phase("check_tiles")

		"check_tiles":
			var cached := vfr_map.get_cached_tile_count()
			check(cached > 0, "map tiles were streamed into the LRU cache (got %d)" % cached)
			check(vfr_map.get_visible_airport_count() > 0,
				"in-view airports detected near LSZH (got %d)" % vfr_map.get_visible_airport_count())
			check(vfr_map.get_visible_navaid_count() > 0,
				"in-view radio navaids near LSZH (KLO VOR-DME; got %d)" % vfr_map.get_visible_navaid_count())
			# Re-issue a known state synchronously: node_3d.gd's own _process
			# overwrites position/heading with JSBSim's every frame.
			vfr_map.update_lv95(AC_E, AC_N, 250.0, false)
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
			vfr_map.update_lv95(AC_E, AC_N, 250.0, false)
			var ac_px := vfr_map.get_projected_px(AC_E, AC_N)
			check(absf(ac_px.x - 160.0) < 0.5 and absf(ac_px.y - 171.0) < 0.5,
				"aircraft projects to the map-area center (got %.2f, %.2f)" % [ac_px.x, ac_px.y])
			var world_pt := Vector2(AC_E + 500.0, AC_N)
			var p0 := vfr_map.get_projected_px(world_pt.x, world_pt.y)
			vfr_map.update_lv95(AC_E + 1000.0, AC_N, 250.0, false)
			var p1 := vfr_map.get_projected_px(world_pt.x, world_pt.y)
			var expected := 1000.0 * (304.0 / vfr_map.get_view_width_m())
			var got := (p1 - p0).length()
			check(absf(got - expected) < 0.5,
				"map world moves with the aircraft (shift %.2f px, expected %.2f px)" % [got, expected])
			_enter_phase("check_layout")

		"check_layout":
			# Title-bar drag = move_frame_by, corner drag = resize_frame_by; both
			# persist to [map] of the config given to setup(). The headless
			# parent is 64 px, below the minimum frame, so nothing clamps here.
			var s0: Vector2 = vfr_map.get_frame_size()
			var o0: Vector2 = vfr_map.get_frame_offset()
			check(s0 == Vector2(320, 320), "default frame is the Medium preset (got %s)" % str(s0))
			vfr_map.move_frame_by(Vector2(-40.0, 30.0))
			var o1: Vector2 = vfr_map.get_frame_offset()
			check(o1.is_equal_approx(Vector2(o0.x + 40.0, o0.y + 30.0)), "dragging the title bar left/down moves the frame (offset %s -> %s)" % [str(o0), str(o1)])
			vfr_map.resize_frame_by(Vector2(50.0, 20.0))
			var s1: Vector2 = vfr_map.get_frame_size()
			var o2: Vector2 = vfr_map.get_frame_offset()
			check(s1 == Vector2(370, 340) and absf(o2.x - (o1.x - 50.0)) < 0.01, "dragging the corner grows the frame, left edge fixed (size %s, offset %s)" % [str(s1), str(o2)])
			vfr_map.resize_frame_left_by(Vector2(-30.0, 10.0))
			var s2: Vector2 = vfr_map.get_frame_size()
			var o3: Vector2 = vfr_map.get_frame_offset()
			check(s2 == Vector2(400, 350) and o3.is_equal_approx(o2), "dragging the bottom-left corner grows the frame, right edge fixed (size %s, offset %s)" % [str(s2), str(o3)])
			vfr_map.resize_frame_left_by(Vector2(30.0, -10.0))
			check(vfr_map.get_frame_size() == Vector2(370, 340), "and back")
			var cfg := ConfigFile.new()
			cfg.load("/tmp/vfr_map_smoke/no_cfg.cfg")
			check(absf(float(cfg.get_value("map", "map_frame_w", 0.0)) - 370.0) < 0.01 and absf(float(cfg.get_value("map", "map_frame_top", 0.0)) - o2.y) < 0.01,
				"frame size and position are persisted to [map]")
			vfr_map.set_frame_size(320)
			check(vfr_map.get_frame_size() == Vector2(320, 320), "the Settings preset puts the frame back to 320 px")
			# Terrain ground map: no map/ tiles, synthetic 34 px terrain tiles
			# for a block of cells far from the jpg pyramid; at the 5 km view
			# the map renders L0 cells straight from the heightmaps.
			var tdir := "/tmp/vfr_map_smoke/terrain"
			DirAccess.make_dir_recursive_absolute(tdir)
			for ni in range(1400, 1406):
				for ei in range(2700, 2706):
					_write_terrain_tile(tdir, ei * 1024, ni * 1024)
			vfr_map.set_tile_paths(PackedStringArray([]))
			vfr_map.set_terrain_paths(PackedStringArray([tdir]))
			while vfr_map.get_zoom_index() > 0:
				vfr_map.zoom_in()
			feed_override = Vector2(2703.0 * 1024.0 + 512.0, 1403.0 * 1024.0 + 512.0)
			terrain_cached_before = vfr_map.get_cached_tile_count()
			_enter_phase("check_terrain_map")

		"check_terrain_map":
			if frame - phase_frame >= 90:
				var got_tiles := vfr_map.get_cached_tile_count() - terrain_cached_before
				check(got_tiles >= 9, "terrain tiles are rendered into map textures where no map/ tile exists (%d new)" % got_tiles)
				feed_override = Vector2.ZERO
				vfr_map.set_tile_paths(PackedStringArray([tmp_dir]))
				while vfr_map.get_zoom_index() < 2:
					vfr_map.zoom_out()
				_enter_phase("check_toggle")

		"check_toggle":
			var was_enabled := vfr_map.is_map_enabled()
			vfr_map.toggle_visible()
			check(vfr_map.is_map_enabled() == (not was_enabled), "toggle_visible flips the enabled flag")
			check(vfr_map.visible == vfr_map.is_map_enabled(), "frame visibility follows the enabled flag")
			# The toggle is remembered: the SettingsStore (Settings > Map, the next
			# Save) and provpilot.cfg [map] follow it, so apply_config() after the
			# settings screen and Continue Flight keep the frame as it was.
			check(app._settings != null and bool(app._settings.get_setting("map_enabled")) == vfr_map.is_map_enabled(), "SettingsStore map_enabled follows the toggle")
			var cfg_after := ConfigFile.new()
			cfg_after.load("/tmp/vfr_map_smoke/no_cfg.cfg")   # the map's config file here (setup() above)
			check(bool(cfg_after.get_value("map", "map_enabled", not vfr_map.is_map_enabled())) == vfr_map.is_map_enabled(), "the map's config [map] map_enabled follows the toggle")
			vfr_map.apply_config()
			check(vfr_map.is_map_enabled() == (not was_enabled), "apply_config() (settings closed) keeps the toggled state")
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
				press_key(KEY_EQUAL, true)  # '+' = Shift+= (the catalog's second map_zoom_in key)
				release_key(KEY_EQUAL)
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
				_enter_phase("slew_park")

		# ── Slew mode ──────────────────────────────────────────────────────
		# While slewing, JSBSim physics is paused so jsb_node.get_godot_position()
		# stays frozen at the pre-slew state; node_3d.gd must feed the map from
		# $AC instead (the fix for "map not moving in slew mode"). Initialize
		# JSBSim at the park point first, mirroring production where it is always
		# running, so the exit reinit has a live state to sync from.

		"slew_park":
			var jsb := app.get_node_or_null("AC/JSBGodot")
			if jsb == null:
				fail("no AC/JSBGodot node for slew test")
			var ac := app.get_node("AC") as Node3D
			# ECEF world: park the aircraft on the ellipsoid at the LV95 point,
			# nose along the true heading (design §9.5).
			var park_xf: Transform3D = app._world.ac_transform_at(AC_E, AC_N, 500.0, 250.0, ac.scale)
			jsb.set_godot_terrain_y_offset(-1.1)  # WHEEL_GROUND_CLEARANCE in node_3d.gd
			jsb.initialize_at_terrain(498.0, park_xf.origin, 250.0)
			ac.global_transform = park_xf
			if frame - phase_frame >= 3:
				press_key(KEY_SHIFT)
				press_key(KEY_Y)
				_enter_phase("slew_enter")

		"slew_enter":
			# node_3d.gd's _process runs SlewMode.update() with both keys held.
			if frame - phase_frame >= 3:
				check(app._slew_mode.is_enabled(), "slew mode entered via Shift+Y")
				var ac := app.get_node("AC") as Node3D
				# Re-park exactly (physics may have nudged AC slightly).
				ac.global_transform = app._world.ac_transform_at(AC_E, AC_N, 500.0, 250.0, ac.scale)
				_enter_phase("slew_move")

		"slew_move":
			var world_pt := Vector2(AC_E + 500.0, AC_N)
			slew_p_before = vfr_map.get_projected_px(world_pt.x, world_pt.y)
			var ac := app.get_node("AC") as Node3D
			# Move 1 km east in LV95. JSBSim's cached position stays frozen at
			# the park point for the rest of this phase group.
			ac.global_transform = app._world.ac_transform_at(AC_E + 1000.0, AC_N, 500.0, 250.0, ac.scale)
			_enter_phase("slew_moved")

		"slew_moved":
			if frame - phase_frame >= 10:
				var world_pt := Vector2(AC_E + 500.0, AC_N)
				slew_p_after = vfr_map.get_projected_px(world_pt.x, world_pt.y)
				var expected := 1000.0 * (304.0 / vfr_map.get_view_width_m())
				var got := (slew_p_after - slew_p_before).length()
				check(absf(got - expected) < 0.5,
					"map follows the aircraft while slewing (shift %.2f px, expected %.2f px)" % [got, expected])
				release_key(KEY_Y)
				release_key(KEY_SHIFT)
				_enter_phase("slew_release")

		"slew_release":
			# Let one frame pass with the keys up so SlewMode's key_held
			# debounce resets before the exit press.
			if frame - phase_frame >= 3:
				press_key(KEY_SHIFT)
				press_key(KEY_Y)
				_enter_phase("slew_exit")

		"slew_exit":
			# Exit reinitializes JSBSim at $AC's position, so the feed must
			# switch back to the (now synced) JSBSim cache without a jump.
			if frame - phase_frame >= 10:
				check(not app._slew_mode.is_enabled(), "slew mode exited via Shift+Y")
				var world_pt := Vector2(AC_E + 500.0, AC_N)
				var p_now := vfr_map.get_projected_px(world_pt.x, world_pt.y)
				var drift := (p_now - slew_p_after).length()
				check(drift < 1.0, "no map jump on slew exit (drift %.2f px)" % drift)
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


