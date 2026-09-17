extends SceneTree
## Smoke test: W3DGlobe suppression-rect API (Phase 2, decision D3a).
## Run: godot --headless --path provpilot -s tests/smoke_globe_suppress.gd
## Exercises the pure-data side of the new C++ bindings (no tree needed —
## the LOD-traversal use of to_global() is covered by in-game flight).

func _initialize() -> void:
	var g := W3DGlobe.new()
	g.clear_suppress_rects()
	if g.get_suppress_rect_count() != 0:
		printerr("FAIL: expected 0 rects after clear")
		quit(1)
		return
	g.add_suppress_rect(-100.0, -50.0, 100.0, 50.0)  # reversed min/max
	g.add_suppress_rect(200.0, 300.0, 400.0, 600.0)  # normal order
	if g.get_suppress_rect_count() != 2:
		printerr("FAIL: expected 2 rects")
		quit(1)
		return
	var checks: Array = [
		[0.0, 0.0, true],          # centre of rect 1
		[-100.0, 50.0, true],      # corner (boundaries inclusive, normalised)
		[101.0, 0.0, false],       # just outside rect 1
		[350.0, 450.0, true],      # inside rect 2
		[150.0, 450.0, false],     # gap between the two rects
	]
	for c in checks:
		var got: bool = g.suppresses_flat_point(c[0], c[1])
		if got != c[2]:
			printerr("FAIL: point (%f, %f) -> %s, expected %s" % [c[0], c[1], str(got), str(c[2])])
			quit(1)
			return
	g.free()
	print("smoke_globe_suppress: OK")
	quit(0)
