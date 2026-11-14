extends Node
## Probe: Copenhagen's parallel ILS. On the 22L and 22R thresholds the own
## localizer (109.5 OXS / 110.9 KLK) must be centred and the other one far off.
## Run headless: godot --headless --path provpilot res://tests/probe_ekch_ils.tscn
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var spots := [["22L", 55.625446, 12.667644, 109.5, 110.9], ["22R", 55.616543, 12.641174, 110.9, 109.5]]
	for s in spots:
		var lv: Vector2 = Geo.wgs84_to_lv95(s[1], s[2])
		root._on_welcome_flight_requested("md80", lv.x, lv.y, 221.0)
		var waited := 0.0
		while not root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(6.0).timeout
		root._cancel_intro_zoom()
		root._welcome_screen.visible = false
		var jsb = root.jsb_node
		var rig = root._aircraft
		var fgc = rig.get_fg_controls()
		print("  [diag] %s: asked %.5f %.5f, FDM at %.5f %.5f" % [s[0], s[1], s[2], jsb.get_latitude_deg(), jsb.get_longitude_deg()])
		for f in [s[3], s[4]]:
			fgc.assign_external("instrumentation/nav/frequencies/selected-mhz", f)
			fgc.assign_external("instrumentation/nav[1]/frequencies/selected-mhz", f)
			await get_tree().create_timer(3.0).timeout
			var cdi: float = float(rig.get_fg_property("instrumentation/nav/heading-needle-deflection"))
			var rwy := str(rig.get_fg_property("instrumentation/nav/ils-runway"))
			var id := str(rig.get_fg_property("instrumentation/nav/nav-id"))
			var inr: float = float(rig.get_fg_property("instrumentation/nav/in-range"))
			print("  [diag] on %s, %.2f: id %s rwy %s in-range %.0f cdi %+.2f, dme1 %s dme2 %s" % [s[0], f, id, rwy, inr, cdi, str(rig.get_fg_property("instrumentation/dme/indicated-distance-nm")), str(rig.get_fg_property("instrumentation/dme[1]/indicated-distance-nm"))])
			if f == s[3]:
				_check(inr > 0.5 and absf(cdi) < 1.0, "%s threshold: %.2f is centred (%+.2f)" % [s[0], f, cdi])
			else:
				_check(absf(cdi) > 5.0, "%s threshold: the parallel runway's %.2f is at full scale (%+.2f)" % [s[0], f, cdi])
	print("probe_ekch_ils: %s" % ("OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails > 0 else 0)
