extends SceneTree
## Phase 2 hole-fix smoke (iterations 2 + 4): suppression must follow
## Scenery3D's LIVE coverage — only cells whose detailed geometry is resident
## suppress the coarse globe (unloaded cells would otherwise be holes to
## space during the intro zoom / after a teleport), and the coverage grid is
## uploaded to the globe as a per-fragment mask with no region clipping, so
## the globe is hidden exactly under resident cells wherever they are.
## Run: godot --headless --path provpilot -s tests/smoke_globe_suppress_clip.gd

var _frame := 0
var _scene: Node = null

# Mask radius in tiles (far_radius 200 + unload_margin 2 + chunk_size 8).
const R_TILES := 210
const N := 2 * R_TILES + 1
const TILE_M := 1024.0

func _initialize() -> void:
	_scene = load("res://node_3d.tscn").instantiate()
	root.add_child(_scene)

func _fail(msg: String) -> bool:
	printerr("FAIL: " + msg)
	quit(1)
	return true

## Tile containing a flat-frame point (same floor the tile manager uses).
func _tile(flat: Vector2) -> Vector2i:
	return Vector2i(
		int(floor((float(_scene.TERRAIN_CENTER_E) - flat.x) / TILE_M)),
		int(floor((flat.y + float(_scene.TERRAIN_CENTER_N)) / TILE_M)))

## Synthetic mask: every tile covered (steady state).
func _full_mask() -> PackedByteArray:
	var m := PackedByteArray()
	m.resize(N * N)
	m.fill(255)
	return m

## Sparse mask: only the central ±80 tiles (what a mid-zoom streamer has
## managed to load so far — chunks arrive one per frame).
func _sparse_mask() -> PackedByteArray:
	var m := PackedByteArray()
	m.resize(N * N)
	m.fill(0)
	for dy in range(-80, 81):
		for dx in range(-80, 81):
			m[(R_TILES + dy) * N + (R_TILES + dx)] = 255
	return m

## Push a synthetic mask through the production conversion path.
func _push(mask: PackedByteArray, cam: Vector2i) -> void:
	_scene._push_suppress_from_coverage(mask, R_TILES, cam.x, cam.y, TILE_M)

func _process(_delta: float) -> bool:
	_frame += 1
	match _frame:
		5:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Camera near Schaffhausen with FULL coverage: the mask has one
			# texel per cell and hides the globe under the camera and out to
			# the coverage edge (25 cells = ~205 km), but not beyond.
			_push(_full_mask(), _tile(Vector2.ZERO))
			var g = s.globe_node
			if g.get_coverage_mask_size() != N:
				return _fail("expected mask size %d, got %d" % [N, g.get_coverage_mask_size()])
			if not g.suppresses_world_point(0.0, 0.0):
				return _fail("camera position not suppressed")
			if g.suppresses_world_point(0.0, 300000.0):
				return _fail("point 300 km from camera suppressed (beyond coverage)")
			print("frame5: full coverage hides the globe out to the coverage edge")
		10:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Over central Norway with FULL coverage: the mask follows the
			# camera (no region clipping) and stops at the coverage edge.
			var focus := Vector2(-547000.0, 1980000.0)
			_push(_full_mask(), _tile(focus))
			var g = s.globe_node
			if not g.suppresses_world_point(focus.x, focus.y):
				return _fail("camera position not suppressed")
			if not g.suppresses_world_point(focus.x + 120000.0, focus.y + 120000.0):
				return _fail("point 120 km from camera not suppressed (inside coverage)")
			if g.suppresses_world_point(focus.x, focus.y + 300000.0):
				return _fail("point 300 km from camera still suppressed (beyond coverage)")
			print("frame10: full coverage suppresses up to the coverage edge")
		15:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Mid-zoom coverage (only the central cells loaded): the globe must
			# stay visible beyond them — this is exactly the black-square /
			# stars-through-the-globe bug of the first iteration.
			var focus := Vector2(-547000.0, 1980000.0)
			_push(_sparse_mask(), _tile(focus))
			var g = s.globe_node
			if not g.suppresses_world_point(focus.x, focus.y):
				return _fail("camera position not suppressed with sparse coverage")
			if g.suppresses_world_point(focus.x, focus.y + 100000.0):
				return _fail("point 100 km out still suppressed although its cells are unloaded (hole to space)")
			if g.suppresses_world_point(focus.x - 100000.0, focus.y):
				return _fail("point 100 km west still suppressed although its cells are unloaded")
			print("frame15: sparse coverage leaves the globe visible beyond loaded cells")
		20:
			var s := _scene
			# Data edge (iteration 4b): one no-data tile next to resident
			# tiles must show the globe on that tile only — the resident
			# neighbours stay masked (an 8 km cell mask unmasked all of them).
			var focus := Vector2(-547000.0, 1980000.0)
			var m := _full_mask()
			var hole := _tile(focus) + Vector2i(2, 0)  # two tiles further east
			m[R_TILES * N + (R_TILES + 2)] = 0
			_push(m, _tile(focus))
			var g = s.globe_node
			var hole_x := float(s.TERRAIN_CENTER_E) - (float(hole.x) + 0.5) * TILE_M
			var hole_z := (float(hole.y) + 0.5) * TILE_M - float(s.TERRAIN_CENTER_N)
			if g.suppresses_world_point(hole_x, hole_z):
				return _fail("no-data tile still suppressed")
			if not g.suppresses_world_point(hole_x + TILE_M, hole_z) \
					or not g.suppresses_world_point(hole_x - TILE_M, hole_z):
				return _fail("resident tiles next to a no-data tile not suppressed")
			print("frame20: tile-resolution mask keeps resident tiles masked next to a no-data tile")
			print("smoke_globe_suppress_clip: OK")
			quit(0)
			return true
	return false
