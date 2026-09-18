extends SceneTree
## Smoke test: W3DGlobe coverage-mask API (design §8.1 iteration 4 / §9 D8).
## Run: godot --headless --path provpilot -s tests/smoke_globe_suppress.gd
## Exercises the pure-data side of the C++ bindings (no tree needed — the
## shader-side discard is covered by in-game flight). The CPU mirror
## suppresses_world_point() must agree with the mask + origin/step mapping,
## including a NEGATIVE step (ProVPilot's +X = West runs opposite to the
## east cell index).

func _fail(msg: String) -> void:
	printerr("FAIL: " + msg)
	quit(1)

func _initialize() -> void:
	var g := W3DGlobe.new()
	if g.get_coverage_mask_size() != 0:
		_fail("expected no mask initially"); return
	# 3x3 cells of 100 m. Row = Z cell (step +100 from z=0), column = X cell
	# (step -100 from x=0, i.e. column 0 spans x in (-100, 0]).
	# Covered: (col 0,row 0), (col 2,row 1), (col 1,row 2).
	var cells := PackedByteArray([255, 0, 0,  0, 0, 255,  0, 255, 0])
	g.set_coverage_mask(cells, 3, 0.0, 0.0, -100.0, 100.0)
	if g.get_coverage_mask_size() != 3:
		_fail("expected mask size 3"); return
	var checks: Array = [
		[-50.0, 50.0, true],     # col 0, row 0
		[-150.0, 50.0, false],   # col 1, row 0
		[-250.0, 150.0, true],   # col 2, row 1
		[-150.0, 250.0, true],   # col 1, row 2
		[-50.0, 250.0, false],   # col 0, row 2
		[50.0, 50.0, false],     # x > origin: outside (negative cell)
		[-350.0, 50.0, false],   # beyond column 2
		[-50.0, -10.0, false],   # z < origin
		[-50.0, 350.0, false],   # beyond row 2
	]
	for c in checks:
		var got: bool = g.suppresses_world_point(c[0], c[1])
		if got != c[2]:
			_fail("point (%f, %f) -> %s, expected %s" % [c[0], c[1], str(got), str(c[2])]); return
	# Wrong-sized data must clear the mask rather than index out of range.
	g.set_coverage_mask(PackedByteArray([255, 255]), 3, 0.0, 0.0, -100.0, 100.0)
	if g.get_coverage_mask_size() != 0 or g.suppresses_world_point(-50.0, 50.0):
		_fail("malformed mask should clear"); return
	g.set_coverage_mask(cells, 3, 0.0, 0.0, -100.0, 100.0)
	g.clear_coverage_mask()
	if g.get_coverage_mask_size() != 0 or g.suppresses_world_point(-50.0, 50.0):
		_fail("clear_coverage_mask did not clear"); return
	g.free()
	print("smoke_globe_suppress: OK")
	quit(0)
