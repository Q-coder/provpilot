extends SceneTree
## Phase 2 hole-fix smoke (iteration 2): suppression must follow Scenery3D's
## LIVE coverage — only cells whose detailed geometry is resident suppress
## the coarse globe. The first iteration clipped static region extents to the
## streaming square and assumed everything inside was loaded; far-zone chunks
## actually stream in one per frame, so during the intro zoom (and after any
## teleport) unloaded cells left holes to space — dark squares + stars seen
## through the globe at altitude. Run: godot --headless --path provpilot -s tests/smoke_globe_suppress_clip.gd

var _frame := 0
var _scene: Node = null

# Norway's terrain extent in the flat frame (from its terrain manifest grid:
# ei 2381..3927, ni 2372..4000 at 1024 m tiles around the LV95 origin).
const NORWAY_RECT := {
	"x_min": -1339000.0, "z_min": 1146000.0,
	"x_max": 245000.0, "z_max": 2814000.0}

# Streaming square in chunk cells (far_radius=200 tiles / chunk_size=8).
const R_CELLS := 25
const N := 2 * R_CELLS + 1
const CHUNK_TILES := 8

func _initialize() -> void:
	_scene = load("res://node_3d.tscn").instantiate()
	root.add_child(_scene)

func _fail(msg: String) -> bool:
	printerr("FAIL: " + msg)
	quit(1)
	return true

## Floor division that matches the tile manager for negative indices.
func _floor_div(a: int, b: int) -> int:
	if a >= 0:
		return a / b
	return -((-a + b - 1) / b)

## Chunk-grid cell containing a flat-frame point (same floor division the
## tile manager uses for its camera chunk index).
func _chunk_cell(flat: Vector2) -> Vector2i:
	var ei := int(floor((float(_scene.TERRAIN_CENTER_E) - flat.x) / 1024.0))
	var ni := int(floor((flat.y + float(_scene.TERRAIN_CENTER_N)) / 1024.0))
	return Vector2i(_floor_div(ei, CHUNK_TILES), _floor_div(ni, CHUNK_TILES))

## Synthetic coverage grid: every cell covered (steady state).
func _full_grid() -> PackedInt32Array:
	var g := PackedInt32Array()
	g.resize(N * N)
	for i in range(N * N):
		g[i] = 1
	return g

## Sparse coverage: only the central 21x21 cells (what a mid-zoom streamer
## has managed to load so far — chunks arrive one per frame).
func _sparse_grid() -> PackedInt32Array:
	var g := PackedInt32Array()
	g.resize(N * N)
	var c := R_CELLS
	for dy in range(-10, 11):
		for dx in range(-10, 11):
			g[(c + dy) * N + (c + dx)] = 1
	return g

## Push a synthetic coverage grid through the production conversion path.
func _push(grid: PackedInt32Array, cam_cg: Vector2i) -> void:
	var s := _scene
	s.globe_node.clear_suppress_rects()
	s._globe_suppress_regions.clear()
	s._globe_suppress_regions.append(NORWAY_RECT)
	s._push_suppress_from_coverage(grid, R_CELLS, cam_cg.x, cam_cg.y, CHUNK_TILES)

func _process(_delta: float) -> bool:
	_frame += 1
	match _frame:
		5:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Camera near Schaffhausen (~2000 km from Norway): even full
			# coverage stays far outside the region extent -> no suppression.
			_push(_full_grid(), _chunk_cell(Vector2.ZERO))
			if s.globe_node.get_suppress_rect_count() != 0:
				return _fail("expected 0 rects with the camera near Schaffhausen, got %d"
					% s.globe_node.get_suppress_rect_count())
			print("frame5: far region not suppressed (camera near Schaffhausen)")
		10:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Over central Norway with FULL coverage: suppression follows the
			# camera and stops at the coverage edge minus the 10 km margin.
			var focus := Vector2(-547000.0, 1980000.0)
			_push(_full_grid(), _chunk_cell(focus))
			var g = s.globe_node
			if g.get_suppress_rect_count() < 1:
				return _fail("expected clipped rects over Norway, got %d"
					% g.get_suppress_rect_count())
			if not g.suppresses_flat_point(focus.x, focus.y):
				return _fail("camera position not suppressed")
			if not g.suppresses_flat_point(focus.x, focus.y + 120000.0):
				return _fail("point 120 km from camera not suppressed (inside coverage)")
			if g.suppresses_flat_point(focus.x, focus.y + 300000.0):
				return _fail("point 300 km from camera still suppressed (beyond coverage)")
			print("frame10: full coverage suppresses up to the coverage edge - margin")
		15:
			var s := _scene
			if not is_instance_valid(s.globe_node):
				return _fail("globe node missing")
			# Mid-zoom coverage (only the central cells loaded): the globe must
			# stay visible beyond them — this is exactly the black-square /
			# stars-through-the-globe bug of the first iteration.
			var focus := Vector2(-547000.0, 1980000.0)
			_push(_sparse_grid(), _chunk_cell(focus))
			var g = s.globe_node
			if not g.suppresses_flat_point(focus.x, focus.y):
				return _fail("camera position not suppressed with sparse coverage")
			if g.suppresses_flat_point(focus.x, focus.y + 100000.0):
				return _fail("point 100 km out still suppressed although its cells are unloaded (hole to space)")
			print("frame15: sparse coverage leaves the globe visible beyond loaded cells")
			print("smoke_globe_suppress_clip: OK")
			quit(0)
			return true
	return false
