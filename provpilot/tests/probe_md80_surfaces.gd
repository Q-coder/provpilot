extends Node
## Probe: MD-80 speed brake lever (B key and the cockpit lever shims), the flaps
## on the outside model (surface-positions/flap-pos-norm) and the DME of NAV 2.
## Run headless: godot --headless --path provpilot res://tests/probe_md80_surfaces.tscn
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _key(code: int, down: bool) -> void:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.physical_keycode = code
	ev.pressed = down
	Input.parse_input_event(ev)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var rig = root._aircraft
	var fga = rig.get_fg_animator()
	var fgc = rig.get_fg_controls()

	# speed brake: B key out and in
	_key(KEY_B, true)
	await get_tree().create_timer(0.3).timeout
	_key(KEY_B, false)
	await get_tree().create_timer(3.0).timeout
	var lever: float = jsb.get_property("/controls/flight/speedbrake")
	var cmd: float = jsb.get_property("/systems/fcs/spoilers/lever-cmd")
	var deg: float = jsb.get_property("/systems/fcs/spoiler-l2/final-deg")
	print("  [diag] B: lever %.2f lever-cmd %.0f spoiler-l2 %.1f deg l1 %.1f deg, sbk-pos %.2f" % [lever, cmd, deg, jsb.get_property("/systems/fcs/spoiler-l1/final-deg"), jsb.get_property("fcs/speedbrake-pos-norm")])
	_check(absf(lever - 0.8) < 0.01, "B puts the spoiler lever out and it stays there (%.2f)" % lever)
	_check(deg > 5.0, "the spoiler panels come up (%.1f deg)" % deg)
	_key(KEY_B, true)
	await get_tree().create_timer(0.3).timeout
	_key(KEY_B, false)
	await get_tree().create_timer(3.0).timeout
	_check(jsb.get_property("/controls/flight/speedbrake") < 0.01 and jsb.get_property("/systems/fcs/spoiler-l2/final-deg") < 1.0, "B again stows them (lever %.2f, %.1f deg)" % [jsb.get_property("/controls/flight/speedbrake"), jsb.get_property("/systems/fcs/spoiler-l2/final-deg")])
	# the cockpit lever: one notch per click
	rig._on_fg_control("provpilot/speedbrake", 1.0)
	await get_tree().create_timer(0.5).timeout
	_check(absf(jsb.get_property("/controls/flight/speedbrake") - 0.2) < 0.01, "a lever click moves one notch (%.2f)" % jsb.get_property("/controls/flight/speedbrake"))
	rig._on_fg_control("provpilot/speedbrake", 0.0)
	await get_tree().create_timer(0.5).timeout
	_check(jsb.get_property("/controls/flight/speedbrake") < 0.01, "... and back")

	# flaps: two steps down, the outside model's property follows
	rig.get_cockpit_controls().step_flaps(1, jsb)
	rig.get_cockpit_controls().step_flaps(1, jsb)
	rig.get_cockpit_controls().step_flaps(1, jsb)
	await get_tree().create_timer(8.0).timeout
	var fdm_flap: float = jsb.get_property("fcs/flap-pos-norm")
	var anim_flap: float = float(fga.get_property("surface-positions/flap-pos-norm"))
	print("  [diag] flaps: fdm %.3f (%.1f deg), model %.3f" % [fdm_flap, jsb.get_property("fcs/flap-pos-deg"), anim_flap])
	_check(fdm_flap > 0.1 and absf(anim_flap - fdm_flap) < 0.02, "the outside model's flaps follow the FDM (%.2f)" % anim_flap)

	# NAV 2 on KLO VOR-DME 114.85
	fgc.assign_external("instrumentation/nav[1]/frequencies/selected-mhz", 114.85)
	await get_tree().create_timer(2.0).timeout
	var d2: float = float(fga.get_property("instrumentation/dme[1]/indicated-distance-nm-x10"))
	var av2: float = float(fga.get_property("instrumentation/dme[1]/available"))
	print("  [diag] dme[1]: available %s x10 %s, fdm in-range %s nm %.2f" % [str(av2), str(d2), str(jsb.get_property("/instrumentation/dme[1]/in-range")), jsb.get_property("/instrumentation/dme[1]/indicated-distance-nm")])
	_check(av2 > 0.5 and d2 > 0.0, "NAV 2's DME window shows a distance (%.1f)" % d2)
	print("probe_md80_surfaces: %s" % ("OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails > 0 else 0)
